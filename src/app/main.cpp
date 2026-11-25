#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app/settings_window.h"
#include "cli/cli_app.h"
#include "common/ipc.h"
#include "common/win_util.h"
#include "demo/demo_app.h"
#include "tsf/register.h"

namespace {

bool WantsFlag(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i < argc; ++i) {
        if (lstrcmpiW(argv[i], flag) == 0) return true;
    }
    return false;
}

// Value after `--update`: the staged directory to copy into this one.
std::wstring FlagValue(int argc, wchar_t** argv, const wchar_t* flag) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (lstrcmpiW(argv[i], flag) == 0) return argv[i + 1];
    }
    return {};
}

// Asks a running engine to exit; false when none is running. The engine
// answers and quits on its UI thread, so its files become replaceable.
bool StopEngine() {
    using namespace pimewin;
    const std::wstring name = EnginePipeName();
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
    Message reply;
    bool sent = WriteMessage(pipe, hello) && ReadMessage(pipe, &reply) &&
                reply.kind == MsgKind::HelloReply;
    if (sent) {
        Message request;
        request.kind = MsgKind::RestartRequest;
        request.seq = 1;
        sent = WriteMessage(pipe, request);
    }
    CloseHandle(pipe);
    if (!sent) return false;
    // Wait for the process to go away so the exe can be overwritten.
    for (int i = 0; i < 50; ++i) {
        if (!WaitNamedPipeW(name.c_str(), 0)) return true;
        Sleep(100);
    }
    return true;
}

// Copies `from` over `to`, recursing into subdirectories. A loaded DLL cannot
// be overwritten but can be renamed, so a busy file is moved aside first.
bool CopyTree(const std::wstring& from, const std::wstring& to) {
    using pimewin::EnsureDirectory;
    using pimewin::PathJoin;
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(PathJoin(from, L"*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    do {
        const std::wstring name = entry.cFileName;
        if (name == L"." || name == L"..") continue;
        // Staged copies of dist carry the previous run's renamed-aside files.
        if (name.size() > 4 && name.compare(name.size() - 4, 4, L".old") == 0) continue;
        const std::wstring source = PathJoin(from, name);
        const std::wstring target = PathJoin(to, name);
        if (entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            ok = EnsureDirectory(target) && CopyTree(source, target) && ok;
            continue;
        }
        // Skip files that already match: the installer cannot replace its own
        // running image, so re-copying an identical build would only leave a
        // renamed-aside copy behind.
        WIN32_FILE_ATTRIBUTE_DATA from_data{};
        WIN32_FILE_ATTRIBUTE_DATA to_data{};
        if (GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &from_data) &&
            GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &to_data) &&
            from_data.nFileSizeLow == to_data.nFileSizeLow &&
            from_data.nFileSizeHigh == to_data.nFileSizeHigh &&
            from_data.ftLastWriteTime.dwLowDateTime == to_data.ftLastWriteTime.dwLowDateTime &&
            from_data.ftLastWriteTime.dwHighDateTime == to_data.ftLastWriteTime.dwHighDateTime) {
            continue;
        }
        if (CopyFileW(source.c_str(), target.c_str(), FALSE)) continue;
        const std::wstring aside = target + L".old";
        DeleteFileW(aside.c_str());
        ok = MoveFileExW(target.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING) &&
             CopyFileW(source.c_str(), target.c_str(), FALSE) && ok;
    } while (FindNextFileW(find, &entry));
    FindClose(find);
    return ok;
}

// Removes the `.old` files a previous update left behind, now that no process
// holds them.
void DropStaleFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW(pimewin::PathJoin(dir, L"*.old").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW(pimewin::PathJoin(dir, entry.cFileName).c_str());
    } while (FindNextFileW(find, &entry));
    FindClose(find);
}

void ShowResult(HWND owner, const std::wstring& text, bool success) {
    MessageBoxW(owner, text.c_str(), L"PIME",
                MB_OK | (success ? MB_ICONINFORMATION : MB_ICONERROR));
}

// input.dll ignores a profile that the calling process registered itself, so
// the input-list update runs in a second, short-lived copy of this process.
bool SyncInputListInFreshProcess() {
    const std::wstring exe = pimewin::ModulePath(nullptr);
    std::wstring command = L"\"" + exe + L"\" --sync-input-list";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                        &process)) {
        return false;
    }
    WaitForSingleObject(process.hProcess, 5000);
    DWORD exit_code = 1;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    const bool install = argv && WantsFlag(argc, argv, L"--install");
    const bool uninstall = argv && WantsFlag(argc, argv, L"--uninstall");
    const std::wstring update = argv ? FlagValue(argc, argv, L"--update") : std::wstring();
    const bool quiet = argv && WantsFlag(argc, argv, L"--quiet");
    const bool sync_input_list =
        argv && WantsFlag(argc, argv, L"--sync-input-list");
    const bool demo = argv && WantsFlag(argc, argv, L"--demo");
    const bool pime = argv && WantsFlag(argc, argv, L"--pime");
    // The GUI exe has no console of its own; attach to the caller's so the
    // headless probe can print when run from a terminal.
    const bool keys = argv && WantsFlag(argc, argv, L"--keys");
    if (keys) {
        // A GUI subsystem process usually inherits the caller's stdout (pipe
        // or console), which keeps output capturable; only attach a console
        // when launched without any standard handle.
        HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == nullptr || out == INVALID_HANDLE_VALUE) {
            if (AttachConsole(ATTACH_PARENT_PROCESS)) {
                out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
                if (out != INVALID_HANDLE_VALUE) SetStdHandle(STD_OUTPUT_HANDLE, out);
            }
        }
        const int result = pimewin::RunCli(argc, argv);
        if (argv) LocalFree(argv);
        return result;
    }

    if (sync_input_list) {
        const int result = pimewin::AddToUserInputList() ? 0 : 1;
        if (argv) LocalFree(argv);
        return result;
    }

    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = 0;

    if (!update.empty()) {
        // Hot update: stop the engine, copy the staged build over this one,
        // and let the next keystroke in any host spawn the new engine.
        const std::wstring here = pimewin::ModuleDirectory(nullptr);
        const bool stopped = StopEngine();
        // Sweep first: files renamed aside during this copy are still held by
        // the process reading them, so only a later run can remove them.
        DropStaleFiles(here);
        const bool copied = CopyTree(update, here);
        if (!quiet) {
            ShowResult(nullptr,
                       copied ? (stopped ? L"更新完成。输入法引擎已重启，正在运行的"
                                           L"程序无需重启。"
                                         : L"更新完成。")
                              : L"更新失败：无法复制文件。",
                       copied);
        }
        result = copied ? 0 : 1;
    } else if (install || uninstall) {
        const std::wstring dll =
            pimewin::PathJoin(pimewin::ModuleDirectory(nullptr), L"pime_tsf.dll");
        if (!pimewin::FileExists(dll)) {
            ShowResult(nullptr,
                       L"找不到 pime_tsf.dll。请先完整构建，再从 dist 目录运行。",
                       false);
            result = 1;
        } else if (install) {
            const bool registered = pimewin::RegisterTextService(dll);
            const bool ok = registered && SyncInputListInFreshProcess();
            if (!quiet) {
                ShowResult(nullptr,
                           ok ? L"安装完成。可在系统输入法列表中选择「PIME 拼音」，"
                                L"或在设置窗口里点「切换到 PIME」。"
                              : L"安装失败。请确认以管理员身份运行，并查看日志。",
                           ok);
            }
            result = ok ? 0 : 1;
        } else {
            const bool ok = pimewin::UnregisterTextService();
            if (!quiet) {
                ShowResult(nullptr,
                           ok ? L"已卸载 PIME 输入法。" : L"卸载未完全成功。", ok);
            }
            result = ok ? 0 : 1;
        }
    } else if (demo) {
        result = pimewin::RunDemoApp(instance, pime);
    } else {
        result = pimewin::RunSettingsApp(instance);
    }

    if (SUCCEEDED(com)) CoUninitialize();
    if (argv) LocalFree(argv);
    return result;
}
