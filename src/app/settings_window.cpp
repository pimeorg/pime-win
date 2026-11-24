#include "app/settings_window.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <string>
#include <vector>

#include "common/settings.h"
#include "common/win_util.h"
#include "tsf/register.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace pimewin {
namespace {

constexpr wchar_t kClassName[] = L"PimeSettingsWindow";
constexpr int kChainCombo = 1;
constexpr int kPageCombo = 2;
constexpr int kThemeCombo = 3;
constexpr int kChineseCheck = 4;
constexpr int kPunctuationCombo = 5;
constexpr int kApplyButton = 101;
constexpr int kInstallButton = 102;
constexpr int kUninstallButton = 103;
constexpr int kActivateButton = 104;

constexpr COLORREF kAccent = RGB(0x2f, 0x6b, 0xff);
constexpr COLORREF kText = RGB(0x1b, 0x1b, 0x1f);
constexpr COLORREF kSecondary = RGB(0x7a, 0x7a, 0x82);

struct AppState {
    HINSTANCE instance = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr;
    HFONT title_font = nullptr;
    HFONT subtitle_font = nullptr;
    HWND chain = nullptr;
    HWND page = nullptr;
    HWND theme = nullptr;
    HWND chinese = nullptr;
    HWND punctuation = nullptr;
    std::wstring status;
    std::vector<std::wstring> chains;
};

int Scaled(const AppState& state, int value) {
    return MulDiv(value, static_cast<int>(state.dpi), 96);
}

std::vector<std::wstring> ListChains() {
    std::vector<std::wstring> names;
    const std::wstring pattern = PathJoin(
        PathJoin(ModuleDirectory(nullptr), L"chains"), L"*.cbor");
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        std::wstring name = data.cFileName;
        const size_t dot = name.rfind(L'.');
        if (dot != std::wstring::npos) name.resize(dot);
        names.push_back(name);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    return names;
}

void AddComboString(HWND combo, const std::wstring& text) {
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(text.c_str()));
}

bool ComboText(HWND combo, std::wstring* out) {
    const int index = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
    if (index < 0) return false;
    const int length = static_cast<int>(SendMessageW(combo, CB_GETLBTEXTLEN, index, 0));
    if (length < 0) return false;
    out->resize(static_cast<size_t>(length) + 1);
    SendMessageW(combo, CB_GETLBTEXT, index,
                 reinterpret_cast<LPARAM>(out->data()));
    out->resize(static_cast<size_t>(length));
    return true;
}

void CreateFonts(AppState& state) {
    if (state.font) DeleteObject(state.font);
    if (state.title_font) DeleteObject(state.title_font);
    if (state.subtitle_font) DeleteObject(state.subtitle_font);

    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics,
                          0);
    metrics.lfMessageFont.lfHeight = -MulDiv(9, state.dpi, 72);
    state.font = CreateFontIndirectW(&metrics.lfMessageFont);

    LOGFONTW title = metrics.lfMessageFont;
    title.lfHeight = -MulDiv(18, state.dpi, 72);
    title.lfWeight = FW_SEMIBOLD;
    lstrcpynW(title.lfFaceName, L"Segoe UI", LF_FACESIZE);
    state.title_font = CreateFontIndirectW(&title);

    LOGFONTW subtitle = metrics.lfMessageFont;
    subtitle.lfHeight = -MulDiv(9, state.dpi, 72);
    lstrcpynW(subtitle.lfFaceName, L"Segoe UI", LF_FACESIZE);
    state.subtitle_font = CreateFontIndirectW(&subtitle);
}

void Place(HWND window, const AppState& state, int x, int y, int width,
           int height) {
    MoveWindow(window, Scaled(state, x), Scaled(state, y),
               Scaled(state, width), Scaled(state, height), TRUE);
}

void Layout(AppState& state, HWND window) {
    const int combo_height = 200;

    Place(state.chain, state, 128, 100, 300, combo_height);
    Place(state.page, state, 128, 140, 100, combo_height);
    Place(state.theme, state, 128, 180, 180, combo_height);
    Place(state.punctuation, state, 128, 218, 180, combo_height);
    Place(state.chinese, state, 128, 256, 260, 24);
    Place(GetDlgItem(window, kApplyButton), state, 24, 296, 112, 32);
    Place(GetDlgItem(window, kInstallButton), state, 146, 296, 112, 32);
    Place(GetDlgItem(window, kUninstallButton), state, 268, 296, 92, 32);
    Place(GetDlgItem(window, kActivateButton), state, 370, 296, 126, 32);
}

void Apply(AppState& state, HWND window) {
    Settings settings = LoadSettings();
    std::wstring chain;
    if (ComboText(state.chain, &chain) && !chain.empty()) {
        settings.chain = chain + L".cbor";
    }
    const int page_index =
        static_cast<int>(SendMessageW(state.page, CB_GETCURSEL, 0, 0));
    if (page_index >= 0) settings.page_size = static_cast<uint32_t>(page_index + 3);
    const int theme_index =
        static_cast<int>(SendMessageW(state.theme, CB_GETCURSEL, 0, 0));
    if (theme_index >= 0) settings.theme = static_cast<uint32_t>(theme_index);
    const int punctuation_index =
        static_cast<int>(SendMessageW(state.punctuation, CB_GETCURSEL, 0, 0));
    if (punctuation_index >= 0) {
        settings.punctuation = punctuation_index == 0 ? 1u : 0u;
    }
    settings.chinese =
        SendMessageW(state.chinese, BM_GETCHECK, 0, 0) == BST_CHECKED ? 1u : 0u;
    SaveSettings(settings);
    NotifyConfigChanged();
    state.status = L"设置已保存，已打开的输入框会立即生效。";
    InvalidateRect(window, nullptr, FALSE);
}

void Populate(AppState& state, HWND window) {
    const Settings settings = LoadSettings();
    state.chains = ListChains();
    std::wstring selected = settings.chain;
    if (selected.size() > 5 && selected.substr(selected.size() - 5) == L".cbor") {
        selected.resize(selected.size() - 5);
    }
    int selected_index = 0;
    for (size_t i = 0; i < state.chains.size(); ++i) {
        AddComboString(state.chain, state.chains[i]);
        if (state.chains[i] == selected) selected_index = static_cast<int>(i);
    }
    if (state.chains.empty()) {
        AddComboString(state.chain, selected);
    }
    SendMessageW(state.chain, CB_SETCURSEL, selected_index, 0);

    for (uint32_t size = 3; size <= 9; ++size) {
        AddComboString(state.page, std::to_wstring(size));
    }
    SendMessageW(state.page, CB_SETCURSEL, settings.page_size - 3, 0);

    AddComboString(state.theme, L"跟随系统");
    AddComboString(state.theme, L"浅色");
    AddComboString(state.theme, L"深色");
    SendMessageW(state.theme, CB_SETCURSEL, settings.theme, 0);

    AddComboString(state.punctuation, L"中文全角（，。？！）");
    AddComboString(state.punctuation, L"英文半角（,.?!）");
    SendMessageW(state.punctuation, CB_SETCURSEL,
                 settings.punctuation ? 0 : 1, 0);

    SendMessageW(state.chinese, BM_SETCHECK,
                 settings.chinese ? BST_CHECKED : BST_UNCHECKED, 0);
    (void)window;
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));

    switch (message) {
        case WM_NCCREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(window, GWLP_USERDATA,
                              reinterpret_cast<LONG_PTR>(create->lpCreateParams));
            return TRUE;
        }
        case WM_CREATE: {
            auto* self = reinterpret_cast<AppState*>(
                GetWindowLongPtrW(window, GWLP_USERDATA));
            self->dpi = GetDpiForWindow(window);
            CreateFonts(*self);
            const DWORD combo_style =
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                WS_VSCROLL;
            self->chain = CreateWindowExW(
                0, WC_COMBOBOXW, L"", combo_style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChainCombo)),
                self->instance, nullptr);
            self->page = CreateWindowExW(
                0, WC_COMBOBOXW, L"", combo_style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPageCombo)),
                self->instance, nullptr);
            self->theme = CreateWindowExW(
                0, WC_COMBOBOXW, L"", combo_style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kThemeCombo)),
                self->instance, nullptr);
            self->punctuation = CreateWindowExW(
                0, WC_COMBOBOXW, L"", combo_style, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(
                    static_cast<INT_PTR>(kPunctuationCombo)),
                self->instance, nullptr);
            self->chinese = CreateWindowExW(
                0, WC_BUTTONW, L"启动时默认中文",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX, 0, 0, 0,
                0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kChineseCheck)),
                self->instance, nullptr);
            const wchar_t* buttons[] = {L"保存并应用", L"安装输入法", L"卸载", L"切换到 PIME"};
            const int ids[] = {kApplyButton, kInstallButton, kUninstallButton,
                               kActivateButton};
            for (int i = 0; i < 4; ++i) {
                CreateWindowExW(
                    0, WC_BUTTONW, buttons[i],
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_GROUP * 0,
                    0, 0, 0, 0, window,
                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])),
                    self->instance, nullptr);
            }
            for (HWND control : {self->chain, self->page, self->theme,
                                 self->punctuation, self->chinese}) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(self->font), TRUE);
            }
            for (int id : ids) {
                SendMessageW(GetDlgItem(window, id), WM_SETFONT,
                             reinterpret_cast<WPARAM>(self->font), TRUE);
            }
            Populate(*self, window);
            Layout(*self, window);
            const BOOL dark = SystemUsesLightTheme() ? FALSE : TRUE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            return 0;
        }
        case WM_DPICHANGED: {
            if (!state) break;
            state->dpi = HIWORD(wparam);
            CreateFonts(*state);
            for (HWND control : {state->chain, state->page, state->theme,
                                 state->punctuation, state->chinese,
                                 GetDlgItem(window, kApplyButton),
                                 GetDlgItem(window, kInstallButton),
                                 GetDlgItem(window, kUninstallButton),
                                 GetDlgItem(window, kActivateButton)}) {
                SendMessageW(control, WM_SETFONT,
                             reinterpret_cast<WPARAM>(state->font), TRUE);
            }
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            Layout(*state, window);
            InvalidateRect(window, nullptr, TRUE);
            return 0;
        }
        case WM_COMMAND: {
            if (!state) break;
            switch (LOWORD(wparam)) {
                case kApplyButton:
                    Apply(*state, window);
                    return 0;
                case kInstallButton:
                    state->status = L"安装需要管理员权限，请在弹出的窗口中确认。";
                    RunElevated(L"--install");
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case kUninstallButton:
                    state->status = L"卸载需要管理员权限，请在弹出的窗口中确认。";
                    RunElevated(L"--uninstall");
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                case kActivateButton:
                    state->status = ActivateTextService()
                                        ? L"已切换到 PIME，开始输入即可。"
                                        : L"切换失败：请先安装输入法。";
                    InvalidateRect(window, nullptr, FALSE);
                    return 0;
                default:
                    break;
            }
            break;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, kText);
            return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT client{};
            GetClientRect(window, &client);
            FillRect(dc, &client,
                     reinterpret_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

            const int margin = state ? Scaled(*state, 24) : 24;
            RECT accent{0, 0, client.right,
                        state ? Scaled(*state, 4) : 4};
            HBRUSH accent_brush = CreateSolidBrush(kAccent);
            FillRect(dc, &accent, accent_brush);
            DeleteObject(accent_brush);

            SetBkMode(dc, TRANSPARENT);
            if (state) {
                SetTextColor(dc, kText);
                HFONT old = static_cast<HFONT>(
                    SelectObject(dc, state->title_font));
                RECT title{margin, Scaled(*state, 18), client.right - margin,
                           Scaled(*state, 52)};
                DrawTextW(dc, L"PIME 拼音输入法", -1, &title,
                          DT_LEFT | DT_SINGLELINE | DT_VCENTER);
                SelectObject(dc, state->subtitle_font);
                SetTextColor(dc, kSecondary);
                RECT subtitle{margin, Scaled(*state, 52), client.right - margin,
                              Scaled(*state, 74)};
                DrawTextW(dc, L"复用 pime-core 与插件包 · Windows TSF 文本服务",
                          -1, &subtitle, DT_LEFT | DT_SINGLELINE);

                RECT line{margin, Scaled(*state, 88), client.right - margin,
                          Scaled(*state, 89)};
                FillRect(dc, &line,
                         reinterpret_cast<HBRUSH>(GetStockObject(LTGRAY_BRUSH)));

                RECT labels[] = {
                    {margin, Scaled(*state, 104), margin + Scaled(*state, 100),
                     Scaled(*state, 126)},
                    {margin, Scaled(*state, 144), margin + Scaled(*state, 100),
                     Scaled(*state, 166)},
                    {margin, Scaled(*state, 184), margin + Scaled(*state, 100),
                     Scaled(*state, 206)},
                    {margin, Scaled(*state, 222), margin + Scaled(*state, 100),
                     Scaled(*state, 244)},
                };
                SetTextColor(dc, kText);
                DrawTextW(dc, L"输入方案", -1, &labels[0],
                          DT_LEFT | DT_SINGLELINE);
                DrawTextW(dc, L"候选每页", -1, &labels[1],
                          DT_LEFT | DT_SINGLELINE);
                DrawTextW(dc, L"外观", -1, &labels[2], DT_LEFT | DT_SINGLELINE);
                DrawTextW(dc, L"标点", -1, &labels[3], DT_LEFT | DT_SINGLELINE);

                SetTextColor(dc, kAccent);
                RECT status{margin, Scaled(*state, 344),
                            client.right - margin, Scaled(*state, 366)};
                DrawTextW(dc, state->status.c_str(), -1, &status,
                          DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

                SetTextColor(dc, kSecondary);
                RECT hint{margin, Scaled(*state, 370), client.right - margin,
                          Scaled(*state, 392)};
                DrawTextW(dc, L"Shift 切换中英文 · 输入法列表里可切换 PIME 拼音",
                          -1, &hint, DT_LEFT | DT_SINGLELINE);
                SelectObject(dc, old);
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            if (state) {
                if (state->font) DeleteObject(state->font);
                if (state->title_font) DeleteObject(state->title_font);
                if (state->subtitle_font) DeleteObject(state->subtitle_font);
                state->font = state->title_font = state->subtitle_font = nullptr;
            }
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int RunSettingsApp(HINSTANCE instance) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    AppState state;
    state.instance = instance;

    const UINT dpi = GetDpiForSystem();
    RECT rect{0, 0, MulDiv(520, dpi, 96), MulDiv(400, dpi, 96)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX &
                                          ~WS_THICKFRAME,
                             FALSE, 0, dpi);
    HWND window = CreateWindowExW(
        0, kClassName, L"PIME 设置",
        (WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME),
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left,
        rect.bottom - rect.top, nullptr, nullptr, instance, &state);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsDialogMessageW(window, &message)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace pimewin
