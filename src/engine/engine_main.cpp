// pime_engine.exe --serve: owns pime-core for every TSF host on this desktop.
//
// The shell (pime_tsf.dll) speaks the framing in common/ipc.h over a named
// pipe. Every pime-core call is marshalled onto CoreEngine's owner thread by
// CoreEngine::Run, so a pipe reader thread may call straight through.
#include <windows.h>
#include <shellapi.h>

#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <pime/hal.h>

#include "common/ipc.h"
#include "common/log.h"
#include "common/settings.h"
#include "common/win_util.h"
#include "engine/core_engine.h"
#include "ui/candidate_window.h"

namespace pimewin {
namespace {

constexpr UINT kEngineUiMessage = WM_APP + 0x52;
constexpr UINT kEngineQuitMessage = WM_APP + 0x53;

struct Client {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    std::mutex write_mutex;
    std::thread reader;
};

// Per-session state the engine needs to draw the strip: the host only applies
// text, so paging, the candidate list and the caret all live here now.
struct SessionUi {
    std::string app;
    bool chinese = true;
    uint32_t page = 0;
    uint32_t page_size = 6;
    bool candidates_up = false;
    std::vector<CandidateItem> items;
    RECT caret{};
};

std::mutex g_state_mutex;
std::set<Client*> g_clients;
std::map<SessionId, Client*> g_session_owner;
std::map<SessionId, SessionUi> g_ui;
std::deque<std::function<void()>> g_ui_tasks;
Settings g_settings;
HWND g_wake = nullptr;
CandidateWindow g_window;
SessionId g_shown = 0;  // session whose strip is up; UI thread only

Settings CurrentSettings() {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    return g_settings;
}

bool DarkFor(uint32_t theme) {
    if (theme == 1) return false;
    if (theme == 2) return true;
    return !SystemUsesLightTheme();
}

// The host used to compute the page itself; the engine does now, from the same
// keys, because only it renders the page arrows.
uint32_t PageAfter(uint32_t page, uint32_t vkey) {
    switch (vkey) {
        case VK_PRIOR:
        case VK_UP:
            return page > 0 ? page - 1 : 0;
        case VK_NEXT:
        case VK_DOWN:
            return page + 1;
        default:
            return 0;
    }
}

// Runs `task` on the UI thread, where the layered window and D2D live.
void PostUi(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_ui_tasks.push_back(std::move(task));
    }
    if (g_wake) PostMessageW(g_wake, kEngineUiMessage, 0, 0);
}

void SendTo(Client* client, const Message& message) {
    std::lock_guard<std::mutex> lock(client->write_mutex);
    WriteMessage(client->pipe, message);
}

Client* OwnerOf(SessionId session) {
    std::lock_guard<std::mutex> lock(g_state_mutex);
    const auto it = g_session_owner.find(session);
    return it == g_session_owner.end() ? nullptr : it->second;
}

// Sends the host the commands it still has to apply: only the TSF context work
// (composition, commit). Candidates and beeps are handled here.
void PushHostCommands(SessionId session, const std::vector<UiCommand>& commands) {
    if (commands.empty()) return;
    Client* owner = OwnerOf(session);
    if (!owner) return;
    Message message;
    message.kind = MsgKind::UiCommands;
    message.session = session;
    ByteWriter writer;
    EncodeCommands(writer, commands);
    message.payload = std::move(writer.data());
    SendTo(owner, message);
}

void PushModeChanged(SessionId session, bool chinese) {
    Client* owner = OwnerOf(session);
    if (!owner) return;
    Message message;
    message.kind = MsgKind::ModeChanged;
    message.session = session;
    message.flags = chinese ? 1 : 0;
    SendTo(owner, message);
}

// Consumes the strip-driving commands, records what the window should show,
// and returns the commands the host must still apply. Any thread.
std::vector<UiCommand> ApplyCommands(SessionId session, const std::vector<UiCommand>& commands) {
    std::vector<UiCommand> host;
    std::vector<CandidateItem> items;
    bool show = false;
    bool hide = false;
    for (const UiCommand& command : commands) {
        switch (command.kind) {
            case UiKind::Candidates:
                items.clear();
                items.reserve(command.candidates.size());
                for (const UiCandidate& candidate : command.candidates) {
                    items.push_back({ToWide(candidate.text), ToWide(candidate.comment),
                                     ToWide(candidate.label)});
                }
                show = true;
                break;
            case UiKind::HideCandidates:
                hide = true;
                break;
            case UiKind::Beep:
                MessageBeep(MB_OK);
                break;
            case UiKind::Composition:
            case UiKind::Commit:
                host.push_back(command);
                break;
            case UiKind::Notify:
                break;
        }
    }

    bool committed = false;
    for (const UiCommand& command : host) {
        if (command.kind == UiKind::Commit) committed = true;
    }
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_ui.find(session);
        if (it == g_ui.end()) return host;
        SessionUi& ui = it->second;
        // Commit and hide end the strip even when the same batch also carries
        // fresh candidates: pime-core emits a commit followed by a candidates
        // + hide pair after space/enter/digit picks.
        if (committed || hide || !show || items.empty()) {
            ui.items.clear();
            ui.candidates_up = false;
            ui.page = 0;
        } else {
            ui.items = std::move(items);
            ui.candidates_up = true;
        }
    }
    return host;
}

void HideStrip(SessionId session) {
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_ui.find(session);
        if (it != g_ui.end()) {
            it->second.items.clear();
            it->second.candidates_up = false;
            it->second.page = 0;
        }
    }
    PostUi([session] {
        if (g_shown == session) {
            g_window.Hide();
            g_shown = 0;
        }
    });
}

// Draws the strip from the session's state. UI thread only.
void Render(SessionId session, bool badge = false) {
    SessionUi ui;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        const auto it = g_ui.find(session);
        if (it == g_ui.end()) return;
        ui = it->second;
    }
    g_window.SetDark(DarkFor(CurrentSettings().theme));
    if (badge) {
        RECT caret = ui.caret;
        if (!caret.right && !caret.bottom) {
            POINT point{};
            GetCursorPos(&point);
            caret = RECT{point.x, point.y, point.x, point.y};
        }
        g_window.ShowModeBadge(ui.chinese, caret);
        g_shown = 0;
        return;
    }
    if (!ui.candidates_up || ui.items.empty()) {
        if (g_shown == session) {
            g_window.Hide();
            g_shown = 0;
        }
        return;
    }
    RECT caret = ui.caret;
    if (!caret.right && !caret.bottom) {
        POINT point{};
        GetCursorPos(&point);
        caret = RECT{point.x, point.y, point.x, point.y};
    }
    g_window.ShowCandidates(ui.items, static_cast<int>(ui.page), ui.page > 0,
                            ui.items.size() >= ui.page_size, ui.chinese, caret);
    g_shown = session;
}

// A key or click resolved on the UI thread (a strip click): hand the host its
// share and redraw what is left.
void ApplyLocal(SessionId session, const KeyResult& result) {
    PushHostCommands(session, ApplyCommands(session, result.commands));
    Render(session);
}

struct EngineUi final : CandidateWindow::Delegate {
    void OnCandidatePicked(int slot) override {
        if (!g_shown || slot < 0) return;
        const SessionId session = g_shown;
        KeyEvent event;
        event.keycode = static_cast<uint32_t>('1' + slot);
        event.action = PIME_KEY_DOWN;
        ApplyLocal(session, CoreEngine::Instance().Key(session, event));
    }
    void OnPageRequested(int direction) override {
        if (!g_shown || direction == 0) return;
        const SessionId session = g_shown;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            SessionUi& ui = g_ui[session];
            ui.page = PageAfter(ui.page, direction < 0 ? VK_PRIOR : VK_NEXT);
        }
        KeyEvent event;
        event.keycode = direction < 0 ? VK_PRIOR : VK_NEXT;
        event.action = PIME_KEY_DOWN;
        ApplyLocal(session, CoreEngine::Instance().Key(session, event));
    }
    void OnModeToggled() override {
        if (!g_shown) return;
        const SessionId session = g_shown;
        bool chinese = false;
        std::string app;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            SessionUi& ui = g_ui[session];
            ui.chinese = !ui.chinese;
            ui.items.clear();
            ui.candidates_up = false;
            ui.page = 0;
            chinese = ui.chinese;
            app = ui.app;
        }
        SaveAppMode(ToWide(app), chinese);
        CoreEngine::Instance().Reset(session);
        g_window.Hide();
        g_shown = 0;
        Render(session, true);
        PushModeChanged(session, chinese);
    }
};

EngineUi g_ui_delegate;

void DrainUi() {
    std::deque<std::function<void()>> tasks;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        tasks.swap(g_ui_tasks);
    }
    for (auto& task : tasks) task();
}

Message Reply(MsgKind kind, const Message& request) {
    Message reply;
    reply.kind = kind;
    reply.seq = request.seq;
    reply.session = request.session;
    return reply;
}

// Re-reads HKCU here, outside any packaged host's virtualization, and caches
// the result: the shell has no other way to see real settings.
Message HelloReply(const Message& request) {
    Message reply = Reply(MsgKind::HelloReply, request);
    const Settings settings = LoadSettings();
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        g_settings = settings;
        for (auto& entry : g_ui) {
            entry.second.page_size = settings.page_size;
        }
    }
    ByteWriter writer;
    writer.Str(ToUtf8(CoreEngine::Instance().version()));
    EncodeSettings(writer, settings);
    reply.payload = std::move(writer.data());
    return reply;
}

void DrainSessions() {
    std::vector<SessionId> ids;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (const auto& entry : g_ui) ids.push_back(entry.first);
    }
    for (SessionId id : ids) {
        std::vector<UiCommand> commands;
        if (!CoreEngine::Instance().TakeAsync(id, &commands) || commands.empty()) continue;
        PushHostCommands(id, ApplyCommands(id, commands));
        PostUi([id] { Render(id); });
    }
}

LRESULT CALLBACK WakeProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (msg == kEngineWakeMessage) {
        DrainSessions();
        return 0;
    }
    if (msg == kEngineUiMessage) {
        DrainUi();
        return 0;
    }
    if (msg == kEngineQuitMessage) {
        WriteLog(LogLevel::Info, "engine", "shutting down for update");
        g_window.Hide();
        g_shown = 0;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void HandleMessage(Client* client, const Message& request) {
    ByteReader reader(request.payload.data(), request.payload.size());
    switch (request.kind) {
        case MsgKind::Hello:
            SendTo(client, HelloReply(request));
            return;
        case MsgKind::OpenSession: {
            std::string app;
            uint32_t field_type = 0, host_caps = 0, page_size = 0;
            if (!reader.Str(&app) || !reader.U32(&field_type) ||
                !reader.U32(&host_caps) || !reader.U32(&page_size)) {
                return;
            }
            const SessionId id = CoreEngine::Instance().OpenSession(
                g_wake, ToWide(app), field_type, host_caps, page_size);
            const Settings settings = CurrentSettings();
            const bool chinese = LoadAppMode(ToWide(app), settings.chinese != 0);
            WriteLog(LogLevel::Info, "ipc",
                     "open session " + std::to_string(id) + " page=" +
                         std::to_string(page_size) + " chinese=" +
                         std::to_string(chinese ? 1 : 0));
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_session_owner[id] = client;
                SessionUi ui;
                ui.app = app;
                ui.chinese = chinese;
                ui.page_size = page_size ? page_size : settings.page_size;
                g_ui[id] = std::move(ui);
            }
            Message reply = Reply(MsgKind::OpenSessionReply, request);
            reply.session = id;
            reply.flags = id ? 1 : 0;
            ByteWriter mode;
            mode.U32(chinese ? 1 : 0);
            reply.payload = std::move(mode.data());
            SendTo(client, reply);
            return;
        }
        case MsgKind::CloseSession: {
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                g_session_owner.erase(request.session);
                g_ui.erase(request.session);
            }
            CoreEngine::Instance().CloseSession(request.session);
            PostUi([session = request.session] {
                if (g_shown == session) {
                    g_window.Hide();
                    g_shown = 0;
                }
            });
            return;
        }
        case MsgKind::Key: {
            KeyEvent event;
            if (!reader.U32(&event.keycode) || !reader.U32(&event.modifiers) ||
                !reader.U32(&event.action) || !reader.Str(&event.text)) {
                return;
            }
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                const auto it = g_ui.find(request.session);
                if (it != g_ui.end()) {
                    it->second.page = PageAfter(it->second.page, event.keycode);
                }
            }
            const KeyResult result = CoreEngine::Instance().Key(request.session, event);
            const std::vector<UiCommand> host = ApplyCommands(request.session, result.commands);
            Message reply = Reply(MsgKind::KeyReply, request);
            reply.flags = result.handled ? 1 : 0;
            ByteWriter writer;
            EncodeCommands(writer, host);
            reply.payload = std::move(writer.data());
            SendTo(client, reply);
            PostUi([session = request.session] { Render(session); });
            return;
        }
        case MsgKind::Select: {
            uint32_t index = 0;
            if (!reader.U32(&index)) return;
            const KeyResult result = CoreEngine::Instance().Select(request.session, index);
            const std::vector<UiCommand> host = ApplyCommands(request.session, result.commands);
            Message reply = Reply(MsgKind::KeyReply, request);
            reply.flags = result.handled ? 1 : 0;
            ByteWriter writer;
            EncodeCommands(writer, host);
            reply.payload = std::move(writer.data());
            SendTo(client, reply);
            PostUi([session = request.session] { Render(session); });
            return;
        }
        case MsgKind::CaretRect: {
            RECT caret{};
            if (!ReadCaret(reader, &caret)) return;
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                const auto it = g_ui.find(request.session);
                if (it == g_ui.end()) return;
                it->second.caret = caret;
            }
            PostUi([session = request.session] { Render(session); });
            return;
        }
        case MsgKind::HideCandidates: {
            HideStrip(request.session);
            return;
        }
        case MsgKind::Focus: {
            CoreEngine::Instance().Focus(request.session, request.flags != 0);
            return;
        }
        case MsgKind::Reset: {
            CoreEngine::Instance().Reset(request.session);
            return;
        }
        case MsgKind::UpdateContext: {
            std::string app;
            uint32_t field_type = 0, host_caps = 0;
            if (!reader.Str(&app) || !reader.U32(&field_type) || !reader.U32(&host_caps)) {
                return;
            }
            CoreEngine::Instance().UpdateContext(request.session, ToWide(app), field_type,
                                                 host_caps);
            return;
        }
        case MsgKind::ReloadSettings: {
            // An empty chain means "just re-read the settings"; a named chain
            // also switches. Either way the reply carries fresh settings,
            // which is the only way a packaged host can see them.
            std::string chain;
            uint32_t page_size = 0;
            if (reader.Str(&chain) && reader.U32(&page_size) && !chain.empty() &&
                chain != ToUtf8(CoreEngine::Instance().active_chain())) {
                CoreEngine::Instance().SwitchChain(chain);
            }
            SendTo(client, HelloReply(request));
            return;
        }
        case MsgKind::SaveAppMode: {
            // The shell toggled 中/英 (Shift, language bar, Ctrl+Space): persist
            // it and flash the badge at the caret it sent along.
            std::string app;
            RECT caret{};
            if (!reader.Str(&app) || !ReadCaret(reader, &caret)) return;
            const bool chinese = request.flags != 0;
            SaveAppMode(ToWide(app), chinese);
            {
                std::lock_guard<std::mutex> lock(g_state_mutex);
                const auto it = g_ui.find(request.session);
                if (it == g_ui.end()) return;
                it->second.chinese = chinese;
                it->second.caret = caret;
                it->second.items.clear();
                it->second.candidates_up = false;
                it->second.page = 0;
            }
            PostUi([session = request.session] { Render(session, true); });
            return;
        }
        case MsgKind::EngineVersion:
            SendTo(client, HelloReply(request));
            return;
        case MsgKind::RestartRequest:
            // The installer staged a new build and wants this process gone so
            // the files can be replaced. Reply first, then quit on the UI
            // thread, which is where the message pump and window live.
            WriteLog(LogLevel::Info, "engine", "restart requested");
            SendTo(client, Reply(MsgKind::EngineVersion, request));
            PostMessageW(g_wake, kEngineQuitMessage, 0, 0);
            return;
        default:
            return;
    }
}

void ClientLoop(Client* client) {
    WriteLog(LogLevel::Info, "ipc", "client attached");
    Message message;
    while (ReadMessage(client->pipe, &message)) {
        HandleMessage(client, message);
    }
    std::vector<SessionId> gone;
    {
        std::lock_guard<std::mutex> lock(g_state_mutex);
        for (auto it = g_session_owner.begin(); it != g_session_owner.end();) {
            if (it->second == client) {
                g_ui.erase(it->first);
                gone.push_back(it->first);
                it = g_session_owner.erase(it);
            } else {
                ++it;
            }
        }
        g_clients.erase(client);
    }
    PostUi([gone] {
        for (SessionId session : gone) {
            if (g_shown != session) continue;
            g_window.Hide();
            g_shown = 0;
        }
    });
    CloseHandle(client->pipe);
    delete client;
}

DWORD WINAPI ServeThread(void*) {
    const std::wstring name = EnginePipeName();
    for (;;) {
        HANDLE pipe = CreateNamedPipeW(
            name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, PIPE_UNLIMITED_INSTANCES,
            1 << 20, 1 << 20, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            WriteLog(LogLevel::Error, "ipc", "CreateNamedPipe failed");
            return 0;
        }
        OVERLAPPED connect{};
        connect.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        BOOL connected = ConnectNamedPipe(pipe, &connect);
        if (!connected) {
            const DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING) {
                connected = WaitForSingleObject(connect.hEvent, INFINITE) == WAIT_OBJECT_0;
            } else {
                connected = err == ERROR_PIPE_CONNECTED;
            }
        }
        CloseHandle(connect.hEvent);
        if (!connected) {
            CloseHandle(pipe);
            continue;
        }
        auto* client = new Client();
        client->pipe = pipe;
        {
            std::lock_guard<std::mutex> lock(g_state_mutex);
            g_clients.insert(client);
        }
        client->reader = std::thread(ClientLoop, client);
        client->reader.detach();
    }
}

bool AlreadyRunning() {
    std::wstring name = EnginePipeName();
    const size_t slash = name.rfind(L'\\');
    if (slash != std::wstring::npos) name = name.substr(slash + 1);
    name = L"Local\\" + name + L"-singleton";
    HANDLE mutex = CreateMutexW(nullptr, TRUE, name.c_str());
    if (!mutex) return true;
    if (GetLastError() == ERROR_ALREADY_EXISTS) return true;
    return false;  // mutex stays alive for the process lifetime
}

void RunServer() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    InitLog(DataDirectory());

    if (AlreadyRunning()) {
        WriteLog(LogLevel::Info, "engine", "already running");
        ExitProcess(0);
    }

    WriteLog(LogLevel::Info, "engine",
             std::string("build ") + __DATE__ + " " + __TIME__ + " exe=" +
                 ToUtf8(ModulePath(nullptr)));

    g_settings = LoadSettings();
    EngineConfig config;
    config.resources_dir = ModuleDirectory(nullptr);
    config.data_dir = DataDirectory();
    config.chain = ToUtf8(g_settings.chain);
    config.page_size = g_settings.page_size;
    if (!CoreEngine::Instance().Start(config)) {
        WriteLog(LogLevel::Error, "engine", "pime-core failed to start");
        ExitProcess(1);
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WakeProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PimeEngineWake";
    RegisterClassExW(&wc);
    g_wake = CreateWindowExW(0, L"PimeEngineWake", L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                             nullptr, wc.hInstance, nullptr);

    // The strip lives here now, not in the host: replacing this process is
    // what makes the candidate window hot-updatable.
    g_window.SetDark(DarkFor(g_settings.theme));
    WriteLog(LogLevel::Info, "engine",
             g_window.Create(&g_ui_delegate) ? "candidate window created"
                                             : "candidate window create failed");

    CloseHandle(CreateThread(nullptr, 0, ServeThread, nullptr, 0, nullptr));
    WriteLog(LogLevel::Info, "engine", "serving " + ToUtf8(EnginePipeName()));

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

}  // namespace
}  // namespace pimewin

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool serve = false;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--serve") == 0) serve = true;
    }
    if (argv) LocalFree(argv);
    if (!serve) return 0;
    pimewin::RunServer();
    return 0;
}
