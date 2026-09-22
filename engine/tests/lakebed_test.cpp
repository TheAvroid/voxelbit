// ---------------------------------------------------------------------------
// lakebed_test.cpp -- is there generated terrain standing in the water?
//
//   g++ -std=c++20 -O2 -I src tests/lakebed_test.cpp -o build/lakebed_test.exe
//   ./build/lakebed_test.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// ("can you clean up terrain thats in water beds? leave rocks and other
// assets, but take out generated terrain in the water beds." user 2026-09-18.)
//
// TWO QUESTIONS, AND THEY PULL IN OPPOSITE DIRECTIONS.
//
//   1. IS EVERY LAKE WATER ALL THE WAY ACROSS? Walk the columns that sit on a
//      known lake's own surface and ask the engine whether each one is wet. A
//      column inside a lake that the engine thinks is dry land is a mound
//      standing in the bed, and the height it stands at says whether it is a
//      bump on the bottom or an island breaking the surface.
//
//   2. IS ANY OF THE WATER SOMEWHERE WATER CANNOT BE? The classifier reads
//      deep shadow and wet rock as water, and where it does, the engine used
//      to carve a flat lake into a mountainside. Sweep the whole window and
//      ask the DEM how level the ground under each piece of water is.
//
// Loosening anything to pass the first tightens the second, which is why both
// are here and why the numbers are printed rather than just a verdict.
//
// A LAKE IS SELECTED BY ITS SURFACE, NOT BY THE CLASS. Taking every pixel the
// imagery calls water inside a lake's radius counts the misclassified hillside
// patches near it as part of the lake, so tightening the engine against those
// made this test report a WORSE lake bed -- the fix and the measurement moving
// in opposite directions, which is the shape of a test measuring the wrong
// thing. world/poi.h has already established each lake's surface elevation, and
// a column belongs to that lake if the DEM puts it on that surface.
//
// It does NOT look at rocks, trees or any other model. Those are assets and
// they stay; this is only about the heightfield.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>
#include <vector>

#include "world/voxelworld.h"
#include "world/poi.h"

using namespace v2;

int main(int argc, char **argv) {
    const std::string demPath = argc > 1 ? argv[1] : "assets/dem/rmnp50.vbdem";
    const std::string covPath = argc > 2 ? argv[2] : "assets/dem/rmnp50.vbcov";
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
    PoiIndex poi;
    poi.build(t.dem(), t.cover());
    printf("dem     %s  (shrink %.1f)\n", demPath.c_str(), shrink);
    printf("cover   %s\n\n", covPath.c_str());

    // How far off a lake's own surface a column may be and still be that lake.
    // Wider than the 2.5 m poi.h builds the surface with, so a shoreline cell
    // the DEM rounds up is still counted rather than quietly dropped.
    const float kOnLakeM = 4.0f;

    printf("  %-10s %9s %9s %8s   %s\n", "lake", "wet cols", "DRY cols", "worst", "");
    long allWet = 0, allDry = 0, allAbove = 0;
    float worstAll = 0.0f, worstAtX = 0, worstAtZ = 0;

    for (const PoiIndex::Poi &p : poi.all()) {
        if (!p.lake) continue;
        const float rW = p.radiusM / shrink;
        long wet = 0, dry = 0, above = 0;
        float worst = 0.0f;
        TerrainMemo memo;
        for (float dz = -rW; dz <= rW; dz += 0.5f)
            for (float dx = -rW; dx <= rW; dx += 0.5f) {
                if (dx * dx + dz * dz > rW * rW) continue;
                const float x = p.x + dx, z = p.z + dz;
                // THE UNJITTERED CELL. at() blurs the sample on purpose for the
                // renderer's sake; a test that asks what the data says must not
                // ask through the thing that deliberately blurs it.
                if (t.cover().atPoint(x, z) != CoverField::Water) continue;
                // ...AND ON THIS LAKE'S SURFACE. See the header note.
                const float asl = t.dem().worldToAsl(t.dem().heightM(x, z));
                if (std::fabs(asl - p.m) > kOnLakeM) continue;
                const int i = int(std::lround(x / VOXEL_M)), j = int(std::lround(z / VOXEL_M));
                int wy = 0;
                if (t.lakeColumn(i, j, memo, &wy)) { ++wet; continue; }
                ++dry;
                const float surf = t.dem().aslToWorld(p.m);
                const float stands = float(t.heightVox(i, j)) * VOXEL_M - surf;
                if (stands > worst) worst = stands;
                if (stands > worstAll) { worstAll = stands; worstAtX = x; worstAtZ = z; }
                if (stands > 0.0f) ++above;
            }
        const double pct = 100.0 * double(dry) / double(wet + dry ? wet + dry : 1);
        printf("  %-10s %9ld %9ld %7.2f m   %s\n", p.name.c_str(), wet, dry, worst,
               dry == 0 ? "clean"
                        : (above ? "ISLANDS BREAKING THE SURFACE" : "mounds on the bed"));
        if (dry) printf("  %-10s %9s %8.2f%% of its own surface is dry land\n", "", "", pct);
        allWet += wet;
        allDry += dry;
        allAbove += above;
    }

    const double pct = 100.0 * double(allDry) / double(allWet + allDry ? allWet + allDry : 1);
    printf("\n  %ld columns on a lake surface, %ld of them dry land (%.2f%%)\n", allWet + allDry,
           allDry, pct);
    printf("  %ld of those stand ABOVE the surface -- islands, not bed\n", allAbove);
    if (allDry)
        printf("  worst %.2f m above the line at (%.0f, %.0f)\n", worstAll, worstAtX, worstAtZ);

    // ---- AND THE OTHER DIRECTION: WATER STANDING ON A HILLSIDE -------------
    long steep = 0;
    {
        printf("\n  -- and the other way: is any of the water on a slope? --\n");
        const float span = t.dem().spanX() * 0.5f - 20.0f;
        static const float kCut[8] = {0.02f, 0.05f, 0.10f, 0.15f, 0.20f, 0.30f, 0.50f, 1.00f};
        long hist[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        long wet = 0, refused = 0;
        float worstGrade = 0.0f, atX = 0, atZ = 0;
        for (float z = -span; z <= span; z += 3.0f)
            for (float x = -span; x <= span; x += 3.0f) {
                const bool claimed = t.cover().atPoint(x, z) == CoverField::Water;
                if (!t.mappedWater(x, z)) {
                    if (claimed) ++refused;
                    continue;
                }
                ++wet;
                // Grade over 20 real m, in real metres per real metre, so the
                // number means what it would on a map. Measured over the whole
                // neighbourhood INCLUDING the bank -- unlike the engine's gate,
                // which only looks across water. That is deliberate: if the two
                // measured the same thing this could not fail.
                const float d = 20.0f / shrink;
                const float a1 = t.dem().worldToAsl(t.dem().heightM(x - d, z));
                const float a2 = t.dem().worldToAsl(t.dem().heightM(x + d, z));
                const float b1 = t.dem().worldToAsl(t.dem().heightM(x, z - d));
                const float b2 = t.dem().worldToAsl(t.dem().heightM(x, z + d));
                const float g = std::max(std::fabs(a2 - a1), std::fabs(b2 - b1)) / 40.0f;
                if (g > worstGrade) { worstGrade = g; atX = x; atZ = z; }
                if (g > 0.30f) ++steep;
                for (int b = 0; b < 8; ++b)
                    if (g > kCut[b]) ++hist[b];
            }
        printf("  %ld water samples in the window; %ld water-classified samples refused\n", wet,
               refused);
        printf("  steepest water sits on a %.0f%% grade at (%.0f, %.0f)\n", worstGrade * 100.0f,
               atX, atZ);
        for (int b = 0; b < 8; ++b)
            printf("      steeper than %3.0f%%: %8ld  (%5.2f%%)\n", kCut[b] * 100.0f, hist[b],
                   100.0 * double(hist[b]) / double(wet ? wet : 1));
    }

    // ---- WHAT COUNTS AS CLEAN, AND WHY THESE NUMBERS -------------------
    //
    // The bar is what a player can SEE, because that is what was reported. A
    // sub-metre lump on a bed under five metres of water is not visible and is
    // not what "terrain in the water beds" meant; a wall standing 22 m out of
    // the surface is. So the height is the real gate and the percentage is the
    // supporting one.
    //
    // WHERE IT STARTED, for anyone tempted to loosen these: 2.42% of lake
    // surface was dry land, the worst of it 22.72 m above the water, and 1,685
    // samples of water stood on ground steeper than 30% -- one patch on a 212%
    // grade. What is left is 0.78%, nothing over 1.12 m, and 22 steep samples
    // out of 131,052. The residue is reservoir drawdown terracing, where the
    // DEM genuinely is not level inside its own shoreline.
    const bool pass = worstAll < 1.5f && pct < 1.0 &&
                      100.0 * double(steep) / double(allWet ? allWet : 1) < 0.05;
    printf("\n%s\n", pass ? "PASS -- the beds are water all the way across, and none of it is "
                            "standing on a hillside"
                          : "FAIL -- see the numbers above");
    return pass ? 0 : 1;
}
