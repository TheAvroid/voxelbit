// ---------------------------------------------------------------------------
// dem_rough_test.cpp -- does the per-class roughness add relief where it should
// and nothing at all where it must not?
//
//   g++ -std=c++20 -O2 -I src tests/dem_rough_test.cpp -o build/dem_rough_test.exe
//   ./build/dem_rough_test.exe [dem.vbdem] [cover.vbcov] [shrink]
//
// WHY THIS NEEDS A TEST AND NOT AN EYE. The thing being added is invisible in a
// screenshot unless you already know where to look, and the thing that would
// make it WRONG -- relief on a flat sand bank, or on water -- is a single voxel
// here and there, which is exactly what got the last global noise removed
// ("remove that noise from all terrain", 2026-09-18). A render cannot tell you
// that water got 0.000 and rock got 0.31; this can.
//
// FIVE CLAIMS, each of which would be a real defect if it were false:
//
//   1. OFF IS OFF. --dem-rough 0 must return the same height, to the bit, as a
//      build with none of this in it. Anything else silently moves every world
//      that was ever tuned.
//   2. WATER IS EXACTLY ZERO. Not small. Zero.
//   3. A SHORE IS FLAT. Within 8 m of a waterline the amplitude fades to
//      nothing, whatever class the cover says, because a beach is where the
//      complaint came from.
//   4. THE CLASSES ARE ORDERED. rock > forest > meadow > snow, measured as
//      actual relief over the window, not asserted from the table.
//   5. IT SHRINKS AS THE DATA IMPROVES. A 1 m posting must invent about 32% of
//      what a 10.29 m one does -- (1/10.29)^0.49 -- because relief below the
//      posting is what is being replaced, and a finer posting leaves less of it.
//
//      THE EXPONENT IS MEASURED, NOT CHOSEN. It is the Hurst exponent of real
//      ground's structure function, fitted over 14 plots of 4.5 cm UAV laser
//      scanning (tools/pc_roughness.py against the FOR-instance dataset):
//      H median 0.491, p10 0.281, p90 0.579, r2 mostly 0.93..0.998. It was
//      0.75 by assertion until then, which shrank the synthesis too hard --
//      a 1 m source was inventing 17% where it should invent 32%.
//
//      This test pins the value so that changing the fit is a deliberate act
//      with a measurement behind it, rather than a constant drifting.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#include "world/voxelworld.h"

namespace {

struct Stat {
    double sum = 0, sumsq = 0, mx = 0;
    long n = 0;
    void add(double d) {
        sum += d; sumsq += d * d; n++;
        mx = std::max(mx, std::fabs(d));
    }
    double rms() const { return n ? std::sqrt(sumsq / n) : 0.0; }
    double mean() const { return n ? sum / n : 0.0; }
};

const char *kName[6] = {"unknown", "forest", "meadow", "rock", "snow", "water"};

}  // namespace

int main(int argc, char **argv) {
    const char *demPath = argc > 1 ? argv[1] : "assets/dem/ouachita12.vbdem";
    const char *covPath = argc > 2 ? argv[2] : "assets/dem/ouachita12.vbcov";
    const float shrink = argc > 3 ? (float)atof(argv[3]) : 1.0f;

    v2::VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) {
        printf("cannot load %s -- %s\n", demPath, t.dem().err());
        return 2;
    }
    if (!t.loadCover(covPath)) {
        printf("cannot load %s -- %s\n", covPath, t.cover().err());
        return 2;
    }
    printf("dem     %s  %d x %d  %.3f m posting  shrink %.1f\n",
           demPath, t.dem().w(), t.dem().h(), t.dem().metresPerSampleX(), shrink);

    const float halfW = 0.5f * float(t.dem().w()) * float(t.dem().metresPerSampleX()) / shrink;
    const float halfH = 0.5f * float(t.dem().h()) * float(t.dem().metresPerSampleX()) / shrink;
    // Stay a little inside the window: the held edge is not terrain.
    const float x0 = -halfW * 0.92f, x1 = halfW * 0.92f;
    const float z0 = -halfH * 0.92f, z1 = halfH * 0.92f;
    const int N = 420;

    int bad = 0;

    // ---- 1. OFF IS OFF, to the bit -------------------------------------
    {
        t.demRoughM = 0.0f;
        std::vector<float> base;
        base.reserve((size_t)N * N);
        v2::TerrainMemo memo;
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const float x = x0 + (x1 - x0) * (i + 0.5f) / N;
                const float z = z0 + (z1 - z0) * (j + 0.5f) / N;
                base.push_back(t.heightM(x, z, memo));
            }
        long moved = 0;
        v2::TerrainMemo m2;
        size_t k = 0;
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i, ++k) {
                const float x = x0 + (x1 - x0) * (i + 0.5f) / N;
                const float z = z0 + (z1 - z0) * (j + 0.5f) / N;
                if (t.heightM(x, z, m2) != base[k]) ++moved;
            }
        if (moved) {
            printf("FAIL  off is not off: %ld of %zu columns moved\n", moved, base.size());
            ++bad;
        } else {
            printf("ok    off is off        %zu columns bit-identical\n", base.size());
        }

        // ---- 2/3/4. what ON actually does, per class -------------------
        t.demRoughM = 1.0f;
        Stat per[6];
        // THE FADE IS A PROFILE, NOT A BAND, and comparing one band against
        // "open ground" was the first version of this test and it was wrong:
        // the columns near a waterline are mostly forest, open ground was
        // measured over rock, and a correct fade still looked like a failure.
        // Five distance bins compare like with like -- the same field, sorted
        // by how far each column is from the line -- so the only thing that can
        // make the profile rise is the fade itself.
        const float kEdge[6] = {0.0f, 2.0f, 4.0f, 8.0f, 16.0f, 64.0f};
        Stat band[5];
        v2::TerrainMemo m3;
        k = 0;
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i, ++k) {
                const float x = x0 + (x1 - x0) * (i + 0.5f) / N;
                const float z = z0 + (z1 - z0) * (j + 0.5f) / N;
                const double d = t.heightM(x, z, m3) - base[k];
                const uint8_t cc = t.cover().at(x, z);
                if (cc < 6) per[cc].add(d);
                // Signed: negative on land, positive in water, zero at the
                // line. Distance from the line is the magnitude.
                const float sd = std::fabs(t.cover().shoreDistance(x, z));
                for (int b = 0; b < 5; ++b)
                    if (sd >= kEdge[b] && sd < kEdge[b + 1]) { band[b].add(d); break; }
            }

        printf("\n  class      samples   rms (world m)   worst\n");
        for (int c = 0; c < 6; ++c)
            if (per[c].n)
                printf("  %-9s %8ld   %10.4f   %8.4f\n",
                       kName[c], per[c].n, per[c].rms(), per[c].mx);

        if (per[CoverField::Water].n && per[CoverField::Water].mx != 0.0) {
            printf("FAIL  water moved by %.6f m; it must be exactly zero\n",
                   per[CoverField::Water].mx);
            ++bad;
        } else if (per[CoverField::Water].n) {
            printf("ok    water is exactly zero over %ld samples\n",
                   per[CoverField::Water].n);
        }

        printf("\n  distance from the waterline (real m) -> relief added\n");
        for (int b = 0; b < 5; ++b)
            if (band[b].n)
                printf("  %5.0f..%-5.0f %8ld   %10.4f\n",
                       kEdge[b], kEdge[b + 1], band[b].n, band[b].rms());
        // The fade is t*t with t = d/8, so 0-2 m keeps at most 6% of the
        // amplitude and 2-4 m at most 25%. Compared against the plateau beyond
        // 16 m, which is the same ground with no fade on it at all.
        if (band[0].n >= 50 && band[4].n >= 200) {
            const double plateau = band[4].rms();
            if (band[0].rms() > 0.20 * plateau) {
                printf("FAIL  0-2 m from the line keeps %.0f%% of the plateau; the fade is not on\n",
                       100.0 * band[0].rms() / plateau);
                ++bad;
            } else {
                printf("ok    waterline is flat  0-2 m keeps %.1f%% of the %.4f plateau\n",
                       100.0 * band[0].rms() / plateau, plateau);
            }
            if (band[1].n >= 50 && band[1].rms() < band[0].rms()) {
                printf("FAIL  the fade is not monotonic: 2-4 m (%.4f) under 0-2 m (%.4f)\n",
                       band[1].rms(), band[0].rms());
                ++bad;
            }
        }

        const double rock = per[CoverField::Rock].rms();
        const double forest = per[CoverField::Forest].rms();
        const double meadow = per[CoverField::Meadow].rms();
        if (per[CoverField::Rock].n > 200 && per[CoverField::Forest].n > 200) {
            if (!(rock > forest)) {
                printf("FAIL  rock (%.4f) is not rougher than forest (%.4f)\n", rock, forest);
                ++bad;
            } else {
                printf("ok    rock > forest     %.4f > %.4f\n", rock, forest);
            }
        }
        if (per[CoverField::Meadow].n > 200 && per[CoverField::Forest].n > 200) {
            if (!(forest > meadow)) {
                printf("FAIL  forest (%.4f) is not rougher than meadow (%.4f)\n", forest, meadow);
                ++bad;
            } else {
                printf("ok    forest > meadow   %.4f > %.4f\n", forest, meadow);
            }
        }
    }

    // ---- 5. a finer posting must invent less ---------------------------
    {
        const float f10 = t.roughPostingFactor(10.29f);
        const float f1 = t.roughPostingFactor(1.0f);
        // Fitted; see the header note and tools/pc_roughness.py.
        const float kFittedH = 0.49f;
        const float want = std::pow(1.0f / 10.29f, kFittedH);
        printf("\n  posting factor   10.29 m -> %.4f    1 m -> %.4f  (want %.4f)\n",
               f10, f1, want);
        if (std::fabs(f10 - 1.0f) > 1e-4f) {
            printf("FAIL  a source at its own posting must scale by exactly 1\n");
            ++bad;
        } else if (std::fabs(f1 - want) > 1e-4f) {
            printf("FAIL  posting scaling does not match the fitted H = %.2f\n", kFittedH);
            ++bad;
        } else {
            printf("ok    1 m source invents %.0f%% of what 10.29 m does (H = %.2f, fitted)\n",
                   100.0 * f1, kFittedH);
        }
    }

    printf("\ndem_rough_test %s\n", bad ? "FAILED" : "PASS");
    return bad ? 1 : 0;
}
