// ---------------------------------------------------------------------------
// player.h -- a person standing on the ground, rather than a camera in space.
//
// The constants are ported from the JS engine's sim/player.js rather than
// invented, because that feel was already tuned and there is no reason for the
// two engines to walk differently. They convert exactly: that engine measures
// in VOXELS and v2 measures in METRES, and a voxel is 10 cm. The head bob is
// the exception, and knowingly so -- see kBobRate.
//
//     WALK 46 vox/s   -> 4.6 m/s        JUMP 66 vox/s -> 6.6 m/s
//     (jump since raised -- see jumpVel; the walk went up to 9.2 and has
//      since come back to 4.97, which is within a rounding error of the
//      original 4.6 -- see the note on walk)
//     SPRINT x1.85                      GRAVITY 200   -> 20 m/s^2
//     EYE 20 vox      -> 2.00 m         (was 18; see below)
//
// THE EYE IS THE ONLY HEIGHT THIS ENGINE HAS. The table above used to carry
// both an eye at 18 voxels and a height at 20, which is the right relationship
// for a body -- the top of a head is a couple of voxels above the eyes in it.
// But nothing ever read the second number: collision is a vertical cylinder
// tested in x and z, there is no head to bump on anything, and the camera is
// the whole of the figure as far as the engine is concerned. So a "height" of
// 20 that put the viewpoint at 18 was a figure that measured 20 on paper and
// stood 1.8 m in the world, which is what it looked like. The eye is 20 voxels
// now and the number means what it says.
//
// Gravity is 20 m/s^2, not 9.81. Real gravity makes a jump feel like a moon
// landing -- the arc is right but it takes twice as long, and the hang time
// reads as floating. Doubling it keeps the same jump height at twice the
// cadence, which is what every first-person game does and why they all feel
// crisper than reality.
//
// THE GROUND IS QUERIED, NOT TRACED. The terrain is a height field and a pure
// function of position, so the surface under the player is a direct evaluation
// -- no ray, no acceleration structure, no dependence on which chunks happen to
// be resident. That also means the player can walk into terrain the renderer
// has not built yet without falling through the world.
//
// THE TREES AND ROCKS ARE SOLID, and they are solid without a ray either. Each
// one is placed with a measured collider beside its instance transform (see
// collide.h), and the player is handed the handful of them that are nearby. So
// both halves of the world are queried the same way -- the ground by evaluating
// it, the decor by looking in a short list -- and neither of them has to ask
// the GPU where anything is.
//
// They are NOT solid in the same way as each other, because they are not the
// same kind of obstacle:
//
//   A TRUNK is a wall at every height and never a floor. Its top is a canopy
//   twenty metres up, and a body put on top of that would be standing in the
//   air over a tree.
//
//   A ROCK is a floor with a top, which is all it takes to be a wall as well:
//   the step logic already refuses a surface more than stepUp above the feet.
//   That one rule gives a small stone you step onto, a boulder you are stopped
//   by, and a rock you can land on from a jump, without any of the three being
//   written down separately.
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/collide.h"
#include "../scene/voxelworld.h"

namespace v2 {

// The ground, plus whatever decor is close enough to matter. The solids are
// borrowed for the call and not kept: the viewer regathers them every tick,
// because which chunks are resident changes underfoot.
struct WalkWorld {
    const VoxelTerrain *terrain = nullptr;
    // WHERE THE HOLES ARE. Null until something has dug one, and null is a
    // legal answer meaning "the generator's world is the whole world" -- see
    // TerrainProbe, which is what reads this. It is here beside the terrain
    // rather than passed alongside it because the two together ARE the ground:
    // anything holding one and not the other is looking at a world that has
    // not been dug in.
    const EditStore *edits = nullptr;
    const Solid *solids = nullptr;
    int solidCount = 0;
};

// ---------------------------------------------------------------------------
// Is this point inside the world?
//
// The same two tests the swing uses, asked of a point rather than of a ray: the
// terrain is a height field, so one comparison; a trunk or a boulder is the
// upright ellipse the body already walks into.
//
// HERE RATHER THAN WITH EITHER OF ITS CALLERS, because both of them ask exactly
// this and a second copy would be two answers to one question. An arrow buries
// itself in whatever it meets first (render/arrows.h) and a butterfly turns
// away from it (render/butterflies.h) -- different verbs, one predicate, and
// the JS engine's rule under both: ground, trunk or branch alike.
//
// THE MODEL HALF IS THE MODEL'S VOXELS. It used to be the collider ellipse,
// which is a cylinder the width of the widest two metres above a model's base:
// an arrow stopped in mid-air a metre off the side of a boulder, and a flier
// turned away from a column of nothing above a rock. See solidAtWorld.
// ---------------------------------------------------------------------------
inline bool insideWorld(const WalkWorld &w, const Vec3 &p) {
    if (!w.terrain) return false;
    const int i = int(floorf(p.x / VOXEL_M)), j = int(floorf(p.z / VOXEL_M));
    const int y = int(floorf(p.y / VOXEL_M));
    if (y <= w.terrain->heightVox(i, j)) return true;
    for (int k = 0; k < w.solidCount; ++k) {
        const Solid &s = w.solids[k];
        if (s.hx <= 0.0f || s.hz <= 0.0f || p.y > s.top) continue;
        if (s.vol) {
            if (solidAtWorld(s, p.x, p.y, p.z, VOXEL_M)) return true;
            continue;
        }
        const float dx = (p.x - s.cx) / s.hx, dz = (p.z - s.cz) / s.hz;
        if (dx * dx + dz * dz < 1.0f) return true;
    }
    return false;
}

class Player {
  public:
    // Feet, in world metres. The camera sits `eye` above this.
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float vy = 0.0f;
    bool onGround = false;
    bool fly = false;

    // THE WHOLE GAIT MOVED DOWN ONE STEP: what used to be a walk is now a run.
    //
    // 9.2 m/s on foot was a sprinter's pace held indefinitely, and sprinting
    // from it reached 17 -- fast enough that the chunk streamer, not the
    // terrain, was setting how far you could see. The anchor for the new
    // numbers is that OLD WALK, kept exactly, as the new top speed:
    //
    //     run  = walk * sprintMul = 4.97 * 1.85 = 9.19  (the old 9.2 walk)
    //     walk = 4.97                                    (a brisk walk)
    //
    // So this is 9.2 / 1.85 rather than a round number, and it is written that
    // way round on purpose: the thing being preserved is the sprint, and the
    // walk is whatever falls out of it. Change sprintMul and this has to move
    // with it or the old walk stops being the new run.
    //
    // Nothing else needed touching. The head bob is expressed as a fraction of
    // `walk` (see updateBob), so the gait rescales itself, and fly mode is
    // walk * 3 and comes down with it.
    float walk = 4.97f;       // m/s -- old walk / sprintMul, so sprint == old walk
    float sprintMul = 1.85f;
    // The port's was 6.6 (JUMP 66 vox/s), which apexes at 1.09 m. Raised 50%
    // in HEIGHT on the user's ask -- and height goes as v^2/2g, so that is
    // sqrt(1.5) on the velocity, not 1.5. 1.09 m -> 1.63 m, while the hang
    // time only lengthens by 22%, which is the point of taking it this way:
    // 1.5x the velocity would have been a 2.45 m moon jump.
    float jumpVel = 8.08f;    // m/s up at the moment of the jump; apex 1.63 m
    float gravity = 20.0f;    // m/s^2
    float eye = 2.00f;        // 20 voxels -- and the whole of the figure,
                              // since nothing is modelled above the eye
    float halfWidth = 0.26f;  // 2.6 voxels, as in the JS engine

    // How far up a step can be climbed without jumping, and how far down the
    // feet will follow the ground before the player is considered to have
    // walked off an edge. Both matter on voxel terrain: without a step-up the
    // player is stopped by every 10 cm lip, and without the step-down a stride
    // downhill is a series of tiny falls.
    float stepUp = 0.62f;
    float stepDown = 0.62f;

    // How much faster each successive mushroom throws you, and the ceiling on
    // it. 3x the jump speed is 9x the jump height.
    static constexpr float kBounceGain = 1.35f;
    static constexpr float kBounceMax = 3.0f;

    // EVERY mushroom, HALF AS HIGH AGAIN -- and half again is not half again
    // as fast.
    //
    // Height goes as v^2/2g, so this is sqrt on the launch speed every time,
    // exactly as the 50% jump raise above took sqrt(1.5). This factor has been
    // raised twice now and the two compound in HEIGHT, not in speed: it was
    // sqrt(1.25) for a quarter more, and a further half again over that is
    // 1.25 * 1.5 = 1.875, so sqrt(1.875). Taking the naive 1.5 would have been
    // a 125% raise instead of 50%, and a cap worth twenty-five jumps.
    //
    // Applied to the LAUNCH SPEED rather than folded into kBounceGain, because
    // the gain compounds: a factor in there would be 50% on the first bounce,
    // 125% on the second and away. This lifts the whole ladder by the same
    // half, capped rung included -- every rung below is 1.500x what it was:
    //
    //     first mushroom    3.72 m -> 5.58 m
    //     second           6.78 m -> 10.16 m
    //     third           12.35 m -> 18.53 m
    //     capped          18.36 m -> 27.54 m
    //
    // The ceiling is still 3x the jump SPEED in the chain, now landing at
    // 4.108x and 16.875 jump heights. Worth knowing what that reaches: the
    // trees are 30.5 m, so a capped bounce now arrives just under the canopy
    // rather than at half its height.
    static constexpr float kBounceBoost = 1.3693064f;  // sqrt(1.875)

    // How fast the eye catches up after a step, per second. 18 is about a
    // 55 ms tail: long enough to remove the jolt, short enough that the view
    // never feels like it is trailing the body.
    float stepSmooth = 18.0f;

    // The head bob. camBobY is added to the eye, never to `pos` -- physics and
    // ground contact must not see it.
    float bobAmp = 0.0f, bobPhase = 0.0f, camBobY = 0.0f;

    // THE ONE PLACE THAT DELIBERATELY LEAVES THE PORT BEHIND. The JS engine
    // bobs at 0.225 of phase per voxel travelled -- 2.25 per metre -- with a
    // 0.55 voxel swing. Those numbers were tuned against ITS walk speed, and v2
    // doubled that speed, which turned the same bob into a fast shallow tremor:
    // the right total movement, delivered too quickly to read as anything but
    // vibration.
    //
    // Half the rate and twice the swing spreads the same motion over twice the
    // distance -- a slow deep roll that reads as weight rather than jitter. The
    // two go together: halving the rate alone flattens the walk into a drift,
    // and doubling the swing alone is seasickness at this cadence.
    static constexpr float kBobRate = 1.125f;   // radians of phase per metre
    static constexpr float kCamBob = 0.11f;     // 1.1 voxels, twice the port's
    static constexpr float kCamBobRun = 0.65f;  // extra swing once past a walk

    // -- THE CROUCH, from the JS engine's tick-body.js and sim/player.js -----
    //
    // Held, not toggled, and on CAPS LOCK -- which is where that engine put it
    // on 2026-08-05, off Alt, off C before that. DEFBINDS names it
    // `crouch: 'CapsLock'` and BINDNAMES calls the row "crouch / fly down", so
    // the same key descends in flight, and v2 does both.
    //
    // THE DROP IS SEVEN VOXELS, and it is taken off the HEIGHT rather than off
    // the eye. That engine carries both -- EYE 18.5 with HEIGHT 20, CR_EYE 11.5
    // with CR_HEIGHT 13 -- and v2 deliberately carries one number that is both
    // (see the note at the top of this file: nothing is modelled above the
    // eye). 20 -> 13 is the pair that matches what v2's number MEANS, and it
    // is the same seven voxels either way.
    //
    // A FRACTION AND NOT A HEIGHT, so it survives the eye slider: crouching is
    // 65% of however tall you are, not a hardcoded 1.3 m that would put a
    // shortened player's head underground and a raised one's barely lower.
    static constexpr float kCrouchEyeMul = 13.0f / 20.0f;  // CR_HEIGHT / HEIGHT
    static constexpr float kCrouchSpeed = 0.45f;           // CROUCHM
    // 13 per second -- the ~150 ms ease that engine's tick-body.js uses, so the
    // eye sinks rather than snapping. There is nothing to hurry for: v2 has no
    // overhead clearance test to beat (see blocked(), which is a wall at every
    // height), so unlike the port there is no instant collision height that the
    // smoothing has to be kept away from.
    static constexpr float kCrouchRate = 13.0f;

    // How high the eye is standing right now -- `eye` full up, kCrouchEyeMul of
    // it fully down, and interpolated between while the crouch eases. The JS
    // engine's eyeH, and the reason the crouch is a smooth sink rather than a
    // cut. NOT written back into `eye`, which is a TUNING value the menu edits
    // and a bake writes: folding a crouch into it would bake a crouched player
    // as the height everyone spawns at.
    float eyeHeight() const { return eye * (1.0f - crouchT_ * (1.0f - kCrouchEyeMul)); }
    // How far into the crouch the eye is, 0..1 -- for anything that wants to
    // show it. The movement itself does not go through here.
    float crouchAmount() const { return crouchT_; }

    // stepLag_ is carried here and NOT in pos, so the physics still sees the
    // feet exactly on the ground while the eye is still catching up.
    Vec3 eyePosition() const {
        return Vec3(pos.x, pos.y + eyeHeight() + camBobY + stepLag_, pos.z);
    }
    float speed() const { return sqrtf(hvx_ * hvx_ + hvz_ * hvz_); }

    // Put the body on the ground at (x, z), stepping aside first if that spot
    // is inside a trunk. The start position is a fixed point on a procedural
    // map, so it can perfectly well be a tree; being welded into one before the
    // first frame is drawn is a poor introduction to a wood.
    void placeOnGround(const WalkWorld &w, float x, float z) {
        findClear(w, &x, &z);
        pos = Vec3(x, groundHeight(w, x, z), z);
        vy = 0.0f;
        onGround = true;
        stepLag_ = 0.0f;
    }

    // -----------------------------------------------------------------------
    // One tick. `move` is the desired horizontal direction, already normalised.
    // -----------------------------------------------------------------------
    void update(const WalkWorld &w, Vec3 move, bool sprint, bool jump, bool down, bool crouch,
                float dt) {
        // NOT WHILE FLYING, exactly as tick-body.js has it: in the air the same
        // key is a descent and there is no gait for it to shorten. The eye
        // therefore rises back to full the moment F is pressed, which is what
        // you want -- a flying crouch is a camera dropped for no reason.
        const bool crouching = crouch && !fly;
        crouchT_ += ((crouching ? 1.0f : 0.0f) - crouchT_) * (1.0f - expf(-kCrouchRate * dt));
        if (crouchT_ < 0.001f) crouchT_ = 0.0f;
        // THE CROUCH BEATS THE SPRINT rather than the two multiplying out to
        // something between them -- `sprint = keys.has(binds.sprint) &&
        // !crouching` in that engine, and holding both should not be a way to
        // creep at four fifths of a walk.
        sprint = sprint && !crouching;
        if (fly) {
            const float spd = walk * 3.0f * (sprint ? sprintMul : 1.0f);
            const float k = 1.0f - expf(-10.0f * dt);
            hvx_ += (move.x * spd - hvx_) * k;
            hvz_ += (move.z * spd - hvz_) * k;

            // FLYING IS NOT NOCLIP. This used to write straight into pos, so a
            // trunk, a boulder and the ground itself were all scenery you drifted
            // through -- and moveAxis already carries a note about a body left
            // "flown into a tree and dropped out of fly mode", which is the state
            // that produced.
            //
            // So it goes through the SAME two axis moves a walk does. Not a copy
            // of them: the wall rules, the step tolerance and the stuck-escape
            // that lets a body trapped inside something walk back out are all one
            // implementation, and a second one here would be free to disagree.
            //
            // onGround is set FIRST because moveAxis branches on it, and flying
            // is the airborne case: no stepping up, and a surface above the feet
            // blocks. It stays false afterwards -- see updateBob, where the gait
            // is already guarded on !fly.
            vy = 0.0f;
            onGround = false;
            moveAxis(w, 0, hvx_ * dt);
            moveAxis(w, 2, hvz_ * dt);

            if (jump) pos.y += spd * dt;
            if (down) pos.y -= spd * dt;
            // AND THE GROUND IS STILL A FLOOR. Descending is the one direction
            // the axis moves above cannot speak for, so it is clamped here --
            // against the same groundInfo a fall lands on, which is the terrain
            // AND the top of anything standable, so you settle onto a boulder
            // rather than into it.
            const Ground g = groundInfo(w, pos.x, pos.z);
            if (pos.y < g.y) pos.y = g.y;
        } else {
            const float spd =
                walk * (sprint ? sprintMul : 1.0f) * (crouching ? kCrouchSpeed : 1.0f);
            // Approached exponentially rather than set outright, and far more
            // slowly in the air (3.2 against 14): that difference IS the sense
            // of having weight, and of not being able to change your mind
            // mid-jump.
            const float k = 1.0f - expf(-(onGround ? 14.0f : 3.2f) * dt);
            hvx_ += (move.x * spd - hvx_) * k;
            hvz_ += (move.z * spd - hvz_) * k;

            // One axis at a time, so sliding along a wall still works: blocked
            // in x does not have to mean blocked in z.
            moveAxis(w, 0, hvx_ * dt);
            moveAxis(w, 2, hvz_ * dt);

            if (onGround && jump) {
                vy = jumpVel;
                onGround = false;
            }

            if (!onGround) {
                vy -= gravity * dt;
                pos.y += vy * dt;
                const Ground g = groundInfo(w, pos.x, pos.z);
                if (pos.y <= g.y && vy <= 0.0f) {
                    pos.y = g.y;
                    if (g.bouncy) {
                        // COMPOUNDING, AND CAPPED. Each landing multiplies the
                        // launch speed, so a run of mushrooms throws you higher
                        // every time -- and since height goes as the SQUARE of
                        // the speed, the cap is on the speed and is still worth
                        // nine times the jump in altitude. Uncapped this is not
                        // a trampoline, it is an escape from the atmosphere in
                        // about a dozen hops.
                        bounceMul_ = minf(bounceMul_ * kBounceGain, kBounceMax);
                        vy = jumpVel * bounceMul_ * kBounceBoost;
                        onGround = false;
                    } else {
                        vy = 0.0f;
                        onGround = true;
                        // The chain only survives while you keep finding
                        // mushrooms: touch real ground and it is spent.
                        bounceMul_ = 1.0f;
                    }
                }
            }
        }
        // The eye eases toward the feet rather than being nailed to them.
        // Decayed here, once, so it is frame-rate independent and so a step
        // taken during moveAxis has already been folded in.
        stepLag_ *= expf(-stepSmooth * dt);
        if (fabsf(stepLag_) < 1e-4f) stepLag_ = 0.0f;

        updateBob(dt);
    }

    // The surface the feet rest on: the terrain, and the top of any standable
    // solid the body is over, whichever is higher.
    //
    // The terrain is sampled at the four corners of the body and taken at its
    // highest -- one sample at the centre lets half the player sink into a step
    // they are standing against. A rock is not sampled but INTERSECTED, because
    // a stone can be narrower than the gap between two corners, and a collider
    // a body can straddle is a collider that flickers.
    // What the feet are resting on: how high, and whether it throws you back.
    struct Ground {
        float y = -1e9f;
        bool bouncy = false;
    };

    // THE COLUMN, NOT THE CYLINDER. The old version took a standable solid's
    // single `top` -- the model's highest voxel -- for every point inside its
    // footprint ellipse, which turns a domed boulder into a flat drum. You
    // could stand on air a metre out from the stone, and the surface you landed
    // on was nowhere near the one you could see.
    //
    // Now each candidate is asked for the height of the actual voxel column
    // under the body, and a body that is off the model or over an empty column
    // gets no answer at all rather than the top of the whole rock.
    //
    // SAMPLED AT FIVE POINTS, the four corners and the centre, and the HIGHEST
    // wins -- the same rule the terrain already used and for the same reason:
    // one sample at the centre lets half the body sink into the step it is
    // standing against.
    Ground groundInfo(const WalkWorld &w, float x, float z) const {
        const float hw = halfWidth;
        Ground g;
        for (int c = 0; c < 4; ++c) {
            const float cx = x + ((c & 1) ? hw : -hw);
            const float cz = z + ((c & 2) ? hw : -hw);
            const int i = int(floorf(cx / VOXEL_M));
            const int j = int(floorf(cz / VOXEL_M));
            g.y = maxf(g.y, float(w.terrain->heightVox(i, j) + 1) * VOXEL_M);
        }
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            // `top` is the model's highest voxel, so it still works as a cheap
            // rejection: nothing in this model can be above it.
            if (!s.standable || s.top <= g.y) continue;
            // THE MODEL'S FOOTPRINT, NOT THE WALL ELLIPSE -- see overModel.
            // `touches` is measured over the bottom two metres, so it rejected
            // a body standing on the wide middle of a big boulder and dropped
            // it through the stone. Anything without a column heightfield has
            // no better answer available and keeps the old test.
            if (s.col ? !overModel(s, x, z, VOXEL_M, hw) : !touches(s, x, z, hw)) continue;

            float hit = -1e9f;
            bool any = false;
            for (int c = 0; c < 5; ++c) {
                const float sx = x + ((c == 4) ? 0.0f : ((c & 1) ? hw : -hw));
                const float sz = z + ((c == 4) ? 0.0f : ((c & 2) ? hw : -hw));
                float y = 0.0f;
                if (solidColumnTop(s, sx, sz, VOXEL_M, &y)) {
                    hit = maxf(hit, y);
                    any = true;
                }
            }
            // No heightfield on this model: fall back to the old flat top
            // rather than letting the body through it.
            if (!any && !s.col) {
                hit = s.top;
                any = true;
            }
            if (any && hit > g.y) {
                g.y = hit;
                g.bouncy = s.bouncy;
            }
        }
        return g;
    }

    // The same surface a fall lands on -- terrain, and the voxel column of
    // any standable model over it. Public because the loose bodies need the
    // identical answer: see World::updateDebris.
    float surfaceAt(const WalkWorld &w, float x, float z) const {
        return groundInfo(w, x, z).y;
    }

    float groundHeight(const WalkWorld &w, float x, float z) const {
        return groundInfo(w, x, z).y;
    }

    // True if the body at (x, z) is inside something that is a wall at every
    // height -- a trunk. Rocks are deliberately not here: they are already
    // walls, by being floors that are too tall to step onto.
    //
    // THE TRUNK'S VOXELS, over the body's own height. The ellipse this used to
    // ask is a circle drawn round the widest part of the bottom two metres, so
    // it stopped a body short of a birch by the width of its bark and stopped
    // it dead where a leaning trunk's ellipse covered open air. Neither is a
    // large error; both are the kind you feel rather than see.
    bool blocked(const WalkWorld &w, float x, float z) const {
        // THE GROUND UNDER THE SPOT, not the body's current height. This is
        // asked of places the body is not standing yet -- the next step, and a
        // spawn point chosen before anything has a height at all -- so anchor
        // it where a body at (x, z) would actually have its feet.
        const float feet =
            w.terrain ? float(w.terrain->heightVox(int(floorf(x / VOXEL_M)),
                                                   int(floorf(z / VOXEL_M))) + 1) * VOXEL_M
                      : pos.y;
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            if (s.standable) continue;
            if (s.vol) {
                if (solidBoxOverlap(s, x, feet, z, feet + kBodyHeightM, halfWidth, VOXEL_M))
                    return true;
                continue;
            }
            if (touches(s, x, z, halfWidth)) return true;
        }
        return false;
    }

    // The nearest spot to (x, z) that is not inside a trunk. In rings outward,
    // so the answer is the closest one and a spawn moves as little as it must.
    // IS THIS OPEN GROUND -- which is a different question from "can I walk
    // here", and placement is the one case that has to ask it.
    //
    // blocked() refuses TRUNKS and nothing else, because a rock is standable
    // and walking into one is meant to put you on top of it. But a body PLACED
    // at a point inside a boulder is not put on top of anything: it takes
    // whatever height the column under it happens to have, and if that column
    // is a low one near the model's edge the body ends up in the stone with
    // the rest of the rock around it. That is the spawn inside a rock.
    //
    // So placement also refuses any standable model that stands proud of the
    // terrain here. Not the model's ellipse -- its actual voxel column, so a
    // spawn beside a boulder is still allowed.
    bool occupied(const WalkWorld &w, float x, float z) const {
        if (blocked(w, x, z)) return true;
        const float hw = halfWidth;
        float gy = -1e9f;
        for (int c = 0; c < 4; ++c) {
            const float cx = x + ((c & 1) ? hw : -hw);
            const float cz = z + ((c & 2) ? hw : -hw);
            gy = maxf(gy, float(w.terrain->heightVox(int(floorf(cx / VOXEL_M)),
                                                    int(floorf(cz / VOXEL_M))) +
                                1) *
                              VOXEL_M);
        }
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            if (!s.standable) continue;
            if (s.col ? !overModel(s, x, z, VOXEL_M, hw) : !touches(s, x, z, hw)) continue;
            float y = 0.0f;
            if (s.col && solidColumnTop(s, x, z, VOXEL_M, &y)) {
                if (y > gy + stepUp) return true;
            } else if (!s.col && s.top > gy + stepUp) {
                return true;
            }
        }
        return false;
    }

    void findClear(const WalkWorld &w, float *x, float *z) const {
        if (!occupied(w, *x, *z)) return;
        for (float r = 0.4f; r <= 6.0f; r += 0.4f)
            for (int a = 0; a < 16; ++a) {
                const float th = float(a) * (TWO_PI / 16.0f);
                const float cx = *x + cosf(th) * r, cz = *z + sinf(th) * r;
                if (!occupied(w, cx, cz)) {
                    *x = cx;
                    *z = cz;
                    return;
                }
            }
    }

  private:
    float hvx_ = 0.0f, hvz_ = 0.0f;
    // 0 standing, 1 fully crouched, and every value between while it eases.
    float crouchT_ = 0.0f;

    // How far the eye is still behind the feet after a step, in metres. Always
    // decaying toward zero; never read by anything that decides where the body
    // is.
    float stepLag_ = 0.0f;
    // 1.0 until the first mushroom; multiplied by every one after it, and spent
    // the moment the feet find ordinary ground.
    float bounceMul_ = 1.0f;

    void moveAxis(const WalkWorld &w, int axis, float d) {
        if (d == 0.0f) return;
        Vec3 next = pos;
        if (axis == 0) next.x += d; else next.z += d;

        // A trunk stops a walk and a fall alike, and unlike the ground it is
        // never something to step up onto.
        //
        // The `stuck` half of the test is what keeps a body that is somehow
        // already inside one -- spawned there, or flown into a tree and dropped
        // out of fly mode -- from being welded in place: if standing here is
        // blocked too then moving cannot make it worse, so let it move and walk
        // out. The ground test below is let off on the same grounds.
        const bool stuck = blocked(w, pos.x, pos.z);
        if (!stuck && blocked(w, next.x, next.z)) return;

        const float g = groundHeight(w, next.x, next.z);

        if (onGround) {
            const float here = groundHeight(w, pos.x, pos.z);
            if (g > pos.y + stepUp && here <= pos.y + stepUp) return;  // a wall, not a step
            pos = next;
            if (g >= pos.y - stepDown) {
                // STEP, SMOOTHED IN THE EYE ONLY.
                //
                // The feet snap to the new surface -- they have to, or the body
                // would be standing inside the step and every later query would
                // be answered from the wrong height. What was jagged was that
                // the CAMERA snapped with them: at 10 cm voxels a walk across
                // rolling ground is a stream of instant 10 cm jolts.
                //
                // So the difference is taken out of the eye and paid back over
                // ~55 ms. The feet are exact, the view is continuous, and
                // nothing that reads pos can tell the difference.
                stepLag_ = clampf(stepLag_ + (pos.y - g), -stepDown, stepUp);
                pos.y = g;
            } else {
                onGround = false;  // walked off a ledge; gravity takes it
            }
        } else {
            // Airborne. A surface above the feet blocks -- but only one you
            // could not have WALKED up, which is why this carries the same
            // stepUp tolerance the grounded branch does.
            //
            // WITHOUT IT, EVERY JUMP STICKS TO THE GROUND FOR ITS FIRST FRAMES.
            // The feet leave at 8 m/s, so they are 1 cm up after a tick at
            // 700 fps -- while groundHeight is the highest of the four body
            // corners, which crosses into the next 10 cm column the moment you
            // move. So a lip you would have strolled over becomes a wall for
            // however long it takes to rise 10 cm, and the whole horizontal
            // move is thrown away for those ticks. MEASURED, holding W+SPACE
            // for six seconds over this terrain: 5-14% of all airborne ticks
            // went nowhere, and a jumping run covered 47.6 m where a walking
            // one covered 55.2 m. It reads as the jump refusing to carry you
            // forward -- you go up, and come down where you started.
            if (g > pos.y + stepUp) return;  // a wall; a step is not
            pos = next;
            if (g > pos.y) {
                // Rode up onto a step in mid-air. The feet snap to it exactly
                // as they do on the ground -- and for the same reason, since
                // every later query is answered from pos -- with the jolt
                // taken out of the eye and paid back over ~55 ms.
                stepLag_ = clampf(stepLag_ + (pos.y - g), -stepDown, stepUp);
                pos.y = g;
            }
        }
    }

    // -----------------------------------------------------------------------
    // The bob, matching the JS engine including the thing it got wrong first.
    //
    // ONE DIP PER STRIDE, not per footfall. cos(phase * 2) is what a real head
    // does -- it dips on each foot -- but at this cadence it reads as a jitter
    // rather than as walking. cos(phase) is slower and reads as a gait.
    //
    // The amplitude is eased rather than set, so starting and stopping ramp the
    // bob in and out instead of switching it. It is zero in the air and zero
    // flying, which falls out of the same easing: the target simply goes to 0.
    // -----------------------------------------------------------------------
    void updateBob(float dt) {
        const float spd2 = speed();
        const float target = (onGround && !fly) ? minf(1.0f, spd2 / walk) : 0.0f;
        bobAmp += (target - bobAmp) * (1.0f - expf(-8.0f * dt));

        // Per METRE TRAVELLED, not per second: the bob has to stay locked to
        // the stride, or the gait would speed up and slow down with the walk
        // instead of lengthening.
        bobPhase += spd2 * dt * kBobRate;
        if (bobPhase > TWO_PI * 1024.0f) bobPhase -= TWO_PI * 1024.0f;  // keep float precision

        // Sprinting is the same curve pushed further rather than a second rule:
        // bobAmp saturates at a walk, and this term takes over above it.
        const float run = clampf(spd2 / walk - 1.0f, 0.0f, 1.0f);
        camBobY = -cosf(bobPhase) * kCamBob * bobAmp * (1.0f + kCamBobRun * run);
    }
};

}  // namespace v2
