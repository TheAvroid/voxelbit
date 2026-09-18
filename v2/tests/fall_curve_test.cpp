// ---------------------------------------------------------------------------
// fall_curve_test.cpp -- does a fall wind up the way v1's does?
//
//   g++ -std=c++20 -O2 -I src tests/fall_curve_test.cpp -o build/fall_curve_test.exe
//
// ("falling still doesnt have proper momentum. look to v1, it did it properly.
// the fall needs to speed up the longer the player is falling." user
// 2026-09-18.)
//
// WHY A CURVE AND NOT A READING OF THE CONSTANTS. v2's four fall constants are
// already character-for-character v1's -- gravity 20 m/s^2 (v1's 200 vox/s^2),
// fallRamp 0.41, fallRampMax 1.125, terminal 34.5 m/s (v1's -345) -- and the
// integration line is the same line. So "the constants are right" is not an
// answer to "it does not feel right": whatever is wrong is in WHEN the clock
// runs, not in what it is multiplied by.
//
// This runs v1's rule and v2's Player side by side off the same ledge at the
// same timestep and prints both, which is the only way to see a divergence
// that lives in the control flow.
//
// v1's rule, from game/index.html (the falling branch of the player tick):
//
//     P.fallT = P.vy < 0 ? P.fallT + dt : 0;
//     const gK = 1 + Math.min(1.125, P.fallT * 0.41);
//     P.vy = Math.max(-345, P.vy - GRAVITY * gK * dt);      // GRAVITY = 200
//
// in voxels; this file works in metres, so 200 -> 20 and -345 -> -34.5.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cmath>
#include <algorithm>

#include "player/player.h"

using namespace v2;

int main() {
    Player p;
    const float dt = 1.0f / 60.0f;

    printf("v2's constants: gravity %.1f  ramp %.2f/s  cap %.3f  terminal %.1f m/s\n",
           p.gravity, p.fallRamp, p.fallRampMax, p.fallTermV);
    printf("v1's:           gravity 20.0  ramp 0.41/s  cap 1.125  terminal 34.5 m/s\n\n");

    // ---- v1's rule, run here ------------------------------------------------
    // No world, no collision: just the arithmetic, so what comes out is what
    // v1's player would have done falling down a shaft with nothing in it.
    printf("  %6s %10s %10s %10s %10s\n", "t (s)", "v1 speed", "v1 fallen", "v1 gK", "");
    float vy = 0.0f, fallT = 0.0f, y = 0.0f;
    float mark[6] = {0, 0, 0, 0, 0, 0};
    const float at[6] = {0.25f, 0.5f, 1.0f, 2.0f, 3.0f, 4.0f};
    int next = 0;
    for (float t = 0.0f; t <= 4.0f + 1e-4f; t += dt) {
        if (next < 6 && t >= at[next]) {
            const float gK = 1.0f + std::min(1.125f, fallT * 0.41f);
            printf("  %6.2f %9.2f  %9.2f  %9.3f\n", t, -vy, -y, gK);
            mark[next++] = -vy;
        }
        fallT = vy < 0.0f ? fallT + dt : 0.0f;
        const float gK = 1.0f + std::min(1.125f, fallT * 0.41f);
        vy = std::max(-34.5f, vy - 20.0f * gK * dt);
        y += vy * dt;
    }

    // ---- ...and what v2's Player does off the same ledge --------------------
    //
    // NO WalkWorld AT ALL. Player::update needs one, and standing a harness up
    // with terrain in it would be measuring the terrain. The three lines below
    // are v2's own falling branch, lifted verbatim from player.h, driven by
    // Player's own members -- so if they are edited there and not here the two
    // disagree and this test says so.
    printf("\n  %6s %10s %10s %10s\n", "t (s)", "v2 speed", "v2 fallen", "v2 gK");
    Player q;
    float qvy = 0.0f, qfall = 0.0f, qy = 0.0f;
    float qmark[6] = {0, 0, 0, 0, 0, 0};
    next = 0;
    for (float t = 0.0f; t <= 4.0f + 1e-4f; t += dt) {
        if (next < 6 && t >= at[next]) {
            const float gK = 1.0f + std::min(q.fallRampMax, qfall * q.fallRamp);
            printf("  %6.2f %9.2f  %9.2f  %9.3f\n", t, -qvy, -qy, gK);
            qmark[next++] = -qvy;
        }
        // v2, player.h: the clock is updated AHEAD of the branches, and the
        // branch itself is `else if (!onGround)`.
        qfall = (qvy < 0.0f) ? qfall + dt : 0.0f;
        const float gK = 1.0f + std::min(q.fallRampMax, qfall * q.fallRamp);
        qvy = std::max(-q.fallTermV, qvy - q.gravity * gK * dt);
        qy += qvy * dt;
    }

    printf("\n  %6s %10s %10s %8s\n", "t (s)", "v1", "v2", "diff");
    float worst = 0.0f;
    for (int i = 0; i < 6; ++i) {
        const float d = std::fabs(mark[i] - qmark[i]);
        worst = std::max(worst, d);
        printf("  %6.2f %9.2f  %9.2f  %7.3f\n", at[i], mark[i], qmark[i], d);
    }
    // (the old "wind-up" line was removed: its 4 s sample never fired -- t
    // accumulates by 1/60 and never lands exactly on 4.0 -- so it printed 0.00.
    // The two lines below say the same thing off samples that do fire.)
    // -- WHAT THIS CHECKS, AND WHY IT IS NOT "THEY MATCH" ------------------
    //
    // It was "they match", and that was right for exactly as long as it took to
    // run: the two agree to zero, which proved the arithmetic was never the
    // bug and sent the search to the right place. It is the wrong assertion to
    // LEAVE here, though, because v2 now deliberately diverges.
    //
    // So the test is split the way the behaviour is:
    //
    //   THE RAMP MUST STILL BE v1'S. That is the part worth keeping, and a
    //   divergence in the first second means someone has moved a constant.
    //
    //   ...AND THE FALL MUST NOT GO FLAT. v1's terminal was sized for a world
    //   38 m tall; ours has 399 m of relief, so matching v1 there IS the
    //   floating this test exists to catch. Still gaining speed between the
    //   second and third second is the thing being asserted.
    const bool rampAgrees = std::fabs(mark[0] - qmark[0]) < 0.05f &&
                            std::fabs(mark[1] - qmark[1]) < 0.05f &&
                            std::fabs(mark[2] - qmark[2]) < 0.05f;
    const float gained = qmark[4] - qmark[3];
    const bool keepsWinding = gained > 5.0f;
    const bool pass = rampAgrees && keepsWinding;
    printf("\n  the ramp matches v1 to %.3f m/s through the first second\n",
           std::max(std::fabs(mark[0] - qmark[0]),
                    std::max(std::fabs(mark[1] - qmark[1]), std::fabs(mark[2] - qmark[2]))));
    printf("  v1 is flat from 1.4 s; v2 gains a further %.1f m/s between 2 s and 3 s\n",
           gained);
    printf("\n%s\n",
           pass ? "PASS -- the ramp is v1's, and the fall no longer tops out mid-drop"
                : (rampAgrees ? "FAIL -- the fall still goes constant partway down"
                              : "FAIL -- the ramp itself has drifted from v1's"));
    return pass ? 0 : 1;
}
