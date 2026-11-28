#include "tsf/ipc_client.h"

#include <algorithm>
#include <chrono>

#include "common/log.h"
#include "common/win_util.h"

namespace pimewin {
namespace {

std::wstring QuoteArg(const std::wstring& path) {
    return L"\"" + path + L"\"";
}

// Elevated hosts must not hand their token to the engine: a medium-integrity
// engine keeps plugins, chains and the settings file at the user's level.
bool SpawnEngine(const std::wstring& exe) {
    std::wstring cmd = QuoteArg(exe) + L" --serve";
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    if (!IsElevated()) {
        const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (ok) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
        }
        return ok != FALSE;
    }

    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &token)) {
        return false;
    }
    HANDLE medium = nullptr;
    bool ok = false;
    if (DuplicateTokenEx(token, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation,
                         TokenPrimary, &medium)) {
        BYTE sid_buffer[SECURITY_MAX_SID_SIZE]{};
        DWORD sid_size = sizeof(sid_buffer);
        if (CreateWellKnownSid(WinMediumLabelSid, nullptr, sid_buffer, &sid_size)) {
            TOKEN_MANDATORY_LABEL label{};
            label.Label.Attributes = SE_GROUP_INTEGRITY;
            label.Label.Sid = reinterpret_cast<PSID>(sid_buffer);
            if (SetTokenInformation(medium, TokenIntegrityLevel, &label,
                                    sizeof(label) + sid_size)) {
                ok = CreateProcessAsUserW(medium, exe.c_str(), cmd.data(), nullptr, nullptr,
                                          FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                                          &pi) != FALSE;
                if (ok) {
                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);
                }
            }
        }
        CloseHandle(medium);
    }
    CloseHandle(token);
    return ok;
}

}  // namespace

IpcClient& IpcClient::Instance() {
    static IpcClient client;
    return client;
}

IpcClient::~IpcClient() {
    stopping_ = true;
    if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
    if (supervisor_.joinable()) supervisor_.join();
}

bool IpcClient::Start(const std::wstring& engine_exe) {
    {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (supervisor_.joinable()) {
            // Already connecting or connected; wait for the handshake instead.
        } else {
            engine_exe_ = engine_exe;
            supervisor_ = std::thread(&IpcClient::Supervise, this);
        }
    }
    std::unique_lock<std::mutex> lock(lifecycle_mutex_);
    return ready_cv_.wait_for(lock, std::chrono::seconds(8), [&] { return ready_.load(); });
}

void IpcClient::Supervise() {
    // 1 s, 2 s, 4 s ... capped at 30 s.
    DWORD backoff = 1000;
    while (!stopping_) {
        if (!ConnectOnce()) {
            // Spawn on every failed attempt, not just the first: an engine
            // that exits or is replaced by an update must come back without
            // the host process restarting. Extra hosts racing here lose the
            // singleton mutex and exit immediately.
            if (!SpawnEngine(engine_exe_)) {
                WriteLog(LogLevel::Error, "ipc", "spawn failed: " + ToUtf8(engine_exe_));
            }
            for (DWORD slept = 0; slept < backoff && !stopping_; slept += 100) Sleep(100);
            backoff = std::min<DWORD>(backoff * 2, 30000);
            continue;
        }
        backoff = 1000;
        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            ready_ = true;
        }
        ready_cv_.notify_all();
        std::thread reader(&IpcClient::ReadLoop, this);
        ReopenSessions();

        reader.join();  // returns when the pipe breaks

        {
            std::lock_guard<std::mutex> lock(lifecycle_mutex_);
            ready_ = false;
            {
                std::lock_guard<std::mutex> reply_lock(reply_mutex_);
                reply_cv_.notify_all();
            }
        }
        WriteLog(LogLevel::Warn, "ipc", "engine disconnected; reconnecting");
    }
}

bool IpcClient::ConnectOnce() {
    const std::wstring name = EnginePipeName();
    WaitNamedPipeW(name.c_str(), 200);
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return false;

    Message hello;
    hello.kind = MsgKind::Hello;
    ByteWriter writer;
    writer.Str(std::string());
    writer.U32(0);
    writer.U32(0);
    writer.U32(0);
    hello.payload = std::move(writer.data());
    if (!WriteMessage(pipe, hello)) {
        CloseHandle(pipe);
        return false;
    }

    Message reply;
    if (!ReadMessage(pipe, &reply) || reply.kind != MsgKind::HelloReply) {
        CloseHandle(pipe);
        return false;
    }
    AdoptHello(reply);
    pipe_ = pipe;
    return true;
}

void IpcClient::AdoptHello(const Message& reply) {
    ByteReader reader(reply.payload.data(), reply.payload.size());
    std::string version;
    Settings settings;
    if (!reader.Str(&version) || !DecodeSettings(reader, &settings)) return;
    version_ = ToWide(version);
    {
        std::lock_guard<std::mutex> lock(settings_mutex_);
        engine_settings_ = settings;
    }
    WriteLog(LogLevel::Info, "ipc",
             "engine " + version + " connected (chinese=" +
                 std::to_string(settings.chinese) + ")");
}

Settings IpcClient::settings() const {
    std::lock_guard<std::mutex> lock(settings_mutex_);
    return engine_settings_;
}

void IpcClient::ReadLoop() {
    Message message;
    while (ReadMessage(pipe_, &message)) {
        if (message.kind == MsgKind::UiCommands || message.kind == MsgKind::ModeChanged) {
            DispatchPush(message);
            continue;
        }
        std::lock_guard<std::mutex> lock(reply_mutex_);
        if (message.seq != waiting_seq_) continue;
        reply_message_ = std::move(message);
        reply_ready_ = true;
        reply_cv_.notify_all();
    }
    CloseHandle(pipe_);
    pipe_ = INVALID_HANDLE_VALUE;
}

uint64_t IpcClient::Remote(SessionId session) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = sessions_.find(session);
    return it == sessions_.end() ? 0 : it->second.remote;
}

bool IpcClient::Request(MsgKind kind, SessionId session, uint32_t flags,
                        const std::string& payload, Message* reply) {
    if (!ready_) return false;
    std::lock_guard<std::mutex> request_lock(request_mutex_);
    if (!ready_) return false;
    Message message;
    message.kind = kind;
    message.session = Remote(session);
    message.flags = flags;
    message.payload = payload;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        message.seq = ++seq_;
        waiting_seq_ = message.seq;
        reply_ready_ = false;
    }
    {
        std::lock_guard<std::mutex> lock(write_mutex_);
        if (!WriteMessage(pipe_, message)) {
            WriteLog(LogLevel::Warn, "ipc",
                     "write failed kind=" + std::to_string(static_cast<uint32_t>(kind)) +
                         " err=" + std::to_string(GetLastError()));
            return false;
        }
    }
    std::unique_lock<std::mutex> lock(reply_mutex_);
    const bool got = reply_cv_.wait_for(lock, std::chrono::seconds(5), [&] {
        return reply_ready_ && waiting_seq_ == message.seq;
    });
    if (!got) return false;
    if (reply) *reply = std::move(reply_message_);
    return true;
}

void IpcClient::Send(MsgKind kind, SessionId session, uint32_t flags,
                     const std::string& payload) {
    if (!ready_) return;
    Message message;
    message.kind = kind;
    message.session = Remote(session);
    message.flags = flags;
    message.payload = payload;
    std::lock_guard<std::mutex> lock(write_mutex_);
    WriteMessage(pipe_, message);
}

void IpcClient::DispatchPush(const Message& message) {
    HWND notify = nullptr;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        const auto it = remote_to_local_.find(message.session);
        if (it == remote_to_local_.end()) return;
        if (message.kind == MsgKind::ModeChanged) {
            mode_pending_[it->second] = message.flags != 0;
        } else {
            std::vector<UiCommand> commands;
            ByteReader reader(message.payload.data(), message.payload.size());
            if (!DecodeCommands(reader, &commands)) return;
            auto& pending = pending_[it->second];
            pending.insert(pending.end(), std::make_move_iterator(commands.begin()),
                           std::make_move_iterator(commands.end()));
        }
        notify = sessions_[it->second].notify;
    }
    if (notify) PostMessageW(notify, kEngineWakeMessage, 0, 0);
}

bool IpcClient::ReopenSessions() {
    std::vector<std::pair<SessionId, Tracked>> snapshot;
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        for (const auto& entry : sessions_) snapshot.emplace_back(entry.first, entry.second);
    }
    for (const auto& entry : snapshot) {
        ByteWriter writer;
        writer.Str(entry.second.app);
        writer.U32(entry.second.field_type);
        writer.U32(entry.second.host_caps);
        writer.U32(entry.second.page_size);
        Message reply;
        if (!Request(MsgKind::OpenSession, 0, 0, writer.data(), &reply)) continue;
        if (!reply.session) continue;
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_[entry.first].remote = reply.session;
        remote_to_local_[reply.session] = entry.first;
    }
    return true;
}

SessionId IpcClient::OpenSession(HWND notify, const std::wstring& app,
                                 uint32_t field_type, uint32_t host_caps,
                                 uint32_t page_size, bool* chinese) {
    if (!ready_) return 0;
    ByteWriter writer;
    writer.Str(ToUtf8(app));
    writer.U32(field_type);
    writer.U32(host_caps);
    writer.U32(page_size);
    Message reply;
    if (!Request(MsgKind::OpenSession, 0, 0, writer.data(), &reply)) return 0;
    if (!reply.session) return 0;
    if (chinese) {
        uint32_t mode = 0;
        ByteReader reader(reply.payload.data(), reply.payload.size());
        if (reader.U32(&mode)) *chinese = mode != 0;
    }

    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const SessionId local = next_session_++;
    Tracked tracked;
    tracked.remote = reply.session;
    tracked.notify = notify;
    tracked.app = ToUtf8(app);
    tracked.field_type = field_type;
    tracked.host_caps = host_caps;
    tracked.page_size = page_size;
    sessions_[local] = tracked;
    remote_to_local_[reply.session] = local;
    return local;
}

void IpcClient::CloseSession(SessionId session) {
    if (!ready_ || !session) return;
    Send(MsgKind::CloseSession, session, 0, std::string());
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = sessions_.find(session);
    if (it == sessions_.end()) return;
    remote_to_local_.erase(it->second.remote);
    pending_.erase(session);
    mode_pending_.erase(session);
    sessions_.erase(it);
}

KeyResult IpcClient::Key(SessionId session, const KeyEvent& event) {
    KeyResult result;
    if (!ready_) return result;
    ByteWriter writer;
    writer.U32(event.keycode);
    writer.U32(event.modifiers);
    writer.U32(event.action);
    writer.Str(event.text);
    Message reply;
    if (!Request(MsgKind::Key, session, 0, writer.data(), &reply)) return result;
    result.handled = reply.flags != 0;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    DecodeCommands(reader, &result.commands);
    return result;
}

KeyResult IpcClient::Select(SessionId session, uint32_t index) {
    KeyResult result;
    if (!ready_) return result;
    ByteWriter writer;
    writer.U32(index);
    Message reply;
    if (!Request(MsgKind::Select, session, 0, writer.data(), &reply)) return result;
    result.handled = reply.flags != 0;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    DecodeCommands(reader, &result.commands);
    return result;
}

void IpcClient::Focus(SessionId session, bool focused) {
    Send(MsgKind::Focus, session, focused ? 1 : 0, std::string());
}

void IpcClient::Reset(SessionId session) {
    Send(MsgKind::Reset, session, 0, std::string());
}

void IpcClient::SaveAppMode(SessionId session, const std::wstring& app, bool chinese,
                            const RECT& caret) {
    ByteWriter writer;
    writer.Str(ToUtf8(app));
    WriteCaret(writer, caret);
    Send(MsgKind::SaveAppMode, session, chinese ? 1 : 0, writer.data());
}

void IpcClient::SendCaret(SessionId session, const RECT& caret) {
    ByteWriter writer;
    WriteCaret(writer, caret);
    Send(MsgKind::CaretRect, session, 0, writer.data());
}

void IpcClient::HideCandidates(SessionId session) {
    Send(MsgKind::HideCandidates, session, 0, std::string());
}

bool IpcClient::TakeModeChange(SessionId session, bool* chinese) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = mode_pending_.find(session);
    if (it == mode_pending_.end()) return false;
    *chinese = it->second;
    mode_pending_.erase(it);
    return true;
}

void IpcClient::UpdateContext(SessionId session, const std::wstring& app,
                              uint32_t field_type, uint32_t host_caps) {
    ByteWriter writer;
    writer.Str(ToUtf8(app));
    writer.U32(field_type);
    writer.U32(host_caps);
    Send(MsgKind::UpdateContext, session, 0, writer.data());
}

void IpcClient::Surrounding(SessionId session, const std::string& before,
                            const std::string& after) {
    ByteWriter writer;
    writer.Str(before);
    writer.Str(after);
    Send(MsgKind::Surrounding, session, 0, writer.data());
}

bool IpcClient::RefreshSettings() {
    ByteWriter writer;
    writer.Str(std::string());
    writer.U32(0);
    Message reply;
    if (!Request(MsgKind::ReloadSettings, 0, 0, writer.data(), &reply)) return false;
    AdoptHello(reply);
    return true;
}

bool IpcClient::SwitchChain(const std::string& chain) {
    ByteWriter writer;
    writer.Str(chain);
    writer.U32(0);
    Message reply;
    if (!Request(MsgKind::ReloadSettings, 0, 0, writer.data(), &reply)) return false;
    AdoptHello(reply);
    return true;
}

bool IpcClient::UserDictList(uint32_t offset, uint32_t limit,
                             UserDictReply* out) {
    ByteWriter writer;
    writer.U32(static_cast<uint32_t>(UserDictOp::List));
    writer.U32(offset);
    writer.U32(limit);
    Message reply;
    if (!Request(MsgKind::UserDict, 0, 0, writer.data(), &reply)) return false;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    return DecodeUserDictReply(reader, out);
}

bool IpcClient::UserDictSet(const std::vector<UserWord>& words, int* status) {
    ByteWriter writer;
    writer.U32(static_cast<uint32_t>(UserDictOp::BatchSet));
    EncodeUserWords(writer, words);
    Message reply;
    if (!Request(MsgKind::UserDict, 0, 0, writer.data(), &reply)) return false;
    UserDictReply result;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    if (!DecodeUserDictReply(reader, &result)) return false;
    *status = result.status;
    return true;
}

bool IpcClient::UserDictDelete(const std::string& code,
                               const std::string& word, int* status) {
    ByteWriter writer;
    writer.U32(static_cast<uint32_t>(UserDictOp::Delete));
    writer.Str(code);
    writer.Str(word);
    Message reply;
    if (!Request(MsgKind::UserDict, 0, 0, writer.data(), &reply)) return false;
    UserDictReply result;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    if (!DecodeUserDictReply(reader, &result)) return false;
    *status = result.status;
    return true;
}

bool IpcClient::UserDictClear(int* status) {
    ByteWriter writer;
    writer.U32(static_cast<uint32_t>(UserDictOp::Clear));
    Message reply;
    if (!Request(MsgKind::UserDict, 0, 0, writer.data(), &reply)) return false;
    UserDictReply result;
    ByteReader reader(reply.payload.data(), reply.payload.size());
    if (!DecodeUserDictReply(reader, &result)) return false;
    *status = result.status;
    return true;
}

bool IpcClient::TakeAsync(SessionId session, std::vector<UiCommand>* out) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    const auto it = pending_.find(session);
    if (it == pending_.end() || it->second.empty()) return false;
    out->insert(out->end(), std::make_move_iterator(it->second.begin()),
                std::make_move_iterator(it->second.end()));
    pending_.erase(it);
    return true;
}

std::wstring IpcClient::version() const {
    return version_;
}

}  // namespace pimewin
