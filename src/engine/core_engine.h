// Host-side owner of pime-core.
//
// A TSF text service is created per UI thread, while pime-core is a
// process-wide singleton bound to the thread that created it. CoreEngine
// therefore owns one dedicated thread, creates core there and marshals every
// call to it. Sink commands are captured by the call that caused them, so a
// key handled on thread A never ends up in the candidate window of thread B.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "common/user_dict_format.h"

namespace pimewin {

// Posted to the session's notify window when asynchronous output is pending.
constexpr UINT kEngineWakeMessage = WM_APP + 0x51;

struct KeyEvent {
    uint32_t keycode = 0;
    uint32_t modifiers = 0;
    uint32_t action = 0;  // PIME_KEY_DOWN / PIME_KEY_UP / PIME_KEY_REPEAT
    std::string text;     // UTF-8, empty for non-printing keys
};

struct UiCandidate {
    std::string text;
    std::string comment;
    std::string label;
};

enum class UiKind {
    Composition,
    Commit,
    Candidates,
    HideCandidates,
    Beep,
    Notify,
};

struct UiCommand {
    UiKind kind = UiKind::HideCandidates;
    std::string text;
    uint32_t cursor = 0;
    std::vector<UiCandidate> candidates;
    int code = 0;
};

struct KeyResult {
    bool handled = false;
    std::vector<UiCommand> commands;
};

struct EngineConfig {
    std::wstring resources_dir;  // contains plugins/ and chains/
    std::wstring data_dir;       // writable per-user directory
    std::string chain = "default.cbor";
    uint32_t page_size = 6;
    uint32_t candidate_budget = 96;
};

// Result of a user-dictionary operation. `status` is a pime_status value;
// `total` is the entry count for List.
struct UserDictReply {
    int status = 0;
    uint32_t total = 0;
    std::vector<UserWord> words;
};

using SessionId = uint64_t;

class CoreEngine {
public:
    static CoreEngine& Instance();

    CoreEngine(const CoreEngine&) = delete;
    CoreEngine& operator=(const CoreEngine&) = delete;

    // Starts the worker and loads plugins + chain. Idempotent. May block while
    // the WASM modules load; call it off the critical UI path when possible.
    bool Start(const EngineConfig& config);
    bool ready() const;
    std::wstring version() const;
    std::vector<std::wstring> chains() const;
    std::wstring active_chain() const;

    SessionId OpenSession(HWND notify, const std::wstring& app,
                          uint32_t field_type, uint32_t host_caps,
                          uint32_t page_size = 0);
    void CloseSession(SessionId session);
    KeyResult Key(SessionId session, const KeyEvent& event);
    KeyResult Select(SessionId session, uint32_t index);
    void Focus(SessionId session, bool focused);
    void Reset(SessionId session);
    void UpdateContext(SessionId session, const std::wstring& app,
                       uint32_t field_type, uint32_t host_caps);
    bool TakeAsync(SessionId session, std::vector<UiCommand>* out);

    // Rebuilds and activates a generation from another chain file.
    bool SwitchChain(const std::string& chain);

    // Writes every dirty plugin store namespace now (learning otherwise
    // reaches disk a few times a second at most).
    void FlushStore();

    // User dictionary, backed by rank's `usr/` namespace. All of these block
    // on the worker thread, so call them from the IPC thread only.
    void UserDictList(uint32_t offset, uint32_t limit, UserDictReply* out);
    void UserDictSet(const std::vector<UserWord>& words, UserDictReply* out);
    void UserDictDelete(const std::string& code, const std::string& word,
                        UserDictReply* out);
    void UserDictClear(UserDictReply* out);

    struct Impl;

private:
    CoreEngine() = default;
    ~CoreEngine();

    Impl* impl_ = nullptr;
};

}  // namespace pimewin
