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
inline constexpr float kDropSpin = 1.2f;  // radians a second

// HOW HIGH IT FLOATS, and this is the number that was missing. The JS engine's
// dropAnchor holds a dropped item 9.0 VOXELS above the ground with a 1.3-voxel
// bob under it, then eases to the item's own resting extent when the hover time
// is up. Without it the toss simply stopped where the arc met the world -- and
// since `pos` is the item's CENTRE, "met the world" put half an axe underground.
inline constexpr float kDropHoverM = 0.9f;  // dropAnchor's 9.0
inline constexpr float kDropBobM = 0.13f;   // ...and its 1.3
inline constexpr float kDropBobHz = 0.32f;  // sin(t * 0.002 ms) = 0.32 Hz

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
        float phase = 0.0f;  // ...and where its bob starts, so eight do not pulse as one
        float age = 0.0f;
        bool flying = true;
        float rest = 0.0f;     // seconds since it landed
        float groundY = 0.0f;  // the terrain under where it landed
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
        d.phase = float(slot) * 0.79f;  // an arbitrary spread, not a random one
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
    // `handFree` is gone deliberately -- see the pickup below.
    int update(float dt, const WalkWorld &w, const Vec3 &player) {
        int got = -1;
        const float h = minf(dt, 0.25f);
        for (Item &d : items_) {
            if (!d.live) continue;
            d.age += h;

            if (d.flying) {
                float left = h;
                TerrainMemo tm;
                while (left > 0.0f && d.flying) {
                    const float s = minf(left, 0.005f);
                    left -= s;
                    const float nvy = d.vel.y + kTossG * s;
                    const Vec3 step(d.vel.x * s, (d.vel.y + nvy) * 0.5f * s, d.vel.z * s);
                    const Vec3 next = d.pos + step;
                    d.vel.y = nvy;

                    // -- THE ARC ENDS AT THE HOVER LINE, NOT AT THE GROUND ---
                    //
                    // This is the JS engine's own terminator, verbatim in
                    // shape: `T > 0.12 && py <= hmap[...] + 9.0`. It stops the
                    // flight where the hover BEGINS, which is what makes the
                    // landing invisible -- the last point of the arc and the
                    // first point of the hover are the same point.
                    //
                    // The first cut of this stopped when the item's CENTRE
                    // entered the world instead, and both halves of that were
                    // wrong to watch: a 90 cm axe was half underground before
                    // anything registered as a landing, and then it snapped up
                    // to the hover. Clipped, then teleported, which is exactly
                    // what it looked like.
                    //
                    // THE 0.12 s IS THAT ENGINE'S TOO, and it earns its place:
                    // the item leaves the hand at chest height, and on ground
                    // that rises in front of you that is already below the
                    // hover line -- so without it a throw uphill lands on the
                    // frame it was thrown.
                    const float g =
                        w.terrain ? w.terrain->heightM(next.x, next.z, tm) : next.y;
                    if (d.age > 0.12f && next.y <= g + kDropHoverM) {
                        d.pos.x = next.x;
                        d.pos.z = next.z;
                        d.groundY = g;
                        d.pos.y = g + kDropHoverM;  // where the hover starts
                        d.flying = false;
                        d.rest = 0.0f;
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
                // HOVERS, THEN SETTLES -- dropAnchor, line for line. Thirty
                // seconds floating and turning, then an ease down to a dead
                // stop, so a wood left alone does not end up full of things
                // bobbing for ever.
                //
                // THE POSITION IS COMPUTED, NOT INTEGRATED, once it has landed.
                // That is what keeps it off the ground rather than in it, and
                // it also means the pickup test below measures against where
                // the item actually is.
                const float e = 1.0f - settle(d);  // 0 while hovering, 1 once settled
                const float half = 0.5f * float(d.sy) * VOXEL_M;
                // THE BOB RAMPS IN over its first second. It is a sine with a
                // per-item phase, so at the instant of landing it is somewhere
                // between plus and minus 13 cm -- and applied cold that is a
                // step in the one place this code exists to keep smooth. The
                // ramp costs nothing and the phases stay spread, so eight
                // dropped tools still do not pulse as one.
                const float bob = sinf(d.rest * kDropBobHz * TWO_PI + d.phase) * kDropBobM *
                                  minf(1.0f, d.rest);
                d.pos.y = d.groundY + kDropHoverM + (half - kDropHoverM) * e + bob * (1.0f - e);
                d.spin += kDropSpin * h * settle(d);
            }
            if (!d.live) continue;

            // -- AND WALKING OVER IT TAKES IT BACK, HANDS FULL OR NOT --------
            //
            // This asked for an empty hand at first, and that is not what the
            // JS engine does: its autoPickup asks whether there is anywhere for
            // the drop TO GO -- a free slot, or a stack with room -- not whether
            // you happen to be holding something. A tool's slot in this kit
            // always exists, so there is always somewhere for it to go, and
            // demanding an empty hand made a dropped pick unrecoverable while
            // an axe was in the hand.
            //
            // HeldItem::give only CHANGES what is in the hand when the hand is
            // empty, so walking over a pick while swinging an axe puts the pick
            // back in the kit without swapping the axe out.
            if (d.age > kPickupArmSec && got < 0) {
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
            // The mesh runs from its own corner, so the translation is the
            // centre less the model's half-box carried through the turn. The
            // hover is already in `pos` -- see the settle in update().
            // THE CENTRE IS IN MODEL VOXELS, NOT METRES, and that is the whole
            // of the wide circular sweep this used to make. `m` already
            // carries VOXEL_M as its scale, so multiplying the half-box by it
            // here applied the scale TWICE: the offset came out ten times too
            // long and the item orbited a point a metre away instead of
            // turning on the spot. The matrix takes model units in and gives
            // metres out -- that is what it is for.
            const float cx = 0.5f * float(d.sx), cy = 0.5f * float(d.sy),
                        cz = 0.5f * float(d.sz);
            const float ox = m[0] * cx + m[1] * cy + m[2] * cz;
            const float oy = m[3] * cx + m[4] * cy + m[5] * cz;
            const float oz = m[6] * cx + m[7] * cy + m[8] * cz;
            // How far it travelled to get here is not passed and never was
            // computed here: World::place differences the transform. See the
            // note over it -- a tossed tool arcs, and then it hovers and turns,
            // and this used to hand the tracer nothing at all.
            world.setDropInstance(i, d.model, m, d.pos.x - ox, d.pos.y - oy, d.pos.z - oz, true);
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
