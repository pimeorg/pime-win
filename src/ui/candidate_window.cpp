#include "ui/candidate_window.h"

#include <d2d1helper.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "common/log.h"
#include "common/win_util.h"

using Microsoft::WRL::ComPtr;

namespace pimewin {
namespace {

constexpr wchar_t kClassName[] = L"PimeCandidateWindow";
constexpr UINT_PTR kBadgeTimer = 1;
constexpr float kCardHeight = 38.0f;
constexpr float kCardRadius = 10.0f;
constexpr float kShadowMargin = 12.0f;
constexpr float kChipWidth = 32.0f;
constexpr float kChipHeight = 26.0f;
constexpr float kArrowWidth = 22.0f;

struct Palette {
    D2D1_COLOR_F card;
    D2D1_COLOR_F border;
    D2D1_COLOR_F text;
    D2D1_COLOR_F secondary;
    D2D1_COLOR_F accent;
    D2D1_COLOR_F accent_text;
    D2D1_COLOR_F hover;
};

Palette PaletteFor(bool dark) {
    Palette p{};
    if (dark) {
        p.card = D2D1::ColorF(0x2A2A2E, 0.98f);
        p.border = D2D1::ColorF(0xFFFFFF, 0.10f);
        p.text = D2D1::ColorF(0xF2F2F5);
        p.secondary = D2D1::ColorF(0xA0A0A8);
        p.accent = D2D1::ColorF(0x4C8DFF);
        p.accent_text = D2D1::ColorF(0x101216);
        p.hover = D2D1::ColorF(0xFFFFFF, 0.08f);
    } else {
        p.card = D2D1::ColorF(0xFEFEFF);
        p.border = D2D1::ColorF(0x000000, 0.09f);
        p.text = D2D1::ColorF(0x1B1B1F);
        p.secondary = D2D1::ColorF(0x7A7A82);
        p.accent = D2D1::ColorF(0x2F6BFF);
        p.accent_text = D2D1::ColorF(0xFFFFFF);
        p.hover = D2D1::ColorF(0x000000, 0.05f);
    }
    return p;
}

void RegisterWindowClass(HINSTANCE instance) {
    static std::once_flag once;
    std::call_once(once, [instance] {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = CandidateWindow::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) {
            WriteLog(LogLevel::Error, "ui",
                     "RegisterClassEx failed " +
                         std::to_string(static_cast<long>(GetLastError())));
        }
    });
}

ComPtr<IDWriteTextFormat> MakeFormat(IDWriteFactory* factory,
                                     const wchar_t* family, float size,
                                     DWRITE_TEXT_ALIGNMENT alignment) {
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(factory->CreateTextFormat(family, nullptr,
                                         DWRITE_FONT_WEIGHT_NORMAL,
                                         DWRITE_FONT_STYLE_NORMAL,
                                         DWRITE_FONT_STRETCH_NORMAL, size,
                                         L"zh-CN", &format))) {
        factory->CreateTextFormat(L"Segoe UI", nullptr,
                                  DWRITE_FONT_WEIGHT_NORMAL,
                                  DWRITE_FONT_STYLE_NORMAL,
                                  DWRITE_FONT_STRETCH_NORMAL, size, L"zh-CN",
                                  &format);
    }
    if (format) {
        format->SetTextAlignment(alignment);
        format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return format;
}

float MeasureText(IDWriteFactory* factory, IDWriteTextFormat* format,
                  const std::wstring& text) {
    if (text.empty() || !format) return 0.0f;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(),
                                         static_cast<UINT32>(text.size()),
                                         format, 1024.0f, 64.0f, &layout))) {
        return 0.0f;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout->GetMetrics(&metrics))) return 0.0f;
    return metrics.widthIncludingTrailingWhitespace;
}

}  // namespace

CandidateWindow::CandidateWindow() = default;

CandidateWindow::~CandidateWindow() { Destroy(); }

bool CandidateWindow::Create(Delegate* delegate) {
    delegate_ = delegate;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    com_initialized_ = SUCCEEDED(com);

    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                   d2d_.GetAddressOf());
    if (FAILED(hr)) {
        WriteLog(LogLevel::Error, "ui",
                 "D2D1CreateFactory " + std::to_string(static_cast<long>(hr)));
        return false;
    }
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(
                                 dwrite_.GetAddressOf()));
    if (FAILED(hr)) {
        WriteLog(LogLevel::Error, "ui",
                 "DWriteCreateFactory " + std::to_string(static_cast<long>(hr)));
        return false;
    }
    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(wic_.GetAddressOf()));
    if (FAILED(hr)) {
        WriteLog(LogLevel::Error, "ui",
                 "WIC factory " + std::to_string(static_cast<long>(hr)));
        return false;
    }

    text_format_ = MakeFormat(dwrite_.Get(), L"Microsoft YaHei UI", 15.0f,
                              DWRITE_TEXT_ALIGNMENT_LEADING);
    label_format_ = MakeFormat(dwrite_.Get(), L"Segoe UI", 10.0f,
                               DWRITE_TEXT_ALIGNMENT_CENTER);
    comment_format_ = MakeFormat(dwrite_.Get(), L"Microsoft YaHei UI", 11.0f,
                                 DWRITE_TEXT_ALIGNMENT_LEADING);
    chip_format_ = MakeFormat(dwrite_.Get(), L"Microsoft YaHei UI", 13.0f,
                              DWRITE_TEXT_ALIGNMENT_CENTER);

    HINSTANCE instance = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&CandidateWindow::WndProc),
                       &instance);
    RegisterWindowClass(instance);
    hwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        kClassName, L"PIME", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance,
        this);
    if (!hwnd_) {
        WriteLog(LogLevel::Error, "ui",
                 "candidate window CreateWindowEx " +
                     std::to_string(static_cast<long>(GetLastError())));
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    dpi_ = SystemDpiForWindow(hwnd_);
    return true;
}

void CandidateWindow::Destroy() {
    if (badge_timer_ && hwnd_) KillTimer(hwnd_, kBadgeTimer);
    badge_timer_ = 0;
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (old_bitmap_ && memory_dc_) SelectObject(memory_dc_, old_bitmap_);
    old_bitmap_ = nullptr;
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    if (memory_dc_) {
        DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }
    dib_bits_ = nullptr;
    target_.Reset();
    bitmap_.Reset();
    target_width_ = target_height_ = 0;
    text_format_.Reset();
    label_format_.Reset();
    comment_format_.Reset();
    chip_format_.Reset();
    wic_.Reset();
    dwrite_.Reset();
    d2d_.Reset();
    if (com_initialized_) {
        CoUninitialize();
        com_initialized_ = false;
    }
}

float CandidateWindow::Scale() const {
    return static_cast<float>(dpi_) / 96.0f;
}

void CandidateWindow::SetDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    if (visible_) Paint();
}

void CandidateWindow::ShowCandidates(const std::vector<CandidateItem>& items,
                                     int page, bool has_prev, bool has_next,
                                     const RECT& caret) {
    if (!hwnd_) return;
    badge_ = false;
    page_ = page;
    has_prev_ = has_prev;
    has_next_ = has_next;
    caret_ = caret;
    if (badge_timer_) {
        KillTimer(hwnd_, kBadgeTimer);
        badge_timer_ = 0;
    }

    items_.clear();
    hits_.clear();
    const float top = kShadowMargin;
    const float bottom = kShadowMargin + kCardHeight;
    float x = kShadowMargin + 8.0f;

    for (const auto& item : items) {
        const float label_width =
            item.label.empty()
                ? 0.0f
                : std::max(8.0f, MeasureText(dwrite_.Get(), label_format_.Get(),
                                            item.label));
        const float text_width =
            MeasureText(dwrite_.Get(), text_format_.Get(), item.text);
        const float comment_width =
            MeasureText(dwrite_.Get(), comment_format_.Get(), item.comment);
        float width = 5.0f + label_width + (item.label.empty() ? 0.0f : 3.0f) +
                      text_width + 5.0f;
        if (!item.comment.empty()) width += 6.0f + comment_width;
        ItemBox box;
        box.rect = D2D1::RectF(x, top + 3.0f, x + width, bottom - 3.0f);
        box.item = item;
        items_.push_back(std::move(box));
        x += width + 2.0f;
    }

    if (!items.empty()) {
        x += 4.0f;
        if (has_prev_) {
            HitBox hit;
            hit.rect = D2D1::RectF(x, top + 2.0f, x + kArrowWidth, bottom - 2.0f);
            hit.kind = -2;
            hits_.push_back(hit);
            x += kArrowWidth;
        }
        if (has_next_) {
            HitBox hit;
            hit.rect = D2D1::RectF(x, top + 2.0f, x + kArrowWidth, bottom - 2.0f);
            hit.kind = -1;
            hits_.push_back(hit);
            x += kArrowWidth;
        }
    }

    card_width_ = x + 8.0f - kShadowMargin;
    card_height_ = kCardHeight;
    window_width_ = card_width_ + kShadowMargin * 2.0f;
    window_height_ = card_height_ + kShadowMargin * 2.0f;
    ShowAt(caret);
}

void CandidateWindow::ShowModeBadge(bool chinese, const RECT& caret) {
    if (!hwnd_) return;
    badge_ = true;
    chinese_ = chinese;
    items_.clear();
    hits_.clear();
    caret_ = caret;

    const float top = kShadowMargin;
    const float x = kShadowMargin + 8.0f;
    chip_rect_ = D2D1::RectF(x, top + (kCardHeight - kChipHeight) / 2.0f,
                             x + kChipWidth,
                             top + (kCardHeight + kChipHeight) / 2.0f);
    card_width_ = kChipWidth + 16.0f;
    card_height_ = kCardHeight;
    window_width_ = card_width_ + kShadowMargin * 2.0f;
    window_height_ = card_height_ + kShadowMargin * 2.0f;
    ShowAt(caret);
    badge_timer_ = SetTimer(hwnd_, kBadgeTimer, 900, nullptr);
}

void CandidateWindow::ShowAt(const RECT& caret) {
    dpi_ = hwnd_ ? SystemDpiForWindow(hwnd_) : 96;
    const float scale = Scale();
    const UINT width = static_cast<UINT>(std::ceil(window_width_ * scale));
    const UINT height = static_cast<UINT>(std::ceil(window_height_ * scale));

    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromRect(&caret, MONITOR_DEFAULTTONEAREST), &monitor);

    const float gap = 6.0f * scale;
    float x = static_cast<float>(caret.left);
    float y = static_cast<float>(caret.bottom) + gap;
    if (y + height > monitor.rcWork.bottom) {
        y = static_cast<float>(caret.top) - static_cast<float>(height) - gap;
    }
    if (x + width > monitor.rcWork.right) {
        x = static_cast<float>(monitor.rcWork.right - width);
    }
    if (x < monitor.rcWork.left) x = static_cast<float>(monitor.rcWork.left);
    if (y < monitor.rcWork.top) y = static_cast<float>(monitor.rcWork.top);

    origin_x_ = static_cast<int>(std::lround(x));
    origin_y_ = static_cast<int>(std::lround(y));
    Paint();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    visible_ = true;
}

bool CandidateWindow::EnsureTargets(float width_dip, float height_dip) {
    const float scale = Scale();
    const UINT width = static_cast<UINT>(std::max(1.0f, std::ceil(width_dip * scale)));
    const UINT height = static_cast<UINT>(std::max(1.0f, std::ceil(height_dip * scale)));
    if (target_ && width == target_width_ && height == target_height_) {
        target_->SetDpi(static_cast<float>(dpi_), static_cast<float>(dpi_));
        return true;
    }

    target_.Reset();
    bitmap_.Reset();
    if (memory_dc_) {
        if (old_bitmap_) SelectObject(memory_dc_, old_bitmap_);
        old_bitmap_ = nullptr;
        DeleteDC(memory_dc_);
        memory_dc_ = nullptr;
    }
    if (dib_) {
        DeleteObject(dib_);
        dib_ = nullptr;
    }
    dib_bits_ = nullptr;

    if (FAILED(wic_->CreateBitmap(
            width, height, GUID_WICPixelFormat32bppPBGRA,
            WICBitmapCacheOnLoad, bitmap_.GetAddressOf()))) {
        return false;
    }
    D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                          D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<float>(dpi_), static_cast<float>(dpi_));
    if (FAILED(d2d_->CreateWicBitmapRenderTarget(bitmap_.Get(), properties,
                                                 target_.GetAddressOf()))) {
        return false;
    }
    target_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = -static_cast<LONG>(height);  // top-down
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    memory_dc_ = CreateCompatibleDC(screen);
    ReleaseDC(nullptr, screen);
    dib_ = CreateDIBSection(memory_dc_, &info, DIB_RGB_COLORS, &dib_bits_,
                            nullptr, 0);
    if (!dib_ || !dib_bits_) return false;
    old_bitmap_ = SelectObject(memory_dc_, dib_);
    target_width_ = width;
    target_height_ = height;
    return true;
}

void CandidateWindow::Paint() {
    if (!hwnd_) return;
    if (!EnsureTargets(window_width_, window_height_)) return;

    const Palette palette = PaletteFor(dark_);
    const float top = kShadowMargin;
    const float bottom = kShadowMargin + card_height_;
    const D2D1_ROUNDED_RECT card = D2D1::RoundedRect(
        D2D1::RectF(kShadowMargin, top, kShadowMargin + card_width_, bottom),
        kCardRadius, kCardRadius);

    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(D2D1::ColorF(0, 0.0f));

    ComPtr<ID2D1SolidColorBrush> brush;
    target_->CreateSolidColorBrush(D2D1::ColorF(0x000000), &brush);

    for (int i = 7; i >= 1; --i) {
        const float grow = static_cast<float>(i) * 1.7f;
        brush->SetColor(D2D1::ColorF(0x000000, 0.016f));
        D2D1_ROUNDED_RECT shadow = card;
        shadow.rect.left -= grow;
        shadow.rect.right += grow;
        shadow.rect.top -= grow * 0.7f;
        shadow.rect.bottom += grow;
        shadow.radiusX += grow * 0.4f;
        shadow.radiusY += grow * 0.4f;
        target_->FillRoundedRectangle(shadow, brush.Get());
    }

    brush->SetColor(palette.card);
    target_->FillRoundedRectangle(card, brush.Get());
    brush->SetColor(palette.border);
    target_->DrawRoundedRectangle(card, brush.Get(), 1.0f);

    // Mode chip (badge only).
    if (badge_) {
        D2D1_ROUNDED_RECT chip =
            D2D1::RoundedRect(chip_rect_, 7.0f, 7.0f);
        if (chinese_) {
            brush->SetColor(palette.accent);
            target_->FillRoundedRectangle(chip, brush.Get());
        } else {
            brush->SetColor(palette.hover);
            target_->FillRoundedRectangle(chip, brush.Get());
            brush->SetColor(palette.border);
            target_->DrawRoundedRectangle(chip, brush.Get(), 1.0f);
        }
    }

    auto draw_text = [&](const std::wstring& text, IDWriteTextFormat* format,
                         D2D1_RECT_F box, const D2D1_COLOR_F& color) {
        if (text.empty() || !format) return;
        ComPtr<IDWriteTextLayout> layout;
        if (FAILED(dwrite_->CreateTextLayout(
                text.c_str(), static_cast<UINT32>(text.size()), format,
                box.right - box.left, box.bottom - box.top, &layout))) {
            return;
        }
        brush->SetColor(color);
        target_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(),
                                brush.Get());
    };

    if (badge_) {
        draw_text(chinese_ ? L"中" : L"英", chip_format_.Get(), chip_rect_,
                  chinese_ ? palette.accent_text : palette.accent);
    }

    for (size_t i = 0; i < items_.size(); ++i) {
        const ItemBox& box = items_[i];
        if (hot_ == static_cast<int>(i)) {
            brush->SetColor(palette.hover);
            target_->FillRoundedRectangle(
                D2D1::RoundedRect(box.rect, 6.0f, 6.0f), brush.Get());
        }
        float x = box.rect.left + 5.0f;
        const float text_width =
            MeasureText(dwrite_.Get(), text_format_.Get(), box.item.text);
        const float label_width =
            box.item.label.empty()
                ? 0.0f
                : std::max(8.0f, MeasureText(dwrite_.Get(), label_format_.Get(),
                                            box.item.label));
        if (!box.item.label.empty()) {
            draw_text(box.item.label, label_format_.Get(),
                      D2D1::RectF(x, box.rect.top, x + label_width,
                                  box.rect.bottom),
                      palette.secondary);
            x += label_width + 3.0f;
        }
        const float row_top = box.rect.top;
        const float row_bottom = box.rect.bottom;
        ComPtr<IDWriteTextLayout> text_layout;
        if (SUCCEEDED(dwrite_->CreateTextLayout(
                box.item.text.c_str(),
                static_cast<UINT32>(box.item.text.size()), text_format_.Get(),
                text_width + 2.0f, row_bottom - row_top, &text_layout))) {
            brush->SetColor(palette.text);
            target_->DrawTextLayout(D2D1::Point2F(x, row_top),
                                    text_layout.Get(), brush.Get());
        }
        if (!box.item.comment.empty()) {
            draw_text(box.item.comment, comment_format_.Get(),
                      D2D1::RectF(x + text_width + 6.0f, box.rect.top,
                                  box.rect.right, box.rect.bottom),
                      palette.secondary);
        }
    }

    // Divider + page chevrons.
    if (!hits_.empty()) {
        const float divider_x = hits_.front().rect.left - 4.0f;
        brush->SetColor(palette.border);
        target_->DrawLine(D2D1::Point2F(divider_x, top + 8.0f),
                          D2D1::Point2F(divider_x, bottom - 8.0f), brush.Get(),
                          1.0f);
        for (const HitBox& hit : hits_) {
            const bool hot = hot_ == hit.kind;
            brush->SetColor(hot ? palette.accent : palette.secondary);
            const float cx = (hit.rect.left + hit.rect.right) / 2.0f;
            const float cy = (hit.rect.top + hit.rect.bottom) / 2.0f;
            const float sign = hit.kind == -2 ? -1.0f : 1.0f;
            const float w = 3.2f;
            target_->DrawLine(
                D2D1::Point2F(cx - w * sign, cy - 4.5f),
                D2D1::Point2F(cx + w * sign, cy), brush.Get(), 1.6f);
            target_->DrawLine(
                D2D1::Point2F(cx + w * sign, cy),
                D2D1::Point2F(cx - w * sign, cy + 4.5f), brush.Get(), 1.6f);
        }
    }

    const HRESULT hr = target_->EndDraw();
    if (FAILED(hr)) {
        target_.Reset();
        return;
    }

    // Copy the WIC surface into the layered window's DIB.
    WICRect lock_rect{0, 0, static_cast<INT>(target_width_),
                      static_cast<INT>(target_height_)};
    ComPtr<IWICBitmapLock> lock;
    if (FAILED(bitmap_->Lock(&lock_rect, WICBitmapLockRead,
                             lock.GetAddressOf()))) {
        return;
    }
    UINT stride = 0;
    UINT size = 0;
    BYTE* data = nullptr;
    if (FAILED(lock->GetStride(&stride)) ||
        FAILED(lock->GetDataPointer(&size, &data))) {
        return;
    }
    const size_t row_bytes = static_cast<size_t>(target_width_) * 4;
    auto* dst = static_cast<uint8_t*>(dib_bits_);
    for (UINT y = 0; y < target_height_; ++y) {
        std::memcpy(dst + y * row_bytes, data + static_cast<size_t>(y) * stride,
                    std::min<size_t>(row_bytes, stride));
    }

    POINT destination{origin_x_, origin_y_};
    SIZE window_size{static_cast<LONG>(target_width_),
                     static_cast<LONG>(target_height_)};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd_, nullptr, &destination, &window_size, memory_dc_,
                        &source, 0, &blend, ULW_ALPHA);
}

int CandidateWindow::HitTest(int x, int y) const {
    const float scale = Scale();
    const D2D1_POINT_2F point{static_cast<float>(x) / scale,
                              static_cast<float>(y) / scale};
    auto inside = [&](const D2D1_RECT_F& rect) {
        return point.x >= rect.left && point.x <= rect.right &&
               point.y >= rect.top && point.y <= rect.bottom;
    };
    if (badge_ && inside(chip_rect_)) return -100;
    for (const HitBox& hit : hits_) {
        if (inside(hit.rect)) return hit.kind;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (inside(items_[i].rect)) return static_cast<int>(i);
    }
    return -1000;
}

void CandidateWindow::UpdateHover(int hit) {
    if (hot_ == hit) return;
    hot_ = hit;
    Paint();
}

void CandidateWindow::Hide() {
    if (!hwnd_) return;
    if (badge_timer_) {
        KillTimer(hwnd_, kBadgeTimer);
        badge_timer_ = 0;
    }
    visible_ = false;
    badge_ = false;
    hot_ = -1000;
    ShowWindow(hwnd_, SW_HIDE);
}

LRESULT CALLBACK CandidateWindow::WndProc(HWND hwnd, UINT message,
                                          WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* self =
        reinterpret_cast<CandidateWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) return self->HandleMessage(hwnd, message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CandidateWindow::HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                                       LPARAM lparam) {
    switch (message) {
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        case WM_MOUSEMOVE: {
            const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            UpdateHover(hit);
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            UpdateHover(-1000);
            return 0;
        case WM_SETCURSOR: {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(hwnd_, &point);
            const int hit = HitTest(point.x, point.y);
            const bool hand = hit >= -2 || hit == -100;
            SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
            return TRUE;
        }
        case WM_LBUTTONDOWN:
            return 0;
        case WM_LBUTTONUP: {
            const int hit = HitTest(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
            if (hit >= 0 && delegate_) delegate_->OnCandidatePicked(hit);
            else if (hit == -2 && delegate_) delegate_->OnPageRequested(-1);
            else if (hit == -1 && delegate_) delegate_->OnPageRequested(1);
            else if (hit == -100 && delegate_ && !badge_) {
                delegate_->OnModeToggled();
            }
            return 0;
        }
        case WM_TIMER:
            if (wparam == kBadgeTimer) {
                KillTimer(hwnd_, kBadgeTimer);
                badge_timer_ = 0;
                Hide();
            }
            return 0;
        case WM_DPICHANGED: {
            dpi_ = HIWORD(wparam);
            if (visible_) ShowAt(caret_);
            return 0;
        }
        default:
            break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

}  // namespace pimewin
