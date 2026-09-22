// ---------------------------------------------------------------------------
// cherry_extent_test -- WHERE IS THE CHERRY WOOD, AND DOES EVERYTHING AGREE?
//
//   g++ -std=c++20 -O2 -I src tests/cherry_extent_test.cpp -o build/tools/cherry_extent_test.exe
//
// WHY THIS EXISTS. "There are STILL pink moss rocks outside the cherry forest"
// -- the fourth report of the same thing, after three fixes that each read
// correctly and each left it happening.
//
// THE ENGINE HAS TWO ANSWERS TO "IS THIS THE BLOSSOM". The TREES are recoloured
// on `cherryMix(x) >= 0.5f` (VoxelChunks::scatter and its twin collectTrees).
// The MOSS, the petals and every other cherry decoration ask
// `woodBit(x) & kWoodCherry`, and woodBit returns the band with the LARGEST
// weight -- which is true well before any weight reaches a half. Wherever the
// two disagree the ground is dressed for the blossom and the trees standing on
// it are green oaks, which is exactly what "outside the cherry forest" means to
// somebody looking at it.
//
// This walks x across a whole band period and reports the disagreement in
// metres, on both sides.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "world/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    const float step = argc > 1 ? float(std::atof(argv[1])) : 1.0f;
    VoxelTerrain t;

    // One full period of the band tiling, from the middle of the world.
    const float lo = -6000.0f, hi = 6000.0f;
    long long bitOnly = 0, mixOnly = 0, both = 0, neither = 0;
    float runLo = 0.0f;
    bool inRun = false;
    std::vector<std::pair<float, float>> runs;   // stretches where only woodBit says cherry

    for (float x = lo; x <= hi; x += step) {
        const bool bit = (t.woodBit(x) & kWoodCherry) != 0;
        const bool mix = t.cherryMix(x) >= 0.5f;
        if (bit && !mix) ++bitOnly;
        else if (mix && !bit) ++mixOnly;
        else if (bit) ++both;
        else ++neither;

        if (bit && !mix && !inRun) { inRun = true; runLo = x; }
        if ((!bit || mix) && inRun) {
            inRun = false;
            if (runs.size() < 40) runs.push_back({runLo, x});
        }
    }

    const double m = double(step);
    std::printf("cherry_extent: %.0f m to %.0f m at %.2f m\n", double(lo), double(hi), double(step));
    std::printf("  both agree      %8.0f m\n", double(both) * m);
    std::printf("  woodBit ONLY    %8.0f m   <- dressed for blossom, planted with oaks\n",
                double(bitOnly) * m);
    std::printf("  cherryMix ONLY  %8.0f m   <- pink trees on undressed ground\n",
                double(mixOnly) * m);
    std::printf("  neither         %8.0f m\n", double(neither) * m);

    if (!runs.empty()) {
        std::printf("  the disagreeing stretches (first %d):\n", int(runs.size()));
        for (const auto &r : runs)
            std::printf("      x %8.0f .. %8.0f   %6.0f m wide   (%s)\n", double(r.first),
                        double(r.second), double(r.second - r.first),
                        t.woodName(0.5f * (r.first + r.second)));
    }

    // A band whose decorations reach further than its trees is the bug. Any
    // stretch at all is one, so the bar is zero -- but print the width first,
    // because the width is what says whether it is a seam or a wood.
    const bool ok = bitOnly == 0;
    std::printf("  %s\n", ok ? "PASS -- the ground and the trees agree everywhere."
                             : "FAIL -- the ground is dressed for cherries the scatter never planted.");
    return ok ? 0 : 1;
}
