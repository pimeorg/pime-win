// Shell-side connection to pime_engine.exe.
//
// One instance per process: every TSF text service on every thread shares it.
// Session ids handed to callers are local and stable; the mapping to the
// engine's ids is re-established whenever the engine restarts, so a host
// never notices the engine was replaced.
#pragma once

#include <windows.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common/ipc.h"
#include "common/settings.h"

namespace pimewin {

class IpcClient {
public:
    static IpcClient& Instance();

    IpcClient(const IpcClient&) = delete;
    IpcClient& operator=(const IpcClient&) = delete;

    // Connects, spawning the engine when it is not running. Blocks until the
    // handshake completes or eight seconds pass.
    bool Start(const std::wstring& engine_exe);
    bool ready() const { return ready_.load(); }
    std::wstring version() const;
    // Settings as the engine sees them. HKCU is virtualized inside packaged
    // hosts (Notepad, Settings, ...), so the shell must not read the registry
    // itself.
    Settings settings() const;

    // Returns the engine's session id, or 0. `chinese` receives the starting
    // mode the engine resolved for this app (per-app memory, else global).
    SessionId OpenSession(HWND notify, const std::wstring& app,
                          uint32_t field_type, uint32_t host_caps,
                          uint32_t page_size, bool* chinese);
    void CloseSession(SessionId session);
    KeyResult Key(SessionId session, const KeyEvent& event);
    KeyResult Select(SessionId session, uint32_t index);
    void Focus(SessionId session, bool focused);
    void Reset(SessionId session);
    void UpdateContext(SessionId session, const std::wstring& app,
                       uint32_t field_type, uint32_t host_caps);
    bool TakeAsync(SessionId session, std::vector<UiCommand>* out);
    // A mode the engine switched on its own (chip click); cleared on read.
    bool TakeModeChange(SessionId session, bool* chinese);
    // Where the composition ends, in the host's physical pixels (the caller
    // de-virtualizes, so the engine never scales). Places the strip.
    void SendCaret(SessionId session, const RECT& caret);
    // The host went away (minimize, focus loss, commit): drop the strip.
    void HideCandidates(SessionId session);
    // Persists the per-app mode in the engine: inside a packaged host the
    // shell's own HKCU writes land in a private, virtualized copy. The caret
    // travels along so the engine can flash the mode badge in place.
    void SaveAppMode(SessionId session, const std::wstring& app, bool chinese,
                     const RECT& caret);
    // Re-reads settings from the engine and caches them; use after the
    // PIME.ConfigChanged broadcast. Returns false when the engine is gone.
    bool RefreshSettings();
    bool SwitchChain(const std::string& chain);

private:
    IpcClient() = default;
    ~IpcClient();

    struct Tracked {
        uint64_t remote = 0;
        HWND notify = nullptr;
        std::string app;
        uint32_t field_type = 0;
        uint32_t host_caps = 0;
        uint32_t page_size = 0;
    };

    void Supervise();
    bool ConnectOnce();
    void AdoptHello(const Message& reply);
    void ReadLoop();
    void DispatchPush(const Message& message);
    bool ReopenSessions();
    bool Request(MsgKind kind, SessionId session, uint32_t flags,
                 const std::string& payload, Message* reply);
    void Send(MsgKind kind, SessionId session, uint32_t flags,
              const std::string& payload);
    uint64_t Remote(SessionId session);

    std::mutex lifecycle_mutex_;
    std::condition_variable ready_cv_;
    std::thread supervisor_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> ready_{false};
    std::wstring engine_exe_;

    std::mutex write_mutex_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;

    std::mutex request_mutex_;
    uint32_t seq_ = 0;
    std::mutex reply_mutex_;
    std::condition_variable reply_cv_;
    uint32_t waiting_seq_ = 0;
    bool reply_ready_ = false;
    Message reply_message_;

    std::mutex sessions_mutex_;
    std::map<SessionId, Tracked> sessions_;
    std::map<uint64_t, SessionId> remote_to_local_;
    std::map<SessionId, std::vector<UiCommand>> pending_;
    std::map<SessionId, bool> mode_pending_;
    SessionId next_session_ = 1;
    std::wstring version_;
    mutable std::mutex settings_mutex_;
    Settings engine_settings_;
};

}  // namespace pimewin
