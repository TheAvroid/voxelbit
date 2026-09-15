// prone_test -- the third stance, driven through the real Player::update.
//
// Four things the stance has to do, and one it has to NOT do:
//   1. A held crouch settles at the CROUCH first, and stays there for the hold
//      window rather than sliding straight past it to the floor.
//   2. Keep holding and the eye reaches the PRONE height.
//   3. Release and the eye comes back up THROUGH the crouch height without
//      stopping at it, and ends up standing -- one release, one motion.
//   4. Coming off the floor is not faster than going down onto it just because
//      the body is aimed further -- that is what the per-segment speed cap is
//      for, and it is the whole reason the rise reads as getting up.
//   5. A crouch that is never held long enough behaves exactly as it always
//      did, and flying cancels the whole thing.
#include <cmath>
#include <cstdio>
#include "render/player.h"
#include "scene/voxelworld.h"
using namespace v2;

static int fails = 0;
static const char *chk(bool ok, const char *good, const char *bad) {
    if (!ok) ++fails;
    return ok ? good : bad;
}

static const float dt = 1.0f / 60.0f;
static const Vec3 still(0.0f, 0.0f, 0.0f);

int main() {
    VoxelTerrain terrain;
    WalkWorld w;
    w.terrain = &terrain;

    Player p;
    p.placeOnGround(w, 0.0f, 0.0f);
    const float standEye = p.eyeHeight();
    const float crouchEye = standEye * Player::kCrouchEyeMul;
    const float proneEye = standEye * Player::kProneEyeMul;
    std::printf("eye heights: standing %.3f m, crouch %.3f m, prone %.3f m\n\n", standEye, crouchEye,
                proneEye);

    // ---- 1. THE CROUCH COMES FIRST, AND IT HOLDS --------------------------
    //
    // Sampled at 250 ms, comfortably after the 150 ms crouch ease and before
    // the 350 ms hold window expires: the eye must be AT the crouch, not on
    // its way past it. This is the check that a "hold to go prone" has not
    // quietly become "the crouch is now slower".
    float toCrouch = -1.0f;
    for (int i = 0; i < 15; ++i) {
        p.update(w, still, false, false, true, true, dt);
        // 95% of the ease and not 99%: the crouch is asymptotic and always was,
        // and its last 1% lands at 354 ms -- past the hold window, which would
        // make this a test of the threshold rather than of the crouch.
        if (toCrouch < 0.0f && p.crouchAmount() > 0.95f) toCrouch = float(i + 1) * dt;
    }
    const float atHoldEdge = p.eyeHeight();
    std::printf("  1. crouch reached at %.0f ms; eye at 250 ms = %.3f m  [%s]\n", toCrouch * 1000.0f,
                atHoldEdge, chk(toCrouch > 0.0f && toCrouch < 0.25f &&
                                    std::fabs(atHoldEdge - crouchEye) < 0.04f && p.proneAmount() == 0.0f,
                                "CROUCHED, not yet prone", "WRONG -- did not settle at the crouch"));

    // ---- 2. KEEP HOLDING AND THE BODY GOES FLAT ---------------------------
    int downTicks = 0, downHalf = 0;
    bool pastHalf = false;
    for (int i = 0; i < 180; ++i) {  // 3 s
        p.update(w, still, false, false, true, true, dt);
        if (p.proneAmount() > 0.0f && p.proneAmount() < 0.99f) ++downTicks;
        if (!pastHalf && p.proneAmount() > 0.0f) {
            ++downHalf;
            if (p.proneAmount() >= 0.5f) pastHalf = true;
        }
    }
    std::printf("  2. lay down over %.0f ms; eye now %.3f m  [%s]\n", float(downTicks) * dt * 1000.0f,
                p.eyeHeight(),
                chk(p.proneAmount() > 0.99f && std::fabs(p.eyeHeight() - proneEye) < 0.01f,
                    "PRONE", "WRONG -- never reached the prone height"));

    // ---- 3. ONE RELEASE, PAST THE CROUCH, ALL THE WAY UP ------------------
    //
    // The pass-through is the point: there must be a frame at the crouch
    // height, and NO run of frames parked there. A body that stops at the
    // crouch and waits for a second release fails on dwell, not on the end
    // state, so both are measured.
    float toCrouchLine = -1.0f, toStanding = -1.0f, upHalf = -1.0f;
    int dwell = 0;
    for (int i = 0; i < 180; ++i) {
        p.update(w, still, false, false, false, false, dt);
        const float t = float(i + 1) * dt;
        if (upHalf < 0.0f && p.proneAmount() <= 0.5f) upHalf = t;
        if (toCrouchLine < 0.0f && p.proneAmount() == 0.0f) toCrouchLine = t;
        if (std::fabs(p.eyeHeight() - crouchEye) < 0.02f) ++dwell;
        if (toStanding < 0.0f && p.crouchAmount() < 0.01f) toStanding = t;
    }
    std::printf("  3. crossed the crouch line at %.0f ms, standing at %.0f ms, %d frames within\n"
                "     2 cm of the crouch height; final eye %.3f m  [%s]\n",
                toCrouchLine * 1000.0f, toStanding * 1000.0f, dwell, p.eyeHeight(),
                chk(toCrouchLine > 0.0f && toStanding > toCrouchLine && dwell <= 4 &&
                        std::fabs(p.eyeHeight() - standEye) < 0.01f,
                    "ROSE THROUGH THE CROUCH IN ONE GO", "WRONG -- stalled at the crouch or short"));

    // ---- 4. GETTING UP IS NOT A LAUNCH ------------------------------------
    //
    // Without the speed cap the rise crossed the prone segment in 108 ms
    // against the ~460 ms it took to lie down, purely because the ease was
    // aimed twice as far. Half is the generous version of that bound.
    const float downMs = float(downHalf) * dt * 1000.0f, upMs = upHalf * 1000.0f;
    std::printf("  4. the near half of the prone segment: %.0f ms down, %.0f ms up  [%s]\n", downMs,
                upMs,
                chk(upMs > downMs * 0.6f, "the cap is doing its job",
                    "WRONG -- the rise is aimed-distance fast, the cap is gone"));

    // ---- 5. A TAP IS STILL JUST A CROUCH, AND FLYING CANCELS IT -----------
    //
    // 300 ms held, under the 350 ms window: the eye must dip to the crouch and
    // come back without ever going below it.
    float lowest = standEye;
    for (int i = 0; i < 18; ++i) {
        p.update(w, still, false, false, true, true, dt);
        lowest = std::fmin(lowest, p.eyeHeight());
    }
    for (int i = 0; i < 60; ++i) {
        p.update(w, still, false, false, false, false, dt);
        lowest = std::fmin(lowest, p.eyeHeight());
    }
    const bool tapOk = lowest > crouchEye - 0.01f && std::fabs(p.eyeHeight() - standEye) < 0.01f;
    std::printf("  5. a 300 ms tap bottomed out at %.3f m (crouch is %.3f)  [%s]\n", lowest,
                crouchEye, chk(tapOk, "still just a crouch", "WRONG -- a tap went prone"));

    p.fly = true;
    for (int i = 0; i < 120; ++i) p.update(w, still, false, false, true, true, dt);
    std::printf("     ...and 2 s of held crouch in FLY leaves the eye at %.3f m  [%s]\n",
                p.eyeHeight(),
                chk(std::fabs(p.eyeHeight() - standEye) < 0.01f, "upright", "WRONG -- crouched in flight"));

    std::printf("\n%s\n", fails ? "FAILED" : "all good");
    return fails ? 1 : 0;
}
