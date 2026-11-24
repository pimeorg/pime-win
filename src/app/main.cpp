#include <windows.h>
#include <objbase.h>
#include <shellapi.h>

#include <string>

#include "app/settings_window.h"
#include "cli/cli_app.h"
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

    if (install || uninstall) {
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
