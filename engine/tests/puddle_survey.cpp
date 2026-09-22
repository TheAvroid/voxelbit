// ---------------------------------------------------------------------------
// puddle_survey.cpp -- how the water is DIVIDED UP, per band, with no GPU.
//
//   g++ -std=c++20 -O2 -I src tests/puddle_survey.cpp -o build/puddle_survey.exe
//   ./build/puddle_survey.exe [span m] [step m] [oakWater] [puddleT] [puddleFade] [cap]
//
// water_survey answers "how much water, and how deep". This answers the one
// question it cannot: HOW MANY SEPARATE PIECES, and how big each piece is.
//
// A wood that is 4% wet is a wood with lakes in it if that 4% is twenty bodies,
// and a wood covered in puddles if it is nine hundred -- and THE WET FRACTION
// IS IDENTICAL IN BOTH CASES. That is the whole reason this file exists (user
// 2026-09-17: "can you prevent little puddles from forming? we want lakes more
// then ponds"). Every water number this engine has been tuned on until now was
// blind to it.
//
// PER BAND, because the three woods have three landforms and three waterlines,
// and a world total hides which of them is puddling.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "scene/voxelworld.h"

using namespace v2;

// THE AREA AT WHICH A BODY OF WATER STOPS BEING A PUDDLE, in square metres.
// 400 is water_survey's own line for "a real one" -- a square twenty metres on
// a side -- and it is reused rather than reinvented so the two agree.
static constexpr double kPondM2 = 400.0;

struct Band {
    const char *name;
    float cx;   // the band centre in x
};

int main(int argc, char **argv) {
    const float span = (argc > 1) ? float(std::atof(argv[1])) : 1600.0f;
    const float step = (argc > 2) ? float(std::atof(argv[2])) : 2.0f;

    VoxelTerrain terrain;
    // ONE ARGUMENT PER BAND, because the three woods have three landforms and
    // the waterline is a property of a wood's height DISTRIBUTION rather than
    // of the wood -- see the note over oakWater, and the sweep that had to be
    // re-run every time the terrain moved.
    if (argc > 3) terrain.pineWater = float(std::atof(argv[3]));
    if (argc > 4) terrain.birchWater = float(std::atof(argv[4]));
    if (argc > 5) terrain.oakWater = float(std::atof(argv[5]));
    if (argc > 6) terrain.puddleT = float(std::atof(argv[6]));
    if (argc > 7) terrain.puddleFade = float(std::atof(argv[7]));
    // ...AND THE BEACH. "theres way too much sand" and "increase the water
    // surface area" are two dials, not one, and only a sweep says which is
    // doing what.
    if (argc > 8) terrain.sandRiseM = float(std::atof(argv[8]));
    if (argc > 9) terrain.birchSandRiseM = float(std::atof(argv[9]));

    // THE BAND CENTRES OFF THE ENGINE'S OWN GEOMETRY rather than written out
    // here: bandCentre is what places every other per-band thing, so a band
    // that moves moves this with it.
    const Band bands[3] = {{"pine", VoxelTerrain::bandCentre(Biome::Pine)},
                           {"birch", VoxelTerrain::bandCentre(Biome::Birch)},
                           {"oak", VoxelTerrain::bandCentre(Biome::Oak)}};

    // 640 m OF X AND THE FULL SPAN OF Z. A band is 800 m wide in x and
    // unbounded along z, so a square window would either miss most of the band
    // or run into its neighbours. Inset well inside the seam, because a lake
    // straddling one belongs to neither wood and would be counted twice.
    const float insetX = 320.0f;
    const int nx = int(2.0f * insetX / step);
    const int nz = int(span / step);
    std::printf("%d x %d columns at %.1f m per band  (%.0f x %.0f m)\n", nx, nz, step,
                2.0 * double(insetX), double(span));
    std::printf("lines: pine %.2f  birch %.2f  oak %.2f   sandRise %.2f / %.2f\n\n",
                terrain.pineWater, terrain.birchWater, terrain.oakWater, terrain.sandRiseM,
                terrain.birchSandRiseM);

    for (const Band &b : bands) {
        TerrainMemo memo;
        std::vector<uint8_t> wet(size_t(nx) * size_t(nz), 0);
        size_t nWet = 0, nSand = 0, nNearMiss = 0;
        for (int j = 0; j < nz; ++j) {
            const float z = -0.5f * span + float(j) * step;
            for (int i = 0; i < nx; ++i) {
                const float x = b.cx - insetX + float(i) * step;
                const int vi = int(std::lround(x / VOXEL_M));
                const int vj = int(std::lround(z / VOXEL_M));
                int waterY = 0;
                const bool w = terrain.lakeColumn(vi, vj, memo, &waterY);
                // THE SHORE, COUNTED SEPARATELY -- "I just see sand banks with
                // no water" is a statement about the RATIO of these two, and
                // no number in this engine reported it per band.
                //
                // A DRY SANDY COLUMN is one the shore rule paints (h is within
                // sandRise of the line) that the depth rule leaves dry. Too
                // many of those against the wet count IS the complaint.
                const int hv = terrain.heightVox(vi, vj, memo);
                const int wl = terrain.lakeLineAt(terrain.wx(vi), terrain.wx(vj), memo);
                if (wl != VoxelTerrain::kNoWaterVox && !w &&
                    hv <= wl + terrain.sandRiseVoxAt(terrain.wx(vi))) {
                    ++nSand;
                    // ...and of those, how many are only just too high. This is
                    // the water that a slightly higher line would win back, as
                    // opposed to beach that is genuinely above the lake.
                    if (hv <= wl + 2) ++nNearMiss;
                }
                if (!w) continue;
                wet[size_t(j) * size_t(nx) + size_t(i)] = 1;
                ++nWet;
            }
        }

        // ---- connected bodies, four-connected ----------------------------
        const double cellArea = double(step) * double(step);
        std::vector<int> label(size_t(nx) * size_t(nz), 0);
        std::vector<size_t> sizes;
        std::vector<int> stack;
        int next = 0;
        for (int start = 0; start < nx * nz; ++start) {
            if (!wet[size_t(start)] || label[size_t(start)]) continue;
            ++next;
            size_t count = 0;
            stack.clear();
            stack.push_back(start);
            label[size_t(start)] = next;
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                ++count;
                const int ci = c % nx, cj = c / nx;
                const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    const int ni = ci + di[d], nj = cj + dj[d];
                    if (ni < 0 || nj < 0 || ni >= nx || nj >= nz) continue;
                    const int nc = nj * nx + ni;
                    if (!wet[size_t(nc)] || label[size_t(nc)]) continue;
                    label[size_t(nc)] = next;
                    stack.push_back(nc);
                }
            }
            sizes.push_back(count);
        }
        // WHERE THE BIGGEST ONE IS, so a render can be pointed at it. A
        // percentage cannot be looked at; a coordinate can.
        {
            int best = 0;
            size_t bestN = 0;
            std::vector<size_t> cnt(size_t(next) + 1, 0);
            for (int k = 0; k < nx * nz; ++k)
                if (label[size_t(k)]) ++cnt[size_t(label[size_t(k)])];
            for (size_t k = 1; k < cnt.size(); ++k)
                if (cnt[k] > bestN) { bestN = cnt[k]; best = int(k); }
            if (best) {
                double sx = 0, sz = 0;
                int deepest = 0;
                TerrainMemo m2;
                for (int k = 0; k < nx * nz; ++k) {
                    if (label[size_t(k)] != best) continue;
                    const int i = k % nx, j = k / nx;
                    const float x = b.cx - insetX + float(i) * step;
                    const float z = -0.5f * span + float(j) * step;
                    sx += double(x);
                    sz += double(z);
                    const int vi = int(std::lround(x / VOXEL_M));
                    const int vj = int(std::lround(z / VOXEL_M));
                    int wy = 0;
                    if (terrain.lakeColumn(vi, vj, m2, &wy))
                        deepest = std::max(deepest, wy - terrain.heightVox(vi, vj, m2));
                }
                std::printf("  BIGGEST BODY centred (%.0f, %.0f), %.0f m2, deepest %d voxels"
                            " (%.2f m)\n",
                            sx / double(bestN), sz / double(bestN),
                            double(bestN) * cellArea, deepest, double(deepest) * VOXEL_M);
            }
        }
        std::sort(sizes.begin(), sizes.end(), std::greater<size_t>());

        // THE NUMBER THAT ACTUALLY SAYS "PUDDLES": how much of the water sits
        // in bodies too small to be worth walking to. A COUNT of small bodies
        // on its own is misleading -- nine hundred specks holding 2% of the
        // water are litter round the edges of real lakes, while nine hundred
        // holding half of it are the lake system shattered.
        size_t nPonds = 0, inPonds = 0, inPuddles = 0;
        for (size_t c : sizes) {
            if (double(c) * cellArea >= kPondM2) {
                ++nPonds;
                inPonds += c;
            } else {
                inPuddles += c;
            }
        }
        (void)inPonds;
        std::printf("=== %s ===\n", b.name);
        std::printf("  %.3f%% wet, %zu bodies, %zu of them over %.0f m2\n",
                    100.0 * double(nWet) / double(size_t(nx) * size_t(nz)), sizes.size(), nPonds,
                    kPondM2);
        std::printf("  shore: %zu dry sandy columns per 100 wet (%zu of them within 2 voxels"
                    " of the line)\n",
                    nWet ? nSand * 100 / nWet : 0, nNearMiss);
        std::printf("  water in puddles (under %.0f m2): %.1f%%\n", kPondM2,
                    nWet ? 100.0 * double(inPuddles) / double(nWet) : 0.0);
        if (!sizes.empty()) {
            std::printf("  largest %.0f m2", double(sizes[0]) * cellArea);
            for (size_t k = 1; k < sizes.size() && k < 6; ++k)
                std::printf(", %.0f", double(sizes[k]) * cellArea);
            std::printf(" m2\n");
            std::printf("  median body %.0f m2\n", double(sizes[sizes.size() / 2]) * cellArea);
        }
        // A HISTOGRAM, because the mean of a distribution this skewed says
        // nothing at all. The buckets are what a player reads standing in
        // front of them: a wet patch, a puddle, a pond, a lake.
        const double lo[5] = {0.0, 100.0, kPondM2, 2000.0, 10000.0};
        const char *nm[5] = {"under 100 m2", "100-400 m2  ", "400-2k m2   ", "2k-10k m2   ",
                             "over 10k m2 "};
        for (int k = 0; k < 5; ++k) {
            const double hi = (k < 4) ? lo[k + 1] : 1e18;
            size_t cnt = 0, area = 0;
            for (size_t c : sizes) {
                const double a = double(c) * cellArea;
                if (a >= lo[k] && a < hi) {
                    ++cnt;
                    area += c;
                }
            }
            std::printf("      %s : %4zu bodies, %5.1f%% of the water\n", nm[k], cnt,
                        nWet ? 100.0 * double(area) / double(nWet) : 0.0);
        }
        std::printf("\n");
    }
    std::fflush(stdout);
    return 0;
}
