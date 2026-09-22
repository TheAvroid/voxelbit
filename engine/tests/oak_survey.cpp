// ---------------------------------------------------------------------------
// WHAT THE OAK BAND ACTUALLY IS -- height distribution, wet fraction, and the
// two seams either side of it.
//
// (user 2026-09-16: "import the oak forest from v1 into v2".)
//
// WRITTEN BEFORE THE ENGINE WAS BUILT, because the waterline is the number that
// has killed every earlier attempt at water here -- see the long note over
// pineWater -- and "the oak band is under twenty metres of water" is not
// something a screenshot tells you quickly. scene/voxelworld.h is GPU-free, so
// the real field can be swept in a second without a 4 GB build.
//
//   g++ -std=c++17 -O2 -I src -o oak_survey.exe tests/oak_survey.cpp
// ---------------------------------------------------------------------------
#include "scene/voxelworld.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace v2;

namespace {

struct Band {
    const char *name;
    Biome biome;
};

void sweep(const VoxelTerrain &t, const Band &band, float halfSpan) {
    const float centre = VoxelTerrain::bandCentre(band.biome);
    std::vector<float> h;
    h.reserve(64000);
    int wet = 0, total = 0;
    // A SQUARE INSIDE THE BAND, not the whole band: the outer 45 m either side
    // is seam, and folding a neighbour's field into this wood's statistics is
    // how you get a median that belongs to nobody.
    for (float x = centre - halfSpan; x <= centre + halfSpan; x += 2.0f)
        for (float z = -600.0f; z <= 600.0f; z += 2.0f) {
            TerrainMemo memo;
            const float y = t.heightM(x, z, memo);
            h.push_back(y);
            ++total;
            const float w = t.waterAt(x);
            if (w != VoxelTerrain::kNoWater && y < w) ++wet;
        }
    std::sort(h.begin(), h.end());
    auto pc = [&](double q) { return h[size_t(q * double(h.size() - 1))]; };
    std::printf("  %-5s  min %5.1f  p5 %5.1f  med %5.1f  p95 %5.1f  max %5.1f   "
                "water %5.1f  wet %5.1f%%\n",
                band.name, double(h.front()), double(pc(0.05)), double(pc(0.50)),
                double(pc(0.95)), double(h.back()), double(t.waterAt(centre)),
                100.0 * double(wet) / double(total));
}

// The worst one-step change in height along x, which is what a wall in the
// world looks like from here. The seams are where to look for it.
void seamScan(const VoxelTerrain &t) {
    float worst = 0.0f, worstAt = 0.0f;
    for (float x = -100.0f; x <= 2500.0f; x += 0.1f) {
        TerrainMemo m0, m1;
        const float a = t.heightM(x, 17.0f, m0);
        const float b = t.heightM(x + 0.1f, 17.0f, m1);
        const float d = std::fabs(b - a);
        if (d > worst) { worst = d; worstAt = x; }
    }
    std::printf("\n  worst 10 cm step in x  %.3f m at x=%.1f  %s\n", double(worst),
                double(worstAt),
                worst < 1.0f ? "no wall -- the terrain's own gradient"
                             : "A STEP THIS BIG IS A WALL -- WRONG");
}

}  // namespace

int main() {
    VoxelTerrain t;
    std::printf("=== OAK SURVEY ===\n");
    std::printf("  a 900 m square inside each band, sampled every 2 m\n\n");
    const Band kBands[] = {
        {"pine", Biome::Pine}, {"birch", Biome::Birch}, {"oak", Biome::Oak}};
    for (const Band &b : kBands) sweep(t, b, 340.0f);

    // -- AND WHERE THE OAK'S LINE SHOULD SIT -------------------------------
    //
    // The first guess put it a quarter of the way up the FORMULA's range and
    // flooded 27% of the band -- the double smoothstep piles the distribution
    // into the middle, so a quarter of the range is near the median rather than
    // near the floor. The birch is the target to match: 7.6% of it is wet, and
    // that is a wood with lakes in it rather than a marsh.
    std::printf("\n  candidate oak waterlines (birch sits at 7.6%% wet)\n");
    VoxelTerrain probe;
    const float centre = VoxelTerrain::bandCentre(Biome::Oak);
    for (float w = 3.0f; w <= 9.6f; w += 0.2f) {
        probe.oakWater = w;
        int wet = 0, total = 0;
        for (float x = centre - 340.0f; x <= centre + 340.0f; x += 2.0f)
            for (float z = -600.0f; z <= 600.0f; z += 2.0f) {
                TerrainMemo memo;
                if (probe.heightM(x, z, memo) < w) ++wet;
                ++total;
            }
        std::printf("    %.1f m  %5.1f%%%s\n", double(w),
                    100.0 * double(wet) / double(total),
                    (100.0 * double(wet) / double(total) >= 7.0 &&
                     100.0 * double(wet) / double(total) <= 8.2)
                        ? "   <- matches the birch"
                        : "");
    }
    seamScan(t);
    return 0;
}
