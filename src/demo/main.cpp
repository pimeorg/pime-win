// A deliberately small RichEdit host: the quickest way to see the TSF service
// composing, paging and committing in a real document.
#include <windows.h>
#include <msctf.h>
#include <richedit.h>
#include <objbase.h>
#include <shellapi.h>
#include <wrl/client.h>

#include <string>

#include "app/resource.h"
#include "tsf/guids.h"

using Microsoft::WRL::ComPtr;

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kClassName[] = L"PimeDemoWindow";

struct DemoState {
    HWND edit = nullptr;
    HFONT font = nullptr;
    HBRUSH background = nullptr;
};

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
    auto* state = reinterpret_cast<DemoState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            auto* self = reinterpret_cast<DemoState*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            LoadLibraryW(L"Msftedit.dll");
            self->background = CreateSolidBrush(RGB(0xff, 0xff, 0xff));
            const UINT dpi = GetDpiForWindow(window);
            LOGFONTW font{};
            font.lfHeight = -MulDiv(13, dpi, 72);
            font.lfWeight = FW_NORMAL;
            lstrcpynW(font.lfFaceName, L"Microsoft YaHei UI", LF_FACESIZE);
            self->font = CreateFontIndirectW(&font);

            self->edit = CreateWindowExW(
                0, MSFTEDIT_CLASS,
                L"在此输入拼音，例如 nihao，然后按空格上屏。\r\n"
                L"Shift 切换中英文；上下键、PageUp/PageDown 或 -/= 翻页；数字或鼠标选词。\r\n",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_TABSTOP | ES_MULTILINE |
                    ES_AUTOVSCROLL | ES_WANTRETURN | ES_NOHIDESEL,
                0, 0, 0, 0, window, nullptr, GetModuleHandleW(nullptr),
                nullptr);
            if (self->edit) {
                SendMessageW(self->edit, WM_SETFONT,
                             reinterpret_cast<WPARAM>(self->font), TRUE);
                SendMessageW(self->edit, EM_SETLIMITTEXT, 0, 0);
                SendMessageW(self->edit, EM_SETBKGNDCOLOR, 0,
                             RGB(0xff, 0xff, 0xff));
                SetFocus(self->edit);
            }
            return 0;
        }
        case WM_SIZE: {
            if (!state || !state->edit) break;
            const int margin = MulDiv(16, GetDpiForWindow(window), 96);
            MoveWindow(state->edit, margin, margin,
                       LOWORD(lparam) - margin * 2,
                       HIWORD(lparam) - margin * 2, TRUE);
            return 0;
        }
        case WM_SETFOCUS:
            if (state && state->edit) SetFocus(state->edit);
            return 0;
        case WM_ERASEBKGND: {
            RECT client{};
            GetClientRect(window, &client);
            FillRect(reinterpret_cast<HDC>(wparam), &client,
                     state && state->background
                         ? state->background
                         : reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
            return 1;
        }
        case WM_DESTROY:
            if (state) {
                if (state->font) DeleteObject(state->font);
                if (state->background) DeleteObject(state->background);
                state->font = nullptr;
                state->background = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

void ActivatePimeForProcess() {
    ComPtr<ITfInputProcessorProfiles> profiles;
    ComPtr<ITfInputProcessorProfileMgr> manager;
    HRESULT hr = CoCreateInstance(CLSID_TF_InputProcessorProfiles, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(profiles.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = profiles.As(&manager);
    if (SUCCEEDED(hr)) {
        hr = manager->ActivateProfile(
            TF_PROFILETYPE_INPUTPROCESSOR, pimewin::kLangId,
            pimewin::kClsidTextService, pimewin::kGuidProfile, nullptr,
            TF_IPPMF_FORPROCESS | TF_IPPMF_ENABLEPROFILE);
    }
    wchar_t path[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, path);
    lstrcatW(path, L"pime-activate.log");
    HANDLE file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        char text[64] = {};
        const int length = wsprintfA(text, "ActivateProfile hr=0x%08lX\r\n",
                                     static_cast<unsigned long>(hr));
        DWORD written = 0;
        WriteFile(file, text, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(file);
    }
}

namespace pimewin {

int RunDemoApp(HINSTANCE instance, bool request_pime) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.hIcon = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
    wc.hIconSm = static_cast<HICON>(LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    DemoState state;
    HWND window = CreateWindowExW(
        0, kClassName, L"PIME 测试编辑器",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 760, 520, nullptr,
        nullptr, instance, &state);
    if (!window) {
        if (SUCCEEDED(com)) CoUninitialize();
        return 1;
    }

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);
    if (request_pime) ActivatePimeForProcess();

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    return static_cast<int>(message.wParam);
}

}  // namespace pimewin
