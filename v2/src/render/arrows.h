// ---------------------------------------------------------------------------
// arrows.h -- what leaves the bow.
//
// A small fixed pool of shafts, each an instance in the same acceleration
// structure as everything else, so an arrow in the air is lit, shadowed and
// bounced exactly as the tool that loosed it is. Ported from the JS engine's
// sim/projectiles.js: the same launch profile, the same gravity, the same
// forgiveness about where a shaft comes to rest.
//
// ---------------------------------------------------------------------------
// A FIXED POOL, AND THAT IS THE WHOLE REASON IT IS AFFORDABLE
//
// The top-level structure is REFIT once a frame rather than rebuilt -- 0.05 ms
// against 1.15 -- and an update may not change how many instances there are.
// So the slots exist from the moment the world is built, every one of them,
// carrying an instance mask of zero until a shaft is actually in it. A pool
// that grew and shrank with the shooting would force a full rebuild on every
// loose and every landing, which is the most expensive possible moment to pay
// for one.
//
// TWELVE. A full draw carries about six seconds of flight and the bow will not
// loose faster than its own release animation, so twelve is several shots in
// the air at once with room to spare; past that the oldest is reused, which is
// what the JS engine does with its own pool.
//
// ---------------------------------------------------------------------------
// THE FLIGHT IS INTEGRATED AS IT GOES, NOT AT RELEASE
//
// The engine this comes from marches the WHOLE arc up front, at release, and
// keeps the impact point -- it can, because its world is a voxel array that is
// all resident. This world streams: a shaft loosed across a valley is aimed at
// chunks that are not built yet, so an arc integrated at release would be
// deciding where an arrow lands against terrain nobody has generated. Stepping
// it per frame asks the same question of the same height field, five
// milliseconds at a time, and gets an answer that is true when it matters.
//
// The step size IS that engine's: 5 ms, small enough that a shaft at full
// draw moves half a voxel a step and cannot tunnel through a trunk.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"
#include "../gpu/world.h"
#include "../scene/collide.h"
#include "../scene/voxelworld.h"
#include "player.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace v2 {

// -- the launch profile, from sim/projectiles.js ---------------------------
//
// ARROW_V is TWICE that engine's thrown profile -- "a bow beats an arm, and the
// flatter arc is the point of it" -- and ARROW_UP is the kick that goes with
// it. Its units are voxels a second; these are metres, which is the same
// numbers over ten.
inline constexpr float kArrowSpeed = 48.0f;  // ARROW_V 480 vox/s, at a full draw
inline constexpr float kArrowUp = 1.8f;      // ARROW_UP 18 vox/s
inline constexpr float kArrowG = -17.0f;     // TOSS_G -170 vox/s^2

// WHERE IT STARTS, and it is not the eye: the viewmodel sits a hand's breadth
// from the lens, far too close to spawn a full-size shaft, so the launch point
// is carried out along the view to where an arrow can be drawn.
inline constexpr float kArrowLaunchM = 0.6f;  // LAUNCH_D 6 voxels
// ...and it must still go WHERE YOU AIMED. Leaving the bow means leaving from a
// point to the side of the eye, so firing straight down the view sends the
// shaft along a parallel line that never crosses the crosshair. Aim at a
// distant point ON the sight line instead and the flight converges onto it
// within a few metres, the way a real bow sight does.
inline constexpr float kArrowAimM = 30.0f;  // AIM_FAR 300 voxels

// How long a landed shaft stays before its slot is free again. The JS engine
// leaves them in the world for good; here the pool is what limits it, and a
// minute is long enough that you can walk up to one you shot.
inline constexpr float kArrowRestSec = 60.0f;

inline constexpr int kArrowSlots = 12;

// ---------------------------------------------------------------------------
class Arrows {
  public:
    struct Shaft {
        Vec3 pos{0, 0, 0};
        Vec3 vel{0, 0, 0};
        // The direction it is POINTING, which stops being the direction it is
        // travelling the moment it lands: a shaft in the ground keeps the angle
        // it went in at.
        Vec3 dir{0, 0, 1};
        float age = 0.0f;
        bool live = false;   // in the air
        bool stuck = false;  // landed, and still standing in whatever it hit
        // Has the arc reached open air? False while a shaft is still inside
        // whatever it was loosed from -- see the note in launch().
        bool free_ = false;
    };

    bool init(World &world, const std::string &voxPath) {
        model_ = world.addHeldModel(voxPath, &sx_, &sy_, &sz_);
        return model_ >= 0;
    }
    // Print where each shaft lands. Off by default -- a line per arrow -- but
    // it is the only way to see the flight without waiting for one to catch
    // the light in the grass.
    bool log = false;

    bool ready() const { return model_ >= 0; }
    int model() const { return model_; }

    // -----------------------------------------------------------------------
    // Loose one. The oldest slot is taken when every one is busy, which is the
    // pool behaving as a pool rather than refusing the shot.
    // -----------------------------------------------------------------------
    void launch(const Vec3 &from, const Vec3 &vel) {
        if (!ready()) return;
        int slot = -1;
        float oldest = -1.0f;
        for (int i = 0; i < kArrowSlots; ++i) {
            if (!shafts_[i].live && !shafts_[i].stuck) { slot = i; break; }
            if (shafts_[i].age > oldest) { oldest = shafts_[i].age; slot = i; }
        }
        if (slot < 0) return;
        Shaft &a = shafts_[size_t(slot)];
        a = Shaft{};
        a.pos = from;
        a.vel = vel;
        const float l = sqrtf(maxf(1e-8f, lengthSq(vel)));
        a.dir = vel * (1.0f / l);
        a.live = true;
        // WAS IT LOOSED FROM INSIDE SOMETHING? Point-blank into a trunk, or
        // aimed steeply down so the launch point sits under the ground the
        // player is standing on. The JS engine's note is that an ungated
        // impact test then sticks the shaft instantly at arm's length, while
        // an ungated FLIGHT falls out of the bottom of the world. It asks the
        // question directly instead, and so does this: a shaft that starts
        // buried is not tested until it has reached open air. FALSE, so the
        // first step that finds open air is what arms the impact test -- a
        // shot into thin air arms it immediately and loses nothing.
        a.free_ = false;
    }

    // -----------------------------------------------------------------------
    // One tick of every shaft, at the JS engine's 5 ms.
    // -----------------------------------------------------------------------
    void update(float dt, const WalkWorld &w) {
        if (!ready()) return;
        // WHERE ANYTHING LANDED THIS TICK, for the impact sound. A list rather
        // than a flag because two shafts really can land on one frame -- that
        // is the same reason the JS engine pools its impact voice four deep --
        // and cleared here rather than by the reader, so a caller that forgets
        // to drain it cannot replay last frame's thud for ever.
        landed_.clear();
        for (Shaft &a : shafts_) {
            if (!a.live && !a.stuck) continue;
            a.age += dt;
            if (a.stuck) {
                if (a.age > kArrowRestSec) a = Shaft{};
                continue;
            }
            float left = minf(dt, 0.25f);  // a stalled frame must not teleport it
            while (left > 0.0f && a.live) {
                const float h = minf(left, 0.005f);
                left -= h;
                const float nvy = a.vel.y + kArrowG * h;
                const Vec3 step(a.vel.x * h, (a.vel.y + nvy) * 0.5f * h, a.vel.z * h);
                const Vec3 next = a.pos + step;
                a.vel.y = nvy;

                const bool blocked = insideWorld(w, next);
                if (!blocked) a.free_ = true;  // out in the open at last
                if (blocked && a.free_) {
                    // It comes to rest at the last point that was NOT inside
                    // anything, so the shaft stands in the surface rather than
                    // vanishing into it.
                    a.live = false;
                    a.stuck = true;
                    a.age = 0.0f;
                    landed_.push_back(a.pos);
                    if (log) {
                        std::printf("v2: arrow stuck at %.1f %.1f %.1f\n", double(a.pos.x),
                                    double(a.pos.y), double(a.pos.z));
                        std::fflush(stdout);
                    }
                    break;
                }
                a.pos = next;
                const float l = sqrtf(maxf(1e-8f, lengthSq(a.vel)));
                a.dir = a.vel * (1.0f / l);
                // Out of the world entirely -- under it, or so far up that
                // nothing can be struck on the way back down for a while.
                if (a.pos.y < -50.0f || a.age > 20.0f) {
                    a = Shaft{};
                    break;
                }
            }
        }
    }

    // -----------------------------------------------------------------------
    // Put every slot on the pipeline. Slots with nothing in them are written
    // with a mask of zero rather than skipped -- see the note at the top about
    // the instance count.
    // -----------------------------------------------------------------------
    void publish(World &world) const {
        if (!ready()) return;
        for (int i = 0; i < kArrowSlots; ++i) {
            const Shaft &a = shafts_[size_t(i)];
            if (!a.live && !a.stuck) {
                world.setArrowInstance(i, model_, nullptr, 0.0f, 0.0f, 0.0f, false);
                continue;
            }
            // The model runs along its own local Z after scene/vox.h's y-up
            // conversion -- the file lays the shaft down its depth axis -- so
            // that is the column the flight direction goes in. The other two
            // are any orthonormal pair; a shaft is round.
            const Vec3 f = a.dir;
            Vec3 up(0.0f, 1.0f, 0.0f);
            if (fabsf(f.y) > 0.99f) up = Vec3(1.0f, 0.0f, 0.0f);
            Vec3 r = cross(up, f);
            const float rl = sqrtf(maxf(1e-8f, lengthSq(r)));
            r = r * (1.0f / rl);
            const Vec3 u = cross(f, r);

            const float s = VOXEL_M;
            const float m[9] = {r.x * s, u.x * s, f.x * s, r.y * s, u.y * s,
                                f.y * s, r.z * s, u.z * s, f.z * s};
            // The mesh runs from its own corner, and `pos` is the middle of the
            // shaft, so the translation is the centre less half the model down
            // its own three axes.
            const Vec3 corner = a.pos - (r * (0.5f * float(sx_) * s) +
                                         u * (0.5f * float(sy_) * s) +
                                         f * (0.5f * float(sz_) * s));
            world.setArrowInstance(i, model_, m, corner.x, corner.y, corner.z, true);
        }
    }

    // Drained by the caller each frame -- see update().
    const std::vector<Vec3> &landedThisTick() const { return landed_; }

    int inFlight() const {
        int n = 0;
        for (const Shaft &a : shafts_)
            if (a.live) ++n;
        return n;
    }

  private:
    int model_ = -1;
    int sx_ = 0, sy_ = 0, sz_ = 0;
    std::vector<Shaft> shafts_ = std::vector<Shaft>(size_t(kArrowSlots));
    std::vector<Vec3> landed_;
};

}  // namespace v2
