#pragma once
// ---------------------------------------------------------------------------
// loadtrace.h -- where the startup seconds actually go.
//
// (user 2026-09-20: "can you optimize the initial load time when it initialized
//  falcor".)
//
// WHY THIS EXISTS. The load already prints three durations -- `models`, `world`
// and `render` -- and on a warm start they come to 8.5 seconds against a
// measured total of 20.3. **Twelve seconds were not attributed to anything**,
// and no amount of reading the source says which call they are in: the startup
// is a device creation, a plugin scan, two NGX DLL loads, an asset pass and a
// shader cache, and any of them is a plausible twelve seconds.
//
// So every milestone stamps itself against one clock started at main(). The
// output is a list of gaps, and the biggest gap is the thing to work on. That
// is the whole design -- it is a ruler, not a profiler.
//
// OFF BY DEFAULT, because a game that prints timing at every launch is a game
// with a debug build's manners. `--load-trace` turns it on.
//
// THE CLOCK STARTS AT STATIC INIT, not at the first mark, so the first gap
// includes everything the C runtime and the dynamic loader did before main --
// which for this binary is Streamline and the NGX signature checks, and those
// are not free.
// ---------------------------------------------------------------------------
#include <chrono>
#include <cstdio>

namespace v2 {

inline bool loadTraceOn = false;

inline std::chrono::steady_clock::time_point &loadT0() {
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    return t0;
}

// Print the time since start and since the previous mark. The GAP is the
// number that matters: a milestone's own cost is the distance from the one
// before it, and a total tells you nothing about which call to look at.
inline void loadMark(const char *what) {
    if (!loadTraceOn) return;
    static std::chrono::steady_clock::time_point prev = loadT0();
    const auto now = std::chrono::steady_clock::now();
    const double all = std::chrono::duration<double>(now - loadT0()).count();
    const double gap = std::chrono::duration<double>(now - prev).count();
    prev = now;
    std::printf("  [load] %7.2fs  +%6.2fs  %s\n", all, gap, what);
    std::fflush(stdout);
}

}   // namespace v2
