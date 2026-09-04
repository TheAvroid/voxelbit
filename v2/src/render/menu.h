// ---------------------------------------------------------------------------
// menu.h -- the settings menu, on Y.
//
// Every knob here is also a launch flag, and that is deliberate: the flags were
// unusable as a way to FIND a setting, because choosing between them meant
// knowing the answer already. Quitting, editing a command line and reloading a
// 66-million-triangle scene to try one number is not a way to learn what the
// number does. In here the change lands on the next frame and the fps counter
// two lines above it reacts, so the trade is visible while you make it.
//
// THE PRESETS ARE MEASURED, not guessed. Each row's numbers came off this
// machine on this scene, and the ordering reflects what they cost. The single
// most useful thing they encode is that PIXELS COST AND BOUNCES ARE NEARLY
// FREE -- going from three bounces to eight costs about three percent, while
// halving the render scale nearly doubles the frame rate. Left to a command
// line, the instinct is always to cut bounces first, which gives up most of the
// lighting for almost none of the speed.
//
// Rows that would need the scene rebuilt -- --trees, --extent, --seed, --pines
// -- are deliberately absent. A menu that silently does nothing is worse than
// one that does not offer the control.
// ---------------------------------------------------------------------------
#pragma once

#include <GLFW/glfw3.h>

#include <cstdio>
#include <cstring>
#include <string>

#include "../scene/daynight.h"
#include "overlay.h"

namespace v2 {

// What the menu is allowed to change. Pointers rather than a copy so an edit is
// live on the next frame with nothing to write back.
struct MenuTarget {
    float *scale = nullptr;
    int *depth = nullptr;
    int *movingDepth = nullptr;
    float *exposure = nullptr;
    float *speed = nullptr;
    float *fov = nullptr;
    DayNight *clock = nullptr;
};

// Live figures the menu reports back. Read-only.
struct MenuStatus {
    float fps = 0.0f;
    float traceMs = 0.0f;
    float resolveMs = 0.0f;
    int renderW = 0, renderH = 0;
    int outW = 0, outH = 0;
    unsigned samples = 0;
    int trees = 0;
};

struct Preset {
    const char *name;
    float scale;
    int depth;
    const char *note;
};

// Measured on an RTX 4070 at a maximised 3820x1990 window, 414 trees.
inline const Preset *presets(int *count) {
    static const Preset kPresets[] = {
        {"Max FPS",  0.25f,  3, "~720 fps  noisy until it settles"},
        {"Fast",     0.35f,  8, "~374 fps  full lighting"},
        {"Balanced", 0.50f,  6, "~206 fps"},
        {"Sharp",    0.70f,  8, "~120 fps"},
        {"Native",   1.00f, 12, "~60 fps   every pixel traced"},
    };
    *count = int(sizeof(kPresets) / sizeof(kPresets[0]));
    return kPresets;
}

// Column positions, in characters. draw() and hitTest() both work from these,
// which is the only reason a click can be trusted to land on the thing that was
// drawn -- two independent sets of numbers would drift the first time a row was
// added and there would be nothing on screen to show it.
namespace layout {
constexpr int kPad = 16;
constexpr int kLabelCol = 2;
constexpr int kValueCol = 20;
constexpr int kDecCol = 62;
constexpr int kIncCol = 66;
constexpr int kCols = 70;
}  // namespace layout

class SettingsMenu {
  public:
    bool open = false;

    // Set when an edit made the accumulated samples meaningless. The viewer
    // clears the film on it; exposure and fly speed deliberately do not raise
    // it, because neither changes what was traced.
    bool invalidated = false;

    // Raised by the "Bake as default" row, cleared by the viewer once it has
    // written the file. bakeStatus is what the row shows afterwards.
    bool bakeRequested = false;
    std::string bakeStatus;

    void toggle() { open = !open; }
    void close() { open = false; }

    void bind(const MenuTarget &t) { t_ = t; }

    // ------------------------------------------------------------------ input
    // Returns true if the key was consumed, so the viewer's own bindings do not
    // also fire -- the arrows in particular, which move the sun when the menu
    // is closed and choose a row when it is open.
    bool key(int glfwKey) {
        if (!open) return false;

        switch (glfwKey) {
            case GLFW_KEY_UP:    row_ = (row_ + rowCount() - 1) % rowCount(); return true;
            case GLFW_KEY_DOWN:  row_ = (row_ + 1) % rowCount(); return true;
            case GLFW_KEY_LEFT:  adjust(-1); return true;
            case GLFW_KEY_RIGHT: adjust(+1); return true;
            case GLFW_KEY_ENTER: if (row_ == 0) applyPreset(preset_); return true;
            default: break;
        }
        // 1..7 jump straight to a preset, wherever the cursor is.
        int n = 0;
        presets(&n);
        if (glfwKey >= GLFW_KEY_1 && glfwKey < GLFW_KEY_1 + n) {
            preset_ = glfwKey - GLFW_KEY_1;
            applyPreset(preset_);
            row_ = 0;
            return true;
        }
        return false;
    }

    // ------------------------------------------------------------------- draw
    void draw(TextPanel &panel, const MenuStatus &st) {
        const int pad = layout::kPad;
        const int lh = panel.lineHeight();
        const int cw = panel.charWidth();
        const COLORREF kDim = RGB(150, 158, 170);
        const COLORREF kText = RGB(226, 232, 240);
        const COLORREF kHot = RGB(126, 220, 255);
        const COLORREF kNote = RGB(128, 140, 155);
        const COLORREF kTitle = RGB(255, 214, 120);

        panel.clear();
        int y = pad;

        panel.text(pad, y, "v2  settings", kTitle, true);
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%.0f fps", st.fps);
            panel.text(panel.width() - pad - int(std::strlen(buf)) * panel.charWidth() - 4, y, buf,
                       kTitle, true);
        }
        y += lh + 2;

        {
            char buf[192];
            std::snprintf(buf, sizeof(buf), "trace %.2f ms   resolve %.2f ms   %d spp",
                          st.traceMs, st.resolveMs, st.samples);
            panel.text(pad, y, buf, kNote);
            y += lh;
            std::snprintf(buf, sizeof(buf), "tracing %dx%d   %d chunks resident",
                          st.renderW, st.renderH, st.trees);
            panel.text(pad, y, buf, kNote);
        }
        // Not "y += lh" from here -- the rows are anchored at the same computed
        // top the hit test uses, so a click cannot land a row away from what it
        // looks like it is over.
        y = rowsTop(lh);

        for (int i = 0; i < rowCount(); ++i) {
            const bool sel = (i == row_);
            if (sel) panel.bar(pad - 6, y - 2, panel.width() - 2 * pad + 12, lh + 2,
                               RGB(38, 52, 70));
            panel.text(pad, y, sel ? ">" : " ", kHot, true);
            panel.text(pad + layout::kLabelCol * cw, y, label(i), sel ? kText : kDim);
            panel.text(pad + layout::kValueCol * cw, y, value(i), sel ? kHot : kText, sel);
            // The steppers are drawn on every row, not just the selected one:
            // they are the click target, and a target that only appears once
            // you have already hit it is not a target.
            panel.text(pad + layout::kDecCol * cw, y, "<", sel ? kHot : kDim, sel);
            panel.text(pad + layout::kIncCol * cw, y, ">", sel ? kHot : kDim, sel);
            y += lh;
        }

        y += 6;
        panel.text(pad, y, "hover or up/down  choose    click < >, wheel, left/right  change",
                   kNote);
        y += lh;
        panel.text(pad, y, "1-5  preset                 Y or ESC  close", kNote);

        panel.compose(214);
    }

    // Sized from the content rather than guessed: 68 columns is the longest
    // preset line ("Upscale" plus its note), and the height is the exact sum of
    // what draw() lays out. An oversized panel is not harmless -- it dims a
    // band of the render for no reason.
    int panelWidth(const TextPanel &p) const { return layout::kCols * p.charWidth() + 32; }
    int panelHeight(const TextPanel &p) const { return (rowCount() + 5) * p.lineHeight() + 48; }

    // ------------------------------------------------------------------ mouse
    // All three take PANEL-LOCAL pixels; the viewer does the window-to-panel
    // conversion because only it knows where the panel was drawn.
    void hover(const TextPanel &p, int px, int py) {
        int r = 0;
        if (hitTest(p, px, py, &r) != Hit::None) row_ = r;
    }

    // True if the click was the menu's. A false here is the viewer's cue that
    // the user clicked outside the panel and meant something else by it.
    bool click(const TextPanel &p, int px, int py) {
        int r = 0;
        const Hit h = hitTest(p, px, py, &r);
        if (h == Hit::None) return false;
        row_ = r;
        if (h == Hit::Dec) adjust(-1);
        else if (h == Hit::Inc) adjust(+1);
        return true;
    }

    bool wheel(const TextPanel &p, int px, int py, int dir) {
        int r = 0;
        if (hitTest(p, px, py, &r) == Hit::None) return false;
        row_ = r;
        adjust(dir);
        return true;
    }

  private:
    MenuTarget t_;
    int row_ = 0;
    int preset_ = 1;  // "Fast" -- the one worth landing on by default

    // Which preset, if any, the live settings currently ARE.
    //
    // Without this the row showed whatever preset the cursor last sat on, which
    // on startup meant advertising "Fast" while the renderer was in fact on
    // whatever --scale said. A menu that misreports the current
    // state is worse than one that has no preset row at all.
    int matchingPreset() const {
        int n = 0;
        const Preset *p = presets(&n);
        for (int i = 0; i < n; ++i)
            if (fabsf(p[i].scale - *t_.scale) < 0.005f && p[i].depth == *t_.depth)
                return i;
        return -1;
    }

    static int rowCount() { return 10; }

    // The y of the first row, from the header block draw() lays out above it.
    static int rowsTop(int lh) { return layout::kPad + (lh + 2) + 2 * lh + 8; }

    enum class Hit { None, Row, Dec, Inc };

    Hit hitTest(const TextPanel &p, int px, int py, int *rowOut) const {
        const int lh = p.lineHeight(), cw = p.charWidth();
        const int top = rowsTop(lh) - 2;
        if (px < 0 || px >= p.width() || py < top) return Hit::None;
        const int idx = (py - top) / lh;
        if (idx < 0 || idx >= rowCount()) return Hit::None;
        *rowOut = idx;
        // Three characters wide, one either side of the glyph: a single
        // character is about seven pixels and too small to hit reliably.
        if (px >= layout::kPad + (layout::kDecCol - 1) * cw &&
            px < layout::kPad + (layout::kDecCol + 2) * cw)
            return Hit::Dec;
        if (px >= layout::kPad + (layout::kIncCol - 1) * cw &&
            px < layout::kPad + (layout::kIncCol + 2) * cw)
            return Hit::Inc;
        return Hit::Row;
    }

    static const char *label(int i) {
        switch (i) {
            case 0: return "Preset";
            case 1: return "Render scale";
            case 2: return "Bounces";
            case 3: return "Bounces (moving)";
            case 4: return "Exposure";
            case 5: return "Walk speed";
            case 6: return "Field of view";
            case 7: return "Time of day";
            case 8: return "Cycle speed";
            default: return "Bake as default";
        }
    }

    std::string value(int i) const {
        char b[128];
        switch (i) {
            case 0: {
                const int m = matchingPreset();
                if (m < 0) {
                    std::snprintf(b, sizeof(b), "%-9s %s", "Custom",
                                  "left/right to pick a preset");
                    break;
                }
                int n = 0;
                const Preset *p = presets(&n);
                std::snprintf(b, sizeof(b), "%-9s %s", p[m].name, p[m].note);
                break;
            }
            case 1: std::snprintf(b, sizeof(b), "%.2f", *t_.scale); break;
            case 2: std::snprintf(b, sizeof(b), "%d", *t_.depth); break;
            case 3: std::snprintf(b, sizeof(b), "%d", *t_.movingDepth); break;
            case 4: std::snprintf(b, sizeof(b), "%.2f", *t_.exposure); break;
            case 5: std::snprintf(b, sizeof(b), "%.1f m/s", *t_.speed); break;
            case 6: std::snprintf(b, sizeof(b), "%.0f deg", *t_.fov); break;
            case 7: {
                char c[16];
                t_.clock->clock(c, sizeof(c));
                std::snprintf(b, sizeof(b), "%s", c);
                break;
            }
            case 8: {
                char c[24];
                t_.clock->speedLabel(c, sizeof(c));
                std::snprintf(b, sizeof(b), "%s   (X + wheel)", c);
                break;
            }
            default:
                std::snprintf(b, sizeof(b), "%s",
                              bakeStatus.empty() ? "click > to save, then rebuild.bat"
                                                 : bakeStatus.c_str());
                break;
        }
        return b;
    }

    void applyPreset(int i) {
        int n = 0;
        const Preset *p = presets(&n);
        if (i < 0 || i >= n) return;
        *t_.scale = p[i].scale;
        *t_.depth = p[i].depth;
        invalidated = true;
    }

    void adjust(int dir) {
        switch (row_) {
            case 0: {
                int n = 0;
                presets(&n);
                // Step from where the settings ACTUALLY are, so the first press
                // moves one preset rather than jumping to a remembered cursor.
                const int m = matchingPreset();
                preset_ = (m >= 0) ? (m + dir + n) % n : (dir > 0 ? 0 : n - 1);
                applyPreset(preset_);
                return;
            }
            case 1:
                // Steps of 0.05 -- fine enough to find the edge of a frame-rate
                // target, coarse enough to cross the range in a few presses.
                *t_.scale = clampf(*t_.scale + 0.05f * dir, 0.15f, 2.0f);
                invalidated = true;
                return;
            case 2:
                *t_.depth = mini(32, maxi(1, *t_.depth + dir));
                invalidated = true;
                return;
            case 3:
                *t_.movingDepth = mini(32, maxi(1, *t_.movingDepth + dir));
                invalidated = true;
                return;
            case 4:
                // Multiplicative, because exposure is perceived that way and a
                // linear step is useless at both ends of the range.
                *t_.exposure = clampf(*t_.exposure * (dir > 0 ? 1.25f : 0.8f), 0.05f, 40.0f);
                return;  // changes no sample: do not throw the accumulation away
            case 5:
                *t_.speed = clampf(*t_.speed * (dir > 0 ? 1.3f : 1.0f / 1.3f), 0.2f, 200.0f);
                return;
            case 6:
                *t_.fov = clampf(*t_.fov + 2.0f * dir, 10.0f, 100.0f);
                invalidated = true;
                return;
            case 7:
                t_.clock->scrubHours(0.25f * dir);  // quarter-hour steps
                invalidated = true;
                return;
            case 8:
                t_.clock->nudgeSpeed(dir > 0);
                return;  // changes no sample already drawn
            default:
                // An action, not a value -- either direction fires it. The
                // viewer does the writing; the menu only asks, because the
                // settings worth baking include several it does not own.
                bakeRequested = true;
                return;
        }
    }
};

}  // namespace v2
