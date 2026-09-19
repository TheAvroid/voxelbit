// rock_overhang_test.cpp -- a rock is a shape, not a column.
//
// (user 2026-09-18: "can you fix the hitboxes on the large rocks. you seem to
// creating a boxed hitbox instead of a hitbox that hugs the voxels of the shape
// itself. investigate and fix this. I cant move the player underneath a big
// rock for example.")
//
// WHAT WAS WRONG. Player::groundInfo asked every standable solid for
// solidColumnTop -- the height of its highest voxel in that column -- and took
// that as the floor. A column heightfield cannot express an overhang, so under
// the flared cap of a big boulder the "floor" came back as the top of the rock
// and the body was lifted onto it. The hitbox was not a box round the rock; it
// was worse, a full-height column under every voxel of it, reaching the sky.
//
// And blocked() skipped standable solids outright, on the argument that walking
// into a rock is meant to put you on top of it. That argument only holds while
// the floor IS the top of the column.
//
// So both moved together, and this pins both:
//
//   * the floor is the highest voxel AT OR BELOW the feet plus one step
//     (solidColumnTopBelow), so an overhang is a ceiling and not a floor;
//   * blocked() tests the body's box against the model's VOXELS from the
//     step-up to the top of the head, so the stone stops you where the stone
//     actually is.
//
// THE SMALL END HAS TO KEEP WORKING and that is half of what is checked here: a
// stone shorter than the step-up must still be something you walk up onto, not
// something you walk into.
//
// GPU-FREE AND BUILDS ITS OWN MODEL, on level_walk_test.cpp's rule: a test that
// needs a gitignored asset is a test that stops running.
//
//   g++ -std=c++17 -O1 -I src tests/rock_overhang_test.cpp -o build/rock.exe
#include "player/player.h"

#include <cstdio>
#include <vector>

using namespace v2;

static int failures = 0;

static void check(bool ok, const char *what, double got, double want) {
    std::printf("  %-58s %s", what, ok ? "ok" : "FAIL");
    if (!ok) {
        std::printf("   (got %.2f, wanted %.2f)", got, want);
        ++failures;
    }
    std::printf("\n");
}

// A mushroom-shaped boulder: a narrow stalk with a wide cap on top, which is
// the shape the report is about. 4 x 4 m footprint, cap from 3.0 m to 3.4 m,
// stalk 0.8 m across in the middle.
struct Rock {
    static constexpr int SX = 40, SZ = 40, SY = 40;
    static constexpr int kCapLo = 30, kCapHi = 34;   // voxels
    std::vector<uint8_t> vol;
    std::vector<int16_t> col;
    Solid s;

    Rock(float baseY, bool capped) {
        vol.assign(size_t(SX) * SZ * SY, 0);
        col.assign(size_t(SX) * SZ, 0);
        auto at = [&](int x, int y, int z) -> uint8_t & {
            return vol[size_t(x) + size_t(z) * SX + size_t(y) * SX * SZ];
        };
        for (int z = 0; z < SZ; ++z)
            for (int x = 0; x < SX; ++x) {
                const bool stalk = x >= 16 && x < 24 && z >= 16 && z < 24;
                int top = 0;
                for (int y = 0; y < SY; ++y) {
                    bool solid = false;
                    if (stalk && y < kCapHi) solid = true;
                    if (capped && y >= kCapLo && y < kCapHi) solid = true;
                    if (solid) {
                        at(x, y, z) = 1;
                        top = y + 1;
                    }
                }
                col[size_t(x) + size_t(z) * SX] = int16_t(top);
            }
        s = Solid{};
        s.standable = true;
        s.cx = 2.0f;
        s.cz = 2.0f;
        s.hx = 2.0f;
        s.hz = 2.0f;
        s.baseY = baseY;
        s.top = baseY + float(kCapHi) * VOXEL_M;
        s.tx = 0.0f;
        s.tz = 0.0f;
        s.yaw = 0;
        s.msx = SX;
        s.msz = SZ;
        s.vsy = SY;
        s.col = col.data();
        s.vol = vol.data();
    }
};

int main() {
    std::printf("rock overhang test -- a rock is a shape, not a column\n");

    // No terrain: walkGroundM answers a flat floor one voxel up. That is the
    // ground the body stands on, and the rock sits on it.
    const float ground = VOXEL_M;

    // ---- 1. THE BIG ROCK WITH AN OVERHANG ---------------------------------
    {
        Rock r(ground, /*capped=*/true);
        WalkWorld w;
        w.solids = &r.s;
        w.solidCount = 1;

        Player p;
        p.pos = Vec3(0.4f, ground, 0.4f);     // under the cap, well off the stalk
        p.onGround = true;

        const float floorUnder = p.groundHeight(w, p.pos.x, p.pos.z, p.pos.y + p.upMax());
        check(floorUnder < ground + 0.2f, "under the cap, the floor is the ground", floorUnder,
              ground);

        const bool stuckUnder = p.blocked(w, p.pos.x, p.pos.z);
        check(!stuckUnder, "under the cap, the body is not blocked", stuckUnder ? 1 : 0, 0);

        // ...and the stalk in the middle still stops you.
        const bool stuckStalk = p.blocked(w, 2.0f, 2.0f);
        check(stuckStalk, "the stalk in the middle blocks", stuckStalk ? 1 : 0, 1);

        // Standing ON the cap: the floor is the cap, and you are not "blocked"
        // by the rock you are standing on.
        Player q;
        q.pos = Vec3(0.4f, ground + float(Rock::kCapHi) * VOXEL_M, 0.4f);
        q.onGround = true;
        const float onTop = q.groundHeight(w, q.pos.x, q.pos.z, q.pos.y + q.upMax());
        check(onTop > ground + 3.0f, "standing on the cap, the floor is the cap", onTop,
              ground + 3.4f);
        const bool stuckOnTop = q.blocked(w, q.pos.x, q.pos.z);
        check(!stuckOnTop, "standing on the cap, the body is not blocked", stuckOnTop ? 1 : 0, 0);

        // A fall from above still lands on the cap: with no ceiling given, the
        // answer is the top of the column, which is what a falling body wants.
        const float landing = p.surfaceAt(w, 0.4f, 0.4f);
        check(landing > ground + 3.0f, "a fall from above still lands on the cap", landing,
              ground + 3.4f);
    }

    // ---- 2. A PEBBLE YOU STEP ONTO ----------------------------------------
    //
    // The whole rock is below the step-up, so it must not block, and its top
    // must still be offered as a floor. This is the half of the behaviour that
    // was already right and had to stay that way.
    {
        Rock r(ground, /*capped=*/false);
        // Cut it down to 4 voxels -- 0.4 m, under the 0.62 m step-up.
        for (int z = 0; z < Rock::SZ; ++z)
            for (int x = 0; x < Rock::SX; ++x) {
                for (int y = 4; y < Rock::SY; ++y)
                    r.vol[size_t(x) + size_t(z) * Rock::SX + size_t(y) * Rock::SX * Rock::SZ] = 0;
                int16_t &h = r.col[size_t(x) + size_t(z) * Rock::SX];
                if (h > 4) h = 4;
            }
        r.s.top = ground + 0.4f;
        WalkWorld w;
        w.solids = &r.s;
        w.solidCount = 1;

        Player p;
        p.pos = Vec3(2.0f, ground, 2.0f);
        p.onGround = true;
        const bool stuck = p.blocked(w, 2.0f, 2.0f);
        check(!stuck, "a 0.4 m stone does not block", stuck ? 1 : 0, 0);
        const float up = p.groundHeight(w, 2.0f, 2.0f, p.pos.y + p.upMax());
        check(up > ground + 0.3f, "...and its top is still a floor to step onto", up,
              ground + 0.4f);
    }

    // ---- 3. OFF THE MODEL ENTIRELY ----------------------------------------
    {
        Rock r(ground, /*capped=*/true);
        WalkWorld w;
        w.solids = &r.s;
        w.solidCount = 1;
        Player p;
        p.pos = Vec3(20.0f, ground, 20.0f);
        p.onGround = true;
        const bool stuck = p.blocked(w, 20.0f, 20.0f);
        check(!stuck, "ten metres away, nothing blocks", stuck ? 1 : 0, 0);
        const float f = p.groundHeight(w, 20.0f, 20.0f, p.pos.y + p.upMax());
        check(f < ground + 0.2f, "...and the floor is the ground", f, ground);
    }

    std::printf("\n%s\n", failures ? "FAIL" : "PASS -- the rock is its own shape.");
    return failures ? 1 : 0;
}
