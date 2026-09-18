// wheat_grass_test -- tall grass wears straw, and the ramp runs the right way.
//
// "can you make the tall grass have a more wheat color to them. maybe at the
// tips at the top its a brown, then a lighter brown to yellow/tan on the way
// back down." (user 2026-09-14), then: "actually make the base of the tall
// wheat grass, the same green as the grass green. then from there, go to
// wheat. a lighter color overal."
//
// WHY THIS IS NOT A SCREENSHOT. Two reasons, and the first is the one
// deriveGroundFromTrees already wrote down: sampled pixels are dominated by
// LIGHTING, not albedo, so a render cannot tell you what colour a material is.
// The second is particular to this change -- a 2 m sward self-shadows, so in
// frame the whole field is nearly black except where the sun happens to land on
// a tip. The picture confirms the tips read gold; it cannot confirm the ramp.
//
// So this walks the REAL terrain through the REAL strandRows and bladeMaterial
// and checks three things nothing else can see:
//
//   1. every tall blade wears its OWN WOOD'S straw and every short one that
//      wood's green -- the two ramps must not be crossed;
//   2. each ramp STARTS at that wood's GRASS_0 exactly, which is the whole of
//      "the same green as the grass green" and the one thing a shared ramp
//      could not do;
//   3. it gets LIGHTER all the way up, and ends warm rather than dark;
//   4. the ramp REACHES a tuft's tip -- ten shades at STRAND_ROW_STEP must
//      cover tallGrassMaxRows, or the top of every blade is one flat colour.
//
//     g++ -std=c++20 -O2 -I src tests/wheat_grass_test.cpp -o build/wheat_grass_test.exe
#include <cmath>
#include <cstdio>

#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static void ck(bool ok, const char *what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++fails;
}

// Rec. 709 luminance of a LINEAR rgb -- the table is linear, so no decode here.
static float lum(const Vec3 &c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

int main() {
    std::printf("\n=== WHEAT GRASS TEST ===\n");

    // ---- 1. what a blade wears, over a real sweep of both woods -------------
    //
    // The two woods are swept separately because the GREEN answer differs
    // between them and the STRAW answer must not.
    struct Wood { const char *name; Biome b; long tall = 0, shortG = 0, wrong = 0; };
    Wood woods[2] = {{"pine", Biome::Pine}, {"birch", Biome::Birch}};

    for (Wood &w : woods) {
        VoxelTerrain t;
        t.forced = true;
        t.biome = w.b;
        TerrainMemo memo;
        // 600 x 600 columns is 60 m square at VOXEL_M, which holds a few dozen
        // tufts -- enough that a broken gate could not pass by luck.
        for (int j = 0; j < 600; ++j)
            for (int i = 0; i < 600; ++i) {
                const int h = t.heightVox(i, j, memo);
                const uint8_t top = t.topMaterial(i, j, h, memo);
                const int rows = t.strandRows(i, j, top, memo);
                if (rows <= 0) continue;
                const uint8_t m = t.bladeMaterial(i, j, rows);
                // WHICH WOOD THIS COLUMN BELONGS TO, asked of the two-argument
                // form -- the green and the straw must agree about it, or a
                // tuft's foot is a different colour from the sward it is in.
                const bool birch = t.bladeMaterial(i, j) == mat::BGRASS_0;
                if (t.tallStrand(rows)) {
                    ++w.tall;
                    if (!isWheat(m)) ++w.wrong;
                    // ...AND ITS OWN WOOD'S STRAW. Crossing the two ramps is
                    // the one failure the split exists to prevent, and it is
                    // invisible in a render: both are straw-coloured.
                    if (m != (birch ? mat::BWHEAT_0 : mat::WHEAT_0)) ++w.wrong;
                } else {
                    ++w.shortG;
                    if (isWheat(m)) ++w.wrong;
                    if (m != (birch ? mat::BGRASS_0 : mat::GRASS_0)) ++w.wrong;
                }
            }
        std::printf("  %-6s wood   %6ld tall blades, %6ld short, %ld wrong\n", w.name, w.tall,
                    w.shortG, w.wrong);
    }
    ck(woods[0].tall > 0 && woods[1].tall > 0, "both woods actually grew some tall grass");
    ck(woods[0].shortG > 0 && woods[1].shortG > 0, "...and some short grass to tell it from");
    ck(woods[0].wrong == 0 && woods[1].wrong == 0,
       "every blade wears its own wood's ramp, green or straw");

    // The two-argument form still answers WHICH WOOD, which is what the flower
    // scatter asks it. A tall column must not change that answer.
    {
        VoxelTerrain t;
        TerrainMemo memo;
        long checked = 0, drift = 0;
        for (int j = 0; j < 300; ++j)
            for (int i = 0; i < 300; ++i) {
                const uint8_t wood = t.bladeMaterial(i, j);
                if (wood != mat::GRASS_0 && wood != mat::BGRASS_0) ++drift;
                ++checked;
            }
        ck(checked > 0 && drift == 0,
           "bladeMaterial(i,j) still answers a WOOD, never straw");
    }

    // ---- 2. each ramp roots in its own wood's green and dries upward -------
    //
    // THE GREENS COME FROM THE TREES, so with no models loaded fillGrassRamp
    // takes its documented fallback green -- which is the right thing to test
    // against: the ramp's SHAPE is what can break, and it must hold for
    // whatever green the pines and birches turn out to be. The absolute colours
    // are a matter for a render.
    Palette pal;
    pal.deriveGroundFromTrees();

    struct Ramp { const char *name; uint8_t base, count, grass; };
    const Ramp ramps[2] = {{"pine", mat::WHEAT_0, mat::WHEAT_COUNT, mat::GRASS_0},
                           {"birch", mat::BWHEAT_0, mat::BWHEAT_COUNT, mat::BGRASS_0}};

    for (const Ramp &r : ramps) {
        std::printf("\n  %s straw, soil to tip\n", r.name);
        const Vec3 g = pal[r.grass].albedo;
        // THE WHOLE OF "the same green as the grass green" -- byte for byte,
        // not near enough. A tuft's foot and the short blades round it are one
        // colour or the tuft is planted in the sward rather than grown out of
        // it.
        const Vec3 a0 = pal[r.base].albedo;
        ck(a0.x == g.x && a0.y == g.y && a0.z == g.z,
           "shade 0 IS that wood's GRASS_0, exactly");

        float prev = -1.0f, peak = -1.0f;
        int peakAt = 0;
        bool rising = true;
        for (int k = 0; k < int(r.count); ++k) {
            const Vec3 a = pal[uint8_t(r.base + k)].albedo;
            const float L = lum(a);
            std::printf("    shade %d  linear %.3f %.3f %.3f   luminance %.3f%s\n", k, a.x, a.y,
                        a.z, L,
                        k == 0 ? "   <- the grass green" : (k >= 8 ? "   <- the head" : ""));
            if (L > peak) { peak = L; peakAt = k; }
            prev = L;
        }
        // RISING UP TO THE PEAK, which is where "a lighter color overal" lives:
        // the first cut ran tan DOWN to a dark brown from its very first shade.
        // Checked only up to the peak because the head deliberately turns after
        // it -- see below.
        for (int k = 1; k <= peakAt; ++k)
            if (lum(pal[uint8_t(r.base + k)].albedo) <=
                lum(pal[uint8_t(r.base + k - 1)].albedo))
                rising = false;
        ck(rising, "it gets lighter every shade up to the peak");
        // ...AND THE PEAK IS NEAR THE TOP, so the rise is the blade rather than
        // a bright band low down with a long dull run above it.
        ck(peakAt >= int(r.count) - 4, "the peak is in the top third of the blade");

        // THE HEAD IS A HUE TURN, NOT A FALL IN VALUE. The request wants a
        // brown tip AND a lighter ramp, and those only sit together if the tip
        // gets WARMER while giving up very little brightness.
        const Vec3 tip = pal[uint8_t(r.base + r.count - 1)].albedo;
        const Vec3 pk = pal[uint8_t(r.base + peakAt)].albedo;
        ck(tip.x > tip.y && tip.y > tip.z, "the tip is r > g > b -- straw, not grey");
        ck(lum(g) < 0.5f * peak, "the foot is far darker than the head");
        ck(lum(tip) >= 0.85f * peak, "...and the head never falls back to dark");
        ck(tip.x / maxf(1e-6f, tip.y) > pk.x / maxf(1e-6f, pk.y),
           "the tip is WARMER than the peak -- a ripe head, not a shadow");

        // The turn happens ON the blade, not at its very base or only at the
        // very top: by the middle shade it must be mostly straw already.
        const Vec3 mid = pal[uint8_t(r.base + r.count / 2)].albedo;
        ck(mid.x > mid.y, "by halfway up it has crossed from green to straw");

        // A BLADE IS STILL A THIN BLADE. Straw roughens and loses some of the
        // back-lit glow, but not all of it -- without translucency a strand is
        // a black stick at dawn, which is the note the green ramp carries.
        ck(pal[uint8_t(r.base + r.count - 1)].translucency > 0.10f,
           "the straw end is still translucent enough to catch a low sun");
    }

    // THE TWO RAMPS ARE NOT THE SAME RAMP at the wet end -- that is the whole
    // reason there are two. (With no models loaded both greens take the same
    // fallback, so this can only be asserted where they differ; the check that
    // bites is shade 0 == GRASS_0 above, run once per wood.)
    std::printf("\n  pine foot %.3f, birch foot %.3f -- each its own wood's green\n",
                lum(pal[mat::WHEAT_0].albedo), lum(pal[mat::BWHEAT_0].albedo));

    // ---- 3. the ramp reaches the tip ----------------------------------------
    //
    // The device reads one shade per STRAND_ROW_STEP voxels from the ground, so
    // the ten shades span this many voxels. Short of tallGrassMaxRows and the
    // top of every tuft clamps to one flat brown -- which is the exact failure
    // six shades would have had.
    {
        VoxelTerrain t;
        const int reach = int(mat::WHEAT_COUNT) * STRAND_ROW_STEP;
        std::printf("\n  ramp reaches %d voxels; a tuft is %d..%d\n", reach, t.tallGrassMinRows,
                    t.tallGrassMaxRows);
        ck(reach >= t.tallGrassMaxRows, "ten shades cover the tallest blade");
        // ...and the shortest tall blade still gets INTO the browns, or half the
        // tufts in the world are gold with no head on them.
        ck((t.tallGrassMinRows - 1) / STRAND_ROW_STEP >= 7,
           "even the shortest tuft blade reaches the head shades");
    }

    // Neither ramp may run into the other or into the model palette -- an
    // overlap paints tall grass with whatever colour a tree happened to
    // register.
    //
    // THE STRAW NO LONGER BUTTS ONTO mat::TREE_BASE, and this asked that it
    // did. The hoe's TILLED and the three SEED shades were inserted between
    // them (2026-09-17), so this failed the moment the earth could be tilled --
    // it was reading "nothing lies between the straw and the models" as "the
    // straw ends where the models begin", and only the first is the rule.
    // Walked to TREE_BASE through the reserved ids instead, which still catches
    // an overlap and does not need editing the next time something is reserved.
    ck(mat::WHEAT_0 + mat::WHEAT_COUNT == mat::BWHEAT_0, "the two straw ramps do not overlap");
    ck(mat::BWHEAT_0 + mat::BWHEAT_COUNT == mat::TILLED,
       "the tilled earth starts just above the straw");
    ck(mat::SEED_0 == mat::TILLED + 1 && mat::SEED_0 + mat::SEED_COUNT == mat::TREE_BASE,
       "the seed shades close the gap to the model palette");

    std::printf("\n%s\n\n", fails ? "FAILURES" : "all ok");
    return fails ? 1 : 0;
}
