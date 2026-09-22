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
#include "core/vecmath.h"
#include "world/world.h"
#include "player/collide.h"
#include "player/bow.h"
#include "world/voxelworld.h"
#include "player/player.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <string>
#include <utility>
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
    // A SHOVEL, and the reason this enum is no longer only about sound.
    //
    // The note above was written when v2 had nothing to carve and the tool's
    // material survived purely as what it sounded like. The world has an inside
    // now, so this decides the BITE as well: what a tool takes is what comes
    // out of the ground when it lands, and the sound follows from the same
    // word rather than from a second table that could disagree with it.
    //
    // Soil is the loose ground -- grass, the soil under it, needle litter,
    // sand, silt. See isSoilMat in scene/voxelworld.h, which is where the list
    // lives so the swing and the audio cannot hold different opinions of it.
    Soil,     // a shovel
    // -- AND A HOE, WHICH DOES NOT BREAK ANYTHING AT ALL ------------------
    //
    // v1's line is the whole design: "the HOE does not chop -- it tills". It is
    // the first entry here that is not a BITE, so `toolTakes` answers false for
    // it against every swing -- correctly, because there is no material it
    // removes. What it does instead happens before the bite chain, beside the
    // wheat, which is the other thing a swing can spend itself on without
    // digging anything. See App::tillGround.
    Earth,    // a hoe
};

struct Tool {
    const char *name = "";
    Takes takes = Takes::Nothing;
    // IS IT ACTUALLY IN THE KIT. False while it is lying in the wood -- the
    // models and the pose stay loaded either way, because a dropped axe is the
    // same axe and picking it up must not cost a reload.
    bool carried = true;
    // -- HOW MANY OF IT (user 2026-09-14) ----------------------------------
    //
    // `carried` is the same fact for the first one and this is the rest of it.
    // Kept as a SECOND field rather than replacing the bool with `n > 0`,
    // because `carried` is read in eight places that mean "is this in the
    // wheel" and none of them wants to learn about counting.
    //
    // THE TWO MOVE TOGETHER AND ONLY give/take/stow MOVE THEM. A tool the
    // player has never dropped sits at 1, which is why the badge is hidden
    // below 2 -- "x1" beside an axe is noise.
    int stack = 1;
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
    // -- CAN THE RIGHT BUTTON BRING THIS UP TO THE EYE ---------------------
    //
    // (user 2026-09-17: "when right clicking, have the gun aim down sights".)
    //
    // THE SAME BUTTON THE BOW DRAWS WITH, and they cannot collide: `wantDraw`
    // asks holdingBow() and this asks `ads`, so a tool is one or the other or
    // neither. A bow being pulled IS the bow's version of this and does not
    // want a second pose on top of it.
    //
    // A SECOND POSE RATHER THAN AN OFFSET. The hip pose is a bake somebody
    // read off the live panel and the sighted pose is another one; expressing
    // the second as a delta from the first would mean re-tuning it every time
    // the first moved, which is exactly what the pose panel is for changing.
    bool ads = false;
    HeldPose adsPose;
    // -- IS THIS SOMETHING YOU CAN EAT ------------------------------------
    //
    // Its `models` are then a BITE STRIP -- kEatFrames of it being eaten down
    // to nothing -- and the right button runs them. See addFood.
    //
    // A SEPARATE FLAG FROM `bow`, which is the other thing whose strip the
    // right button drives, because the two must never both be true and a
    // single enum would invite a tool that is half of each. `ads` is the third
    // claimant on that button and is likewise exclusive.
    bool food = false;
    // -- HOW MANY FRAMES OF RELOAD SIT BEHIND THE REST POSE ----------------
    //
    // A COUNT AND NOT A FLAG, and it is the fourth kind of strip in this file.
    // `models[0]` is the gun at rest -- which is what every other tool's [0] is
    // -- and `models[1 .. reloadFrames]` are the cycle. Zero means the tool has
    // none, so `models[0]` is all there is and nothing downstream changes.
    //
    // THE ART DECIDES HOW MANY. addGun reads whatever .vox files are in the
    // reload directory, so dropping two more frames in between 36 and 37 makes
    // the cycle smoother and needs no code. That is why this is not a constant.
    int reloadFrames = 0;
    // -- HOW LONG ONE TURN OF THAT STRIP TAKES, AND WHAT IT LOADS ----------
    //
    // (user 2026-09-18: "the pistol animation is way too slow. increase it by
    // 4x. also it needs to play multiple times depending on how many shots have
    // been powered. its a standard revolver with 6 rounds.")
    //
    // A MAGAZINE AND A CYLINDER ARE NOT THE SAME ANIMATION, and this pair of
    // fields is the whole difference. The rifle's strip is ONE magazine change:
    // it plays once and the magazine is full, so `reloadRounds` is 0, which
    // means "this cycle loads the whole thing". The pistol's is ONE ROUND going
    // into a cylinder -- so it loads 1, and the gun plays it once per round it
    // is short. Six empty chambers is the strip six times.
    //
    // WHICH ALSO SETTLES THE LENGTH. A cycle that fills a whole magazine can
    // afford 1800 ms; the same 1800 for a single round is thirty seconds to
    // fill a revolver. See kPistolReloadMs.
    //
    // ZERO MEANS kReloadMs, and it is written that way round because this
    // struct is declared ABOVE that constant -- the timings sit with the
    // recoil curve, where the rest of the animation numbers are. See
    // HeldItem::reloadCycleMs, which is the one place the fallback lives.
    float reloadMs = 0.0f;
    int reloadRounds = 0;
    // -- HOW MANY OF THOSE FRAMES ARE THE ACTION OPENING -------------------
    //
    // (user 2026-09-18: "the reload needs to stay open as it cycles through
    // the bullets. it currently closes the chamber everytime it reloads 1
    // bullet.")
    //
    // A CYLINDER IS SWUNG OUT ONCE AND CLOSED ONCE, however many rounds go
    // into it -- so a strip that loads one round at a time is really three
    // things and not one: `reloadLead` leading frames that are the gun
    // OPENING, the rest of the strip which is one ROUND going in, and the lead
    // played BACKWARDS to close. Replaying the whole strip per round replays
    // the opening, and an opening starts from a CLOSED gun -- which is the
    // chamber shutting itself between every round.
    //
    // MEASURED, NOT WRITTEN DOWN. addGun takes it as the frames before the
    // first one with MORE voxels in it than the gun at rest: the round is
    // matter the gun does not own, so the frame it appears in is the frame the
    // loading starts, and everything before it is the action travelling. Art
    // with a different number of swing frames needs no code. Zero means the
    // strip was not split, and zero is exactly the behaviour this replaced:
    // one phase, the whole strip, once per round.
    int reloadLead = 0;
    // -- WHERE THE BARREL ACTUALLY ENDS, in BOX voxels ---------------------
    //
    // (user 2026-09-18: "the bullets seem to come from 1 voxel underneath the
    // tip of the guns point".)
    //
    // The centre of the first non-empty slice from each end of the depth axis,
    // and the depth of that slice's outer face. See HeldItem::muzzle for why
    // the box's own middle is the wrong point, and measureTips for how these
    // are taken. Anything that is not a gun gets the box's middle, which is
    // what the old code did for everything.
    float tipLo[3] = {0.0f, 0.0f, 0.0f};
    float tipHi[3] = {0.0f, 0.0f, 0.0f};
};

// -- the bow's own timing, from the JS engine's ui/audio.js -----------------
//
// The right button pulls 00 -> 02 and HOLDS at 02; releasing runs 03 out to the
// end and returns to rest. Both are STEPPED, not interpolated: a frame is
// picked, so the bow reads as drawn art rather than as a tween.
// -- EATING, FROM THE BROWSER ENGINE'S OWN TWO NUMBERS ----------------------
//
// (user 2026-09-17: "import the eating mechanics from v1 onto all of the
//  food.")
//
// 900 ms A BITE, held. Long enough that eating is a thing you commit to and
// short enough that it is not a chore; v1 arrived at it and there is no reason
// to re-derive it.
//
// TWENTY-ONE FRAMES, which is also v1's -- but NOT for v1's reason. There the
// strip was played on a fixed 24 fps clock and the count had to be chosen so
// the animation finished near the bite (13 frames ran 542 ms of a 900 ms bite
// and the last frame hung for 358 ms, which reads as a stall). Here the frame
// is picked from the bite's own PROGRESS, so any count finishes exactly with
// it and this is purely how fine the carve looks. Twenty-one is about one
// frame per 43 ms, which is smooth at any frame rate.
inline constexpr float kEatMs = 900.0f;
inline constexpr int kEatFrames = 21;

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
//
// -- HALF AGAIN AS FAST (user 2026-09-20: "I just need it to switch hand held
//    items faster") -------------------------------------------------------
//
// 240 -> 160 here and 130 -> 87 at kSwapOutMs: both legs cut to two thirds, so
// the whole change of hands goes 370 ms to 247. Both, because the swap is the
// two played back to back and speeding up only the rise would leave the old
// tool dawdling on its way out -- which is the half you notice, since nothing
// is in frame during it.
inline constexpr float kSwapMs = 160.0f;

// -- THE RECOIL CURVE -- see the block in HeldItem::xform ------------------
//
// 40 ms out and 200 ms back. At the rifle's 100 ms between rounds that means
// automatic fire never lets the gun settle, which is correct: it climbs and
// shakes while the trigger is down and drops home when it is let go.
//
// The travel is in the pose's own units (world voxels from the eye), so 2.2 is
// 22 cm back and 0.9 is 9 cm up -- big for a viewmodel, and it has to be: the
// gun is 90 cm from the eye and a centimetre there is nothing.
inline constexpr float kRecoilOutS = 0.040f;
inline constexpr float kRecoilBackS = 0.200f;
inline constexpr float kRecoilBackVox = 2.2f;
inline constexpr float kRecoilUpVox = 0.9f;
inline constexpr float kRecoilPitch = 0.11f;   // radians of nose-up

// -- THE RELOAD, AND THE NINE FRAMES THAT WERE ALREADY ON DISK -------------
//
// (user 2026-09-18: "then the player fires with left click and the number goes
// down until 0, where the gun then reloads. there are animations for the reload
// cycle. look in the assault file.")
//
// guns/assault_rifle/reload/32..40.vox, which v1 INDEXES and never plays --
// there is no gun code in that engine at all (vox-index.js lists the strip and
// nothing reads it), so the timing below is v2's and there is no number to
// carry over. The strip is the magazine coming out of the bottom of the box and
// going back in: 32 is the gun at rest, 33-35 drop the magazine clear, 36 is
// the gun with NO magazine at all -- 30 voxels rather than 32, which is the
// tell -- and 37-40 seat a fresh one and come back to rest.
//
// 900 ms IS THE WHOLE CYCLE -- nine frames at 100 ms each. The frames are
// STEPPED off that clock, like the bow's draw: a frame is picked rather than
// tweened, so it reads as drawn art. See HeldItem::model.
//
// IT WAS 1800 (user 2026-09-18: "double the reload speed of the assault
// rifle"). The old number was chosen so a frame lasted exactly one round of
// fire rate (kBulletIntervalMs, 200 ms) and a reload cost nine shots of time.
// That relationship is deliberately gone: half a fire-rate interval a frame
// now, and a reload costs four and a half shots. Worth knowing because it is
// the kind of tie that gets quietly restored by somebody tidying up.
inline constexpr float kReloadMs = 900.0f;
// -- ...AND THE REVOLVER'S, WHICH IS ITS OWN NUMBER ----------------------
//
// (user 2026-09-18: "the pistol animation is way too slow. increase it by 4x".)
//
// 450 ms, and it USED TO BE WRITTEN AS kReloadMs * 0.25f because that is the
// arithmetic the ask was phrased in. It is spelled out now, because the two
// guns were re-timed separately the moment the rifle was asked for on its own
// -- derived, halving the rifle would have silently halved the pistol to
// 225 ms as well, and nothing would have said so.
//
// IT IS PER ROUND, NOT PER RELOAD. See Tool::reloadRounds: the pistol loads one
// chamber a turn and turns once per empty chamber, so a full six is 2.7 s of
// animation -- which is why the cycle itself had to get short. At 1800 it would
// have been eleven seconds.
inline constexpr float kPistolReloadMs = 450.0f;

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
// -- AND THE HALF OF THE SWAP THAT WAS NEVER WRITTEN ----------------------
//
// (user 2026-09-19: "when switching weapons, there is some ghosting that gets
//  left behind. can you apply motion vectors on items that dissapear to make
//  room for the next item?")
//
// cycle()'s own note has claimed since it was written that "the tool drops out
// of frame and the next rises in", and only the SECOND half was ever built: the
// outgoing tool vanished on the frame the wheel turned and the new one bounced
// up in its place.
//
// THE GHOST IS A DISOCCLUSION, AND IT IS THE ONE CASE AN UPSCALER CANNOT GUESS.
// The tool's own pixels are already handled -- V6Params::heldPrevValid drops to
// zero on a change of object, so they get no history. What is left is every
// pixel the old tool VACATED: it shows the world now, it carries the world's
// own motion vector, and that vector is correct -- it points at where that
// world point was last frame, which is a pixel the tool was covering. So Ray
// Reconstruction fetches the axe as history for a patch of forest floor. No
// motion vector can say otherwise, because nothing about the world moved.
//
// WHAT FIXES IT IS NOT A VECTOR, IT IS NOT VANISHING. Lower the old tool out of
// the bottom of the frame first and the model only changes when nothing of it
// is on screen: every frame in between is a small movement of a surface that is
// still there, which the pose-difference vector describes exactly. The
// disocclusion is gone rather than papered over.
//
// 130 ms, which is under the eye's own fusion threshold for "one movement" and
// long enough at 60 Hz to be a dozen frames of honest motion. kSwapOutVox is
// how far it travels: the tool sits about 8.7 voxels forward of the eye, so
// half the screen subtends roughly five voxels there, and nine clears the
// bottom of the frame at every field of view this engine offers.
// 87, from 130 -- see kSwapMs for why both legs move together. Still over the
// eye's fusion threshold for "one movement" and still five frames at 60 Hz, so
// it reads as a motion rather than as the tool teleporting out of frame.
inline constexpr double kSwapOutMs = 87.0;
inline constexpr float kSwapOutVox = 9.0f;
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
    // LOOSE IS THE FIFTH, and it is a kind rather than a flag on Trunk because
    // it is edited down a different path entirely: a placed model is an
    // instance in a chunk with a stump to leave behind, and a felled tree is a
    // rigid body that has neither. What it IS made of is `takesAs` below --
    // wood, stone or soil, exactly the same three the tools declare.
    enum Kind { None, Ground, Trunk, Rock, Loose };
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

    // WHAT A LOOSE BODY IS MADE OF, on a Loose hit and only then.
    //
    // A Solid answers this by being standable or not; a body cannot, because a
    // felled tree is lying down and so is a boulder that has rolled. So the
    // body remembers what it was cut from and hands it out here -- see
    // World::DebrisTakes, which these mirror one for one.
    Takes takesAs = Takes::Nothing;
    // WHICH BODY, so the carve can find it again. -1 unless kind is Loose.
    int debris = -1;

    // THE INSTANCE THE BLOW LANDED ON, on a Trunk or Rock hit and only then.
    // Carried whole because breaking a model needs its transform to find the
    // struck voxel and its identity to find the instance -- see World::carveModel.
    // Meaningless when kind is Ground or None, and never read there.
    Solid solid;

    // THE RAY ITSELF, kept so the carve and the hit cannot disagree.
    //
    // Both walk the model's voxels now -- swingRayModels to decide THAT the
    // blow landed, World::carveModel to decide WHICH voxel it landed on -- so
    // handing the second one the identical ray is what makes the second one
    // certain to find something. The hit POINT would not do: it is a position
    // on a face, and a rounding either way puts it in the air outside the
    // voxel or in the one behind it.
    Vec3 eye{0, 0, 0};
    Vec3 dir{0, 0, 0};
    float reach = 0.0f;
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
// THE MODELS ARE MARCHED THE SAME WAY, and they did not used to be. A tree or
// a boulder in v2 is an instance transform over a shared voxel grid, which is
// not the world's grid this walks -- so for a long time a model was tested as
// the upright elliptic cylinder its collider describes. That is a different
// shape from the rock, and every symptom of it was reported as the tool being
// unreliable: a ray over the shoulder of a dome missed a rock plainly under the
// crosshair, and a body standing ON a big rock was inside the ellipse, took the
// far root, and found it twenty metres away and out of reach. See
// rayModelVoxels in scene/collide.h, which walks the model's OWN grid with the
// same Amanatides and Woo the terrain gets.
// ---------------------------------------------------------------------------
// THE MODELS ALONE, with the terrain left out of it.
//
// swingRay below reports the NEAREST thing under the crosshair, and near a
// boulder the ground often is nearer: you stand against a rock, aim a little
// down, and the terrain march answers first. The blow is then classified as
// Ground on grass or soil, and a pick -- which takes stone and nothing else --
// refuses it. That is "the pick does not work on all of the rocks", and it
// depends on where you stand rather than on the rock.
//
// So a tool that has been refused can ask this instead: is there a model under
// the crosshair at all, within reach. Same voxels, same reach, same order --
// only without the ground winning on distance.
// HOW FAR A SWING REACHES, POINTED THAT WAY.
//
// The reach opens up as you look down, so the ground at your feet is always in
// range without the horizontal reach having to be long enough to hit a tree two
// body lengths away.
//
// SAID ONCE because three different rays need the same answer -- the terrain
// march, the models, and the loose bodies -- and a body you can hit half a
// metre further away than the rock behind it is a bug nobody would ever think
// to look for here.
inline float swingReachM(const Vec3 &dir) {
    const float cp = sqrtf(maxf(0.0f, dir.x * dir.x + dir.z * dir.z));
    return minf(kReach3dVox, kReachHorizVox / maxf(0.15f, cp)) * VOXEL_M;
}

inline Swing swingRayModels(const WalkWorld &w, const Vec3 &eye, const Vec3 &dir) {
    Swing out;
    if (!w.terrain) return out;

    const float reach = swingReachM(dir);

    // -- the trunks and the boulders ----------------------------------------
    //
    // Tested first and kept as a CANDIDATE, not returned: the ground may still
    // be nearer, and the swing lands on whatever the crosshair reaches first.
    float bestT = reach;
    for (int i = 0; i < w.solidCount; ++i) {
        const Solid &s = w.solids[i];
        if (s.hx <= 0.0f || s.hz <= 0.0f) continue;

        float t = 0.0f;
        if (s.vol) {
            // THE MODEL'S OWN VOXELS. First solid one along the ray, or nothing
            // -- no roots, no inside-outside case, no shape that is not the
            // rock. See rayModelVoxels.
            if (!rayModelVoxels(s, eye, dir, bestT, VOXEL_M, &t)) continue;
            if (t >= bestT) continue;
        } else {
            // NO VOLUME ON THIS ONE, which should not happen for anything the
            // world places and is kept so a Solid built some other way still
            // answers. The old elliptic cylinder, unchanged.
            const float ox = (eye.x - s.cx) / s.hx, oz = (eye.z - s.cz) / s.hz;
            const float dx = dir.x / s.hx, dz = dir.z / s.hz;
            const float a = dx * dx + dz * dz;
            if (a < 1e-12f) continue;
            const float b = 2.0f * (ox * dx + oz * dz);
            const float c = ox * ox + oz * oz - 1.0f;
            const float disc = b * b - 4.0f * a * c;
            if (disc < 0.0f) continue;
            const float sq = sqrtf(disc);
            t = (-b - sq) / (2.0f * a);
            if (t < 0.0f) t = (-b + sq) / (2.0f * a);
            if (t < 0.0f || t >= bestT) continue;
            if (eye.y + dir.y * t > s.top) continue;
        }
        bestT = t;
        out.hit = true;
        out.kind = s.standable ? Swing::Rock : Swing::Trunk;
        out.solid = s;
        out.eye = eye;
        out.dir = dir;
        out.reach = reach;
        // -- FLESHY, NOT BOUNCY (user 2026-09-21: "have the axe be able to
        //    take chunks our of the cactus like the pick") ---------------
        //
        // `soft` is what makes a hit answer to BOTH the axe and the pick --
        // see toolTakes, one line above the switch. It was `bouncy`, which is
        // the mushroom and only the mushroom, so a saguaro fell through to
        // `standable ? Rock : Trunk`, came out a Rock and asked for the pick
        // alone.
        //
        // KINDS 7 AND 8 ARE THE CACTUS AND THE DESERT SHRUB, and World's debris
        // classifier already groups them with the mushroom under exactly this
        // reasoning, in its own words: "a saguaro is a fleshy plant, which is
        // what a mushroom is". The two now agree about what a cactus is instead
        // of disagreeing across two files.
        out.soft = s.bouncy || s.modelKind == 7 || s.modelKind == 8;
        out.material = mat::AIR;
        out.dist = t;
        out.point = eye + dir * t;
    }

    return out;
}

// ---------------------------------------------------------------------------
// DOES THIS TOOL TAKE WHAT THIS SWING RAN INTO -- asked ONCE, by everyone.
//
// This is the JS engine's toolTakesFor, and the reason it is one function
// rather than two is that engine's own note, quoted in toolsound.h: "a sound
// that disagrees with the swing is worse than no sound, because it teaches the
// player the wrong thing about their tool."
//
// v2 HAD IT AS TWO. The swing decided in App::onFrame and the audio decided
// again in ToolSounds::blow, and they agreed only because both were edited
// together every time. That is a promise kept by hand, and adding a third
// material is exactly the change that breaks one: a shovel that bit the ground
// while the audio still called the ground unbreakable would play the WRONG-TOOL
// knock on a blow that worked.
//
// SOFTNESS IS ASKED HERE NOW, AND THE AUDIO STILL ASKS IT SEPARATELY. Those
// are two different questions about the same flag and they were conflated in
// the note this replaces: "nobody recorded a sound for a mushroom cap" is a
// fact about the audio, and blow() keeps its own early-out for it. WHICH TOOLS
// CAN CUT ONE is a fact about the tools, and it belongs here.
// ---------------------------------------------------------------------------
inline bool toolTakes(Takes t, const Swing &s) {
    if (!s.hit) return false;
    // -----------------------------------------------------------------------
    // A MUSHROOM ANSWERS TO BOTH TOOLS (user 2026-09-14: "let the axe take a
    // chunk out of the mushrooms just like the pick can").
    //
    // It is not stone and it is not wood, and the only honest thing to do with
    // a material that is neither is to let both edged tools cut it -- which is
    // what "just like the pick can" asks for: the pick keeps working, the axe
    // starts. A shovel and a bow still do nothing, which is why this is not
    // simply `return t != Takes::Nothing`.
    //
    // BEFORE THE Loose ARM AND BEFORE THE SWITCH, so it covers a cap still
    // standing on its stem (a soft Swing::Rock -- see swingRayModels, which
    // takes it from Solid::bouncy) and one lying on the ground (a soft
    // Swing::Loose -- see kDebrisSoft) with one line instead of an arm in each.
    // Standing and fallen disagreeing about which tool works is exactly the
    // shape of bug the note above this function exists to prevent.
    // -----------------------------------------------------------------------
    if (s.soft) return t == Takes::Wood || t == Takes::Stone;
    // A LOOSE BODY CARRIES ITS OWN ANSWER, so this is one comparison and not a
    // fourth arm in every case below. A felled tree is wood wherever it is
    // lying and whatever it is lying on, which is precisely what the player
    // means by "hit a felled tree with an axe": the same tool that cut it down
    // is the tool that breaks it up.
    if (s.kind == Swing::Loose) return t != Takes::Nothing && s.takesAs == t;
    switch (t) {
        case Takes::Wood:
            return s.kind == Swing::Trunk;
        // A BOULDER AND A BARE HILLSIDE ARE ONE MATERIAL TO A PICK. That
        // engine's pickOnlyTab is about stone, not about whether the stone is a
        // model or the terrain, and half the stone in this world is terrain.
        case Takes::Stone:
            return s.kind == Swing::Rock ||
                   (s.kind == Swing::Ground && isStoneMat(s.material));
        // ...AND SOIL HAS ONLY ONE HOME. Nothing this world places as a model
        // is made of it, so there is no Rock arm to match the one above.
        case Takes::Soil:
            return s.kind == Swing::Ground && isSoilMat(s.material);
        default:
            return false;   // a bow, and an empty hand
    }
}

inline Swing swingRay(const WalkWorld &w, const Vec3 &eye, const Vec3 &dir) {
    Swing out;
    if (!w.terrain) return out;

    const float reach = swingReachM(dir);

    // The models first -- see swingRayModels, which owns that pass now.
    out = swingRayModels(w, eye, dir);
    float bestT = out.hit ? out.dist : reach;

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
    // GENERATED, THEN DUG. The march used to ask the height field alone, which
    // meant it could not see a hole: every swing after the first stopped on the
    // surface that used to be there and carved air. See TerrainProbe.
    TerrainProbe probe(w.terrain, w.edits);
    for (int guard = 0; guard < 4096 && t <= maxT; ++guard) {
        // One comparison in the ordinary case -- the terrain is a height field
        // -- and one map lookup in a chunk somebody has dug in.
        if (probe.solid(vx, vz, vy)) {
            out.hit = true;
            out.kind = Swing::Ground;
            out.soft = false;
            // THE VOXEL THE RAY STOPPED IN, which is the one the player is
            // looking at. See TerrainProbe::material for why it is not the
            // column's surface any more.
            out.material = probe.material(vx, vz, vy);
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
// WHAT THE SWING WENT THROUGH ON ITS WAY TO THE GROUND.
//
// A BLADE IS NOT SOLID AND MUST NOT BECOME SOLID. swingRay stops on
// TerrainProbe::solid, which is the column height -- grass and wheat stand
// ABOVE that, so a swing passes straight through a field and lands on the dirt.
// That is correct for everything else in the engine and is exactly why it is:
// making a blade solid would stop the player walking through long grass, stop
// arrows, and stop the march every flying animal's floor is built on.
//
// So this is a SECOND march, run only when something wants to know. It stops on
// the first voxel whose material is a blade, which is the plant you were
// actually aiming at -- and it stops short of `maxDist`, which the caller sets
// to however far the ordinary swing got, so wheat behind a rock is not cut
// through the rock.
// ---------------------------------------------------------------------------
struct BladeHit {
    bool hit = false;
    uint8_t material = 0;
    Vec3 point{0, 0, 0};
    float dist = 0.0f;
};

inline BladeHit bladeRay(const WalkWorld &w, const Vec3 &eye, const Vec3 &dir, float maxDist) {
    BladeHit out;
    if (!w.terrain) return out;
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
    const float maxT = maxDist / VOXEL_M;
    float t = 0.0f;
    TerrainProbe probe(w.terrain, w.edits);
    for (int guard = 0; guard < 4096 && t <= maxT; ++guard) {
        const uint8_t m = probe.material(vx, vz, vy);
        if (isBlade(m)) {
            out.hit = true;
            out.material = m;
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

    // -- HOLD THE SIGHTS UP WITHOUT HOLDING THE BUTTON -------------------
    //
    // (user 2026-09-17: "have a checkmark on the aim down site box, where when
    // I check it, the gun aims down sights, where I can then adjust the
    // position".)
    //
    // A SECOND WAY TO ASK FOR THE SAME THING, not a second state. It is OR'd
    // with the right button in update() and everything downstream -- the ease,
    // the pose blend, the steadied bob -- is untouched, so what you tune is
    // exactly what the button gives you and not a preview of it.
    //
    // IT EXISTS BECAUSE THE PANEL NEEDS BOTH HANDS. The sighted pose is only on
    // screen while the gun is up, and the gun was only up while the right
    // button was down -- which is the button you would have to let go of to
    // drag a slider. Seven sliders you can only see while you are not touching
    // them are seven sliders nobody can use, which is the stack card's own
    // complaint about its badge.
    //
    // CLEARED WHEN THE [K] PANEL CLOSES. The checkbox is drawn on that card and
    // nowhere else, so leaving it set would weld the gun to your eye with no
    // visible control to turn it off.
    bool adsHold = false;

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
             Takes takes = Takes::Nothing, int selfMergeTol = 0,
             const HeldPose *adsPose = nullptr) {
        Tool t;
        t.name = name;
        t.takes = takes;
        t.pose = pose;
        // An adsPose is what makes a tool aimable -- there is no second flag to
        // disagree with it. See Tool::ads.
        if (adsPose) {
            t.ads = true;
            t.adsPose = *adsPose;
        }
        t.path = voxPath;
        const int m = world.addHeldModel(voxPath, &t.sx, &t.sy, &t.sz, selfMergeTol);
        if (m < 0) return false;
        // THE BOX'S OWN MIDDLE, which is what muzzle() used to take for
        // everything. Right for a model that fills its box, and nothing here
        // is a strip -- a gun goes through addGun, which measures instead.
        t.tipLo[0] = t.tipHi[0] = 0.5f * float(t.sx);
        t.tipLo[1] = t.tipHi[1] = 0.5f * float(t.sy);
        t.tipLo[2] = 0.0f;
        t.tipHi[2] = float(t.sz);
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
    // CLAIM THE KIT'S COLOURS BEFORE THE WOOD IS FULL OF ANIMALS.
    //
    // THE BOW WAS RENDERING WITH A COLOUR MISSING (user 2026-09-14, "the bow is
    // broken, missing voxels") and this is why. The measurement, taken on a
    // plain start:
    //
    //     palette  255 of 255 entries used  -- FULL
    //     v2: PALETTE FULL -- 14 colours could not be registered
    //
    // Fourteen is not fourteen colours. overflowed() counts failed CALLS, and
    // the bow is fourteen models -- seven nocked, seven bare -- each of which
    // asked for ONE colour it could not have. The composed frames want 12
    // distinct entries nocked and 9 bare; the engine reported 11 and 8. So the
    // table ran out EXACTLY at the bow, one distinct colour short, and one
    // shade of it stopped being drawn in every frame of the draw.
    //
    // THE RULE IS THE ONE prewarmRoom ALREADY FOLLOWS, and it is written on
    // that function: colours are served first-come, so ORDER OF REGISTRATION IS
    // PRIORITY, and whatever asks last is what you cannot see. The pause room
    // was moved to the front for this exact reason and the held kit is now the
    // thing at the back of the queue -- it loads after the terrain, after the
    // trees, after the butterflies, birds, lake, flock and bunnies.
    //
    // NOT A RESERVED TAIL, which is the fix that suggests itself and is the
    // wrong one: a ceiling held below mat::COUNT steals from whoever asks last,
    // which is this kit, which is the bug. The note over Palette::forModelColor
    // says so from the last time it happened to the stone heads.
    //
    // COLOURS ONLY. The geometry still loads where it always did -- addHeldVox
    // needs the pool and the structures, and those do not exist yet here. This
    // asks Palette for exactly the entries that load will ask for, with the
    // same `exact` key, so the later call is a cache hit and mints nothing.
    //
    // AFTER deriveGroundFromTrees, WHICH IS WHY IT IS NOT EARLIER STILL. That
    // pass reads back the entries the TREES minted to decide what a hillside is
    // made of, and the note on the flyer band records the hazard in as many
    // words: a model registered ahead of it becomes a candidate for soil. A
    // hillside the colour of a stone axe head is a real outcome of getting this
    // one line's position wrong.
    //
    // Returns how many entries it took, for the report at the call site.
    // -----------------------------------------------------------------------
    // -- THE KIT MODELS THAT ARE RAMPS RATHER THAN DETAIL ----------------
    //
    // See World::addHeldVox for the mechanism. A TOOL is a dozen distinct
    // colours and every one of them is a feature you look at from thirty
    // centimetres; a RAMP is one hue in a handful of steps, and the steps are
    // there to shade a curve, not to be told apart.
    //
    // ONE OF THEM, AND THE STEAK IS NOT IT ANY MORE:
    //
    //   wheat   5 colours -- one red and FOUR CREAMS inside 16 of each other
    //           (255,243,149 down to 255,238,127) -- folds to 3
    //
    // Those four creams are one colour with rounding on it, and nobody has ever
    // looked at a stalk of wheat and counted them.
    //
    // THE STEAK WAS FOLDED HERE AND IT SHOWED (user 2026-09-15: "the steaks
    // pallette is still not correct", the third time on this model). Its nine
    // are five reds twelve apart and four pinks -- a RAMP ACROSS A SURFACE, not
    // rounding, and folding it to five turns a shaded slab into bands. The
    // wheat's creams sit on a stalk a few voxels wide; the steak's ramp is the
    // whole of what the object looks like.
    //
    // It costs four more entries and they were there: 250 of 255 before this,
    // 254 after. The start-up line prints the total, so the day it stops
    // fitting is a number rather than a surprise.
    //
    // BY PATH, because the reservation below has nothing but the path to go on
    // and the two have to fold identically: reserve nine and use five and the
    // four left over are gone for the session.
    // 24 RATHER THAN 16, and the wheat is the only thing it applies to: its
    // four creams span 22.6 end to end (255,243,149 to 255,238,127), so at 16
    // they fold to two groups and at 24 to one. That last entry is what the
    // steak's reds needed -- the table has no spare, and a cream a player
    // cannot distinguish is the cheapest thing in it to give up.
    static constexpr int kRampMergeTol = 24;
    // -- ...AND THE STEAK FOLDS ONLY WHAT IS ACTUALLY A DUPLICATE ---------
    //
    // Its nine are not one ramp, they are TWO, and they are not the same kind
    // of thing. Measured, consecutive distances:
    //
    //     the five reds     13.9, 13.9, 14.7, 13.9      the meat's shading
    //     red -> pink       97.0                        a different material
    //     the four pinks    10.0, 10.6, 10.0            marbling, near-identical
    //
    // At 16 both ramps fold and the meat goes to five flat bands, which is what
    // was reported twice. At 12 the REDS survive -- every step is over 13 -- and
    // only the pinks collapse, which is seven entries and costs nothing anybody
    // can see. Nine exact does not fit: it takes the palette past 255 and the
    // wheat starts losing voxels.
    static constexpr int kSteakMergeTol = 12;
    // -- AND THE GUN IS NOT ON THIS LIST, WHICH IT WAS FOR A DAY ------------
    //
    // ("the guns color pallete is off", user 2026-09-17.)
    //
    // The assault rifle was folded at kRampMergeTol because eleven exact
    // entries would not fit -- ELEVEN SHADES over 32 voxels: seven near-blacks
    // from 43 to 64, three light greys from 152 to 166, and one red. Folded, it
    // asked for five and the worst voxel moved 10 of 255.
    //
    // THAT WAS THE WRONG TRADE AND THIS FILE ALREADY SAID SO. The rule is
    // written twice over in World::addHeldVox, once in v1's words: "the tool
    // the player stares at stays byte-accurate". A stone axe 7/255 out was
    // reported as broken three times. A gun is held closer and looked at more
    // than any axe ever was, and 53% of its voxels are in the dark ramp that
    // the fold collapsed -- so the fold is visible on the majority of the
    // model, not on a corner of it.
    //
    // The table was made to fit instead, and the entries came from the level
    // rather than from the gun: nuketown's two greens now map onto the
    // broadleaf grass ramp the oak wood already owns (see World::loadLevel), so
    // they cost nothing. What the kit stares at is exact again.
    //
    // -- WHAT THE GUN GETS INSTEAD: EIGHT OF ELEVEN, AND NOTHING MOVES 3/255 -
    //
    // Byte-exact did not fit. Restoring all eleven took the table to 255 of 255
    // and the wheat lost two of its colours -- the same silent failure, moved
    // onto a different model. (A per-level palette is the real answer to that
    // and is half built; see the long note in World::setLevel for exactly where
    // it stops.)
    //
    // So this is the smallest tolerance that frees the three entries needed,
    // and it is nothing like the fold that was reported. MEASURED on the model:
    //
    //     tol 24  ->  5 entries, worst voxel moves 10/255   <- what was wrong
    //     tol  8  ->  8 entries, worst voxel moves  3/255   <- this
    //     tol  0  -> 11 entries, exact, does not fit
    //
    // At 8 the three shades that go are 61->64, 54->57 and 47->50: three levels
    // each, on a near-black. sRGB quantisation is one level, so this is three
    // of them on colours that differ by four -- against the SEVEN-shade collapse
    // that flattened 53% of the model onto one grey. The receiver keeps its
    // ramp, the light greys keep all three of theirs, and the red is untouched.
    // -- ...AND IT IS ZERO NOW, WHICH IS WHERE IT SHOULD HAVE STAYED ------
    //
    // Reported twice: "the guns color pallete is off", then "its missing
    // color". Both times the answer was a fold, and both times the fold was
    // only there because the table was full:
    //
    //     tol 24  ->  5 of 11 entries, worst voxel 10/255
    //     tol  8  ->  8 of 11 entries, worst voxel  3/255
    //     tol  0  -> 11 of 11, byte-exact
    //
    // The table is NOT full any more -- it reads 242 of 255 with the level's
    // greens on the grass ramp and the bulb's cap on the stone one -- so there
    // is no trade left to make and the rule in World::addHeldVox applies
    // unaltered: "the tool the player stares at stays byte-accurate". Three
    // entries is what it costs and there are thirteen.
    //
    // kGunMergeTol IS GONE ON PURPOSE. If the table ever fills again, take the
    // entries from the level -- which is a backdrop at tens of metres -- and
    // not from the thing held 90 cm from the eye.
    static constexpr int kGunMergeTol = 0;
    // -- AND THE WHEAT IS NOT FOLDED ANY MORE ---------------------------
    //
    // (user 2026-09-21: "the hand held wheat is still not authoring the right
    //  colors. reload the hand held wheat color palete".)
    //
    // BOTH HALVES HAD TO MOVE, which is the trap this function's own note is
    // about: add() passes a tolerance and prewarmColors RESERVES against one,
    // and "reserve nine and use five and the four left over are gone for the
    // session". Changing the call site alone would have left this reserving
    // the folded count while the load asked for the unfolded one.
    //
    // WHY IT CAN GO: kRampMergeTol existed because the wood's table was 250 of
    // 255 full. Per-object palettes landed after that -- a colour the wood
    // refuses is minted into the model's OWN table -- so the four creams are
    // no longer three entries the wood has to find. See the note at the wheat's
    // add in app_load.inl.
    static int mergeTolFor(const std::string &p) {
        if (p.find("/food/") != std::string::npos) return kSteakMergeTol;
        return 0;
    }

    // `selfMergeTol` here must match what add() will pass for the same model,
    // or the reservation claims entries the load never asks for.
    static int prewarmColors(World &world, const std::vector<std::string> &toolPaths,
                             const std::string &bowPath) {
        const int before = world.palette.used();
        // The same walk addHeldVox does: only the entries some voxel actually
        // wears, and `exact` because a held colour never merges. See the note
        // there for why the whole kit is exempt and not just the stone greys.
        auto mint = [&](const VoxModel &mo, int tol) {
            std::vector<bool> used(256, false);
            for (uint8_t v : mo.m) used[v] = true;
            // THE SAME FOLD addHeldVox WILL DO, so the reservation is exactly
            // what the load will ask for -- see mergeTolFor.
            std::vector<int> reps;
            for (int e = 1; e <= 255; ++e) {
                if (!used[size_t(e)]) continue;
                const std::array<uint8_t, 4> &c = mo.pal[size_t(e) - 1];
                bool merged = false;
                if (tol > 0)
                    for (int r : reps) {
                        const std::array<uint8_t, 4> &q = mo.pal[size_t(r) - 1];
                        const int dr = int(q[0]) - int(c[0]), dg = int(q[1]) - int(c[1]),
                                  db = int(q[2]) - int(c[2]);
                        if (dr * dr + dg * dg + db * db <= tol * tol) {
                            merged = true;
                            break;
                        }
                    }
                if (merged) continue;
                reps.push_back(e);
                // conifer=false, exactly as addHeldVox does -- the two
                // have to agree or the reservation stores a needle and
                // the load asks for a seed.
                // -- AND THE ID IS RECORDED, WHICH IT WAS NOT ---------------
                //
                // heldMtl_ is the mask World::buildLevelPalette reserves out
                // of the ARCADE's own table, because the kit walks through [O]
                // with you and one id has to mean one thing in both places.
                // It was only ever written by addHeldVox -- the LOAD -- and
                // the load happens after the arcade has already minted, so the
                // mask read empty and the arcade reserved nothing: measured at
                // "reserved 0 held-kit entries". It survived on luck, because
                // the arcade allocates top-down from 254 and the wood had only
                // reached 183. Recorded HERE instead, where the ids are minted
                // and before anything else asks for one.
                world.noteHeldMtl(world.palette.forModelColor(c, /*conifer=*/false,
                                                              /*exact=*/true));
            }
        };
        for (const std::string &p : toolPaths) {
            if (p.empty()) continue;
            VoxModel mo;
            std::string err;
            // SILENT ON FAILURE, and deliberately: this is a reservation, not a
            // load. A path that will not open is reported with its reasons by
            // the real load a few hundred lines later, and saying it twice
            // would only make the start-up look like it failed twice.
            // EXACTLY, FOR EVERY MEMBER OF THE KIT. A held colour never
            // merges -- see the note over World::addHeldVox, and the steak,
            // which was the one exception to that for a day and was banded by
            // it. `matchTol` survives on the signature because the capability
            // is worth having; nothing in the kit uses it now.
            if (voxLoad(p, &mo, &err)) mint(mo, mergeTolFor(p));
        }
        // THE BOW IS COMPOSED, NOT READ, so it has to be cut here to be asked
        // about -- parseBowStrip touches no device and the strip is thrown away
        // again. Both halves: the bare frames carry a colour the nocked ones do
        // not need to have claimed for them, and it was one of the two that
        // went missing.
        if (!bowPath.empty()) {
            std::string err;
            const BowStrip strip = parseBowStrip(bowPath, &err);
            // THE BOW IS A TOOL: exact, like the rest of the kit.
            for (const VoxModel &m : strip.withArrow) mint(m, 0);
            for (const VoxModel &m : strip.bowOnly) mint(m, 0);
        }
        return world.palette.used() - before;
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
    // -----------------------------------------------------------------------
    // A FOOD, AND THE STRIP IT IS EATEN THROUGH.
    //
    // (user 2026-09-17: "import the eating mechanics from v1 onto all of the
    //  food.")
    //
    // ONE WHOLE MODEL BECOMES kEatFrames FRAMES OF ITSELF BEING EATEN. This is
    // the browser engine's `eatStrip`, ported, and the carve rule is the whole
    // of it: score every voxel by its distance from a BITE POINT at one top
    // corner, sort, and drop them in that order. An apple then loses the side
    // you bit while the stalk at the far corner survives to the last frame,
    // which is what makes it read as being eaten rather than dissolving.
    //
    // THE SORT IS MADE TOTAL ON PURPOSE. Ties break on z, then x, then y --
    // v1's own tie-break, and the reason it has one is that a sort which is
    // not total is a food that is eaten in a different order on every boot.
    //
    // FRAME 0 IS THE WHOLE FRUIT, so "an apple in your hand" and "frame zero
    // of eating one" are the same model and can never disagree.
    //
    // THE LAST FRAME IS ONE VOXEL, not zero -- see biteFrame, where the reason
    // is v2's and not v1's: an empty model cannot be registered here at all.
    //
    // IT COSTS NO PALETTE. Every frame is a subset of the same voxels, and
    // addHeldVox registers held colours with an EXACT key -- so twenty-one
    // frames of an apple mint the apple's two colours once between them.
    // -----------------------------------------------------------------------
    // ONE FRAME OF A FOOD BEING EATEN -- see addFood for the rule and why the
    // sort has to be total. `f` runs 0 (whole) to kEatFrames - 1 (empty).
    static VoxModel biteFrame(const VoxModel &m, int f) {
        VoxModel out = m;
        // Every solid cell, in one list, so the bite ORDER is a sort rather
        // than a rule repeated per frame.
        struct Cell { int i; float d2; int x, y, z; };
        std::vector<Cell> cl;
        cl.reserve(m.m.size() / 4);
        // The bite point: one top corner of the model's own box. `by` is the
        // middle of the depth axis, which is where a mouth meets a ball.
        const float bx = float(m.sx - 1), by = 0.5f * float(m.sy - 1), bz = float(m.sz - 1);
        for (int z = 0; z < m.sz; ++z)
            for (int y = 0; y < m.sy; ++y)
                for (int x = 0; x < m.sx; ++x) {
                    const int i = x + y * m.sx + z * m.sx * m.sy;
                    if (i >= int(m.m.size()) || !m.m[size_t(i)]) continue;
                    const float dx = float(x) - bx, dy = float(y) - by, dz = float(z) - bz;
                    cl.push_back({i, dx * dx + dy * dy + dz * dz, x, y, z});
                }
        std::sort(cl.begin(), cl.end(), [](const Cell &a, const Cell &b) {
            if (a.d2 != b.d2) return a.d2 < b.d2;
            if (a.z != b.z) return a.z < b.z;
            if (a.x != b.x) return a.x < b.x;
            return a.y < b.y;
        });
        // How many have been eaten by this frame.
        //
        // ...AND THE LAST FRAME KEEPS ONE VOXEL, WHICH IS NOT A TASTE
        // DECISION. v1 eats a food down to nothing on its final frame and says
        // why ("make sure the food dissapears properly" -- a scrap that blinks
        // out reads as the model being switched off). Here an EMPTY model
        // cannot be registered at all: World::addHeldVox meshes it to nothing
        // and skips it, addFood then fails, and the whole food is left out of
        // the kit. Measured -- all three foods came back "meshed to nothing --
        // skipped" and every food slot was -1, which is the feature silently
        // absent rather than visibly wrong.
        //
        // The crumb costs nothing to look at: the last frame is reached at
        // progress 1.0, which is the same instant wantEat takes the food off
        // the stack, so it is replaced by the next item in the hand on that
        // frame rather than shown.
        const int gone =
            (kEatFrames < 2)
                ? 0
                : mini(int(cl.size()) - 1,
                       int(float(cl.size()) * float(f) / float(kEatFrames - 1) + 0.5f));
        for (int k = 0; k < gone && k < int(cl.size()); ++k) out.m[size_t(cl[size_t(k)].i)] = 0;
        return out;
    }

    bool addFood(World &world, const char *name, const std::string &voxPath,
                 const HeldPose &pose, int selfMergeTol = 0) {
        VoxModel m;
        std::string err;
        if (!voxLoad(voxPath, &m, &err)) {
            std::fprintf(stderr, "v2: food %s: %s -- skipped\n", voxPath.c_str(),
                         err.c_str());
            return false;
        }
        Tool t;
        t.name = name;
        t.pose = pose;
        t.path = voxPath;
        t.food = true;
        t.takes = Takes::Nothing;   // you do not mine with an apple
        for (int f = 0; f < kEatFrames; ++f) {
            const VoxModel bit = biteFrame(m, f);
            const int i = world.addHeldVox(bit, voxPath, &t.sx, &t.sy, &t.sz, selfMergeTol);
            if (i < 0) return false;
            t.models.push_back(i);
        }
        tools_.push_back(t);
        std::printf("v2: food %s  %d bite frames, %dx%dx%d\n", voxPath.c_str(), kEatFrames,
                    t.sx, t.sy, t.sz);
        std::fflush(stdout);
        return true;
    }

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
    // EVERY .vox IN A DIRECTORY, IN THE ORDER THE NUMBERS IN THEIR NAMES PUT
    // THEM.
    //
    // THE ART DECIDES THE STRIP, not a list in here. The reload frames are
    // named 32..40 -- numbers out of the browser engine's own sheet, where they
    // are frames of one long animation that also holds the fire pair -- so a
    // hard-coded list here would have to be edited to re-time the reload, and a
    // list that disagrees with the folder is a frame that silently never plays.
    // v1 keeps exactly such a list (vox-index.js) and never plays any of them.
    //
    // NUMERIC, NOT LEXICOGRAPHIC: a strip that ever reaches ten frames sorts
    // "10" before "9" as text, which is one frame played in the wrong place and
    // nothing to say so. A name that is not a number sorts last, which is where
    // the other guns' "base" frame belongs.
    // -----------------------------------------------------------------------
    static std::vector<std::string> stripFiles(const std::string &dir) {
        std::vector<std::pair<long long, std::string>> found;
        std::error_code ec;
        const std::filesystem::path root(dir);
        for (const auto &e : std::filesystem::directory_iterator(root, ec)) {
            if (ec) break;
            if (!e.is_regular_file(ec)) continue;
            std::string ext = e.path().extension().string();
            for (char &c : ext) c = char(std::tolower(int(uint8_t(c))));
            if (ext != ".vox") continue;
            const std::string stem = e.path().stem().string();
            const bool numeric =
                !stem.empty() && stem.find_first_not_of("0123456789") == std::string::npos;
            found.emplace_back(numeric ? std::atoll(stem.c_str()) : (1LL << 40),
                               e.path().string());
        }
        std::sort(found.begin(), found.end());
        std::vector<std::string> out;
        out.reserve(found.size());
        for (auto &f : found) out.push_back(f.second);
        return out;
    }

    // -----------------------------------------------------------------------
    // THE WHOLE STRIP FOLDS AS ONE, OR ITS FRAMES DISAGREE ABOUT THE PALETTE.
    //
    // World::addHeldVox's self-merge walks the entries a MODEL uses and keeps
    // the first of each cluster as its representative. Run it per frame and a
    // frame that is missing a colour -- reload/36 is the gun with no magazine,
    // 30 voxels where the others have 32 -- can elect a different
    // representative and mint an entry nothing else asked for. The palette is
    // at 242 of 255; a strip that quietly costs nine more of them is the bug in
    // the palette note arriving by a new road.
    //
    // So the fold is done ONCE over the union of the strip and baked into the
    // indices, and every frame then registers EXACTLY. At kGunMergeTol, which
    // is 0 today, this is the identity and the registration is byte-for-byte
    // what the single frame already got -- which is the point: it costs nothing
    // now and cannot go wrong later.
    // -----------------------------------------------------------------------
    static void foldStrip(std::vector<VoxModel> &frames, int tol) {
        if (tol <= 0 || frames.empty()) return;
        std::vector<bool> used(256, false);
        for (const VoxModel &f : frames)
            for (uint8_t v : f.m) used[v] = true;
        std::vector<int> repOf(256, 0), reps;
        for (int e = 1; e <= 255; ++e) {
            if (!used[size_t(e)]) continue;
            const std::array<uint8_t, 4> &c = frames[0].pal[size_t(e) - 1];
            int hit = 0;
            for (int r : reps) {
                const std::array<uint8_t, 4> &q = frames[0].pal[size_t(r) - 1];
                const int dr = int(q[0]) - int(c[0]), dg = int(q[1]) - int(c[1]),
                          db = int(q[2]) - int(c[2]);
                if (dr * dr + dg * dg + db * db <= tol * tol) {
                    hit = r;
                    break;
                }
            }
            if (hit) {
                repOf[size_t(e)] = hit;
            } else {
                reps.push_back(e);
                repOf[size_t(e)] = e;
            }
        }
        for (VoxModel &f : frames)
            for (uint8_t &v : f.m)
                if (v) v = uint8_t(repOf[size_t(v)]);
    }

    // -----------------------------------------------------------------------
    // WHERE EACH FRAME OF A STRIP SITS INSIDE THE SHARED BOX, FOUND BY LOOKING.
    //
    // MagicaVoxel trims every frame to its own contents, so the files of one
    // animation are different sizes and NOTHING IN THEM SAYS WHERE THEY LINE
    // UP. Get that wrong and the gun itself jumps about the screen while the
    // magazine changes, which reads as the whole model being broken rather than
    // as a frame being one voxel out.
    //
    // THE FIRST CUT WAS A RULE AND THE RULE DID NOT GENERALISE. The rifle's
    // frames grow downward -- its receiver row sits one below the top of the
    // box in all eleven files -- so "pad underneath, align the tops" fitted it
    // exactly. The pistol grows on THREE different sides: the magazine comes in
    // from the low-x side, an extra row of depth appears at the high-y end, and
    // the height never changes at all. A per-axis rule for that is a rule with
    // a case for every gun in the folder.
    //
    // SO IT IS MEASURED INSTEAD: the shift that puts the most of a frame's own
    // voxels on top of the rest pose's is the shift where the two are the same
    // gun. That is a cross-correlation over a 22-to-32-voxel model and a shift
    // range of a voxel or three -- microseconds, once, at load.
    //
    // IT REPRODUCES THE OLD RULE ON THE OLD ART, which is what makes it safe to
    // swap in: the rifle's twenty-voxel body only overlaps at the top-aligned
    // shift, so the shared box comes out 3 x 6 x 11 exactly as padUnder left
    // it, and the pose bake that was written against that box does not move.
    //
    // TIES GO TO THE SMALLEST SHIFT, so a frame identical to the rest pose sits
    // at zero rather than at whatever the scan happened to reach first.
    // -----------------------------------------------------------------------
    static int stripOverlap(const VoxModel &a, const VoxModel &b, int sx, int sy, int sz) {
        int n = 0;
        for (int z = 0; z < b.sz; ++z)
            for (int y = 0; y < b.sy; ++y)
                for (int x = 0; x < b.sx; ++x)
                    if (b.at(x, y, z) && a.at(x + sx, y + sy, z + sz)) ++n;
        return n;
    }

    // `b`'s origin in `a`'s coordinates. Returns the overlap it scored.
    static int fitFrame(const VoxModel &a, const VoxModel &b, int *ox, int *oy, int *oz) {
        int best = -1, bx = 0, by = 0, bz = 0, bestCost = 0;
        for (int sz = -(b.sz - 1); sz <= a.sz - 1; ++sz)
            for (int sy = -(b.sy - 1); sy <= a.sy - 1; ++sy)
                for (int sx = -(b.sx - 1); sx <= a.sx - 1; ++sx) {
                    const int n = stripOverlap(a, b, sx, sy, sz);
                    if (n <= 0) continue;
                    const int cost = abs(sx) + abs(sy) + abs(sz);
                    if (n > best || (n == best && cost < bestCost)) {
                        best = n;
                        bestCost = cost;
                        bx = sx;
                        by = sy;
                        bz = sz;
                    }
                }
        *ox = bx;
        *oy = by;
        *oz = bz;
        return best;
    }

    // The strip, fitted and padded into one grid. `frames[0]` is the rest pose
    // and is the reference everything else is aligned to.
    static void fitStrip(std::vector<VoxModel> *frames) {
        if (!frames || frames->empty()) return;
        std::vector<VoxModel> &f = *frames;
        std::vector<int> ox(f.size(), 0), oy(f.size(), 0), oz(f.size(), 0);
        for (size_t i = 1; i < f.size(); ++i)
            fitFrame(f[0], f[i], &ox[i], &oy[i], &oz[i]);
        int lo[3] = {0, 0, 0}, hi[3] = {f[0].sx, f[0].sy, f[0].sz};
        for (size_t i = 0; i < f.size(); ++i) {
            lo[0] = mini(lo[0], ox[i]);
            lo[1] = mini(lo[1], oy[i]);
            lo[2] = mini(lo[2], oz[i]);
            hi[0] = maxi(hi[0], ox[i] + f[i].sx);
            hi[1] = maxi(hi[1], oy[i] + f[i].sy);
            hi[2] = maxi(hi[2], oz[i] + f[i].sz);
        }
        const int W = hi[0] - lo[0], H = hi[1] - lo[1], D = hi[2] - lo[2];
        for (size_t i = 0; i < f.size(); ++i) {
            VoxModel out;
            out.sx = W;
            out.sy = H;
            out.sz = D;
            out.pal = f[i].pal;
            out.m.assign(size_t(W) * size_t(H) * size_t(D), 0);
            const int px = ox[i] - lo[0], py = oy[i] - lo[1], pz = oz[i] - lo[2];
            for (int z = 0; z < f[i].sz; ++z)
                for (int y = 0; y < f[i].sy; ++y)
                    for (int x = 0; x < f[i].sx; ++x)
                        out.m[size_t(x + px) + size_t(y + py) * size_t(W) +
                              size_t(z + pz) * size_t(W) * size_t(H)] = f[i].at(x, y, z);
            f[i] = std::move(out);
        }
    }

    // -----------------------------------------------------------------------
    // HOW MANY LEADING FRAMES ARE THE ACTION OPENING rather than a round going
    // in. See Tool::reloadLead for what it is for.
    //
    // THE ROUND IS THE MEASUREMENT. It is a piece of matter the gun does not
    // own, so the first frame holding MORE voxels than the rest pose is the
    // first frame of the loading, and everything between the rest pose and it
    // is the action travelling. On the pistol's ten frames that is four: 00 is
    // the gun closed, 01 and 02 have the cylinder half out, 03 has it out, and
    // 04 is where the round first appears under the frame.
    //
    // RETURNS 0 IF NOTHING EVER GAINS A VOXEL, which is a strip this rule has
    // no opinion about -- and 0 is the un-split cycle the code had before.
    // Capped one short of the end so a split always leaves a loading phase.
    // -----------------------------------------------------------------------
    static int measureLead(const std::vector<VoxModel> &frames) {
        if (frames.size() < 2) return 0;
        const int rest = countVox(frames[0]);
        for (size_t i = 1; i < frames.size(); ++i)
            if (countVox(frames[i]) > rest) return mini(int(i) - 1, int(frames.size()) - 2);
        return 0;
    }
    static int countVox(const VoxModel &m) {
        int n = 0;
        for (int z = 0; z < m.sz; ++z)
            for (int y = 0; y < m.sy; ++y)
                for (int x = 0; x < m.sx; ++x)
                    if (m.at(x, y, z)) ++n;
        return n;
    }

    // -----------------------------------------------------------------------
    // WHERE THE TWO ENDS OF A MODEL ACTUALLY ARE, in the box's own voxels.
    //
    // (user 2026-09-18: "the bullets seem to come from 1 voxel underneath the
    // tip of the guns point".)
    //
    // Walks in from each end of the DEPTH axis to the first slice with anything
    // in it, and takes the centre of that slice's occupied cells across the
    // other two. So for a gun: the middle of the barrel where the barrel stops,
    // and the middle of the stock where the stock stops -- not the middle of
    // whatever box the strip needed.
    //
    // THE DEPTH TERM IS THE OUTER FACE of that slice, which is why the two ends
    // are not symmetric: the low end's face is at the slice, the high end's is
    // one past it.
    //
    // A VoxModel IS Z-UP, so its y is the depth (Tool::sz) and its z is the
    // height (Tool::sy) -- the same swap toWorldWhole makes. Getting that the
    // other way round puts the muzzle out of the side of the gun.
    // -----------------------------------------------------------------------
    static void measureTips(const VoxModel &m, Tool *t) {
        if (!t) return;
        // The box's middle, which is what every non-gun keeps.
        t->tipLo[0] = t->tipHi[0] = 0.5f * float(m.sx);
        t->tipLo[1] = t->tipHi[1] = 0.5f * float(m.sz);
        t->tipLo[2] = 0.0f;
        t->tipHi[2] = float(m.sy);
        for (int dir = 0; dir < 2; ++dir) {
            for (int step = 0; step < m.sy; ++step) {
                const int y = dir ? (m.sy - 1 - step) : step;
                double ax = 0.0, az = 0.0;
                int n = 0;
                for (int z = 0; z < m.sz; ++z)
                    for (int x = 0; x < m.sx; ++x)
                        if (m.at(x, y, z)) {
                            ax += double(x) + 0.5;
                            az += double(z) + 0.5;
                            ++n;
                        }
                if (!n) continue;
                float *o = dir ? t->tipHi : t->tipLo;
                o[0] = float(ax / double(n));
                o[1] = float(az / double(n));
                o[2] = dir ? float(y + 1) : float(y);
                break;
            }
        }
    }

    // -----------------------------------------------------------------------
    // A GUN: ONE REST POSE WITH A RELOAD CYCLE BEHIND IT.
    //
    // (user 2026-09-18: "there are animations for the reload cycle. look in the
    // assault file.")
    //
    // add()'s job plus a strip, and the strip is why this is its own function
    // rather than an argument: the frames have to be padded into ONE SHARED BOX
    // before any of them is registered. Every held pose is measured from the
    // middle of that box -- see HeldItem::xform, and the toWorldWhole note in
    // World::addHeldVox -- so frames of different sizes are frames that each
    // move the gun.
    //
    // AND THE SHARED BOX IS TALLER THAN THE GUN, WHICH MOVES THE POSE. The rest
    // frame is 4 voxels tall and the strip's tallest is 6, so the box grows two
    // rows DOWNWARD and its centre drops one -- which lifts the gun one voxel
    // (10 cm at scale 1) up the screen for free. The rifle's two baked poses
    // are shifted by exactly that: see the y in app_load.inl's kRifleHip and
    // kRifleAds, both one lower than the numbers the user baked off the panel,
    // with the arithmetic written out there.
    //
    // A MISSING STRIP IS NOT AN ERROR. No reload directory, or files that will
    // not load, leaves `reloadFrames` at 0: the gun still fires and still
    // reloads, holding the rest pose for the whole cycle. A viewmodel that
    // refuses to load is a worse bug than one that does not animate.
    // -----------------------------------------------------------------------
    bool addGun(World &world, const char *name, const std::string &voxPath,
                const std::string &reloadDir, const HeldPose &pose, int selfMergeTol = 0,
                const HeldPose *adsPose = nullptr, float reloadMs = kReloadMs,
                int reloadRounds = 0) {
        std::string err;
        std::vector<VoxModel> frames(1);
        if (!voxLoad(voxPath, &frames[0], &err)) {
            std::fprintf(stderr, "v2: gun %s: %s -- skipped\n", voxPath.c_str(), err.c_str());
            return false;
        }
        for (const std::string &p : stripFiles(reloadDir)) {
            VoxModel m;
            if (!voxLoad(p, &m, &err)) {
                std::fprintf(stderr, "v2: gun reload frame %s: %s -- skipped\n", p.c_str(),
                             err.c_str());
                continue;
            }
            // IT HAS TO BE THE SAME GUN, and overlapping the rest pose at all is
            // the test -- see fitStrip, which finds WHERE it overlaps. A frame
            // that cannot be laid on the rest pose anywhere is not a frame of
            // this animation, and padding it into the shared box would put a
            // second gun in the hand for a tenth of a second.
            int fx = 0, fy = 0, fz = 0;
            if (fitFrame(frames[0], m, &fx, &fy, &fz) <= 0) {
                std::fprintf(stderr,
                             "v2: gun reload frame %s (%dx%dx%d) shares no voxel with the rest"
                             " pose -- skipped\n",
                             p.c_str(), m.sx, m.sy, m.sz);
                continue;
            }
            frames.push_back(std::move(m));
        }
        foldStrip(frames, selfMergeTol);
        // ONE GRID FOR THE WHOLE STRIP, with every frame laid where it lines up
        // with the rest pose. After this they are all the same size, which is
        // what the pose below is measured against.
        fitStrip(&frames);

        Tool t;
        t.name = name;
        t.takes = Takes::Nothing;
        t.pose = pose;
        if (adsPose) {
            t.ads = true;
            t.adsPose = *adsPose;
        }
        t.path = voxPath;
        t.reloadMs = reloadMs;
        t.reloadRounds = reloadRounds;
        // AFTER fitStrip, so the numbers are in the SHARED box the pose is
        // measured against -- see measureTips and HeldItem::muzzle.
        measureTips(frames[0], &t);
        for (size_t f = 0; f < frames.size(); ++f) {
            // EXACTLY, because foldStrip has already done the merging -- see
            // its note. Passing the tolerance on here would fold every frame a
            // second time, against itself.
            const int i = world.addHeldVox(frames[f],
                                           f ? (voxPath + " (reload)") : voxPath, &t.sx, &t.sy,
                                           &t.sz, 0);
            if (i < 0) return false;
            t.models.push_back(i);
        }
        t.reloadFrames = int(frames.size()) - 1;
        // WHERE THE OPENING STOPS AND THE ROUND STARTS -- see Tool::reloadLead
        // for why this is measured off the art rather than written down, and
        // why only a gun that loads round by round is split at all.
        t.reloadLead = reloadRounds > 0 ? measureLead(frames) : 0;
        tools_.push_back(t);
        std::printf("v2: gun %s  %d reload frames (%d opening), %dx%dx%d\n",
                    voxPath.c_str(), t.reloadFrames, t.reloadLead, t.sx, t.sy, t.sz);
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
    // -- ONE OFF THE PILE, WHATEVER BECOMES OF IT -------------------------
    //
    // Shared by dropSelected (which then throws it on the ground) and by a
    // BITE (which does not) -- see wantEat. The stack bookkeeping is the same
    // act either way and two copies of it is how a slot ends up `carried`
    // with a stack of zero, which draws nothing and cannot be scrolled off.
    //
    // Returns which slot gave one up, or -1.
    int consumeSelected() {
        if (!ready() || !tools_[size_t(sel_)].carried) return -1;
        if (tools_[size_t(sel_)].models.empty()) return -1;
        const int gone = sel_;
        // ONE OF THE PILE, NOT THE PILE. Eating the top apple of nine leaves
        // eight in the hand and the hand where it was -- only the LAST one
        // takes the slot out of the wheel and moves the selection on.
        if (tools_[size_t(gone)].stack > 1) {
            --tools_[size_t(gone)].stack;
            drawing_ = false;
            loosed_ = false;
            return gone;
        }
        tools_[size_t(gone)].stack = 0;
        tools_[size_t(gone)].carried = false;
        drawing_ = false;
        loosed_ = false;
        // -- ...AND NEITHER A BITE NOR A RELOAD SURVIVES THIS -------------
        //
        // (user 2026-09-21: "when planting seeds, when planting the last
        //  seed, I had a steak in my hand, and the player ate it while the
        //  player placed the last seed".)
        //
        // THE LAST ONE OF A STACK MOVES THE HAND, and this moves it by
        // writing sel_ directly -- so it goes round cycle() and select(),
        // which are the two places that knew to cancel. The note over
        // eating_ already states the rule this was breaking: a clock that
        // survives the item it was started on finishes a bite on whatever is
        // now in the hand.
        //
        // AND eating_ IS NOT CLEARED BY LETTING GO. Start a bite on the
        // steak, release before it finishes, scroll to the seeds, plant them
        // -- the last seed hands the steak back to a clock that ran out
        // minutes ago, and wantEat swallows it on the very next frame. That
        // is the report, exactly: one seed planted, one steak eaten.
        cancelEat();
        cancelReload();
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

    int dropSelected() {
        // AN EMPTY HAND CANNOT BE PUT DOWN. Without this, Q on the fourth slot
        // marked it uncarried -- taking it out of the wheel for good, since
        // nothing can ever pick it up again -- and tossed a drop whose model is
        // -1, which is an item lying in the wood that cannot be drawn or
        // collected. Having no models is exactly what makes it the empty slot,
        // so it is the right thing to ask. consumeSelected asks it.
        return consumeSelected();
    }

    // -----------------------------------------------------------------------
    // ...AND PUT ONE IN THE KIT WITHOUT PUTTING IT IN THE WHEEL.
    //
    // take() is how a tool leaves the hand and it does two things at once --
    // clears `carried` AND moves the selection on. A slot that has never been
    // held needs only the first: the wheat and the seeds are loaded at startup
    // so that picking one up costs no load, and until you cut some there is
    // nothing to scroll to. Calling take() for that would also change what the
    // hand opens with, which is the one thing that must not move.
    // -----------------------------------------------------------------------
    void stow(int tool) {
        if (tool < 0 || tool >= int(tools_.size())) return;
        tools_[size_t(tool)].carried = false;
        tools_[size_t(tool)].stack = 0;
        if (tool == sel_) cancelEat();   // see cycle
    }

    // ...and take it back. The hand only changes to it if it was empty, so
    // walking over a pick while swinging an axe does not swap the axe out.
    // ...and how deep one goes. v1's STACK_MAX, which it moved from 8 to 10.
    static constexpr int kStackMax = 10;

    void give(int tool) {
        if (tool < 0 || tool >= int(tools_.size())) return;
        Tool &t = tools_[size_t(tool)];
        // A SECOND ONE STACKS; THE FIRST ONE IS A PICKUP. Walking over wheat
        // while already carrying some must not reset the pile to one, which is
        // what the bare `carried = true` did -- every stalk you gathered was
        // the only stalk you had.
        t.stack = t.carried ? mini(kStackMax, t.stack + 1) : 1;
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
    // -- ...AND WHETHER IT IS SOMETHING YOU CAN EAT ------------------------
    bool holdingFood() const { return ready() && tools_[size_t(sel_)].food; }
    bool eating() const { return eating_; }
    // 0 at the first bite, 1 when it is finished -- for the HUD and the test.
    float eatProgress() const {
        if (!eating_) return 0.0f;
        return clampf(float((nowMs_ - eatT0_) / double(kEatMs)), 0.0f, 1.0f);
    }

    // THE RIGHT BUTTON, ON A FOOD. Returns true on the frame the bite
    // FINISHES, which is the frame the caller pays out on.
    //
    // ONE PRESS, NOT A HOLD (user 2026-09-17: "when eating something, the user
    // should only have to press right click once, not hold it down").
    //
    // IT WAS A HOLD because that is what v1 does and what the bow beside it
    // does -- and a bow is the wrong model for this. A draw is a thing you
    // aim, so holding it IS the action; a bite is a thing you commit to, and
    // asking someone to keep a button down for nine tenths of a second while
    // nothing they do changes the outcome is a hold that buys nothing.
    //
    // SO THE PRESS ARMS IT AND THE CLOCK FINISHES IT. `down` is only read on
    // its RISING EDGE: once a bite has started, releasing does nothing and
    // holding does nothing, and the food is eaten kEatMs later either way.
    //
    // THE EDGE IS TRACKED HERE rather than by the caller, because `eating_`
    // and the thing that starts it belong to the same object -- a caller that
    // owned the edge would have to know when a bite is already running to
    // avoid restarting one on the next press, which is exactly this state.
    // -----------------------------------------------------------------------
    // THIS BUTTON IS ALREADY DOWN, AND IT WAS DOWN FOR SOMETHING ELSE.
    //
    // (user 2026-09-17: "as soon as I right click to pick it up, it
    //  automatically starts eating it".)
    //
    // THE PICK AND THE BITE ARE THE SAME BUTTON, which is what makes this
    // happen and is not itself wrong: right-click takes the fruit, right-click
    // eats it. But wantEat starts a bite on the RISING EDGE, and until the
    // fruit was picked there was no food in the hand, so eatWasDown_ was false
    // -- so the press that put the apple in your hand read as a fresh press on
    // an apple and started the mouthful in the same frame.
    //
    // So the pick says "this press is spent". The next one eats. Letting go
    // and clicking again is what a player does anyway; taking a bite out of
    // something the instant you pick it up is not.
    void spendEatPress() { eatWasDown_ = true; }

    bool wantEat(bool down, double nowMs) {
        nowMs_ = nowMs;
        const bool pressed = down && !eatWasDown_;
        eatWasDown_ = down;
        if (!holdingFood()) {
            eating_ = false;
            return false;
        }
        if (!eating_) {
            // A FRESH PRESS STARTS ONE, and nothing else does.
            if (!pressed) return false;
            eating_ = true;
            eatT0_ = nowMs;
            bitNow_ = true;   // the caller's cue to play the chew -- see bitNow
            return false;
        }
        if (nowMs - eatT0_ < double(kEatMs)) return false;
        // ...AND IT IS SWALLOWED. One from the stack, and the clock is reset
        // rather than left finished, or holding the button would eat the whole
        // stack in one press.
        eating_ = false;
        consumeSelected();
        return true;
    }

    // Anything that changes what is in the hand has to say so -- see eating_.
    void cancelEat() { eating_ = false; }

    // TRUE ON THE ONE FRAME A BITE BEGAN, and false ever after -- the same
    // shape drewNow() has for the bow, and for the same reason: the sound
    // belongs to the EDGE, and an edge read twice is an edge that fires twice.
    bool bitNow() {
        const bool b = bitNow_;
        bitNow_ = false;
        return b;
    }

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
    // -- ...AND WHAT IS ACTUALLY ON SCREEN, WHICH IS NOT ALWAYS THE SAME -----
    //
    // For the render alone -- the model, the pose and the motion vector's
    // identity. During the drop (see kSwapOutMs) the hand already holds the new
    // tool as far as the swing, the HUD and every action are concerned, and the
    // OLD one is still falling out of frame. Two questions, two answers; the
    // alternative is holding sel_ back for a tenth of a second and making every
    // other reader of it wrong instead.
    int drawnTool() const { return swapOut_ >= 0 && swapOutK() >= 0.0f ? swapOut_ : sel_; }
    const char *name() const { return ready() ? tools_[size_t(sel_)].name : "empty"; }

    // The pose of whatever is in the hand, for the menu to edit.
    HeldPose &pose() { return tools_[size_t(sel_)].pose; }
    // -- ...AND THE SIGHTED ONE, WHICH IS A SECOND BAKE ------------------
    //
    // (user 2026-09-17: "also let me adjust the aim down sights position in
    // the settings menu".)
    //
    // The hip pose and the sighted pose are two independent seven-number bakes
    // (see Tool::adsPose for why it is not stored as an offset), so they get
    // two independent panels. `aimable` is what says whether the second one is
    // worth drawing -- an axe has no sights.
    HeldPose &adsPose() { return tools_[size_t(sel_)].adsPose; }
    bool aimable() const { return ready() && tools_[size_t(sel_)].ads; }
    // How far up to the eye the tool has travelled, 0..1. Read by the panel so
    // that somebody tuning the sighted pose can see whether they are actually
    // looking at it -- seven sliders that move nothing are seven sliders
    // nobody can use, which is the stack card's own note.
    float adsAmount() const { return ads_; }

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
        const int was = sel_;
        // SKIP WHAT IS NOT BEING CARRIED. The wheel walks the kit, and a tool
        // lying on the ground is not in it -- stopping on an empty hand would
        // read as the wheel being broken.
        for (int i = 0; i < n; ++i) {
            sel_ = ((sel_ + d) % n + n) % n;
            if (tools_[size_t(sel_)].carried) break;
        }
        armSwap(was);
        // A BITE DOES NOT SURVIVE THE HAND CHANGING. Scrolling from a
        // half-eaten apple to an orange with the button still down would
        // otherwise finish the apple's clock on the orange -- see eating_.
        cancelEat();
        cancelReload();   // ...and neither does a reload -- see cancelReload
    }
    void select(int i) {
        if (i < 0 || i >= int(tools_.size()) || i == sel_) return;
        const int was = sel_;
        sel_ = i;
        armSwap(was);
        cancelEat();   // see cycle
        cancelReload();
    }

    // THE TOOL THAT IS LEAVING GOES OUT FIRST -- see kSwapOutMs. `was` is what
    // the hand held a moment ago; it keeps being DRAWN, sinking, until the drop
    // has carried it off the bottom of the frame, and only then does the model
    // change. A hand that was empty has nothing to lower and starts the rise at
    // once, which is what drawing a tool for the first time should look like.
    void armSwap(int was) {
        swapT0_ = nowMs_;
        swapOut_ = (was >= 0 && was < int(tools_.size()) && was != sel_ &&
                    tools_[size_t(was)].carried)
                       ? was
                       : -1;
        if (swapOut_ >= 0) swapT0_ = nowMs_ + kSwapOutMs;   // the bounce starts after the drop
    }
    // HOW FAR THROUGH THE DROP, 0 at the start and 1 once it is out of frame.
    // Negative swapT means the drop is still running -- see armSwap, which puts
    // the bounce's clock in the future by exactly its length.
    float swapOutK() const {
        if (swapOut_ < 0) return -1.0f;
        const double left = swapT0_ - nowMs_;
        if (left <= 0.0) return -1.0f;
        return clampf(float(1.0 - left / kSwapOutMs), 0.0f, 1.0f);
    }

    // -----------------------------------------------------------------------
    // A ROUND JUST WENT OFF -- kick the thing in the hand.
    //
    // (user 2026-09-17: "have it shoot bullets then give the gun recoil when it
    // shoots".)
    //
    // ONE TIMESTAMP, LIKE THE SWING AND THE SWAP. Every movement in this file
    // is a curve read off a start time rather than a velocity integrated per
    // frame, and for the reason the swap's note gives: a curve is the same
    // shape at any frame rate and cannot drift. Re-arming it mid-recoil simply
    // restarts the curve, which is what automatic fire should look like.
    void kick() { recoilT0_ = nowMs_; }

    // -----------------------------------------------------------------------
    // THE RELOAD CYCLE -- ARM IT, ASK ABOUT IT, AND CATCH ITS END.
    //
    // (user 2026-09-18: "the number goes down until 0, where the gun then
    // reloads ... so also implement a reload function while you are at it.")
    //
    // THE CLOCK IS HERE AND THE MAGAZINE IS NOT. What is in the gun is the
    // App's number -- it is spent by fireRifle, refilled by the door into the
    // level, and drawn beside the hand -- and this file owns only how long the
    // gun is busy and which frame that puts on screen. Two clocks would be one
    // clock too many: nowMs_ is the same one the swing, the swap, the recoil
    // and the bite are all timed off, so the animation cannot drift from the
    // moment the rounds come back.
    //
    // reloadDone() IS AN EDGE AND IT CONSUMES ITSELF. The App polls it once a
    // frame and refills on the one frame it answers true -- the same shape as
    // update()'s impact return, and for the same reason: "is it finished" asked
    // every frame of a flag that stays set would refill the magazine for ever.
    // -----------------------------------------------------------------------
    // -- ...AND IT MAY TAKE SEVERAL TURNS OF THE STRIP -------------------
    //
    // (user 2026-09-18: "it needs to play multiple times depending on how many
    // shots have been powered. its a standard revolver with 6 rounds.")
    //
    // `cycles` IS HOW MANY TIMES THE ANIMATION RUNS, and the caller works it
    // out because the caller is the one holding the magazine -- see
    // App::reloadGun, which asks for one turn per empty chamber. A gun with a
    // magazine asks for one.
    //
    // reloadDone() THEN FIRES ONCE PER ROUND rather than once per reload, which
    // is what makes a revolver a revolver: a round goes in, the count beside
    // the hand steps up, and the loading frames start again. The clock is
    // ADVANCED rather than reset -- reloadT0_ += ms -- so six rounds take
    // exactly six passes however the frames fall.
    //
    // -- ...AND THE ACTION STAYS OPEN ACROSS ALL OF THEM ------------------
    //
    // (user 2026-09-18: "the reload needs to stay open as it cycles through
    // the bullets. it currently closes the chamber everytime it reloads 1
    // bullet.")
    //
    // WHICH IS WHY THE CYCLE HAS PHASES AND NOT JUST TURNS. The strip's
    // leading frames are the cylinder swinging OUT (Tool::reloadLead), so
    // restarting the strip per round restarts from the gun CLOSED -- the
    // chamber slamming shut and flying open again between every round. The
    // opening is played once, the loading frames are what repeats, and the
    // opening backwards is the close. One swing out, one swing in, six rounds.
    //
    // A GUN WITH NO LEAD NEVER LEAVES Loading, and that phase is then the whole
    // strip -- so a magazine change is one pass of everything, byte for byte
    // the cycle this replaced.
    bool startReload(int cycles = 1) {
        if (reloading_ || !ready()) return false;
        reloading_ = true;
        reloadT0_ = nowMs_;
        reloadLeft_ = maxi(1, cycles);
        reloadPhase_ = reloadLeadFrames() > 0 ? RPhase::Opening : RPhase::Loading;
        return true;
    }
    bool reloading() const { return reloading_; }
    // How long ONE PASS OF THE WHOLE STRIP takes for whatever is in the hand.
    // Still the tool's own `reloadMs`, unchanged: what the phases divide up is
    // this number, so the drawn frames keep the pace they were authored at.
    float reloadCycleMs() const {
        if (!ready()) return kReloadMs;
        const float ms = tools_[size_t(sel_)].reloadMs;
        return ms > 1.0f ? ms : kReloadMs;
    }
    // -- THE PHASES ARE MEASURED IN FRAMES, NOT IN FRACTIONS OF A CYCLE ----
    //
    // ONE FRAME TAKES THE SAME TIME IN ALL THREE, which is the whole of the
    // pacing: 450 ms over ten frames is 45 ms each, so the pistol's opening is
    // 4 x 45, each round is 6 x 45, and the close is 4 x 45 again. A full six
    // is then 44 frames rather than 60 -- the animation stopped repeating the
    // part that was wrong, and got shorter by exactly that part.
    int reloadLeadFrames() const {
        if (!ready()) return 0;
        const Tool &t = tools_[size_t(sel_)];
        return t.reloadFrames > 1 ? mini(maxi(t.reloadLead, 0), t.reloadFrames - 1) : 0;
    }
    float reloadFrameMs() const {
        const int n = ready() ? tools_[size_t(sel_)].reloadFrames : 0;
        return n > 0 ? reloadCycleMs() / float(n) : reloadCycleMs();
    }
    // How long the phase that is running now lasts.
    float reloadPhaseMs() const {
        const int lead = reloadLeadFrames();
        const int n = ready() ? tools_[size_t(sel_)].reloadFrames : 0;
        const int frames = reloadPhase_ == RPhase::Loading ? maxi(1, n - lead) : maxi(1, lead);
        return float(frames) * reloadFrameMs();
    }
    // How far through THAT phase, 0..1.
    float reloadAmount() const {
        if (!reloading_) return 0.0f;
        const double ms = double(reloadPhaseMs());
        return ms > 0.0 ? clampf(float((nowMs_ - reloadT0_) / ms), 0.0f, 1.0f) : 1.0f;
    }
    // -- TRUE ON THE FRAME A ROUND ARRIVES, and on no other ----------------
    //
    // The opening and the close pass through here too and answer FALSE: they
    // are the gun moving, not the magazine filling, and a caller that loaded on
    // either would put a round in before the cylinder was out. The loop is for
    // the phases that load nothing -- a hitch long enough to swallow the whole
    // opening must still arrive at the first round, not sit on the boundary
    // for a frame. Every branch either advances a phase or returns, so it ends.
    bool reloadDone() {
        if (!reloading_) return false;
        for (;;) {
            const double ms = double(reloadPhaseMs());
            if (ms > 0.0 && nowMs_ - reloadT0_ < ms) return false;
            reloadT0_ += ms;
            if (reloadPhase_ == RPhase::Opening) {
                reloadPhase_ = RPhase::Loading;
                continue;
            }
            if (reloadPhase_ == RPhase::Closing) {
                reloading_ = false;
                return false;
            }
            // A ROUND IS IN. Stay in Loading while there are chambers left --
            // that is the strip's loading frames playing again, from a gun that
            // is already open.
            if (--reloadLeft_ > 0) return true;
            if (reloadLeadFrames() > 0) reloadPhase_ = RPhase::Closing;
            else reloading_ = false;
            return true;
        }
    }
    // ...and how many rounds are still to come, so a caller can tell the last
    // one from the rest without counting them itself. ZERO WHILE IT CLOSES,
    // which is the difference between "the gun is busy" (reloading()) and
    // "more rounds are coming" -- the badge has stopped moving by then.
    int reloadLeft() const { return reloading_ ? reloadLeft_ : 0; }
    // -- WHICH FRAME OF THE STRIP IS ON SCREEN, 0-BASED --------------------
    //
    // model() ASKS THIS RATHER THAN WORKING IT OUT, so a log and the hand hold
    // the same gun -- a diagnostic that computes the frame a second time can
    // agree with itself while disagreeing with the render, which is the one
    // thing it exists not to do. -1 when nothing is reloading.
    //
    // THE CLOSE IS THE OPENING BACKWARDS, and that is the whole of it: the art
    // has no closing frames because it needs none -- a cylinder coming in is
    // one going out with the clock reversed. It ends on the strip's first
    // frame, which is the gun shut, which is the rest pose model() falls back
    // to a frame later.
    int reloadStrip() const {
        if (!reloading_ || !ready()) return -1;
        const Tool &t = tools_[size_t(sel_)];
        if (t.reloadFrames <= 0) return -1;
        const int lead = reloadLeadFrames();
        const int span = reloadPhase_ == RPhase::Loading ? maxi(1, t.reloadFrames - lead)
                                                        : maxi(1, lead);
        const int step = mini(span - 1, int(reloadAmount() * float(span)));
        if (reloadPhase_ == RPhase::Opening) return step;
        if (reloadPhase_ == RPhase::Closing) return maxi(0, lead - 1 - step);
        return lead + step;
    }
    const char *reloadPhaseName() const {
        if (!reloading_) return "rest";
        return reloadPhase_ == RPhase::Opening   ? "opening"
               : reloadPhase_ == RPhase::Closing ? "closing"
                                                 : "loading";
    }
    // THE HAND CHANGING CANCELS IT, exactly as it cancels a bite -- see
    // cancelEat. A reload half played on a gun you have scrolled away from
    // would finish on whatever is in your hand now, and refill a magazine that
    // is not on screen.
    void cancelReload() { reloading_ = false; }

    // -----------------------------------------------------------------------
    // TAKE THE WHOLE WHEEL AWAY, AND GIVE IT BACK EXACTLY.
    //
    // (user 2026-09-17: "remove all the tools from the hand on the nuketown
    // level. only the gun should be in the hand.")
    //
    // A SNAPSHOT RATHER THAN stow()/give(), and the stack is why. `give` sets a
    // stack of one on anything that was not carried -- see its note -- so
    // walking into the level with nine stalks of wheat and back out again would
    // hand you one. carried AND stack are the pair that describe a slot, so the
    // pair is what is saved and the pair is what comes back.
    //
    // The gun itself is not special-cased here: the caller stows everything,
    // gives the rifle, and on the way out restores this -- which puts the kit
    // back exactly as it was and leaves the rifle wherever the restore says,
    // which is "not carried", because it never was in the wood.
    std::vector<std::pair<bool, int>> snapshotKit() const {
        std::vector<std::pair<bool, int>> out;
        out.reserve(tools_.size());
        for (const Tool &t : tools_) out.emplace_back(t.carried, t.stack);
        return out;
    }
    void restoreKit(const std::vector<std::pair<bool, int>> &k) {
        for (size_t i = 0; i < tools_.size() && i < k.size(); ++i) {
            tools_[i].carried = k[i].first;
            tools_[i].stack = k[i].second;
        }
        // THE HAND MAY BE POINTING AT SOMETHING THAT IS GONE. Restoring the kit
        // can uncarry whatever was selected, and a selection on an uncarried
        // slot draws nothing and scrolls oddly; step it to the next real one.
        if (!ready() || tools_[size_t(sel_)].carried) return;
        cycle(1);
    }

    // -----------------------------------------------------------------------
    // THE MUZZLE, IN THE WORLD -- where a round is born.
    //
    // (user 2026-09-17: "the bullet should appear at the tip of the gun".)
    //
    // THE FAR END, FOUND RATHER THAN NAMED. The model's long axis is its DEPTH
    // (see the pose note in app.h -- the gun is authored 3 x 4 x 11) but WHICH
    // end of it points away from the player is a function of the pose's roll,
    // and that roll was flipped once already. So both ends are computed and the
    // one further from the eye wins: re-pose the gun, turn it end for end, and
    // the muzzle follows without this function being told.
    //
    // It costs a full xform() -- the swing curve, the bob, the breath, the
    // swap spring -- which is the point: the round leaves from where the barrel
    // actually IS this frame, recoil and all, not from where the pose says it
    // rests.
    bool muzzle(const V6Camera &cam, float bobPhase, float bobAmp, Vec3 *out) const {
        if (!out || !ready()) return false;
        const HeldXform hx = xform(cam, bobPhase, bobAmp);
        if (!hx.show) return false;
        const Tool &t = tools_[size_t(sel_)];
        const Vec3 colX(hx.m[0], hx.m[3], hx.m[6]);
        const Vec3 colY(hx.m[1], hx.m[4], hx.m[7]);
        const Vec3 colZ(hx.m[2], hx.m[5], hx.m[8]);
        // -- THE BARREL, NOT THE MIDDLE OF THE BOX -------------------------
        //
        // (user 2026-09-18: "the bullets seem to come from 1 voxel underneath
        // the tip of the guns point".)
        //
        // AND IT IS EXACTLY ONE VOXEL, WHICH IS THE PADDING. This used to take
        // the box's own middle -- 0.5 * sx, 0.5 * sy -- which is the right
        // point only for a model that fills its box. A gun does not: its frames
        // share a box sized to the WHOLE STRIP (see fitStrip), and the rifle's
        // is two rows deeper than the gun because the magazine drops out of the
        // bottom during the reload. So the box's middle sits a voxel below the
        // gun, and the round left from under the barrel.
        //
        // MEASURED OFF THE REST POSE INSTEAD, at load: `tipLo` and `tipHi` are
        // the centre of the first non-empty SLICE from each end of the depth
        // axis, in box voxels, with the depth of that slice's outer face. That
        // is the barrel's own tip whatever the box around it is doing, and it
        // is right for the pistol too, whose box is a row longer than the gun
        // at one end because a reload frame reaches further.
        const Vec3 base(hx.tx, hx.ty, hx.tz);
        const Vec3 a2 = base + colX * t.tipLo[0] + colY * t.tipLo[1] + colZ * t.tipLo[2];
        const Vec3 b2 = base + colX * t.tipHi[0] + colY * t.tipHi[1] + colZ * t.tipHi[2];
        const Vec3 eye(cam.pos.x, cam.pos.y, cam.pos.z);
        *out = lengthSq(a2 - eye) > lengthSq(b2 - eye) ? a2 : b2;
        return true;
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
        // THE ONE THAT IS ON SCREEN -- see drawnTool. A tool on its way out
        // keeps its own model until it has left, which is the whole of why the
        // change of geometry is no longer something an upscaler ever sees.
        const Tool &t = tools_[size_t(drawnTool())];
        if (t.models.empty()) return -1;
        // -- A FOOD SHOWS HOW MUCH OF IT IS LEFT ---------------------------
        //
        // Picked from the bite's PROGRESS rather than from a frame clock of
        // its own -- see kEatFrames for why that is the whole difference from
        // v1 here. Not eating is frame 0, which is the whole food, so this is
        // also the ordinary held model and the two can never disagree.
        if (t.food) {
            const float k = eating_ ? clampf(float((nowMs_ - eatT0_) / double(kEatMs)), 0.0f, 1.0f)
                                    : 0.0f;
            const size_t f = size_t(mini(kEatFrames - 1, int(k * float(kEatFrames - 1) + 0.5f)));
            return t.models[f < t.models.size() ? f : 0];
        }
        // -- A GUN SHOWS THE MAGAZINE COMING OUT AND GOING BACK IN ---------
        //
        // STEPPED OFF THE CYCLE'S OWN CLOCK, exactly as the bow's draw is: the
        // frame is PICKED, never tweened, so nine drawn frames read as nine
        // drawn frames. `reloading_` is the whole gate -- at rest, and for any
        // tool with no strip, this falls through to models[0] like everything
        // else in the kit.
        //
        // int(k * n) RATHER THAN A ROUND, so each of the n frames owns an equal
        // slice of the cycle and the last one is on screen until the magazine
        // is actually full. Rounding would give the first and last frames half
        // a slice each, which is a reload that starts and ends on a flicker.
        if (reloading_ && t.reloadFrames > 0) {
            // THE PHASE'S OWN CLOCK, and reloadT0_ is the start of the phase
            // that is running -- so this line never has to know how many rounds
            // are left, only which third of the strip it is in.
            const size_t i = size_t(1 + reloadStrip());
            return t.models[i < t.models.size() ? i : 0];
        }
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
        // -- AIMING DOWN THE SIGHTS, ON THE SAME BUTTON AS THE DRAW ----------
        //
        // Eased rather than switched: a gun that teleports to the eye reads as
        // a glitch, and every other movement in this file is a curve. 90 ms up
        // and the same down, which is about as fast as a viewmodel can travel
        // without the eye losing it -- kSwapMs is 240 and that is a change of
        // hands, a bigger thing than this.
        //
        // EXPONENTIAL, FRAME-RATE INDEPENDENT. `1 - exp(-dt/tau)` rather than a
        // fixed step per frame, or the aim is twice as fast at 120 fps as it is
        // at 60 -- which is the bug the bob amplitudes above were written to
        // avoid and it would be a shame to reintroduce it one function later.
        {
            // THE BUTTON OR THE PANEL'S TOGGLE -- see adsHold. Both mean
            // "bring it up", so they are one condition rather than two states
            // that could disagree about where the gun is.
            const bool wantAds =
                (drawHeld || adsHold) && shown && ready() && tools_[size_t(sel_)].ads;
            const float tau = 0.090f;
            const float k = 1.0f - expf(-maxf(0.0f, dt) / tau);
            ads_ += ((wantAds ? 1.0f : 0.0f) - ads_) * k;
            if (ads_ < 0.001f) ads_ = 0.0f;
            if (ads_ > 0.999f) ads_ = 1.0f;
        }
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
        const Tool &tool = tools_[size_t(drawnTool())];
        // -- THE HIP POSE, OR SOMEWHERE ON THE WAY TO THE SIGHTED ONE --------
        //
        // A plain lerp of all seven numbers. The two rotations a gun uses are
        // identical between its poses so nothing here has to think about angle
        // wrap-around; if an aimable tool is ever given poses that differ in
        // yaw by more than half a turn, this is the line that would need to
        // shortest-arc them.
        HeldPose posed = tool.pose;
        if (tool.ads && ads_ > 0.0f) {
            const HeldPose &a = tool.adsPose;
            const float u = ads_;
            auto mix = [u](float from, float to) { return from + (to - from) * u; };
            posed.x = mix(tool.pose.x, a.x);
            posed.y = mix(tool.pose.y, a.y);
            posed.z = mix(tool.pose.z, a.z);
            posed.yaw = mix(tool.pose.yaw, a.yaw);
            posed.pitch = mix(tool.pose.pitch, a.pitch);
            posed.roll = mix(tool.pose.roll, a.roll);
            posed.scale = mix(tool.pose.scale, a.scale);
        }
        const HeldPose &pose = posed;
        // AND THE HAND GOES STILL WHILE IT IS UP. The stride bob and the idle
        // breath are what make a carried thing read as carried; down the sights
        // they read as a wobble you cannot aim through. Damped to a sixth
        // rather than to nothing, so the gun is steady without being welded to
        // the screen.
        const float steady = 1.0f - 0.85f * ads_;
        bobAmp *= steady;

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
        float swPitch = -0.9f * wind + 1.35f * chop;
        float hx = pose.x * (1.0f + 0.06f * wind - 0.85f * chop);
        float hy = pose.y + 0.22f * wind - 0.18f * chop;
        float hz = pose.z - 0.05f * wind + 0.18f * chop;

        // -- ...AND THE RECOIL, WHICH IS A THIRD CURVE ON THE SAME THREE -----
        //
        // (user 2026-09-17: "give the gun recoil when it shoots".)
        //
        // SHARP OUT, SLOW BACK, which is the whole of what recoil looks like:
        // the gun is thrown back and up in kRecoilOutS -- about two frames, so
        // it reads as an impulse rather than a movement -- and then eases home
        // over five times that. A symmetric curve reads as a bounce, which is
        // the swap's job and not this one.
        //
        // IT RIDES ON TOP OF THE SWING rather than replacing it, because the
        // two are independent: nothing stops a tool being swung and fired, and
        // if anything ever is, adding the two displacements is the answer that
        // does not need a rule.
        //
        // THE PITCH IS THE PART YOU SEE. Moving the anchor alone slides the gun
        // about the screen; tipping its nose up is what reads as a gun going
        // off, and it is one term because swPitch is already in the rotation
        // below.
        {
            const float rt = float(nowMs_ - recoilT0_) * 0.001f;
            if (rt >= 0.0f && rt < kRecoilOutS + kRecoilBackS) {
                const float env = rt < kRecoilOutS
                                      ? rt / kRecoilOutS
                                      : 1.0f - (rt - kRecoilOutS) / kRecoilBackS;
                const float e = env * env * (3.0f - 2.0f * env);   // smooth both ends
                hz -= kRecoilBackVox * e;   // back toward the eye
                hy += kRecoilUpVox * e;     // ...and up
                swPitch -= kRecoilPitch * e;
            }
        }

        // -- the swap, which bounces -----------------------------------------
        // See kSwapW: this is the spring's step response, and the sign is what
        // makes it a bounce rather than a rise -- the term is SUBTRACTED, so
        // x(t) crossing zero carries the pose up past where it rests before it
        // comes back down onto it.
        const float swapT = float(nowMs_ - swapT0_) * 0.001f;
        float swapF = 0.0f;
        // ...AND NOT BEFORE IT STARTS. armSwap puts this clock in the FUTURE by
        // the length of the drop, so swapT is negative for every frame of it --
        // and the spring's exp(-zeta*w*t) is exp(+...) there, which does not
        // taper off, it detonates. The drop owns those frames; the bounce takes
        // over at zero.
        if (swapT >= 0.0f && swapT < kSwapSettleMs * 0.001f) {
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
        // -- ...AND THE OTHER HALF OF IT, WHICH IS THE TOOL LEAVING ---------
        //
        // See kSwapOutMs. EASED IN rather than linear: a tool that starts
        // falling at full speed reads as dropped, and what this is animating is
        // a hand being lowered. k*k is the first half of the same smoothstep
        // the rest of this file uses and is the right half -- it leaves slowly
        // and is moving fastest as it goes off the bottom, where nothing can
        // see it stop.
        const float outK = swapOutK();
        if (outK >= 0.0f) hy -= kSwapOutVox * outK * outK;
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
              sway * steady;
        hy += (-fabsf(cosf(bobPhase)) * 0.112f * bobAmp +
               (sinf(ms * 0.0017f + 0.9f) * 0.0138f + sinf(ms * 0.00091f) * 0.0072f) *
                   live_) *
              sway * steady;

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
    // WHICH TOOL IS STILL BEING DRAWN WHILE IT LEAVES, and -1 once it has gone.
    // Only the RENDER reads it: sel_ changes on the frame the wheel turns, so
    // the swing, the HUD and every action are exactly as they were and the two
    // halves of the swap cannot disagree about what is in the hand.
    int swapOut_ = -1;
    bool wasShown_ = true;
    // How alive the hand is, 0..1 -- eased, and the gate on the idle sway.
    float live_ = 0.0f;

    // -- the bow ------------------------------------------------------------
    int bowFrames_ = 0;
    bool drawing_ = false;  // the right button is down on a bow
    // WHEN THE LAST ROUND WENT OFF. Far enough in the past that nothing is
    // recoiling on the first frame. See kick() and the recoil block in xform().
    double recoilT0_ = -1.0e9;
    // THE RELOAD, and unlike every other clock in this file it has a FLAG
    // beside it. The swing, the swap and the recoil all run once off a
    // timestamp and are over when their curve is; the reload has to be
    // cancellable mid-cycle -- scrolling off the gun ends it -- and "cancelled"
    // and "finished long ago" are the same timestamp. See startReload.
    bool reloading_ = false;
    double reloadT0_ = -1.0e9;
    // ROUNDS STILL TO GO IN. One turn for a magazine, one per empty chamber
    // for a revolver -- see startReload.
    int reloadLeft_ = 0;
    // WHICH THIRD OF THE STRIP IS ON SCREEN. Opening runs once, Loading runs
    // once per round, Closing runs the opening backwards -- see reloadPhase_'s
    // own note in reloadDone, and Tool::reloadLead for why a strip is three
    // things. A gun with no lead never leaves Loading, which is one phase for
    // the whole cycle and is what a magazine change already was.
    enum class RPhase { Opening, Loading, Closing };
    RPhase reloadPhase_ = RPhase::Loading;
    // ...and how far up to the eye an aimable tool has travelled -- 0 at the
    // hip, 1 down the sights. Eased in update(); see Tool::ads.
    float ads_ = 0.0f;
    bool loosed_ = false;   // shot, and not yet settled back to rest
    ArrowOffset arrow_;     // where the nocked arrow sits, in voxels
    bool drewNow_ = false;  // the pull began this frame
    bool nockedNow_ = false;  // ...and the string settled back this frame
    double bowT0_ = -1.0e9, bowRel_ = -1.0e9;
    // -- THE BITE ---------------------------------------------------------
    //
    // `eating_` is the right button held on a food and `eatT0_` is when it
    // went down. Both are cleared by anything that could make the food in hand
    // stop being the food in hand -- a scroll, a drop, a stow -- because a
    // clock that survives the item it was started on finishes a bite on
    // whatever is now in the hand. See cancelEat.
    bool eating_ = false;
    // The right button's state LAST frame, so a bite starts on the press
    // rather than on the hold -- see wantEat.
    bool eatWasDown_ = false;
    bool bitNow_ = false;
    double eatT0_ = -1.0e9;

    bool swungNow_ = false;
};

}  // namespace v2
