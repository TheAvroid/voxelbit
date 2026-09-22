// ---------------------------------------------------------------------------
// seabed_probe.cpp -- how deep is the water, and is there a bed under it?
//
//   g++ -std=c++20 -O2 -I src tests/seabed_probe.cpp -o build/seabed.exe
//   ./build/seabed.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// ("the water has missing terrain on the ocean floor" -- user 2026-09-18, of a
// shot where the shallows show a sandy bed and the deep water is flat black.)
//
// THE QUESTION THIS SETTLES is which of two very different things that black
// is. Either the bed genuinely is not there -- a hole in the generated terrain
// -- or it is there and the water is simply too deep to see through, which is
// Beer-Lambert doing exactly what it was tuned to do.
//
// So this does not look at pixels. It walks a transect across the water and
// asks the terrain itself for the bed height under every wet column, and
// prints the DEPTH distribution against what the shader's own extinction
// leaves of a white bed at that depth. lakebed_test.cpp answers "is the water
// wet all the way across"; this answers "and how far down is the bottom".
//
// waterSigma is copied from Trace.cs.slang. Duplicated deliberately and
// labelled: this is a diagnostic, and a diagnostic that silently tracks the
// thing it is measuring cannot disagree with it.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#include "world/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    const std::string demPath = argc > 1 ? argv[1] : "assets/dem/acadia10.vbdem";
    const std::string covPath = argc > 2 ? argv[2] : "assets/dem/acadia10.vbcov";
    const float shrink = argc > 3 ? float(atof(argv[3])) : 6.0f;

    VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        printf("FAIL  dem: %s\n", t.dem().err());
        return 1;
    }
    if (!t.loadCover(covPath)) {
        printf("FAIL  cover: %s\n", t.cover().err());
        return 1;
    }
    printf("dem   %s  (shrink %.1f)\ncover %s\n\n", demPath.c_str(), shrink, covPath.c_str());

    // The shader's own per-metre extinction, and the round trip a bed is seen
    // through: down to the bed and back up to the eye.
    const double sig[3] = {1.20, 0.46, 0.208};
    auto seenAt = [&](double d) { return exp(-sig[2] * 2.0 * d); };   // blue, the survivor

    // A wide sweep of the whole window rather than one line: the shot is of an
    // ocean and the interesting columns are the ones furthest from any shore.
    const int R = 2600;          // world metres out from the origin
    const int STEP = 5;
    long long wet = 0, dry = 0, noBed = 0;
    double deepest = 0.0;
    int deepI = 0, deepJ = 0;
    std::vector<long long> hist(12, 0);   // 0-1, 1-2, ... 10+, in metres

    TerrainMemo memo;
    for (int z = -R; z <= R; z += STEP)
        for (int x = -R; x <= R; x += STEP) {
            const int i = int(floorf(float(x) / VOXEL_M));
            const int j = int(floorf(float(z) / VOXEL_M));
            int wl = 0;
            if (!t.lakeColumn(i, j, memo, &wl)) {
                ++dry;
                continue;
            }
            ++wet;
            const int bed = t.heightVox(i, j, memo);
            // A bed BELOW the bottom of the world, or a column with no solid in
            // it at all, is the hole this probe is looking for.
            if (bed <= 0) {
                ++noBed;
                continue;
            }
            const double depth = double(wl - bed) * double(VOXEL_M);
            if (depth > deepest) {
                deepest = depth;
                deepI = x;
                deepJ = z;
            }
            const int b = int(std::min(11.0, std::max(0.0, depth)));
            ++hist[size_t(b)];
        }

    printf("%lld wet columns sampled, %lld dry\n", wet, dry);
    printf("columns with NO BED AT ALL: %lld", noBed);
    printf("%s\n\n", noBed ? "   <-- a real hole in the terrain" : "   (none -- the bed is there)");
    printf("deepest water %.2f m at (%d, %d)\n\n", deepest, deepI, deepJ);

    printf("  depth        columns     what the shader leaves of a white bed\n");
    for (size_t b = 0; b < hist.size(); ++b) {
        if (!hist[b]) continue;
        const double mid = double(b) + 0.5;
        const double vis = seenAt(mid) * 100.0;
        char bar[40];
        int n = int(std::min(38.0, vis * 0.38));
        for (int k = 0; k < n; ++k) bar[k] = '#';
        bar[n] = 0;
        if (b == 11)
            printf("  10+ m     %10lld      %5.2f%% blue  %s\n", hist[b], vis, bar);
        else
            printf("  %2zu-%2zu m   %10lld      %5.2f%% blue  %s\n", b, b + 1, hist[b], vis, bar);
    }
    printf("\n(the third column is exp(-2 * 0.208 * depth): the blue that survives the\n"
           " trip down to the bed and back. Red and green are gone far sooner.)\n");
    return 0;
}
