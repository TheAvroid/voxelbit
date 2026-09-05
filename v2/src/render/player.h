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
//     (both since raised -- see walk and jumpVel)
//     SPRINT x1.85                      GRAVITY 200   -> 20 m/s^2
//     EYE 18 vox      -> 1.80 m         HEIGHT 20 vox -> 2.00 m
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
    const Solid *solids = nullptr;
    int solidCount = 0;
};

class Player {
  public:
    // Feet, in world metres. The camera sits `eye` above this.
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float vy = 0.0f;
    bool onGround = false;
    bool fly = false;

    float walk = 9.2f;        // m/s -- the JS engine's 4.6, doubled
    float sprintMul = 1.85f;
    // The port's was 6.6 (JUMP 66 vox/s), which apexes at 1.09 m. Raised 50%
    // in HEIGHT on the user's ask -- and height goes as v^2/2g, so that is
    // sqrt(1.5) on the velocity, not 1.5. 1.09 m -> 1.63 m, while the hang
    // time only lengthens by 22%, which is the point of taking it this way:
    // 1.5x the velocity would have been a 2.45 m moon jump.
    float jumpVel = 8.08f;    // m/s up at the moment of the jump; apex 1.63 m
    float gravity = 20.0f;    // m/s^2
    float eye = 1.80f;        // 18 voxels
    float halfWidth = 0.26f;  // 2.6 voxels, as in the JS engine

    // How far up a step can be climbed without jumping, and how far down the
    // feet will follow the ground before the player is considered to have
    // walked off an edge. Both matter on voxel terrain: without a step-up the
    // player is stopped by every 10 cm lip, and without the step-down a stride
    // downhill is a series of tiny falls.
    float stepUp = 0.62f;
    float stepDown = 0.62f;

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

    // stepLag_ is carried here and NOT in pos, so the physics still sees the
    // feet exactly on the ground while the eye is still catching up.
    Vec3 eyePosition() const { return Vec3(pos.x, pos.y + eye + camBobY + stepLag_, pos.z); }
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
    void update(const WalkWorld &w, Vec3 move, bool sprint, bool jump, bool down, float dt) {
        if (fly) {
            const float spd = walk * 3.0f * (sprint ? sprintMul : 1.0f);
            const float k = 1.0f - expf(-10.0f * dt);
            hvx_ += (move.x * spd - hvx_) * k;
            hvz_ += (move.z * spd - hvz_) * k;
            pos.x += hvx_ * dt;
            pos.z += hvz_ * dt;
            if (jump) pos.y += spd * dt;
            if (down) pos.y -= spd * dt;
            vy = 0.0f;
            onGround = false;
        } else {
            const float spd = walk * (sprint ? sprintMul : 1.0f);
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
                const float g = groundHeight(w, pos.x, pos.z);
                if (pos.y <= g && vy <= 0.0f) {
                    pos.y = g;
                    vy = 0.0f;
                    onGround = true;
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
    float groundHeight(const WalkWorld &w, float x, float z) const {
        const float hw = halfWidth;
        float best = -1e9f;
        for (int c = 0; c < 4; ++c) {
            const float cx = x + ((c & 1) ? hw : -hw);
            const float cz = z + ((c & 2) ? hw : -hw);
            const int i = int(floorf(cx / VOXEL_M));
            const int j = int(floorf(cz / VOXEL_M));
            best = maxf(best, float(w.terrain->heightVox(i, j) + 1) * VOXEL_M);
        }
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            if (!s.standable || s.top <= best) continue;
            if (touches(s, x, z, hw)) best = s.top;
        }
        return best;
    }

    // True if the body at (x, z) is inside something that is a wall at every
    // height -- a trunk. Rocks are deliberately not here: they are already
    // walls, by being floors that are too tall to step onto.
    bool blocked(const WalkWorld &w, float x, float z) const {
        for (int i = 0; i < w.solidCount; ++i) {
            const Solid &s = w.solids[i];
            if (!s.standable && touches(s, x, z, halfWidth)) return true;
        }
        return false;
    }

    // The nearest spot to (x, z) that is not inside a trunk. In rings outward,
    // so the answer is the closest one and a spawn moves as little as it must.
    void findClear(const WalkWorld &w, float *x, float *z) const {
        if (!blocked(w, *x, *z)) return;
        for (float r = 0.4f; r <= 6.0f; r += 0.4f)
            for (int a = 0; a < 16; ++a) {
                const float th = float(a) * (TWO_PI / 16.0f);
                const float cx = *x + cosf(th) * r, cz = *z + sinf(th) * r;
                if (!blocked(w, cx, cz)) {
                    *x = cx;
                    *z = cz;
                    return;
                }
            }
    }

  private:
    float hvx_ = 0.0f, hvz_ = 0.0f;

    // How far the eye is still behind the feet after a step, in metres. Always
    // decaying toward zero; never read by anything that decides where the body
    // is.
    float stepLag_ = 0.0f;

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
