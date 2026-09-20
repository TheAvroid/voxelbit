// ---------------------------------------------------------------------------
// edt_test.cpp -- is CoverField's distance transform actually exact?
//
//   g++ -std=c++20 -O2 -I src tests/edt_test.cpp -o build/edt_test.exe
//   ./build/edt_test.exe
//
// (user 2026-09-19: "the terrain inside of water beds are very
// square/rectangle".)
//
// WHY THIS NEEDS A TEST. The lake bed is `depth = 0.45 * toShoreW`, and
// toShoreW comes straight out of this transform -- so the transform's
// iso-distance contours ARE the bed's depth contours. A distance transform
// that is 5% long in some directions does not read as "5% wrong", it reads as
// a bed with flat facets and hard corners, and then the 0.1 m quantiser lays a
// terrace along each facet and any facet clearing the waterline becomes a
// straight island.
//
// The old 3-4 chamfer (D1 = 10, D2 = 14) is exact along the axes and the
// diagonals and wrong in between, which is precisely the shape that was
// reported. So the thing to test is not "is it close" but "is it EXACT, in
// every direction" -- and a distance transform is one of the rare things with
// perfect ground truth available: brute force over every seed.
//
// THE DIRECTIONAL TEST IS THE ONE THAT MATTERS. A single-seed grid compared
// against sqrt(dx^2+dy^2) catches an approximate transform immediately, and
// reporting the error BY ANGLE shows the octagon: a chamfer's error peaks at
// 22.5 degrees and vanishes at 0 and 45.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <vector>
#include <algorithm>

#include "world/cover.h"

namespace {

const int INF = 1 << 28;

// What the transform should say, computed the slow honest way.
std::vector<double> brute(const std::vector<int> &seed, int w, int h) {
    std::vector<double> out((size_t)w * h, 1e30);
    std::vector<std::pair<int, int>> pts;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            if (!seed[(size_t)j * w + i]) pts.push_back({i, j});
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            double best = 1e30;
            for (auto &p : pts) {
                const double dx = i - p.first, dy = j - p.second;
                best = std::min(best, dx * dx + dy * dy);
            }
            out[(size_t)j * w + i] = std::sqrt(best);
        }
    return out;
}

// The transform that USED to be here, so the improvement is shown and not
// merely asserted.
void chamfer34(std::vector<int> &d, int w, int h) {
    const int D1 = 10, D2 = 14;
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i) {
            const size_t k = (size_t)j * w + i;
            if (!d[k]) continue;
            int best = d[k];
            if (i) best = std::min(best, d[k - 1] + D1);
            if (j) {
                best = std::min(best, d[k - w] + D1);
                if (i) best = std::min(best, d[k - w - 1] + D2);
                if (i < w - 1) best = std::min(best, d[k - w + 1] + D2);
            }
            d[k] = best;
        }
    for (int j = h - 1; j >= 0; --j)
        for (int i = w - 1; i >= 0; --i) {
            const size_t k = (size_t)j * w + i;
            if (!d[k]) continue;
            int best = d[k];
            if (i < w - 1) best = std::min(best, d[k + 1] + D1);
            if (j < h - 1) {
                best = std::min(best, d[k + w] + D1);
                if (i) best = std::min(best, d[k + w - 1] + D2);
                if (i < w - 1) best = std::min(best, d[k + w + 1] + D2);
            }
            d[k] = best;
        }
}

struct Err { double worst = 0, mean = 0; long n = 0; };

Err compare(const std::vector<int> &got, const std::vector<double> &want, int w, int h) {
    Err e;
    for (size_t k = 0; k < want.size(); ++k) {
        if (want[k] > 1e29) continue;
        const double g = double(got[k]) / 10.0;     // tenths of a cell
        const double d = std::fabs(g - want[k]);
        e.worst = std::max(e.worst, d);
        e.mean += d;
        ++e.n;
    }
    if (e.n) e.mean /= e.n;
    return e;
}

}  // namespace

int main() {
    int bad = 0;

    // ---- 1. ONE SEED, EVERY DIRECTION ---------------------------------
    {
        const int w = 129, h = 129;
        std::vector<int> seed((size_t)w * h, INF);
        seed[(size_t)(h / 2) * w + w / 2] = 0;

        std::vector<int> e = seed, c = seed;
        CoverField::chamfer(e, w, h);
        chamfer34(c, w, h);
        const std::vector<double> truth = brute(seed, w, h);

        const Err ee = compare(e, truth, w, h);
        const Err ce = compare(c, truth, w, h);
        printf("single seed, %dx%d, against brute-force Euclidean:\n", w, h);
        printf("  exact EDT    worst %.4f cells   mean %.5f\n", ee.worst, ee.mean);
        printf("  3-4 chamfer  worst %.4f cells   mean %.5f   <- what was here\n",
               ce.worst, ce.mean);
        // A tenth of a cell is the quantiser's own step, so that is the floor.
        if (ee.worst > 0.051) {
            printf("FAIL  the EDT is not exact to the stored precision\n");
            ++bad;
        } else {
            printf("ok    exact to the stored tenth-of-a-cell precision\n");
        }
        if (!(ee.worst < ce.worst)) {
            printf("FAIL  the EDT is no better than the chamfer it replaced\n");
            ++bad;
        }

        // ---- 2. THE OCTAGON, BY ANGLE ---------------------------------
        // The whole complaint in one table: error as a function of direction.
        printf("\n  error by angle at r = 48 cells (0 and 45 deg are where a\n"
               "  chamfer is exact; 22.5 is where it is worst):\n");
        printf("  %8s %12s %12s\n", "angle", "exact EDT", "3-4 chamfer");
        double chamferSpread = 0.0, edtSpread = 0.0;
        for (int a = 0; a <= 45; a += 5) {
            const double rad = a * 3.14159265358979 / 180.0;
            const int i = w / 2 + int(std::lround(48.0 * std::cos(rad)));
            const int j = h / 2 + int(std::lround(48.0 * std::sin(rad)));
            const size_t k = (size_t)j * w + i;
            const double t = truth[k];
            const double de = double(e[k]) / 10.0 - t;
            const double dc = double(c[k]) / 10.0 - t;
            printf("  %6d d %11.3f %12.3f\n", a, de, dc);
            chamferSpread = std::max(chamferSpread, std::fabs(dc));
            edtSpread = std::max(edtSpread, std::fabs(de));
        }
        printf("  worst over those angles:  EDT %.3f   chamfer %.3f cells\n",
               edtSpread, chamferSpread);
        if (chamferSpread < 1.0) {
            printf("FAIL  the chamfer should be over a cell out at r = 48\n");
            ++bad;
        }
    }

    // ---- 3. MANY SEEDS, WHICH IS THE REAL SHAPE OF A COASTLINE --------
    {
        const int w = 96, h = 96;
        std::vector<int> seed((size_t)w * h, INF);
        unsigned s = 12345u;
        int placed = 0;
        for (int k = 0; k < 40; ++k) {
            s = s * 1664525u + 1013904223u;
            const int i = int((s >> 16) % w);
            s = s * 1664525u + 1013904223u;
            const int j = int((s >> 16) % h);
            if (seed[(size_t)j * w + i]) { seed[(size_t)j * w + i] = 0; ++placed; }
        }
        std::vector<int> e = seed;
        CoverField::chamfer(e, w, h);
        const Err ee = compare(e, brute(seed, w, h), w, h);
        printf("\n%d scattered seeds, %dx%d:  worst %.4f   mean %.5f cells\n",
               placed, w, h, ee.worst, ee.mean);
        if (ee.worst > 0.051) {
            printf("FAIL  multi-seed transform is not exact\n");
            ++bad;
        } else {
            printf("ok    exact with many seeds too\n");
        }
    }

    // ---- 4. DEGENERATE INPUTS -----------------------------------------
    {
        std::vector<int> none((size_t)8 * 8, INF);
        CoverField::chamfer(none, 8, 8);     // no seeds at all
        bool allBig = true;
        for (int v : none) if (v < (1 << 27)) allBig = false;
        std::vector<int> all((size_t)8 * 8, 0);
        CoverField::chamfer(all, 8, 8);      // every cell a seed
        bool allZero = true;
        for (int v : all) if (v != 0) allZero = false;
        std::vector<int> empty;
        CoverField::chamfer(empty, 0, 0);    // must not crash
        if (!allBig || !allZero) {
            printf("\nFAIL  degenerate inputs: no-seed %s, all-seed %s\n",
                   allBig ? "ok" : "BROKEN", allZero ? "ok" : "BROKEN");
            ++bad;
        } else {
            printf("\nok    no seeds / all seeds / empty grid all behave\n");
        }
    }

    printf("\nedt_test %s\n", bad ? "FAILED" : "PASS");
    return bad ? 1 : 0;
}
