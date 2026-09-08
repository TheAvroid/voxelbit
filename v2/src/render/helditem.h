// ---------------------------------------------------------------------------
// helditem.h -- the axe in the player's hand, and the swing.
//
// Ported from the JS engine's held-item system (src/assets/held-items.js,
// src/ui/hud.js's PICK_DEFS, the swing block in src/main/tick-camera.js and the
// reach constants in src/sim/tools.js). What is here is the whole of that
// system's SHAPE -- a model, a pose, a three-phase swing and an impact the
// swing lands at.
//
// WHAT THIS FILE DOES NOT DO IS DRAW IT. The JS engine composites its viewmodel
// over the finished frame with a lighting model of its own (heldLight, in its
// render/wgsl/pre.js) because it has no other way to reach it. v2 does: the tool
// is a MODEL IN THE SCENE -- meshed, in a bottom-level structure, and placed by
// an instance transform this file computes once a frame (World::loadHeldModel
// and setHeldInstance). It is lit, shadowed and bounced exactly as a boulder
// is, because to the tracer it is one.
//
// SO IT CASTS A SHADOW AND SHOWS UP IN THE LIGHT IT BOUNCES, which the earlier
// cut of this could not: that one walked the voxel grid in camera space at the
// primary vertex, which no secondary ray could see. The price is that a tool
// standing in the world can intersect it -- walk into a trunk and the axe goes
// through it -- and that depth of field blurs it like anything else this close
// to the lens.
//
// ---------------------------------------------------------------------------
// THE POSE IS THE JS ENGINE'S BAKE, BAR ONE NUMBER
//
//     { x: 0.91, y: -0.10, z: 0.96, yaw: 0.04, pitch: -1.42, roll: 1.58,
//       scale: 0.08 }
//
// hand-tuned in that engine's held-item panel on 2026-07-15 and unchanged
// since. Two conversions stand between it and this renderer and BOTH are
// necessary -- with either one missing the axe is in the wrong place rather
// than approximately right.
//
// ONE NUMBER HAS SINCE MOVED: x is 0.800, not 0.91, at the user's own call on
// 2026-09-06 -- the tool sat further out toward the edge of the frame here than
// it reads in the engine it came from. It is written into the default rather
// than left on the slider because a slider is a session and a default is what
// v2 opens with. The rest of the bake is untouched.
//
// The two conversions:
//
//   VOXELS TO METRES. That engine's world unit is the voxel; this one's is the
//   metre, at ten voxels to it. So every length here is multiplied by VOXEL_M.
//
//   72 DEGREES TO WHATEVER THE PLAYER HAS SET. A pose fixed in camera space is
//   fixed in SPACE, not on the screen: widen the field of view and the same
//   anchor slides toward the middle of the frame and shrinks. v2 opens at 90
//   degrees where the JS engine was 72, and the Fov slider moves it further. So
//   the lateral offsets and the model's size are scaled by the ratio of the two
//   half-angle tangents, which holds the axe at the same place in the FRAME and
//   the same size on it at any field of view -- which is what "the same
//   position" means for a thing held in front of the eye.
//
// ---------------------------------------------------------------------------
// WHAT A SWING DOES HERE, AND WHAT IT CANNOT DO YET
//
// The animation is exact: 570 ms, the same three phases, the same easing, and
// the impact still lands 250 ms in. The hold-to-repeat is the same rule.
//
// The BITE is not ported, and it is not a matter of effort. In the JS engine a
// swing carves a sphere out of a mutable voxel array and drops the chunk; v2's
// world is procedural geometry compiled into acceleration structures, with no
// per-voxel store to write to and nothing that could rebuild a chunk's mesh
// mid-frame. Carving would be a destructible-world feature, not a port.
//
// What IS ported is everything up to that point -- the reach, the voxel-
// accurate march, and the verdict -- so the swing knows exactly what it hit and
// how far away it was. `Swing::hit` is where a bite would be spent.
// ---------------------------------------------------------------------------
#pragma once

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"
#include "../gpu/world.h"
#include "../scene/collide.h"
#include "bow.h"
#include "../scene/voxelworld.h"
#include "player.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace v2 {

// ---------------------------------------------------------------------------
// One item's pose in the hand. Angles in radians, offsets in VOXELS, at the JS
// engine's 72-degree field of view -- see the header. Kept in those units and
// not converted at load, because this is the thing the settings menu edits and
// the thing that gets pasted back into the source: a slider that read 0.091
// metres would not be recognisable as the 0.91 in the engine it came from.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// A POSE IS NOW IN METRES AND IN WORLD VOXELS, and `scale` is 1.
//
// It used to be neither. The offsets were voxels at the JS engine's 72-degree
// field of view, corrected to whatever this camera has, and `scale` shrank a
// model authored at one voxel per unit down to the few MILLIMETRES a viewmodel
// voxel measured -- 11.0 mm for the tools, 14.6 mm for the bow, which is the
// mismatch that started this (user 2026-09-07: "make sure all the hand held
// items are the same size ... the hand held items need to match the 10cm voxel
// resolution. no exceptions").
//
// So a held voxel IS a world voxel. scale 1.0 is exact, the axe is 50 x 90 x 10
// centimetres because that is what its 5 x 9 x 1 grid measures at 10 cm, and
// the bow is a real 1.8 m bow. It is the same object in the hand, on the
// ground, and in the acceleration structure.
//
// AND THE FIELD-OF-VIEW CORRECTION IS GONE WITH IT. That existed to hold a
// SCREEN-SPACE viewmodel at the same place and size in the frame whatever the
// lens was doing. A real object does not work that way: it stands where it
// stands, and a wider lens simply sees more of the room around it. Keeping the
// correction would have made a 10 cm voxel measure 13.8 cm at 90 degrees and
// something else again at 60, which is the one thing "no exceptions" rules out.
// kPoseTanHalfFov is therefore no longer read by anything.
//
// The offsets below are ten times what they were, because the models are ten
// times bigger and the framing they were tuned to is worth keeping: an object
// N times the size at N times the distance projects to exactly the same place.
// ---------------------------------------------------------------------------
struct HeldPose {
    float x = 7.27f, y = -0.91f, z = 8.73f;  // in world voxels from the eye
    float yaw = 0.04f, pitch = -1.42f, roll = 1.58f;
    float scale = 1.0f;  // 1 model voxel per world voxel -- see above
};

// ---------------------------------------------------------------------------
// ONE TOOL: a model, the pose it hangs at, and what to call it.
//
// The pose is PER TOOL and not shared, which is the JS engine's rule and the
// reason its PICK_DEFS is a table rather than a constant -- tuning one item
// there never moves the others. The pick happens to start on the axe's bake
// because it is the same haft and the same swing, exactly as that engine's
// own comment says of it.
// ---------------------------------------------------------------------------
// WHAT A TOOL CAN TAKE, which is the JS engine's toolTakesFor (sim/tools.js)
// reduced to the materials v2 can actually be swung at.
//
// That engine asks this of the SWING and the AUDIO both, through one function,
// and its note says why in as many words: "a sound that disagrees with the
// swing is worse than no sound, because it teaches the player the wrong thing
// about their tool." v2 has nothing to carve, so the swing has no opinion --
// which leaves the audio as the only thing this decides, and makes it more
// important rather than less that it is declared on the tool rather than
// guessed from its name at the moment a blow lands.
enum class Takes : uint8_t {
    Nothing,  // a bow. It does not swing at all -- see HeldItem::update
    Wood,     // an axe
    Stone,    // a pick
};

struct Tool {
    const char *name = "";
    Takes takes = Takes::Nothing;
    // IS IT ACTUALLY IN THE KIT. False while it is lying in the wood -- the
    // models and the pose stay loaded either way, because a dropped axe is the
    // same axe and picking it up must not cost a reload.
    bool carried = true;
    // Where the model was read from. Only the bow needs it -- see retuneArrow,
    // which recomposes the whole strip from the file every time the arrow moves
    // -- but it is on the Tool rather than beside the bow because "which file
    // is this" is a property of a tool and not of one feature.
    std::string path;
    // A STRIP, not a model. Most tools are one frame and index [0] for ever;
    // the bow is fourteen -- seven of the draw with an arrow on the string,
    // then the same seven without it, for after the loose. They are separate
    // bottom-level structures sharing one top-level slot, so stepping through
    // the draw is the same refit as moving the hand.
    std::vector<int> models;
    HeldPose pose;
    int sx = 0, sy = 0, sz = 0;  // the strip's shared box, in its own voxels
    // Whether the draw clock drives which frame is shown.
    bool bow = false;
};

// -- the bow's own timing, from the JS engine's ui/audio.js -----------------
//
// The right button pulls 00 -> 02 and HOLDS at 02; releasing runs 03 out to the
// end and returns to rest. Both are STEPPED, not interpolated: a frame is
// picked, so the bow reads as drawn art rather than as a tween.
inline constexpr float kBowDrawMs = 260.0f;
// The loose and the return to rest -- TWICE the speed of the pull.
inline constexpr float kBowRelMs = 130.0f;

// -- the swing, from tick-camera.js -----------------------------------------
//
// 570 ms end to end, with the impact at 250. Both numbers are load-bearing and
// neither is round: the animation was tuned against them, and the impact is
// timed to the frame the head is at the bottom of the arc, so moving one
// without the other lands the blow before or after the axe arrives.
inline constexpr float kSwingMs = 570.0f;
inline constexpr float kImpactMs = 250.0f;
// How long a tool takes to rise back into frame after a change of hands.
inline constexpr float kSwapMs = 240.0f;

// -- AND IT OVERSHOOTS ON THE WAY UP (user 2026-09-07) ----------------------
//
// "the object comes up, a little higher then it sits naturally then returns to
// the resting state." The swap already lifted the tool into frame, but along a
// smoothstep, which is monotonic -- it approaches the resting pose and stops.
// There is nothing to overshoot with, so a bounce cannot be tuned into it; it
// has to be a different curve.
//
// A bounce IS a spring, so this is one: the unit step response of a damped
// second-order system,
//
//     x(t) = e^(-zwt) * (cos(wd t) + (z w / wd) sin(wd t)),   wd = w sqrt(1-z^2)
//
// which is 1 at t=0 and decays to 0 through zero -- so a pose displaced by
// -drop * x(t) starts a drop low, rises, passes its resting place, and settles
// back onto it. One expression for the whole movement, and the shape is set by
// two numbers that mean something rather than by hand-fitted keyframes.
//
// The overshoot is exp(-pi z / sqrt(1-z^2)) of the drop, so z = 0.34 rises
// about a third of the drop above the pose -- around two centimetres at this
// scale, which reads as a bounce without looking sprung. w sets the speed: the
// first crossing is at a quarter period, ~70 ms, so the tool is UP about as
// fast as it was before and spends the rest of the time settling.
inline constexpr float kSwapW = 22.0f;      // rad/s
inline constexpr float kSwapZeta = 0.34f;   // ...and its damping ratio
// Settled to within a thousandth: 4 time constants, 1/(z*w) each.
inline constexpr float kSwapSettleMs = 4000.0f / (kSwapZeta * kSwapW);

// -- the reach, from tools.js -----------------------------------------------
//
// 53 voxels level, opening out to 107 when you look steeply down -- the same
// pair the JS engine shares between every stone tool and its kill test, so the
// axe and the crosshair can never disagree about what is close enough. In
// metres here; the constants themselves are still the voxel counts, so they can
// be read against the engine they came from.
inline constexpr float kReachHorizVox = 53.0f;
inline constexpr float kReach3dVox = 107.0f;

// ---------------------------------------------------------------------------
// WHAT A SWING RAN INTO.
//
// `kind` is deliberately coarse. The JS engine's version of this answers with a
// palette id because it is about to carve that material; this one has nothing
// to carve, so what it is actually useful for is telling the player what they
// hit -- ground, trunk, rock, or thin air.
// ---------------------------------------------------------------------------
struct Swing {
    enum Kind { None, Ground, Trunk, Rock };
    Kind kind = None;
    bool hit = false;
    float dist = 0.0f;   // metres from the eye
    Vec3 point{0, 0, 0}; // where it landed, world metres
    // WHAT THE GROUND IS MADE OF, on a Ground hit and only then -- one of the
    // mat:: ids, mat::AIR otherwise. The kinds above are coarse on purpose (see
    // the note), but "ground" covers a grass bank and a bare stone hillside,
    // and a pick has a great deal to say about the difference between them. It
    // is one lookup, on the one frame a blow lands, so the four extra height
    // evaluations topMaterial wants for its slope are paid at most once every
    // 570 ms.
    uint8_t material = mat::AIR;
    // A MUSHROOM, not a boulder. Both are standable solids and the kind above
    // cannot tell them apart; `bouncy` is set for mushrooms alone (see
    // makeInstance), so this carries it out rather than inventing a fifth kind
    // for one cap. The audio is the only reader: the engine this came from
    // leaves a mushroom cap silent.
    bool soft = false;
};

// ---------------------------------------------------------------------------
// The melee ray: what is under the crosshair, within reach.
//
// AMANATIDES AND WOO OVER THE TERRAIN, exactly as tools.js's voxRay is, and for
// the reason its own note gives: a fixed-step march SAMPLES a ray rather than
// walking it, so it can skip a column entirely or land twice in one. This
// visits every column the ray truly passes through, in order, and the distance
// it reports is the true entry distance.
//
// THE MODELS ARE NOT MARCHED THE SAME WAY, and that is the one real departure
// from the original. A tree in v2 exists as an instance transform and a
// collider -- an upright ellipse the width of the TRUNK, measured off the
// model's own voxels (see scene/collide.h) -- and not as voxels in a grid this
// could step through. So a trunk is tested as that cylinder. Doing it any other
// way would mean disagreeing with the thing that decides what you can walk
// into, and an axe that bites where a body cannot stand is worse than a coarse
// one.
// ---------------------------------------------------------------------------
inline Swing swingRay(const WalkWorld &w, const Vec3 &eye, const Vec3 &dir) {
    Swing out;
    if (!w.terrain) return out;

    // The reach opens up as you look down, so the ground at your feet is always
    // in range without the horizontal reach having to be long enough to hit a
    // tree two body lengths away.
    const float cp = sqrtf(maxf(0.0f, dir.x * dir.x + dir.z * dir.z));
    const float reach =
        minf(kReach3dVox, kReachHorizVox / maxf(0.15f, cp)) * VOXEL_M;

    // -- the trunks and the boulders ----------------------------------------
    //
    // Tested first and kept as a CANDIDATE, not returned: the ground may still
    // be nearer, and the swing lands on whatever the crosshair reaches first.
    float bestT = reach;
    for (int i = 0; i < w.solidCount; ++i) {
        const Solid &s = w.solids[i];
        if (s.hx <= 0.0f || s.hz <= 0.0f) continue;
        // Ray against the upright elliptic cylinder, solved in the space where
        // the ellipse is a unit circle: divide both axes by their half extent
        // and it is an ordinary quadratic.
        const float ox = (eye.x - s.cx) / s.hx, oz = (eye.z - s.cz) / s.hz;
        const float dx = dir.x / s.hx, dz = dir.z / s.hz;
        const float a = dx * dx + dz * dz;
        if (a < 1e-12f) continue;
        const float b = 2.0f * (ox * dx + oz * dz);
        const float c = ox * ox + oz * oz - 1.0f;
        const float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f) continue;
        const float sq = sqrtf(disc);
        // The near root, or the far one when the eye is already inside the
        // ellipse -- standing against a trunk still lets you chop it.
        float t = (-b - sq) / (2.0f * a);
        if (t < 0.0f) t = (-b + sq) / (2.0f * a);
        if (t < 0.0f || t >= bestT) continue;
        const float y = eye.y + dir.y * t;
        // The cylinder runs from the ground to the model's top. Below the
        // ground is not a miss -- it is the ground's business, and the march
        // below will find it.
        if (y > s.top) continue;
        bestT = t;
        out.hit = true;
        out.kind = s.standable ? Swing::Rock : Swing::Trunk;
        out.soft = s.bouncy;
        out.material = mat::AIR;
        out.dist = t;
        out.point = eye + dir * t;
    }

    // -- the ground ---------------------------------------------------------
    const float ox = eye.x / VOXEL_M, oy = eye.y / VOXEL_M, oz = eye.z / VOXEL_M;
    int vx = int(floorf(ox)), vy = int(floorf(oy)), vz = int(floorf(oz));
    const int sx = dir.x > 0.0f ? 1 : -1, sy = dir.y > 0.0f ? 1 : -1,
              sz = dir.z > 0.0f ? 1 : -1;
    const float kInf = 1e30f;
    const float ax = fabsf(dir.x) < 1e-9f ? kInf : 1.0f / fabsf(dir.x);
    const float ay = fabsf(dir.y) < 1e-9f ? kInf : 1.0f / fabsf(dir.y);
    const float az = fabsf(dir.z) < 1e-9f ? kInf : 1.0f / fabsf(dir.z);
    float tx = (ax == kInf) ? kInf : (dir.x > 0.0f ? float(vx) + 1.0f - ox : ox - float(vx)) * ax;
    float ty = (ay == kInf) ? kInf : (dir.y > 0.0f ? float(vy) + 1.0f - oy : oy - float(vy)) * ay;
    float tz = (az == kInf) ? kInf : (dir.z > 0.0f ? float(vz) + 1.0f - oz : oz - float(vz)) * az;

    // In VOXELS, because that is what the march measures in. The candidate
    // above is in metres, so it is converted once here rather than the march
    // converting on every step.
    const float maxT = bestT / VOXEL_M;
    float t = 0.0f;
    TerrainMemo memo;
    for (int guard = 0; guard < 4096 && t <= maxT; ++guard) {
        // The terrain is a height field, so "is this column solid at this
        // height" is one comparison. heightVox is the topmost solid voxel.
        if (vy <= w.terrain->heightVox(vx, vz, memo)) {
            out.hit = true;
            out.kind = Swing::Ground;
            out.soft = false;
            // The column's own surface material, not the voxel the ray stopped
            // in: the march stops at the topmost solid voxel, which IS the
            // surface, and topMaterial is the one function that decides what
            // that surface is made of -- so this cannot disagree with what the
            // mesher put there to be looked at.
            out.material = w.terrain->topMaterial(vx, vz, w.terrain->heightVox(vx, vz, memo), memo);
            out.dist = t * VOXEL_M;
            out.point = eye + dir * out.dist;
            return out;
        }
        if (tx <= ty && tx <= tz) { t = tx; tx += ax; vx += sx; }
        else if (ty <= tz) { t = ty; ty += ay; vy += sy; }
        else { t = tz; tz += az; vz += sz; }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The item itself: the model, the animation clocks, and the pass that draws it.
// ---------------------------------------------------------------------------
// Where the tool sits this frame, as a top-level instance: a 3x3 in row-major
// order with the scale already in it, and a translation. See setHeldInstance.
struct HeldXform {
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    bool show = false;
    // WHERE THE ITEM IS IN THE CAMERA'S OWN FRAME, in metres: right, up,
    // forward. The renderer does not need it -- the nine numbers above already
    // place the model -- but a projectile leaving the hand does, and it needs
    // the pose AFTER the swing, the bob and the sway have moved it. Reported
    // here rather than recomputed, so the shaft cannot leave from a bow that is
    // somewhere else. See App::loose.
    Vec3 cam{0, 0, 0};
};

class HeldItem {
  public:
    // Whether there is anything in the hand at all. H puts it away.
    bool shown = true;

    // -------------------------------------------------------------------
    // HOW MUCH THE HAND MOVES, as a gain over the stride and the breath in
    // xform(). 1.0 is what the constants down there describe; defaults::
    // kHandSway sets what is actually used, and it is 2.0.
    //
    // A GAIN AND NOT SIX NUMBERS. The movement is two sine pairs with tuned
    // phases and periods, and what has been asked of it three times running
    // is that it be BIGGER -- never faster, never a different shape. One
    // multiplier is exactly that ask and cannot express anything else, so it
    // can go to either end without the hand starting to buzz or to drift.
    //
    // WHY THE 2.0 IS NOT JUST FOLDED INTO THOSE CONSTANTS: they are the
    // tuned SHAPE, carried over from the JS engine and readable against it
    // (see the note beside them). Multiplying them out would leave six
    // numbers that match nothing in either engine and no way to say how far
    // from the original this has travelled. It has travelled 2.0.
    //
    // ZERO IS A USEFUL SETTING, not a degenerate one: it nails the tool to
    // the pose, which is what a reference screenshot of a viewmodel wants.
    // -------------------------------------------------------------------
    float sway = 1.0f;

    // -----------------------------------------------------------------------
    // Put a tool in the kit. Everything about it as GEOMETRY -- the palette
    // registration, the mesh, the triangle pool, the structure -- lives in the
    // world, because that is where every other model's does; what stays here is
    // the model's box, which is all a pose needs.
    //
    // ORDER IS THE ORDER YOU SCROLL THROUGH, and the first one added is what
    // the hand opens with -- the JS engine's giveStartKit rule, where the axe
    // goes in first so slot one is what the player holds when the world
    // appears.
    // -----------------------------------------------------------------------
    bool add(World &world, const char *name, const std::string &voxPath, const HeldPose &pose,
             Takes takes = Takes::Nothing) {
        Tool t;
        t.name = name;
        t.takes = takes;
        t.pose = pose;
        t.path = voxPath;
        const int m = world.addHeldModel(voxPath, &t.sx, &t.sy, &t.sz);
        if (m < 0) return false;
        t.models.push_back(m);
        tools_.push_back(t);
        return true;
    }

    // -----------------------------------------------------------------------
    // AN EMPTY HAND, AS A SLOT (user 2026-09-07: "give an empty hand. so 4
    // slots total").
    //
    // A Tool with no models. Everything downstream already handles that -- the
    // model lookup returns -1 for an empty strip and the world draws nothing --
    // so this needs no special case anywhere else, which is why it is a slot
    // rather than a mode. `shown` stays what it is: H still puts the whole hand
    // away, and that is a different thing from holding nothing.
    //
    // It TAKES nothing, so a swing with it lands on no material and the sound
    // path stays silent of its own accord -- see Takes and ToolSounds::blow.
    // -----------------------------------------------------------------------
    bool addEmpty(const char *name) {
        Tool t;
        t.name = name;
        t.takes = Takes::Nothing;
        tools_.push_back(t);
        return true;
    }

    // -----------------------------------------------------------------------
    // The bow: one file, cut into a strip, registered as fourteen models.
    //
    // The two halves go in one after the other and the draw indexes them by
    // arithmetic rather than by a second list -- nocked frame f is models[f],
    // bare frame f is models[frames + f] -- which is the same trick the engine
    // this came from uses with BOW_IT and BOW_NOCK being two runs of
    // consecutive item ids.
    // -----------------------------------------------------------------------
    bool addBow(World &world, const char *name, const std::string &voxPath,
                const HeldPose &pose) {
        std::string err;
        const BowStrip strip = parseBowStrip(voxPath, &err);
        if (!strip.ok()) {
            std::fprintf(stderr, "v2: bow %s: %s -- skipped\n", voxPath.c_str(),
                         err.empty() ? "no strip" : err.c_str());
            return false;
        }

        Tool t;
        t.name = name;
        t.pose = pose;
        t.path = voxPath;
        t.bow = true;
        for (const VoxModel &m : strip.withArrow) {
            const int i = world.addHeldVox(m, voxPath + " (nocked)", &t.sx, &t.sy, &t.sz);
            if (i < 0) return false;
            t.models.push_back(i);
        }
        for (const VoxModel &m : strip.bowOnly) {
            const int i = world.addHeldVox(m, voxPath + " (bare)", &t.sx, &t.sy, &t.sz);
            if (i < 0) return false;
            t.models.push_back(i);
        }
        bowFrames_ = strip.frames;
        tools_.push_back(t);
        std::printf("v2: bow %s  %d frames, %dx%dx%d\n", voxPath.c_str(), strip.frames, t.sx,
                    t.sy, t.sz);
        std::fflush(stdout);
        return true;
    }

    // -----------------------------------------------------------------------
    // PUT DOWN WHAT IS IN THE HAND. Returns which tool left, or -1.
    //
    // The tool stays loaded and keeps its pose; only `carried` moves. The hand
    // then falls to the next thing being carried, or to nothing at all -- and
    // nothing is a real state here, which is why `shown` is cleared rather than
    // the selection being left pointing at something that is not there.
    // -----------------------------------------------------------------------
    int dropSelected() {
        if (!ready() || !tools_[size_t(sel_)].carried) return -1;
        // AN EMPTY HAND CANNOT BE PUT DOWN. Without this, Q on the fourth slot
        // marked it uncarried -- taking it out of the wheel for good, since
        // nothing can ever pick it up again -- and tossed a drop whose model is
        // -1, which is an item lying in the wood that cannot be drawn or
        // collected. Having no models is exactly what makes it the empty slot,
        // so it is the right thing to ask.
        if (tools_[size_t(sel_)].models.empty()) return -1;
        const int gone = sel_;
        tools_[size_t(gone)].carried = false;
        drawing_ = false;
        loosed_ = false;
        const int n = int(tools_.size());
        for (int i = 1; i <= n; ++i) {
            const int j = (gone + i) % n;
            if (tools_[size_t(j)].carried) {
                sel_ = j;
                swapT0_ = nowMs_;
                shown = true;
                return gone;
            }
        }
        shown = false;  // an empty hand
        return gone;
    }

    // ...and take it back. The hand only changes to it if it was empty, so
    // walking over a pick while swinging an axe does not swap the axe out.
    void give(int tool) {
        if (tool < 0 || tool >= int(tools_.size())) return;
        tools_[size_t(tool)].carried = true;
        if (!shown) {
            sel_ = tool;
            swapT0_ = nowMs_;
            shown = true;
        }
    }
    bool carrying() const { return ready() && tools_[size_t(sel_)].carried; }
    const Tool &tool(int i) const { return tools_[size_t(i)]; }

    bool holdingBow() const { return ready() && tools_[size_t(sel_)].bow; }
    Takes takes() const { return ready() ? tools_[size_t(sel_)].takes : Takes::Nothing; }

    // The arrow's offset on the string, in whole voxels -- see ArrowOffset in
    // render/bow.h for what it means and why it cannot be fractional. Read by
    // the settings panel, which is the only thing that writes it.
    ArrowOffset &arrow() { return arrow_; }
    const ArrowOffset &arrow() const { return arrow_; }

    // -----------------------------------------------------------------------
    // MOVE THE ARROW, AND REBUILD THE STRIP AROUND IT.
    //
    // There is no cheaper way and it is worth saying why, because "it is only
    // three numbers" invites one. The arrow is not an object placed near the
    // bow -- it is voxels STAMPED INTO the same grid the bow is stamped into
    // (render/bow.h), so moving it is not a transform, it is a different model.
    // All fourteen frames go, not the seven with an arrow in them: both strips
    // share one box by design, and a box that changed for one strip and not the
    // other would shift the bow by a voxel the moment an arrow was loosed.
    //
    // IT COSTS TWO DEVICE SYNCS PER FRAME OF THE STRIP -- see World::buildBlas
    // -- so this is a tuning control and not something to drive from anything
    // that runs per frame. That is also why the panel steps it in whole voxels:
    // a value that can only change when you mean it is a value that cannot be
    // dragged through fifty rebuilds.
    // -----------------------------------------------------------------------
    bool retuneArrow(World &world, const ArrowOffset &off) {
        // THE BOW, NOT WHATEVER IS IN THE HAND. This asked the SELECTED tool at
        // first, which is right whenever the panel is showing the rows -- they
        // only appear with a bow held -- and silently wrong everywhere else.
        // --arrow-pos applies at load, and at load the hand holds the axe, so
        // the whole thing returned false and did nothing without saying so.
        // The arrow belongs to the bow however the request arrived.
        int bi = -1;
        for (size_t i = 0; i < tools_.size(); ++i)
            if (tools_[i].bow) {
                bi = int(i);
                break;
            }
        if (bi < 0) return false;
        Tool &t = tools_[size_t(bi)];
        std::string err;
        const BowStrip strip = parseBowStrip(t.path, &err, off);
        if (!strip.ok() || strip.frames != bowFrames_) {
            std::fprintf(stderr, "v2: bow %s: %s -- arrow not moved\n", t.path.c_str(),
                         err.empty() ? "the strip changed shape" : err.c_str());
            return false;
        }
        if (int(t.models.size()) < strip.frames * 2) return false;
        for (int f = 0; f < strip.frames; ++f) {
            world.replaceHeldVox(t.models[size_t(f)], strip.withArrow[size_t(f)],
                                 t.path + " (nocked)", &t.sx, &t.sy, &t.sz);
            world.replaceHeldVox(t.models[size_t(strip.frames + f)], strip.bowOnly[size_t(f)],
                                 t.path + " (bare)", &t.sx, &t.sy, &t.sz);
        }
        arrow_ = off;
        return true;
    }

    bool ready() const { return !tools_.empty(); }
    int count() const { return int(tools_.size()); }
    int selected() const { return sel_; }
    const char *name() const { return ready() ? tools_[size_t(sel_)].name : "empty"; }

    // The pose of whatever is in the hand, for the menu to edit.
    HeldPose &pose() { return tools_[size_t(sel_)].pose; }

    // -----------------------------------------------------------------------
    // Change tools. `d` is +1 or -1 and it wraps, which is what a wheel wants.
    //
    // THE SWAP ANIMATION IS THE POINT OF ROUTING IT THROUGH HERE: the tool
    // drops out of frame and the next rises in, which is what makes a change of
    // hands read as one rather than as a cut. See kSwapMs.
    // -----------------------------------------------------------------------
    void cycle(int d) {
        if (tools_.size() < 2) return;
        const int n = int(tools_.size());
        // SKIP WHAT IS NOT BEING CARRIED. The wheel walks the kit, and a tool
        // lying on the ground is not in it -- stopping on an empty hand would
        // read as the wheel being broken.
        for (int i = 0; i < n; ++i) {
            sel_ = ((sel_ + d) % n + n) % n;
            if (tools_[size_t(sel_)].carried) break;
        }
        swapT0_ = nowMs_;
    }
    void select(int i) {
        if (i < 0 || i >= int(tools_.size()) || i == sel_) return;
        sel_ = i;
        swapT0_ = nowMs_;
    }

    // -----------------------------------------------------------------------
    // Which model the world should put in the held slot this frame.
    //
    // WHICH FRAME OF THE DRAW, for the bow, and it is the JS engine's bowFrame
    // exactly: pull to 02 over kBowDrawMs and hold there while the button is
    // down, then run 03 to the end over kBowRelMs and settle back to 00. The
    // BARE strip takes over from the moment it is loosed, so the arrow is gone
    // from the bow the instant it leaves -- and comes back when the strip
    // returns to rest, which is the bow being nocked again.
    // -----------------------------------------------------------------------
    int model() const {
        if (!ready()) return -1;
        const Tool &t = tools_[size_t(sel_)];
        if (t.models.empty()) return -1;
        if (!t.bow || bowFrames_ <= 0) return t.models[0];

        int f = 0;
        if (drawing_) {
            const float k = float((nowMs_ - bowT0_) / double(kBowDrawMs));
            f = mini(2, int(clampf(k, 0.0f, 1.0f) * 3.0f));
        } else {
            const float e = float((nowMs_ - bowRel_) / double(kBowRelMs));
            if (e >= 0.0f && e < 1.0f)
                f = mini(bowFrames_ - 1, 3 + int(e * float(bowFrames_ - 3)));
        }
        const bool bare = loosed_ && f > 0;
        const size_t i = size_t(f) + (bare ? size_t(bowFrames_) : 0);
        return t.models[i < t.models.size() ? i : 0];
    }

    // True from the loose until the bow settles back to rest -- the frames
    // without the arrow on them.
    bool bowLoosed() const { return loosed_; }

    // -----------------------------------------------------------------------
    // One tick of the animation.
    //
    // `swingHeld` is the left mouse button's state, and holding it swings over
    // and over -- the JS engine's rule, in its own words: each auto-repeat
    // re-arms the impact-timed hit. Returns true on the ONE frame an impact
    // lands, which is 250 ms into whichever swing is running.
    //
    // `bobAmp` is the player's walk bob, and it does double duty: see the note
    // below on why the idle sway is gated on it.
    // -----------------------------------------------------------------------
    // `drawHeld` is the right mouse button, and it only means anything with a
    // bow in the hand. Returns true on the frame an arrow is loosed, with `draw`
    // set to how far it was pulled -- 0 at the earliest release, 1 at a full
    // pull, which is what the shot is worth.
    bool update(float dt, bool swingHeld, float bobAmp, bool drawHeld = false,
                float *draw = nullptr) {
        nowMs_ += double(dt) * 1000.0;

        // -- the draw --------------------------------------------------------
        //
        // THREE MOMENTS AND NOT ONE, because the bow's voices need all three --
        // the string starts creaking when the pull begins, the creak is cut the
        // instant it is released whether or not a shaft left, and the re-nock
        // speaks when the bow settles back to rest. That is the JS engine's
        // ui/audio.js exactly: playBowStretch on the mousedown, stopBowStretch
        // on the mouseup, playBowReload from the tick that sees bowAtRest.
        // Reported as edges rather than as state so a caller cannot fire one of
        // them twice by reading it on two frames.
        bool loosedNow = false;
        drewNow_ = false;
        nockedNow_ = false;
        const bool wantDraw = drawHeld && shown && holdingBow();
        if (wantDraw && !drawing_) {
            drawing_ = true;
            loosed_ = false;
            bowT0_ = nowMs_;
            drewNow_ = true;
        } else if (!wantDraw && drawing_) {
            drawing_ = false;
            bowRel_ = nowMs_;
            loosed_ = true;
            loosedNow = true;
            // HOW FAR IT WAS PULLED, and the shot is worth exactly that: the
            // JS engine's dk, clamped 0..1 over the same kBowDrawMs the frames
            // step through, so a snatched release carries less than a held one.
            if (draw) *draw = clampf(float((nowMs_ - bowT0_) / double(kBowDrawMs)), 0.0f, 1.0f);
        }
        // Back at rest: the bow is nocked again and the arrow is on it.
        if (loosed_ && !drawing_ && nowMs_ - bowRel_ >= double(kBowRelMs)) {
            loosed_ = false;
            nockedNow_ = true;
        }

        if (shown != wasShown_) {
            // A tool that has just come into the hand rises into frame rather
            // than appearing. Also what a swap would use, when there is ever a
            // second thing to hold.
            wasShown_ = shown;
            if (shown) swapT0_ = nowMs_;
        }

        if (swingHeld && shown && !holdingBow() && nowMs_ - swingStart_ >= double(kSwingMs)) {
            swingStart_ = nowMs_;
            impactAt_ = nowMs_ + double(kImpactMs);
        }

        // -- THE IDLE SWAY IS GATED, AND THE FILM IS WHY --------------------
        //
        // The JS engine's hand drifts on two slow sine waves for ever, because
        // a held arm does. Ported straight it would move the tool by a few
        // pixels EVERY frame, and this renderer accumulates: on the route
        // without reconstruction a still camera converges over a few hundred
        // samples, and a viewmodel that never stops moving is one smeared
        // across all of them -- or, if the film were invalidated for it, one
        // that stops the whole frame ever converging.
        //
        // It costs a top-level refit per frame as well now, which is cheap but
        // not free. So the sway lives only while something is already moving:
        // the walk bob, or a swing. Both come with the film being thrown away
        // anyway -- the bob moves the eye, the swing is over in 570 ms -- so
        // the sway costs nothing that was not already being paid. Stand still
        // and the tool settles, which is exactly the state a screenshot wants.
        const float target = maxf(bobAmp, (nowMs_ - swingStart_ < double(kSwingMs) || drawing_)
                                              ? 1.0f
                                              : 0.0f);
        live_ += (target - live_) * (1.0f - expf(-6.0f * dt));

        if (impactAt_ > 0.0 && nowMs_ >= impactAt_) {
            impactAt_ = 0.0;
            swungNow_ = true;
        } else {
            swungNow_ = false;
        }
        return loosedNow;
    }

    // True on the ONE frame a swing's blow lands, 250 ms into it. Read after
    // update(), which returns whether an ARROW was loosed instead -- the two
    // cannot happen together, since a bow does not swing.
    bool struck() const { return swungNow_; }

    // The two edges the bow's audio hangs off -- see the note in update().
    // True on exactly one frame each.
    bool drewNow() const { return drewNow_; }
    bool nockedNow() const { return nockedNow_; }
    // ...and whether the release that just happened was long enough to count.
    // The JS engine will not loose on a tap: BOW_DRAW_MS * 0.5.
    bool drawing() const { return drawing_; }

    // Where in the swing we are, 0..1, and >= 1 when nothing is swinging. Only
    // the menu's readout reads this; the pose below computes its own.
    float swingT() const { return float((nowMs_ - swingStart_) / double(kSwingMs)); }

    // True while the tool is moving under its own steam -- a swing, a swap, or
    // the sway. The film has to be thrown away on those frames, exactly as it
    // is when the camera moves: the samples already in it describe a tool that
    // is no longer where they say. It is NOT true for the walk bob, which
    // already invalidates through the eye it moves.
    bool animating() const {
        return shown && (nowMs_ - swingStart_ < double(kSwingMs) ||
                         nowMs_ - swapT0_ < double(kSwapSettleMs) || live_ > 0.01f || drawing_ ||
                         (loosed_ && nowMs_ - bowRel_ < double(kBowRelMs)));
    }

    // -----------------------------------------------------------------------
    // The pose, as an instance transform.
    //
    // Everything the JS engine's tick-camera.js does between the swing curves
    // and `set3(52, AX, showId)`: the three phases, the anchor the strike drags
    // toward the middle of the screen, the walk bob, the idle sway, the swap,
    // and the three axes built out of the result -- and then those axes carried
    // out of camera space into the world, where the structure lives.
    // -----------------------------------------------------------------------
    HeldXform xform(const V6Camera &cam, float bobPhase, float bobAmp) const {
        HeldXform out;
        out.show = shown && ready();
        if (!out.show) return out;
        const Tool &tool = tools_[size_t(sel_)];
        const HeldPose &pose = tool.pose;

        // -- the swing ------------------------------------------------------
        //
        // WINDUP raises the tool up and back over the first 35%; the STRIKE
        // slams it down to the middle of the screen over the next 20%,
        // accelerating; then it eases back to rest across the remaining 45%.
        // Smoothstep in, quadratic through the blow, smoothstep out.
        const float t = float((nowMs_ - swingStart_) / double(kSwingMs));
        float wind = 0.0f, chop = 0.0f;
        if (t < 1.0f) {
            if (t < 0.35f) {
                const float k = t / 0.35f;
                wind = k * k * (3.0f - 2.0f * k);
            } else if (t < 0.55f) {
                const float k = (t - 0.35f) / 0.2f;
                wind = 1.0f - k;
                chop = k * k;
            } else {
                const float k = (t - 0.55f) / 0.45f;
                chop = 1.0f - k * k * (3.0f - 2.0f * k);
            }
        }

        // The windup tips the head back and up; the strike drives it down and
        // forward, and drags the anchor most of the way to the screen centre.
        const float swPitch = -0.9f * wind + 1.35f * chop;
        float hx = pose.x * (1.0f + 0.06f * wind - 0.85f * chop);
        float hy = pose.y + 0.22f * wind - 0.18f * chop;
        float hz = pose.z - 0.05f * wind + 0.18f * chop;

        // -- the swap, which bounces -----------------------------------------
        // See kSwapW: this is the spring's step response, and the sign is what
        // makes it a bounce rather than a rise -- the term is SUBTRACTED, so
        // x(t) crossing zero carries the pose up past where it rests before it
        // comes back down onto it.
        const float swapT = float(nowMs_ - swapT0_) * 0.001f;
        float swapF = 0.0f;
        if (swapT < kSwapSettleMs * 0.001f) {
            const float wd = kSwapW * sqrtf(1.0f - kSwapZeta * kSwapZeta);
            swapF = expf(-kSwapZeta * kSwapW * swapT) *
                    (cosf(wd * swapT) + (kSwapZeta * kSwapW / wd) * sinf(wd * swapT));
        }
        // TWICE THE TRAVEL (user 2026-09-07: "double the switch bounce
        // depth"). 0.62 -> 1.24, and the overshoot doubles with it because the
        // spring's shape is a ratio -- z alone decides how far past the pose it
        // goes, as a FRACTION of the drop, so scaling the drop scales the whole
        // movement and the bounce keeps its proportions rather than becoming a
        // deeper dip with the same little hop on the end.
        // DOUBLED AGAIN (user 2026-09-07). 0.62 -> 1.24 -> 2.48. The spring's
        // shape is untouched by this: z alone sets how far past the pose it
        // rides, as a fraction of the drop, so the whole movement scales and
        // the overshoot keeps its proportion of it.
        hy -= 2.48f * swapF;
        // The forward part does NOT overshoot -- only the lift does. A tool that
        // also swung toward the camera and back read as being shoved rather than
        // bounced, and the ask was for one of those. maxf clips the spring's
        // negative half without changing the half that rises.
        hz -= 0.40f * maxf(0.0f, swapF);

        // -- the bob and the breath -----------------------------------------
        //
        // THE BOB AMPLITUDES ARE DOUBLED, and it is the same correction
        // player.h already made to the camera's own bob for the same reason:
        // this engine walks at twice the JS engine's speed, so the bob was
        // halved in RATE to keep it a gait rather than a tremor, and the swing
        // has to double to cover the same ground over that longer stride. The
        // two are read off the same bobPhase, so the head and the hand cannot
        // drift apart.
        //
        // DOUBLED AGAIN, BOB AND BREATH BOTH (user 2026-09-08: "double the
        // movement of the hand items, double the motion sway"). Every
        // amplitude on these two lines is twice what it was -- 0.150 -> 0.300
        // and 0.056 -> 0.112 on the stride, and the four breathing terms with
        // them -- which is how the JS engine has always taken this ask too:
        // its own comment beside the same four numbers reads "idle breathing
        // sway (tripled)". Scaling the constants is the whole change, because
        // the SHAPE is in the phases and the periods and none of those move.
        //
        // The breath is still gated on live_ and the stride still on bobAmp,
        // so a tool held still is held still and a screenshot still converges
        // -- see the note above on why that gate exists at all. This makes the
        // movement bigger, not more frequent and not more often.
        //
        // ...AND THE WHOLE OF IT IS SCALED BY `sway` -- see the member. The
        // constants below stay the 1.0 reference, so what is written here is
        // still the tuned shape and the gain says how much of it is used.
        const float ms = float(nowMs_);
        hx += (sinf(bobPhase) * 0.300f * bobAmp +
               (sinf(ms * 0.0013f) * 0.0138f + sinf(ms * 0.00073f + 1.7f) * 0.0078f) *
                   live_) *
              sway;
        hy += (-fabsf(cosf(bobPhase)) * 0.112f * bobAmp +
               (sinf(ms * 0.0017f + 0.9f) * 0.0138f + sinf(ms * 0.00091f) * 0.0072f) *
                   live_) *
              sway;

        // -- the three axes, in CAMERA space --------------------------------
        //
        // R = Rx(pitch) . Ry(yaw) . Rz(roll), pitch OUTERMOST -- which puts the
        // gimbal singularity at yaw +-90 degrees, a long way from any pose
        // anyone has tuned, so all three sliders stay independent.
        const float cy = cosf(pose.yaw), sy = sinf(pose.yaw);
        const float cp = cosf(pose.pitch + swPitch), sp = sinf(pose.pitch + swPitch);
        const float cr = cosf(pose.roll), sr = sinf(pose.roll);
        const Vec3 ax(cr * cy, sr * cp + cr * sy * sp, sr * sp - cr * sy * cp);
        const Vec3 ay(-sr * cy, cr * cp - sr * sy * sp, cr * sp + sr * sy * cp);
        const Vec3 az(sy, -cy * sp, cy * cp);

        // -- into this renderer's units ------------------------------------
        // NO FIELD-OF-VIEW TERM ANYWHERE. See the note over HeldPose: this is a
        // real object at the world's own voxel size, so neither where it is nor
        // how big it is depends on the lens.
        const Vec3 anchor(hx * VOXEL_M, hy * VOXEL_M, hz * VOXEL_M);
        const float voxel = pose.scale * VOXEL_M;

        // -- camera space to world ------------------------------------------
        //
        // The pose is expressed against the camera's own axes; the structure is
        // in the world. This is the one place the two meet, and it is a change
        // of basis and nothing more -- the camera's three vectors are
        // orthonormal, so no inverse and no transpose are involved.
        const Vec3 cu(cam.u.x, cam.u.y, cam.u.z);
        const Vec3 cv(cam.v.x, cam.v.y, cam.v.z);
        const Vec3 cw(cam.w.x, cam.w.y, cam.w.z);
        auto toWorldDir = [&](const Vec3 &v) { return cu * v.x + cv * v.y + cw * v.z; };

        // THE MODEL'S AXES ARE NOT THE POSE'S, and this is the swap that has to
        // happen exactly once. scene/vox.h's toWorld() gives a y-up grid --
        // local x is the model's width, y its HEIGHT, z its depth -- while the
        // pose was authored against the .vox file's own z-up axes, where the
        // second is depth and the third is height. So the instance's second
        // column is the pose's THIRD axis and vice versa. Get it the other way
        // round and the axe lies on its side, which is the failure scene/vox.h
        // warns about in its own header.
        const Vec3 colX = toWorldDir(ax) * voxel;  // width
        const Vec3 colY = toWorldDir(az) * voxel;  // height
        const Vec3 colZ = toWorldDir(ay) * voxel;  // depth

        out.m[0] = colX.x; out.m[1] = colY.x; out.m[2] = colZ.x;
        out.m[3] = colX.y; out.m[4] = colY.y; out.m[5] = colZ.y;
        out.m[6] = colX.z; out.m[7] = colY.z; out.m[8] = colZ.z;

        // The mesh runs from its own (0,0,0) corner, and the pose names the
        // CENTRE of the box, so the translation is the centre less half the
        // model carried down its own three axes.
        out.cam = anchor;
        const Vec3 eye(cam.pos.x, cam.pos.y, cam.pos.z);
        const Vec3 centre = eye + toWorldDir(anchor);
        const Vec3 corner = centre - (colX * (0.5f * float(tool.sx)) +
                                      colY * (0.5f * float(tool.sy)) +
                                      colZ * (0.5f * float(tool.sz)));
        out.tx = corner.x;
        out.ty = corner.y;
        out.tz = corner.z;
        return out;
    }

  private:
    std::vector<Tool> tools_;
    int sel_ = 0;

    // Milliseconds since the item came up, on the same clock the walk uses --
    // the shot clock under --shot-walk, so a scripted take animates identically
    // to a live one.
    double nowMs_ = 0.0;
    // Far enough in the past that nothing is mid-swing on the first frame.
    double swingStart_ = -1.0e9;
    double impactAt_ = 0.0;
    double swapT0_ = 0.0;
    bool wasShown_ = true;
    // How alive the hand is, 0..1 -- eased, and the gate on the idle sway.
    float live_ = 0.0f;

    // -- the bow ------------------------------------------------------------
    int bowFrames_ = 0;
    bool drawing_ = false;  // the right button is down on a bow
    bool loosed_ = false;   // shot, and not yet settled back to rest
    ArrowOffset arrow_;     // where the nocked arrow sits, in voxels
    bool drewNow_ = false;  // the pull began this frame
    bool nockedNow_ = false;  // ...and the string settled back this frame
    double bowT0_ = -1.0e9, bowRel_ = -1.0e9;
    bool swungNow_ = false;
};

}  // namespace v2
