// Candidate strip / mode badge, drawn with Direct2D + DirectWrite into a
// layered window. No UI toolkit: one Win32 window, one DIB, one UpdateLayeredWindow.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <wrl/client.h>

namespace pimewin {

struct CandidateItem {
    std::wstring text;
    std::wstring comment;
    std::wstring label;
};

class CandidateWindow {
public:
    struct Delegate {
        virtual void OnCandidatePicked(int slot) = 0;
        virtual void OnPageRequested(int direction) = 0;  // -1 previous, +1 next
        virtual void OnModeToggled() = 0;
        virtual ~Delegate() = default;
    };

    CandidateWindow();
    ~CandidateWindow();

    bool Create(Delegate* delegate);
    void Destroy();

    void SetDark(bool dark);
    // 0 = horizontal strip, 1 = vertical list; font is the candidate text
    // size in DIP. Applied to the next paint, and immediately when visible.
    void SetStyle(bool vertical, uint32_t font_dip);

    // `caret` is the composition caret in physical screen pixels. `page` is
    // the zero-based page shown; `has_prev`/`has_next` drive the chevrons.
    void ShowCandidates(const std::vector<CandidateItem>& items, int page,
                        bool has_prev, bool has_next, const RECT& caret);
    void ShowModeBadge(bool chinese, const RECT& caret);
    void Hide();

    HWND hwnd() const { return hwnd_; }
    bool visible() const { return visible_; }
    bool showing_badge() const { return visible_ && badge_; }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam,
                                    LPARAM lparam);

private:
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam,
                          LPARAM lparam);

    void CreateFormats();
    void LayoutCandidates();
    void Paint();
    bool EnsureTargets(float width_dip, float height_dip);
    float Scale() const;
    int HitTest(int x, int y) const;  // -2 prev, -1 next, >=0 slot, -100 chip
    void UpdateHover(int hit);
    void ShowAt(const RECT& caret);

    struct ItemBox {
        D2D1_RECT_F rect{};
        CandidateItem item;
    };
    struct HitBox {
        D2D1_RECT_F rect{};
        int kind = -100;  // -100 none, -2 prev page, -1 next page
    };

    HWND hwnd_ = nullptr;
    Delegate* delegate_ = nullptr;
    bool visible_ = false;
    bool dark_ = false;
    bool badge_ = false;
    bool chinese_ = true;
    bool vertical_ = false;
    uint32_t font_dip_ = 15;
    float row_height_ = 38.0f;
    int page_ = 0;
    bool has_prev_ = false;
    bool has_next_ = false;
    UINT dpi_ = 96;
    RECT caret_{};
    std::vector<CandidateItem> raw_items_;
    std::vector<ItemBox> items_;
    std::vector<HitBox> hits_;
    D2D1_RECT_F chip_rect_{};
    float card_width_ = 0;
    float card_height_ = 0;
    float window_width_ = 0;
    float window_height_ = 0;
    int origin_x_ = 0;
    int origin_y_ = 0;
    int hot_ = -100;
    UINT_PTR badge_timer_ = 0;

    Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
    Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
    Microsoft::WRL::ComPtr<IWICImagingFactory> wic_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> text_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> label_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> comment_format_;
    Microsoft::WRL::ComPtr<IDWriteTextFormat> chip_format_;

    Microsoft::WRL::ComPtr<IWICBitmap> bitmap_;
    Microsoft::WRL::ComPtr<ID2D1RenderTarget> target_;
    HDC memory_dc_ = nullptr;
    HBITMAP dib_ = nullptr;
    HGDIOBJ old_bitmap_ = nullptr;
    void* dib_bits_ = nullptr;
    UINT target_width_ = 0;
    UINT target_height_ = 0;
    bool com_initialized_ = false;
};

}  // namespace pimewin
