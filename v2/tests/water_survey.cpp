// ---------------------------------------------------------------------------
// water_survey.cpp -- where the water actually is, measured, with no GPU.
//
//   g++ -std=c++20 -O2 -I src tests/water_survey.cpp -o build/water_survey.exe
//   ./build/water_survey.exe [span m] [step m]
//
// THIS HARNESS EXISTS BECAUSE ONE NUMBER KILLED EVERY EARLIER ATTEMPT AT WATER
// IN THIS ENGINE. The line shipped at 2.6 m while the ground bottomed out at
// 5.8, so it flooded 0.000% of the world -- no lake anywhere, and no SAND
// either, because topMaterial picks sand and silt by distance from the line.
// Neither failure looked like a waterline. They looked like a broken mesher and
// a broken material, and they were chased as such.
//
// So nothing about the water is tuned by looking at it. It is tuned here, on
// four numbers that say the difference between a lake and an ocean:
//
//   PER BAND. The two woods have different landforms; a line that suits one
//   drowns the other, and a world total hides exactly that.
//   WET FRACTION. How much of the ground is under water at all.
//   BODIES. Connected components -- two dozen lakes, or one sea.
//   DEPTH. A puddle one voxel deep reads as a wet patch, not as water.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "scene/voxelworld.h"

using namespace v2;

static float pctl(std::vector<float> v, double p) {
    if (v.empty()) return 0.0f;
    const size_t k = size_t(p * double(v.size() - 1) + 0.5);
    std::nth_element(v.begin(), v.begin() + k, v.end());
    return v[k];
}

static void report(const char *label, const std::vector<float> &v) {
    if (v.empty()) {
        std::printf("  %-8s (none)\n", label);
        return;
    }
    std::printf("  %-8s n=%-8zu min %6.1f  p1 %6.1f  p5 %6.1f  med %6.1f  max %6.1f\n", label,
                v.size(), pctl(v, 0.0), pctl(v, 0.01), pctl(v, 0.05), pctl(v, 0.50), pctl(v, 1.0));
}

int main(int argc, char **argv) {
    const float span = (argc > 1) ? float(std::atof(argv[1])) : 3200.0f;
    const float step = (argc > 2) ? float(std::atof(argv[2])) : 4.0f;
    const int n = int(span / step);
    const float half = 0.5f * span;

    VoxelTerrain terrain;
    // Overrides, so the constants can be swept without a rebuild:
    //   water_survey <span> <step> [pineWater] [basinBed] [basinT]
    if (argc > 3) terrain.pineWater = float(std::atof(argv[3]));
    if (argc > 4) terrain.basinBed = float(std::atof(argv[4]));
    if (argc > 5) terrain.basinT = float(std::atof(argv[5]));
    // 1.0 = no flattening at all, i.e. the shore the basin carve alone leaves.
    if (argc > 6) terrain.bankFlat = float(std::atof(argv[6]));
    if (argc > 7) terrain.bankRiseM = float(std::atof(argv[7]));
    std::printf("%d x %d columns at %.1f m (%.0f m square)\n", n, n, step, span);
    std::printf("pineWater %.1f m, birchWater %s, basinT %.2f\n\n", terrain.pineWater,
                terrain.birchWater == VoxelTerrain::kNoWater ? "dry" : "set", terrain.basinT);
    std::printf("basinBed %.1f m below the line\n\n", terrain.basinBed);

    std::vector<float> pine, birch;
    std::vector<uint8_t> wet(size_t(n) * n, 0);
    std::vector<float> depth;
    size_t wetPine = 0, wetBirch = 0, nPine = 0, nBirch = 0;

    TerrainMemo memo;
    for (int j = 0; j < n; ++j) {
        const float z = -half + float(j) * step;
        for (int i = 0; i < n; ++i) {
            const float x = -half + float(i) * step;
            const float h = terrain.heightM(x, z, memo);
            const bool isPine = terrain.birchMix(x) <= 0.001f;
            if (isPine) { pine.push_back(h); ++nPine; }
            else { birch.push_back(h); ++nBirch; }

            // The engine's own test, not a re-derivation of it.
            const int vi = int(std::lround(x / VOXEL_M)), vj = int(std::lround(z / VOXEL_M));
            int waterY = 0;
            if (terrain.lakeColumn(vi, vj, memo, &waterY)) {
                wet[size_t(j) * n + i] = 1;
                if (isPine) ++wetPine; else ++wetBirch;
                depth.push_back((float(waterY) - std::floor(h / VOXEL_M)) * VOXEL_M);
            }
        }
    }

    std::printf("=== the height field, metres ===\n");
    report("pine", pine);
    report("birch", birch);

    std::printf("\n=== what is under water ===\n");
    const size_t wetAll = wetPine + wetBirch;
    std::printf("  overall %.3f%%   pine %.3f%%   birch %.3f%%\n",
                100.0 * double(wetAll) / double(size_t(n) * n),
                nPine ? 100.0 * double(wetPine) / double(nPine) : 0.0,
                nBirch ? 100.0 * double(wetBirch) / double(nBirch) : 0.0);
    if (!depth.empty())
        std::printf("  depth   min %.1f m   med %.1f m   p95 %.1f m   max %.1f m\n",
                    pctl(depth, 0.0), pctl(depth, 0.50), pctl(depth, 0.95), pctl(depth, 1.0));

    // ---- connected bodies -------------------------------------------------
    //
    // TWO DOZEN LAKES OR ONE SEA is the question the wet fraction cannot
    // answer. 1.2% of the world under water is a wood with lakes in it if it
    // is three hundred bodies and an inland sea if it is one.
    std::printf("\n=== bodies ===\n");
    {
        const float cellArea = step * step;
        std::vector<int> label(size_t(n) * n, 0);
        std::vector<size_t> sizes;
        std::vector<int> stack;
        int next = 0;
        for (int start = 0; start < n * n; ++start) {
            if (!wet[start] || label[start]) continue;
            ++next;
            size_t count = 0;
            stack.clear();
            stack.push_back(start);
            label[start] = next;
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                ++count;
                const int ci = c % n, cj = c / n;
                const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    const int ni = ci + di[d], nj = cj + dj[d];
                    if (ni < 0 || nj < 0 || ni >= n || nj >= n) continue;
                    const int nc = nj * n + ni;
                    if (!wet[nc] || label[nc]) continue;
                    label[nc] = next;
                    stack.push_back(nc);
                }
            }
            sizes.push_back(count);
        }
        std::sort(sizes.begin(), sizes.end(), std::greater<size_t>());
        size_t big = 0;
        for (size_t c : sizes) if (double(c) * cellArea >= 400.0) ++big;
        std::printf("  %zu bodies, %zu of them over 400 m2\n", sizes.size(), big);
        if (!sizes.empty()) {
            std::printf("  largest %.0f m2", double(sizes[0]) * cellArea);
            for (size_t k = 1; k < sizes.size() && k < 5; ++k)
                std::printf(", %.0f", double(sizes[k]) * cellArea);
            std::printf(" m2\n");
        }
        // The single most useful line here: a world where one body is most of
        // the water is an ocean however small the percentage looks.
        if (!sizes.empty())
            std::printf("  the largest body is %.1f%% of all the water\n",
                        100.0 * double(sizes[0]) / double(wetAll));
    }

    // ---- DOES THE BEACH END IN A CLIFF? ------------------------------------
    //
    // THE ONE TEST THE BANK NEEDS, AND IT CANNOT BE A SCREENSHOT. A shore that
    // drops off a step is invisible in a wide shot and obvious the moment you
    // walk off it, so v4 measured it instead: the step to all four neighbours
    // over every column within 1.2 m of the waterline. Before its ease, 14,738
    // steps of exactly 7 voxels; after, zero steps of 4 or more, worst case 2.
    //
    // The residual 2 is inherent rather than a fault: the lowest bank column
    // sits one voxel over the line and the shallowest wet column one under it,
    // so the water's own edge is a two-voxel step and always will be.
    std::printf("\n=== the step at the shore (the bank's acceptance test) ===\n");
    {
        TerrainMemo m3;
        const float probe = 1.2f;  // metres either side of the line
        size_t counts[16] = {0};
        size_t worst = 0, total = 0, big = 0;
        const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
        // ADJACENT COLUMNS, NOT A METRE APART. The first version of this
        // sampled neighbours ten voxels away and reported a worst step of 62
        // voxels -- which is the hillside's SLOPE over a metre, not a step, and
        // says nothing about whether the beach has a cliff in it. A step is
        // between columns that touch.
        //
        // So: every voxel column in a 100 m square on the lake, which is a
        // million heights and about a second.
        for (int j = -500; j < 500; ++j)
            for (int i = -500; i < 500; ++i) {
                const int vi = -4544 + i, vj = -4269 + j;  // one voxel apart
                const int wl = terrain.lakeLineAt(terrain.wx(vi), terrain.wx(vj), m3);
                if (wl == VoxelTerrain::kNoWaterVox) continue;
                const int h = terrain.heightVox(vi, vj, m3);
                if (std::abs(h - wl) * VOXEL_M > probe) continue;  // not at the shore
                for (int d = 0; d < 4; ++d) {
                    const int nh = terrain.heightVox(vi + di[d], vj + dj[d], m3);
                    const size_t st = size_t(std::abs(nh - h));
                    ++total;
                    if (st > worst) worst = st;
                    if (st >= 4) ++big;
                    counts[st < 15 ? st : 15]++;
                }
            }
        std::printf("  %zu shore steps sampled, worst %zu voxels, %zu of 4+\n", total, worst, big);
        // WHAT THE BANK ACTUALLY CHANGES, which the step histogram cannot see.
        // Flattening never moves a column out of the band -- the map is
        // monotonic onto itself -- it lowers the band's lower half toward the
        // water. So the thing to measure is the PROFILE: the mean rise of the
        // shore above the line. A beach is a break in slope, and this is the
        // break.
        {
            double sum = 0;
            size_t n2 = 0;
            for (int j = -500; j < 500; ++j)
                for (int i = -500; i < 500; ++i) {
                    const int vi = -4544 + i, vj = -4269 + j;
                    const int wl2 = terrain.lakeLineAt(terrain.wx(vi), terrain.wx(vj), m3);
                    if (wl2 == VoxelTerrain::kNoWaterVox) continue;
                    const int h2 = terrain.heightVox(vi, vj, m3);
                    const int d2 = h2 - wl2;
                    if (d2 <= 0 || d2 > terrain.bankRiseVox()) continue;
                    sum += double(d2) * VOXEL_M;
                    ++n2;
                }
            if (n2)
                std::printf("  beach: %zu columns, mean rise %.2f m above the line\n",
                            n2, sum / double(n2));
        }
        for (size_t k = 0; k < 10; ++k)
            if (counts[k]) std::printf("      %zu voxels : %zu\n", k, counts[k]);
        if (big == 0 && total > 0) std::printf("  no cliff: nothing steps 4 voxels or more.\n");
    }

    // ---- and the sand, which lives or dies with the line -------------------
    std::printf("\n=== the shore band ===\n");
    {
        // ASKED THE WAY topMaterial ASKS IT, through the BASIN-GATED line.
        //
        // This used to call waterVoxAt -- the band's line with no basin test --
        // which is a different rule from the one that paints the ground, and it
        // reported a shore band four times the size of the water. topMaterial
        // had the same bug, and the visible result was sand pits all over the
        // wood with no water in them. Measuring it the wrong way is how it
        // survived a survey that was supposed to catch exactly this.
        size_t bed = 0, beach = 0, dryBed = 0;
        TerrainMemo m2;
        for (int j = 0; j < n; ++j) {
            const float z = -half + float(j) * step;
            for (int i = 0; i < n; ++i) {
                const float x = -half + float(i) * step;
                const int vi = int(std::lround(x / VOXEL_M)), vj = int(std::lround(z / VOXEL_M));
                const int h = terrain.heightVox(vi, vj, m2);
                const int wl = terrain.lakeLineAt(x, z, m2);
                // Sand under no water at all is the bug; count it explicitly.
                const int band = terrain.waterVoxAt(x);
                if (wl == VoxelTerrain::kNoWaterVox && band != VoxelTerrain::kNoWaterVox &&
                    h <= band + 8)
                    ++dryBed;
                if (h <= wl) ++bed;
                else if (h <= wl + 8) ++beach;
            }
        }
        std::printf("  %zu columns of bed (sand/silt), %zu of beach above the line\n", bed, beach);
        std::printf("  %zu columns the OLD band-only rule would have sanded with no water\n",
                    dryBed);
        if (bed == 0 && beach == 0)
            std::printf("  ^ BOTH ZERO: there is no water and no sand in this world at all.\n");
    }
    return 0;
}
