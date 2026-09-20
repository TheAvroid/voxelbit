// ---------------------------------------------------------------------------
// petal_canopy_test -- ARE THE FALLEN PETALS UNDER THE TREES, OR EVERYWHERE?
//
//   g++ -std=c++20 -O2 -I src tests/petal_canopy_test.cpp -o build/tools/petal_canopy_test.exe
//   ./build/tools/petal_canopy_test.exe [span m = 160] [stride m = 0.2]
//
// WHY THIS EXISTS. "Make sure the pink petals are only located underneath the
// tree" (user, 2026-09-19) is a statement about AREA, and area is the one thing
// a screenshot of a cherry wood cannot show you: the canopy is closed overhead,
// so every camera either looks at blossom or at the two square metres of grass
// between two trunks. Both pictures look the same whether the rule is right or
// wrong.
//
// WHAT IT MEASURES. The petal gate has two halves -- standDensity > kPetalStand
// (is this thick wood) and canopyNear (is a trunk within reach) -- and the
// second one is the fix. So it walks a patch of the cherry band and reports the
// share of ground each half admits, which is the number the ask is about, plus
// the trunk density the lattice implies as a cross-check that canopyNear is
// mirroring a real wood rather than an empty one.
//
// THE LATTICE IS SET BY HAND HERE, to the values World::load publishes out of
// the mesher (see VoxelTerrain::CanopyLattice). If those knobs are ever tuned
// this harness has to follow them -- it is measuring the rule, not reading the
// engine's mind.
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
    const float span = argc > 1 ? float(std::atof(argv[1])) : 160.0f;
    const float step = argc > 2 ? float(std::atof(argv[2])) : 0.2f;

    VoxelTerrain t;
    // WHAT World::load PUBLISHES -- see the note at the top.
    t.canopy.ok = true;
    t.canopy.strideM = 4.4f;     // birchStride: the birches are loaded
    t.canopy.seed = 20260904u;   // VoxelChunks::seed
    t.canopy.density = 0.0975f;  // oakDensity
    t.canopy.knee = 0.30f;
    t.canopy.span = 0.32f;
    t.canopy.gain = 1.20f;
    t.canopy.floorV = 0.262f;

    // ---- find the middle of a cherry band ---------------------------------
    float cx = 0.0f;
    bool found = false;
    for (float x = -6000.0f; x <= 6000.0f && !found; x += 2.0f)
        if (t.woodBit(x) & kWoodCherry) {
            // walk to the middle of the run so the patch is not on a seam
            float a = x;
            while ((t.woodBit(a) & kWoodCherry) && a > x - 2000.0f) a -= 2.0f;
            float b = x;
            while ((t.woodBit(b) & kWoodCherry) && b < x + 2000.0f) b += 2.0f;
            cx = 0.5f * (a + b);
            found = true;
        }
    if (!found) {
        std::printf("petal_canopy: no cherry band found -- nothing to measure\n");
        return 1;
    }
    std::printf("petal_canopy: cherry band centred at x = %.0f, patch %.0f m at %.2f m\n", cx,
                span, step);

    // ---- the two halves of the gate, over the patch -----------------------
    long long cells = 0, stand = 0, both = 0;
    double sumDist = 0.0;
    long long distN = 0;
    const float cz = 0.0f;
    for (float z = cz - 0.5f * span; z < cz + 0.5f * span; z += step)
        for (float x = cx - 0.5f * span; x < cx + 0.5f * span; x += step) {
            if (!(t.woodBit(x) & kWoodCherry)) continue;
            TerrainMemo memo;
            ++cells;
            if (t.standDensity(x, z, memo.stand) <= VoxelTerrain::kPetalStand) continue;
            ++stand;
            float tx = 0.0f, tz = 0.0f;
            // No DEM in the harness, so stemFill is 1 -- see VoxelTerrain::stemFill.
            if (!t.canopyNear(x, z, 1.0f, memo, &tx, &tz)) continue;
            ++both;
            sumDist += std::sqrt((x - tx) * (x - tx) + (z - tz) * (z - tz));
            ++distN;
        }
    if (!cells) {
        std::printf("petal_canopy: the patch held no cherry columns\n");
        return 1;
    }
    const double pStand = 100.0 * double(stand) / double(cells);
    const double pBoth = 100.0 * double(both) / double(cells);
    std::printf("  ground     %lld columns in the band\n", cells);
    std::printf("  stand only %8lld  %5.1f%%   <- what the petals used to cover\n", stand, pStand);
    std::printf("  under a tree %6lld  %5.1f%%   <- what they cover now\n", both, pBoth);
    std::printf("  cut        %5.1fx smaller,  mean %.2f m from the trunk (cap %.2f)\n",
                both ? double(stand) / double(both) : 0.0, distN ? sumDist / double(distN) : 0.0,
                t.canopy.radiusM);

    // ---- and the wood the lattice implies ---------------------------------
    //
    // A cross-check, not an assertion: if canopyNear were mirroring an empty
    // lattice the share above would be zero and look like a pass. Counted by
    // walking the same patch on the lattice's own grid.
    long long sites = 0;
    const float st = t.canopy.strideM;
    const int steps = int(CHUNK_M / st);
    const int c0 = int(std::floor((cx - 0.5f * span) / CHUNK_M));
    const int c1 = int(std::floor((cx + 0.5f * span) / CHUNK_M));
    const int d0 = int(std::floor((cz - 0.5f * span) / CHUNK_M));
    const int d1 = int(std::floor((cz + 0.5f * span) / CHUNK_M));
    for (int dz = d0; dz <= d1; ++dz)
        for (int dx = c0; dx <= c1; ++dx)
            for (int j = 0; j <= steps; ++j)
                for (int i = 0; i <= steps; ++i) {
                    const float bx = float(dx) * CHUNK_M + float(i) * st;
                    const float bz = float(dz) * CHUNK_M + float(j) * st;
                    const uint32_t cell =
                        hashU32(uint32_t(int(bx * 16.0f)), uint32_t(int(bz * 16.0f)) ^ 0x9E37u);
                    const float sx = bx + (hashUnit(t.canopy.seed + 11u, cell) - 0.5f) * st * 1.8f;
                    const float sz = bz + (hashUnit(t.canopy.seed + 12u, cell) - 0.5f) * st * 1.8f;
                    const int ci = int(std::floor(sx / VOXEL_M));
                    const int cj = int(std::floor(sz / VOXEL_M));
                    if (ci < dx * CHUNK_VOX || ci >= dx * CHUNK_VOX + CHUNK_VOX ||
                        cj < dz * CHUNK_VOX || cj >= dz * CHUNK_VOX + CHUNK_VOX)
                        continue;
                    if (sx < cx - 0.5f * span || sx >= cx + 0.5f * span) continue;
                    if (sz < cz - 0.5f * span || sz >= cz + 0.5f * span) continue;
                    TerrainMemo memo;
                    const float sg = saturate(
                        (t.standDensity(sx, sz, memo.stand) - t.canopy.knee) / t.canopy.span);
                    const float accept = (sg * sg * t.canopy.gain + t.canopy.floorV) *
                                         t.canopy.density;
                    if (hashUnit(t.canopy.seed + 13u, cell) > accept) continue;
                    ++sites;
                }
    const double ha = double(span) * double(span) / 10000.0;
    std::printf("  lattice    %lld trunks in the patch, %.0f a hectare\n", sites,
                ha > 0.0 ? double(sites) / ha : 0.0);

    // A patch of wood with no trunks in it cannot be a wood, and petals over
    // more than a quarter of the ground are not "underneath the tree".
    const bool ok = sites > 0 && pBoth < 25.0 && both > 0;
    std::printf("  %s\n", ok ? "PASS -- the petals are under the trunks."
                             : "FAIL -- see the shares above.");
    return ok ? 0 : 1;
}
