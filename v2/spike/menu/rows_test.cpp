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
    bool grain = true;
    unsigned live = 2;
    v2::DayNight clock;

    v2::MenuTarget t;
    t.scale = &scale;  t.depth = &depth;  t.movingDepth = &movingDepth;
    t.exposure = &exposure;  t.speed = &speed;  t.fov = &fov;  t.clock = &clock;
    t.samplesPerFrame = &spf;  t.constantGrain = &grain;
    t.liveMaxAccum = &live;
    m.bind(t);
    m.open = true;

    int fails = 0;
    // The cursor starts at 0 and wraps, so its position is tracked rather than
    // reset -- "lots of UPs" only lands on row 0 if the count is a multiple of
    // the row count, which is exactly the sort of thing that makes a working
    // menu look broken.
    int cur = 0;
    const int rows = 12;
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

    // And the bake action must still be the LAST row, not one of these --
    // firing a bake by accident rewrites defaults.h behind the user's back.
    go(11);
    m.bakeRequested = false;
    m.key(GLFW_KEY_RIGHT);
    if (!m.bakeRequested) { std::printf("FAIL: row 12 is not the bake action\n"); ++fails; }
    if (spf != 1 || !grain) { std::printf("FAIL: the bake moved a render setting\n"); ++fails; }

    std::printf("%s\n", fails ? "settings rows: FAILED"
                              : "settings rows: OK -- samples steps and clamps, grain toggles, "
                                "bake is still last and touches nothing else");
    return fails ? 1 : 0;
}
