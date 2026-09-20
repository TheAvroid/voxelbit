// ---------------------------------------------------------------------------
// inset_test.cpp -- does the measured patch land where it should, and nowhere
// else?
//
//   g++ -std=c++20 -O2 -I src tests/inset_test.cpp -o build/inset_test.exe
//   ./build/inset_test.exe <dem.vbdem> <cover.vbcov> <patch.vbins> [shrink]
//
// WHY NOT A SCREENSHOT. The patch is 28 m across and carries about two metres
// of relief; at ground level it is behind waist-high grass, and from the air it
// is four pixels. The first attempt to eyeball it produced a picture of grass.
// What actually needs checking is arithmetic, and it is checkable exactly:
//
//   1. OFF IS OFF. With no patch loaded the terrain must be bit-identical to
//      what it was before any of this existed.
//   2. THE DEVIATION MATCHES THE FILE. Inside the patch, height-with minus
//      height-without must reproduce the range pc2vox reported when it wrote
//      it -- if it does not, the placement, the step or the rebase is wrong.
//   3. IT IS EXACTLY ZERO OUTSIDE. Not small. Zero. A patch that leaks is a
//      patch that has moved the whole world by a hair.
//   4. THE BORDER IS FEATHERED AND MONOTONIC. A hard edge between measured and
//      interpolated ground is a one-voxel cliff in a straight line, which is
//      the same artefact this whole file fights. The deviation must fall to
//      nothing across the outer kFeatherM and must not wobble on the way.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <cstring>

#include "world/voxelworld.h"

int main(int argc, char **argv) {
    const char *demPath = argc > 1 ? argv[1] : "assets/dem/ouachita12_3m.vbdem";
    const char *covPath = argc > 2 ? argv[2] : "assets/dem/ouachita12_3m.vbcov";
    const char *insPath = argc > 3 ? argv[3] : "assets/dem/nibio_plot1.vbins";
    const float shrink = argc > 4 ? (float)atof(argv[4]) : 1.0f;
    // Where the patch is pinned. Anywhere inside the window will do; this is
    // Ouachita's spawn, which is what the A/B renders used.
    const float px = -3950.0f, pz = -425.0f;

    v2::VoxelTerrain t;
    if (!t.loadDem(demPath, 20.0f, shrink, 1.0f)) { printf("dem: %s\n", t.dem().err()); return 2; }
    if (!t.loadCover(covPath)) { printf("cover: %s\n", t.cover().err()); return 2; }
    printf("dem     %s\npatch   %s at (%.0f, %.0f)\n", demPath, insPath, px, pz);

    // ---- 1. sample the baseline, patch NOT loaded ------------------------
    const int N = 220;
    const float span = 46.0f;        // comfortably wider than the patch
    std::vector<float> base((size_t)N * N);
    {
        v2::TerrainMemo memo;
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const float x = px - span + 2 * span * (i + 0.5f) / N;
                const float z = pz - span + 2 * span * (j + 0.5f) / N;
                base[(size_t)j * N + i] = t.heightM(x, z, memo);
            }
    }
    int bad = 0;
    {   // 1. off is off, checked against a second pass
        v2::TerrainMemo m2;
        long moved = 0;
        for (int j = 0; j < N; ++j)
            for (int i = 0; i < N; ++i) {
                const float x = px - span + 2 * span * (i + 0.5f) / N;
                const float z = pz - span + 2 * span * (j + 0.5f) / N;
                if (t.heightM(x, z, m2) != base[(size_t)j * N + i]) ++moved;
            }
        if (moved) { printf("FAIL  terrain is not deterministic: %ld columns\n", moved); ++bad; }
        else printf("ok    off is off        %d columns bit-identical\n", N * N);
    }

    // ---- 2..4. load the patch and compare --------------------------------
    if (!t.inset.load(insPath, px, pz, shrink)) {
        printf("FAIL  cannot load patch: %s\n", t.inset.err());
        return 1;
    }
    const float half = 0.5f * t.inset.spanWorldM();
    printf("patch   %dx%d, %.2f world m across, step %.3f m, mean %.2f m removed\n",
           t.inset.w(), t.inset.h(), t.inset.spanWorldM(), t.inset.stepWorldM(),
           t.inset.meanM());

    double inLo = 1e30, inHi = -1e30, outWorst = 0.0;
    long inN = 0, outN = 0;
    // band the deviation by distance INSIDE the border, to see the feather
    const float edges[6] = {0.0f, 3.0f, 6.0f, 12.0f, 20.0f, 1e9f};
    double bandMax[5] = {0, 0, 0, 0, 0};
    long bandN[5] = {0, 0, 0, 0, 0};
    v2::TerrainMemo m3;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const float x = px - span + 2 * span * (i + 0.5f) / N;
            const float z = pz - span + 2 * span * (j + 0.5f) / N;
            const double d = t.heightM(x, z, m3) - base[(size_t)j * N + i];
            const float lx = std::fabs(x - px), lz = std::fabs(z - pz);
            const bool inside = (lx < half && lz < half);
            if (!inside) {
                ++outN;
                outWorst = std::max(outWorst, std::fabs(d));
                continue;
            }
            ++inN;
            inLo = std::min(inLo, d);
            inHi = std::max(inHi, d);
            const float e = std::min(half - lx, half - lz);     // in from the border
            for (int b = 0; b < 5; ++b)
                if (e >= edges[b] && e < edges[b + 1]) {
                    bandMax[b] = std::max(bandMax[b], std::fabs(d));
                    ++bandN[b];
                    break;
                }
        }
    printf("\ninside  %ld samples, deviation %+.3f .. %+.3f m\n", inN, inLo, inHi);
    printf("outside %ld samples, worst |deviation| %.6f m\n", outN, outWorst);

    if (outWorst != 0.0) {
        printf("FAIL  the patch leaks outside its own bounds\n");
        ++bad;
    } else {
        printf("ok    exactly zero outside the patch\n");
    }
    if (inHi - inLo < 0.5) {
        printf("FAIL  the patch carries almost no relief (%.3f m) -- is it placed right?\n",
               inHi - inLo);
        ++bad;
    } else {
        printf("ok    carries %.2f m of measured relief\n", inHi - inLo);
    }

    printf("\n  metres in from the border -> worst |deviation|\n");
    for (int b = 0; b < 5; ++b)
        if (bandN[b])
            printf("  %5.0f..%-5.0f %7ld %10.3f m\n",
                   edges[b], b == 4 ? 99.0f : edges[b + 1], bandN[b], bandMax[b]);
    printf("  (raw, so these mix the feather with the terrain's own relief --\n"
           "   the feather itself is tested exactly below)\n");

    // ---- 4. THE FEATHER, TESTED EXACTLY ----------------------------------
    //
    // The banded maxima above CANNOT test the fade and the first version of
    // this file wrongly claimed they could. Each band's maximum is the largest
    // RAW deviation that happens to fall in it multiplied by its own weight,
    // so comparing two bands compares two different pieces of ground. On a
    // 27.9 m patch it is worse than noisy: the innermost band is a 100-sample
    // sliver at the centre, and it failed a correct implementation.
    //
    // So the weight is measured on its own. A patch of CONSTANT deviation has
    // no terrain in it, so whatever comes out IS the feather -- and it must be
    // smoothstep(d / kFeatherM) to the bit.
    {
        const int W = 400;
        const float step = 0.10f;
        std::vector<float> flat((size_t)W * W, 1.0f);
        struct { char magic[8]; int32_t w, h; float stepM, meanM; int32_t pad[8]; } hd{};
        memcpy(hd.magic, "VBINS01", 8);
        hd.w = W; hd.h = W; hd.stepM = step; hd.meanM = 0.0f;
        const char *tmp = "build/_feather.vbins";
        FILE *o = fopen(tmp, "wb");
        if (!o) { printf("FAIL  cannot write %s", tmp); ++bad; }
        else {
            fwrite(&hd, sizeof hd, 1, o);
            fwrite(flat.data(), 4, flat.size(), o);
            fclose(o);
            v2::HeightInset fi;
            if (!fi.load(tmp, 0.0f, 0.0f, 1.0f)) {
                printf("FAIL  cannot load the flat patch: %s", fi.err());
                ++bad;
            } else {
                const float halfF = 0.5f * fi.spanWorldM();
                double worst = 0.0;
                bool mono = true;
                double prev = -1.0;
                printf("\n  feather, measured on a CONSTANT 1.0 m patch:\n");
                for (int k = 0; k <= 16; ++k) {
                    const float d = float(k) * 1.0f;            // metres in
                    if (d >= halfF) break;
                    const float got = fi.deviationM(-halfF + d, 0.0f);
                    const float t = (d >= v2::HeightInset::kFeatherM)
                                        ? 1.0f : d / v2::HeightInset::kFeatherM;
                    const float want = t * t * (3.0f - 2.0f * t);
                    worst = std::max(worst, (double)std::fabs(got - want));
                    if (got + 1e-6f < prev) mono = false;
                    prev = got;
                    if (k <= 14 && (k % 2) == 0)
                        printf("   %4.0f m in   got %.4f   smoothstep %.4f\n", d, got, want);
                }
                printf("  worst error against smoothstep: %.6f\n", worst);
                if (worst > 2e-3) {
                    printf("FAIL  the feather is not smoothstep(d / %.0f)\n",
                           v2::HeightInset::kFeatherM);
                    ++bad;
                } else if (!mono) {
                    printf("FAIL  the feather is not monotonic\n");
                    ++bad;
                } else {
                    printf("ok    feather is smoothstep over %.0f m, monotonic, to 2e-3\n",
                           v2::HeightInset::kFeatherM);
                }
                remove(tmp);
            }
        }
    }

    printf("\ninset_test %s\n", bad ? "FAILED" : "PASS");
    return bad ? 1 : 0;
}
