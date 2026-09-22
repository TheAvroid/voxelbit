// lily_shore_test -- a leaf that drifts for hours and never gets into the sand.
//
// "Have the lillypads bounce off the sandy shore, instead of clipping through
// it." The old behaviour is easy to reproduce and easy to measure against: a
// pad was a POINT tested against the two-metre water field, so this drives both
// over the same lake, from the same starting places, and counts how much of a
// leaf ends up over dry land.
//
// scene/shore.h is GPU-free, so this is the REAL narrow phase -- the same
// discOnWater and the same driftFloater the engine calls -- over the real
// terrain, in about a second.
//
//     g++ -std=c++20 -O2 -I src tests/lily_shore_test.cpp -o build/lily_shore_test.exe
#include <cmath>
#include <cstdio>
#include <vector>

#include "scene/shore.h"
using namespace v2;

static int fails = 0;
static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

// The three lily models, measured from the art: 5x1x4, 7x1x6 and 9x3x9 voxels.
// The radius that has to stay over water is the longest half plus the clearance
// -- see lilyR_ in render/lake.h.
static const float kRad[3] = {0.5f * 5 * VOXEL_M + 0.06f, 0.5f * 7 * VOXEL_M + 0.06f,
                              0.5f * 9 * VOXEL_M + 0.06f};

// v1's drift, as render/lake.h runs it.
static const float kDrift = 0.11f, kTurnMin = 3.0f, kTurnMax = 7.0f, kShoreTurn = 2.6f;
static const float kPush = 0.6f, kSpinKick = 0.35f, kSpinMax = 0.33f;

// WHAT THE FIELD USED TO ANSWER. Two-metre cells, each carrying the wetness of
// its own corner column -- WaterField::rebuild, exactly, so the "before" column
// of this table is the shipped behaviour and not a caricature of it.
static bool fieldWet(const VoxelTerrain &t, TerrainMemo &memo, float x, float z) {
    const float cell = 2.0f;
    const float cx = floorf(x / cell) * cell, cz = floorf(z / cell) * cell;
    return wetColumnAt(t, memo, cx, cz);
}

struct Run {
    long ticks = 0;
    long overLand = 0;    // ticks with any part of the leaf over dry ground
    double deepest = 0;   // ...and the worst of them, in metres past the rim
    long bounces = 0;
    long aground = 0;
};

// How far into the bank the leaf is: the furthest rim point that is over land,
// measured from the centre outward. Zero when it is clear.
static double intoLand(const VoxelTerrain &t, TerrainMemo &memo, float x, float z, float r) {
    double worst = 0.0;
    for (int k = 0; k < kShoreRimSamples; ++k) {
        const float a = float(k) * (6.2831853f / float(kShoreRimSamples));
        const float dx = sinf(a), dz = cosf(a);
        if (wetColumnAt(t, memo, x + dx * r, z + dz * r)) continue;
        // Walk in until it is wet again -- that is how much of the leaf is out.
        double d = r;
        for (int s = 0; s < 40; ++s) {
            const double q = r - 0.05 * (s + 1);
            if (q <= 0.0) { d = r; break; }
            if (wetColumnAt(t, memo, x + dx * float(q), z + dz * float(q))) {
                d = r - q;
                break;
            }
        }
        worst = worst > d ? worst : d;
    }
    return worst;
}

// One pad, `ticks` ticks. `bounce` picks which rule it drifts under.
static Run drift(const VoxelTerrain &t, float x0, float z0, float th0, int model, int ticks,
                 bool bounce) {
    Run rn;
    TerrainMemo memo;
    Floater f;
    f.x = x0;
    f.z = z0;
    f.mth = th0;
    const float r = kRad[model];
    const float dt = 1.0f / 60.0f;
    float turnAt = 0.0f, clock = 0.0f;
    uint32_t rng = hashU32(uint32_t(x0 * 13.0f), uint32_t(z0 * 7.0f)) | 1u;
    auto rnd = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return float(rng >> 8) * (1.0f / 16777216.0f);
    };

    for (int i = 0; i < ticks; ++i) {
        clock += dt;
        // The wander, as stepPad runs it.
        if (clock > turnAt) {
            turnAt = clock + kTurnMin + rnd() * (kTurnMax - kTurnMin);
            f.mth += (rnd() - 0.5f) * 1.2f;
        }
        // The old three-metre lookahead, which both rules keep: it is what
        // turns a pad away from a bank BEFORE it arrives, and the bounce is
        // what happens when it arrives anyway.
        if (!fieldWet(t, memo, f.x + sinf(f.mth) * 3.0f, f.z + cosf(f.mth) * 3.0f))
            f.mth += kShoreTurn * dt;

        if (bounce) {
            const ShoreHit h =
                driftFloater(t, memo, &f, r, kDrift * dt, kPush * dt, kSpinKick, kSpinMax);
            if (h == ShoreHit::Bounced) ++rn.bounces;
            if (h == ShoreHit::Aground) ++rn.aground;
        } else {
            // AS IT SHIPPED: a point, against the field's two-metre cells.
            const float nx = f.x + sinf(f.mth) * kDrift * dt;
            const float nz = f.z + cosf(f.mth) * kDrift * dt;
            if (fieldWet(t, memo, nx, nz)) {
                f.x = nx;
                f.z = nz;
            }
        }

        ++rn.ticks;
        const double into = intoLand(t, memo, f.x, f.z, r);
        if (into > 0.0) {
            ++rn.overLand;
            rn.deepest = rn.deepest > into ? rn.deepest : into;
        }
    }
    return rn;
}

static void report(const char *what, const Run &r) {
    std::printf("  %-26s %7ld ticks  over land %6ld (%5.2f%%)  deepest %.2f m  "
                "bounces %ld  aground %ld\n",
                what, r.ticks, r.overLand, 100.0 * double(r.overLand) / double(r.ticks),
                r.deepest, r.bounces, r.aground);
}

int main() {
    VoxelTerrain t;
    t.forced = true;
    t.biome = Biome::Pine;
    TerrainMemo memo;

    // ---- FIND A SHORE TO DRIFT AT -----------------------------------------
    //
    // Not the middle of a lake: a pad in open water never meets a bank and
    // would pass this test by never being asked a question. Every start is a
    // place a leaf FITS -- which is what fill() now guarantees at the spawn --
    // with dry ground within five metres of it.
    //
    // THE FIT MATTERS AND IT IS NOT PEDANTRY. A wet COLUMN near a bank is not
    // the same thing as a spot a 0.9 m leaf fits in, and starting one in the
    // sand would measure the recovery rather than the bounce. The recovery has
    // its own section at the bottom.
    std::vector<std::pair<float, float>> starts;
    for (int j = -3000; j < 3000 && starts.size() < 24; j += 7)
        for (int i = -5200; i < -3000 && starts.size() < 24; i += 7) {
            const int vi = -4544 + i, vj = -4269 + j;
            int wl = 0;
            if (!t.lakeColumn(vi, vj, memo, &wl)) continue;
            const float x = t.wx(vi), z = t.wx(vj);
            if (!discOnWater(t, memo, x, z, kRad[2], nullptr, nullptr)) continue;
            bool bank = false;
            for (int k = 0; k < 8 && !bank; ++k) {
                const float a = float(k) * 0.785398f;
                if (!wetColumnAt(t, memo, x + sinf(a) * 5.0f, z + cosf(a) * 5.0f)) bank = true;
            }
            if (!bank) continue;
            // Not two starts in the same puddle.
            bool dup = false;   // not `near`: windows.h macro -- see lake.h::duckPads
            for (auto &s : starts)
                if (fabsf(s.first - x) < 12.0f && fabsf(s.second - z) < 12.0f) dup = true;
            if (!dup) starts.emplace_back(x, z);
        }
    std::printf("%zu starts, each a spot a leaf FITS with a bank within 5 m\n\n", starts.size());
    if (starts.size() < 8) {
        std::printf("NOT ENOUGH SHORE FOUND -- the test proves nothing\n");
        return 1;
    }

    // ---- 20,000 TICKS EACH, BOTH RULES ------------------------------------
    //
    // At 60 Hz that is five and a half minutes a pad, which at 0.11 m/s is 36 m
    // of drift -- several times the leash a real pad is kept on, so every one of
    // them meets its shore many times over.
    Run was, now;
    for (size_t s = 0; s < starts.size(); ++s) {
        const int model = int(s % 3);
        const float th = float(s) * 0.7f;
        const Run a = drift(t, starts[s].first, starts[s].second, th, model, 20000, false);
        const Run b = drift(t, starts[s].first, starts[s].second, th, model, 20000, true);
        was.ticks += a.ticks; was.overLand += a.overLand; was.bounces += a.bounces;
        was.aground += a.aground;
        was.deepest = was.deepest > a.deepest ? was.deepest : a.deepest;
        now.ticks += b.ticks; now.overLand += b.overLand; now.bounces += b.bounces;
        now.aground += b.aground;
        now.deepest = now.deepest > b.deepest ? now.deepest : b.deepest;
    }
    report("a point vs the field", was);
    report("a disc vs the terrain", now);
    std::printf("\n  %s\n", chk(now.overLand == 0,
                                "NO PART OF A LEAF IS EVER OVER SAND -- ok",
                                "A LEAF IS STILL CLIPPING THE SHORE"));
    std::printf("  %s\n", chk(now.bounces > 0, "...and it does meet the bank, so the test asked "
                                               "the question -- ok",
                              "NOTHING EVER BOUNCED -- the test proves nothing"));

    // ---- AND A PAD THAT STARTS IN THE SAND WALKS OUT ----------------------
    //
    // The recovery, which is the other half: a lake can be re-cut under a leaf,
    // and a bounce alone would leave it there for ever because every heading is
    // refused at once.
    {
        int recovered = 0, tried = 0, retired = 0, worst = 0;
        for (size_t s = 0; s < starts.size(); ++s) {
            // Walk inland from a shore until the leaf is properly aground.
            TerrainMemo m2;
            Floater f;
            f.x = starts[s].first;
            f.z = starts[s].second;
            const float r = kRad[s % 3];
            float bx = 0, bz = 0;
            bool found = false;
            for (int k = 0; k < 8 && !found; ++k) {
                const float a = float(k) * 0.785398f;
                for (float d = 1.0f; d < 12.0f; d += 0.5f) {
                    const float x = f.x + sinf(a) * d, z = f.z + cosf(a) * d;
                    if (!wetColumnAt(t, m2, x, z) && !discOnWater(t, m2, x, z, r, nullptr,
                                                                  nullptr)) {
                        bx = x;
                        bz = z;
                        found = true;
                        break;
                    }
                }
            }
            if (!found) continue;
            ++tried;
            f.x = bx;
            f.z = bz;
            f.mth = 0.0f;
            bool out = false;
            float stuck = 0.0f;
            int sec = 0;
            for (int i = 0; i < 3600 && !out; ++i) {
                const ShoreHit h =
                    driftFloater(t, m2, &f, r, kDrift / 60.0f, kPush / 60.0f, kSpinKick, kSpinMax);
                stuck = (h == ShoreHit::Aground) ? stuck + 1.0f / 60.0f : 0.0f;
                out = discOnWater(t, m2, f.x, f.z, r, nullptr, nullptr);
                if (out) sec = i;
            }
            if (out) {
                ++recovered;
                worst = worst > sec ? worst : sec;
            } else {
                // Not a failure: this is water narrower than the leaf, and the
                // answer for that is to give the slot back rather than to keep
                // pushing. kLilyStuckSec is six seconds; this one is well past
                // it, so the engine would have retired it long before here.
                ++retired;
                if (stuck < 6.0f) ++fails;
            }
        }
        std::printf("  %d of %d pads dropped in the sand got back on the water (worst %.1f s); "
                    "%d were in water narrower than themselves and retire instead  %s\n",
                    recovered, tried, double(worst) / 60.0, retired,
                    chk(tried > 0 && recovered + retired == tried, "-- ok",
                        "-- ONE IS NEITHER OUT NOR RETIRED"));
    }

    std::printf("\n%s\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
