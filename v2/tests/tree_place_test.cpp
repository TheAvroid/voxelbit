// ---------------------------------------------------------------------------
// tree_place_test.cpp -- does a tree stand ON the real ground, or over it?
//
//   g++ -std=c++20 -O2 -I src tests/tree_place_test.cpp -o build/tree_place_test.exe
//   ./build/tree_place_test.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// THE QUESTION THIS ASKS. A tree is planted from ONE column's height, and then
// its trunk occupies a footprint several metres across. On the old invented
// landform that was safe: 90 m of relief spread over kilometres is nearly flat
// under anything tree-sized. Measured Colorado is not -- and the dataset is
// shrunk 6x, which multiplies every grade by six. A 30% real slope becomes
// 180% in world terms, and a trunk planted at the uphill corner of its own
// footprint hangs in the air at the downhill one.
//
// So this measures the DROP ACROSS A FOOTPRINT at exactly the columns the
// scatter is allowed to plant on -- not everywhere, because the cover and the
// slope gate already refuse most of the steep ground, and a number averaged
// over terrain no tree can stand on says nothing about the trees.
//
// TREES DO NOT FLOAT, AND THIS TEST ORIGINALLY CLAIMED THEY DID. scatter()
// already calls groundDrop() over the rotated footprint and sinks the model by
// `drop - base` (chunks.h, the extraSink block), which is the same machinery
// the boulders use. So the gap is closed by construction and the raw drop is
// NOT the defect -- measuring it and calling it a failure was measuring the
// terrain and blaming the engine.
//
// WHAT IS ACTUALLY AT RISK IS THE OPPOSITE: over-burial. extraSink is
// uncapped, so a trunk on a 6 m drop is pushed 6 m into the hill, and whatever
// of the model sat in that 6 m -- the flare of the base, the lowest branches --
// is now underground. That is what this measures.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>

#include "world/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    const std::string dem = argc > 1 ? argv[1] : "assets/dem/cheesman30.vbdem";
    const std::string cov = argc > 2 ? argv[2] : "assets/dem/cheesman30.vbcov";
    const float shrink = argc > 3 ? float(atof(argv[3])) : 6.0f;

    VoxelTerrain t;
    if (!t.loadDem(dem, 20.0f, shrink, 1.0f)) {
        printf("FAIL  dem: %s\n", t.dem().err());
        return 1;
    }
    const bool haveCover = t.loadCover(cov);
    printf("dem     %s  (shrink %.1f)\n", dem.c_str(), shrink);
    printf("cover   %s\n", haveCover ? cov.c_str() : "(none)");

    // A pine's trunk is about 2 m across at the base at this scale; the canopy
    // is wider but only the base has to touch the ground.
    const float kFootM = 2.0f;
    const float kBadM = 2.0f;   // sunk this far and the trunk base is buried

    long eligible = 0, buried = 0, considered = 0;
    double sumDrop = 0;
    float worst = 0.0f, worstAt[2] = {0, 0};
    std::vector<int> hist(12, 0);

    const float span = t.dem().spanX() * 0.5f - 50.0f;
    for (float z = -span; z <= span; z += 7.3f) {
        for (float x = -span; x <= span; x += 7.3f) {
            ++considered;
            const int i = int(std::lround(x / VOXEL_M)), j = int(std::lround(z / VOXEL_M));
            // the gates the scatter itself applies, in the same order
            const int h = t.heightVox(i, j);
            const int slope = std::max(std::abs(t.heightVox(i + 1, j) - t.heightVox(i - 1, j)),
                                       std::abs(t.heightVox(i, j + 1) - t.heightVox(i, j - 1)));
            if (slope >= VoxelTerrain::kTreeSlope) continue;
            if (t.treeRejectedByAltitude(i, j, h)) continue;
            if (!t.coverAllowsTree(x, z)) continue;
            ++eligible;

            // drop across the footprint: the planted height against the lowest
            // ground any part of the base sits over
            int lo = h;
            for (int dz = -1; dz <= 1; ++dz)
                for (int dx = -1; dx <= 1; ++dx) {
                    const int ii = i + int(dx * kFootM * 0.5f / VOXEL_M);
                    const int jj = j + int(dz * kFootM * 0.5f / VOXEL_M);
                    lo = std::min(lo, t.heightVox(ii, jj));
                }
            const float drop = float(h - lo) * VOXEL_M;
            sumDrop += drop;
            if (drop > worst) { worst = drop; worstAt[0] = x; worstAt[1] = z; }
            const int b = std::min(11, int(drop / 0.25f));
            hist[size_t(b)]++;
            if (drop > kBadM) ++buried;
        }
    }

    printf("\ncolumns swept        %ld\n", considered);
    printf("a tree may stand on  %ld  (%.1f%%)\n", eligible,
           100.0 * double(eligible) / double(considered ? considered : 1));
    if (!eligible) { printf("\nFAIL -- nothing is plantable; the gates are wrong\n"); return 1; }
    printf("\nhow far scatter() must SINK a tree to close the gap under a %.1f m\n", kFootM);
    printf("footprint. extraSink already matches this, so none of it is daylight:\n");
    printf("  mean  %.3f m\n", sumDrop / double(eligible));
    printf("  worst %.3f m at (%.0f, %.0f)\n", worst, worstAt[0], worstAt[1]);
    printf("  sunk over %.2f m: %ld  (%.2f%% of plantable columns)\n", kBadM, buried,
           100.0 * double(buried) / double(eligible));
    printf("\n  distribution, 0.25 m buckets:\n");
    for (size_t b = 0; b < hist.size(); ++b)
        if (hist[b])
            printf("    %4.2f-%4.2f m %8d  %5.1f%%\n", double(b) * 0.25, double(b + 1) * 0.25,
                   hist[b], 100.0 * hist[b] / double(eligible));

    const bool pass = 100.0 * double(buried) / double(eligible) < 5.0;
    printf("\n%s\n", pass ? "PASS -- sunk onto the ground, not buried in it"
                          : "FAIL -- too many trunks are sunk far enough to bury their base");
    return pass ? 0 : 1;
}
