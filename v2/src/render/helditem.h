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
struct HeldPose {
    float x = 0.800f, y = -0.10f, z = 0.96f;
    float yaw = 0.04f, pitch = -1.42f, roll = 1.58f;
    float scale = 0.08f;
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
struct Tool {
    const char *name = "";
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

// The field of view the poses above were tuned at, as the tangent of its half
// angle. 72 degrees, from FOV in the JS engine's ui/hud.js.
inline constexpr float kPoseTanHalfFov = 0.72654253f;  // tanf(36 degrees)

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
};

class HeldItem {
  public:
    // Whether there is anything in the hand at all. H puts it away.
    bool shown = true;

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
    bool add(World &world, const char *name, const std::string &voxPath, const HeldPose &pose) {
        Tool t;
        t.name = name;
        t.pose = pose;
        const int m = world.addHeldModel(voxPath, &t.sx, &t.sy, &t.sz);
        if (m < 0) return false;
        t.models.push_back(m);
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

    bool holdingBow() const { return ready() && tools_[size_t(sel_)].bow; }

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
        sel_ = ((sel_ + d) % n + n) % n;
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
        bool loosedNow = false;
        const bool wantDraw = drawHeld && shown && holdingBow();
        if (wantDraw && !drawing_) {
            drawing_ = true;
            loosed_ = false;
            bowT0_ = nowMs_;
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
        if (loosed_ && !drawing_ && nowMs_ - bowRel_ >= double(kBowRelMs)) loosed_ = false;

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
                         nowMs_ - swapT0_ < double(kSwapMs) || live_ > 0.01f || drawing_ ||
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

        // -- the swap -------------------------------------------------------
        // Squared, so it leaves fast and settles softly.
        const float swapR = maxf(0.0f, 1.0f - float((nowMs_ - swapT0_) / double(kSwapMs)));
        const float swapF = swapR * swapR * (3.0f - 2.0f * swapR);
        hy -= 0.62f * swapF * swapF;
        hz -= 0.10f * swapF;

        // -- the bob and the breath -----------------------------------------
        //
        // THE BOB AMPLITUDES ARE DOUBLED, and it is the same correction
        // player.h already made to the camera's own bob for the same reason:
        // this engine walks at twice the JS engine's speed, so the bob was
        // halved in RATE to keep it a gait rather than a tremor, and the swing
        // has to double to cover the same ground over that longer stride. The
        // two are read off the same bobPhase, so the head and the hand cannot
        // drift apart.
        const float ms = float(nowMs_);
        hx += sinf(bobPhase) * 0.150f * bobAmp +
              (sinf(ms * 0.0013f) * 0.0069f + sinf(ms * 0.00073f + 1.7f) * 0.0039f) * live_;
        hy += -fabsf(cosf(bobPhase)) * 0.056f * bobAmp +
              (sinf(ms * 0.0017f + 0.9f) * 0.0069f + sinf(ms * 0.00091f) * 0.0036f) * live_;

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

        // -- into this renderer's units and this camera's field of view ------
        // See the header: metres, and the lateral offsets and the model's size
        // referenced to the 72 degrees the pose was tuned at.
        const float k = (cam.halfH > 1e-4f) ? cam.halfH / kPoseTanHalfFov : 1.0f;
        const Vec3 anchor(hx * k * VOXEL_M, hy * k * VOXEL_M, hz * VOXEL_M);
        const float voxel = pose.scale * k * VOXEL_M;

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
    double bowT0_ = -1.0e9, bowRel_ = -1.0e9;
    bool swungNow_ = false;
};

}  // namespace v2
