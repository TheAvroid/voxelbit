// ---------------------------------------------------------------------------
// terrain_step_test -- THE HEIGHT FIELD HAS NO CLIFF IN IT, ANYWHERE.
//
//   g++ -std=c++20 -O2 -I src tests/terrain_step_test.cpp -o build/terrain_step_test.exe
//   ./build/terrain_step_test.exe [span m] [z rows]
//
// WHY THIS EXISTS. A wall 7.3 m tall stood across a lake in the birch wood
// (user 2026-09-13, "getting a wall in the lake in the birch"). heightM carved
// the pine landform, then lerped the carved result toward an UNCARVED birch
// field -- except above mix >= 0.999, which took an early return with the full
// carve applied. So the basin's depth was scaled by (1 - mix) through the seam
// and snapped back to full at one value of mix, and mix is a pure function of
// x, so the snap is a plane of constant x. Every basin that lay across one of
// those planes was half a lake with a cliff down its side.
//
// NOTHING CAUGHT IT. water_survey's shore test looks at columns beside water at
// a 4 m stride; there are four of these lines in 3.2 km and they have to cross
// a shoreline to be sampled at all. The parity and A/B tests compare the mesher
// against a model of the mesher, and the mesher was faithfully drawing the
// wall it was given. The bug was in the FIELD, so the test has to be too.
//
// WHAT IT ASSERTS. The terrain is a landscape: adjacent voxel columns may
// differ by the slope of a hillside and no more. Anything past kCliffM is a
// discontinuity -- a threshold somewhere upstream that a continuous field
// stepped across -- and it will read as a wall whatever the mesher does with
// it. The offenders are ranked by x and printed with birchMix and bandDist
// beside them, because a threshold in the band machinery is by far the most
// likely cause and those two numbers name it immediately.
//
// It samples voxel-dense in x (the axis every band threshold lives on) across
// sparse rows of z, which is the cheapest shape that can see a plane of
// constant x. A wall on a plane of constant z would be a new kind of bug; the
// z pass is here so it would not be a silent one.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

#include "scene/voxelworld.h"

using namespace v2;

static int fails = 0;

static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

// The terrain's own gradient reaches about 0.5 m across one 10 cm column where
// the fine octave sits on a steep hillside -- measured, 25.6 M columns, and the
// top of that distribution is a smooth tail with nothing standing out of it.
// One metre is twice that: comfortably clear of the landscape, and a twentieth
// of the wall this test was written for.
static constexpr float kCliffM = 1.0f;

int main(int argc, char **argv) {
    const float span = (argc > 1) ? float(std::atof(argv[1])) : 3200.0f;
    const int rows = (argc > 2) ? std::atoi(argv[2]) : 200;
    const float step = VOXEL_M;  // one column, which is the only stride that
                                 // can see a one-column step
    const int n = int(span / step);
    const float half = 0.5f * span;
    const int rowStride = std::max(1, n / std::max(1, rows));

    VoxelTerrain terrain;

    std::printf("terrain_step_test -- %.0f m square, %d columns of x per row, "
                "%d rows, cliff = %.2f m\n",
                double(span), n, n / rowStride, double(kCliffM));

    // ----------------------------------------------------------------- x
    std::vector<double> worstAtX(n, 0.0);
    double worstX = 0.0;
    float wx = 0.0f, wz = 0.0f;
    int overX = 0;
    long long samples = 0;
    for (int j = 0; j < n; j += rowStride) {
        const float z = -half + float(j) * step;
        float prev = terrain.heightM(-half, z);
        for (int i = 1; i < n; ++i) {
            const float x = -half + float(i) * step;
            const float h = terrain.heightM(x, z);
            const double d = std::fabs(h - prev);
            ++samples;
            if (d > worstAtX[i]) worstAtX[i] = d;
            if (d > kCliffM) ++overX;
            if (d > worstX) { worstX = d; wx = x; wz = z; }
            prev = h;
        }
    }

    std::printf("\n=== across x, the axis the bands live on ===\n");
    std::printf("  %lld column pairs, %d over %.2f m\n", samples, overX, double(kCliffM));
    std::printf("  worst %.2f m at x %.1f z %.1f   birchMix %.6f   bandDist %.1f m\n", worstX,
                double(wx), double(wz), double(VoxelTerrain::birchWeight(wx)),
                double(VoxelTerrain::bandDist(wx)));

    // Ranked by x, because a threshold shows up as several rows all failing on
    // the SAME x -- and one x repeating is the signature that separates a
    // discontinuity from a steep hillside.
    std::vector<std::pair<double, int>> rank;
    rank.reserve(size_t(n));
    for (int i = 0; i < n; ++i) rank.push_back({worstAtX[i], i});
    std::sort(rank.rbegin(), rank.rend());
    std::printf("\n  worst column of x, ranked (a repeated bandDist is a threshold)\n");
    std::printf("  %10s %9s %10s %11s\n", "x", "step m", "birchMix", "bandDist");
    for (int k = 0; k < 8 && k < int(rank.size()); ++k) {
        const float x = -half + float(rank[k].second) * step;
        std::printf("  %10.2f %9.2f %10.6f %11.2f\n", double(x), rank[k].first,
                    double(VoxelTerrain::birchWeight(x)), double(VoxelTerrain::bandDist(x)));
    }
    std::printf("\n  %s\n", chk(overX == 0, "no cliff across x -- ok",
                                "*** a wall stands somewhere on a line of x ***"));

    // ----------------------------------------------------------------- z
    // Nothing in the terrain thresholds on z, so this pass is here to say so
    // rather than to check a known risk.
    double worstZ = 0.0;
    float zx = 0.0f, zz = 0.0f;
    int overZ = 0;
    for (int i = 0; i < n; i += rowStride) {
        const float x = -half + float(i) * step;
        float prev = terrain.heightM(x, -half);
        for (int j = 1; j < n; ++j) {
            const float z = -half + float(j) * step;
            const float h = terrain.heightM(x, z);
            const double d = std::fabs(h - prev);
            if (d > kCliffM) ++overZ;
            if (d > worstZ) { worstZ = d; zx = x; zz = z; }
            prev = h;
        }
    }
    std::printf("\n=== across z ===\n");
    std::printf("  %d over %.2f m, worst %.2f m at x %.1f z %.1f\n", overZ, double(kCliffM),
                worstZ, double(zx), double(zz));
    std::printf("  %s\n", chk(overZ == 0, "no cliff across z -- ok",
                              "*** a wall stands somewhere on a line of z ***"));

    std::printf("\n%s (%d failures)\n", fails ? "FAILED" : "PASSED", fails);
    return fails ? 1 : 0;
}
