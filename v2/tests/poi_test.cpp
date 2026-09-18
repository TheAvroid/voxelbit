// ---------------------------------------------------------------------------
// poi_test.cpp -- are the places /locate offers really there?
//
//   g++ -std=c++20 -O2 -I src tests/poi_test.cpp -o build/poi_test.exe
//   ./build/poi_test.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// WHY THIS EXISTS. The POI index is built from the loaded data, and it has
// already been wired in at the wrong point in the load twice: inside the DEM
// block it saw no water (twelve summits, no lakes), inside the cover block it
// saw no window without a cover (no places at all). Both failures print a
// perfectly plausible line and are only visible if you know what the count
// SHOULD be -- so the count is checked here rather than read off a log.
//
// It also checks the two things a name can be wrong about. A summit has to be
// higher than the ground around it and a lake has to be flat and wet, and both
// of those are re-measured from the DEM here, independently of the code that
// chose them. A place that fails its own definition is a place /locate would
// take you somewhere wrong.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <algorithm>

#include "world/dem.h"
#include "world/cover.h"
#include "world/poi.h"

int main(int argc, char **argv) {
    const std::string demPath = argc > 1 ? argv[1] : "assets/dem/rmnp50.vbdem";
    const std::string covPath = argc > 2 ? argv[2] : "assets/dem/rmnp50.vbcov";
    const float shrink = argc > 3 ? float(atof(argv[3])) : 6.0f;

    DemField dem;
    if (!dem.load(demPath, 20.0f, shrink, 1.0f)) {
        printf("FAIL  dem: %s\n", dem.err());
        return 1;
    }
    CoverField cov;
    const bool haveCover = cov.load(covPath, shrink);
    printf("dem     %s  %dx%d  %.0f..%.0f m asl  (shrink %.1f)\n", demPath.c_str(), dem.w(),
           dem.h(), dem.minM(), dem.maxM(), shrink);
    printf("cover   %s\n", haveCover ? covPath.c_str() : "(none)");

    PoiIndex poi;
    poi.build(dem, cov);
    printf("\n%d places, %d of them with a real name\n\n", int(poi.all().size()), poi.namedCount());

    int peaks = 0, lakes = 0, badPeak = 0, badLake = 0;
    const float cellM = float(dem.metresPerSampleX());
    for (const PoiIndex::Poi &p : poi.all()) {
        // Re-measure, from the data, what the entry claims to be.
        //
        //   a summit: nothing within 200 world m is higher.
        //   a lake:   you are standing ON one. The cover calls the spot water,
        //             most of a ring around it is water too, and those wet
        //             samples are at one height, because a water surface is
        //             level.
        //
        // THE RING IS SIZED FROM THE LAKE, which two earlier versions of this
        // test got wrong in opposite directions. A fixed 100 world m ring asked
        // the valley walls to be flat and failed every real mountain lake; a
        // fixed 30 m one still overshot the small tarns, which are barely 200
        // real metres across, and marked them dry. Half the radius of a disc of
        // the same area lands inside any of them.
        //
        // It reads atPoint(), not at(): at() jitters by up to a cell on purpose
        // for the renderer's sake, and a test that asks what the data says
        // should not be asking through the thing that deliberately blurs it.
        const float lo = dem.worldToAsl(dem.heightM(p.x, p.z));
        float ring = 200.0f;
        if (p.lake) {
            const float areaM2 = p.size * (4.0f * cellM) * (4.0f * cellM);
            ring = std::sqrt(areaM2 / 3.14159265f) * 0.5f / shrink;   // world metres
        }
        float worst = 0.0f;
        bool higher = false;
        int wet = 0;
        for (int k = 0; k < 16; ++k) {
            const float a = float(k) * (6.2831853f / 16.0f);
            const float sx = p.x + std::cos(a) * ring, sz = p.z + std::sin(a) * ring;
            const float v = dem.worldToAsl(dem.heightM(sx, sz));
            if (!p.lake) {
                worst = std::max(worst, std::fabs(v - lo));
                if (v > lo + 1.0f) higher = true;
            } else if (!haveCover || cov.atPoint(sx, sz) == CoverField::Water) {
                ++wet;
                worst = std::max(worst, std::fabs(v - lo));
            }
        }
        bool ok = true;
        if (p.lake) {
            ++lakes;
            const bool here = !haveCover || cov.atPoint(p.x, p.z) == CoverField::Water;
            // HALF THE RING, NOT MOST OF IT. A ring test proves you are on a
            // surface; it cannot also prove the lake is round, and the biggest
            // body in this window is a 5 km reservoir with arms, so a ring
            // drawn at half its equivalent radius crosses land on some
            // bearings. What matters is that the point is water and every wet
            // sample is at the same height.
            ok = here && wet >= 8 && worst < 5.0f;
            if (!ok) ++badLake;
            printf("  %-9s lake %6.0f m asl  at %8.0f, %8.0f  %4.0f m across, %2d/16 of the ring "
                   "wet, surface varies by %4.1f m  %s%s\n",
                   p.name.c_str(), p.m, p.x, p.z, ring * 4.0f * shrink, wet, worst,
                   p.named ? "[named] " : "",
                   ok ? "" : (here ? "  <-- FAILS ITS OWN DEFINITION"
                                   : "  <-- NOT EVEN WATER AT THE POINT"));
        } else {
            ++peaks;
            ok = !higher;
            if (!ok) ++badPeak;
            printf("  %-9s peak %6.0f m asl  at %8.0f, %8.0f  stands over %5.1f m  %s%s\n",
                   p.name.c_str(), p.m, p.x, p.z, worst, p.named ? "[named] " : "",
                   ok ? "" : "  <-- FAILS ITS OWN DEFINITION");
        }
    }

    printf("\n%d summits, %d lakes\n", peaks, lakes);
    // /locate has to be able to reach every one of them by the name it printed.
    int unreachable = 0;
    for (const PoiIndex::Poi &p : poi.all())
        if (poi.find(p.name) != &p) ++unreachable;
    printf("names /locate cannot resolve: %d\n", unreachable);

    const bool pass = peaks > 0 && (!haveCover || lakes > 0) && badPeak == 0 && badLake == 0 &&
                      unreachable == 0;
    printf("\n%s\n", pass ? "PASS -- every place is where the data says it is"
                          : "FAIL -- see the marked rows above");
    return pass ? 0 : 1;
}
