// tall_grass_test -- the tufts, measured as a DENSITY rather than as a count.
//
// "Reduce the tall grass patch in half while also keeping the density." Those
// pull against each other, and the only way to know whether both happened is to
// measure strands per square metre of tuft, not strands per tuft. The engine's
// own constants are instance fields, so this runs the real strandRows over the
// real terrain twice -- once with the old numbers, once with the shipped ones --
// and prints both.
//
// It also counts what "dont put flowers on tall grass" costs: the flower scatter
// plants on any column that grew a blade, so the bill is the share of those
// columns that the tuft draw turned tall.
//
//     g++ -std=c++20 -O2 -I src tests/tall_grass_test.cpp -o build/tall_grass_test.exe
#include <cmath>
#include <cstdio>
#include <vector>

#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

struct Count {
    long columns = 0;    // columns walked
    long blades = 0;     // ...that grew a blade at all
    long tall = 0;       // ...of which are tuft blades
    double tuftArea = 0; // m^2 of tuft disc inside the square
};

// One sweep of a square, column by column, through the REAL strandRows.
static Count sweep(const VoxelTerrain &t, int i0, int j0, int n) {
    Count c;
    TerrainMemo memo;
    for (int j = j0; j < j0 + n; ++j)
        for (int i = i0; i < i0 + n; ++i) {
            const int h = t.heightVox(i, j, memo);
            const uint8_t top = t.topMaterial(i, j, h, memo);
            const int rows = t.strandRows(i, j, top, memo);
            ++c.columns;
            if (rows <= 0) continue;
            ++c.blades;
            if (t.tallStrand(rows)) ++c.tall;
        }
    return c;
}

// How much of the square is inside SOME tuft, which is the area the tall
// strands are spread over. Walked on the same lattice so the two agree.
static double tuftArea(const VoxelTerrain &t, int i0, int j0, int n) {
    long in = 0;
    for (int j = j0; j < j0 + n; ++j)
        for (int i = i0; i < i0 + n; ++i) {
            float rad = 0.0f, want = 0.0f;
            if (t.tuftAt(t.wx(i), t.wx(j), &rad, &want)) ++in;
        }
    return double(in) * double(VOXEL_M) * double(VOXEL_M);
}

static void report(const char *what, const Count &c, double area) {
    const double m2 = double(c.columns) * double(VOXEL_M) * double(VOXEL_M);
    std::printf("  %-22s blades %8ld (%.1f%% of columns)  tall %6ld  "
                "tuft area %7.0f m2  TALL PER m2 OF TUFT %6.2f\n",
                what, c.blades, 100.0 * double(c.blades) / double(c.columns), c.tall, area,
                area > 0 ? double(c.tall) / area : 0.0);
    (void)m2;
}

int main() {
    // 400 m of wood, which is 4000 columns a side: big enough to hold a few
    // hundred tuft sites at a 13 m lattice.
    const int kN = 4000;

    for (int wood = 0; wood < 2; ++wood) {
        VoxelTerrain t;
        t.forced = true;
        t.biome = wood ? Biome::Birch : Biome::Pine;
        const int i0 = wood ? 40000 : -20000, j0 = wood ? -8000 : 12000;

        std::printf("%s wood, %d x %d m\n", wood ? "BIRCH" : "PINE",
                    int(kN * VOXEL_M), int(kN * VOXEL_M));

        // ---- AS IT WAS: 1.2-2.2 m discs holding 60-120 strands ------------
        t.tuftRadMinM = 1.2f;
        t.tuftRadMaxM = 2.2f;
        t.tuftStrandsMin = 60.0f;
        t.tuftStrandsMax = 120.0f;
        const Count was = sweep(t, i0, j0, kN);
        const double aWas = tuftArea(t, i0, j0, kN);
        report("before (1.2-2.2 m)", was, aWas);

        // ---- AS IT SHIPS: half the radius, a quarter of the strands -------
        VoxelTerrain fresh;   // the defaults, untouched -- what the engine runs
        fresh.forced = true;
        fresh.biome = t.biome;
        const Count now = sweep(fresh, i0, j0, kN);
        const double aNow = tuftArea(fresh, i0, j0, kN);
        report("after  (0.6-1.1 m)", now, aNow);

        const double dWas = aWas > 0 ? double(was.tall) / aWas : 0.0;
        const double dNow = aNow > 0 ? double(now.tall) / aNow : 0.0;
        const double dens = dWas > 0 ? dNow / dWas : 0.0;
        const double area = aWas > 0 ? aNow / aWas : 0.0;
        std::printf("  tuft area is %.0f%% of what it was, and the tall grass in it is %.0f%% as "
                    "dense  %s\n",
                    100.0 * area, 100.0 * dens,
                    chk(area > 0.18 && area < 0.32 && dens > 0.94 && dens < 1.06,
                        "-- HALF THE PATCH, THE SAME DENSITY, ok", "-- WRONG"));

        // ---- ...AND WHAT IT COSTS THE FLOWERS -----------------------------
        //
        // scatterSmall plants a flower on any column that grew a blade and now
        // refuses the tall ones, so this share is exactly the loss.
        const double lostWas = 100.0 * double(was.tall) / double(was.blades);
        const double lostNow = 100.0 * double(now.tall) / double(now.blades);
        std::printf("  flower columns refused: %.2f%% of them, from %.2f%% before the tufts were "
                    "halved  %s\n\n",
                    lostNow, lostWas,
                    chk(lostNow < 1.5, "-- the bloom count barely moves, ok",
                        "-- TOO MANY FLOWERS LOST"));
    }

    // ---- AND THE BLADES THEMSELVES ARE UNTOUCHED --------------------------
    //
    // The invariant this file exists to protect, and the one that was broken
    // the first time tall grass was attempted: the tuft draw runs AFTER the
    // planting draw, so it may change how tall a blade is and never whether
    // there is one. Same square, tufts off, blade count must be identical.
    {
        VoxelTerrain a, b;
        a.forced = b.forced = true;
        a.biome = b.biome = Biome::Pine;
        b.tuftCoverage = 0.0f;   // no tufts at all
        const Count wa = sweep(a, -20000, 12000, 1200);
        const Count wb = sweep(b, -20000, 12000, 1200);
        std::printf("PLANTING IS UNTOUCHED  %ld blades with tufts, %ld without  %s\n",
                    wa.blades, wb.blades,
                    chk(wa.blades == wb.blades, "-- identical, ok", "-- THE TUFTS MOVED THE GRASS"));
    }

    std::printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
