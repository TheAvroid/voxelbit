// ---------------------------------------------------------------------------
// overlay.h -- text on top of the render, without a font file.
//
// WHY GDI AND NOT A BAKED BITMAP FONT. The usual way to get text into a GL 2.1
// context with no extension loader is to embed an 8x8 glyph table and blit it.
// That is a kilobyte of hex literals which are unreadable in the source and
// silently wrong if a single byte is off -- and "silently wrong" for a font
// means a menu that renders as noise. Windows already has a text rasteriser,
// this engine already links gdi32 for the GL context, and asking it for a
// monospaced face costs about sixty lines. The text is real Consolas,
// antialiased, at whatever size the panel wants.
//
// WHY THE PANEL IS ITS OWN TEXTURE, drawn at WINDOW resolution rather than
// composited into the frame. The frame is rendered at --scale and stretched to
// the window, so at scale 0.25 anything drawn into it is magnified four times:
// the menu would be blurrier than the render behind it, which is exactly
// backwards for the one thing on screen you need to read. A second quad at 1:1
// with GL_NEAREST keeps the glyphs crisp no matter how low the render
// resolution goes -- and lets the menu stay legible precisely in the settings
// where it is most needed.
// ---------------------------------------------------------------------------
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace v2 {

// One RGBA panel, rasterised on the CPU and handed to GL as a texture.
class TextPanel {
  public:
    ~TextPanel() { release(); }

    int width() const { return w_; }
    int height() const { return h_; }
    const unsigned char *pixels() const { return rgba_.data(); }
    int lineHeight() const { return lineH_; }

    // Recreate the GDI surface. Cheap enough to call on a size change, far too
    // expensive to call per frame -- CreateFont hits the font cache.
    bool resize(int w, int h, int fontPx) {
        if (w == w_ && h == h_ && fontPx == fontPx_) return true;
        release();
        if (w <= 0 || h <= 0) return false;

        dc_ = CreateCompatibleDC(nullptr);
        if (!dc_) return false;

        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;  // negative: top-down, so row 0 is the top
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        bmp_ = CreateDIBSection(dc_, &bi, DIB_RGB_COLORS, &bits_, nullptr, 0);
        if (!bmp_) { release(); return false; }
        oldBmp_ = (HBITMAP)SelectObject(dc_, bmp_);

        // A FIXED_PITCH face, so a column of values lines up without measuring
        // every string. Consolas if it is there, whatever the mapper finds if
        // it is not -- the request is by family, not by name alone.
        font_ = CreateFontA(fontPx, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            FIXED_PITCH | FF_MODERN, "Consolas");
        bold_ = CreateFontA(fontPx, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                            OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                            FIXED_PITCH | FF_MODERN, "Consolas");
        oldFont_ = (HFONT)SelectObject(dc_, font_);
        SetBkMode(dc_, TRANSPARENT);

        TEXTMETRICA tm = {};
        GetTextMetricsA(dc_, &tm);
        lineH_ = tm.tmHeight + 2;
        charW_ = tm.tmAveCharWidth;

        w_ = w;
        h_ = h;
        fontPx_ = fontPx;
        rgba_.assign(size_t(w) * h * 4, 0);
        return true;
    }

    int charWidth() const { return charW_; }

    // Wipe to the panel background. Text is drawn on black and the coverage is
    // recovered from luminance at composite time, so the clear must be black.
    void clear() {
        if (bits_) std::memset(bits_, 0, size_t(w_) * h_ * 4);
    }

    void text(int x, int y, const std::string &s, COLORREF colour, bool heavy = false) {
        if (!dc_ || s.empty()) return;
        SelectObject(dc_, heavy ? bold_ : font_);
        SetTextColor(dc_, colour);
        TextOutA(dc_, x, y, s.c_str(), int(s.size()));
    }

    // Fill a rectangle in the panel -- used for the selected row's highlight.
    void bar(int x, int y, int w, int h, COLORREF colour) {
        if (!dc_) return;
        RECT r = {x, y, x + w, y + h};
        HBRUSH b = CreateSolidBrush(colour);
        FillRect(dc_, &r, b);
        DeleteObject(b);
    }

    // -----------------------------------------------------------------------
    // Turn the GDI surface into premultiplied-alpha-free RGBA for GL.
    //
    // GDI gives back BGRX with no usable alpha channel -- it does not track
    // coverage for TextOut. So coverage is taken from luminance: the surface
    // was cleared to black, therefore anything bright is glyph. That is exact
    // for antialiased text on black, which is what this panel always is.
    // -----------------------------------------------------------------------
    void compose(unsigned char panelAlpha) {
        if (!bits_) return;
        const unsigned char *src = static_cast<const unsigned char *>(bits_);
        const size_t n = size_t(w_) * h_;
        for (size_t i = 0; i < n; ++i) {
            const unsigned b = src[i * 4 + 0], g = src[i * 4 + 1], r = src[i * 4 + 2];
            const unsigned lum = r > g ? (r > b ? r : b) : (g > b ? g : b);
            // Background of the panel: a near-black blue, so the render behind
            // reads as dimmed rather than tinted.
            const unsigned bgR = 10, bgG = 12, bgB = 16;
            rgba_[i * 4 + 0] = (unsigned char)((bgR * (255 - lum) + r * lum) / 255);
            rgba_[i * 4 + 1] = (unsigned char)((bgG * (255 - lum) + g * lum) / 255);
            rgba_[i * 4 + 2] = (unsigned char)((bgB * (255 - lum) + b * lum) / 255);
            const unsigned a = panelAlpha + (255 - panelAlpha) * lum / 255;
            rgba_[i * 4 + 3] = (unsigned char)(a > 255 ? 255 : a);
        }
    }

    void release() {
        if (dc_) {
            if (oldFont_) SelectObject(dc_, oldFont_);
            if (oldBmp_) SelectObject(dc_, oldBmp_);
        }
        if (font_) { DeleteObject(font_); font_ = nullptr; }
        if (bold_) { DeleteObject(bold_); bold_ = nullptr; }
        if (bmp_) { DeleteObject(bmp_); bmp_ = nullptr; }
        if (dc_) { DeleteDC(dc_); dc_ = nullptr; }
        oldFont_ = nullptr;
        oldBmp_ = nullptr;
        bits_ = nullptr;
        w_ = h_ = 0;
        fontPx_ = 0;
    }

  private:
    HDC dc_ = nullptr;
    HBITMAP bmp_ = nullptr, oldBmp_ = nullptr;
    HFONT font_ = nullptr, bold_ = nullptr, oldFont_ = nullptr;
    void *bits_ = nullptr;
    std::vector<unsigned char> rgba_;
    int w_ = 0, h_ = 0, fontPx_ = 0, lineH_ = 16, charW_ = 8;
};

}  // namespace v2
