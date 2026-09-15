#pragma once
// ---------------------------------------------------------------------------
// THE ASSET EDITOR'S TOOLS, PORTED FROM v1.
//
// The deck itself is older than this file -- World::buildStage makes the white
// floor and app.h's [I] is the door -- and what stood on it was a cardinal that
// you could look at and nothing else. That is a VIEWER. v1's asset editor is a
// tool, and the difference is that you HANDLE the thing on the stage and the
// output is a block of code:
//
//   ui/input.js:   "asset editor: , / . scrub frames, E move the frame,
//                   R rotate it, <- / -> reorder"
//   ui/input.js:   "left-click the stamped cardinal to SELECT it (pause) /
//                   click again to resume"
//   ...and one button whose tooltip is the whole design:
//                  "copy the per-frame offsets so you can paste them back to
//                   be baked into the code"
//
// THE SUBJECT IS THE PORCUPINE (user 2026-09-14: "put the porcupine on the
// asset editor in the middle and remove the bunny from it"). It was the bunny
// from 2026-09-13 until then, and v1's editor stages two subjects in two lanes
// with [b] to swap which the tools operate on. The list is kEditSubjects
// below, which is one row now, and [B] steps round it.
//
// WHAT THE TOOLS EDIT IS A BunnyBake, NOT THE .vox FILES. See the long note
// over that struct in bunnies.h for why alignment cannot live in the art. This
// class holds a WORKING COPY of the compiled-in table, draws the strip through
// it, and hands the result back as C++ you paste over the rows in bunnies.h.
// Nothing here writes a file and nothing here changes what the animals in the
// wood are doing until you paste.
//
// THE NAME `BunnyBake` OUTLIVED THE BUNNY and is worth not being confused by:
// the struct, poseOf, and the whole strip/slot/src machinery never knew what
// animal they were holding -- bunnies.h says so itself, over poseOf, in a note
// written when the SKUNK was the second thing to need it. A porcupine on the
// deck is that generality being used, not a rabbit in disguise.
//
// AND IT DRAWS THROUGH Bunnies::pose. That is the one rule that makes the tool
// worth trusting: the deck and the wood run the same arithmetic over the same
// table, so a bake that looks right here cannot look different out there.
// ---------------------------------------------------------------------------
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "Utils/UI/InputTypes.h"

#include "../gpu/world.h"
#include "bunnies.h"
#include "camera.h"

namespace v2 {

// Falcor's, so the key names below read the way app.h's do. A redefinition
// of the same alias in the same namespace is legal, which is what lets app.h
// keep its own copy of this line.
namespace Input = Falcor::Input;

// The strip plays at the rate everything else in this engine does; the editor
// only ever pauses it, never speeds it up. A scrub tool whose PLAY speed is
// also a setting is a tool that can show you a timing the game will not.
inline constexpr float kEditFps = 12.0f;

// ---------------------------------------------------------------------------
// THE GIZMOS -- v1'S, AS REAL VOXELS.
//
// "you left click the object to select it. then arrows pop up to move the
// object. press r to bring up the rotation arrows." That is v1's asset editor
// exactly, and its shapes are worth copying rather than inventing:
//
//   THREE STUBBY ARROWS off the model's + faces, one per axis, in the colours
//   every 3D tool uses -- X red, Y green, Z blue. A one-voxel shaft with a
//   DIAMOND pyramidal head over its last three voxels, widest at the head's
//   base and a single voxel at the tip.
//
//   TWO RINGS for rotation: a FLAT one round the waist (yaw, amber) and an
//   UPRIGHT one (pitch, violet). A ring rather than an arrow because the thing
//   you are dragging is an angle, and the shape says which angle.
//
// THEY ARE GEOMETRY, NOT AN OVERLAY, and that is the one decision worth
// defending. v1 stamps them into its voxel grid, so they are lit, shadowed and
// occluded like everything else -- you can tell an arrow is BEHIND the rabbit
// because the rabbit is in front of it. An ImGui overlay would have been far
// less code and would have floated on the glass with no depth at all, which in
// a tool whose entire job is judging where things are in space is the wrong
// trade. v2 says the same thing about the pause room's labels: "a place the
// player is standing in, and text pasted on the glass is not in it".
//
// v1'S NUMBERS, IN VOXELS. Both engines are 10 cm voxels and the rabbit is the
// same size in both, so the proportions carry over unchanged.
// ---------------------------------------------------------------------------
// -- ...BUT NOT v1'S SIZES, AND THE RENDER IS WHY ---------------------------
//
// v1 uses GIZ_LEN 13 and a ring pad of 4. Built to those numbers and
// photographed, the handles BURIED THE RABBIT: an arrow 1.3 m long off a body
// half a metre tall, and a ring 1.8 m across with the subject a pale speck in
// the middle of it. v1's exhibits are birches, flamingos and armadillos, and
// 13 is a sensible handle on those; this deck's subject is the smallest animal
// either engine has. A handle that is twice the height of the thing it handles
// has stopped being a handle.
//
// The pick slabs are deliberately NOT cut with them. Generous picking costs
// nothing on a deck with two objects on it, and a thin shaft you have to hit
// exactly is the one way a gizmo can feel broken while working perfectly.
inline constexpr int kGizLen = 9;     // how far an arrow reaches
inline constexpr int kGizGap = 2;     // ...starting this far off the model's face
inline constexpr int kGizHead = 3;    // the head's length, and its widest half-width is 2
inline constexpr int kGizPickVox = 3; // the pick slab's half-thickness across the shaft
inline constexpr int kRingPad = 3;    // the ring stands this far outside the model
inline constexpr int kRingPickVox = 3;   // ...and its slab is this thin through the plane
inline constexpr int kRingSteps = 96;    // points round the circle, before rounding to voxels

// HOW FAR A DRAG GOES. v1's, and both are a feel rather than a derivation:
// 0.05 voxels per pixel along the axis' own screen direction, and 55 pixels per
// quarter turn.
inline constexpr float kDragVoxPerPx = 0.05f;
inline constexpr float kDragPxPerQuarter = 55.0f;

enum GizmoKind { kGizNone = 0, kGizMove = 1, kGizRot = 2 };

// HOW MANY FLYER SLOTS THE HANDLES BORROW, after the one the subject is in.
// Three arrows is the most either gizmo needs. They come out of the BUNNY band
// (kBunnySlots is 10 and the population is suppressed on the deck -- see
// App::stageSubject), so the editor costs the structure nothing it did not
// already have.
inline constexpr int kGizSlots = 3;

// THE HEADING THE SUBJECT IS STAGED AT. Zero looks down -Z in this engine and
// the deck puts the camera on the +Z side looking back, so the rabbit arrives
// face on. It is named rather than written as a literal because the axis
// mapping below depends on it -- see bakeAxis.
inline constexpr float kEditHeading = 0.0f;

// What the crosshair is on. The order is the pick order: a gizmo in front of
// the model is a gizmo, not a click on the model behind it.
enum GizHit { kHitNone = -1, kHitX = 0, kHitY = 1, kHitZ = 2, kHitYaw = 3, kHitPitch = 4,
              kHitBody = 5 };

// -- A BOX, AND A RAY AGAINST IT --------------------------------------------
// The slab test v1 runs three times over (arrows, rings, body). `tmin` starts
// just off the eye so a box the camera is standing inside still answers.
struct GizBox {
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    bool hit(const Vec3 &o, const Vec3 &d, float *t) const {
        float tmin = 0.02f, tmax = 1e4f;
        const float od[3] = {o.x, o.y, o.z}, dd[3] = {d.x, d.y, d.z};
        for (int a = 0; a < 3; ++a) {
            const float inv = 1.0f / (fabsf(dd[a]) < 1e-9f ? 1e-9f : dd[a]);
            float ta = (lo[a] - od[a]) * inv, tb = (hi[a] - od[a]) * inv;
            if (ta > tb) { const float sw = ta; ta = tb; tb = sw; }
            tmin = maxf(tmin, ta);
            tmax = minf(tmax, tb);
            if (tmin > tmax) return false;
        }
        *t = tmin;
        return true;
    }
};

// ---------------------------------------------------------------------------
// WHAT STANDS ON THE DECK.
//
// "put the porcupine on the asset editor in the middle and remove the bunny
// from it." (user 2026-09-14.) ONE ROW PER SUBJECT, and this is the whole of
// the answer to that -- [B] cycles THIS list, enter() takes a working copy of
// exactly these, [C] exports exactly these, and the first row is what you are
// standing in front of when the deck comes up.
//
// IT IS NOT `kBunnyStrips`, AND THAT DISTINCTION IS THE POINT. The rabbit's
// three strips are still in bunnies.h and still compiled, because THE WILD
// RABBITS ARE DRAWN FROM THEM -- removing them from the enum would take the
// hop out of the wood, which is not what was asked for. What the deck offers
// and what the animal library contains are two different lists, and until now
// there was only one of them.
//
// SO ADDING A SUBJECT IS ONE ROW HERE plus its strip in bunnies.h's enum, its
// bake table between the PASTE markers, and the two lines in bakeSource that
// name that table -- which the static_assert below will not let you forget.
// -----------------------------------------------------------------------
inline constexpr int kEditSubjects[] = {kStripPorcupine};
inline constexpr int kEditSubjectCount =
    int(sizeof(kEditSubjects) / sizeof(kEditSubjects[0]));

// ---------------------------------------------------------------------------
class AssetEdit {
  public:
    void attach(Bunnies *b) { buns_ = b; }

    bool on() const { return on_; }

    // -----------------------------------------------------------------------
    // ONTO THE DECK. `at` is World::stageCentre() -- the middle, which is what
    // was asked for and also the only place on a square deck that needs no
    // explanation.
    //
    // THE WORKING COPY IS TAKEN ON EVERY ENTRY, from the compiled tables. So
    // leaving and coming back is a RESET, deliberately: the export is the only
    // way work leaves this tool, and a session that silently survived a trip to
    // the wood would make it easy to believe a bake had been committed when it
    // had only been typed. The console line printed on the way out is the
    // safety net -- see leave().
    // -----------------------------------------------------------------------
    void enter(World &world, const Vec3 &at) {
        buildGizmos(world);
        at_ = at;
        on_ = true;
        gizmo_ = kGizNone;
        drag_ = kHitNone;
        subj_ = 0;
        strip_ = kEditSubjects[0];
        sel_ = 0;
        axis_ = 2;
        playing_ = true;
        clock_ = 0.0f;
        // THE SUBJECTS, NOT EVERY STRIP. A working copy of the rabbit's tables
        // would be three vectors nothing on this deck can reach, and dirty()
        // would then compare them for ever against a bake nobody edited.
        for (int k = 0; k < kEditSubjectCount; ++k) {
            const int s = kEditSubjects[k];
            const int n = buns_ ? buns_->frames(s) : 0;
            bake_[s].assign(size_t(n), BunnyBake{0, 0, 0, 0, 0, 0});
            // THE TABLE'S OWN LENGTH BOUNDS THE READ, not the strip's. They
            // agree today and the paste target is a fixed-size array -- so the
            // day somebody adds a twelfth .vox to the folder and forgets the
            // row, this leaves the extra slot at identity instead of reading
            // off the end of a constexpr array.
            const int m = mini(n, bunnyBakeCount(s));
            for (int i = 0; i < m; ++i) bake_[s][size_t(i)] = bunnyBake(s)[i];
            for (int i = m; i < n; ++i) bake_[s][size_t(i)].src = i;
        }
        std::printf("v2: asset editor -- %s, %d frames.  , . frame   <- -> reorder   "
                    "R/V turn   ^ v nudge   G axis   B strip   K play   C copy   N reset\n",
                    bunnyStripName(strip_), frameCount());
        std::fflush(stdout);
    }

    // -----------------------------------------------------------------------
    // OFF THE DECK, AND THE BAKE GOES TO THE CONSOLE ON THE WAY OUT.
    //
    // v1 autosaves every nudge into localStorage and its note is blunt about
    // why -- alignment work is slow and there is nothing else it is written
    // down in. This has no localStorage and deliberately no file: a tool that
    // writes into the asset tree is a tool that can disagree with the source
    // you are about to paste into. Printing the table instead costs one line of
    // console and means an hour of nudging cannot be lost to a stray [I].
    // -----------------------------------------------------------------------
    void leave() {
        if (on_ && dirty()) {
            std::printf("v2: asset editor -- unexported edits, here they are:\n%s",
                        bakeSource().c_str());
            std::fflush(stdout);
        }
        on_ = false;
    }

    // -----------------------------------------------------------------------
    // THE EDITOR OWNS THE KEYBOARD WHILE IT IS UP, which is v1's rule word for
    // word ("the asset editor owns these two keys while it is up") and the only
    // rule that works: a key cannot mean two things at once in one mode. So R
    // turns a frame here rather than starting a recording, and the arrows move
    // frames rather than scrubbing the clock. Both get their meanings back the
    // moment you step off the deck.
    //
    // Returns true when the press was the editor's, so app.h can stop.
    // -----------------------------------------------------------------------
    bool key(const Falcor::KeyboardEvent &e) {
        if (!on_ || !buns_) return false;
        const bool shift = e.hasModifier(Input::Modifier::Shift);
        // A STRIP CAN BE EMPTY AND [B] HAS TO STILL WORK. loadStrip gives up on
        // the whole strip when one of its eleven files is missing, so the turns
        // can be absent while the hop is there -- and a tool that swallowed
        // every key on an empty strip would leave you standing on one with no
        // way off it, and with [R] silently starting a recording instead.
        const bool any = frameCount() > 0;
        switch (e.key) {
            case Input::Key::Comma: if (any) step(-1); return true;
            case Input::Key::Period: if (any) step(1); return true;
            case Input::Key::Left: if (any) reorder(-1); return true;
            case Input::Key::Right: if (any) reorder(1); return true;
            case Input::Key::Up: if (any) nudge(1); return true;
            case Input::Key::Down: if (any) nudge(-1); return true;
            // -- [E] THE MOVE ARROWS, [R] THE ROTATION RINGS ---------------
            //
            // v1's two keys and v1's two rules: each turns the other off, and
            // [R] works only on something already selected -- its note says
            // why, "an R press with nothing selected records instead (never
            // auto-selects)", which in v2 means a stray R on the deck must not
            // silently swallow the recorder's key AND do nothing visible.
            //
            // Both PAUSE, because a handle on a moving model cannot be aimed
            // at. That is the same identity click() relies on: selected is
            // paused.
            case Input::Key::E:
                if (!any) return true;
                pause();
                gizmo_ = (gizmo_ == kGizMove) ? kGizNone : kGizMove;
                drag_ = kHitNone;
                say(gizmo_ == kGizMove ? "move arrows -- drag one" : "handles off");
                return true;
            case Input::Key::R:
                if (!any || playing_) return false;   // nothing selected: let the recorder have it
                gizmo_ = (gizmo_ == kGizRot) ? kGizMove : kGizRot;
                drag_ = kHitNone;
                say(gizmo_ == kGizRot ? "rotation rings -- drag one" : "move arrows -- drag one");
                return true;
            // ...AND ONE QUARTER TURN WITHOUT THE RING. v1 has no keyboard
            // rotate at all, and a ring is the better tool for finding an
            // orientation -- but it costs kDragPxPerQuarter of travel per step,
            // so when you already know the answer is "a quarter the other way"
            // this is the shorter road to it.
            // V yaws, SHIFT+V pitches. ONE DIRECTION EACH is enough: a
            // quarter turn wraps at four, so the way back is three more
            // presses and a second binding buys nothing.
            case Input::Key::V: if (any) turn(shift ? 0 : 1, 1); return true;
            case Input::Key::G:
                if (!any) return true;
                axis_ = (axis_ + 1) % 3;
                say("nudge axis %c", "XYZ"[axis_]);
                return true;
            case Input::Key::B:
                // ROUND THE SUBJECT LIST, which is one strip today -- so this
                // is a no-op that still SAYS what you are looking at, and
                // becomes a cycle again the moment a second row is added.
                subj_ = (subj_ + 1) % kEditSubjectCount;
                strip_ = kEditSubjects[subj_];
                sel_ = 0;
                clock_ = 0.0f;
                say("%s -- %d frames", bunnyStripName(strip_), frameCount());
                return true;
            case Input::Key::K:
                if (!any) return true;
                playing_ = !playing_;
                say(playing_ ? "playing" : "paused on frame %d", sel_);
                return true;
            case Input::Key::C: copyOut(); return true;
            case Input::Key::N: clearStrip(); return true;
            default: return false;
        }
    }

    // The strip runs unless something paused it. Every tool pauses it, exactly
    // as v1's do ("scrubbing pauses the animation"): a frame you are aligning
    // that slides out from under you a twelfth of a second later is not a frame
    // you can align.
    void update(float dt) {
        if (!on_ || !playing_) return;
        const int n = frameCount();
        if (n <= 0) return;
        clock_ += dt * kEditFps;
        while (clock_ >= float(n)) clock_ -= float(n);
        sel_ = maxi(0, mini(n - 1, int(clock_)));
    }

    // ONE INSTANCE, through the population's own pose(). Facing 0, which in
    // this engine looks down -Z -- and the deck puts the camera on the +Z side
    // looking back, so the subject arrives face on rather than showing you its
    // tail. Fade 1: nothing is materialising here.
    void publish(World &world, int slot) {
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        auto hide = [&](int k) {
            world.setFlyerInstance(slot + k, 0, nullptr, 0, 0, 0, nullptr, false);
        };
        if (!on_ || !buns_ || frameCount() == 0) {
            for (int k = 0; k <= kGizSlots; ++k) hide(k);
            world.flushFlyerInstances();
            return;
        }
        const BunnyPose p = buns_->pose(strip_, sel_, 0.0f, 1.0f, at_, bake_[strip_].data());
        if (p.model < 0) {
            for (int k = 0; k <= kGizSlots; ++k) hide(k);
            world.flushFlyerInstances();
            return;
        }
        world.setFlyerInstance(slot, p.model, p.m, p.tx, p.ty, p.tz, nullptr, true);

        // -- THE HANDLES HANG OFF THE POSE'S OWN BOX ----------------------
        //
        // ONE MEASUREMENT, USED TWICE: the same numbers place the arrow and
        // size the box that picks it. v1 learned this the hard way -- its pick
        // box was derived separately from the frame's dims, so the moment a
        // bake made the frog march the drawn gizmo and the clickable one parted
        // company and "clicking the frog stopped selecting it". Here they come
        // out of the same loop or they come out of neither.
        layout(p);
        int k = 1;
        // HANDLES ONLY WHILE IT IS HELD STILL. v1 draws its ring and gizmos
        // inside `if (ED.paused)` and the reason is the same one that makes
        // selected and paused one state: a handle on a model that is animating
        // under it cannot be aimed at, so drawing it would only invite a click
        // that lands on the body behind it.
        if (playing_) gizmo_ = kGizNone;
        if (gizmo_ == kGizMove) {
            for (int a = 0; a < 3; ++a, ++k)
                if (arrow_[a] >= 0)
                    world.setFlyerInstance(slot + k, arrow_[a], kI, gizAt_[a][0], gizAt_[a][1],
                                           gizAt_[a][2], nullptr, true);
                else
                    hide(k);
        } else if (gizmo_ == kGizRot) {
            for (int r = 0; r < 2; ++r, ++k)
                if (ring_[r] >= 0)
                    world.setFlyerInstance(slot + k, ring_[r], kI, gizAt_[3 + r][0],
                                           gizAt_[3 + r][1], gizAt_[3 + r][2], nullptr, true);
                else
                    hide(k);
        }
        for (; k <= kGizSlots; ++k) hide(k);
        world.flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // WHERE EVERY HANDLE IS, AND WHAT PICKING IT MEANS.
    //
    // setFlyerInstance puts the model's OWN ORIGIN at the translation, so each
    // arrow is placed by its tail and centred across; the rings by their corner
    // so their middles land on the model's.
    // -----------------------------------------------------------------------
    void layout(const BunnyPose &p) {
        const float c[3] = {0.5f * (p.lo[0] + p.hi[0]), 0.5f * (p.lo[1] + p.hi[1]),
                            0.5f * (p.lo[2] + p.hi[2])};
        c_[0] = c[0];
        c_[1] = c[1];
        c_[2] = c[2];
        body_.lo[0] = p.lo[0]; body_.lo[1] = p.lo[1]; body_.lo[2] = p.lo[2];
        body_.hi[0] = p.hi[0]; body_.hi[1] = p.hi[1]; body_.hi[2] = p.hi[2];

        const float gap = float(kGizGap) * VOXEL_M;
        const float pick = float(kGizPickVox) * VOXEL_M;
        const float slop = VOXEL_M;   // v1's one voxel of give at each end
        for (int a = 0; a < 3; ++a) {
            const float tail = p.hi[a] + gap;
            const float len = float(arrowDim_[a][a]) * VOXEL_M;
            for (int r = 0; r < 3; ++r) {
                const float half = 0.5f * float(arrowDim_[a][r]) * VOXEL_M;
                gizAt_[a][r] = (r == a) ? tail : (c[r] - half);
                arrowBox_[a].lo[r] = (r == a) ? (tail - slop) : (c[r] - pick);
                arrowBox_[a].hi[r] = (r == a) ? (tail + len + slop) : (c[r] + pick);
            }
        }
        const float rr = float(ringR_ + 1) * VOXEL_M;
        const float rpick = float(kRingPickVox) * VOXEL_M;
        for (int r = 0; r < 2; ++r) {
            const int thin = (r == 0) ? 1 : 0;   // yaw is flat in Y, pitch upright through X
            for (int q = 0; q < 3; ++q) {
                gizAt_[3 + r][q] = c[q] - 0.5f * float(ringDim_[r][q]) * VOXEL_M;
                const float h = (q == thin) ? rpick : rr;
                ringBox_[r].lo[q] = c[q] - h;
                ringBox_[r].hi[q] = c[q] + h;
            }
        }
    }

    // -----------------------------------------------------------------------
    // A LEFT CLICK ON THE DECK, which is v1's whole selection model:
    //
    //   "left-click the stamped cardinal to SELECT it (pause) / click again to
    //    resume; then , . scrub the frames"
    //
    // SELECTED AND PAUSED ARE THE SAME STATE, and that is not a shortcut. A
    // handle hanging off a model that is moving under it cannot be aimed at,
    // and a frame you are aligning has to hold still to be aligned -- so there
    // is no useful state where one is true and the other is not, and two flags
    // would only be two ways to describe the same thing wrongly.
    //
    // THE ARROWS COME UP WITH THE SELECTION (user 2026-09-13: "you left click
    // the object to select it. then, arrow pop up to move the object"). v1
    // wants an [E] after the click before its move gizmo appears; one press is
    // better and nothing is lost, since [E] still toggles them off and on.
    //
    // THE PICK ORDER IS HANDLES FIRST, and it has to be: the arrows stand
    // outside the model but a ring passes in FRONT of it from most angles, and
    // a click that fell through to the body would deselect the thing you were
    // trying to turn.
    // -----------------------------------------------------------------------
    bool click(const Vec3 &o, const Vec3 &d) {
        if (!on_ || !buns_ || frameCount() == 0) return false;
        float best = 1e30f, t = 0.0f;
        int hit = kHitNone;
        if (!playing_ && gizmo_ == kGizMove)
            for (int a = 0; a < 3; ++a)
                if (arrow_[a] >= 0 && arrowBox_[a].hit(o, d, &t) && t < best) {
                    best = t;
                    hit = a;
                }
        // -- A RING IS GRABBED ON THE RING, NOT ANYWHERE IN ITS DISC -------
        //
        // The slab a ring picks with is a PLATE through the model's waist, so
        // it contains the model. v1 stops there and lives with the result:
        // with its rings up, a click on the body grabs the ring instead and
        // there is no way to deselect by clicking. One more test fixes it --
        // the hit has to land near the ring's own radius -- and it costs a
        // square root that runs once per click.
        if (!playing_ && gizmo_ == kGizRot)
            for (int r = 0; r < 2; ++r) {
                if (ring_[r] < 0 || !ringBox_[r].hit(o, d, &t) || t >= best) continue;
                const float hp[3] = {o.x + d.x * t, o.y + d.y * t, o.z + d.z * t};
                // The ring's two in-plane axes: yaw lies in XZ, pitch in YZ.
                const int u = (r == 0) ? 0 : 1, v = 2;
                const float du = hp[u] - c_[u], dv = hp[v] - c_[v];
                const float rad = sqrtf(du * du + dv * dv);
                if (fabsf(rad - float(ringR_) * VOXEL_M) > float(kRingPickVox) * VOXEL_M)
                    continue;
                best = t;
                hit = kHitYaw + r;
            }
        if (hit != kHitNone) {
            drag_ = hit;
            acc_ = 0.0f;
            return true;
        }
        // ...and otherwise the body, which TOGGLES. An AABB rather than the
        // voxels: v1 notes that an exact-voxel ray slips through the gaps of a
        // small sparse model, and a rabbit's ears are exactly that.
        if (!body_.hit(o, d, &t)) return false;
        select(playing_);
        return true;
    }

    void select(bool on) {
        playing_ = !on;
        gizmo_ = on ? kGizMove : kGizNone;
        drag_ = kHitNone;
        if (on) {
            clock_ = float(sel_);
            say("selected -- drag an arrow to move it, [R] to turn it");
        } else {
            say("playing");
        }
    }

    bool dragging() const { return on_ && drag_ != kHitNone; }

    // SELECT IT WITHOUT A MOUSE -- what --gizmo drives. The same two lines the
    // click and the [R] key take, so a capture taken this way is a capture of
    // what a hand would have produced.
    void pick(int kind) {
        if (!on_ || frameCount() == 0) return;
        select(true);
        gizmo_ = (kind == 2) ? kGizRot : kGizMove;
    }

    // -----------------------------------------------------------------------
    // THE DRAG ITSELF. `rx`/`ry` are the frame's mouse delta in pixels, +right
    // and +UP -- the same pair applyMouseLook would have turned into a look.
    //
    // AN ARROW IS DRAGGED ALONG ITS OWN SCREEN DIRECTION, not along the
    // camera's. That is v1's rule and it is the difference between a handle
    // that follows the pointer and one that fights it: the red arrow may be
    // running up-left across the screen from where you stand, so pulling
    // up-left has to be what moves it, whatever the camera's own axes are.
    // Project the world axis onto screen-right and screen-up, and the dot of
    // that with the mouse delta is the travel.
    //
    // A RING IS SIMPLER because what it drives is quantised anyway: yaw off
    // horizontal travel, pitch off vertical, one quarter turn per
    // kDragPxPerQuarter of it. v1 does exactly this.
    //
    // THE ACCUMULATOR IS DRAINED IN A LOOP RATHER THAN ROUNDED. A fast flick
    // is several voxels in one frame's delta, and a version that applied at
    // most one step per event would quietly lag behind the pointer and then
    // catch up when you stopped.
    // -----------------------------------------------------------------------
    void dragBy(float rx, float ry, float yawDeg, float pitchDeg) {
        if (!on_ || drag_ == kHitNone) return;
        if (drag_ == kHitYaw || drag_ == kHitPitch) {
            acc_ += (drag_ == kHitYaw) ? rx : ry;
            while (acc_ >= kDragPxPerQuarter) {
                turn(drag_ == kHitYaw ? 1 : 0, 1);
                acc_ -= kDragPxPerQuarter;
            }
            while (acc_ <= -kDragPxPerQuarter) {
                turn(drag_ == kHitYaw ? 1 : 0, -1);
                acc_ += kDragPxPerQuarter;
            }
            return;
        }
        const Vec3 f = Camera::direction(yawDeg, pitchDeg);
        const Vec3 rgt = normalize(cross(f, Vec3(0.0f, 1.0f, 0.0f)));
        const Vec3 up = cross(rgt, f);
        const int a = drag_;
        const float ar = (a == 0) ? rgt.x : (a == 1) ? rgt.y : rgt.z;
        const float au = (a == 0) ? up.x : (a == 1) ? up.y : up.z;
        acc_ += (rx * ar + ry * au) * kDragVoxPerPx;
        while (acc_ >= 1.0f) {
            nudgeAxis(a, 1);
            acc_ -= 1.0f;
        }
        while (acc_ <= -1.0f) {
            nudgeAxis(a, -1);
            acc_ += 1.0f;
        }
    }

    // LET GO, AND SAY WHERE IT ENDED UP. v1 autosaves here; this has nothing to
    // save to (see leave()), so what the release buys is the one line of
    // feedback that tells you the drag landed where you meant it to.
    void release() {
        if (drag_ == kHitNone) return;
        const bool moved = drag_ <= kHitZ;
        drag_ = kHitNone;
        acc_ = 0.0f;
        if (frameCount() <= 0) return;
        const BunnyBake &b = bake_[strip_][size_t(sel_)];
        if (moved)
            say("frame %d at %+d %+d %+d vox", sel_ + 1, b.ox, b.oy, b.oz);
        else
            say("frame %d at yaw %d  pitch %d", sel_ + 1, ((b.yaw % 4) + 4) % 4,
                ((b.pitch % 4) + 4) % 4);
    }

    // -----------------------------------------------------------------------
    // THE EXPORT, AND IT IS C++ RATHER THAN v1'S JSON.
    //
    // v1 copies `{"bunny_jump":[{"frame":0,"ox":0,...}]}` and somebody turns
    // that into a BUNNY_JUMP_BAKE by hand. There is no reason for that step
    // here: the destination is a C++ table in a header this tool can see, so
    // what goes on the clipboard is the table itself, formatted the way the
    // file already formats it. Paste replaces the rows between the two markers
    // in bunnies.h and the wood has the change.
    //
    // ALL THREE STRIPS, ALWAYS -- v1 exports both its lanes at once for the
    // same reason: alignment on a turn is usually done against the hop it
    // hands over to, and an export that carried only the strip you were
    // looking at would make you do the trip twice to keep them in step.
    // -----------------------------------------------------------------------
    std::string bakeSource() const {
        // Indexed by STRIP, so a row here can never drift from the enum -- the
        // export names the variable you paste over, and naming the wrong one
        // is a paste that compiles and silently re-bakes a different animal.
        static const char *kVar[kBunnyStrips] = {"kBunnyHopBake", "kBunnyTurnLBake",
                                                 "kBunnyTurnRBake", "kPorcupineWalkBake"};
        static const char *kCount[kBunnyStrips] = {"kBunnyJumpFrames", "kBunnyTurnFrames",
                                                   "kBunnyTurnFrames", "kPorcupineFrames"};
        std::string s;
        // ONLY WHAT THE DECK EDITS. Exporting the rabbit's three as well would
        // hand you a paste that reverts any bake committed since this build --
        // [C] would quietly undo somebody else's work.
        for (int k = 0; k < kEditSubjectCount; ++k) {
            const int st = kEditSubjects[k];
            char head[200];
            std::snprintf(head, sizeof(head), "inline constexpr BunnyBake %s[%s] = {\n", kVar[st],
                          kCount[st]);
            s += head;
            // A STRIP THAT DID NOT LOAD EXPORTS THE TABLE IT ALREADY HAS, not
            // an empty brace list. The paste target is a fixed-size array, so
            // `= {};` is legal C++ that zero-fills it -- which would silently
            // throw away a committed bake because one .vox happened to be
            // missing from a folder the day somebody pressed [C].
            const std::vector<BunnyBake> committed(bunnyBake(st),
                                                   bunnyBake(st) + bunnyBakeCount(st));
            const std::vector<BunnyBake> &b = bake_[st].empty() ? committed : bake_[st];
            for (size_t i = 0; i < b.size(); ++i) {
                char row[160];
                // Four rows a line, which is what the file already does, so a
                // paste over the identity table produces a diff of the numbers
                // that changed rather than of the whole block.
                std::snprintf(row, sizeof(row), "%s{%d, %d, %d, %d, %d, %d},%s",
                              (i % 4 == 0) ? "    " : "  ", b[i].src, b[i].ox, b[i].oy, b[i].oz,
                              b[i].yaw, b[i].pitch, (i % 4 == 3 || i + 1 == b.size()) ? "\n" : "");
                s += row;
            }
            s += "};\n";
        }
        return s;
    }

    // Has anything been said that the compiled tables do not already say?
    // The subjects only -- the strips this deck does not offer have no working
    // copy to differ from, and enter() leaves those vectors empty.
    bool dirty() const {
        for (int k = 0; k < kEditSubjectCount; ++k) {
            const int st = kEditSubjects[k];
            const std::vector<BunnyBake> &b = bake_[st];
            const size_t lim = mini(int(b.size()), bunnyBakeCount(st));
            for (size_t i = 0; i < lim; ++i) {
                const BunnyBake &c = bunnyBake(st)[i];
                if (b[i].src != c.src || b[i].ox != c.ox || b[i].oy != c.oy || b[i].oz != c.oz ||
                    b[i].yaw != c.yaw || b[i].pitch != c.pitch)
                    return true;
            }
        }
        return false;
    }

    // -- WHAT THE PANEL SAYS ------------------------------------------------
    // Built here rather than in app.h's onGuiRender because every number on it
    // is this class's state, and a readout assembled next to the widget is a
    // readout that drifts from what the tool is actually holding.
    void hudLines(std::vector<std::string> *out) const {
        out->clear();
        if (!on_ || !buns_) return;
        const int n = frameCount();
        char buf[200];
        std::snprintf(buf, sizeof(buf), "%s   frame %d/%d%s", bunnyStripName(strip_),
                      n ? sel_ + 1 : 0, n, playing_ ? "" : "   PAUSED");
        out->push_back(buf);
        if (n <= 0) {
            out->push_back("no frames loaded");
            return;
        }
        const BunnyBake &b = bake_[strip_][size_t(sel_)];
        std::snprintf(buf, sizeof(buf), "plays %02d.vox   %+d %+d %+d vox   yaw %d  pitch %d",
                      b.src, b.ox, b.oy, b.oz, ((b.yaw % 4) + 4) % 4, ((b.pitch % 4) + 4) % 4);
        out->push_back(buf);
        std::snprintf(buf, sizeof(buf), "%s   nudge axis %c%s",
                      playing_        ? "click it to select"
                      : gizmo_ == kGizRot ? "ROTATION RINGS  [E] arrows"
                      : gizmo_ == kGizMove ? "MOVE ARROWS  [R] rings"
                                           : "selected  [E] arrows  [R] rings",
                      "XYZ"[axis_], dirty() ? "   * unexported" : "");
        out->push_back(buf);
        if (!msg_.empty()) out->push_back(msg_);
    }

    // The key list, for F1 and for the panel. One source for both, so the help
    // cannot go on describing a binding after it has moved.
    static const char *const *help(int *n) {
        static const char *const kRows[] = {
            "  LEFT CLICK IT         select the porcupine -- the move arrows come up",
            "  drag an arrow         slide this frame along that axis",
            "  R                     swap to the rotation rings; drag one to turn it",
            "  E                     back to the move arrows / put them away",
            "  click it again        deselect, and the strip plays on",
            "  ,  .                  previous / next frame",
            "  left / right          reorder: this frame earlier / later",
            "  up / down             nudge one voxel -- G picks the axis",
            "  V / SHIFT+V           one quarter turn: yaw / pitch",
            "  B                     next subject -- the porcupine is the only one",
            "  C                     COPY THE BAKE to the clipboard, as C++",
            "  N                     throw this strip's edits away",
        };
        *n = int(sizeof(kRows) / sizeof(kRows[0]));
        return kRows;
    }

  private:
    // -----------------------------------------------------------------------
    // THE GIZMO GEOMETRY, BUILT ONCE.
    //
    // In code rather than authored, for the reason the pause room's shell is:
    // these are fixtures of the interface and nobody will re-author them, so
    // there is nothing for a .vox file to be the source of truth about.
    //
    // FIVE MODELS RATHER THAN TWO AND A ROTATION. addFlyerModel bakes the
    // palette into the mesh, so three colours is three models whatever else is
    // shared -- and once a red arrow and a green one are separate meshes
    // anyway, building each one already pointing down its own axis costs
    // nothing and removes the orientation from the placement entirely. Every
    // gizmo instance is then a translation and an identity 3x3. The room's
    // three buttons are one file and three palettes for the same reason.
    //
    // LAZY, on the first [I], because a stage nobody opens should not have
    // paid for five BLASes.
    // -----------------------------------------------------------------------
    void buildGizmos(World &world) {
        if (gizBuilt_ || !buns_) return;
        gizBuilt_ = true;

        // v1's colours, unchanged: X red, Y green, Z blue, yaw amber, pitch
        // violet. They are the colours every 3D tool uses for these handles,
        // which is the whole argument for them.
        static const uint8_t kArrowRgb[3][3] = {{240, 60, 55}, {70, 210, 70}, {80, 130, 255}};
        static const uint8_t kRingRgb[2][3] = {{255, 190, 40}, {180, 90, 255}};

        for (int a = 0; a < 3; ++a) {
            VoxModel mo = arrowModel(a, kArrowRgb[a]);
            int sx = 0, sy = 0, sz = 0;
            arrow_[a] = world.addFlyerModel(mo, "gizmo arrow", &sx, &sy, &sz);
            arrowDim_[a][0] = sx;
            arrowDim_[a][1] = sy;
            arrowDim_[a][2] = sz;
        }
        // THE RING IS SIZED ONCE, OFF THE BIGGEST FRAME OF ANY SUBJECT, and it
        // does NOT follow the pose. v1 rebuilds its rings every layout from the
        // current frame's footprint, which it can because they are stamped
        // voxels; here a per-frame radius is a per-frame BLAS. The fixed radius
        // is also the better tool: a handle that changes size as the animation
        // plays under it is a handle you have to re-find.
        //
        // THE SUBJECTS, NOT EVERY STRIP -- a ring sized round the rabbit would
        // be a ring sized round an animal that is not on this deck. The
        // porcupine is the shorter of the two, so measuring the rabbit would
        // leave the handles floating well clear of the body.
        int big = 1;
        for (int k = 0; k < kEditSubjectCount; ++k) {
            const int st = kEditSubjects[k];
            for (int i = 0; i < buns_->frames(st); ++i) {
                const BunnyPose q = buns_->pose(st, i, 0.0f, 1.0f, Vec3(0, 0, 0), nullptr);
                for (int r = 0; r < 3; ++r)
                    big = maxi(big, int((q.hi[r] - q.lo[r]) / VOXEL_M + 0.5f));
            }
        }
        ringR_ = (big >> 1) + kRingPad;
        for (int r = 0; r < 2; ++r) {
            VoxModel mo = ringModel(r, ringR_, kRingRgb[r]);
            int sx = 0, sy = 0, sz = 0;
            ring_[r] = world.addFlyerModel(mo, "gizmo ring", &sx, &sy, &sz);
            ringDim_[r][0] = sx;
            ringDim_[r][1] = sy;
            ringDim_[r][2] = sz;
        }
        std::printf("  gizmo    3 arrows %d long, 2 rings r=%d\n", kGizLen, ringR_);
    }

    // -- ONE ARROW, POINTING DOWN WORLD AXIS `ax` --------------------------
    //
    // A .vox model is authored z-UP and toWorldWhole swaps z into world y (see
    // scene/vox.h), so everything here is written in WORLD terms and put
    // through one helper that does the swap. Getting that wrong is how a
    // green arrow ends up lying on its side, and it is worth exactly one
    // function rather than three chances to make the mistake.
    static VoxModel arrowModel(int ax, const uint8_t *rgb) {
        // Along the axis: kGizLen. Across it: the head's widest half-width is
        // kGizHead - 1, so five voxels covers it.
        const int wide = 2 * (kGizHead - 1) + 1;
        int dim[3] = {wide, wide, wide};
        dim[ax] = kGizLen;
        VoxModel mo = blank(dim, rgb);
        const int c = wide / 2;
        for (int i = 0; i < kGizLen; ++i) {
            // v1's head: the tip is one voxel and it widens back to the head's
            // base. r is 0 down the shaft, which is what makes the shaft one
            // voxel thick.
            const int tip = kGizLen - 1 - i;
            const int r = tip < kGizHead ? tip : 0;
            for (int a = -r; a <= r; ++a)
                for (int b = -r; b <= r; ++b) {
                    // DIAMOND, not a square: the corners of each slice are cut
                    // out, which is what makes it read as a cone rather than a
                    // stack of blocks.
                    if (abs(a) + abs(b) > r) continue;
                    int w[3];
                    w[ax] = i;
                    w[(ax + 1) % 3] = c + a;
                    w[(ax + 2) % 3] = c + b;
                    setWorld(&mo, dim, w[0], w[1], w[2]);
                }
        }
        return mo;
    }

    // -- ONE RING, IN THE PLANE THE ANGLE TURNS IN -------------------------
    // kind 0 = yaw: flat, in world XZ, so dragging it spins the model about
    // the vertical. kind 1 = pitch: upright in world YZ, about the horizontal
    // X -- which is the axis bunnyQuarter's pitch actually uses.
    static VoxModel ringModel(int kind, int R, const uint8_t *rgb) {
        const int d = 2 * R + 1;
        int dim[3] = {d, d, d};
        dim[kind == 0 ? 1 : 0] = 1;   // one voxel thick through its own plane
        VoxModel mo = blank(dim, rgb);
        for (int i = 0; i < kRingSteps; ++i) {
            const float a = float(i) / float(kRingSteps) * 6.28318531f;
            const int u = int(roundf(cosf(a) * float(R)));
            const int v = int(roundf(sinf(a) * float(R)));
            if (kind == 0)
                setWorld(&mo, dim, R + u, 0, R + v);   // XZ
            else
                setWorld(&mo, dim, 0, R + u, R + v);   // YZ
        }
        return mo;
    }

    // A model of world size dim[], one palette entry, nothing in it yet.
    static VoxModel blank(const int *dim, const uint8_t *rgb) {
        VoxModel mo;
        mo.sx = dim[0];
        mo.sy = dim[2];   // world z is the model's y
        mo.sz = dim[1];   // world y is the model's z
        mo.m.assign(size_t(mo.sx) * size_t(mo.sy) * size_t(mo.sz), 0);
        mo.pal[0] = {rgb[0], rgb[1], rgb[2], 255};
        return mo;
    }
    static void setWorld(VoxModel *mo, const int *dim, int wx, int wy, int wz) {
        if (wx < 0 || wy < 0 || wz < 0 || wx >= dim[0] || wy >= dim[1] || wz >= dim[2]) return;
        mo->m[size_t(wx) + size_t(wz) * size_t(mo->sx) +
              size_t(wy) * size_t(mo->sx) * size_t(mo->sy)] = 1;
    }

    int frameCount() const { return buns_ ? buns_->frames(strip_) : 0; }

    void pause() { playing_ = false; }

    void step(int d) {
        const int n = frameCount();
        if (n <= 0) return;
        pause();
        sel_ = ((sel_ + d) % n + n) % n;
        clock_ = float(sel_);
        msg_.clear();
    }

    // -- REORDER: SWAP TWO SLOTS' src, NOT TWO FRAMES ----------------------
    //
    // v1 swaps the frame OBJECTS in its array. It can: its frames are parsed
    // voxels living in the editor. Here the strip is eleven BLASes on the
    // device that the wood is also using, so what moves is the row that says
    // WHICH of them plays WHEN -- which is the same edit, expressed where it
    // can actually be baked. The selection follows the frame, as v1's does.
    void reorder(int d) {
        const int n = frameCount();
        if (n < 2) return;
        pause();
        const int j = sel_ + d;
        if (j < 0 || j >= n) return;   // no wrap: v1's edMoveStep refuses it too
        std::vector<BunnyBake> &b = bake_[strip_];
        const BunnyBake t = b[size_t(sel_)];
        b[size_t(sel_)] = b[size_t(j)];
        b[size_t(j)] = t;
        sel_ = j;
        clock_ = float(sel_);
        say("frame %d now plays %02d.vox", sel_ + 1, b[size_t(sel_)].src);
    }

    // -- NUDGE, AND THE TAIL OF THE STRIP COMES WITH IT --------------------
    //
    // This is v1's ED_FOLLOW = 1 and it is the setting that makes offsetting a
    // strip tractable. A bound is a body travelling: frame 5 is not two voxels
    // forward of the ORIGIN, it is two voxels forward of frame 4, and every
    // frame after it inherits that. Moving one frame alone means re-typing the
    // same delta into six more rows and re-checking all of them.
    //
    // ONLY THE FRAMES AFTER IT. v1 says why in one line -- "everything before
    // it is work you have already signed off".
    //
    // PER AXIS, WHICH IS THE ONE PLACE THIS DEPARTS FROM v1. Its follow assigns
    // the whole offset (`g.ox = f.ox; g.oy = f.oy; g.oz = f.oz`), so a nudge in
    // Y also flattens X and Z down the tail -- and a bound is built by laying
    // travel into oz frame by frame and THEN adding the rise, which that would
    // undo every time. Carrying only the axis you moved leaves the other two
    // where you put them.
    void nudge(int d) { nudgeAxis(axis_, d); }

    // -- A WORLD AXIS IS NOT A BAKE COMPONENT --------------------------------
    //
    // ox/oy/oz are in the ANIMAL's frame and the animal is turned to face you:
    // pose() composes them through H = Ry(heading + pi), so at the deck's
    // heading world +X is ox DECREASING and world +Z is oz decreasing. Drag the
    // red arrow without this and the rabbit goes the other way, which is the
    // kind of bug that reads as "the gizmo is inverted" and is really two
    // frames of reference being confused for one.
    //
    // Derived from H rather than written down as three signs, because H is
    // built from kEditHeading and a staged heading that ever changes must not
    // silently invert the handles. A quarter-turn heading makes H a signed
    // permutation, so exactly one component answers and the rest are zero.
    void nudgeAxis(int worldAxis, int d) {
        int comp3 = 0, sign = 0;
        bakeAxis(worldAxis, &comp3, &sign);
        if (!sign) return;
        nudgeComp(comp3, d * sign);
    }

    static void bakeAxis(int worldAxis, int *comp3, int *sign) {
        const float c = cosf(kEditHeading + 3.14159265f), sn = sinf(kEditHeading + 3.14159265f);
        const float H[9] = {c, 0.0f, sn, 0.0f, 1.0f, 0.0f, -sn, 0.0f, c};
        // H maps bake -> world, so its TRANSPOSE maps world -> bake: the column
        // of H that this world row reads from is the row of H^T we want.
        *comp3 = 0;
        *sign = 0;
        float best = 0.5f;   // anything below this is numerical dust, not an axis
        for (int k = 0; k < 3; ++k) {
            const float v = H[worldAxis * 3 + k];
            if (fabsf(v) <= best) continue;
            best = fabsf(v);
            *comp3 = k;
            *sign = (v > 0.0f) ? 1 : -1;
        }
    }

    void nudgeComp(int comp3, int d) {
        const int n = frameCount();
        if (n <= 0) return;
        pause();
        std::vector<BunnyBake> &b = bake_[strip_];
        int *f = comp(&b[size_t(sel_)], comp3);
        *f += d;
        for (int i = sel_ + 1; i < n; ++i) {
            *comp(&b[size_t(i)], comp3) = *f;
        }
        const BunnyBake &s = b[size_t(sel_)];
        say("%+d %+d %+d vox (and the %d after it)", s.ox, s.oy, s.oz, n - 1 - sel_);
    }

    // A quarter turn of THIS frame only. v1 is explicit that rotation is not
    // carried down the strip the way an offset is -- "[r] is per-frame by
    // nature, and a sequence whose frames are rotated to a common heading is
    // exactly what the bake tables are for".
    void turn(int axis, int d) {
        if (frameCount() <= 0) return;
        pause();
        BunnyBake &b = bake_[strip_][size_t(sel_)];
        int &q = axis ? b.yaw : b.pitch;
        q = ((q + d) % 4 + 4) % 4;
        say("%s %d quarter%s", axis ? "yaw" : "pitch", q, q == 1 ? "" : "s");
    }

    void clearStrip() {
        const int n = mini(frameCount(), bunnyBakeCount(strip_));
        for (int i = 0; i < n; ++i) bake_[strip_][size_t(i)] = bunnyBake(strip_)[i];
        say("%s back to the committed bake", bunnyStripName(strip_));
    }

    // -- THE CLIPBOARD, WHICH IS THE POINT OF THE WHOLE TOOL ---------------
    //
    // ...AND THE CONSOLE AS WELL, ALWAYS. OpenClipboard fails outright when
    // another process is holding it, and it does so by returning false rather
    // than by anything the user would see -- v1 hit the same class of problem
    // from the other side (navigator.clipboard REJECTS on an unfocused page,
    // "a bare try/catch would have reported success on the one path most likely
    // to fail"). Printing it too means the copy can fail and the work still be
    // in front of you.
    void copyOut() {
        const std::string src = bakeSource();
        const bool ok = toClipboard(src);
        std::printf("v2: asset editor -- bake %s:\n%s", ok ? "copied" : "NOT copied (clipboard busy)",
                    src.c_str());
        std::fflush(stdout);
        say(ok ? "copied -- paste it into bunnies.h" : "clipboard busy -- it is in the console");
    }

    static bool toClipboard(const std::string &s) {
        if (!OpenClipboard(nullptr)) return false;
        bool ok = false;
        if (EmptyClipboard()) {
            // +1 for the terminator: CF_TEXT is a NUL-terminated block, and a
            // handle sized to strlen() hands every reader whatever follows it.
            HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
            if (h) {
                if (void *p = GlobalLock(h)) {
                    std::memcpy(p, s.c_str(), s.size() + 1);
                    GlobalUnlock(h);
                    // OWNERSHIP PASSES TO THE CLIPBOARD on success, so the
                    // handle is freed here only when it does not.
                    ok = SetClipboardData(CF_TEXT, h) != nullptr;
                }
                if (!ok) GlobalFree(h);
            }
        }
        CloseClipboard();
        return ok;
    }

    static int *comp(BunnyBake *b, int axis) {
        return axis == 0 ? &b->ox : axis == 1 ? &b->oy : &b->oz;
    }

    template <typename... A>
    void say(const char *f, A... a) {
        char buf[200];
        std::snprintf(buf, sizeof(buf), f, a...);
        msg_ = buf;
    }
    void say(const char *f) { msg_ = f; }

    Bunnies *buns_ = nullptr;
    bool on_ = false;
    // -- the handles: five models, built once, and where they are this frame --
    bool gizBuilt_ = false;
    int arrow_[3] = {-1, -1, -1};
    int arrowDim_[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    int ring_[2] = {-1, -1};
    int ringDim_[2][3] = {{0, 0, 0}, {0, 0, 0}};
    int ringR_ = 8;
    int gizmo_ = kGizNone;
    int drag_ = kHitNone;
    float acc_ = 0.0f;
    float gizAt_[5][3] = {};
    float c_[3] = {0, 0, 0};   // the pose's middle, where the rings are hung
    GizBox arrowBox_[3], ringBox_[2], body_;
    Vec3 at_ = Vec3(0.0f, 0.0f, 0.0f);
    // WHICH SUBJECT, AND ITS STRIP. Two fields rather than one because [B]
    // steps round kEditSubjects while everything else indexes by strip -- and
    // deriving one from the other at every use is how a list with a hole in it
    // ends up editing a strip the deck does not offer.
    int subj_ = 0;
    int strip_ = kEditSubjects[0];
    int sel_ = 0;
    int axis_ = 2;            // Z, which is the one a bound travels along
    bool playing_ = true;
    float clock_ = 0.0f;
    std::string msg_;
    std::vector<BunnyBake> bake_[kBunnyStrips];
};

}  // namespace v2
