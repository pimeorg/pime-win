// Wire protocol between the TSF shell (pime_tsf.dll, in-proc) and the engine
// process (pime_engine.exe). Header-only: both sides compile the same codec.
//
// Frame: [u32 total_len][Header 32B][payload]
// total_len counts the header and the payload.
#pragma once

#include <windows.h>
#include <sddl.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "common/settings.h"
#include "common/win_util.h"
#include "engine/core_engine.h"

namespace pimewin {

constexpr uint32_t kIpcMagic = 0x50494D45;  // 'PIME'
constexpr uint32_t kIpcVersion = 1;
constexpr size_t kIpcHeaderSize = 32;

enum class MsgKind : uint32_t {
    Hello = 1,
    HelloReply = 2,
    OpenSession = 3,
    OpenSessionReply = 4,
    CloseSession = 5,
    Key = 6,
    KeyReply = 7,
    Select = 8,
    Focus = 9,
    Reset = 10,
    UpdateContext = 11,
    CaretRect = 12,
    ReloadSettings = 13,
    EngineVersion = 14,
    RestartRequest = 15,
    UiCommands = 16,  // engine -> shell push
    ModeChanged = 17,  // engine -> shell push
    SaveAppMode = 18,  // shell -> engine: persist per-app 中/英
    HideCandidates = 19,  // shell -> engine: the host went away, hide the strip
    UserDict = 20,        // shell -> engine: read/write the user dictionary
    UserDictReply = 21,
};

enum class UserDictOp : uint32_t {
    List = 0,      // {offset, limit}
    Delete = 1,    // {code, word}
    Clear = 2,     // {}
    BatchSet = 3,  // {words}
};

struct Message {
    MsgKind kind = MsgKind::Hello;
    uint32_t seq = 0;
    uint64_t session = 0;
    uint32_t flags = 0;
    std::string payload;
};

class ByteWriter {
public:
    void U32(uint32_t v) { buf_.append(reinterpret_cast<const char*>(&v), 4); }
    void I32(int32_t v) { buf_.append(reinterpret_cast<const char*>(&v), 4); }
    void U64(uint64_t v) { buf_.append(reinterpret_cast<const char*>(&v), 8); }
    void Bytes(const void* p, size_t n) { buf_.append(static_cast<const char*>(p), n); }
    void Str(const std::string& s) {
        U32(static_cast<uint32_t>(s.size()));
        buf_.append(s);
    }
    std::string& data() { return buf_; }

private:
    std::string buf_;
};

class ByteReader {
public:
    ByteReader(const char* p, size_t n) : p_(p), n_(n) {}
    bool U32(uint32_t* v) { return Bytes(v, 4); }
    bool I32(int32_t* v) { return Bytes(v, 4); }
    bool U64(uint64_t* v) { return Bytes(v, 8); }
    bool Bytes(void* out, size_t n) {
        if (!ok_ || off_ + n > n_) return ok_ = false;
        std::memcpy(out, p_ + off_, n);
        off_ += n;
        return true;
    }
    bool Str(std::string* s) {
        uint32_t n = 0;
        if (!U32(&n) || off_ + n > n_) return ok_ = false;
        s->assign(p_ + off_, n);
        off_ += n;
        return true;
    }
    bool ok() const { return ok_; }

private:
    const char* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

// The host's caret in physical screen pixels: the shell converts out of its
// own DPI awareness before sending, so the engine never has to scale.
inline void WriteCaret(ByteWriter& w, const RECT& caret) {
    w.I32(caret.left);
    w.I32(caret.top);
    w.I32(caret.right);
    w.I32(caret.bottom);
}

inline bool ReadCaret(ByteReader& r, RECT* caret) {
    int32_t values[4] = {};
    for (int32_t& value : values) {
        if (!r.I32(&value)) return false;
    }
    *caret = RECT{values[0], values[1], values[2], values[3]};
    return true;
}

inline void EncodeCommands(ByteWriter& w, const std::vector<UiCommand>& commands) {
    w.U32(static_cast<uint32_t>(commands.size()));
    for (const UiCommand& command : commands) {
        w.U32(static_cast<uint32_t>(command.kind));
        w.Str(command.text);
        w.U32(command.cursor);
        w.I32(command.code);
        w.U32(static_cast<uint32_t>(command.candidates.size()));
        for (const UiCandidate& candidate : command.candidates) {
            w.Str(candidate.text);
            w.Str(candidate.comment);
            w.Str(candidate.label);
        }
    }
}

inline bool DecodeCommands(ByteReader& r, std::vector<UiCommand>* out) {
    uint32_t count = 0;
    if (!r.U32(&count)) return false;
    out->clear();
    out->reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        UiCommand command;
        uint32_t kind = 0;
        if (!r.U32(&kind) || !r.Str(&command.text) || !r.U32(&command.cursor) ||
            !r.I32(&command.code)) {
            return false;
        }
        command.kind = static_cast<UiKind>(kind);
        uint32_t n = 0;
        if (!r.U32(&n)) return false;
        command.candidates.resize(n);
        for (UiCandidate& candidate : command.candidates) {
            if (!r.Str(&candidate.text) || !r.Str(&candidate.comment) ||
                !r.Str(&candidate.label)) {
                return false;
            }
        }
        out->push_back(std::move(command));
    }
    return true;
}

inline void EncodeUserWord(ByteWriter& w, const UserWord& word) {
    w.Str(word.code);
    w.Str(word.word);
    w.U32(word.weight);
}

inline bool DecodeUserWord(ByteReader& r, UserWord* word) {
    return r.Str(&word->code) && r.Str(&word->word) && r.U32(&word->weight);
}

inline void EncodeUserWords(ByteWriter& w, const std::vector<UserWord>& words) {
    w.U32(static_cast<uint32_t>(words.size()));
    for (const UserWord& word : words) EncodeUserWord(w, word);
}

inline bool DecodeUserWords(ByteReader& r, std::vector<UserWord>* out) {
    uint32_t count = 0;
    if (!r.U32(&count)) return false;
    out->clear();
    for (uint32_t i = 0; i < count; ++i) {
        UserWord word;
        if (!DecodeUserWord(r, &word)) return false;
        out->push_back(std::move(word));
    }
    return true;
}

// List reply: [i32 status][u32 total][words]. Every other op: [i32 status].
inline void EncodeUserDictReply(ByteWriter& w, const UserDictReply& reply) {
    w.I32(reply.status);
    w.U32(reply.total);
    EncodeUserWords(w, reply.words);
}

inline bool DecodeUserDictReply(ByteReader& r, UserDictReply* reply) {
    return r.I32(&reply->status) && r.U32(&reply->total) &&
           DecodeUserWords(r, &reply->words);
}

// Settings travel in HelloReply and ReloadSettings replies. The engine reads
// them from HKCU on the outside; a packaged host's own read is virtualized.
inline void EncodeSettings(ByteWriter& w, const Settings& s) {
    w.Str(ToUtf8(s.chain));
    w.U32(s.theme);
    w.U32(s.page_size);
    w.U32(s.chinese);
    w.U32(s.punctuation);
    w.U32(s.candidate_vertical);
    w.U32(s.candidate_font);
}

inline bool DecodeSettings(ByteReader& r, Settings* s) {
    std::string chain;
    if (!r.Str(&chain) || !r.U32(&s->theme) || !r.U32(&s->page_size) ||
        !r.U32(&s->chinese) || !r.U32(&s->punctuation) ||
        !r.U32(&s->candidate_vertical) || !r.U32(&s->candidate_font)) {
        return false;
    }
    s->chain = ToWide(chain);
    return true;
}

inline std::string EncodeMessage(const Message& m) {
    std::string out;
    uint32_t total = static_cast<uint32_t>(kIpcHeaderSize + m.payload.size());
    out.append(reinterpret_cast<const char*>(&total), 4);
    uint32_t magic = kIpcMagic;
    uint32_t version = kIpcVersion;
    uint32_t kind = static_cast<uint32_t>(m.kind);
    uint32_t payload_len = static_cast<uint32_t>(m.payload.size());
    out.append(reinterpret_cast<const char*>(&magic), 4);
    out.append(reinterpret_cast<const char*>(&version), 4);
    out.append(reinterpret_cast<const char*>(&kind), 4);
    out.append(reinterpret_cast<const char*>(&m.seq), 4);
    out.append(reinterpret_cast<const char*>(&m.session), 8);
    out.append(reinterpret_cast<const char*>(&m.flags), 4);
    out.append(reinterpret_cast<const char*>(&payload_len), 4);
    out.append(m.payload);
    return out;
}

inline bool DecodeMessage(const std::string& frame, Message* out) {
    if (frame.size() < 4 + kIpcHeaderSize) return false;
    const char* p = frame.data();
    uint32_t total = 0;
    std::memcpy(&total, p, 4);
    if (total != frame.size() - 4) return false;
    ByteReader r(p + 4, total);
    uint32_t magic = 0, version = 0, kind = 0, payload_len = 0;
    if (!r.U32(&magic) || !r.U32(&version) || !r.U32(&kind) || !r.U32(&out->seq) ||
        !r.U64(&out->session) || !r.U32(&out->flags) || !r.U32(&payload_len)) {
        return false;
    }
    if (magic != kIpcMagic || version != kIpcVersion) return false;
    out->kind = static_cast<MsgKind>(kind);
    out->payload.assign(p + 4 + kIpcHeaderSize, payload_len);
    return true;
}

// Both ends open the pipe with FILE_FLAG_OVERLAPPED: a reader thread blocks in
// ReadFile while other threads write, and a synchronous handle does not let
// those two overlap reliably.
inline bool PipeIO(HANDLE pipe, void* buffer, DWORD size, bool write, DWORD* done) {
    OVERLAPPED overlapped{};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) return false;
    const BOOL started = write
                             ? WriteFile(pipe, buffer, size, nullptr, &overlapped)
                             : ReadFile(pipe, buffer, size, nullptr, &overlapped);
    bool ok = started != FALSE;
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        ok = WaitForSingleObject(overlapped.hEvent, INFINITE) == WAIT_OBJECT_0;
    }
    DWORD transferred = 0;
    if (ok) ok = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
    CloseHandle(overlapped.hEvent);
    if (done) *done = transferred;
    return ok;
}

inline bool WriteMessage(HANDLE pipe, const Message& m) {
    const std::string frame = EncodeMessage(m);
    DWORD written = 0;
    return PipeIO(pipe, const_cast<char*>(frame.data()), static_cast<DWORD>(frame.size()),
                  true, &written) &&
           written == frame.size();
}

// Blocking read of one whole frame. Returns false on EOF or a malformed frame.
inline bool ReadMessage(HANDLE pipe, Message* out) {
    uint32_t total = 0;
    DWORD got = 0;
    if (!PipeIO(pipe, &total, 4, false, &got) || got != 4) return false;
    if (total < kIpcHeaderSize || total > (1u << 24)) return false;
    std::string frame(4 + total, '\0');
    std::memcpy(frame.data(), &total, 4);
    DWORD off = 0;
    while (off < total) {
        DWORD n = 0;
        if (!PipeIO(pipe, frame.data() + 4 + off, total - off, false, &n) || n == 0) {
            return false;
        }
        off += n;
    }
    return DecodeMessage(frame, out);
}

// \\.\pipe\pime-{SID}-engine. The SID keeps users (and sessions) apart without
// needing an explicit DACL: a pipe's default DACL already admits the owner.
inline std::wstring EnginePipeName() {
    std::wstring name = L"\\\\.\\pipe\\pime-unknown-engine";
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return name;
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::string buffer(size, '\0');
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
        auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPWSTR sid = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid)) {
            name = L"\\\\.\\pipe\\pime-";
            name += sid;
            name += L"-engine";
            LocalFree(sid);
        }
    }
    CloseHandle(token);
    return name;
}

}  // namespace pimewin
