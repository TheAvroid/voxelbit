// ---------------------------------------------------------------------------
// drops.h -- what leaves your hand when you press Q.
//
// Ported from the JS engine's dropHeld (src/sim/hands.js) and the drop life
// that follows it: the same toss profile, the same eight slots, the same hover
// and spin, and the same walk-over pickup (its autoPickup, in ui/audio.js).
//
// IT IS THE ARROW'S MACHINERY WITH A DIFFERENT VERB. render/arrows.h already
// carries a fixed pool of instances, a ballistic integration against streamed
// terrain and a landing test, and this is the same three things -- so the
// numbers below are the only part that is new. Where they overlap they agree:
// TOSS_G is the gravity a shaft already falls under.
//
// ---------------------------------------------------------------------------
// IT LEAVES FROM THE HAND, NOT FROM THE EYE
//
// The JS engine's note on the line is "launch from the held item's true world
// spot ... it FLIES out of the hand", and that is the whole difference between
// a drop and a spawn. v2 knows exactly where the hand is -- HeldXform::cam is
// the pose after the swing, the bob and the sway -- so the toss starts at the
// item's own world centre and the object you were holding is the object that
// flies. Nothing appears and nothing vanishes.
//
// ---------------------------------------------------------------------------
// AND IT IS THE SAME SIZE ON THE GROUND AS IT WAS IN THE HAND
//
// Which is true now and was not before: a held voxel is a world voxel (see the
// note over HeldPose), so a dropped axe is the 90 cm axe you were carrying
// rather than a 10 cm model of one. The instance transform is a free turn about
// Y times VOXEL_M -- orthonormal times a uniform scale, which is exactly what
// KIND_FLYER exists to normalise, so a drop is shaded through the same path a
// butterfly is.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"
#include "../gpu/world.h"
#include "../scene/collide.h"
#include "../scene/voxelworld.h"
#include "player.h"

#include <cmath>
#include <string>
#include <vector>

namespace v2 {

// -- the toss, from hands.js ------------------------------------------------
// That engine counts in voxels a second; these are metres, which is the same
// numbers over ten. TOSS_G is the gravity render/arrows.h already falls under
// and is named there as the same constant.
inline constexpr float kTossSpeed = 5.5f;  // TOSS_V 55 -- along the view
inline constexpr float kTossUp = 1.8f;     // TOSS_UP 18 -- the up-kick over it
inline constexpr float kTossG = -17.0f;    // TOSS_G -170

// EIGHT, and that is the user's own number over there: "have the max number of
// floating hand held items on the field 8 instead of 4" (2026-08-20). The
// oldest is retired when a ninth is thrown, which is what that engine's
// drops.shift() does.
inline constexpr int kDropSlots = 8;

// How long a landed item hovers before it settles, and how long the settling
// takes -- DROP_REST_MS and DROP_REST_EASE.
inline constexpr float kDropRestSec = 30.0f;
inline constexpr float kDropEaseSec = 1.0f;
// It turns while it hovers. Slow enough to read as an object on display rather
// than a pickup spinning in an arcade.
inline constexpr float kDropSpin = 1.2f;   // radians a second
inline constexpr float kDropBobM = 0.06f;  // and rides up and down this far
inline constexpr float kDropBobHz = 0.7f;

// AUTO_PICK_R 16, in metres. Walk this close with a free hand and it comes back
// to you -- the same rule that engine has, and the reason a drop is not a way
// to lose your axe permanently.
inline constexpr float kPickupM = 1.6f;
// ...but not the instant it leaves your hand. A throw that could be walked into
// on the frame it was thrown would be a Q that does nothing.
inline constexpr float kPickupArmSec = 0.6f;

// ---------------------------------------------------------------------------
class Drops {
  public:
    struct Item {
        bool live = false;
        int tool = -1;    // which entry of the kit this was
        int model = -1;   // ...and the model that draws it
        int sx = 0, sy = 0, sz = 0;
        Vec3 pos{0, 0, 0};
        Vec3 vel{0, 0, 0};
        float spin = 0.0f;   // its turn about Y
        float age = 0.0f;
        bool flying = true;
        float rest = 0.0f;   // seconds since it landed
        float restY = 0.0f;  // ...and where it landed
    };

    // -----------------------------------------------------------------------
    // Throw one. `from` is the held item's own world centre, `dir` the view.
    //
    // The oldest slot is taken when all eight are busy, which is the pool
    // behaving as a pool rather than refusing the drop -- and is that engine's
    // drops.shift() by another name.
    // -----------------------------------------------------------------------
    void toss(int tool, int model, int sx, int sy, int sz, const Vec3 &from, const Vec3 &dir) {
        int slot = -1;
        float oldest = -1.0f;
        for (int i = 0; i < kDropSlots; ++i) {
            if (!items_[size_t(i)].live) {
                slot = i;
                break;
            }
            if (items_[size_t(i)].age > oldest) {
                oldest = items_[size_t(i)].age;
                slot = i;
            }
        }
        if (slot < 0) return;

        Item &d = items_[size_t(slot)];
        d = Item{};
        d.live = true;
        d.tool = tool;
        d.model = model;
        d.sx = sx;
        d.sy = sy;
        d.sz = sz;
        d.pos = from;
        d.vel = dir * kTossSpeed;
        d.vel.y += kTossUp;
        d.spin = atan2f(dir.x, dir.z);  // it leaves facing the way you were
        d.flying = true;
    }

    // -----------------------------------------------------------------------
    // One tick. Returns the tool index of anything picked up this frame, or -1.
    //
    // The flight is integrated at the arrows' own 5 ms for the arrows' own
    // reason: this world streams, so an arc solved up front would be deciding
    // where a thrown axe lands against terrain nobody has generated. The JS
    // engine marches the whole arc at the throw because it can -- its world is
    // one resident array.
    // -----------------------------------------------------------------------
    int update(float dt, const WalkWorld &w, const Vec3 &player, bool handFree) {
        int got = -1;
        const float h = minf(dt, 0.25f);
        for (Item &d : items_) {
            if (!d.live) continue;
            d.age += h;

            if (d.flying) {
                float left = h;
                while (left > 0.0f && d.flying) {
                    const float s = minf(left, 0.005f);
                    left -= s;
                    const float nvy = d.vel.y + kTossG * s;
                    const Vec3 step(d.vel.x * s, (d.vel.y + nvy) * 0.5f * s, d.vel.z * s);
                    const Vec3 next = d.pos + step;
                    d.vel.y = nvy;
                    // The same predicate the arrow lands on, and the player
                    // walks into -- see insideWorld in render/player.h.
                    if (insideWorld(w, next)) {
                        d.flying = false;
                        d.rest = 0.0f;
                        d.restY = d.pos.y;
                        break;
                    }
                    d.pos = next;
                    if (d.pos.y < -50.0f || d.age > 8.0f) {
                        d = Item{};
                        break;
                    }
                }
            } else {
                d.rest += h;
                // HOVERS, THEN SETTLES. That engine's DROP_REST_MS: thirty
                // seconds of turning slowly a hand's breadth off the ground,
                // then an ease down to a dead stop so a wood left alone does
                // not end up full of things bobbing for ever.
                d.spin += kDropSpin * h * settle(d);
            }
            if (!d.live) continue;

            // -- and walking over it takes it back ---------------------------
            if (handFree && d.age > kPickupArmSec && got < 0) {
                const Vec3 o = d.pos - player;
                if (lengthSq(o) < kPickupM * kPickupM) {
                    got = d.tool;
                    d = Item{};
                }
            }
        }
        return got;
    }

    // -----------------------------------------------------------------------
    // Every slot onto the pipeline, empty ones included -- the band is a fixed
    // size and a slot just vacated has to be told it is empty.
    // -----------------------------------------------------------------------
    void publish(World &world) const {
        for (int i = 0; i < kDropSlots; ++i) {
            const Item &d = items_[size_t(i)];
            if (!d.live || d.model < 0) {
                world.setDropInstance(i, 0, nullptr, 0.0f, 0.0f, 0.0f, false);
                continue;
            }
            const float c = cosf(d.spin), s = sinf(d.spin);
            const float v = VOXEL_M;
            const float m[9] = {c * v, 0.0f, s * v, 0.0f, v, 0.0f, -s * v, 0.0f, c * v};
            // A hover while it is resting, easing away as it settles. The mesh
            // runs from its own corner, so the translation is the centre less
            // the model's half-box carried through the turn.
            const float lift =
                d.flying ? 0.0f
                         : (kDropBobM * (0.5f + 0.5f * sinf(d.rest * kDropBobHz * TWO_PI)) *
                            settle(d));
            const float cx = 0.5f * float(d.sx) * v, cy = 0.5f * float(d.sy) * v,
                        cz = 0.5f * float(d.sz) * v;
            const float ox = m[0] * cx + m[1] * cy + m[2] * cz;
            const float oy = m[3] * cx + m[4] * cy + m[5] * cz;
            const float oz = m[6] * cx + m[7] * cy + m[8] * cz;
            world.setDropInstance(i, d.model, m, d.pos.x - ox, d.pos.y + lift - oy, d.pos.z - oz,
                                  true);
        }
        world.flushDropInstances();
    }

    int count() const {
        int n = 0;
        for (const Item &d : items_)
            if (d.live) ++n;
        return n;
    }

  private:
    // 1 while it hovers, easing to 0 over the last second of its rest -- so the
    // turn and the bob stop together rather than one of them stopping first.
    static float settle(const Item &d) {
        if (d.flying) return 1.0f;
        const float over = d.rest - kDropRestSec;
        if (over <= 0.0f) return 1.0f;
        return maxf(0.0f, 1.0f - over / kDropEaseSec);
    }

    std::vector<Item> items_ = std::vector<Item>(size_t(kDropSlots));
};

}  // namespace v2
