// ---------------------------------------------------------------------------
// lake_cover_test.cpp -- does a lake the PHOTOGRAPH found actually hold water?
//
//   g++ -std=c++20 -O2 -I src tests/lake_cover_test.cpp -o build/lake_cover_test.exe
//   ./build/lake_cover_test.exe [name]     (default: the app's own demPath)
//
// (user 2026-09-18: "I think you are misidentifying the blue terrain as clay or
//  something instead of water. make the blue terrain water.")
//
// SILT IS THE CLAY. Where the imagery says Water, topMaterial returns mat::SILT
// -- that is the lake BED, and it is only ever meant to be seen THROUGH water,
// because heightM drops the bed and lakeLineAt puts the surface back at the
// DEM's own level so the mesher can fill between them. Bare silt on the surface
// means one of those three disagreed with the other two.
//
// So this asks all three at once, at every mapped-water sample:
//
//     cover says Water   ->   is the column WET, and how deep?
//     cover says Water   ->   what does topMaterial actually return?
//     cover does NOT say Water, but the column is wet -> the procedural lakes
//
// GPU-FREE. world/voxelworld.h has no Falcor in it, so the whole 50 km window
// can be probed in a second without building the engine.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "world/voxelworld.h"

using namespace v2;

int main(int argc, char **argv) {
    const std::string name = argc > 1 ? argv[1] : "rmnp50";
    const std::string dem = "assets/dem/" + name + ".vbdem";
    const std::string cov = "assets/dem/" + name + ".vbcov";

    VoxelTerrain terrain;
    // The app's own defaults -- app.h demBaseM / demScale / demExag. A probe at
    // a different scale is a probe of a different world.
    if (!terrain.loadDem(dem, 20.0f, 6.0f, 1.0f)) {
        printf("FAIL  could not load %s\n  %s\n", dem.c_str(), terrain.dem().err());
        return 1;
    }
    if (!terrain.loadCover(cov)) {
        printf("FAIL  could not load %s\n", cov.c_str());
        return 1;
    }
    const DemField &d = terrain.dem();
    printf("%s: dem %d x %d, world %.2f x %.2f km, cover %s\n", name.c_str(), d.w(), d.h(),
           d.spanX() / 1000.0f, d.spanZ() / 1000.0f,
           terrain.usingCover() ? "loaded" : "MISSING");

    // The world is centred on the origin and is spanX/shrink across.
    // spanX() IS ALREADY WORLD METRES -- DemField divides by the shrink when it
    // loads. Dividing again here sampled the middle 1.39 km of an 8.3 km world,
    // which is 2.8% of the area, and reported 30 water samples out of 810,000.
    const float halfX = 0.5f * d.spanX();
    const float halfZ = 0.5f * d.spanZ();
    const int kN = 900;   // samples a side -- ~9 m apart over a 8.3 km world

    long long water = 0, wet = 0, dry = 0, silt = 0, otherMat = 0;
    long long proceduralWet = 0;
    std::vector<int> depths;
    std::vector<float> shores;
    // A few dry ones to print, because a count does not say WHERE.
    struct Bad { float x, z; int h, line, mat; float shore; };
    std::vector<Bad> bad;

    TerrainMemo memo;
    for (int a = 0; a < kN; ++a)
        for (int b = 0; b < kN; ++b) {
            const float x = -halfX + (2.0f * halfX) * (float(a) + 0.5f) / float(kN);
            const float z = -halfZ + (2.0f * halfZ) * (float(b) + 0.5f) / float(kN);
            const int i = int(std::floor(x / VOXEL_M));
            const int j = int(std::floor(z / VOXEL_M));
            const int h = terrain.heightVox(i, j, memo);
            const int line = terrain.lakeLineAt(x, z, memo);
            const bool isWet = terrain.wetColumn(i, j, h, line, memo);
            const bool mapped =
                terrain.cover().ok() && terrain.cover().at(x, z) == CoverField::Water;
            if (mapped) {
                ++water;
                if (isWet) {
                    ++wet;
                    depths.push_back(line - h);
                    shores.push_back(terrain.cover().shoreDistance(x, z));
                } else {
                    ++dry;
                    if (bad.size() < 10)
                        bad.push_back({x, z, h, line, int(terrain.topMaterial(i, j, h, 0, memo)),
                                       terrain.cover().shoreDistance(x, z)});
                }
                const uint8_t m = terrain.topMaterial(i, j, h, 0, memo);
                if (m == mat::SILT) ++silt; else ++otherMat;
            } else if (isWet) {
                ++proceduralWet;
            }
        }

    const long long n = (long long)kN * kN;
    printf("\n%lld samples over the whole window\n", n);
    printf("  cover says WATER      %8lld  (%.2f%%)\n", water, 100.0 * double(water) / double(n));
    printf("    ...and it is WET    %8lld  (%.1f%% of them)\n", wet,
           water ? 100.0 * double(wet) / double(water) : 0.0);
    printf("    ...and it is DRY    %8lld  (%.1f%% of them)   <- bare lake bed\n", dry,
           water ? 100.0 * double(dry) / double(water) : 0.0);
    printf("    topMaterial SILT    %8lld    other %lld\n", silt, otherMat);
    printf("  wet WITHOUT cover     %8lld  (the procedural lakes)\n", proceduralWet);

    if (!depths.empty()) {
        std::sort(depths.begin(), depths.end());
        printf("  depth of the wet ones: min %d, median %d, max %d voxels\n", depths.front(),
               depths[depths.size() / 2], depths.back());
    }
    if (!shores.empty()) {
        std::sort(shores.begin(), shores.end());
        printf("  distance to shore (REAL m):  min %.1f, median %.1f, max %.1f\n",
               double(shores.front()), double(shores[shores.size() / 2]),
               double(shores.back()));
    }
    if (!bad.empty()) {
        printf("\n  mapped water that holds none -- h is the ground, line is the surface:\n");
        for (const Bad &e : bad)
            printf("    (%9.1f,%9.1f)  h %6d  line %6d  span %4d  mat %3d  shore %.1f m\n",
                   double(e.x), double(e.z), e.h, e.line, e.line - e.h, e.mat, double(e.shore));
    }

    printf("\n  %s\n", dry == 0 && water > 0
                           ? "PASS -- every mapped lake holds water."
                           : (water == 0 ? "FAIL -- the imagery found no water at all."
                                         : "FAIL -- mapped water is standing dry."));
    return 0;
}
