#pragma once
// ---------------------------------------------------------------------------
// WHAT THE ASSAULT RIFLE THROWS.
//
// (user 2026-09-17: "currently the gun hits when left clicking. instead of that
// happening, have it shoot bullets then give the gun recoil when it shoots.
// create a 1 voxel bullet when it shoots. have it share the same properties as
// the spark voxel. so the bullet should appear at the tip of the gun and travel
// straight." / "also have the guns bullets take out a small chunk of the
// environment much like the arrow does.")
//
// -- WHY THIS IS NOT Arrows, WHICH IT OTHERWISE LOOKS LIKE ------------------
//
// An arrow is a SHAFT: it has a model with an orientation, it rolls about its
// own axis in flight, it arcs under gravity, and when it stops it STAYS, stuck
// in whatever it hit, as something you can walk up to and look at. Every one of
// those is state this file would have to carry and then ignore.
//
// A bullet is a point that goes in a straight line and stops existing. It has
// no gravity (see kBulletDrop), no roll, no pointing direction worth storing
// and nothing to leave behind. Sharing Arrows would mean a second set of
// branches through every one of those behaviours, in the file that already
// carries the bow's.
//
// WHAT IT DOES SHARE is the shape of the update: a substepped march so a round
// travelling 120 m/s cannot tunnel through a wall between two frames, the same
// question asked of the world on each substep, and impacts reported as a list
// rather than a flag because two can land on one tick.
//
// -- IT IS THE SPARK'S VOXEL, AND THAT IS THE USER'S ASK --------------------
//
// "have it share the same properties as the spark voxel." So this owns no
// model and no material: Particles::sparkModel() is handed in and published
// into this band's slots. The spark is one voxel of a PRIVATE emissive
// material (see V6Params::emitters and the long note in particles.h about what
// it cost to get one), which is exactly what a tracer round should look like --
// and it costs the 255-entry palette nothing, which nothing else in this engine
// can currently say.
// ---------------------------------------------------------------------------
#include <functional>
#include <vector>

#include "core/vecmath.h"
#include "gpu/world.h"
#include "render/player.h"

namespace v2 {

// kBulletSlots and kBulletSlot0 are declared in gpu/world.h with the rest of
// the flyer band -- see the note there for why the count lives beside the
// array's length and not beside the population.

// 120 m/s. Not a real muzzle velocity -- a real one crosses this whole map in
// under a second and would never be seen. This is fast enough to read as a
// bullet and slow enough to watch, which is the same trade v1 made for its
// arrow at 40.
inline constexpr float kBulletSpeed = 120.0f;

// A ROUND DOES NOT DROP. "travel straight", and at this range it is also true:
// 50 m at 120 m/s is 0.4 s, which is 80 cm of real drop -- but the ask is a
// straight line and a straight line is cheaper to be sure of. Kept as a named
// zero rather than deleted so the day somebody wants ballistics there is one
// place to put them.
inline constexpr float kBulletDrop = 0.0f;

// Long enough to cross the map, short enough that a shot into the sky does not
// sit in a slot for ever. 120 m/s for 1.2 s is 144 m; nuketown is 98 m on its
// long axis.
inline constexpr float kBulletLifeS = 1.2f;

// The march step, in metres. A round covers 2 m in a 60 Hz frame, so the frame
// itself is far too coarse a step to ask the world anything on -- at one test
// per frame a bullet passes clean through any wall thinner than two metres.
// 10 cm is the voxel, which is the finest answer the world can give.
inline constexpr float kBulletStepM = 0.10f;

// How far out the solids gather reaches while a round is up. Arrows use 90 m
// for the same reason; a bullet is faster but does not live as long.
inline constexpr float kBulletSolidsM = 90.0f;

// -- THE CYCLIC RATE, AND THE CLIMB THAT GOES WITH IT ----------------------
//
// 200 ms is 300 rounds a minute (user 2026-09-17: "slow down the firerate in
// half"). It shipped at 100 -- a real assault rifle's 600 -- and that is a lot
// of remeshing when every round takes a chip out of the map, as well as being
// faster than the eye can follow a tracer.
//
// IT NOW MATCHES THE RECOIL CURVE, which is a happy accident worth keeping: the
// kick is 40 ms out and 200 ms back, so at 200 ms between rounds the gun very
// nearly settles between shots and then is thrown again. At 100 it never came
// home at all.
inline constexpr double kBulletIntervalMs = 200.0;
// ...and how far the VIEW climbs per round, in degrees. A third of a degree:
// a ten-round burst walks the aim up three and a half, which is felt and not
// fought. Not decayed -- see App::fireRifle.
inline constexpr float kRifleClimbDeg = 0.34f;

// HOW FAR OUT THE BARREL AND THE CROSSHAIR MEET, in metres. The round starts at
// the muzzle and is aimed at a point this far along the view axis, so the two
// lines converge there and are within a few centimetres of each other over the
// whole of this map -- which is 98 m on its long axis. See App::fireRifle.
inline constexpr float kBulletConvergeM = 60.0f;

// ---------------------------------------------------------------------------
class Bullets {
  public:
    struct Round {
        Vec3 pos{0, 0, 0};
        Vec3 vel{0, 0, 0};
        float age = 0.0f;
        bool live = false;
        // -- HAS IT BEEN DRAWN WHERE IT WAS BORN ---------------------------
        //
        // (user 2026-09-17, twice: "the bullet doesnt seem to be coming from
        // the tip of the assault rifle".)
        //
        // THE MUZZLE WAS ALWAYS RIGHT AND NOBODY COULD EVER SEE IT. launch()
        // and update() run in the same frame, so a round was moved before it
        // was ever published -- and at 120 m/s a 60 Hz frame is TWO METRES. Its
        // first appearance was two metres downrange, well past the barrel and
        // past the player's own shoulder, which reads exactly like a tracer
        // coming from somewhere else.
        //
        // So the frame it is fired on, it does not move. One frame at the
        // barrel is 16 ms, which is a muzzle flash -- and the flash is at the
        // muzzle because that is literally where it is.
        bool born = true;
    };

    // WHERE ONE STOPPED AND WHICH WAY IT WAS GOING. The direction is what the
    // chip needs -- App::arrowChip backs off along it and re-probes, so that a
    // bullet and an arrow classify what they hit through the one function that
    // decides rock-or-trunk-or-ground for every blow in the game.
    struct Impact {
        Vec3 at{0, 0, 0};
        Vec3 dir{0, 0, 1};
    };

    // The spark's one-voxel model, borrowed rather than loaded. -1 until
    // Particles has been through its own load, which is why this is a call and
    // not a constructor argument.
    void useModel(int model) { model_ = model; }
    bool ready() const { return model_ >= 0; }

    void launch(const Vec3 &from, const Vec3 &dir) {
        if (!ready()) return;
        int slot = -1;
        float oldest = -1.0f;
        for (int i = 0; i < kBulletSlots; ++i) {
            if (!r_[size_t(i)].live) {
                slot = i;
                break;
            }
            if (r_[size_t(i)].age > oldest) {
                oldest = r_[size_t(i)].age;
                slot = i;
            }
        }
        if (slot < 0) return;
        const float l = sqrtf(maxf(1e-8f, lengthSq(dir)));
        Round &b = r_[size_t(slot)];
        b = Round{};
        b.pos = from;
        b.vel = dir * (kBulletSpeed / l);
        b.live = true;
        b.born = true;   // drawn at the barrel before it is allowed to travel
    }

    using LifeF = std::function<bool(const Vec3 &)>;

    // -----------------------------------------------------------------------
    // MARCHED, NOT STEPPED. See kBulletStepM: the frame is far too coarse to
    // ask the world on, so each round walks its own path in voxel-sized pieces
    // and the world is asked at every one of them.
    //
    // THE FIRST STEPS ARE NOT TESTED. A round is born at the muzzle, which is a
    // metre in front of the eye and can easily be inside a wall the player is
    // standing against -- and an ungated test then detonates it in the player's
    // face. Arrows::launch has the same guard and the same reason; this one
    // arms on distance rather than on open air because a bullet is not slow
    // enough for the difference to matter.
    // -----------------------------------------------------------------------
    void update(float dt, const WalkWorld &w, const LifeF &hitLife = nullptr) {
        impacts_.clear();
        if (!ready() || dt <= 0.0f) return;
        for (int i = 0; i < kBulletSlots; ++i) {
            Round &b = r_[size_t(i)];
            if (!b.live) continue;
            // ONE FRAME AT THE MUZZLE. See Round::born.
            if (b.born) {
                b.born = false;
                continue;
            }
            b.age += dt;
            if (b.age >= kBulletLifeS) {
                b.live = false;
                continue;
            }
            const float dist = sqrtf(maxf(1e-8f, lengthSq(b.vel))) * dt;
            const int steps = maxi(1, int(dist / kBulletStepM + 0.5f));
            const float h = dt / float(steps);
            const Vec3 dir = b.vel * (1.0f / sqrtf(maxf(1e-8f, lengthSq(b.vel))));
            for (int s = 0; s < steps && b.live; ++s) {
                b.vel.y -= kBulletDrop * h;
                const Vec3 next = b.pos + b.vel * h;
                const float travelled = float(b.age - dt) * kBulletSpeed + float(s) * kBulletStepM;
                if (travelled > kBulletArmM) {
                    if (hitLife && hitLife(next)) {
                        b.live = false;
                        impacts_.push_back({next, dir});
                        break;
                    }
                    // insideWorld is the SAME test the arrow and the walk
                    // ask -- see player.h. And the impact is recorded at
                    // `next`, the first point INSIDE the material, not at the
                    // last one outside it: a carve wants the voxel actually
                    // struck, which is Arrows' own note on the line this comes
                    // from.
                    if (insideWorld(w, next)) {
                        b.live = false;
                        impacts_.push_back({next, dir});
                        break;
                    }
                }
                b.pos = next;
            }
        }
    }

    // One flyer slot per round, exactly as the sparks are drawn -- see
    // Particles::publish, whose note on the scale is the one that matters: a
    // flyer's object space is ALREADY metres, so the transform is a turn and a
    // translation and nothing else. A bullet does not turn, so it is the
    // identity and a translation.
    void publish(World &world) const {
        for (int i = 0; i < kBulletSlots; ++i) {
            const Round &b = r_[size_t(i)];
            if (!b.live || model_ < 0) {
                world.setFlyerInstance(kBulletSlot0 + i, -1, nullptr, 0.0f, 0.0f, 0.0f, nullptr,
                                       false);
                continue;
            }
            const float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
            const float h = 0.5f * VOXEL_M;
            // -- THE LAST ARGUMENT IS `show` AND IT WAS FALSE ---------------
            //
            // Every round was launched, marched, tested and reported an impact
            // exactly as it should -- and was then published INVISIBLE. See
            // World::setFlyerInstance: `ok = show && m && model >= 0`, so a
            // false there is a slot that carries a model and draws nothing.
            //
            // It reads like the `false` two lines up in the dead branch, which
            // is what made it easy to write and impossible to see: that one is
            // correct, because a slot with no round in it must NOT draw. This
            // one is the live path. The user's report was "I dont see the spark
            // voxel coming out of the gun", and the log said the rounds were
            // there -- both were true at once.
            world.setFlyerInstance(kBulletSlot0 + i, model_, m, b.pos.x - h, b.pos.y - h,
                                   b.pos.z - h, nullptr, true);
        }
    }

    const std::vector<Impact> &impactsThisTick() const { return impacts_; }

    // -- AND THE LIST HAS TO BE EMPTIED BY SOMEONE WHEN NOTHING IS FLYING ----
    //
    // `impacts_` is cleared at the top of update(), and App only calls update()
    // while inFlight() > 0. The round that has just landed is the round that
    // takes inFlight() to zero -- so from the frame after an impact, update()
    // stops being called, the list is never cleared, and App's drain loop
    // replays the SAME impact for ever. Measured at 18 replays of one shot
    // before the capture, each one a fresh 90 m collider gather and a fresh
    // carve attempt at a hole that is already there.
    //
    // It cannot be fixed by calling update() unconditionally: the gather is
    // what the gate exists to avoid. So the caller clears it instead.
    void clearImpacts() { impacts_.clear(); }
    // Where a round in flight is right now. For the light publish (a tracer is
    // an ember and lights the air around it -- see App::publishSparkLights)
    // and for the headless tests.
    bool at(int i, Vec3 *out) const {
        if (i < 0 || i >= kBulletSlots || !r_[size_t(i)].live) return false;
        if (out) *out = r_[size_t(i)].pos;
        return true;
    }

    int inFlight() const {
        int n = 0;
        for (int i = 0; i < kBulletSlots; ++i)
            if (r_[size_t(i)].live) ++n;
        return n;
    }

  private:
    // -- HOW FAR A ROUND FLIES BEFORE IT IS ALLOWED TO HIT ANYTHING -------
    //
    // (user 2026-09-17: "the chunking mechanic from the bullet doesnt work
    //  point blank. fix this.")
    //
    // IT WAS A METRE AND A HALF, AND THAT IS THE WHOLE BUG. A round is born at
    // the MUZZLE (see HeldItem::muzzle), which is already out past the body --
    // so the metre and a half was not clearing the player, it was deleting the
    // first fifteen substeps of every shot. Anything closer than that could
    // not be hit at all: the round flew through the wall and armed on the far
    // side of it.
    //
    // ONE SUBSTEP IS WHAT IS ACTUALLY NEEDED. The arming exists so a round
    // cannot register against whatever the muzzle is momentarily inside of on
    // the frame it is created -- the held model's own grid, or a wall the
    // player is leaning on as the pose swings. A single kBulletStepM covers
    // that and nothing more.
    //
    // AND PRESSING THE MUZZLE INTO A WALL AND FIRING SHOULD CUT THE WALL.
    // That is the case being asked for, and it is also the correct one: there
    // is no reading of a gun in which the round goes somewhere else.
    static constexpr float kBulletArmM = kBulletStepM;

    int model_ = -1;
    Round r_[kBulletSlots];
    std::vector<Impact> impacts_;
};

}  // namespace v2
