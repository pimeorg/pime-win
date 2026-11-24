#include "app/settings_window.h"

#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "common/settings.h"
#include "common/win_util.h"
#include "tsf/guids.h"
#include "tsf/register.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

constexpr wchar_t kClassName[] = L"PimeSettingsWindow";
constexpr wchar_t kWindowTitle[] = L"PIME 拼音输入法";
constexpr wchar_t kVersion[] = L"1.0.0";

// Logical (96 dpi) metrics. The render target is given the window DPI, so
// every coordinate below is a DIP and needs no manual scaling.
constexpr float kWidth = 900.f;
constexpr float kHeight = 640.f;
constexpr float kSidebar = 224.f;
constexpr float kMargin = 32.f;

enum class Page { General, Appearance, About };
enum class Menu { None, Chain, PageSize };

enum : int {
    kNone = -1,
    kNavGeneral = 1,
    kNavAppearance = 2,
    kNavAbout = 3,
    kChain = 10,
    kPageSize = 11,
    kPunctFull = 20,
    kPunctHalf = 21,
    kThemeSystem = 30,
    kThemeLight = 31,
    kThemeDark = 32,
    kToggle = 40,
    kApply = 50,
    kClose = 51,
    kInstall = 52,
    kUninstall = 53,
    kActivate = 54,
    kMenuItem = 100,
};

struct Box {
    float l = 0, t = 0, r = 0, b = 0;
    bool Contains(float x, float y) const {
        return x >= l && x < r && y >= t && y < b;
    }
    float W() const { return r - l; }
    float H() const { return b - t; }
};

struct Layout {
    Box nav[3];
    Box chain;
    Box page_size;
    Box punct[2];
    Box theme[3];
    Box toggle;
    Box apply;
    Box close;
    Box install;
    Box uninstall;
    Box activate;
    Box menu;
    int menu_items = 0;
};

struct Palette {
    D2D1_COLOR_F bg;
    D2D1_COLOR_F sidebar;
    D2D1_COLOR_F card;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accent_soft;
    D2D1_COLOR_F track;
    D2D1_COLOR_F hover;
    D2D1_COLOR_F danger;
    D2D1_COLOR_F success;
    D2D1_COLOR_F success_soft;
};

struct Tween {
    float value = 0;
    float from = 0;
    float to = 0;
    ULONGLONG start = 0;
    bool running = false;
};

struct AppState {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    UINT dpi = 96;
    float width = kWidth;
    float height = kHeight;

    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dwrite;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<IDWriteTextFormat> f_logo;
    ComPtr<IDWriteTextFormat> f_app;
    ComPtr<IDWriteTextFormat> f_page;
    ComPtr<IDWriteTextFormat> f_section;
    ComPtr<IDWriteTextFormat> f_label;
    ComPtr<IDWriteTextFormat> f_desc;
    ComPtr<IDWriteTextFormat> f_value;
    ComPtr<IDWriteTextFormat> f_small;
    ComPtr<IDWriteTextFormat> f_button;
    ComPtr<IDWriteTextFormat> f_badge;

    Page page = Page::General;
    Menu menu = Menu::None;
    int focus = kNone;
    bool focus_visible = false;
    float mx = -1000, my = -1000;
    bool mouse_in = false;
    bool system_dark = false;
    bool installed = false;

    Settings settings;
    Settings saved;
    std::vector<std::wstring> chains;

    std::wstring status;
    bool status_ok = true;

    Tween toggle;

    Layout layout;
};

D2D1_COLOR_F Rgb(int r, int g, int b, float a = 1.f) {
    return D2D1::ColorF(r / 255.f, g / 255.f, b / 255.f, a);
}

D2D1_COLOR_F Mix(const D2D1_COLOR_F& a, const D2D1_COLOR_F& b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

D2D1_COLOR_F Brighter(const D2D1_COLOR_F& c, float amount) {
    return D2D1::ColorF(c.r + (1.f - c.r) * amount,
                        c.g + (1.f - c.g) * amount,
                        c.b + (1.f - c.b) * amount, c.a);
}

Palette Pal(bool dark) {
    Palette p{};
    if (dark) {
        p.bg = Rgb(23, 24, 27);
        p.sidebar = Rgb(30, 32, 36);
        p.card = Rgb(36, 38, 43);
        p.border = Rgb(53, 56, 62);
        p.text = Rgb(241, 242, 244);
        p.secondary = Rgb(150, 156, 170);
        p.accent = Rgb(91, 140, 255);
        p.accent_soft = Rgb(38, 49, 76);
        p.track = Rgb(59, 63, 70);
        p.hover = Rgb(43, 46, 52);
        p.danger = Rgb(255, 107, 107);
        p.success = Rgb(76, 199, 118);
        p.success_soft = Rgb(32, 51, 41);
    } else {
        p.bg = Rgb(244, 246, 249);
        p.sidebar = Rgb(238, 241, 246);
        p.card = Rgb(255, 255, 255);
        p.border = Rgb(224, 228, 234);
        p.text = Rgb(27, 28, 32);
        p.secondary = Rgb(109, 115, 124);
        p.accent = Rgb(47, 107, 255);
        p.accent_soft = Rgb(232, 239, 255);
        p.track = Rgb(216, 220, 227);
        p.hover = Rgb(241, 243, 247);
        p.danger = Rgb(217, 58, 58);
        p.success = Rgb(47, 160, 79);
        p.success_soft = Rgb(232, 246, 237);
    }
    return p;
}

// ---- drawing helpers -------------------------------------------------------

void FillBox(AppState& s, Box b, float radius, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius),
        brush.Get());
}

void StrokeBox(AppState& s, Box b, float radius, const D2D1_COLOR_F& color,
               float width) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(b.l, b.t, b.r, b.b), radius, radius),
        brush.Get(), width);
}

void FillPlain(AppState& s, Box b, const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillRectangle(D2D1::RectF(b.l, b.t, b.r, b.b), brush.Get());
}

void FillEllipse(AppState& s, float cx, float cy, float radius,
                 const D2D1_COLOR_F& color) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius),
                      brush.Get());
}

void DrawLine(AppState& s, float x0, float y0, float x1, float y1,
              const D2D1_COLOR_F& color, float width) {
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), brush.Get(),
                   width);
}

void DrawText(AppState& s, IDWriteTextFormat* format, const std::wstring& text,
              Box box, const D2D1_COLOR_F& color,
              DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
    if (text.empty() || box.W() <= 0) return;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s.dwrite->CreateTextLayout(text.c_str(),
                                          static_cast<UINT32>(text.size()),
                                          format, box.W(), box.H(),
                                          layout.GetAddressOf()))) {
        return;
    }
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(s.rt->CreateSolidColorBrush(color, brush.GetAddressOf()))) return;
    s.rt->DrawTextLayout(D2D1::Point2F(box.l, box.t), layout.Get(), brush.Get());
}

float MeasureText(AppState& s, IDWriteTextFormat* format,
                  const std::wstring& text) {
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(s.dwrite->CreateTextLayout(text.c_str(),
                                          static_cast<UINT32>(text.size()),
                                          format, 4096.f, 128.f,
                                          layout.GetAddressOf()))) {
        return 0;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0;
    return metrics.width;
}

void DrawChevron(AppState& s, float cx, float cy, const D2D1_COLOR_F& color) {
    DrawLine(s, cx - 4, cy - 2, cx, cy + 2, color, 1.6f);
    DrawLine(s, cx, cy + 2, cx + 4, cy - 2, color, 1.6f);
}

void FocusRing(AppState& s, Box b, int id, float radius) {
    if (s.focus != id || !s.focus_visible) return;
    StrokeBox(s, Box{b.l - 2, b.t - 2, b.r + 2, b.b + 2}, radius + 2,
              Pal(s.system_dark).accent, 1.5f);
}

// ---- state helpers ---------------------------------------------------------

bool IsInstalled() {
    wchar_t clsid[64] = {};
    if (StringFromGUID2(kClsidTextService, clsid, 64) <= 0) return false;
    const std::wstring key = L"CLSID\\" + std::wstring(clsid) + L"\\InprocServer32";
    wchar_t path[MAX_PATH * 2] = {};
    DWORD size = sizeof(path);
    return RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, RRF_RT_REG_SZ,
                        nullptr, path, &size) == ERROR_SUCCESS;
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

std::wstring ChainName(const std::wstring& file) {
    std::wstring name = file;
    if (name.size() > 5 && name.substr(name.size() - 5) == L".cbor") {
        name.resize(name.size() - 5);
    }
    return name;
}

int ChainIndex(const AppState& s) {
    const std::wstring current = ChainName(s.settings.chain);
    for (size_t i = 0; i < s.chains.size(); ++i) {
        if (s.chains[i] == current) return static_cast<int>(i);
    }
    return 0;
}

int PageIndex(const AppState& s) {
    const int index = static_cast<int>(s.settings.page_size) - 3;
    return index < 0 ? 0 : (index > 6 ? 6 : index);
}

bool SameSettings(const Settings& a, const Settings& b) {
    return a.chain == b.chain && a.theme == b.theme &&
           a.page_size == b.page_size && a.chinese == b.chinese &&
           a.punctuation == b.punctuation;
}

bool Dirty(const AppState& s) { return !SameSettings(s.settings, s.saved); }

bool Hover(const AppState& s, Box b) {
    return s.mouse_in && s.menu == Menu::None && b.Contains(s.mx, s.my);
}

bool CandidateDark(const AppState& s) {
    if (s.settings.theme == 2) return true;
    if (s.settings.theme == 1) return false;
    return s.system_dark;
}

// ---- text formats ----------------------------------------------------------

void MakeFormat(AppState& s, float size, DWRITE_FONT_WEIGHT weight,
                ComPtr<IDWriteTextFormat>* out) {
    if (FAILED(s.dwrite->CreateTextFormat(
            L"Segoe UI", nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
            out->GetAddressOf()))) {
        return;
    }
    (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

bool CreateFormats(AppState& s) {
    if (!s.dwrite) return false;
    MakeFormat(s, 20.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_logo);
    MakeFormat(s, 16.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_app);
    MakeFormat(s, 19.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_page);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_section);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_label);
    MakeFormat(s, 11.5f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_desc);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_value);
    MakeFormat(s, 11.f, DWRITE_FONT_WEIGHT_NORMAL, &s.f_small);
    MakeFormat(s, 13.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_button);
    MakeFormat(s, 12.f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &s.f_badge);
    return s.f_page && s.f_button;
}

// ---- layout ----------------------------------------------------------------

Box ItemBox(const AppState& s, int index) {
    const Layout& l = s.layout;
    return Box{l.menu.l + 4, l.menu.t + 6 + index * 34.f,
               l.menu.r - 4, l.menu.t + 6 + (index + 1) * 34.f};
}

void BuildLayout(AppState& s) {
    Layout l{};
    const float w = s.width;
    const float h = s.height;
    const float x0 = kSidebar + kMargin;
    const float x1 = w - kMargin;

    for (int i = 0; i < 3; ++i) {
        l.nav[i] = Box{16.f, 116.f + i * 52.f, kSidebar - 16.f,
                       156.f + i * 52.f};
    }

    l.close = Box{x1 - 130.f - 12.f - 92.f, h - 52.f, x1 - 130.f - 12.f,
                  h - 18.f};
    l.apply = Box{x1 - 130.f, h - 52.f, x1, h - 18.f};

    if (s.page == Page::General) {
        const float card_top = 96.f;
        const float card_bottom = 298.f;
        const float row_left = x0 + 20.f;
        const float row_right = x1 - 20.f;
        l.chain = Box{row_right - 220.f, card_top + 25.f, row_right,
                      card_top + 61.f};
        l.page_size = Box{row_right - 130.f, card_top + 83.f, row_right,
                          card_top + 119.f};
        const float seg_right = row_right;
        const float seg_top = card_top + 141.f;
        l.punct[0] = Box{seg_right - 300.f, seg_top, seg_right - 150.f,
                         seg_top + 36.f};
        l.punct[1] = Box{seg_right - 150.f, seg_top, seg_right, seg_top + 36.f};
        l.toggle = Box{row_right - 44.f, 314.f + 22.f, row_right, 314.f + 46.f};
        (void)card_bottom;
        (void)row_left;
    } else if (s.page == Page::Appearance) {
        const float card_top = 96.f;
        const float left = x0 + 20.f;
        const float right = x1 - 20.f;
        const float gap = 12.f;
        const float tile_w = (right - left - gap * 2.f) / 3.f;
        for (int i = 0; i < 3; ++i) {
            l.theme[i] = Box{left + i * (tile_w + gap), card_top + 44.f,
                             left + i * (tile_w + gap) + tile_w,
                             card_top + 148.f};
        }
    } else {
        const float card_top = 338.f;
        l.install = Box{x0 + 20.f, card_top + 76.f, x0 + 168.f,
                        card_top + 112.f};
        l.uninstall = Box{x0 + 178.f, card_top + 76.f, x0 + 266.f,
                          card_top + 112.f};
        l.activate = Box{x0 + 276.f, card_top + 76.f, x0 + 412.f,
                         card_top + 112.f};
    }

    if (s.menu == Menu::Chain || s.menu == Menu::PageSize) {
        const Box& anchor =
            s.menu == Menu::Chain ? l.chain : l.page_size;
        const int count = s.menu == Menu::Chain
                              ? static_cast<int>(s.chains.size())
                              : 7;
        const float height = 12.f + count * 34.f;
        Box menu{anchor.l, anchor.b + 4.f, anchor.l + anchor.W(),
                 anchor.b + 4.f + height};
        if (menu.b > h - 80.f) {
            menu = Box{anchor.l, anchor.t - 4.f - height,
                       anchor.l + anchor.W(), anchor.t - 4.f};
        }
        l.menu = menu;
        l.menu_items = count;
    }

    s.layout = l;
}

// ---- hit testing -----------------------------------------------------------

int HitTest(const AppState& s, float x, float y) {
    if (s.menu != Menu::None) {
        for (int i = 0; i < s.layout.menu_items; ++i) {
            if (ItemBox(s, i).Contains(x, y)) return kMenuItem + i;
        }
        return kNone;
    }
    for (int i = 0; i < 3; ++i) {
        if (s.layout.nav[i].Contains(x, y)) return kNavGeneral + i;
    }
    if (s.page == Page::General) {
        if (s.layout.chain.Contains(x, y)) return kChain;
        if (s.layout.page_size.Contains(x, y)) return kPageSize;
        if (s.layout.punct[0].Contains(x, y)) return kPunctFull;
        if (s.layout.punct[1].Contains(x, y)) return kPunctHalf;
        if (s.layout.toggle.Contains(x, y)) return kToggle;
    } else if (s.page == Page::Appearance) {
        for (int i = 0; i < 3; ++i) {
            if (s.layout.theme[i].Contains(x, y)) return kThemeSystem + i;
        }
    } else {
        if (s.layout.install.Contains(x, y)) return kInstall;
        if (s.layout.uninstall.Contains(x, y)) return kUninstall;
        if (s.layout.activate.Contains(x, y)) return kActivate;
    }
    if (s.layout.apply.Contains(x, y)) return kApply;
    if (s.layout.close.Contains(x, y)) return kClose;
    return kNone;
}

// ---- drawing ---------------------------------------------------------------

void DrawSelect(AppState& s, const Box& b, const std::wstring& value, int id,
                bool open) {
    const Palette p = Pal(s.system_dark);
    const bool hot = Hover(s, b);
    FillBox(s, b, 8.f, p.card);
    StrokeBox(s, b, 8.f, (hot || open) ? p.accent : p.border, 1.f);
    DrawText(s, s.f_value.Get(), value,
             Box{b.l + 14.f, b.t, b.r - 32.f, b.b}, p.text);
    DrawChevron(s, b.r - 18.f, b.t + b.H() / 2.f, p.secondary);
    FocusRing(s, b, id, 8.f);
}

void DrawSegmented(AppState& s, const Box& b, const wchar_t* const* labels,
                   int count, int selected, int first_id) {
    const Palette p = Pal(s.system_dark);
    const float pad = 3.f;
    FillBox(s, b, 9.f, p.track);
    const float inner_w = b.W() - pad * 2.f;
    const float seg_w = inner_w / count;
    for (int i = 0; i < count; ++i) {
        Box seg{b.l + pad + i * seg_w, b.t + pad,
                b.l + pad + (i + 1) * seg_w, b.b - pad};
        if (i == selected) {
            FillBox(s, seg, 7.f, p.card);
            StrokeBox(s, seg, 7.f, p.border, 1.f);
        } else if (Hover(s, seg)) {
            FillBox(s, seg, 7.f, p.hover);
        }
        DrawText(s, s.f_value.Get(), labels[i], seg,
                 i == selected ? p.text : p.secondary,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (s.focus >= first_id && s.focus < first_id + count) {
        FocusRing(s, b, s.focus, 9.f);
    }
}

void DrawToggle(AppState& s, const Box& b) {
    const Palette p = Pal(s.system_dark);
    const float t = s.toggle.value;
    FillBox(s, b, b.H() / 2.f, Mix(p.track, p.accent, t));
    const float knob = b.H() - 6.f;
    const float cx = b.l + 3.f + knob / 2.f + t * (b.W() - 6.f - knob);
    FillEllipse(s, cx, b.t + b.H() / 2.f, knob / 2.f, Rgb(255, 255, 255));
    FocusRing(s, b, kToggle, b.H() / 2.f);
}

enum class ButtonKind { Primary, Secondary, Danger };

void DrawButton(AppState& s, const Box& b, const wchar_t* label, int id,
                ButtonKind kind) {
    const Palette p = Pal(s.system_dark);
    const bool hot = Hover(s, b);
    if (kind == ButtonKind::Primary) {
        FillBox(s, b, 8.f, hot ? Brighter(p.accent, 0.12f) : p.accent);
        DrawText(s, s.f_button.Get(), label, b, Rgb(255, 255, 255),
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        FillBox(s, b, 8.f, p.card);
        StrokeBox(s, b, 8.f, hot ? p.accent : p.border, 1.f);
        DrawText(s, s.f_button.Get(), label, b,
                 kind == ButtonKind::Danger ? p.danger : p.text,
                 DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    FocusRing(s, b, id, 8.f);
}

void DrawCard(AppState& s, const Box& b) {
    const Palette p = Pal(s.system_dark);
    FillBox(s, b, 12.f, p.card);
    StrokeBox(s, b, 12.f, p.border, 1.f);
}

void DrawRowLabel(AppState& s, const Box& card, float row_top,
                  const wchar_t* label, const wchar_t* description) {
    const Palette p = Pal(s.system_dark);
    DrawText(s, s.f_label.Get(), label,
             Box{card.l + 20.f, row_top + 6.f, card.r - 260.f, row_top + 30.f},
             p.text);
    DrawText(s, s.f_desc.Get(), description,
             Box{card.l + 20.f, row_top + 31.f, card.r - 260.f, row_top + 49.f},
             p.secondary);
}

void DrawSidebar(AppState& s) {
    const Palette p = Pal(s.system_dark);
    FillPlain(s, Box{0, 0, kSidebar, s.height}, p.sidebar);
    FillPlain(s, Box{kSidebar - 1.f, 0, kSidebar, s.height}, p.border);

    FillBox(s, Box{20.f, 24.f, 60.f, 64.f}, 11.f, p.accent);
    DrawText(s, s.f_logo.Get(), L"拼", Box{20.f, 24.f, 60.f, 64.f},
             Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(s, s.f_app.Get(), L"PIME 拼音",
             Box{72.f, 26.f, kSidebar - 12.f, 46.f}, p.text);
    DrawText(s, s.f_small.Get(), L"Windows 输入法",
             Box{72.f, 46.f, kSidebar - 12.f, 64.f}, p.secondary);

    const Page pages[3] = {Page::General, Page::Appearance, Page::About};
    const wchar_t* names[3] = {L"常规", L"外观", L"关于"};
    const int ids[3] = {kNavGeneral, kNavAppearance, kNavAbout};
    for (int i = 0; i < 3; ++i) {
        const Box& b = s.layout.nav[i];
        const bool selected = s.page == pages[i];
        if (selected) {
            FillBox(s, b, 9.f, p.accent_soft);
            FillBox(s, Box{b.l + 1.f, b.t + 13.f, b.l + 4.f, b.b - 13.f},
                    2.f, p.accent);
        } else if (Hover(s, b)) {
            FillBox(s, b, 9.f, p.hover);
        }
        DrawText(s, selected ? s.f_button.Get() : s.f_label.Get(), names[i],
                 Box{b.l + 16.f, b.t, b.r - 8.f, b.b},
                 selected ? p.accent : p.text);
        FocusRing(s, b, ids[i], 9.f);
    }

    const float bottom = s.height;
    DrawLine(s, 20.f, bottom - 86.f, kSidebar - 20.f, bottom - 86.f,
             p.border, 1.f);
    FillEllipse(s, 24.f, bottom - 65.f, 3.5f,
                s.installed ? p.success : p.secondary);
    DrawText(s, s.f_small.Get(),
             s.installed ? L"输入法已安装" : L"输入法未安装",
             Box{34.f, bottom - 74.f, kSidebar - 16.f, bottom - 56.f},
             p.secondary);
    DrawText(s, s.f_small.Get(), L"版本 " + std::wstring(kVersion),
             Box{20.f, bottom - 44.f, kSidebar - 16.f, bottom - 26.f},
             p.secondary);
}

void DrawPageHeader(AppState& s, const wchar_t* title,
                    const wchar_t* description) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;
    DrawText(s, s.f_page.Get(), title, Box{x0, 28.f, x1, 56.f}, p.text);
    DrawText(s, s.f_desc.Get(), description, Box{x0, 58.f, x1, 76.f},
             p.secondary);
}

void DrawGeneralPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 298.f};
    DrawCard(s, card1);

    DrawRowLabel(s, card1, 110.f, L"输入方案", L"选择 assets/chains 目录中的插件链");
    DrawSelect(s, s.layout.chain,
               s.chains.empty() ? ChainName(s.settings.chain)
                                : s.chains[ChainIndex(s)],
               kChain, s.menu == Menu::Chain);
    DrawLine(s, card1.l + 20.f, 168.f, card1.r - 20.f, 168.f, p.border, 1.f);

    DrawRowLabel(s, card1, 168.f, L"候选每页", L"每页显示的候选数量");
    DrawSelect(s, s.layout.page_size,
               std::to_wstring(s.settings.page_size) + L" 个", kPageSize,
               s.menu == Menu::PageSize);
    DrawLine(s, card1.l + 20.f, 226.f, card1.r - 20.f, 226.f, p.border, 1.f);

    DrawRowLabel(s, card1, 226.f, L"标点风格",
                 L"输入中按标点会先上屏候选词，再输入标点");
    const wchar_t* punct_labels[2] = {L"中文全角 ，。？！", L"英文半角 ,.?!"};
    const int punct_selected = s.settings.punctuation ? 0 : 1;
    DrawSegmented(s, Box{s.layout.punct[0].l, s.layout.punct[0].t,
                         s.layout.punct[1].r, s.layout.punct[0].b},
                  punct_labels, 2, punct_selected,
                  kPunctFull);

    const Box card2{x0, 314.f, x1, 418.f};
    DrawCard(s, card2);
    DrawRowLabel(s, card2, 314.f, L"启动时默认中文",
                 L"新应用默认进入中文输入");
    DrawToggle(s, s.layout.toggle);
    DrawText(s, s.f_small.Get(),
             L"每个应用会分别记住上次的中英文状态，按 Shift 轻按可切换。",
             Box{card2.l + 20.f, 386.f, card2.r - 20.f, 404.f}, p.secondary);

    const Box card3{x0, 434.f, x1, 554.f};
    DrawCard(s, card3);
    DrawText(s, s.f_section.Get(), L"快捷键",
             Box{card3.l + 20.f, card3.t + 14.f, card3.r - 20.f,
                 card3.t + 34.f},
             p.text);
    struct Shortcut {
        const wchar_t* key;
        const wchar_t* description;
    };
    const Shortcut shortcuts[] = {
        {L"Shift", L"轻按切换中英文"},
        {L"Space", L"选择第一个候选"},
        {L"数字键", L"选择对应候选"},
        {L"↑ ↓ / PgUp PgDn", L"翻页"},
        {L"v + 数字", L"符号与算式输入"},
        {L"Esc", L"取消当前输入"},
    };
    const float column = (card3.W() - 40.f) / 2.f;
    for (int i = 0; i < 6; ++i) {
        const float cx = card3.l + 20.f + (i % 2) * column;
        const float cy = card3.t + 44.f + (i / 2) * 24.f;
        DrawText(s, s.f_desc.Get(), shortcuts[i].key,
                 Box{cx, cy, cx + 116.f, cy + 22.f}, p.text);
        DrawText(s, s.f_desc.Get(), shortcuts[i].description,
                 Box{cx + 122.f, cy, cx + column - 8.f, cy + 22.f},
                 p.secondary);
    }
}

void DrawThemeTile(AppState& s, int index, const wchar_t* label, int id) {
    const Palette p = Pal(s.system_dark);
    const Box& b = s.layout.theme[index];
    const bool selected = static_cast<int>(s.settings.theme) == index;
    const bool hot = Hover(s, b);
    FillBox(s, b, 10.f, selected ? p.accent_soft : p.card);
    StrokeBox(s, b, 10.f, selected ? p.accent : (hot ? p.secondary : p.border),
              selected ? 1.6f : 1.f);

    const Box preview{b.l + 12.f, b.t + 12.f, b.r - 12.f, b.t + 58.f};
    D2D1_COLOR_F preview_bg = Rgb(246, 248, 251);
    D2D1_COLOR_F line = Rgb(205, 212, 222);
    if (index == 2) {
        preview_bg = Rgb(28, 30, 34);
        line = Rgb(70, 75, 84);
    } else if (index == 0) {
        preview_bg = s.system_dark ? Rgb(28, 30, 34) : Rgb(246, 248, 251);
        line = s.system_dark ? Rgb(70, 75, 84) : Rgb(205, 212, 222);
    }
    FillBox(s, preview, 7.f, preview_bg);
    FillBox(s, Box{preview.l + 10.f, preview.t + 12.f, preview.l + 10.f + 34.f,
                   preview.t + 17.f},
            2.5f, line);
    FillBox(s, Box{preview.l + 10.f, preview.t + 23.f, preview.l + 10.f + 58.f,
                   preview.t + 28.f},
            2.5f, line);
    FillBox(s, Box{preview.l + 10.f, preview.t + 34.f, preview.l + 10.f + 24.f,
                   preview.t + 39.f},
            2.5f, line);

    DrawText(s, s.f_value.Get(), label,
             Box{b.l + 8.f, b.t + 64.f, b.r - 8.f, b.t + 88.f},
             selected ? p.accent : p.text, DWRITE_TEXT_ALIGNMENT_CENTER);
    if (selected) {
        FillEllipse(s, b.r - 16.f, b.t + 16.f, 7.f, p.accent);
        DrawText(s, s.f_badge.Get(), L"✓",
                 Box{b.r - 24.f, b.t + 7.f, b.r - 8.f, b.t + 25.f},
                 Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    FocusRing(s, b, id, 10.f);
}

void DrawCandidatePreview(AppState& s, const Box& card) {
    const Palette p = Pal(s.system_dark);
    const bool dark = CandidateDark(s);
    const D2D1_COLOR_F strip = dark ? Rgb(42, 43, 47) : Rgb(255, 255, 255);
    const D2D1_COLOR_F border = dark ? Rgb(58, 61, 68) : Rgb(224, 228, 234);
    const D2D1_COLOR_F text = dark ? Rgb(238, 239, 242) : Rgb(27, 28, 32);
    const D2D1_COLOR_F sub = dark ? Rgb(154, 160, 170) : Rgb(109, 115, 124);
    const D2D1_COLOR_F accent = dark ? Rgb(91, 140, 255) : Rgb(47, 107, 255);

    const Box strip_box{card.l + 20.f, card.t + 44.f, card.r - 20.f,
                        card.t + 108.f};
    FillBox(s, strip_box, 10.f, strip);
    StrokeBox(s, strip_box, 10.f, border, 1.f);

    const float cy = strip_box.t + strip_box.H() / 2.f;
    const Box badge{strip_box.l + 12.f, cy - 12.f, strip_box.l + 36.f,
                    cy + 12.f};
    FillBox(s, badge, 6.f, accent);
    DrawText(s, s.f_badge.Get(), s.settings.chinese ? L"中" : L"英", badge,
             Rgb(255, 255, 255), DWRITE_TEXT_ALIGNMENT_CENTER);

    const std::wstring pinyin = L"pinyin";
    const float pin_x = strip_box.l + 48.f;
    DrawText(s, s.f_value.Get(), pinyin,
             Box{pin_x, strip_box.t, pin_x + 90.f, strip_box.b}, text);
    const float pin_w = MeasureText(s, s.f_value.Get(), pinyin);
    DrawLine(s, pin_x + 1.f, cy + 10.f, pin_x + pin_w, cy + 10.f, accent, 1.f);

    DrawText(s, s.f_value.Get(),
             L"1 拼音    2 品    3 频    4 拼    5 聘",
             Box{pin_x + 100.f, strip_box.t, strip_box.r - 90.f, strip_box.b},
             text);
    DrawText(s, s.f_small.Get(), dark ? L"深色" : L"浅色",
             Box{strip_box.r - 80.f, strip_box.t, strip_box.r - 12.f,
                 strip_box.b},
             sub, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

void DrawAppearancePage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 264.f};
    DrawCard(s, card1);
    DrawText(s, s.f_section.Get(), L"主题",
             Box{card1.l + 20.f, card1.t + 14.f, card1.r - 20.f,
                 card1.t + 34.f},
             p.text);
    DrawThemeTile(s, 0, L"跟随系统", kThemeSystem);
    DrawThemeTile(s, 1, L"浅色", kThemeLight);
    DrawThemeTile(s, 2, L"深色", kThemeDark);

    const Box card2{x0, 280.f, x1, 420.f};
    DrawCard(s, card2);
    DrawText(s, s.f_section.Get(), L"候选窗口",
             Box{card2.l + 20.f, card2.t + 14.f, card2.r - 20.f,
                 card2.t + 34.f},
             p.text);
    DrawCandidatePreview(s, card2);
    DrawText(s, s.f_small.Get(), L"候选条与模式气泡跟随上面的主题设置。",
             Box{card2.l + 20.f, 392.f, card2.r - 20.f, 408.f}, p.secondary);
}

void DrawAboutPage(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;

    const Box card1{x0, 96.f, x1, 324.f};
    DrawCard(s, card1);
    DrawText(s, s.f_section.Get(), L"应用信息",
             Box{card1.l + 20.f, card1.t + 14.f, card1.r - 20.f,
                 card1.t + 34.f},
             p.text);
    struct Row {
        const wchar_t* name;
        std::wstring value;
    };
    const Row rows[] = {
        {L"版本", kVersion},
        {L"输入法内核", L"pime-core（WAMR 插件运行时）"},
        {L"输入方案", ChainName(s.settings.chain)},
        {L"日志目录", DataDirectory()},
        {L"许可证", L"Apache License 2.0"},
    };
    float y = card1.t + 46.f;
    for (const Row& row : rows) {
        DrawText(s, s.f_desc.Get(), row.name,
                 Box{card1.l + 20.f, y, card1.l + 140.f, y + 32.f},
                 p.secondary);
        DrawText(s, s.f_value.Get(), row.value,
                 Box{card1.l + 140.f, y, card1.r - 20.f, y + 32.f}, p.text);
        y += 34.f;
    }

    const Box card2{x0, 338.f, x1, 466.f};
    DrawCard(s, card2);
    DrawText(s, s.f_section.Get(), L"输入法状态",
             Box{card2.l + 20.f, card2.t + 14.f, card2.r - 160.f,
                 card2.t + 38.f},
             p.text);
    const Box pill{card2.r - 92.f, card2.t + 16.f, card2.r - 20.f,
                   card2.t + 40.f};
    FillBox(s, pill, 12.f, s.installed ? p.success_soft : p.track);
    DrawText(s, s.f_small.Get(), s.installed ? L"已安装" : L"未安装", pill,
             s.installed ? p.success : p.secondary,
             DWRITE_TEXT_ALIGNMENT_CENTER);
    DrawText(s, s.f_desc.Get(),
             s.installed
                 ? L"已注册为 zh-CN 文本服务，可在系统输入法列表中选择「PIME 拼音」。"
                 : L"尚未安装。安装需要管理员权限，安装后即可在输入法列表里选择。",
             Box{card2.l + 20.f, card2.t + 44.f, card2.r - 20.f,
                 card2.t + 64.f},
             p.secondary);

    DrawButton(s, s.layout.install, L"安装或重新安装", kInstall,
               ButtonKind::Secondary);
    DrawButton(s, s.layout.uninstall, L"卸载", kUninstall, ButtonKind::Danger);
    DrawButton(s, s.layout.activate, L"切换到 PIME", kActivate,
               ButtonKind::Primary);
}

void DrawFooter(AppState& s) {
    const Palette p = Pal(s.system_dark);
    const float x0 = kSidebar + kMargin;
    const float x1 = s.width - kMargin;
    DrawLine(s, x0, s.height - 64.f, x1, s.height - 64.f, p.border, 1.f);

    std::wstring message;
    D2D1_COLOR_F color = p.secondary;
    if (Dirty(s)) {
        message = L"有未保存的更改";
    } else if (!s.status.empty()) {
        message = s.status;
        color = s.status_ok ? p.accent : p.danger;
    }
    DrawText(s, s.f_desc.Get(), message,
             Box{x0 + 14.f, s.height - 52.f, x1 - 260.f, s.height - 18.f},
             color);
    if (!message.empty()) {
        FillEllipse(s, x0 + 4.f, s.height - 35.f, 3.f,
                    Dirty(s) ? p.secondary : color);
    }

    DrawButton(s, s.layout.close, L"关闭", kClose, ButtonKind::Secondary);
    DrawButton(s, s.layout.apply, L"保存并应用", kApply, ButtonKind::Primary);
}

void DrawMenu(AppState& s) {
    if (s.menu == Menu::None || s.layout.menu_items <= 0) return;
    const Palette p = Pal(s.system_dark);
    const Box& pop = s.layout.menu;
    FillBox(s, Box{pop.l + 2.f, pop.t + 6.f, pop.r + 2.f, pop.b + 8.f}, 8.f,
            Rgb(0, 0, 0, 0.16f));
    FillBox(s, pop, 8.f, p.card);
    StrokeBox(s, pop, 8.f, p.border, 1.f);

    if (s.menu == Menu::Chain) {
        const int selected = ChainIndex(s);
        for (size_t i = 0; i < s.chains.size(); ++i) {
            const Box item = ItemBox(s, static_cast<int>(i));
            const bool hot = s.mouse_in && item.Contains(s.mx, s.my);
            if (hot) FillBox(s, item, 6.f, p.accent_soft);
            const bool current = static_cast<int>(i) == selected;
            DrawText(s, s.f_value.Get(), s.chains[i],
                     Box{item.l + 12.f, item.t, item.r - 32.f, item.b},
                     current ? p.accent : p.text);
            if (current) {
                DrawText(s, s.f_badge.Get(), L"✓",
                         Box{item.r - 28.f, item.t, item.r - 8.f, item.b},
                         p.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    } else {
        for (int i = 0; i < 7; ++i) {
            const Box item = ItemBox(s, i);
            const bool hot = s.mouse_in && item.Contains(s.mx, s.my);
            if (hot) FillBox(s, item, 6.f, p.accent_soft);
            const bool current = s.settings.page_size ==
                                 static_cast<uint32_t>(i + 3);
            DrawText(s, s.f_value.Get(),
                     std::to_wstring(i + 3) + L" 个候选",
                     Box{item.l + 12.f, item.t, item.r - 32.f, item.b},
                     current ? p.accent : p.text);
            if (current) {
                DrawText(s, s.f_badge.Get(), L"✓",
                         Box{item.r - 28.f, item.t, item.r - 8.f, item.b},
                         p.accent, DWRITE_TEXT_ALIGNMENT_CENTER);
            }
        }
    }
}

void Draw(AppState& s) {
    const Palette p = Pal(s.system_dark);
    s.rt->Clear(p.bg);
    DrawSidebar(s);
    switch (s.page) {
        case Page::General:
            DrawPageHeader(s, L"常规", L"输入方案、候选数量与标点行为");
            DrawGeneralPage(s);
            break;
        case Page::Appearance:
            DrawPageHeader(s, L"外观", L"主题与候选窗口样式");
            DrawAppearancePage(s);
            break;
        case Page::About:
            DrawPageHeader(s, L"关于", L"安装状态与应用信息");
            DrawAboutPage(s);
            break;
    }
    DrawFooter(s);
    DrawMenu(s);
}

// ---- actions ---------------------------------------------------------------

void Invalidate(AppState& s, HWND window) {
    BuildLayout(s);
    InvalidateRect(window, nullptr, FALSE);
}

void ApplySettings(AppState& s, HWND window) {
    Settings out = s.settings;
    SaveSettings(out);
    s.saved = out;
    NotifyConfigChanged();
    s.status = L"设置已保存并应用";
    s.status_ok = true;
    Invalidate(s, window);
}

void ActivateById(AppState& s, HWND window, int id) {
    switch (id) {
        case kNavGeneral:
        case kNavAppearance:
        case kNavAbout:
            s.page = static_cast<Page>(id - kNavGeneral);
            s.menu = Menu::None;
            s.focus = kNone;
            Invalidate(s, window);
            return;
        case kChain:
            s.menu = s.menu == Menu::Chain ? Menu::None : Menu::Chain;
            Invalidate(s, window);
            return;
        case kPageSize:
            s.menu = s.menu == Menu::PageSize ? Menu::None : Menu::PageSize;
            Invalidate(s, window);
            return;
        case kPunctFull:
            s.settings.punctuation = 1;
            Invalidate(s, window);
            return;
        case kPunctHalf:
            s.settings.punctuation = 0;
            Invalidate(s, window);
            return;
        case kThemeSystem:
        case kThemeLight:
        case kThemeDark:
            s.settings.theme = static_cast<uint32_t>(id - kThemeSystem);
            Invalidate(s, window);
            return;
        case kToggle: {
            s.settings.chinese = s.settings.chinese ? 0 : 1;
            s.toggle.from = s.toggle.value;
            s.toggle.to = s.settings.chinese ? 1.f : 0.f;
            s.toggle.start = GetTickCount64();
            s.toggle.running = true;
            SetTimer(window, 1, 16, nullptr);
            Invalidate(s, window);
            return;
        }
        case kApply:
            ApplySettings(s, window);
            return;
        case kClose:
            DestroyWindow(window);
            return;
        case kInstall:
            s.status = L"已请求安装，请在弹出的窗口中确认。";
            s.status_ok = true;
            RunElevated(L"--install");
            Invalidate(s, window);
            return;
        case kUninstall:
            s.status = L"已请求卸载，请在弹出的窗口中确认。";
            s.status_ok = true;
            RunElevated(L"--uninstall");
            Invalidate(s, window);
            return;
        case kActivate:
            if (ActivateTextService()) {
                s.status = L"已切换到 PIME，可以开始输入。";
                s.status_ok = true;
            } else {
                s.status = L"切换失败：请先安装输入法。";
                s.status_ok = false;
            }
            Invalidate(s, window);
            return;
        default:
            return;
    }
}

void Click(AppState& s, HWND window, float x, float y) {
    if (s.menu != Menu::None) {
        const int hit = HitTest(s, x, y);
        if (hit >= kMenuItem) {
            const int index = hit - kMenuItem;
            if (s.menu == Menu::Chain && index < static_cast<int>(s.chains.size())) {
                s.settings.chain = s.chains[index] + L".cbor";
            } else if (s.menu == Menu::PageSize && index < 7) {
                s.settings.page_size = static_cast<uint32_t>(index + 3);
            }
        }
        s.menu = Menu::None;
        Invalidate(s, window);
        return;
    }
    const int hit = HitTest(s, x, y);
    if (hit == kNone) return;
    s.focus = hit;
    s.focus_visible = false;
    ActivateById(s, window, hit);
}

std::vector<int> FocusOrder(const AppState& s) {
    std::vector<int> order = {kNavGeneral, kNavAppearance, kNavAbout};
    if (s.page == Page::General) {
        order.push_back(kChain);
        order.push_back(kPageSize);
        order.push_back(kPunctFull);
        order.push_back(kPunctHalf);
        order.push_back(kToggle);
    } else if (s.page == Page::Appearance) {
        order.push_back(kThemeSystem);
        order.push_back(kThemeLight);
        order.push_back(kThemeDark);
    } else {
        order.push_back(kInstall);
        order.push_back(kUninstall);
        order.push_back(kActivate);
    }
    order.push_back(kApply);
    order.push_back(kClose);
    return order;
}

// ---- window plumbing -------------------------------------------------------

bool EnsureTarget(AppState& s) {
    if (!s.d2d) return false;
    if (s.rt) return true;
    RECT rc{};
    GetClientRect(s.window, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties();
    props.dpiX = static_cast<float>(s.dpi);
    props.dpiY = static_cast<float>(s.dpi);
    if (FAILED(s.d2d->CreateHwndRenderTarget(
            props,
            D2D1::HwndRenderTargetProperties(
                s.window,
                D2D1::SizeU(static_cast<UINT32>(rc.right),
                            static_cast<UINT32>(rc.bottom))),
            s.rt.GetAddressOf()))) {
        return false;
    }
    s.rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

void UpdateTheme(AppState& s, HWND window) {
    s.system_dark = !SystemUsesLightTheme();
    const BOOL dark = s.system_dark ? TRUE : FALSE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    InvalidateRect(window, nullptr, FALSE);
}

LRESULT CALLBACK WndProc(HWND window, UINT message, WPARAM wparam,
                         LPARAM lparam) {
    auto* s = reinterpret_cast<AppState*>(
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
            self->window = window;
            self->dpi = GetDpiForWindow(window);
            self->system_dark = !SystemUsesLightTheme();
            self->installed = IsInstalled();
            self->settings = LoadSettings();
            self->saved = self->settings;
            self->chains = ListChains();
            const std::wstring current = ChainName(self->settings.chain);
            bool found = false;
            for (const auto& name : self->chains) {
                if (name == current) found = true;
            }
            if (!found) self->chains.push_back(current);
            self->toggle.value = self->settings.chinese ? 1.f : 0.f;

            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                              self->d2d.GetAddressOf());
            DWriteCreateFactory(
                DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                reinterpret_cast<IUnknown**>(self->dwrite.GetAddressOf()));
            CreateFormats(*self);

            RECT rc{};
            GetClientRect(window, &rc);
            self->width = rc.right * 96.f / static_cast<float>(self->dpi);
            self->height = rc.bottom * 96.f / static_cast<float>(self->dpi);
            BuildLayout(*self);
            const BOOL dark = self->system_dark ? TRUE : FALSE;
            DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(window, &paint);
            if (s && s->d2d && s->dwrite) {
                if (EnsureTarget(*s)) {
                    s->rt->BeginDraw();
                    Draw(*s);
                    const HRESULT hr = s->rt->EndDraw();
                    if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
                        s->rt.Reset();
                    }
                }
            }
            EndPaint(window, &paint);
            return 0;
        }
        case WM_SIZE: {
            if (!s) break;
            s->width = LOWORD(lparam) * 96.f / static_cast<float>(s->dpi);
            s->height = HIWORD(lparam) * 96.f / static_cast<float>(s->dpi);
            if (s->rt) {
                s->rt->Resize(D2D1::SizeU(LOWORD(lparam), HIWORD(lparam)));
            }
            BuildLayout(*s);
            return 0;
        }
        case WM_DPICHANGED: {
            if (!s) break;
            s->dpi = HIWORD(wparam);
            s->rt.Reset();
            const RECT* suggested = reinterpret_cast<const RECT*>(lparam);
            s->width = kWidth;
            s->height = kHeight;
            RECT rect{0, 0, static_cast<LONG>(kWidth * s->dpi / 96.f),
                      static_cast<LONG>(kHeight * s->dpi / 96.f)};
            AdjustWindowRectExForDpi(
                &rect,
                static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)),
                FALSE, 0, s->dpi);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                         rect.right - rect.left, rect.bottom - rect.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            BuildLayout(*s);
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            if (!s) break;
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            s->mx = x * 96.f / static_cast<float>(s->dpi);
            s->my = y * 96.f / static_cast<float>(s->dpi);
            if (!s->mouse_in) {
                s->mouse_in = true;
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
                TrackMouseEvent(&track);
            }
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (s) {
                s->mouse_in = false;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONDOWN: {
            if (!s) break;
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            Click(*s, window, x * 96.f / static_cast<float>(s->dpi),
                  y * 96.f / static_cast<float>(s->dpi));
            return 0;
        }
        case WM_SETCURSOR:
            if (s && LOWORD(lparam) == HTCLIENT) {
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(window, &point);
                const float x = point.x * 96.f / static_cast<float>(s->dpi);
                const float y = point.y * 96.f / static_cast<float>(s->dpi);
                SetCursor(LoadCursorW(nullptr, HitTest(*s, x, y) != kNone
                                                   ? IDC_HAND
                                                   : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN: {
            if (!s) break;
            if (wparam == VK_ESCAPE) {
                if (s->menu != Menu::None) {
                    s->menu = Menu::None;
                } else {
                    s->focus = kNone;
                }
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_TAB) {
                std::vector<int> order = FocusOrder(*s);
                int index = -1;
                for (size_t i = 0; i < order.size(); ++i) {
                    if (order[i] == s->focus) index = static_cast<int>(i);
                }
                const bool back = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                const int count = static_cast<int>(order.size());
                index = back ? (index <= 0 ? count - 1 : index - 1)
                             : (index + 1) % count;
                s->focus = order[index];
                s->focus_visible = true;
                InvalidateRect(window, nullptr, FALSE);
                return 0;
            }
            if ((wparam == VK_RETURN || wparam == VK_SPACE) &&
                s->focus != kNone) {
                ActivateById(*s, window, s->focus);
                return 0;
            }
            break;
        }
        case WM_TIMER:
            if (s && wparam == 1) {
                const float progress = static_cast<float>(GetTickCount64() -
                                                          s->toggle.start) /
                                       120.f;
                if (progress >= 1.f) {
                    s->toggle.value = s->toggle.to;
                    s->toggle.running = false;
                    KillTimer(window, 1);
                } else {
                    const float eased = 1.f - (1.f - progress) * (1.f - progress) *
                                                  (1.f - progress);
                    s->toggle.value =
                        s->toggle.from + (s->toggle.to - s->toggle.from) * eased;
                }
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_ACTIVATE:
            if (s && LOWORD(wparam) != WA_INACTIVE) {
                s->installed = IsInstalled();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        case WM_SETTINGCHANGE:
            if (s && lparam &&
                lstrcmpiW(reinterpret_cast<const wchar_t*>(lparam),
                          L"ImmersiveColorSet") == 0) {
                UpdateTheme(*s, window);
            }
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            KillTimer(window, 1);
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
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    AppState state;
    state.instance = instance;

    const DWORD style =
        WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
    const UINT dpi = GetDpiForSystem();
    RECT rect{0, 0, MulDiv(static_cast<int>(kWidth), dpi, 96),
              MulDiv(static_cast<int>(kHeight), dpi, 96)};
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
    HWND window = CreateWindowExW(0, kClassName, kWindowTitle, style,
                                  CW_USEDEFAULT, CW_USEDEFAULT,
                                  rect.right - rect.left,
                                  rect.bottom - rect.top, nullptr, nullptr,
                                  instance, &state);
    if (!window) return 1;

    ShowWindow(window, SW_SHOW);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}

}  // namespace pimewin
