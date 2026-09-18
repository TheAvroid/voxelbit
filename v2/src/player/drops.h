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

#include <functional>   // Drops::FloorF
#include "core/vecmath.h"
#include "world/world.h"
#include "player/collide.h"
#include "world/voxelworld.h"
#include "player/player.h"

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

// -- ...AND THE SPILL, WHICH IS NOT A THROW --------------------------------
//
// What comes out of a broken plant has not been thrown by anybody: it should
// hop out of where the plant was and land beside it, not sail five metres down
// your eyeline. Same integration, same landing, a tenth of the speed and most
// of that upward -- so two items leaving the same tuft separate enough to read
// as two things and stay within reach of the swing that freed them.
inline constexpr float kSpillSpeed = 0.9f;
inline constexpr float kSpillUp = 2.2f;

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

// AND IT NEVER COMES DOWN THE WHOLE WAY (user 2026-09-07): three voxels of air
// under a dropped item, always, whatever else the hover is doing.
//
// It is a floor under the UNDERSIDE, not a height for the centre, and the
// difference is the whole point. `pos` is the centre, so a rule written as a
// centre height means a tall item floats less than a short one -- and the two
// places this bites were both real. The settle below eased to `half`, which
// puts the underside exactly ON the ground, so every drop lay down flat after
// thirty seconds. And the hover itself is a centre height of 9 voxels, so an
// item 18 voxels tall hovered with its base already at ground level and the
// 1.3-voxel bob then took it under.
//
// Written as a gap, both go away at once and the item's size stops mattering.
inline constexpr float kDropFloorM = 0.3f;  // three 10 cm voxels
inline constexpr float kDropBobHz = 0.32f;  // sin(t * 0.002 ms) = 0.32 Hz

// AUTO_PICK_R 16, in metres. Walk this close with a free hand and it comes back
// to you -- the same rule that engine has, and the reason a drop is not a way
// to lose your axe permanently.
inline constexpr float kPickupM = 1.6f;
// ...but not the instant it leaves your hand. A throw that could be walked into
// on the frame it was thrown would be a Q that does nothing.
inline constexpr float kPickupArmSec = 0.6f;

// -- AND IT FLIES TO YOU RATHER THAN VANISHING (user 2026-09-07) ------------
//
// "have the object float towards the player like the player is absorbing it."
// Walking into range used to delete the drop and grant the tool on the same
// frame, so a pickup was a thing that had already happened by the time you
// could see it. The JS engine never did that: autoPickup hands the item to
// startGrab, which flies it in over GRAB_MS, and a chunk knocked off a rock
// takes the same trip on sim/solver.js's absorb curve.
//
// This is that curve, and the numbers are all its:
//
//   * GRAB_MS 360, the flight for a DROPPED item. (A chunk's own absorbFly is
//     672 -- longer, because it sets off from wherever it was knocked to.)
//   * smoothstep on the way, "leaves the ground gently, arrives fast".
//   * absorbY -12 voxels, so it converges BELOW the eye -- at the chest, not
//     in your face. The engine over there retuned this twice and landed here.
//   * a 3-voxel arc, sin(e * pi), "slight arc so it lifts rather than slides".
//   * full size the whole way. That engine's note is worth keeping: it used to
//     shrink to a tenth on the way in, "which read as the chunk evaporating
//     rather than being picked up".
inline constexpr float kGrabSec = 0.36f;      // GRAB_MS
inline constexpr float kAbsorbEyeM = -1.2f;   // absorbY, -12 voxels off the eye
inline constexpr float kAbsorbArcM = 0.3f;    // ...and its 3-voxel lift
inline constexpr float kAbsorbSpin = 6.0f;    // omega, rad/s -- it tumbles on the way in

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
        // -- ...OR THE SURFACE IT WAS TOLD TO LAND ON INSTEAD ---------------
        //
        // (user 2026-09-15: "when killing a fish have the raw steak float above
        // the water", then again: "have the steak appear above the water
        // floating, not in it".)
        //
        // THE FIRST FIX MOVED THE WRONG END OF THE FLIGHT. dropMeatAt lifted
        // the START of the arc to the waterline, and the arc then flew on and
        // terminated where it always had -- at `terrain->heightM`, which under
        // a lake is the SEABED. So the steak was released at the surface and
        // fell straight through it to the bottom, which is the same place it
        // ended up before and for the same reason: the hover is measured from
        // the ground, and nothing in this file had ever heard of water.
        //
        // -1e9 means "ask the terrain", which is every other drop in the game.
        float floorY = -1e9f;
        // -- being absorbed --------------------------------------------------
        // `from` is captured ONCE, at the instant the flight is armed, and the
        // curve is evaluated against it rather than integrated. That is the
        // same choice the hover makes for the same reason: a computed position
        // cannot drift, and the arrival lands exactly on the target however
        // ragged the frame times were on the way.
        bool taken = false;
        float fly = 0.0f;
        Vec3 from{0, 0, 0};
        // How far it turned since the last frame -- what the motion vector
        // needs, which is not the same as how fast it is turning.
        float dspin = 0.0f;
    };

    // -----------------------------------------------------------------------
    // Throw one. `from` is the held item's own world centre, `dir` the view.
    //
    // The oldest slot is taken when all eight are busy, which is the pool
    // behaving as a pool rather than refusing the drop -- and is that engine's
    // drops.shift() by another name.
    // -----------------------------------------------------------------------
    // Returns the slot it went into, or -1 -- see spill(), which relaunches it.
    int toss(int tool, int model, int sx, int sy, int sz, const Vec3 &from, const Vec3 &dir) {
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
        if (slot < 0) return -1;

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
        return slot;
    }

    // -----------------------------------------------------------------------
    // ...AND ONE THAT FALLS OUT OF SOMETHING RATHER THAN OUT OF YOUR HAND.
    //
    // `at` is where the thing that broke was standing and `spread` is a bearing
    // -- the caller gives each item of a set a different one, so a plant that
    // pays out two does not stack them in the same spot. Everything after the
    // launch is toss()'s: the same arc, the same landing, the same hover, the
    // same walk-over pickup.
    // -----------------------------------------------------------------------
    // `floorY` is what the item comes to rest ON, for the one caller that knows
    // better than the terrain does -- a steak dropped over a lake. Left alone
    // it is the ground, which is every other spill in the game. See
    // Item::floorY for what went wrong when only the launch height was moved.
    void spill(int tool, int model, int sx, int sy, int sz, const Vec3 &at, float bearing,
               float floorY = -1e9f) {
        const Vec3 out(sinf(bearing), 0.0f, cosf(bearing));
        const int slot = toss(tool, model, sx, sy, sz, at, out);
        if (slot < 0) return;
        // toss() left along the view at a throwing speed; this is the same
        // launch at a spill's. Rewriting the velocity of the slot it USED --
        // rather than hunting for the youngest item, which is two items on any
        // frame a plant pays out twice -- keeps one arc in this file.
        Item &d = items_[size_t(slot)];
        d.vel = out * kSpillSpeed;
        d.vel.y = kSpillUp;
        d.floorY = floorY;   // toss() reset it; set it after, not before
    }

    // -----------------------------------------------------------------------
    // STRAIGHT INTO THE FLIGHT, WITH NO ARC AND NO LANDING.
    //
    // (user 2026-09-17: "when picking up a orange/apple, have the item smoothly
    //  go to the players hand position".)
    //
    // A PICKED FRUIT WAS TELEPORTING. pickFruit called give() and select(), so
    // the apple stopped being on the branch and started being in the hand
    // between one frame and the next, with nothing in between.
    //
    // THE FLIGHT ALREADY EXISTS and is the one every other pickup uses: walk
    // over a dropped axe and it lifts off the ground and converges on your
    // chest over kGrabSec on the absorb curve. All a picked fruit needs is to
    // start in that state instead of arriving at it -- so this is spill()
    // without the arc: the item is created AT the branch and armed as taken on
    // the same frame.
    //
    // IT PAYS OUT THROUGH THE SAME DOOR. arrivedThisTick reports it when it
    // lands, the caller gives the kit slot there, and the pickup sound plays
    // with it -- so there is one place where a thing becomes yours rather than
    // two that can disagree.
    // -----------------------------------------------------------------------
    bool grabFrom(int tool, int model, int sx, int sy, int sz, const Vec3 &at) {
        const int slot = toss(tool, model, sx, sy, sz, at, Vec3(0.0f, 1.0f, 0.0f));
        if (slot < 0) return false;
        Item &d = items_[size_t(slot)];
        d.vel = Vec3(0.0f, 0.0f, 0.0f);
        d.flying = false;
        d.taken = true;
        d.fly = 0.0f;
        d.from = at;
        d.pos = at;
        return true;
    }

    // ...and where the newest live drop is, for --kill-test. The one thing a
    // test cannot see from outside: whether the steak came to rest ON the lake
    // or at the bottom of it. See Item::floorY.
    bool newestDrop(Vec3 *at, bool *flying) const {
        int best = -1;
        float young = 1e9f;
        for (int i = 0; i < kDropSlots; ++i) {
            const Item &d = items_[size_t(i)];
            if (!d.live || d.age > young) continue;
            young = d.age;
            best = i;
        }
        if (best < 0) return false;
        *at = items_[size_t(best)].pos;
        *flying = items_[size_t(best)].flying;
        return true;
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
    // True on the frame an item left the ground for the player, which is NOT
    // the frame it arrives -- see ToolSounds::pickedUp for why the difference
    // matters. Cleared at the top of every update.
    bool snatchedNow() const { return snatched_; }

    // -----------------------------------------------------------------------
    // ...AND WHAT ARRIVED THIS TICK, WHICH IS A LIST AND NOT A NUMBER.
    //
    // Returns how many, and arrivedThisTick() is what they were. It used to
    // return a single tool index because only one item could ever be in flight
    // -- see the gate below -- so "the one that landed" was a complete answer.
    // With several converging it is not, and the failure mode of keeping the
    // int would have been silent: the second and third items of a pile would
    // vanish out of the world without ever reaching the kit.
    //
    // CLEARED AT THE TOP, the same idiom snatchedNow uses for the other end of
    // the flight.
    // -----------------------------------------------------------------------
    // WHAT IS UNDER A FALLING ITEM, when the terrain is not the answer.
    //
    // (user 2026-09-17: "when pressing q on the nuketown map, the item goes
    //  right through the map".)
    //
    // A DROP IS AN ARC AND A GROUND TEST, not a physics body -- which is why
    // it is cheap and why it fell through. The test asked
    // `w.terrain->heightM`, and in the level the terrain is the WOOD's, six
    // hundred metres below the floor you are standing on. Debris does not have
    // this problem because a debris body is a real actor and buildWindow puts
    // the level in its collider; a drop has no collider at all.
    //
    // A FUNCTION RATHER THAN A SECOND WORLD POINTER, because the level is not
    // a VoxelTerrain and never will be -- it is a voxel grid in its own sky.
    // The caller knows which place it is in; this only needs a floor. Null is
    // the ordinary case and means "ask the terrain", which is what every wood
    // caller wants.
    using FloorF = std::function<float(float, float, float)>;

    int update(float dt, const WalkWorld &w, const Vec3 &player, const Vec3 &eye,
               const FloorF &floorAt = nullptr) {
        arrived_.clear();
        snatched_ = false;
        const float h = minf(dt, 0.25f);
        for (Item &d : items_) {
            if (!d.live) continue;
            d.age += h;

            // -- ...AND ONCE IT IS COMING TO YOU, NOTHING ELSE MOVES IT ------
            //
            // Ahead of the toss and the hover both, because an item on this
            // curve is no longer subject to either -- the JS solver says the
            // same thing in one line at the top of its integrator, "an
            // absorbing chunk is driven by its flight curve, not by physics".
            // The target is read LIVE off the eye so the item follows a player
            // who keeps walking, which is that engine's "tracked live" note.
            if (d.taken) {
                d.fly += h;
                const float k = minf(1.0f, d.fly / kGrabSec);
                const float e = k * k * (3.0f - 2.0f * k);
                const Vec3 to(eye.x, eye.y + kAbsorbEyeM, eye.z);
                d.pos = d.from + (to - d.from) * e;
                d.pos.y += sinf(e * PI) * kAbsorbArcM;
                const float wasFly = d.spin;
                d.spin += kAbsorbSpin * h;
                d.dspin = d.spin - wasFly;
                if (k >= 1.0f) {
                    arrived_.push_back(d.tool);
                    d = Item{};
                }
                continue;
            }

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
                    // THE FLOOR THIS ITEM WAS GIVEN, if it was given one --
                    // see Item::floorY. A steak dropped over a lake lands on
                    // the LAKE; everything else asks the terrain, as it always
                    // has. MAX of the two rather than a replacement, so a floor
                    // handed to a drop that then drifts over the bank still
                    // comes to rest on the bank rather than inside it.
                    // THE LEVEL'S FLOOR WHEN THERE IS ONE -- see FloorF. It
                    // takes the item's own y because a map has floors above
                    // floors and the one that matters is the one under THIS
                    // item, not the top of the column.
                    const float gt = floorAt ? floorAt(next.x, next.y, next.z)
                                    : w.terrain ? w.terrain->heightM(next.x, next.z, tm)
                                                : next.y;
                    const float g = d.floorY > -1e8f ? maxf(gt, d.floorY) : gt;
                    // The hover LINE is still what stops the arc -- see the
                    // note above -- but where it comes to rest is subject to
                    // the same floor as everything below, so a big item does
                    // not spend its first frame lower than it will ever be
                    // again.
                    if (d.age > 0.12f && next.y <= g + kDropHoverM) {
                        d.pos.x = next.x;
                        d.pos.z = next.z;
                        d.groundY = g;
                        d.pos.y = maxf(g + kDropHoverM,
                                       g + 0.5f * float(d.sy) * VOXEL_M + kDropFloorM);
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
                // The ease still runs from the hover to the resting extent; the
                // resting extent is just three voxels higher than the ground
                // now. The clamp is what makes it a MINIMUM rather than merely
                // an endpoint -- it catches the trough of the bob, and it
                // catches an item tall enough that its hover was already lower
                // than its floor, which then simply hangs at the floor.
                const float low = d.groundY + half + kDropFloorM;
                const float air = d.groundY + kDropHoverM;
                d.pos.y = maxf(low, air + (low - air) * e + bob * (1.0f - e));
                const float was = d.spin;
                d.spin += kDropSpin * h * settle(d);
                d.dspin = d.spin - was;
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
            // -- AS MANY AT ONCE AS ARE IN REACH (user 2026-09-14: "have it
            //    where the player can absorb multiple object at once, instead
            //    of one at a time in a line") ------------------------------
            //
            // THIS WAS `got < 0 && !anyTaken()`, and both halves were v1's:
            // "one flight at a time -- startGrab would clobber the item already
            // in the air". Over there that is a real constraint, because
            // grabAnim is ONE record -- a second grab would overwrite the
            // first. Here every drop carries its own `taken`, `fly` and `from`,
            // so nothing is shared and nothing can be clobbered; the rule came
            // across with the code rather than with a reason.
            //
            // v1'S OWN NOTE PREDICTED THIS COMPLAINT AND NAMED THE FIX: "only
            // ONE grab may be in the air at a time ... so a PILE of items
            // drains at one per flight and the wait compounds. If that ever
            // becomes the complaint, the fix is to allow concurrent grabs
            // rather than to shorten the flight again."
            //
            // `got < 0` HAD TO GO WITH IT AND WAS THE HARDER HALF: update
            // returned ONE tool index, so even with concurrent flights only one
            // arrival per frame could be reported and the rest would be
            // absorbed into nothing. See arrivedThisTick.
            if (d.age > kPickupArmSec) {
                // -- THE REACH IS THE WHOLE BODY, NOT THE SOLES OF THE FEET --
                //
                // (user 2026-09-18: "I cant pick up steak when its floating in
                // the water and im swimming.")
                //
                // AND IT WAS ARITHMETIC, NOT A GATE. `player` is the feet, and
                // measuring a 1.6 m sphere from there is fine for a drop lying
                // on the ground beside them. A swimmer is a body hanging from
                // the surface, and the one thing that is NOT near the surface
                // is their feet:
                //
                //   the steak floats at   surface + kDropHoverM  = +0.90 m
                //   a treading swimmer's eye rides at
                //                         surface + swimRise +- swimBob
                //   ...so the feet sit at surface - 0.90 -+ 0.65
                //
                // which puts 1.15 m to 2.45 m between the feet and the meat
                // depending on where in the bob you happen to be. Against
                // kPickupM = 1.6 that is reachable only near the top of the
                // cycle, and only if you are within 1.11 m horizontally --
                // at the bottom of the bob it is out of reach at any distance.
                // It reads as "you cannot pick it up", because mostly you
                // cannot, and the times you can look like luck.
                //
                // A CAPSULE, WHICH IS WHAT A BODY IS. The nearest point on the
                // segment from the feet to the eye, so an item beside ANY part
                // of you is in reach. On land this can only help -- the feet
                // are still one end of it -- and it is the more honest model
                // anyway: a drop hovering at 0.9 m used to be measured from the
                // soles, costing 0.9 m of the 1.6 m budget before any
                // horizontal distance was counted at all.
                //
                // TO THE EYE AND NOT TO THE CHEST, even though the chest is
                // where the item flies to. Reaching for a thing and pulling it
                // to your chest are different distances; a swimmer's chest bobs
                // to 0.75 m under the surface, which still leaves 1.65 m to a
                // steak floating on it -- the same bug, one decimetre smaller.
                const float loY = minf(player.y, eye.y), hiY = maxf(player.y, eye.y);
                const Vec3 o = d.pos - Vec3(player.x, minf(hiY, maxf(loY, d.pos.y)), player.z);
                if (lengthSq(o) < kPickupM * kPickupM) {
                    d.taken = true;
                    d.fly = 0.0f;
                    d.from = d.pos;
                    // IT IS LEVITATING BY CONSTRUCTION. That engine gates the
                    // sound on `lev` because it can also grab a rock out of a
                    // wall or a worm out of the dirt, and neither of those is
                    // an item hovering off the ground. Everything in THIS list
                    // hovers -- see kDropFloorM -- so the gate is the list.
                    snatched_ = true;
                }
            }
        }
        return int(arrived_.size());
    }

    // The tools that reached the chest this tick, in the order they landed.
    // Empty on almost every frame; never more than kDropSlots long.
    const std::vector<int> &arrivedThisTick() const { return arrived_; }

    // -----------------------------------------------------------------------
    // Every slot onto the pipeline, empty ones included -- the band is a fixed
    // size and a slot just vacated has to be told it is empty.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // EVERY DROP OFF THE FIELD.
    //
    // For --kill-test, which asks "did this kill leave a steak" as a DELTA on
    // the live count -- and there are eight slots (v1's own cap), so after the
    // eighth carcass the ninth steak REPLACES one and the delta is zero. Two
    // fish were reported as leaving nothing while the engine's own log beside
    // them said they had left a steak, which is a test measuring the pool
    // rather than the kill.
    //
    // publish() writes every slot including the dead ones, so nothing else has
    // to be told: the instances go dark on the next frame.
    // -----------------------------------------------------------------------
    void clearAll() {
        for (Item &d : items_) d = Item{};
    }

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
            const float spin[4] = {d.pos.x, d.pos.y, d.pos.z, d.dspin};
            world.setDropInstance(i, d.model, m, d.pos.x - ox, d.pos.y - oy, d.pos.z - oz, true,
                                  spin);
        }
        world.flushDropInstances();
    }

    // WHAT IT ACTUALLY CLEARS, so that the floor can be checked rather than
    // argued about. The underside is `pos.y - half` BY CONSTRUCTION -- publish()
    // subtracts exactly that to turn a centre into the corner a voxel mesh
    // wants -- so this is the one number the three-voxel rule is about, and it
    // is read from the same place the renderer reads.
    bool clearance(int i, float *outM) const {
        if (i < 0 || i >= kDropSlots) return false;
        const Item &d = items_[size_t(i)];
        if (!d.live) return false;
        *outM = d.pos.y - 0.5f * float(d.sy) * VOXEL_M - d.groundY;
        return true;
    }

    // IS ANYTHING IN FLIGHT. No longer a gate on starting one -- see the note
    // there -- and kept because it is still the honest answer to "is an absorb
    // happening", which is what a test or a HUD would ask.
    bool anyTaken() const {
        for (const Item &d : items_)
            if (d.live && d.taken) return true;
        return false;
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
    bool snatched_ = false;  // one frame, at the snatch -- see snatchedNow
    // One frame, at the ARRIVAL -- see arrivedThisTick. Reserved once so a
    // frame that absorbs a pile does not allocate in the middle of the tick.
    std::vector<int> arrived_;
};

}  // namespace v2
