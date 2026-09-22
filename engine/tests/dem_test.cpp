// ---------------------------------------------------------------------------
// dem_test.cpp -- is the ground the real Colorado, and is it walkable?
//
//   g++ -std=c++20 -O2 -I src tests/dem_test.cpp -o build/dem_test.exe
//   ./build/dem_test.exe [path.vbdem]
//
// FOUR QUESTIONS, and only the first is the obvious one.
//
// 1. IS IT LOADED. Cheap, and it is the one failure that announces itself.
//
// 2. IS IT THE RIGHT WAY UP AND THE RIGHT WAY ROUND. A north-up grid sampled
//    with the row axis flipped gives a mirrored landscape that is smooth,
//    plausible, mountainous and WRONG, and nothing about looking at it says so.
//    The only check that can fail is against a summit whose position is known
//    independently: Mount Elbert, 4401 m, -106.4453 39.1178. worldOf() says
//    where that should be in world metres; heightM has to agree there and
//    nowhere else.
//
// 3. IS IT CONTINUOUS. The engine quantises to 10 cm and meshes a surface. A
//    one-voxel column step is a cliff face the mesher has to draw and the
//    player has to climb, and the terrain_step_test found a 7.31 m wall in the
//    procedural field exactly this way. Real terrain has real cliffs, so the
//    number here is not "must be zero" -- it is "must match the source", and a
//    step far larger than the DEM's own 10.29 m posting gradient means the
//    interpolation is broken, not that Colorado is.
//
// 4. DOES IT REACH THE ENGINE. heightVox is what the mesher and the collider
//    actually call. If heightM is right and heightVox is not, the quantisation
//    is where it broke.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>

#include "scene/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    const std::string path =
        argc > 1 ? argv[1] : "assets/dem/elbert40.vbdem";

    VoxelTerrain terrain;
    if (!terrain.loadDem(path)) {
        printf("FAIL  could not load %s\n  %s\n", path.c_str(), terrain.dem().err());
        return 1;
    }
    const DemField &d = terrain.dem();
    printf("loaded  %s\n", path.c_str());
    printf("  grid    %d x %d\n", d.w(), d.h());
    printf("  source  %.1f .. %.1f m above sea level (relief %.1f m)\n",
           d.minM(), d.maxM(), d.reliefM());
    printf("  datum   %.1f m subtracted -> world ground %.1f .. %.1f m\n",
           d.datum(), d.minM() - d.datum(), d.maxM() - d.datum());
    printf("  extent  %.2f km x %.2f km, world origin at its centre\n",
           d.spanX() / 1000.0f, d.spanZ() / 1000.0f);

    // ---------------------------------------------------------------- 2. aim
    // Mount Elbert, and three more with enough separation that a mirror or a
    // transpose cannot satisfy all of them at once.
    struct Probe { const char *name; double lon, lat; float trueM; };
    const Probe probes[] = {
        {"Mount Elbert",   -106.4453, 39.1178, 4401.0f},
        {"Mount Massive",  -106.4757, 39.1875, 4398.0f},
        {"La Plata Peak",  -106.4729, 39.0294, 4372.0f},
        {"Leadville",      -106.2925, 39.2508, 3094.0f},
    };
    printf("\naim -- world position of a known summit, and the height there\n");
    int bad = 0;
    for (const Probe &p : probes) {
        float wx = 0, wz = 0;
        d.worldOf(p.lon, p.lat, &wx, &wz);
        const float got = terrain.heightM(wx, wz) + d.datum();  // back to sea level
        const float err = got - p.trueM;
        const bool inside = std::fabs(wx) <= d.spanX() * 0.5f && std::fabs(wz) <= d.spanZ() * 0.5f;
        printf("  %-14s world (%8.1f, %8.1f) m   %7.1f m   err %+6.1f  %s%s\n",
               p.name, wx, wz, got, err, inside ? "" : "[OUTSIDE WINDOW] ",
               std::fabs(err) < 30.0f ? "ok" : "SUSPECT");
        if (inside && std::fabs(err) >= 30.0f) ++bad;
    }

    // --------------------------------------------------------- 3. continuity
    // One-voxel steps across a band through the middle of the window. The
    // source's own worst gradient is the yardstick: 10.29 m between postings,
    // so even a 45-degree mountainside is ~1.03 m per posting and ~0.10 m per
    // voxel. Anything in the tens of metres is a bug in here, not a cliff.
    printf("\ncontinuity -- worst one-voxel step, 4 km band, 10 cm columns\n");
    float worst = 0.0f; float worstAt[2] = {0, 0};
    double sum = 0; long n = 0;
    for (float z = -2000.0f; z <= 2000.0f; z += 10.0f) {
        float prev = terrain.heightM(-2000.0f, z);
        for (float x = -2000.0f + VOXEL_M; x <= 2000.0f; x += VOXEL_M) {
            const float hh = terrain.heightM(x, z);
            const float step = std::fabs(hh - prev);
            if (step > worst) { worst = step; worstAt[0] = x; worstAt[1] = z; }
            sum += step; ++n;
            prev = hh;
        }
    }
    printf("  worst %.3f m at (%.1f, %.1f)   mean %.4f m over %ld steps\n",
           worst, worstAt[0], worstAt[1], sum / double(n), n);
    const bool stepOk = worst < 5.0f;
    printf("  %s\n", stepOk ? "ok -- no wall" : "SUSPECT -- that is a wall, not a hillside");

    // ------------------------------------------------------------ 4. the API
    // heightVox is what the mesher calls. Sample a coarse grid and report the
    // spread in voxels, which is the number that says whether the world has
    // mountains in it at all.
    printf("\nengine -- heightVox over the window\n");
    int lo = 1 << 30, hi = -(1 << 30);
    for (int j = -1800; j <= 1800; j += 37) {
        for (int i = -1800; i <= 1800; i += 37) {
            const int v = terrain.heightVox(i * 10, j * 10);
            lo = std::min(lo, v); hi = std::max(hi, v);
        }
    }
    printf("  heightVox %d .. %d   (%.1f m of relief, %d voxels)\n",
           lo, hi, (hi - lo) * VOXEL_M, hi - lo);

    const bool pass = bad == 0 && stepOk && (hi - lo) > 5000;
    printf("\n%s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
