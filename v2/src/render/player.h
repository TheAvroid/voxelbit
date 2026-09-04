// ---------------------------------------------------------------------------
// player.h -- a person standing on the ground, rather than a camera in space.
//
// The constants are ported from the JS engine's sim/player.js rather than
// invented, because that feel was already tuned and there is no reason for the
// two engines to walk differently. They convert exactly: that engine measures
// in VOXELS and v2 measures in METRES, and a voxel is 10 cm.
//
//     WALK 46 vox/s   -> 4.6 m/s        JUMP 66 vox/s -> 6.6 m/s
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
// The trees and rocks are NOT solid. They are instanced models with no
// representation in the height field, and making them collide would mean
// tracing against the IAS -- a different and much larger job than this.
// ---------------------------------------------------------------------------
#pragma once

#include "../scene/voxelworld.h"

namespace v2 {

class Player {
  public:
    // Feet, in world metres. The camera sits `eye` above this.
    Vec3 pos{0.0f, 0.0f, 0.0f};
    float vy = 0.0f;
    bool onGround = false;
    bool fly = false;

    float walk = 9.2f;        // m/s -- the JS engine's 4.6, doubled
    float sprintMul = 1.85f;
    float jumpVel = 6.6f;     // m/s upward at the moment of the jump
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

    // The head bob, also from the JS engine. camBobY is added to the eye, never
    // to `pos` -- physics and ground contact must not see it.
    float bobAmp = 0.0f, bobPhase = 0.0f, camBobY = 0.0f;
    static constexpr float kCamBob = 0.055f;    // 0.55 voxels
    static constexpr float kCamBobRun = 0.65f;  // extra swing once past a walk

    // stepLag_ is carried here and NOT in pos, so the physics still sees the
    // feet exactly on the ground while the eye is still catching up.
    Vec3 eyePosition() const { return Vec3(pos.x, pos.y + eye + camBobY + stepLag_, pos.z); }
    float speed() const { return sqrtf(hvx_ * hvx_ + hvz_ * hvz_); }

    void placeOnGround(const VoxelTerrain &t, float x, float z) {
        pos = Vec3(x, groundHeight(t, x, z), z);
        vy = 0.0f;
        onGround = true;
        stepLag_ = 0.0f;
    }

    // -----------------------------------------------------------------------
    // One tick. `move` is the desired horizontal direction, already normalised.
    // -----------------------------------------------------------------------
    void update(const VoxelTerrain &t, Vec3 move, bool sprint, bool jump, bool down, float dt) {
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
            moveAxis(t, 0, hvx_ * dt);
            moveAxis(t, 2, hvz_ * dt);

            if (onGround && jump) {
                vy = jumpVel;
                onGround = false;
            }

            if (!onGround) {
                vy -= gravity * dt;
                pos.y += vy * dt;
                const float g = groundHeight(t, pos.x, pos.z);
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

    // The surface the feet rest on, sampled at the four corners of the body and
    // taken at its highest. One sample at the centre lets half the player sink
    // into a step they are standing against.
    float groundHeight(const VoxelTerrain &t, float x, float z) const {
        const float w = halfWidth;
        float best = -1e9f;
        for (int c = 0; c < 4; ++c) {
            const float cx = x + ((c & 1) ? w : -w);
            const float cz = z + ((c & 2) ? w : -w);
            const int i = int(floorf(cx / VOXEL_M));
            const int j = int(floorf(cz / VOXEL_M));
            best = maxf(best, float(t.heightVox(i, j) + 1) * VOXEL_M);
        }
        return best;
    }

  private:
    float hvx_ = 0.0f, hvz_ = 0.0f;

    // How far the eye is still behind the feet after a step, in metres. Always
    // decaying toward zero; never read by anything that decides where the body
    // is.
    float stepLag_ = 0.0f;

    void moveAxis(const VoxelTerrain &t, int axis, float d) {
        if (d == 0.0f) return;
        Vec3 next = pos;
        if (axis == 0) next.x += d; else next.z += d;
        const float g = groundHeight(t, next.x, next.z);

        if (onGround) {
            if (g > pos.y + stepUp) return;  // a wall, not a step
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
            // Airborne: only a surface ABOVE the feet can block. Anything at or
            // below is simply air being flown through.
            if (g > pos.y) return;
            pos = next;
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

        // The JS phase rate is 0.225 per voxel travelled; a voxel is 0.1 m, so
        // the same stride length in metres is 2.25.
        bobPhase += spd2 * dt * 2.25f;
        if (bobPhase > TWO_PI * 1024.0f) bobPhase -= TWO_PI * 1024.0f;  // keep float precision

        // Sprinting is the same curve pushed further rather than a second rule:
        // bobAmp saturates at a walk, and this term takes over above it.
        const float run = clampf(spd2 / walk - 1.0f, 0.0f, 1.0f);
        camBobY = -cosf(bobPhase) * kCamBob * bobAmp * (1.0f + kCamBobRun * run);
    }
};

}  // namespace v2
