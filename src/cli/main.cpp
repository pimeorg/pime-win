// Headless probe for the reused kernel + plugin packages.
//
//   pime --keys nihao            type nihao, show the chain output
//   pime --keys nihao --commit   ... then press space to commit
//   pime --dict                  list the user dictionary (code, word, weight)
//
// It runs the same host layer as the text service (CoreEngine), so a failure
// here is a kernel/plugin/chain problem, not a TSF one.
#include <windows.h>

#include <pime/hal.h>

#include <cstdio>
#include <string>
#include <vector>

#include "common/log.h"
#include "common/win_util.h"
#include "engine/core_engine.h"

namespace {

using pimewin::ToUtf8;
using pimewin::ToWide;

void Print(const std::wstring& text) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (!WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()),
                       &written, nullptr)) {
        const std::string utf8 = ToUtf8(text);
        fwrite(utf8.c_str(), 1, utf8.size(), stdout);
    }
}

std::wstring Describe(const pimewin::UiCommand& command) {
    switch (command.kind) {
        case pimewin::UiKind::Composition:
            return L"[composition] " + ToWide(command.text) + L"\n";
        case pimewin::UiKind::Commit:
            return L"[commit] " + ToWide(command.text) + L"\n";
        case pimewin::UiKind::Candidates: {
            std::wstring line = L"[candidates]";
            for (const auto& item : command.candidates) {
                line += L" ";
                if (!item.label.empty()) line += ToWide(item.label) + L".";
                line += ToWide(item.text);
                if (!item.comment.empty()) {
                    line += L"(" + ToWide(item.comment) + L")";
                }
            }
            line += L"\n";
            return line;
        }
        case pimewin::UiKind::HideCandidates:
            return L"[hide candidates]\n";
        case pimewin::UiKind::Beep:
            return L"[beep]\n";
        case pimewin::UiKind::Notify:
            return L"[notify] " + std::to_wstring(command.code) + L"\n";
    }
    return {};
}

std::vector<std::string> SplitScalars(const std::string& text) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < text.size()) {
        unsigned char lead = static_cast<unsigned char>(text[i]);
        size_t length = 1;
        if ((lead & 0xE0) == 0xC0) length = 2;
        else if ((lead & 0xF0) == 0xE0) length = 3;
        else if ((lead & 0xF8) == 0xF0) length = 4;
        if (i + length > text.size()) length = 1;
        out.push_back(text.substr(i, length));
        i += length;
    }
    return out;
}

uint32_t ScalarValue(const std::string& utf8) {
    const auto* p = reinterpret_cast<const unsigned char*>(utf8.data());
    if (utf8.size() == 1) return p[0];
    if (utf8.size() == 2) return ((p[0] & 0x1Fu) << 6) | (p[1] & 0x3Fu);
    if (utf8.size() == 3) {
        return ((p[0] & 0x0Fu) << 12) | ((p[1] & 0x3Fu) << 6) | (p[2] & 0x3Fu);
    }
    return ((p[0] & 0x07u) << 18) | ((p[1] & 0x3Fu) << 12) |
           ((p[2] & 0x3Fu) << 6) | (p[3] & 0x3Fu);
}

struct Options {
    std::wstring resources;
    std::wstring data;
    std::string chain = "pinyin.cbor";
    std::string keys;
    bool commit = false;
    bool dict = false;
};

Options Parse(int argc, wchar_t** argv) {
    Options options;
    options.resources = pimewin::ModuleDirectory(nullptr);
    options.data = pimewin::DataDirectory();
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto next = [&]() -> std::wstring {
            return i + 1 < argc ? argv[++i] : std::wstring();
        };
        if (arg == L"--res") options.resources = next();
        else if (arg == L"--data") options.data = next();
        else if (arg == L"--chain") options.chain = pimewin::ToUtf8(next());
        else if (arg == L"--keys") options.keys = pimewin::ToUtf8(next());
        else if (arg == L"--commit") options.commit = true;
        else if (arg == L"--dict") options.dict = true;
        else if (arg == L"--help" || arg == L"-h") {
            Print(L"pime --keys TEXT [--commit] [--res DIR] [--data DIR] "
                  L"[--chain FILE]\n"
                  L"pime --dict [--res DIR] [--data DIR]\n");
            ExitProcess(0);
        }
    }
    if (options.keys.empty()) options.keys = "nihao";
    return options;
}

int Run(const Options& options) {
    using namespace pimewin;
    CoreEngine& engine = CoreEngine::Instance();
    EngineConfig config;
    config.resources_dir = options.resources;
    config.data_dir = options.data;
    config.chain = options.chain;

    const ULONGLONG started = GetTickCount64();
    if (!engine.Start(config)) {
        Print(L"engine start failed; see " + options.data + L"\\pime-*.log\n");
        return 1;
    }
    Print(L"core " + engine.version() + L" started in " +
          std::to_wstring(GetTickCount64() - started) + L" ms\n");

    if (options.dict) {
        UserDictReply reply;
        engine.UserDictList(0, 0, &reply);
        if (reply.status != 0) {
            Print(L"dict failed: " + std::to_wstring(reply.status) + L"\n");
            return 1;
        }
        Print(L"dict " + std::to_wstring(reply.total) + L" entries\n");
        for (const UserWord& word : reply.words) {
            Print(L"  " + ToWide(word.code) + L"\t" + ToWide(word.word) +
                  L"\t" + std::to_wstring(word.weight) + L"\n");
        }
        return 0;
    }

    const SessionId session = engine.OpenSession(nullptr, L"pime.exe",
                                                 PIME_FIELD_TEXT, 0);
    if (!session) {
        Print(L"session create failed\n");
        return 1;
    }
    engine.Focus(session, true);

    bool saw_output = false;
    for (const std::string& scalar : SplitScalars(options.keys)) {
        KeyEvent event;
        event.keycode = ScalarValue(scalar);
        event.text = scalar;
        event.action = PIME_KEY_DOWN;
        KeyResult result = engine.Key(session, event);
        if (!result.handled) {
            Print(L"[pass] " + ToWide(scalar) + L"\n");
        }
        for (const auto& command : result.commands) {
            if (command.kind == UiKind::Commit) saw_output = true;
            Print(Describe(command));
        }
    }

    if (options.commit) {
        KeyEvent space;
        space.keycode = VK_SPACE;
        space.text = " ";
        space.action = PIME_KEY_DOWN;
        KeyResult result = engine.Key(session, space);
        for (const auto& command : result.commands) {
            if (command.kind == UiKind::Commit) saw_output = true;
            Print(Describe(command));
        }
        if (!result.handled) Print(L"[pass] space\n");
    }

    engine.Focus(session, false);
    engine.CloseSession(session);
    engine.FlushStore();
    return saw_output ? 0 : 2;
}

}  // namespace

namespace pimewin {

int RunCli(int argc, wchar_t** argv) {
    return Run(Parse(argc, argv));
}

}  // namespace pimewin
