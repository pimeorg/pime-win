#include "engine/core_engine.h"

#include <pime/pime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "common/chain_file.h"
#include "common/log.h"
#include "common/store_key.h"
#include "common/win_util.h"

namespace pimewin {
namespace {

constexpr uint32_t kUserDictAliasLen = sizeof(kUserDictAlias) - 1;
constexpr uint32_t kUserDictPrefixLen = sizeof(kUserDictPrefix) - 1;

void CollectStoreEntry(void* ud, const char* key, uint32_t key_len,
                       const char* value, uint32_t value_len) {
    auto* out =
        static_cast<std::vector<std::pair<std::string, std::string>>*>(ud);
    out->emplace_back(std::string(key, key_len), std::string(value, value_len));
}

// pime-core stores paths as narrow strings and hands them to
// std::filesystem, which interprets narrow strings in the ANSI code page.
// ASCII paths round-trip unchanged; anything else is passed as an 8.3 path.
std::string CorePath(const std::wstring& path) {
    bool ascii = true;
    for (wchar_t ch : path) {
        if (ch > 0x7F) {
            ascii = false;
            break;
        }
    }
    if (ascii) return ToUtf8(path);

    wchar_t short_path[MAX_PATH * 2] = {};
    if (GetShortPathNameW(path.c_str(), short_path,
                          static_cast<DWORD>(sizeof(short_path) /
                                             sizeof(short_path[0]))) != 0) {
        bool short_ascii = true;
        for (const wchar_t* p = short_path; *p; ++p) {
            if (*p > 0x7F) {
                short_ascii = false;
                break;
            }
        }
        if (short_ascii) return ToUtf8(short_path);
    }
    return ToUtf8(path);
}

// The settings store the active scheme as "<id>.cbor"; the chain itself lives
// inside the scheme package (or a user edit in the data directory).
std::string ChainStem(const std::string& chain) {
    std::string stem = chain;
    if (stem.size() > 5 && stem.compare(stem.size() - 5, 5, ".cbor") == 0) {
        stem.resize(stem.size() - 5);
    }
    return stem;
}

pime_status SinkComposition(void* ud, const pime_composition* value);
pime_status SinkCommit(void* ud, const pime_text* value);
pime_status SinkShowCandidates(void* ud, const pime_candidate_list* value);
pime_status SinkHideCandidates(void* ud);
pime_status SinkAudioOut(void*, const pime_audio_frame*) { return PIME_OK; }
pime_status SinkBeep(void* ud);
pime_status SinkClearInk(void*) { return PIME_OK; }
pime_status SinkNotify(void* ud, pime_status code);
void SinkLog(void* ud, const pime_log_record* record);
void SinkWake(void* ud);

struct Session {
    SessionId id = 0;
    pime_session* session = nullptr;
    HWND notify = nullptr;
    bool focused = false;
    std::wstring app;
    uint32_t field_type = PIME_FIELD_TEXT;
    uint32_t host_caps = 0;
    std::vector<UiCommand> pending;
};

}  // namespace

struct CoreEngine::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::shared_ptr<std::function<void()>>> tasks;
    std::shared_ptr<std::function<void()>> current;
    std::thread worker;
    bool running = false;
    bool stopping = false;
    bool ready = false;

    std::mutex start_mutex;
    int start_state = 0;  // 0 idle, 1 starting, 2 finished

    EngineConfig config;
    pime_core* core = nullptr;
    uint64_t generation = 0;
    SessionId next_session = 1;
    std::unordered_map<SessionId, std::unique_ptr<Session>> sessions;
    Session* active = nullptr;

    std::vector<UiCommand> capture;
    bool capturing = false;
    uint64_t last_flush = 0;

    // ---- worker plumbing -------------------------------------------------
    bool Run(const std::function<void()>& fn) {
        auto task = std::make_shared<std::function<void()>>(fn);
        std::unique_lock<std::mutex> lock(mutex);
        if (!running) return false;
        tasks.push_back(task);
        cv.notify_all();
        cv.wait(lock, [&] { return *task == nullptr; });
        return true;
    }

    void MarkDone(const std::shared_ptr<std::function<void()>>& task) {
        std::lock_guard<std::mutex> lock(mutex);
        *task = nullptr;
        cv.notify_all();
    }

    void WorkerMain() {
        for (;;) {
            std::shared_ptr<std::function<void()>> task;
            bool idle = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (tasks.empty()) {
                    if (stopping) return;
                    cv.wait_for(lock, std::chrono::milliseconds(10));
                    if (tasks.empty()) idle = true;
                    else task = tasks.front();
                } else {
                    task = tasks.front();
                }
                if (task) tasks.pop_front();
            }
            if (task) {
                (*task)();
                MarkDone(task);
            }
            if (idle) {
                Pump();
                FlushStore(false);
            }
        }
    }

    // Learning lands in the plugin store; the engine lives for the whole
    // session, so it has to reach disk on its own rather than at shutdown.
    void FlushStore(bool force) {
        if (!core) return;
        const uint64_t now = GetTickCount64();
        if (!force && now - last_flush < 200) return;
        last_flush = now;
        pime_core_store_flush_all(core);
    }

    Session* Find(SessionId id) {
        const auto it = sessions.find(id);
        return it == sessions.end() ? nullptr : it->second.get();
    }

    void Emit(UiCommand command) {
        if (capturing) {
            capture.push_back(std::move(command));
            return;
        }
        if (!active) return;
        active->pending.push_back(std::move(command));
        if (active->notify) {
            PostMessageW(active->notify, kEngineWakeMessage, 0, 0);
        }
    }

    // ---- core lifecycle --------------------------------------------------
    bool LoadPlugins(pime_core* instance, const std::wstring& package_root) {
        const std::wstring pattern = PathJoin(package_root, L"*");
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            WriteLog(LogLevel::Error, "plugins",
                     "no plugin packages found in " + ToUtf8(package_root));
            return false;
        }
        int loaded = 0;
        int failed = 0;
        do {
            if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
            if (data.cFileName[0] == L'.') continue;
            const std::wstring package = PathJoin(package_root, data.cFileName);
            const std::string path = CorePath(package);
            const pime_status status = pime_plugin_load(
                instance, path.c_str(), static_cast<uint32_t>(path.size()));
            if (status == PIME_OK) {
                ++loaded;
            } else {
                ++failed;
                WriteLog(LogLevel::Warn, "plugins",
                         ToUtf8(data.cFileName) + ": " +
                             pime_status_string(status));
            }
        } while (FindNextFileW(find, &data));
        FindClose(find);
        WriteLog(LogLevel::Info, "plugins",
                 "loaded " + std::to_string(loaded) + ", skipped " +
                     std::to_string(failed));
        return loaded > 0;
    }

    // A scheme package carries the plugins its chain references. Loading is
    // idempotent in core (same id/version/code hash returns OK), so switching
    // schemes can load the new package without unloading the previous one.
    void LoadSchemePlugins(const std::string& chain) {
        for (const std::wstring& dir :
             SchemePluginDirs(config.resources_dir, config.data_dir,
                              ChainStem(chain))) {
            LoadPlugins(core, dir);
        }
    }

    bool ActivateChain(pime_core* instance, const std::string& chain,
                       uint64_t* out_generation) {
        LoadSchemePlugins(chain);
        std::vector<uint8_t> bytes;
        const std::wstring path = SchemeChainPath(
            config.resources_dir, config.data_dir, ChainStem(chain));
        if (path.empty() || !ReadFileBytes(path, &bytes)) {
            WriteLog(LogLevel::Error, "chain",
                     "cannot read scheme " + ChainStem(chain));
            return false;
        }
        uint64_t built = 0;
        pime_status status = pime_generation_build(
            instance, bytes.data(), static_cast<uint32_t>(bytes.size()), &built);
        if (status != PIME_OK) {
            WriteLog(LogLevel::Error, "chain",
                     "build " + chain + ": " + pime_status_string(status));
            return false;
        }
        status = pime_generation_activate(instance, built);
        if (status != PIME_OK) {
            WriteLog(LogLevel::Error, "chain",
                     "activate " + chain + ": " + pime_status_string(status));
            pime_generation_retire(instance, built);
            return false;
        }
        *out_generation = built;
        WriteLog(LogLevel::Info, "chain",
                 "active generation " + std::to_string(built) + " from " + chain);
        return true;
    }

    bool Initialize() {
        if (!EnsureDirectory(config.data_dir)) {
            WriteLog(LogLevel::Error, "engine", "cannot create data directory");
            return false;
        }
        InitLog(config.data_dir);
        WriteLog(LogLevel::Info, "engine",
                 "start resources=" + ToUtf8(config.resources_dir));

        pime_core_config core_config{};
        core_config.size = sizeof(core_config);
        core_config.version = PIME_ABI_MAJOR;
        const std::string data_dir = CorePath(config.data_dir);
        const std::string plugin_dir = CorePath(PathJoin(config.data_dir, L"plugins"));
        core_config.data_dir = data_dir.c_str();
        core_config.data_dir_len = static_cast<uint32_t>(data_dir.size());
        core_config.plugin_dir = plugin_dir.c_str();
        core_config.plugin_dir_len = static_cast<uint32_t>(plugin_dir.size());
        core_config.log_level = PIME_LOG_INFO;
        core_config.flags = PIME_FLAG_ALLOW_INSECURE;
        core_config.memory_budget_mb = 256;

        // Without a key every store call is denied: no learning, no user
        // dictionary. The vector only has to outlive pime_core_create, which
        // copies it into the core options.
        std::vector<uint8_t> store_key;
        if (LoadOrCreateStoreKey(config.data_dir, &store_key)) {
            core_config.store_key = store_key.data();
            core_config.store_key_len = static_cast<uint32_t>(store_key.size());
        }

        if (!EnsureDirectory(PathJoin(config.data_dir, L"plugins"))) {
            WriteLog(LogLevel::Error, "engine", "cannot create plugin directory");
            return false;
        }

        pime_host_sink sink{};
        sink.size = sizeof(sink);
        sink.version = PIME_ABI_MAJOR;
        sink.set_composition = &SinkComposition;
        sink.commit = &SinkCommit;
        sink.show_candidates = &SinkShowCandidates;
        sink.hide_candidates = &SinkHideCandidates;
        sink.audio_out = &SinkAudioOut;
        sink.beep = &SinkBeep;
        sink.clear_ink = &SinkClearInk;
        sink.notify = &SinkNotify;
        sink.on_log = &SinkLog;
        sink.wake = &SinkWake;

        core = pime_core_create(&core_config, &sink, this);
        if (!core) {
            WriteLog(LogLevel::Error, "engine", "pime_core_create failed");
            return false;
        }

        // Declared capabilities are not granted implicitly; the host decides.
        // Same set as the Android shell so the same plugin packages run.
        static const char* const kCapabilities[] = {
            "input.read",   "workspace",     "output.emit", "asset.read",
            "store.kv",     "time",          "context.read", "surrounding.read",
            "audio.capture", "audio.pcm",    "log",         "thread",
            "random",       "net.ws",        "net.wss",     "compress.gzip",
            "crypto.*",
        };
        for (const char* capability : kCapabilities) {
            pime_core_permission(core, capability,
                                 static_cast<uint32_t>(std::strlen(capability)), 1);
        }

        if (!ActivateChain(core, config.chain, &generation)) {
            // A scheme the user installed may have been removed; pinyin is
            // always part of the default install, so fall back to it.
            if (config.chain == "pinyin.cbor" ||
                !ActivateChain(core, "pinyin.cbor", &generation)) {
                return false;
            }
            WriteLog(LogLevel::Warn, "chain",
                     "fell back to pinyin from " + config.chain);
            config.chain = "pinyin.cbor";
        }

        uint32_t major = 0, minor = 0, patch = 0;
        pime_core_version(&major, &minor, &patch);
        WriteLog(LogLevel::Info, "engine",
                 "core " + std::to_string(major) + "." + std::to_string(minor) +
                     "." + std::to_string(patch) + " chain " + config.chain);
        return true;
    }

    void Pump() {
        if (!core) return;
        capturing = false;
        pime_core_pump(core);
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            stopping = true;
            cv.notify_all();
        }
        if (worker.joinable()) worker.join();
        if (core) {
            pime_core_destroy(core);
            core = nullptr;
        }
    }
};

namespace {

UiCommand MakeComposition(const pime_composition* value) {
    UiCommand command;
    command.kind = UiKind::Composition;
    if (value->text && value->text_len) {
        command.text.assign(value->text, value->text_len);
    }
    command.cursor = value->cursor;
    return command;
}

pime_status SinkComposition(void* ud, const pime_composition* value) {
    if (!ud || !value) return PIME_INVALID;
    static_cast<CoreEngine::Impl*>(ud)->Emit(MakeComposition(value));
    return PIME_OK;
}

pime_status SinkCommit(void* ud, const pime_text* value) {
    if (!ud || !value) return PIME_INVALID;
    UiCommand command;
    command.kind = UiKind::Commit;
    if (value->text && value->text_len) {
        command.text.assign(value->text, value->text_len);
    }
    static_cast<CoreEngine::Impl*>(ud)->Emit(std::move(command));
    return PIME_OK;
}

pime_status SinkShowCandidates(void* ud, const pime_candidate_list* value) {
    if (!ud || !value) return PIME_INVALID;
    UiCommand command;
    command.kind = UiKind::Candidates;
    command.candidates.reserve(value->count);
    for (uint32_t i = 0; i < value->count; ++i) {
        const pime_candidate& item = value->items[i];
        UiCandidate candidate;
        if (item.text && item.text_len) candidate.text.assign(item.text, item.text_len);
        if (item.comment && item.comment_len) {
            candidate.comment.assign(item.comment, item.comment_len);
        }
        if (item.label && item.label_len) {
            candidate.label.assign(item.label, item.label_len);
        }
        command.candidates.push_back(std::move(candidate));
    }
    static_cast<CoreEngine::Impl*>(ud)->Emit(std::move(command));
    return PIME_OK;
}

pime_status SinkHideCandidates(void* ud) {
    if (!ud) return PIME_INVALID;
    UiCommand command;
    command.kind = UiKind::HideCandidates;
    static_cast<CoreEngine::Impl*>(ud)->Emit(std::move(command));
    return PIME_OK;
}

pime_status SinkBeep(void* ud) {
    if (!ud) return PIME_INVALID;
    UiCommand command;
    command.kind = UiKind::Beep;
    static_cast<CoreEngine::Impl*>(ud)->Emit(std::move(command));
    return PIME_OK;
}

pime_status SinkNotify(void* ud, pime_status code) {
    if (!ud) return PIME_INVALID;
    UiCommand command;
    command.kind = UiKind::Notify;
    command.code = static_cast<int>(code);
    static_cast<CoreEngine::Impl*>(ud)->Emit(std::move(command));
    return PIME_OK;
}

void SinkLog(void* ud, const pime_log_record* record) {
    (void)ud;
    if (!record) return;
    const std::string source =
        record->source ? std::string(record->source, record->source_len) : "core";
    const std::string message =
        record->msg ? std::string(record->msg, record->msg_len) : std::string();
    LogLevel level = LogLevel::Info;
    if (record->level == PIME_LOG_ERROR) level = LogLevel::Error;
    else if (record->level == PIME_LOG_WARN) level = LogLevel::Warn;
    else if (record->level >= PIME_LOG_DEBUG) level = LogLevel::Debug;
    WriteLog(level, source.c_str(), message);
}

void SinkWake(void* ud) {
    auto* self = static_cast<CoreEngine::Impl*>(ud);
    if (!self) return;
    std::lock_guard<std::mutex> lock(self->mutex);
    self->cv.notify_all();
}

}  // namespace

CoreEngine& CoreEngine::Instance() {
    static CoreEngine* engine = new CoreEngine();
    return *engine;
}

CoreEngine::~CoreEngine() = default;

bool CoreEngine::Start(const EngineConfig& config) {
    if (!impl_) {
        static std::mutex init_mutex;
        std::lock_guard<std::mutex> lock(init_mutex);
        if (!impl_) impl_ = new Impl();
    }
    std::lock_guard<std::mutex> start_lock(impl_->start_mutex);
    if (impl_->start_state == 2) return impl_->ready;
    if (impl_->start_state == 1) return false;

    impl_->start_state = 1;
    impl_->config = config;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->running = true;
    }
    impl_->worker = std::thread([this] { impl_->WorkerMain(); });

    bool ok = false;
    impl_->Run([&] { ok = impl_->Initialize(); });
    impl_->ready = ok;
    impl_->start_state = 2;
    if (!ok) {
        WriteLog(LogLevel::Error, "engine", "startup failed");
    }
    return ok;
}

bool CoreEngine::ready() const {
    return impl_ && impl_->ready;
}

std::wstring CoreEngine::version() const {
    uint32_t major = 0, minor = 0, patch = 0;
    pime_core_version(&major, &minor, &patch);
    return std::to_wstring(major) + L"." + std::to_wstring(minor) + L"." +
           std::to_wstring(patch);
}

std::vector<std::wstring> CoreEngine::chains() const {
    std::vector<std::wstring> names;
    if (!impl_) return names;
    for (const SchemeInfo& scheme :
         ListSchemePackages(impl_->config.resources_dir,
                            impl_->config.data_dir)) {
        names.push_back(ToWide(scheme.id) + L".cbor");
    }
    return names;
}

std::wstring CoreEngine::active_chain() const {
    return impl_ ? ToWide(impl_->config.chain) : std::wstring();
}

SessionId CoreEngine::OpenSession(HWND notify, const std::wstring& app,
                                  uint32_t field_type, uint32_t host_caps,
                                  uint32_t page_size) {
    SessionId id = 0;
    if (!impl_ || !impl_->ready) return 0;
    impl_->Run([&] {
        pime_session_config config{};
        config.size = sizeof(config);
        config.version = PIME_ABI_MAJOR;
        pime_session* session = pime_session_create(impl_->core, &config);
        if (!session) {
            WriteLog(LogLevel::Error, "session", "pime_session_create failed");
            return;
        }

        auto storage = std::make_unique<Session>();
        storage->id = impl_->next_session++;
        storage->session = session;
        storage->notify = notify;
        storage->app = app;
        storage->field_type = field_type;
        storage->host_caps = host_caps;
        id = storage->id;

        const std::string app_utf8 = ToUtf8(app);
        const char locale[] = "zh-CN";
        pime_context context{};
        context.size = sizeof(context);
        context.version = PIME_ABI_MAJOR;
        context.app_id = app_utf8.c_str();
        context.app_id_len = static_cast<uint32_t>(app_utf8.size());
        context.field_type = field_type;
        context.input_type = PIME_INPUT_KEYBOARD;
        context.locale = locale;
        context.locale_len = static_cast<uint32_t>(std::strlen(locale));
        context.page_size = page_size ? page_size : impl_->config.page_size;
        context.host_caps = host_caps;
        context.max_candidates = 0;
        pime_core_context(session, &context);
        pime_core_candidate_budget(session, impl_->config.candidate_budget);
        impl_->sessions.emplace(id, std::move(storage));
    });
    return id;
}

void CoreEngine::CloseSession(SessionId session) {
    if (!impl_ || !session) return;
    impl_->Run([&] {
        const auto it = impl_->sessions.find(session);
        if (it == impl_->sessions.end()) return;
        if (impl_->active == it->second.get()) impl_->active = nullptr;
        pime_session_destroy(it->second->session);
        impl_->sessions.erase(it);
    });
}

KeyResult CoreEngine::Key(SessionId id, const KeyEvent& event) {
    KeyResult result;
    if (!impl_ || !impl_->ready) return result;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        pime_key_event key{};
        key.size = sizeof(key);
        key.version = PIME_ABI_MAJOR;
        key.keycode = event.keycode;
        key.modifiers = event.modifiers;
        key.action = event.action;
        key.text = event.text.empty() ? nullptr : event.text.data();
        key.text_len = static_cast<uint32_t>(event.text.size());
        key.time_ms = GetTickCount64();

        impl_->capture.clear();
        impl_->capturing = true;
        int handled = 0;
        const pime_status status = pime_core_key(session->session, &key, &handled);
        pime_core_pump(impl_->core);
        impl_->capturing = false;

        result.handled = handled != 0;
        result.commands = std::move(impl_->capture);
        impl_->capture.clear();
        if (status != PIME_OK) {
            WriteLog(LogLevel::Warn, "key",
                     std::string("pime_core_key: ") + pime_status_string(status) +
                         " session=" + std::to_string(id) +
                         " core_session=" +
                         std::to_string(pime_session_id(session->session)));
        }
    });
    return result;
}

KeyResult CoreEngine::Select(SessionId id, uint32_t index) {
    KeyResult result;
    if (!impl_ || !impl_->ready) return result;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        pime_candidate_event event{};
        event.size = sizeof(event);
        event.version = PIME_ABI_MAJOR;
        event.index = index;

        impl_->capture.clear();
        impl_->capturing = true;
        const pime_status status = pime_core_candidate(session->session, &event);
        pime_core_pump(impl_->core);
        impl_->capturing = false;

        result.handled = status == PIME_OK;
        result.commands = std::move(impl_->capture);
        impl_->capture.clear();
    });
    return result;
}

KeyResult CoreEngine::Surrounding(SessionId id, const std::string& before,
                                  const std::string& after) {
    KeyResult result;
    if (!impl_ || !impl_->ready) return result;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        pime_text before_text{};
        before_text.size = sizeof(before_text);
        before_text.version = PIME_ABI_MAJOR;
        before_text.text = before.empty() ? nullptr : before.data();
        before_text.text_len = static_cast<uint32_t>(before.size());
        pime_text after_text{};
        after_text.size = sizeof(after_text);
        after_text.version = PIME_ABI_MAJOR;
        after_text.text = after.empty() ? nullptr : after.data();
        after_text.text_len = static_cast<uint32_t>(after.size());

        impl_->capture.clear();
        impl_->capturing = true;
        const pime_status status =
            pime_core_surrounding(session->session, &before_text, &after_text);
        pime_core_pump(impl_->core);
        impl_->capturing = false;

        result.commands = std::move(impl_->capture);
        impl_->capture.clear();
        if (status != PIME_OK) {
            WriteLog(LogLevel::Warn, "surrounding",
                     std::string("pime_core_surrounding: ") + pime_status_string(status));
            return;
        }
        // A surrounding refresh only describes the caret's left context, so a
        // strip that survives it would be stale (the text it was predicted
        // from is gone). Predict plugins that have nothing to offer emit no
        // command at all, so drop the strip here instead.
        bool strip_command = false;
        for (const UiCommand& command : result.commands) {
            if (command.kind == UiKind::Candidates ||
                command.kind == UiKind::HideCandidates) {
                strip_command = true;
                break;
            }
        }
        if (!strip_command) {
            UiCommand hide;
            hide.kind = UiKind::HideCandidates;
            result.commands.push_back(std::move(hide));
        }
    });
    return result;
}

void CoreEngine::Focus(SessionId id, bool focused) {
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        session->focused = focused;
        if (focused) impl_->active = session;
        else if (impl_->active == session) impl_->active = nullptr;
        pime_core_focus(session->session, focused ? 1 : 0);
    });
}

void CoreEngine::Reset(SessionId id) {
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        session->pending.clear();
        pime_core_reset(session->session);
    });
}

void CoreEngine::UpdateContext(SessionId id, const std::wstring& app,
                               uint32_t field_type, uint32_t host_caps) {
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        session->app = app;
        session->field_type = field_type;
        session->host_caps = host_caps;
        const std::string app_utf8 = ToUtf8(app);
        const char locale[] = "zh-CN";
        pime_context context{};
        context.size = sizeof(context);
        context.version = PIME_ABI_MAJOR;
        context.app_id = app_utf8.c_str();
        context.app_id_len = static_cast<uint32_t>(app_utf8.size());
        context.field_type = field_type;
        context.input_type = PIME_INPUT_KEYBOARD;
        context.locale = locale;
        context.locale_len = static_cast<uint32_t>(std::strlen(locale));
        context.page_size = impl_->config.page_size;
        context.host_caps = host_caps;
        context.max_candidates = 0;
        pime_core_context(session->session, &context);
    });
}

bool CoreEngine::TakeAsync(SessionId id, std::vector<UiCommand>* out) {
    if (!impl_ || !impl_->ready) return false;
    bool any = false;
    impl_->Run([&] {
        Session* session = impl_->Find(id);
        if (!session) return;
        any = !session->pending.empty();
        if (any) {
            out->insert(out->end(),
                        std::make_move_iterator(session->pending.begin()),
                        std::make_move_iterator(session->pending.end()));
            session->pending.clear();
        }
    });
    return any;
}

bool CoreEngine::SwitchChain(const std::string& chain) {
    if (!impl_ || !impl_->ready) return false;
    bool ok = false;
    impl_->Run([&] {
        // Compare on the worker: every IPC client reloads settings at once, so
        // a check on the caller's thread would let them all queue a rebuild.
        if (chain == impl_->config.chain) {
            ok = true;
            return;
        }
        uint64_t next = 0;
        if (!impl_->ActivateChain(impl_->core, chain, &next)) return;
        const uint64_t previous = impl_->generation;
        impl_->generation = next;
        impl_->config.chain = chain;
        if (previous) pime_generation_retire(impl_->core, previous);
        ok = true;
    });
    return ok;
}

void CoreEngine::FlushStore() {
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] { impl_->FlushStore(true); });
}

void CoreEngine::UserDictList(uint32_t offset, uint32_t limit,
                              UserDictReply* out) {
    out->status = PIME_CLOSED;
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        std::vector<std::pair<std::string, std::string>> entries;
        out->status = pime_core_store_scan(
            impl_->core, kUserDictAlias, kUserDictAliasLen, kUserDictPrefix,
            kUserDictPrefixLen, 0, &CollectStoreEntry, &entries);
        out->words.clear();
        if (out->status != PIME_OK) return;

        std::vector<UserWord> words;
        words.reserve(entries.size());
        for (const auto& entry : entries) {
            UserWord word;
            if (!ParseUserDictKey(entry.first, &word.code, &word.word)) continue;
            word.weight = DecodeUserDictValue(entry.second);
            words.push_back(std::move(word));
        }
        out->total = static_cast<uint32_t>(words.size());

        std::sort(words.begin(), words.end(),
                  [](const UserWord& a, const UserWord& b) {
                      if (a.weight != b.weight) return a.weight > b.weight;
                      if (a.word != b.word) return a.word < b.word;
                      return a.code < b.code;
                  });
        if (offset >= words.size()) return;
        const size_t end =
            limit == 0 ? words.size()
                       : std::min(words.size(), static_cast<size_t>(offset) + limit);
        out->words.assign(std::make_move_iterator(words.begin() + offset),
                          std::make_move_iterator(words.begin() + end));
    });
}

void CoreEngine::UserDictSet(const std::vector<UserWord>& words,
                             UserDictReply* out) {
    out->status = PIME_CLOSED;
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        pime_status first = PIME_OK;
        for (const UserWord& word : words) {
            const std::string key = MakeUserDictKey(word.code, word.word);
            const std::string value = EncodeUserDictValue(word.weight);
            const pime_status status = pime_core_store_set(
                impl_->core, kUserDictAlias, kUserDictAliasLen, key.data(),
                static_cast<uint32_t>(key.size()), value.data(),
                static_cast<uint32_t>(value.size()));
            if (status != PIME_OK && first == PIME_OK) first = status;
            if (status == PIME_FULL) break;
        }
        const pime_status flushed = pime_core_store_flush(
            impl_->core, kUserDictAlias, kUserDictAliasLen);
        out->status = first != PIME_OK ? first : flushed;
    });
}

void CoreEngine::UserDictDelete(const std::string& code,
                                const std::string& word,
                                UserDictReply* out) {
    out->status = PIME_CLOSED;
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        const std::string key = MakeUserDictKey(code, word);
        int deleted = 0;
        pime_status status = pime_core_store_del(
            impl_->core, kUserDictAlias, kUserDictAliasLen, key.data(),
            static_cast<uint32_t>(key.size()), &deleted);
        if (status == PIME_OK && deleted) {
            status = pime_core_store_flush(impl_->core, kUserDictAlias,
                                           kUserDictAliasLen);
        } else if (status == PIME_OK) {
            status = PIME_NOT_FOUND;
        }
        out->status = status;
    });
}

void CoreEngine::UserDictClear(UserDictReply* out) {
    out->status = PIME_CLOSED;
    if (!impl_ || !impl_->ready) return;
    impl_->Run([&] {
        std::vector<std::pair<std::string, std::string>> entries;
        pime_status status = pime_core_store_scan(
            impl_->core, kUserDictAlias, kUserDictAliasLen, kUserDictPrefix,
            kUserDictPrefixLen, 0, &CollectStoreEntry, &entries);
        if (status != PIME_OK) {
            out->status = status;
            return;
        }
        for (const auto& entry : entries) {
            int deleted = 0;
            const pime_status erased = pime_core_store_del(
                impl_->core, kUserDictAlias, kUserDictAliasLen,
                entry.first.data(), static_cast<uint32_t>(entry.first.size()),
                &deleted);
            if (erased != PIME_OK && status == PIME_OK) status = erased;
        }
        const pime_status flushed = pime_core_store_flush(
            impl_->core, kUserDictAlias, kUserDictAliasLen);
        out->status = status != PIME_OK ? status : flushed;
    });
}

}  // namespace pimewin
