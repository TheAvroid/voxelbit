// ---------------------------------------------------------------------------
// Do the settings rows actually move the things they claim to?
//
// Driving the real window turned out to be unverifiable from here -- Windows
// refuses SetForegroundWindow to a process that has not just received input,
// so synthetic keys landed about one run in three, and arrows are extended
// scancodes that arrive as numpad presses without KEYEVENTF_EXTENDEDKEY. Both
// made working rows look broken. So this drives SettingsMenu directly: no
// window, no GDI, no injection.
// ---------------------------------------------------------------------------
#include <cstdio>

#include "../../src/render/menu.h"

int main() {
    v2::SettingsMenu m;
    float scale = 0.70f, exposure = 1.25f, speed = 9.2f, fov = 50.0f;
    int depth = 10, movingDepth = 4, spf = 1;
    bool grain = true, skyNee = false, gi = false;
    unsigned live = 2;
    v2::DayNight clock;

    v2::MenuTarget t;
    t.scale = &scale;  t.depth = &depth;  t.movingDepth = &movingDepth;
    t.exposure = &exposure;  t.speed = &speed;  t.fov = &fov;  t.clock = &clock;
    t.samplesPerFrame = &spf;  t.constantGrain = &grain;
    t.liveMaxAccum = &live;  t.skyNee = &skyNee;  t.gi = &gi;
    m.bind(t);
    m.open = true;

    int fails = 0;
    // The cursor starts at 0 and wraps, so its position is tracked rather than
    // reset -- "lots of UPs" only lands on row 0 if the count is a multiple of
    // the row count, which is exactly the sort of thing that makes a working
    // menu look broken.
    int cur = 0;
    // Asked of the menu rather than written down here. A hardcoded count is a
    // test that breaks every time a row is added, which trains you to edit the
    // number instead of reading the failure.
    const int rows = v2::SettingsMenu::rowCount();
    auto go = [&](int row) {
        while (cur != row) {
            m.key(GLFW_KEY_DOWN);
            cur = (cur + 1) % rows;
        }
    };

    // Samples / frame is row 9 and must step by one, both ways, and clamp.
    go(9);
    m.key(GLFW_KEY_RIGHT);
    if (spf != 2) { std::printf("FAIL: samples row did not raise (got %d)\n", spf); ++fails; }
    m.key(GLFW_KEY_RIGHT);
    if (spf != 3) { std::printf("FAIL: samples row did not raise twice (got %d)\n", spf); ++fails; }
    for (int i = 0; i < 8; ++i) m.key(GLFW_KEY_LEFT);
    if (spf != 1) { std::printf("FAIL: samples row did not clamp at 1 (got %d)\n", spf); ++fails; }

    // ...and it must be the SAMPLES row, not a neighbour: nothing else moved.
    if (scale != 0.70f || depth != 10 || exposure != 1.25f || fov != 50.0f) {
        std::printf("FAIL: row 9 moved something other than the sample count\n");
        ++fails;
    }

    // Constant grain is row 10 and is a toggle.
    go(10);
    m.key(GLFW_KEY_RIGHT);
    if (grain) { std::printf("FAIL: grain row did not toggle off\n"); ++fails; }
    m.key(GLFW_KEY_LEFT);
    if (!grain) { std::printf("FAIL: grain row did not toggle back on\n"); ++fails; }

    // Sky sampling is row 11 and is a toggle, defaulting off.
    go(11);
    m.key(GLFW_KEY_RIGHT);
    if (!skyNee) { std::printf("FAIL: sky row did not toggle on\n"); ++fails; }
    m.key(GLFW_KEY_LEFT);
    if (skyNee) { std::printf("FAIL: sky row did not toggle back off\n"); ++fails; }
    if (spf != 1 || !grain) { std::printf("FAIL: the sky row moved a neighbour\n"); ++fails; }

    // Baked GI is row 12 and is a toggle, defaulting off.
    go(12);
    m.key(GLFW_KEY_RIGHT);
    if (!gi) { std::printf("FAIL: gi row did not toggle on\n"); ++fails; }
    m.key(GLFW_KEY_LEFT);
    if (gi) { std::printf("FAIL: gi row did not toggle back off\n"); ++fails; }
    if (skyNee || spf != 1) { std::printf("FAIL: the gi row moved a neighbour\n"); ++fails; }

    // And the bake action must still be the LAST row, not one of these --
    // firing a bake by accident rewrites defaults.h behind the user's back.
    go(rows - 1);
    m.bakeRequested = false;
    m.key(GLFW_KEY_RIGHT);
    if (!m.bakeRequested) { std::printf("FAIL: row %d is not the bake action\n", rows - 1); ++fails; }
    if (spf != 1 || !grain) { std::printf("FAIL: the bake moved a render setting\n"); ++fails; }

    std::printf("%s\n", fails ? "settings rows: FAILED"
                              : "settings rows: OK -- samples steps and clamps, the three toggles toggle, "
                                "bake is still last and touches nothing else");
    return fails ? 1 : 0;
}
