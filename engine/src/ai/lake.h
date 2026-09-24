// ---------------------------------------------------------------------------
// lake.h -- what lives ON and IN the water: lily pads, salmon, dragonflies.
//
// Ported from the JS engine (game/index.html), which is the reference for all
// three. Its own structure is a single `wbf` array of creature records
// discriminated by `B.kind`, with one enormous tick that branches on it; that
// does not survive the move to C++ intact and should not, so what is ported is
// each creature's BEHAVIOUR and its CONSTANTS, not the dispatch.
//
// THE THREE SHARE ONE THING AND IT IS THE REASON THEY SHARE A FILE: all of them
// need to know where the water is, every frame, cheaply. VoxelTerrain answers
// that honestly but slowly -- three noise evaluations and a depth rule per
// column -- and a fish asks it a dozen times a tick for its whiskers. So the
// water is sampled ONCE into a small local field (see WaterField) and all three
// read that.
//
// THEY ARE REAL INSTANCES, not sprites: they go in the flyer band, wear the
// same materials the wood does, and are lit by the same path tracer as
// everything else -- which is what "make sure they use the same lighting pass
// as the tools and everything else" asks for. Nothing here has its own shader.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/world.h"
#include "player/collide.h"
#include "world/shore.h"
#include "voxel/vox.h"
#include "world/voxelworld.h"

namespace v2 {

// -- THE MODELS -------------------------------------------------------------
//
// assets/life/salmon holds 00..11 plus base.vox, and base is SOURCE ART rather
// than a frame -- the JS engine skips it by name and so does this. The
// dragonfly is six frames the same way. The pads are three separate models,
// not a strip.
inline constexpr int kSalmonFrames = 12;
// ...AND THE BETTA HAS EIGHT, NOT TWELVE. Its art is assets/life/betta/00..07,
// and asking for the salmon's twelve made loadStrip open 08 and report
// "cannot open ... skipped" to stderr -- where a start-up warning among
// hundreds is exactly as good as silence. The strip length is per species
// because the ART is per species; there is no house number for it.
inline constexpr int kBettaFrames = 8;
inline constexpr int kDflyFrames = 6;
inline constexpr int kLilyModels = 3;

// -- HOW MANY OF EACH ARE ALIVE AT ONCE -------------------------------------
//
// Small, and deliberately: these are things you come across on one lake, not a
// population. The flyer band is a fixed reservation (see kFlyerInstances), so
// every slot here costs a TLAS entry whether or not anything is in it.
inline constexpr int kSalmonCount = 10;
// -- AND THE BASS, WHICH IS A SALMON THAT DOES NOT SCHOOL -------------------
//
// Same twelve-frame strip, same steering, same leap, same everything -- "make
// sure the bass follows the same mechanics as the salmon. the bass does not
// have a school of fish however." So it is not a second kind of animal in the
// code: it is a Fish with species 1, and the ONE place that asks is
// schoolsWith(), which decides whether it may found or join.
//
// ONE ARRAY, TWO RUNS. fish_ holds the salmon first and the bass after them,
// because the flyer band's slot layout has to be fixed per population -- a
// bass taking a salmon's slot mid-swim is a motion vector between two
// unrelated objects, which is the one thing that band exists to get right.
inline constexpr int kBassCount = 6;
// ---------------------------------------------------------------------------
// ONE FRAME OF ONE STRIP: the model, and HALF ITS OWN BOX.
//
// The half-box is per FRAME and that is the whole point of this type existing.
// setFlyerInstance puts a model's own ORIGIN at the translation, so a caller
// that wants the body centred has to take the half-box off -- and a swim frame's
// box CHANGES WIDTH as the tail sweeps, by up to two voxels. Centring every
// frame on one stored half-box therefore slides the fish sideways by half that
// difference, in step with the tail beat.
//
// REPORTED AS "the animations of the fish look off centered... the koi looks
// fine though" (user 2026-09-13), and the koi is the clue that proves it: its
// box is 5 wide in ten of its twelve frames, so its error is 0.08 of a voxel
// and invisible, while the bass runs 5 to 7 wide and sits two thirds of a voxel
// off its own position. Measured across all six, the fixed half-box leaves a
// mean offset of -0.17, +0.67, +0.50, +0.50, +0.17 and -0.08 voxels; the
// frame's own box leaves 0.00 for every one of them, which is what a body
// centred on its own position means.
//
// AND THE BOX CENTRE IS THE AUTHORED ALIGNMENT, not a guess. MagicaVoxel
// centres every frame of a keyframed shape on the SAME node translation -- see
// the note in render/bow.h, which corrects for exactly this on the bow -- so
// asking each frame to stand on its own centre reproduces what the art was
// drawn against.
//
// NAMED FishFrame BECAUSE v2 ALREADY HAS A `Frame` at namespace scope. A
// member declared `std::vector<Frame>` binds to that one and the errors name
// the uses rather than the declaration; render/bunnies.h has the same note over
// the same trap.
// ---------------------------------------------------------------------------
struct FishFrame {
    int model = -1;
    float hx = 0.0f, hy = 0.0f, hz = 0.0f;   // half the frame's own box, in metres
};

// -- THE KOI, WHICH IS A BASS THAT LOOKS LIKE A KOI -------------------------
//
// "Same fish mechanics as the bass" -- so it is species 2 and the only thing it
// does differently is wear a different strip.
//
// ITS TWELVE FRAMES LIVE IN ONE FILE, which is the only way it differs from
// every other fish here: assets/life/koi.vox is keyframed onto a single shape
// node rather than exported as twelve numbered files. This note used to say the
// koi had no swim cycle at all -- it has one, and what it lacked was a reader.
// See loadFrames, and the note at the call.
inline constexpr int kKoiCount = 6;
// -- ...AND THE MINNOWS, WHICH SCHOOL LIKE THE SALMON -----------------------
//
// Species 3, and the one thing that makes it a schooling fish is that fill
// gives it a `want` -- see shortSchool, where `want > 0` is the whole test.
// There are more of them than of anything else because a minnow is 30 cm and a
// dozen of them is one modest shoal.
inline constexpr int kMinnowCount = 12;
// -- THE CATFISH, WHICH IS ANOTHER BASS ------------------------------------
//
// Species 4, solitary, twelve frames. v1's world-fish list is salmon, minnow,
// betta, bass, blue_gill, catfish and koi -- and the BETTA is excluded from
// ordinary water there by construction (its own note: tick-creatures picks with
// `wk % (FISHES.length - 1)`, "an exclusion that names the betta only by it
// being LAST"). So against v1 the water here is still short one fish after this
// one: the BLUE_GILL. Its art is in assets/life/blue_gill and adding it is a
// count, a cell size, a salt and one line in fill -- see fillFish.
inline constexpr int kCatfishCount = 6;
// -- THE BLUE GILL, WHICH COMPLETES v1'S WATER ------------------------------
//
// Species 5 and solitary. With this one v2 holds every fish v1 puts in ordinary
// water: salmon, minnow, bass, blue_gill, catfish and koi. The BETTA is the only
// name left on v1's list and it is not a world fish THERE either -- its picker
// is `wk % (FISHES.length - 1)`, which that engine's own note calls "an
// exclusion that names the betta only by it being LAST".
inline constexpr int kBluegillCount = 6;
// -- THE BETTA, AND IT IS THE CHERRY WOOD'S OWN FISH ------------------------
//
// (user 2026-09-21: "import the pink betta fish into the cherry forest
//  water/lakes. make them swim in schools like the salmon".)
//
// THE TWO NOTES ABOVE BOTH SAY THIS FISH IS DELIBERATELY ABSENT, and they are
// right about v1: its picker is `wk % (FISHES.length - 1)`, which that engine
// calls "an exclusion that names the betta only by it being LAST". So the
// betta was never in ordinary water THERE either, and nothing here is being
// reversed -- it is being given the one water v1 had no equivalent of.
//
// SPECIES 6, AND IT SCHOOLS. `want > 0` is the only thing in this file that
// separates a schooling fish from a solitary one, so "like the salmon" is one
// argument to fillFish and the salmon's own cell size.
//
// TEN, WHICH IS THE SALMON'S COUNT. A school wants enough fish to read as one
// (see kSchoolMin/kSchoolMax); six would be two short schools rather than one.
inline constexpr int kBettaCount = 10;

// ---------------------------------------------------------------------------
// THE DUCKS, PORTED FROM v1: FOUR MOTHERS AND THREE DUCKLINGS EACH.
//
// Its numbers exactly -- DUCK_N = 4, BABY_N = 12, and its own comment says
// "every mother leads exactly 3 ducklings (user)". They live here rather than in
// a header of their own because a duck is a LAKE creature: it needs the water
// field, the site lattice, the fade and the recycling that everything else on
// this water already has, and giving it a second 44,100-column field of its own
// to answer the same question would be the expensive way to write the same
// behaviour.
//
// -- THE MOTHER: EDGE AVOIDANCE, THEN A WANDER --------------------------------
//
// v1's block, and its note names the request: "keep the ducks further away from
// the terrain". Eight rays at 45 degrees, each walked from 4 to 15 voxels; the
// first dry sample on a ray pushes back with weight (17 - d), so a near bank
// shoves harder than a far one. The resultant is a heading. With nothing in
// range it wanders gently instead, re-rolling every two to five seconds.
//
// THE WIND-UP GUARD IS NOT DECORATION. v1 carries a signed turn accumulator and,
// past four radians in one direction, unwinds the LONG way round rather than
// completing the loop -- without it a duck in a narrow inlet circles for ever,
// because every ray on one side is blocked and the resultant never changes sign.
//
// -- THE DUCKLINGS: A LINE, NOT A FLOCK ---------------------------------------
//
// Each steers at a spot 0.45 m directly behind its LEADER, and the leader is the
// mother for the first and the sibling ahead for the rest. That one rule is what
// makes a line rather than three ducklings converging on one point. Speed comes
// from the distance to that spot -- v1's three-step table, which is what lets a
// straggler scramble and a duckling in place idle.
// ---------------------------------------------------------------------------
inline constexpr int kDuckCount = 4;          // v1's DUCK_N
inline constexpr int kBabyPerDuck = 3;        // "exactly 3 ducklings"
inline constexpr int kBabyCount = kDuckCount * kBabyPerDuck;   // v1's BABY_N = 12

inline constexpr float kDuckCellM = 38.0f;    // one family per cell of the lattice
inline constexpr uint32_t kDuckSalt = 0xD0CCu;
inline constexpr float kDuckSpeed = 0.70f;    // v1's 7 vox/s
inline constexpr float kDuckYaw = 2.8f;       // its turn cap, rad/s
inline constexpr float kDuckAvoidGain = 2.8f, kDuckFollowGain = 2.2f;
inline constexpr float kDuckSeeMin = 0.4f, kDuckSeeMax = 1.5f, kDuckSeeStep = 0.3f;
inline constexpr float kDuckWindUp = 4.0f;    // radians of turn before it unwinds
inline constexpr float kDuckWanderMin = 2.0f, kDuckWanderMax = 5.0f;
inline constexpr float kDuckHeelM = 0.45f;    // how far behind its leader a duckling swims
// v1's speed table for a duckling, by distance to its heel spot: scrambling,
// keeping up, or idling.
inline constexpr float kBabyFarM = 0.9f, kBabyNearM = 0.35f;
inline constexpr float kBabyFast = 1.0f, kBabyKeep = 0.7f, kBabyIdle = 0.15f;
// HOW MUCH OF THE SWELL A DUCK RIDES -- v1's DUCK_SWAY, halved on request there.
// A duck sitting rigidly at the still line while the water moves under it is the
// one thing that makes it read as a decal.
inline constexpr float kDuckSway = 0.5f;
// A duckling this far from its mother has lost the line and is put back behind
// her -- v1 recycles at 40 voxels for the same reason.
inline constexpr float kBabyLostM = 4.0f;

// -- AN ORPHANED DUCKLING WEEPS -- v1's CRY_WAIT / CRY_MS / CRY_GAP ---------
//
// (user 2026-09-15: "the babies should cry".)
//
// v1's numbers exactly, in seconds rather than ms. The WAIT is the one that
// looks arbitrary and is not: v1's note says the tears "start after the
// mother's death poof has cleared", and the poof is sixteen smoke voxels living
// 1.0-1.5 s. 900 ms is the moment the column has thinned enough for a tear to
// be the thing you notice.
//
// ONE TEAR EVERY 260 ms, which v1 records as the user's own words -- "one after
// the other" -- rather than a stream. Three ducklings at that rate never need
// more than the four tear slots the band gives them.
inline constexpr float kCryWaitSec = 0.90f;
inline constexpr float kCrySec = 3.00f;
inline constexpr float kCryGapSec = 0.26f;
// How far the surface itself moves under it, before kDuckSway takes its share.
inline constexpr float kDuckBobM = 0.06f;
// -- AND SHE RIDES ONE VOXEL PROUDER OF IT (user 2026-09-14: "have the ducks
// one voxel higher on average in the water") --------------------------------
//
// ON TOP OF THE 0.45 HALF-BOX in putDuck, not instead of it. That constant is
// the WATERLINE -- where on her body the surface cuts, which is a fact about a
// duck -- and this is how deep she floats, which is a fact about buoyancy. One
// number each, so trimming the float does not move the waterline up her flank.
inline constexpr float kDuckRideM = VOXEL_M;
// -- A DUCK GOES ROUND A LILY PAD ------------------------------------------
//
// "Have ducks go around lillypads, not clip right through them." They are the
// one pair in this file that genuinely collides: everything else either shares
// the surface with nothing (the fish are under it, the dragonfly is over it) or
// is kept apart by the spawn rule and never moves far enough to meet again. A
// duck paddles a metre a second through water a pad is drifting across at a
// tenth of that, so they WILL meet, and nothing was watching for it.
//
// Three rules, and the shape of them is the fish's, learned the same way:
//
//   SEE IT -- the mother's ray fan stops at a leaf exactly as it stops at a
//             bank, so she turns away a metre and a half out and the whole
//             line follows her round it. This is what makes it read as going
//             AROUND rather than as bumping into.
//   SLIDE  -- a step that would put a body on a leaf is retried either side
//             before it is given up, which is separateFish's own lesson: a
//             refused step that only adds a turn becomes a spin when several
//             headings are refused at once.
//   PUSH   -- and after everything has moved, nothing is allowed to be
//             overlapping one. A steering term cannot promise that (a duck
//             turns at a finite rate and a pad drifts under it), which is
//             exactly why the salmon needed separateFish on top of its own
//             avoidance.
//
// THE PUSH IS RATE-LIMITED rather than resolved in one frame. In the steady
// state the overlap is a centimetre and the cap never binds; what it is for is
// the one case that can produce a big one -- something appearing on top of a
// duck -- where instantly resolving it is a teleport. Twice the paddle speed
// clears the worst possible overlap in about half a second.
inline constexpr float kDuckPadPushM = 1.4f;   // m/s, = 2 x kDuckSpeed
// The headings a refused step is retried at, either side of the one it wanted.
inline constexpr float kDuckSlideA = 0.6f, kDuckSlideB = 1.2f;
// How near counts as having MET a leaf, for the report. Half a metre outside
// touching, which at a duck's 0.7 m/s is most of a second of closing.
inline constexpr float kDuckNearPadM = 0.5f;
// MEASURED, both halves of it, over one 40,000-tick soak of the same lake with
// 4 duck families (16 bodies) and 24 leaves in it:
//
//                       closest approach   duck-ticks within 0.5 m   INSIDE a leaf
//     as it shipped          0.00 m              13,852               5,326 (0.833 m deep)
//     seeing / sliding / pushed
//                            0.51 m                   0                   0
//
// 0.833 m deep is a whole duck inside a large pad. The A/B ran off a temporary
// constant gating the three rules, which was then DELETED rather than left at
// true -- an always-true switch nothing will ever flip is the same dead weight
// as kLakeMinPlaceM, which sat in this file for a day meaning nothing.
//
// ...AND THE SOAK IS WHY THE FIRST RUN PROVED NOTHING. The default offline
// camera's lake reported 0 clashes with the avoidance switched OFF, because its
// ducks and its pads never came within 17.55 m of each other in eleven minutes
// of simulation. --cam-x/--cam-z put the camera in a lake big enough to hold
// both, and that is the only reason the numbers above exist. A zero from a run
// where nothing ever met is not a result, which is what `closest` is printed
// for.
// TWENTY-FOUR PADS, UP FROM TWELVE. They are the thing you see a lake BY at
// distance -- "I should be able to see lillypads far away in the water" -- and
// twelve spread over everything inside 170 m is a pad every fifty metres.
// -- ...AND SEVENTY-EIGHT, BECAUSE THE DISC GREW -------------------------
//
// (user 2026-09-21: "have the lillypads have the same render distance as the
//  terrain".)
//
// Twenty-four was twenty-four over a 170 m disc. The pads reach the chunk ring
// now (see kPadReachDefaultM), which is 3.26x the AREA, so holding the count
// would have spread the same pads over three times the water and thinned every
// lake you stand next to -- trading a pop at the edge for an empty middle.
// 24 x 3.26 = 78 keeps the density that was there.
//
// IT COSTS 54 SLOTS IN THE LAKE BAND, and kLakeSlots must be raised by exactly
// the same amount: the static_assert below is an EQUALITY, deliberately, for
// the reason its own note gives.
inline constexpr int kLilyCount = 78;
inline constexpr int kDflyCount = 4;   // halved (user 2026-09-13)

// ---------------------------------------------------------------------------
// THE FISH, CONFIG FOR CONFIG FROM THE JS ENGINE'S FISH_CFG.
//
// Its numbers are VOXELS PER SECOND and both engines are 10 cm voxels, so they
// carry over as metres by dividing by ten. Kept as its own names so the two can
// be read side by side.
// ---------------------------------------------------------------------------
inline constexpr float kFishCruise = 2.2f;    // m/s      (JS baseSpeed 22)
inline constexpr float kFishFleeMul = 2.0f;   //          (JS fleeMult)
inline constexpr float kFishAnimFps = 24.0f;  //          (JS animFps)
// -- THE SPHERE OF INFLUENCE ------------------------------------------------
//
// 5.6 m, and a SPHERE rather than a ground circle -- the JS engine's threat
// scan includes the vertical gap, with the player's own offset, so swimming
// over a fish spooks it and standing on a bank above one does too. Its note
// records this being doubled from 2.8 m on request: "a fish now breaks well
// before you are on top of it".
inline constexpr float kFishThreatM = 5.6f;   // (JS threatR 56)
// ...and the state LINGERS after the threat leaves, which is the whole reason
// the number exists: without it a fish at the rim flickers between cruise and
// flee every frame the player shifts his weight.
inline constexpr float kFishFleeHold = 1.2f;  // seconds  (JS fleeHold)
inline constexpr float kFishYawRate = 2.2f;   // rad/s    (JS yawRate)
inline constexpr float kFishFleeYaw = 6.0f;   // rad/s    (JS fleeYawRate)
inline constexpr float kFishPitchMax = 0.30f; // rad      (JS pitchMax)
inline constexpr float kFishPitchGain = 1.6f; //          (JS pitchGain)
// How often the whiskers are re-sampled. The JS engine's note is worth keeping:
// "SENSE AT ~14 Hz, ACT EVERY FRAME ... at uncapped render rates the per-frame
// probing WAS the fish AI cost".
inline constexpr float kFishSenseSec = 0.07f;

// -- THE PADS ---------------------------------------------------------------
//
// "slow drift on the water + constant free rotation; movement heading (mth) is
// independent of the visual spin". 1.1 voxels a second.
inline constexpr float kLilyDrift = 0.11f;    // m/s      (JS 1.1)
inline constexpr float kLilySpinMax = 0.22f;  // rad/s, its own per pad
inline constexpr float kLilyTurnMin = 3.0f, kLilyTurnMax = 7.0f;   // s between wanders
inline constexpr float kLilyShoreTurn = 2.6f; // rad/s away from a dry lookahead

// -- THE DRAGONFLY ----------------------------------------------------------
//
// The JS engine says it outright: "Flies the butterfly's kind-0 code path
// VERBATIM; only its HOME differs (water, not meadow)". So this is the
// butterfly's wander with a water home and a tighter leash -- a dragonfly
// works one stretch of bank rather than a meadow.
// DOUBLED, 2.6 -> 5.2 (user 2026-09-13). That puts it just under the
// butterflies' 5.6, which is about right for the two insects side by side.
inline constexpr float kDflySpeed = 5.2f;     // m/s
// -- ...AND THE SAME 2x RULE EVERY OTHER FLYER OBEYS ------------------------
//
// The butterflies have had this since they were written -- kFlyThreatM,
// kFlyFleeHold and kFlyFleeMul in butterflies.h, which are the JS engine's
// FLY_THREAT_R 30, FLY_FLEE_HOLD 1.2 and FLY_FLEE_MULT 2.0. The dragonfly was
// ported from the butterfly's flight path and did not get them.
//
// THE RADIUS IS THE FLYERS' OWN AND NOT THE FISH'S, and the JS note says why:
// "a fish cruises at 22 and a butterfly at 56, so the fish's 56-voxel sphere is
// two and a half seconds of fish travel but one second of butterfly travel --
// shared, it would leave every butterfly permanently spooked".
inline constexpr float kDflyThreatM = 3.0f;   // (JS FLY_THREAT_R 30)
inline constexpr float kDflyFleeHold = 1.2f;  // (JS FLY_FLEE_HOLD)
inline constexpr float kDflyFleeMul = 2.0f;   // (JS FLY_FLEE_MULT)
inline constexpr float kDflyLeashM = 9.0f;    // how far from its home it strays
inline constexpr float kDflyLoM = 0.35f, kDflyHiM = 1.30f;   // height over the water
// 24, THE SAME RATE EVERY OTHER STRIP IN THIS ENGINE PLAYS AT -- the
// butterflies' kFlyFps, the songbirds' kBirdFrameMs, the salmon's animFps and
// the JS engine's BIRD_FLAP. It was 30 for no reason anyone wrote down.
inline constexpr float kDflyFps = 24.0f;

// -- HOW LONG A DRAGONFLY MAY BE BLOCKED BEFORE IT GIVES UP ON ITS HEADING ---
//
// It got STUCK, and the cause was that a blocked step did not move it: the
// recovery set an intent toward home and returned, so a dragonfly whose home
// direction ALSO left the water simply stopped, in the air, for ever. And if it
// happened to be sitting exactly on its home the direction was atan2(0, 0),
// which is a constant -- so it could not even turn.
inline constexpr float kDflyStuckSec = 0.6f;

// ---------------------------------------------------------------------------
// THE LEAP, FROM THE JS ENGINE'S FISH_CFG.jump.
//
// Its units are voxels; both engines are 10 cm voxels, so they divide by ten.
//
//     cooldownMin/Max  9 / 31 s between attempts, over the species multiplier
//                      (salmon is 1.0 -- it is the one that leaps most)
//     vMin/vMax        65 / 88 vox/s up. The peak is v^2/2g, so 1.3 to 2.4 m
//                      clear of the surface. Its note records this being
//                      DOUBLED on request, and that the speed went up by
//                      sqrt(2) because height scales with the square.
//     gravity          165 vox/s^2 -- the arc's own fall, not the world's
//     forward          26 vox/s carried through the arc -> 2.0 to 2.8 m across
//     minDepth         5 vox. "only water this deep can launch a leap (never
//                      from a shelf it could land back onto)"
//
// THE ARC IS BALLISTIC AND UNSTEERED. v1 validates the splash-down at launch
// and then holds the line: "airborne is exempt ... a salmon's leap was
// validated at launch and must fly its arc at full speed", and bending the
// heading in the air "would curve the arc off its validated splash-down".
// ---------------------------------------------------------------------------
inline constexpr float kJumpCoolMin = 9.0f, kJumpCoolMax = 31.0f;
inline constexpr float kJumpVMin = 6.5f, kJumpVMax = 8.8f;
inline constexpr float kJumpGrav = 16.5f;
inline constexpr float kJumpFwd = 2.6f;
inline constexpr float kJumpMinDepthM = 0.5f;
// -- HOW SHALLOW IS TOO SHALLOW TO SWIM IN --------------------------------
//
// Reported as "a fish was caught swimming in the sand". Two things put it
// there and this is the first: the step test only asked whether the
// destination cell was WET, not whether it was deep enough to hold a fish, so
// a salmon could swim onto a one-voxel shelf and then hold a depth inside it.
//
// The second is the field's own resolution. Cells are two metres (see
// WaterField::kCellM) and the depth is one sample per cell, so the bed under
// the fish can be up to a metre and a half from the bed that was measured. The
// clearance below is what absorbs that: swimming a quarter of a metre off the
// bed instead of an eighth means a sampling error has to be twice as large
// before any of it shows.
inline constexpr float kFishShallowM = 0.45f;   // a cell under this is not swimmable
// -- AND WHERE A FISH MAY BE BORN -------------------------------------------
//
// Twelve metres, against the engine-wide thirty, for the same reason as
// kPadBirthMinM and with more room: a fish is 0.6 m, it is UNDER a rippling
// surface, and from a bank you cannot see one arrive at twelve metres the way
// you can see a leaf unfold. What the thirty was costing is the same thing it
// cost the pads -- a pond smaller than sixty metres across has no site outside
// the floor at all, so it could not be stocked while you stood on it.
//
// THE CONE IS LEFT AT kBirthFarM for fish, unlike the pads. A fish that is
// born swims, so where it appears is not where it is seen a second later, and
// the case for pulling the cone in was the pads' stillness.
inline constexpr float kFishBirthMinM = 12.0f;
inline constexpr float kFishBedClearM = 0.25f;  // ...and it keeps this far off the bed
// -- HOW THE LEAP STARTS, WHICH IS NOT WITH A TELEPORT ---------------------
//
// The first version put the fish AT the surface on the frame it decided to
// jump -- "f->y = topM - 0.14f" -- and set its pitch straight to the arc's,
// which from a metre down is a salmon appearing at the surface already nose-up
// and already at full speed. Reported as wanting "a clean smooth transition".
//
// There is nothing to fix in the arc: the arc was right. What was missing is
// the RUN-UP, which is the half of a leap that happens under water. So the
// fish now leaves from wherever it is, and the vertical speed it leaves with
// is EASED to the launch speed instead of being assigned -- over about a fifth
// of a second, which at 6.5 to 8.8 m/s is half a metre of water. The pitch
// follows from that speed rather than being set, so it rises with it and the
// nose comes up as the body does.
//
// GRAVITY ONLY ABOVE THE SURFACE. Under it the fish is swimming, and a swimming
// fish does not decelerate at 16.5 m/s^2 -- that is what makes the break
// through the surface the fastest moment of the leap rather than a corner in
// the curve.
// ---------------------------------------------------------------------------
// SALMON SWIM IN SCHOOLS OF THREE TO SIX.
//
// A SCHOOL IS A LEADER AND ITS FOLLOWERS, not a flocking simulation. Boids --
// separation, alignment, cohesion, three radii and six weights each -- is the
// general answer to "make them move as a group", and it is the wrong one here:
// what it buys is emergent structure nobody asked for, and what it costs is
// that the shape is never quite stable and never quite yours. A station keeping
// formation is one subtraction per fish and it holds its shape through a turn.
//
// THE STATION IS IN THE LEADER'S FRAME, which is the whole trick: the offsets
// are fixed, the frame rotates, so the V banks as a unit instead of unfolding
// every time the front fish turns.
//
// THE LEADER IS AN ORDINARY FISH. It whiskers, it holds a depth, it flees, it
// leaps. Nothing in stepFish knows it is being followed -- which is what keeps
// a school from being a second kind of animal with its own bugs.
inline constexpr int kSchoolMin = 3, kSchoolMax = 6;
// -- A SHOAL, NOT A FORMATION ----------------------------------------------
//
// The first version put every follower on a rank of a V, and that is exactly
// what was wrong with it: "dont have the school of salmon form a v formation.
// make it a bit more random ... make sure they are moving around within the
// group, not staying stationary."
//
// A V is what geese do because it buys them something. Fish do not hold a
// lattice -- a shoal is a CLOUD that travels, and every animal in it is
// constantly changing places inside a shape that stays roughly the same. So
// the station is now three things summed, and the second and third are what
// make it alive:
//
//   1. A BASE OFFSET drawn from a hash -- somewhere in a flattened disc behind
//      the leader, which is the shape of the cloud.
//   2. A SLOW WANDER on top of it, two sinusoids at rates that do not divide
//      into each other, so the path never repeats and no two fish drift in
//      step. Same trick as the butterflies' two-swell bob, for the same reason.
//   3. A RESHUFFLE every twenty to fifty seconds, which re-rolls the base. This
//      is what makes a fish at the back end up at the front -- a wander alone
//      keeps everybody in their own neighbourhood for ever.
//
// ...and then SEPARATION on top of all three, which is what stops the random
// offsets from ever putting two fish in the same place and is most of what
// reads as jostling.
inline constexpr float kSchoolSpreadM = 1.5f;    // the cloud's radius, across
inline constexpr float kSchoolDeepM = 1.9f;      // ...and along the swim, behind
inline constexpr float kSchoolBackM = 0.8f;      // how far behind its centre sits
inline constexpr float kSchoolWanderM = 0.45f;   // how far a fish drifts inside it
inline constexpr float kSchoolWanLo = 0.13f, kSchoolWanHi = 0.37f;   // rad/s
inline constexpr float kSchoolShuffleMin = 20.0f, kSchoolShuffleMax = 50.0f;
// PERSONAL SPACE, DEFENDED TWICE. The steering term below is what makes a fish
// LEAN AWAY from a neighbour, and it is most of what reads as jostling -- but it
// cannot be relied on to actually keep them apart, because a fish can only turn
// at kFishYawRate and a leaping one is not steering at all. Measured with only
// the steering term: the closest two fish in a twelve-thousand-tick run came
// within 0.01 m of each other, which is one salmon inside another.
//
// So there is a second, positional pass after everything has moved (see
// separateFish). Between them: the steering makes it look right, and the push
// makes it TRUE.
inline constexpr float kSchoolSepM = 0.85f;      // personal space, for steering
inline constexpr float kSchoolSepRate = 2.6f;    // ...and how hard it is leaned
inline constexpr float kFishBodyM = 0.62f;       // ...and how close two may ever be
inline constexpr float kSchoolApartM = 0.9f;   // the only spacing rule inside a school
// -- THE FOLLOWER MATCHES A VELOCITY, IT DOES NOT CHASE A POINT -------------
//
// THIS WAS MEASURED TWICE AND THE FIRST ANSWER WAS BACKWARDS. Steering at the
// station and adding speed proportional to the distance from it -- pure pursuit
// -- settled the average follower 2.14 m off station. Raising the gain to close
// harder made it 3.86 m. That is the signature of a pursuit loop against a turn
// rate limit: the fish arrives carrying the closing speed, cannot turn out of
// it fast enough, sails past, and comes back round.
//
// So the controller asks for a VELOCITY instead: the leader's own, plus a
// correction toward the station. On station the correction is zero and the fish
// is already going exactly where the leader is going, so there is nothing to
// overshoot -- the error decays instead of ringing. The gain is a rate, 1/s,
// and the cap is now only there for a fish that has been left a long way
// behind.
// 1.3, and it can be raised safely BECAUSE the controller is a velocity match
// rather than a pursuit -- there is no overshoot for a higher gain to amplify.
// Under the old pursuit loop, raising the gain made the error worse; here it
// simply shortens the time constant.
inline constexpr float kSchoolCloseRate = 1.3f;   // 1/s -- how fast the offset decays
inline constexpr float kSchoolCloseMax = 2.2f;    // m/s over the leader, for a straggler
inline constexpr float kSchoolDepthRate = 0.9f;   // 1/s -- the depth the school shares

inline constexpr float kJumpRiseRate = 14.0f;   // 1/s -- the ease onto launch speed
inline constexpr float kJumpPitchRate = 9.0f;   // 1/s -- and the nose onto the arc
inline constexpr float kJumpBreakM = 0.14f;     // half a body: where "out" begins

// How far any of this lives from the player before its slot is recycled, and
// how far out water is looked for. The BIRDS' own lesson applies (see
// v2-birds-popped-and-flickered): the gather has to be wider than the drop or
// a thing is recycled for want of data rather than for want of water.
// WIDER THAN IT WAS. Thirty metres of a lake is not much water to put thirty
// creatures in, and the result was exactly what it sounds like -- see the
// separation rule in fill().
// -- ...AND IT IS NOW THE WHOLE OF WHAT THE FIELD REACHES -----------------
//
// 45 m WAS THE CLUSTERING. Reported as "the dragonflies and fish spawn in
// clusters nearest the player which is wrong -- they need to spawn evened out
// through the lake at all times", and 45 against a 30 m floor is exactly that:
// a fifteen-metre-thick ANNULUS around the player, which every creature was
// born into and which walks with you. Nothing was spread over a lake because
// nothing was ever allowed more than forty-five metres from your feet.
//
// 170 m, just inside the drop radius, so the eligible water is the whole lake
// rather than a band of it -- and WaterField::pick is uniform over wet cells,
// so uniform over the eligible water is what "evened out through the lake"
// means arithmetically.
inline constexpr float kLakePlaceM = 170.0f;
// -- ...AND NOTHING IS BORN NEARER THAN THIS -------------------------------
//
// Reported as "the lillypads seem to just appear in front of me randomly", and
// that is exactly what was happening: fill() accepted any wet spot inside the
// place radius, INCLUDING one five metres away, and it runs every frame. Walk
// along a shore and pads behind you pass the drop radius, free their slots, and
// the next frame puts them back wherever there happens to be water -- which is
// as likely to be at your feet as at the far side.
//
// A pad drifts at 0.11 m/s, so it is scenery: once placed it stays where it
// was put, and the only thing that ever moves it is the player walking. That
// makes the spawn distance the whole of the problem.
//
// Thirty metres. Far enough that an arrival is a speck at the edge of
// attention rather than an event in front of you, and still inside the place
// radius so there is room to find a spot.
// TWELVE, down from thirty. The floor mattered when the ceiling was 45 -- it
// was half the band, and everything was being respawned constantly because the
// drop radius was 60 m. With the lake itself as the spawn area and a 180 m
// drop, a creature is recycled rarely enough that "it appeared in front of me"
// is a rare event rather than the steady state -- and a floor of thirty metres
// would empty any lake smaller than that, which is most of them.
//
// -- AND THEN IT WAS NEITHER TWELVE NOR ENFORCED ---------------------------
//
// Reported again on 2026-09-14: "I saw lillypads just appear in front of me.
// they seemed to just grow out of nowhere ... every biome should share similar
// mechanics. make sure there are no entities that do this, they all share the
// same spawning mechanics."
//
// The constant was still here and NOTHING READ IT. The rewrite that moved the
// spawn onto the site lattice replaced the whole place/min band with one
// distance test in gatherSites, and only the ceiling survived the move -- so
// for a day the floor was twelve metres in a comment and zero in the code,
// which is the worst of the three possible values. `grep kLakeMinPlaceM`
// returned exactly one line: this one.
//
// The rest of the reasoning above was wrong as well, and worth keeping so it is
// not rediscovered: a floor does NOT empty a small lake. It is a distance from
// the PLAYER, not a size of water -- you walk up to a pond and its sites pass
// through the whole band on the way in, so it is populated long before you
// reach it. What empties a lake is having no slots left to give it, which is a
// different fault with a different fix (see yieldSite).
//
// The number is kBirthMinM in core/noise.h now, shared with the butterflies,
// the rabbits and the perched songbirds, because "they all share the same
// spawning mechanics" is the request and one constant is the only way to be
// sure of it.
// -- NOTHING SPAWNS ON TOP OF ANYTHING ELSE --------------------------------
//
// v1 does this and its note says why it had to: "sometimes the fish cluster up
// in one area. fix this", answered with a spawn rule that places them a fixed
// distance apart. Its measurement is worth keeping -- nearest-neighbour
// distance at spawn was "min 33 / median 72" voxels, and it had been much
// worse.
//
// TWO NUMBERS, because "two fish in the same place" and "a fish under a lily
// pad" are different degrees of wrong. Same-kind spacing is generous enough
// that a population reads as spread; cross-kind only has to stop them
// intersecting, and a dragonfly over a pad is a picture rather than a fault.
inline constexpr float kLakeApartM = 5.0f;      // between two of a kind
inline constexpr float kLakeApartAnyM = 1.8f;   // between any two things
// -- HOW FAR IT MAY GET BEFORE ITS SLOT IS TAKEN BACK -----------------------
//
// 60 m was visible, and it was reported: "the salmon, lillypads, and
// dragonflies disappear when flown out of distance. dont let them disappear."
// 100 m is the same number the perched songbirds settled on for the same
// reason -- see v2-birds-popped-and-flickered, where one radius doing three
// jobs produced exactly this.
// 180 m NOW, and with a FADE under it. Reported again on 2026-09-13: "the
// dragonflies and fish are still disappearing ... it happens when the view
// distance is too great. give them the same treatment as the butterflies."
//
// The butterflies' treatment is two things and only one of them is a number:
// a keep radius past the range the animal can be made out at, AND a fade so
// that whatever happens at the edge of it happens over most of a second rather
// than between two frames. Both are here now -- see kLakeFadeSec.
//
// 180 m is where a 60 cm salmon is a fifth of a pixel at 1080p. There is no
// radius at which a fixed number of instances covers an unbounded view, so this
// is the honest version of "infinite": far enough that the recycling happens
// below the resolution of the screen.
inline constexpr float kLakeDropM = 180.0f;
// -- ...AND THE LILY PADS GO AS FAR AS THE GROUND DOES ----------------------
//
// (user 2026-09-21: "have the lillypads have the same render distance as the
//  terrain".)
//
// A pad is the only lake creature that sits ON the surface in plain sight, so
// it is the one whose 180 m edge you can actually watch things vanish at while
// the hillside behind it carries on to the chunk ring. This is that ring --
// viewChunks x CHUNK_VOX x VOXEL_M, 307 m at the default 12 -- and it is a
// RUNTIME value rather than a constant because --view moves the ring and a
// constant would silently stop matching it.
//
// BOTH RADII MOVE, and that is the half that is easy to miss: a drop radius on
// its own only keeps pads you were once close to. Sites are gathered to the
// same distance, so the far side of a lake has pads before you walk to it.
//
// AND THE COUNT MOVES WITH THE AREA, or the same 24 pads spread over 3.3x the
// disc and every lake gets emptier near you -- which is a worse bug than the
// one being fixed. See kLilyCount.
inline constexpr float kPadReachDefaultM = 307.2f;   // 12 x 256 x 0.1
// ...and the hysteresis under it, so a creature sitting exactly on the line is
// not claimed and dropped on alternate frames.
inline constexpr float kLakeFadeSec = 0.8f;
inline constexpr float kLakeFadeMin = 0.08f;
// -- ...AND THE FIELD HAS TO BE WIDER THAN THAT -----------------------------
//
// THIS IS THE HALF THAT IS EASY TO MISS. A fish steers by WaterField::at, and
// outside the field that answers "no water" -- so a fish beyond the sampled
// square does not merely lose its bearings, it turns hard every frame and
// stops. Raising the drop radius without raising this would trade a
// disappearing fish for a frozen one.
//
// 220 m against a 100 m drop, so a live creature is always well inside the
// data. It is 48,400 columns a rebuild against 6,400 -- and the rebuild only
// happens when the PLAYER leaves the trustworthy middle, which at this size is
// every sixty metres of walking.
// 420 m, AND THE CELL WENT UP WITH IT -- see WaterField::kCellM. A field has to
// cover the drop radius from both sides, so 180 m of drop needs 360 m of field
// plus a margin to rebuild in. At the old one-metre cell that would have been
// 176,400 columns a rebuild against 48,400; at two metres it is 44,100, which
// is FEWER than the 220 m field cost, over nearly twice the reach.
inline constexpr float kLakeFieldM = 420.0f;  // the sampled square, per side

// The lattice every population in this engine is born on now lives in
// core/noise.h beside the hash it is built from -- see siteOf there. It moved
// when the bunnies became the third caller.
//
// HOW BIG A CELL IS PER POPULATION, which is the only dial density has. A pad
// every nine metres of water is a lake with pads on it; a school of salmon
// every thirty is a lake with two or three schools in it.
inline constexpr float kPadCellM = 9.0f;
inline constexpr float kDflyCellM = 26.0f;
inline constexpr float kSchoolCellM = 34.0f;
inline constexpr float kBassCellM = 21.0f;
inline constexpr float kKoiCellM = 27.0f;
inline constexpr float kMinnowCellM = 30.0f;
inline constexpr float kCatfishCellM = 24.0f;
inline constexpr float kBluegillCellM = 22.0f;
// The salts keep the four lattices independent -- without them a pad, a bass
// and a dragonfly would all want the exact same point in every cell.
inline constexpr uint32_t kPadSalt = 0x11A9u, kDflySalt = 0xD91Fu;
inline constexpr uint32_t kSchoolSalt = 0xF15Bu, kBassSalt = 0xBA55u;
inline constexpr uint32_t kKoiSalt = 0x6001u, kMinnowSalt = 0x111Du;
inline constexpr uint32_t kCatfishSalt = 0xCA75u;
inline constexpr uint32_t kBluegillSalt = 0x81C6u;
// The betta shoals on the salmon's own spacing -- see kBettaCount.
inline constexpr float kBettaCellM = kSchoolCellM;
inline constexpr uint32_t kBettaSalt = 0xB37Au;
// A lily pad is leashed to its site for the reason a butterfly is leashed to
// its home: the slot is given up when the SITE leaves range, so a pad that had
// drifted a hundred metres from it would be recycled somewhere it is plainly
// visible. Five metres is a lake-sized wander.
inline constexpr float kLilyLeashM = 5.0f;
// -- WHERE A PAD MAY BE BORN, WHICH IS NOT WHERE A RABBIT MAY ---------------
//
// Six metres, against the engine-wide thirty. See BirthGate::mayAt for the
// argument in full: the floor is a proxy for `could this be watched arriving`
// and for something that never moves the view cone answers that question
// exactly, so the floor is only there to keep a leaf from unfolding at arm's
// reach. At thirty it was refusing every site on any pond smaller than sixty
// metres across -- so a small lake could not be populated at all while you
// stood on it, and only filled once you walked away and the yield rule or the
// drop radius moved the slots for you. That is the delay that has been
// reported three times.
//
// THE CONE IS UNTOUCHED AND IS STILL DOING THE REAL WORK: nothing is born in
// the hundred and forty degrees you are facing, at any distance under
// kBirthFarM. A pad born six metres BEHIND you is simply there when you turn
// round, which is what scenery is supposed to be.
inline constexpr float kPadBirthMinM = 6.0f;
// -- HOW FAST A SLOT MAY MOVE TO BETTER WATER -- see yieldSite --------------
//
// ONE EVERY TWO SECONDS WAS THE OTHER HALF OF "THE LIFE IN THE WATER HAS A VERY
// DELAYED SPAWN" (user 2026-09-14). Walk from one pond to the next while the
// first is still inside the 180 m drop radius and it holds every slot, so the
// pond at your feet can only fill as fast as this rule releases them: at one
// per two seconds, twenty-four lily pads is FORTY-EIGHT SECONDS of standing at
// an empty lake.
//
// FOUR AT A TIME, EVERY SECOND. That is a lake populated in six seconds rather
// than a minute, and it is safe for the reason the rule is safe at all: a
// retiring creature is at least kYieldMarginM further away than the site that
// wants its slot, and the site it is being retired FOR has nothing within
// kYieldLonelyM of it. Nothing visible moves; what changes is where the
// population is, and it changes at walking pace instead of at a crawl.
//
// STILL NOT UNBOUNDED. A cap is what keeps this from swapping a whole
// population between two frames the moment a better lake comes into range --
// which would be a different complaint with the same cause.
inline constexpr float kYieldEverySec = 1.0f;
inline constexpr int kYieldPerPass = 4;

// -- A LILY PAD IS A DISC, AND THE SHORE IS SOLID ---------------------------
//
// "Have the lillypads bounce off the sandy shore, instead of clipping through
// it." They clipped for two reasons that compound, and only one of them is the
// missing bounce:
//
//   * A PAD WAS A POINT. The lookahead asked whether the water was wet three
//     metres ahead of its CENTRE, so half a leaf could be over sand and every
//     test in the file still said yes. The leaves are 0.5 to 0.9 m across.
//   * THE FIELD CANNOT ANSWER THIS. WaterField is sampled on a two-metre
//     lattice and each cell carries the wetness of its own CORNER column, so
//     "is it wet here" is only right to within a cell -- a pad can be most of
//     two metres inland with its cell still reading wet. That is the clipping,
//     and no amount of lookahead in the field fixes it, because the field does
//     not know where the shore is to better than 2 m.
//
// So the field stays the BROAD phase (is there a bank anywhere near this pad)
// and the terrain itself is the NARROW one (exactly where does the water end).
// The terrain is asked per column, which is what the shore actually is, and it
// is only asked for pads the field says are near a bank -- typically two or
// three of the twenty-four.
// The rim points themselves are kShoreRimSamples in scene/shore.h, which is
// where the disc test lives.
inline constexpr float kLilyBankM = 0.06f;        // a little clear of the bank
// How far from a pad a dry cell has to be before the exact test is worth
// running. A cell is 2 m and a pad is under half a metre, so anything inside
// three metres could be a bank the leaf can reach this second.
inline constexpr float kLilyNearBankM = 3.0f;
// A pad that is somehow ALREADY over land walks out at this rate. It is a
// recovery, not a behaviour: the same shape as recoverFish, and it exists
// because a lake can be re-carved under a leaf (an axe, a chunk streaming in,
// the field rebuilding somewhere new) and a pad with every heading refused
// would otherwise sit in the sand for ever.
inline constexpr float kLilyPushM = 0.6f;         // m/s out of the bank
// ...AND IF THAT DOES NOT WORK, IT IS NOT A PAD'S PLACE. Measured: a leaf can
// end up in a finger of water narrower than itself, where the middle is wet,
// the push has somewhere to go, and the disc still fits nowhere -- 1 of 24 in
// tests/lily_shore_test.cpp. Pushing for ever there is a lily pad lying in the
// sand for the rest of the session. Six seconds of getting nowhere and it
// retires instead: the slot comes back and fill() puts a pad somewhere a pad
// fits. The fish have the same rule and the same reasoning -- recoverFish
// recycles one it cannot get back to water.
inline constexpr float kLilyStuckSec = 6.0f;
// A bounce puts a little turn into the leaf, because a leaf that glances off a
// bank and carries on spinning at exactly its old rate reads as a sprite. It is
// CLAMPED to the spin a pad is allowed to have anyway -- this may add character
// to the drift, never a new kind of motion.
inline constexpr float kLilyBounceSpin = 0.35f;

// ---------------------------------------------------------------------------
// A DRAGONFLY SETTLES ON A LILY PAD.
//
// PORTED FROM THE JS ENGINE'S LADYBUG, which is the creature that already
// solved this: "it cruises 6-16 s, settles, sits 3-7 s holding frame 00, then
// climbs back." Four phases and the timings are its, because they are what make
// a landing read as a landing rather than as a hover.
//
// THREE OF ITS HARD-WON RULES CARRY OVER EXACTLY:
//
//   THE DESCENT SUSPENDS THE FLIGHT. Its note is blunt about why -- the
//   altitude servo and the wander are "what makes it a flyer; there is no way
//   to reach the ground with them running". So phases 1-3 below return before
//   any of the ordinary steering is reached.
//
//   THE TARGET IS PINNED AT COMMIT. v1 pins the COLUMN and comes straight down
//   it, because "letting it keep drifting while it descended is why it arrived
//   high: the target height was computed for the column it committed over, and
//   by touchdown it was somewhere else entirely."
//
//   A FRIGHTENED ONE ABANDONS THE LANDING and there is a DEADLINE on the
//   descent, "so a descent can never become a permanent snag".
//
// AND ONE THING IS DELIBERATELY NOT v1'S. Its pinned column is a fixed point on
// the ground; ours is a lily pad, and a lily pad DRIFTS. So what is pinned here
// is not a place but a FRAME: the offset from the pad's centre, in the pad's own
// rotating basis. Riding that frame is what "the dragonfly should glide with the
// lillypad as it moves over the water" means -- it holds its spot on the leaf
// while the leaf goes where it likes, and turns with the leaf's own spin.
// ---------------------------------------------------------------------------
inline constexpr float kDflyFlyMin = 6.0f, kDflyFlyMax = 16.0f;    // between attempts
inline constexpr float kDflySitMin = 3.0f, kDflySitMax = 7.0f;     // and how long it rests
inline constexpr float kDflyRetryMin = 2.0f, kDflyRetryMax = 5.0f; // ...if none was in reach
inline constexpr float kDflyDropRate = 2.2f;    // 1/s, v1's own ease onto the surface
inline constexpr float kDflyDropMaxS = 3.5f;    // the deadline on a descent
inline constexpr float kDflyReachM = 3.0f;      // how near a pad must be to commit
inline constexpr float kDflyPerchM = 0.10f;     // how far above the leaf it sits
// HOW MUCH OF THE LEAF IT MAY SIT ON, as a fraction of the leaf's own half
// width. A constant was wrong in both directions at once: 0.45 m is outside a
// small pad (half width 0.2 m) and wastes most of a large one. 0.55 keeps the
// body inside the leaf whatever size the leaf is.
inline constexpr float kDflyOnPadFrac = 0.55f;

// -- THE BAND'S RESERVATION MUST BE EXACTLY WHAT publish() WRITES ------------
//
// kLakeSlots is a contract, not a preference: the band is one fixed layout and
// the songbirds are addressed from the end of this run. Raising the counts here
// without raising it there had the lake write 44 slots into a 26-slot
// reservation -- it overwrote nine songbirds and all three pause-room buttons,
// and the only symptom anybody saw was that the buttons had gone. Nothing threw,
// because setFlyerInstance's bounds check is against the BAND, which was still
// the right size; it was the tenancy inside it that was wrong.
static_assert(kSalmonCount + kBassCount + kKoiCount + kMinnowCount + kCatfishCount +
                      kBluegillCount + kBettaCount + kLilyCount + kDflyCount + kDuckCount +
                      kBabyCount ==
                  kLakeSlots,
              "LakeLife publishes kSalmonCount + kBassCount + kLilyCount + kDflyCount slots; "
              "kLakeSlots in gpu/world.h must reserve exactly that many");

// ---------------------------------------------------------------------------
// WHERE THE WATER IS, SAMPLED ONCE.
//
// A square of one-metre cells centred on the player. Each cell knows whether it
// is wet and, if so, where its surface and its bed are.
//
// ONE METRE, WHICH IS COARSER THAN A FISH. A salmon is about 0.6 m and its
// whiskers reach a few body lengths, so the field is not a collision surface --
// it is a map of which water exists. The fish's own clearance comes from
// testing several cells along a heading, exactly as the JS engine's fishReach
// walks its voxels; the resolution difference costs a fish the last few
// centimetres of a bank, which is under its own body width.
//
// REBUILT ON MOVEMENT, NOT ON A CLOCK. The field is only wrong when the player
// has left it, so that is what triggers it -- and it is rebuilt with a margin
// so the rebuild happens before the edge is reached rather than at it.
// ---------------------------------------------------------------------------
class WaterField {
  public:
    // -- TWO METRES, WHICH IS COARSER THAN IT WAS AND HAD TO BE -----------
    //
    // The field's cost is its CELL COUNT, and the reach that was wanted is four
    // times the area. At one metre that is 176,400 columns; at two it is 44,100,
    // which is less than the old field cost for less than half the reach.
    //
    // WHAT IT COSTS is the last metre of a bank: reach() walks in cells, so a
    // fish now keeps a two-metre berth from a shore instead of a one-metre one.
    // A salmon is 0.6 m and a lake is tens of metres, so this is a fish that
    // swims slightly further out -- not a fish that behaves differently.
    static constexpr float kCellM = 2.0f;
    static constexpr int kN = int(kLakeFieldM / kCellM);   // 210 x 210 = 44,100 columns

    // -----------------------------------------------------------------------
    // HAS THE PLAYER LEFT THE PART OF THE FIELD THAT IS TRUSTWORTHY?
    //
    // The condition that has to hold is that EVERY LIVE CREATURE is inside the
    // sampled square -- a creature may be kLakeDropM from the player, so the
    // player may be at most (half - drop) from the centre. This subtracted HALF
    // the drop, which is a bound that does not mean anything, and at a 60 m
    // drop it happened not to matter.
    //
    // It costs one rebuild every (half - drop) metres of walking: at 220 m and
    // a 100 m drop that is every ten metres, and a rebuild measures 5.97 ms on
    // this terrain. That is the price of nothing ever popping, and it is paid
    // on the frame thread -- if it ever shows, the fix is to build it on a
    // worker rather than to shrink it.
    // -----------------------------------------------------------------------
    static constexpr float kSafeM = kLakeFieldM * 0.5f - kLakeDropM;   // 210 - 180 = 30 m
    static_assert(kSafeM > 8.0f, "the field must be wider than the drop radius, with room to "
                                 "rebuild in -- otherwise stale() is true every frame");

    bool stale(const Vec3 &p) const {
        if (!built_) return true;
        return fabsf(p.x - cx_) > kSafeM || fabsf(p.z - cz_) > kSafeM;
    }

    void rebuild(const VoxelTerrain &t, const Vec3 &p) {
        cx_ = p.x;
        cz_ = p.z;
        wet_.assign(size_t(kN) * kN, 0);
        top_.assign(size_t(kN) * kN, 0.0f);
        bed_.assign(size_t(kN) * kN, 0.0f);
        TerrainMemo memo;
        any_ = false;
        // i ON THE INSIDE, which is what keeps the generator's lattice cache
        // warm -- the same reason the chunk mesher walks its columns this way.
        for (int j = 0; j < kN; ++j) {
            const float z = cz_ + (float(j) - kN * 0.5f) * kCellM;
            for (int i = 0; i < kN; ++i) {
                const float x = cx_ + (float(i) - kN * 0.5f) * kCellM;
                const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
                const int line = t.lakeLineAt(t.wx(ci), t.wx(cj), memo);
                if (line == VoxelTerrain::kNoWaterVox) continue;
                const int h = t.heightVox(ci, cj, memo);
                if (!t.wetColumn(ci, cj, h, line, memo)) continue;
                const size_t k = size_t(j) * kN + size_t(i);
                wet_[k] = 1;
                bed_[k] = float(h + 1) * VOXEL_M;
                top_[k] = float(line + 1) * VOXEL_M;
                any_ = true;
            }
        }
        findLake();
        built_ = true;
    }

    // -----------------------------------------------------------------------
    // WHICH OF THE WET CELLS ARE ONE BODY OF WATER.
    //
    // THE SPAWN NEEDS THIS AND NOTHING ELSE DOES. "They need to spawn evened
    // out through the lake at all times" -- through THE LAKE, and a field is
    // not a lake. It is a 420 m square that may hold one pond, or a pond and
    // half a river, or a shoreline running off two edges.
    //
    // Picking uniformly over every wet cell in it gets the arithmetic right and
    // the answer wrong: uniform over AREA puts most of the population in the
    // outer annulus, because that is where most of the area is. Measured on the
    // first build of this: 99 / 153 / 170 m nearest / mean / farthest, which is
    // a ring at the horizon -- the original clustering complaint turned inside
    // out.
    //
    // So the field floods out from the wet cell NEAREST THE PLAYER and keeps
    // the connected component it finds. That is the water you are standing at,
    // and uniform over it is what "evened out through the lake" means. A lake
    // forty metres across gets all of them; one that runs past the field's edge
    // gets them spread to the edge.
    //
    // ONE BFS OVER AT MOST 44,100 CELLS, on the frame that already walked all
    // of them through the terrain generator -- it is a rounding error against
    // the rebuild it rides on.
    // -----------------------------------------------------------------------
    void findLake() {
        lake_.clear();
        if (!any_) return;
        // The seed: the wet cell nearest the middle, which is the player.
        const int mid = kN / 2;
        int seed = -1;
        int bestD2 = 2 * kN * kN + 1;
        for (int j = 0; j < kN; ++j)
            for (int i = 0; i < kN; ++i) {
                const size_t k = size_t(j) * kN + size_t(i);
                if (!wet_[k]) continue;
                const int dx = i - mid, dz = j - mid;
                const int d2 = dx * dx + dz * dz;
                if (d2 < bestD2) { bestD2 = d2; seed = int(k); }
            }
        if (seed < 0) return;

        seen_.assign(size_t(kN) * kN, 0);
        std::vector<int> stack;
        stack.push_back(seed);
        seen_[size_t(seed)] = 1;
        while (!stack.empty()) {
            const int k = stack.back();
            stack.pop_back();
            lake_.push_back(k);
            const int i = k % kN, j = k / kN;
            const int ni[4] = {i - 1, i + 1, i, i};
            const int nj[4] = {j, j, j - 1, j + 1};
            for (int e = 0; e < 4; ++e) {
                if (ni[e] < 0 || nj[e] < 0 || ni[e] >= kN || nj[e] >= kN) continue;
                const size_t n = size_t(nj[e]) * kN + size_t(ni[e]);
                if (seen_[n] || !wet_[n]) continue;
                seen_[n] = 1;
                stack.push_back(int(n));
            }
        }
    }

    // How many cells the lake is, and how far its far side runs -- the offline
    // report prints both, and fill() has nothing to spread over without it.
    size_t lakeCells() const { return lake_.size(); }

    bool any() const { return any_; }
    bool built() const { return built_; }

    // Is there water here, and how deep? Outside the field the answer is NO --
    // which is the safe direction: a fish that swims off the edge of what we
    // know is turned back rather than allowed to leave the lake.
    // -----------------------------------------------------------------------
    // THE SURFACE AT THIS EXACT COLUMN, NOT AT THE CELL IT FALLS IN.
    //
    // (user 2026-09-14: "lillypads are underwater when on a slopped water
    // lake.")
    //
    // THE FIELD IS SAMPLED EVERY TWO METRES and top_ holds one height per cell,
    // taken at that cell's own column. That is the right resolution for
    // steering a fish and the wrong one for FLOATING something, because the
    // water line is not constant across a lake -- lakeLineAt takes x and z, and
    // a body that spans a band seam has a stepped surface. A leaf handed its
    // cell's height is up to a cell away from where it is actually sitting, and
    // on the high side of the step that puts it UNDER the water.
    //
    // The note this replaces said the line is "a property of the lake rather
    // than of the column under it", and that was the assumption: true of a flat
    // pond and not of this world's lakes.
    //
    // ASKED OF THE TERRAIN, not of the field, because the field has no finer
    // answer to give. It costs one lakeLineAt per floating thing per frame --
    // twenty-four pads and sixteen ducks -- against a cell lookup, and it is
    // the same call the field's own rebuild makes.
    // -----------------------------------------------------------------------
    static bool exactTop(const VoxelTerrain &t, TerrainMemo &memo, float x, float z, float *topM) {
        const int i = int(floorf(x / VOXEL_M)), j = int(floorf(z / VOXEL_M));
        const int line = t.lakeLineAt(t.wx(i), t.wx(j), memo);
        if (line == VoxelTerrain::kNoWaterVox) return false;
        *topM = float(line + 1) * VOXEL_M;
        return true;
    }

    bool at(float x, float z, float *topM = nullptr, float *bedM = nullptr) const {
        const int i = int(floorf((x - cx_) / kCellM + kN * 0.5f));
        const int j = int(floorf((z - cz_) / kCellM + kN * 0.5f));
        if (!built_ || i < 0 || j < 0 || i >= kN || j >= kN) return false;
        const size_t k = size_t(j) * kN + size_t(i);
        if (!wet_[k]) return false;
        if (topM) *topM = top_[k];
        if (bedM) *bedM = bed_[k];
        return true;
    }

    // IS THIS INSIDE THE SAMPLED SQUARE AT ALL? A DIFFERENT QUESTION FROM at().
    //
    // at() answers "is there water here", and false covers two completely
    // different situations: dry land inside the field, and anywhere at all
    // outside it. Everything that reads at() was treating them the same, and
    // that is the whole of "a fish got stuck in the air" -- see LakeLife::update.
    bool covers(float x, float z) const {
        const int i = int(floorf((x - cx_) / kCellM + kN * 0.5f));
        const int j = int(floorf((z - cz_) / kCellM + kN * 0.5f));
        return built_ && i >= 0 && j >= 0 && i < kN && j < kN;
    }

    // The nearest wet cell to a point, searched in rings. For getting something
    // that should not be on dry land back off it.
    bool nearestWet(float x, float z, int maxCells, float *wx, float *wz) const {
        if (!built_) return false;
        const int i0 = int(floorf((x - cx_) / kCellM + kN * 0.5f));
        const int j0 = int(floorf((z - cz_) / kCellM + kN * 0.5f));
        for (int r = 0; r <= maxCells; ++r)
            for (int dj = -r; dj <= r; ++dj)
                for (int di = -r; di <= r; ++di) {
                    // The RING only -- the inside of it was covered by a
                    // smaller r, and walking the whole square each time turns
                    // this from O(r) into O(r^2) for no new answers.
                    if (r > 0 && abs(di) != r && abs(dj) != r) continue;
                    const int i = i0 + di, j = j0 + dj;
                    if (i < 0 || j < 0 || i >= kN || j >= kN) continue;
                    if (!wet_[size_t(j) * kN + size_t(i)]) continue;
                    *wx = cx_ + (float(i) - kN * 0.5f) * kCellM + 0.5f * kCellM;
                    *wz = cz_ + (float(j) - kN * 0.5f) * kCellM + 0.5f * kCellM;
                    return true;
                }
        return false;
    }

    // HOW FAR OPEN WATER RUNS ALONG A HEADING, in metres, to a limit. The JS
    // engine's fishReach, and it is the compass for every steering decision the
    // fish makes: cruise picks the freest whisker, flee picks the freest escape.
    // minDepthM: water shallower than this counts as a WALL, not as water.
    //
    // The fish's whiskers used plain at(), which is "is it wet" -- so a lake's
    // shelf read as open water and a cruising fish would steer happily into
    // ten metres of ankle-deep. The step test then refused it cell by cell and
    // the fish turned hard against a shore it had aimed at. In a school that
    // is worse than untidy: the hard turn is what breaks the formation, and the
    // average follower went from 0.36 m off station to 2.20 m when the step
    // test learned about depth and the whiskers did not.
    float reach(float x, float z, float th, float maxM, float minDepthM = 0.0f) const {
        const float sx = sinf(th), sz = cosf(th);
        float d = kCellM;
        while (d <= maxM) {
            float t = 0.0f, b = 0.0f;
            if (!at(x + sx * d, z + sz * d, &t, &b) || (t - b) < minDepthM) return d - kCellM;
            d += kCellM;
        }
        return maxM;
    }

    // One wet cell, picked by a hash -- for spawning. Returns false on a dry
    // field, which is most of the world.
    bool pick(uint32_t h, float *x, float *z, float *topM, float *bedM) const {
        if (lake_.empty()) return false;
        for (int tries = 0; tries < 24; ++tries) {
            const uint32_t q = hashU32(h, uint32_t(tries));
            // STRAIGHT INTO THE LAKE'S OWN CELL LIST. Rejection sampling over
            // the square was the old form and it is the wrong tool now: a lake
            // is a few per cent of a 420 m field, so twenty-four tries would
            // usually find nothing at all and the population would starve.
            const size_t k = size_t(lake_[q % uint32_t(lake_.size())]);
            const int i = int(k % size_t(kN)), j = int(k / size_t(kN));
            if (!wet_[k]) continue;
            // -- SOMEWHERE IN THE CELL, NOT AT ITS CORNER -------------------
            //
            // The cell is two metres now, and returning its corner would put
            // every lily pad in the wood on one two-metre lattice -- which does
            // not read as scattered, it reads as planted. The offset stays
            // inside the cell (0.15..0.85 of it) so the position still maps
            // back to the cell whose depth is being reported.
            const float jx = 0.15f + hashUnit(0x9C1u, q) * 0.70f;
            const float jz = 0.15f + hashUnit(0x9C2u, q) * 0.70f;
            *x = cx_ + (float(i) - kN * 0.5f) * kCellM + jx * kCellM;
            *z = cz_ + (float(j) - kN * 0.5f) * kCellM + jz * kCellM;
            *topM = top_[k];
            *bedM = bed_[k];
            return true;
        }
        return false;
    }

  private:
    float cx_ = 0.0f, cz_ = 0.0f;
    bool built_ = false, any_ = false;
    std::vector<uint8_t> wet_;
    std::vector<float> top_, bed_;
    std::vector<int> lake_;    // the connected body the player is at -- findLake
    std::vector<uint8_t> seen_;
};

// ---------------------------------------------------------------------------
// EVERYTHING THAT LIVES ON THE LAKE.
// ---------------------------------------------------------------------------
class LakeLife {
  public:
    bool ready() const { return ready_; }

    // -----------------------------------------------------------------------
    // The three model sets. A missing set disables only itself -- a world with
    // no dragonfly art still gets its fish.
    // -----------------------------------------------------------------------
    bool load(World &world, const std::string &lifeDir, const std::string &decorDir) {
        loadStrip(world, lifeDir + "/salmon", kSalmonFrames, &salmon_, "salmon");
        loadStrip(world, lifeDir + "/bass", kSalmonFrames, &bass_, "bass");
        // -- THE KOI'S TWELVE FRAMES ARE INSIDE ONE FILE ------------------
        //
        // It is loaded by hand rather than through loadStrip, and not because
        // there is nothing to number: there are twelve frames, exactly as many
        // as the salmon has. They are just packaged differently -- the salmon
        // is twelve numbered .vox files in a folder, and koi.vox is ONE file
        // with twelve models keyframed onto a single shape node.
        //
        // IT USED TO BE READ AS ONE MODEL, and the note here used to say the
        // koi had no swim cycle. It has one; what it did not have was a reader.
        // voxLoad COMPOSES a file, so all twelve frames were laid on top of one
        // another -- 26 voxels became 130 in the same 5 x 10 x 4 box -- and
        // what came out was a solid lump that never flexed. Reported as "it's
        // loading all frames at once" (user 2026-09-13). voxParse no longer
        // composes a shape's extra models, so that path gives a clean frame 0
        // now; this asks for all twelve instead.
        //
        // voxLoadAll IS THE RIGHT READER because the frames are the file's
        // SIZE/XYZI pairs in order, and for a keyframed shape that order is the
        // authored one -- checked against koi.vox's own node, whose model list
        // is 0..11. Nothing here needs the graph: one shape means one
        // translation, and every frame is centred on it.
        loadFrames(world, lifeDir + "/koi.vox", &koi_, "koi");
        loadStrip(world, lifeDir + "/minnow", kSalmonFrames, &minnow_, "minnow");
        loadStrip(world, lifeDir + "/catfish", kSalmonFrames, &catfish_, "catfish");
        loadStrip(world, lifeDir + "/blue_gill", kSalmonFrames, &bluegill_, "blue gill");
        loadStrip(world, lifeDir + "/betta", kBettaFrames, &betta_, "betta");

        // THE DUCKS ARE TWO SINGLE MODELS, not strips -- base.vox is the mother
        // and baby.vox the duckling, and neither has a paddle cycle. A duck is
        // carried by the water rather than by its own animation, so a still
        // model reads correctly in a way a still fish would not.
        {
            const char *kD[2] = {"/duck/base.vox", "/duck/baby.vox"};
            for (int i = 0; i < 2; ++i) {
                VoxModel mo;
                std::string err;
                if (!voxLoad(lifeDir + kD[i], &mo, &err)) {
                    std::fprintf(stderr, "v2: duck %s\n", err.c_str());
                    continue;
                }
                int sx = 0, sy = 0, sz = 0;
                const int m = world.addFlyerModel(mo, i ? "duckling" : "duck", &sx, &sy, &sz, true);
                if (m < 0) continue;
                (i ? duckB_ : duckM_).push_back(m);
                if (i) {
                    babyHX_ = 0.5f * float(sx) * VOXEL_M;
                    babyHY_ = 0.5f * float(sy) * VOXEL_M;
                    babyHZ_ = 0.5f * float(sz) * VOXEL_M;
                    scanBabyEyes(mo);
                } else {
                    duckHX_ = 0.5f * float(sx) * VOXEL_M;
                    duckHY_ = 0.5f * float(sy) * VOXEL_M;
                    duckHZ_ = 0.5f * float(sz) * VOXEL_M;
                }
                // ...AND THE DISC THAT MEETS A LILY PAD. A duck turns freely
                // about Y, so the only footprint true at every heading is the
                // circle round its longer half -- the same argument, and the
                // same arithmetic, as lilyR_.
                (i ? babyR_ : duckR_) = maxf(0.5f * float(sx) * VOXEL_M,
                                             0.5f * float(sz) * VOXEL_M);
            }
        }
        ducks_.assign(size_t(kDuckCount + kBabyCount), Duck{});
        loadStrip(world, lifeDir + "/dragonfly", kDflyFrames, &dfly_, "dragonfly");

        static const char *kPads[kLilyModels] = {"lillypad_small", "lillypad_medium",
                                                 "lillypad_large"};
        for (int i = 0; i < kLilyModels; ++i) {
            const std::string path = decorDir + "/" + kPads[i] + ".vox";
            VoxModel mo;
            std::string err;
            if (!voxLoad(path, &mo, &err)) {
                std::fprintf(stderr, "v2: lily %s: %s -- skipped\n", path.c_str(), err.c_str());
                continue;
            }
            int sx = 0, sy = 0, sz = 0;
            const int m = world.addFlyerModel(mo, kPads[i], &sx, &sy, &sz);
            if (m < 0) continue;
            lily_.push_back(m);
            lilyHalf_.push_back(0.5f * float(sy) * VOXEL_M);
            // ...AND ACROSS, WHICH IS WHAT WAS MISSING. See putPad: the
            // translation handed to place() is the model's CORNER, so the half
            // has to be taken off or the leaf is drawn beside its own position.
            lilyHX_.push_back(0.5f * float(sx) * VOXEL_M);
            lilyHZ_.push_back(0.5f * float(sz) * VOXEL_M);
            // ...AND THE DISC THAT BOUNCES. A leaf spins freely (`th` is its
            // own), so the only footprint that is true at every heading is the
            // circle round its longest half -- which is what has to be kept off
            // the sand. Plus a few centimetres, so the rim stops just short of
            // the bank rather than exactly on it.
            lilyR_.push_back(maxf(lilyHX_.back(), lilyHZ_.back()) + kLilyBankM);
        }

        fish_.resize(size_t(kSalmonCount + kBassCount + kKoiCount + kMinnowCount + kCatfishCount +
                            kBluegillCount + kBettaCount));
        pads_.resize(kLilyCount);
        flies_.resize(kDflyCount);
        fishHold_.clear();
        dflyHold_.clear();
        duckHold_.clear();
        ready_ = !salmon_.empty() || !bass_.empty() || !koi_.empty() || !minnow_.empty() ||
                 !lily_.empty() || !dfly_.empty() || !duckM_.empty();
        if (ready_)
            std::printf("  lake     %zu betta + %zu salmon + %zu bass + %zu koi + %zu minnow "
                        "+ %zu catfish "
                        "+ %zu blue gill frames, %zu lily models, %zu dragonfly frames\n",
                        betta_.size(), salmon_.size(), bass_.size(), koi_.size(),
                        minnow_.size(), catfish_.size(), bluegill_.size(), lily_.size(),
                        dfly_.size());
        return ready_;
    }

    // -----------------------------------------------------------------------
    // ONE TICK. The field first, because everything else reads it.
    // -----------------------------------------------------------------------
    // `look` is which way the player is FACING, for the birth cone -- see
    // kBirthConeCos. Zero (the default) tests distance only, which is what the
    // offline renders pass and why their reports are comparable across this
    // change.
    // HOW FAR THE LILY PADS REACH -- see kPadReachDefaultM. Public because the
    // app sets it from the world's own chunk ring each frame, so it follows
    // --view rather than being a constant that quietly stops matching.
    float padReachM = kPadReachDefaultM;

    // -- WHICH WOOD A COLUMN IS IN, as a kWood* bit -----------------------
    //
    // (user 2026-09-21: the betta is "the cherry forest water/lakes".)
    //
    // NOTHING IN THIS FILE HAS EVER ASKED. Water is water and a salmon has no
    // opinion about the trees on the bank -- the same reasoning the
    // butterflies give. The betta is the first lake creature that belongs to
    // ONE BAND, so this is the gate, held the way Bunnies holds its own and
    // for the same reason: one population asks it, per candidate site, and
    // threading a template parameter through every filler to save an indirect
    // call would spread it over the class.
    //
    // ASKED OF THE SITE, NOT OF THE FISH. A betta may drift across the seam
    // afterwards and that is fine; a population that thinned every time one of
    // them did would not be.
    std::function<uint8_t(float)> wood;

    void update(float dt, const VoxelTerrain &terrain, const Vec3 &player,
                const Vec3 &look = Vec3(0.0f, 0.0f, 0.0f)) {
        if (!ready_) return;
        // BORROWED FOR THE TICK, so the things that FLOAT can ask for the water
        // line at their own column -- see WaterField::exactTop. The duck is why
        // it is a member: stepDuckBody is three calls deep and threading a
        // terrain reference through all of them to answer one question is more
        // signature than the question is worth.
        terrain_ = &terrain;
        clock_ += dt;
        // BEFORE ANYTHING ASKS. It is what decides whether this tick is allowed
        // to put a creature down where you are looking -- see kBirthMinM.
        birth_.tick(dt, player.x, player.z, look.x, look.z);
        if (field_.stale(player)) field_.rebuild(terrain, player);
        recycle(player, dt);
        // ...AND A KILL IS GIVEN BACK ON THE LAKE'S OWN DROP -- see KillHold.
        fishHold_.release(player.x, player.z, kLakeDropM);
        dflyHold_.release(player.x, player.z, kLakeDropM);
        duckHold_.release(player.x, player.z, kLakeDropM);
        fill(terrain, player);
        // -- A CREATURE OUTSIDE THE FIELD HOLDS STILL, IT DOES NOT VANISH ---
        //
        // Steering reads WaterField::at, and outside the square that answers
        // "no water" -- so a creature out there would not merely lose its
        // bearings, it would turn hard every frame and thrash. The staleness
        // rule above is supposed to make this impossible; this is the belt to
        // its braces, for the frames between a player crossing the margin and
        // the rebuild landing.
        //
        // STILL LIVE AND STILL DRAWN. Holding position for a few frames at
        // ninety metres is invisible; disappearing is not, which is the whole
        // point of this pass.
        auto inField = [&](float x, float z) { return field_.at(x, z); };
        // THE LEADER IS PASSED IN RATHER THAN LOOKED UP, so stepFish stays a
        // function of one fish and whatever it is following -- and a follower
        // whose leader is mid-leap is simply on its own for those two seconds,
        // which is what a fish would do.
        for (size_t i = 0; i < fish_.size(); ++i) {
            Fish &f = fish_[i];
            if (!f.live || !inField(f.x, f.z)) continue;
            const Fish *ld = nullptr;
            if (f.lead >= 0 && size_t(f.lead) < fish_.size()) {
                const Fish &L = fish_[size_t(f.lead)];
                // A LEAPING LEADER IS STILL A LEADER. Its x and z keep
                // advancing through the arc and the station is horizontal, so
                // there is nothing to follow it into the air WITH -- the depth
                // comes from lead->hold, which a leap does not touch. Dropping
                // the school for the two seconds of an arc left every follower
                // on its own whiskers, and they scattered.
                if (L.live) ld = &L;
            }
            // THE TURN IS DIFFERENCED AROUND THE STEP, not inside it: the
            // heading has several writers in there (the steer, the wall slide,
            // the school station) and instrumenting each is how one gets
            // missed. See Fish::dth.
            const float thWas = f.th;
            stepFish(&f, ld, dt, player);
            // Wrapped to (-pi, pi] the same way Bunnies::angleTo does; that one
            // is a member of another class and this file cannot see it.
            const float dth = f.th - thWas;
            f.dth = atan2f(sinf(dth), cosf(dth));
        }
        separateFish();
        // -- ...AND ANYTHING THAT IS SOMEHOW NOT OVER WATER COMES BACK -------
        //
        // The loop above skips a fish that is not over water, and for a fish
        // beyond the field that is right -- it is ninety metres away and
        // holding still is invisible. INSIDE the field it is a bug, and the
        // skip turns the bug into a permanent one: the fish is never stepped
        // again, so it hangs wherever it stopped. That is precisely "a fish
        // tried to jump out of water bounds and got stuck in the air" -- the
        // leap abandoned itself over a bank and the fish was then frozen there
        // for the rest of the session.
        //
        // Prevention is the arc validation in stepFish; this is the net under
        // it, because a lake can also drain out from under a fish when the
        // field is rebuilt somewhere new.
        for (Fish &f : fish_)
            if (f.live && !field_.at(f.x, f.z) && field_.covers(f.x, f.z))
                recoverFish(&f, dt);
        for (Pad &p : pads_) if (p.live && inField(p.x, p.z)) stepPad(&p, dt, terrain);
        // MOTHERS FIRST, THEN THE LINE. A duckling steers at a spot behind its
        // leader, so the leader has to have moved this frame or the whole line
        // is chasing where the family was last frame -- which at four animals
        // deep is four frames of lag by the tail.
        for (size_t i = 0; i < ducks_.size(); ++i)
            if (ducks_[i].live && ducks_[i].mom < 0 && inField(ducks_[i].x, ducks_[i].z))
                stepDuck(&ducks_[i], uint32_t(i), dt, player);
        // AN ORPHAN GOES DOWN THE MOTHER'S PATH, which is v1's structure
        // literally: its follow-the-line branch is `isBaby && !orphan` and the
        // else is the adult wander, shared. Everything that branch needs is
        // already right for a duckling -- stepDuck asks duckRadius, which
        // answers babyR_ for anything with a mother index, and kDuckSpeed is
        // 0.70 where kBabyKeep is 0.7, so an orphan paddles at exactly the
        // steady pace v1 gives it ("never the 10 of a duckling scrambling to
        // catch up"). Wandering it in its own function would have been a second
        // edge-avoider to keep in step with the first.
        for (size_t i = 0; i < ducks_.size(); ++i) {
            Duck &d = ducks_[i];
            if (!d.live || d.mom < 0) continue;
            if (d.orphan) {
                if (inField(d.x, d.z)) stepDuck(&d, uint32_t(i), dt, player);
            } else {
                stepDuckling(&d, uint32_t(i), dt);
            }
        }
        cryTick();
        // AFTER THE WHOLE FAMILY HAS MOVED, not inside the step: a duckling is
        // placed relative to a leader that has already moved this tick, so
        // correcting one mid-line would be correcting a position the rest of the
        // line has not been told about yet. Same reasoning as separateFish.
        duckOffPads(dt);
        for (Dfly &d : flies_) if (d.live && inField(d.x, d.z)) stepDfly(&d, dt, player);
    }

    // -----------------------------------------------------------------------
    // ...AND WHERE THE THREE OF THEM ARE, AS INSTANCES.
    //
    // The slot layout is FIXED per population for the reason the flyer band
    // itself is: a lily taking a fish's slot mid-drift is a motion vector
    // between two unrelated objects, which is the one thing the band exists to
    // get right.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        // THE LAKE IS EXPECTED TO BE CLEAN AND IS CHECKED ANYWAY. Nothing that
        // lives here can meet a rock: scatter() refuses any column within eight
        // voxels of the waterline, so a boulder is never in the water in the
        // first place. That is an argument about another file, which is exactly
        // the kind that stops being true without anybody editing this one.
        // EVERY ONE OF THEM FLAGGED inWater. A fish swims in it, a duck floats
        // at the line, a pad lies on it and a dragonfly works the surface a
        // hand's breadth up -- all four are AT the water by definition, and the
        // check that finds a land animal under it must not also find these.
        for (const Fish &f : fish_)
            if (f.live) out->push_back({Vec3(f.x, f.y, f.z), "fish", 0.2f, false, true});
        for (const Duck &d : ducks_)
            if (d.live) out->push_back({Vec3(d.x, d.y, d.z), "duck", 0.25f, false, true});
        for (const Pad &p : pads_)
            if (p.live) out->push_back({Vec3(p.x, p.y, p.z), "lilypad", 0.45f, false, true});
        for (const Dfly &d : flies_)
            if (d.live) out->push_back({Vec3(d.x, d.y, d.z), "dragonfly", 0.15f, false, true});
    }

    void publish(World &world) {
        if (!ready_) return;
        const int base = kButterflySlots + kBirdSlots;
        int slot = base;
        for (const Fish &f : fish_) putFish(world, slot++, f);
        for (const Pad &p : pads_) putPad(world, slot++, p);
        for (const Dfly &d : flies_) putDfly(world, slot++, d);
        for (const Duck &d : ducks_) putDuck(world, slot++, d);
        world.flushFlyerInstances();
    }

    // -- HOW FAR OUT THE POPULATION REACHES, for the offline report --------
    //
    // It is here because "they spawn in clusters nearest the player" is a claim
    // about a DISTRIBUTION, and the only honest way to answer it is to print
    // one. Nearest, mean and farthest over everything alive: a band spawn shows
    // as three numbers within a few metres of each other, a spread one does not.
    void spread(const Vec3 &p, float *lo, float *mean, float *hi) const {
        *lo = 1e9f;
        *hi = 0.0f;
        *mean = 0.0f;
        int n = 0;
        auto take = [&](float x, float z) {
            const float d = sqrtf((x - p.x) * (x - p.x) + (z - p.z) * (z - p.z));
            *lo = minf(*lo, d);
            *hi = maxf(*hi, d);
            *mean += d;
            ++n;
        };
        for (const Fish &f : fish_) if (f.live) take(f.x, f.z);
        for (const Pad &q : pads_) if (q.live) take(q.x, q.z);
        for (const Dfly &d : flies_) if (d.live) take(d.hx, d.hz);
        if (n == 0) { *lo = 0.0f; return; }
        *mean /= float(n);
    }

    // -- THE SCHOOLS, AS A STRING ------------------------------------------
    //
    // "swim in schools of 3-6" is a claim about a partition, and the only way
    // to check a partition is to print it. Writes "4 + 3 + 3" and returns how
    // many schools that was; a lake where every fish is its own school prints
    // ten ones, which is the failure this is here to catch.
    // How far the followers are from where they are supposed to be, averaged.
    // A formation that holds reads well under a metre; one that has quietly
    // become "five fish near each other" reads several.
    float stationErr() const {
        float sum = 0.0f;
        int n = 0;
        for (const Fish &f : fish_)
            if (f.live && f.lead >= 0) { sum += f.schoolErr; ++n; }
        return n ? sum / float(n) : 0.0f;
    }

    int schools(char *out, size_t n, int species) const {
        size_t at = 0;
        int ns = 0;
        if (n) out[0] = 0;
        for (size_t i = 0; i < fish_.size(); ++i) {
            // want > 0 KEEPS THE BASS OUT OF IT. Every bass has lead == -1, so
            // without this the report read "6 + 4 + 1 + 1 + 1 + 1 + 1 + 1" --
            // six solitary fish counted as six schools of one, which is exactly
            // the failure this line exists to catch and would have hidden it.
            if (!fish_[i].live || fish_[i].lead != -1 || fish_[i].want <= 0) continue;
            // ...AND OF THE SPECIES ASKED FOR. Two of the four fish school now,
            // and without this the line read "4 schools of salmon: 6 + 5 + 3 + 4"
            // over a lake holding six salmon -- it was counting the minnows'
            // schools as theirs.
            if (fish_[i].species != species) continue;
            const int k = schoolSize(int(i));
            const int w = std::snprintf(out + at, (at < n) ? n - at : 0, "%s%d",
                                        ns ? " + " : "", k);
            if (w > 0) at += size_t(w);
            ++ns;
        }
        return ns;
    }

    // How many of each swim here -- the offline report prints it, and it is the
    // only way to tell "the bass never loaded" from "the bass are behind you".
    int ducksLiving(bool mothers) const {
        int n = 0;
        for (const Duck &d : ducks_)
            if (d.live && ((d.mom < 0) == mothers)) ++n;
        return n;
    }

    // How often a duck met a lily pad, and how often it was INSIDE one. Both
    // are body-tick counts accumulated since the lake was loaded; the second
    // must be zero. See duckOffPads.
    // NOT named *near*. windows.h still defines near and far as empty macros
    // from the segmented-memory era, so the parameter simply DISAPPEARS and the
    // line below compiles as *= padNear_ -- reported as C2059 at the assignment,
    // which names neither the macro nor the header. birds.h and app.h each
    // carry this note already; this is the fourth time in the project.
    void duckPads(long *nearN, long *clash, float *worst, float *closest) const {
        *nearN = padNear_;
        *clash = padClash_;
        *worst = padWorst_;
        // ...AND HOW CLOSE THE TWO POPULATIONS EVER ACTUALLY GET, which is the
        // question that has to be answered before the two numbers above mean
        // anything: a run where no duck was ever within twenty metres of a leaf
        // reports zero clashes whatever the code does.
        *closest = padClosest_;
    }

    // -----------------------------------------------------------------------
    // WHERE THE BANK IS, for anything that lives BESIDE the water rather than
    // in it.
    //
    // THE FROG IS THE ONLY CALLER and that is why this is here rather than in
    // render/critters.h: the water field is 420 m of sampled columns this file
    // already rebuilds and already owns, and a second population re-deriving
    // "where is the lake" off the terrain would be a SECOND DEFINITION OF WET
    // in the engine -- exactly the drift wetColumnAt exists to prevent.
    //
    // A SPOT IN THE WATER, THEN A STEP OUT OF IT. pick() lands IN the lake --
    // it is what the fish spawn through -- and the walk outward from there is
    // what makes this a bank rather than a puddle. v1 arrives at the same place
    // from the other end: its DES_WATER is a SHORE RADIUS and not a wet test,
    // "because a frog sits ON the bank, not in the lake".
    void bankSpots(uint32_t seed, int want, std::vector<Vec3> *out) const {
        out->clear();
        if (!field_.built() || !field_.any()) return;
        for (int k = 0; k < want * 8 && int(out->size()) < want; ++k) {
            float wx = 0.0f, wz = 0.0f, top = 0.0f, bed = 0.0f;
            if (!field_.pick(hashU32(seed, uint32_t(k)), &wx, &wz, &top, &bed)) return;
            const float a = hashUnit(0x5A1u, hashU32(seed, uint32_t(k) * 13u + 7u)) * 6.2831853f;
            for (int r = 1; r <= 8; ++r) {
                const float bx = wx + cosf(a) * float(r);
                const float bz = wz + sinf(a) * float(r);
                if (field_.at(bx, bz)) continue;   // still in the lake
                out->push_back(Vec3(bx, 0.0f, bz));
                break;
            }
        }
    }

    void census(int *byFish, int *pads, int *flies) const {
        for (int i = 0; i < kFishSpecies; ++i) byFish[i] = 0;
        *pads = *flies = 0;
        for (const Fish &f : fish_)
            if (f.live) ++byFish[f.species & (kFishSpecies - 1)];
        for (const Pad &p : pads_) *pads += p.live ? 1 : 0;
        for (const Dfly &d : flies_) *flies += d.live ? 1 : 0;
    }

    int living() const {
        int n = 0;
        for (const Fish &f : fish_) n += f.live;
        for (const Pad &p : pads_) n += p.live;
        for (const Dfly &d : flies_) n += d.live;
        for (const Duck &d : ducks_) n += d.live;
        return n;
    }

    // -- WHERE THE NEAREST ONE IS, per population ----------------------------
    //
    // What /locate salmon, /locate duck and the rest are taken to. Four
    // functions rather than one with a selector, because the four containers
    // hold four unrelated structs and the only thing a selector would buy is
    // one switch here instead of one switch in the caller.
    //
    // THE FISH IS THE ONLY ONE THAT ASKS WHICH. Six species share `fish_` and
    // are told apart by `species`, laid out in the fill order at the top of
    // fillAll -- 0 salmon, 1 bass, 2 koi, 3 minnow, 4 catfish, 5 blue gill.
    // Pass -1 for any of them.
    //
    // XZ ONLY, like Bunnies::nearest and Bees::nearest: a catfish on the bed
    // is not further off than a salmon at the surface above it, and the
    // arrival is a spot on the shore either way -- see standNear in app.h.
    bool nearestFish(const Vec3 &p, int species, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Fish &f : fish_) {
            if (!f.live || (species >= 0 && f.species != species)) continue;
            const float dx = f.x - p.x, dz = f.z - p.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = Vec3(f.x, f.y, f.z);
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    // A MOTHER, NOT A DUCKLING. The three in the line are hers and are within
    // a couple of metres of her, so sending the player to whichever of the
    // four happens to be nearest would be the same trip with a worse aim.
    bool nearestDuck(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Duck &d : ducks_) {
            if (!d.live || d.mom >= 0) continue;
            const float dx = d.x - p.x, dz = d.z - p.z;
            const float q = dx * dx + dz * dz;
            if (q >= best) continue;
            best = q;
            if (at) *at = Vec3(d.x, d.y, d.z);
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    bool nearestPad(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Pad &q : pads_) {
            if (!q.live) continue;
            const float dx = q.x - p.x, dz = q.z - p.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = Vec3(q.x, q.y, q.z);
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    bool nearestDfly(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Dfly &d : flies_) {
            if (!d.live) continue;
            const float dx = d.x - p.x, dz = d.z - p.z;
            const float q = dx * dx + dz * dz;
            if (q >= best) continue;
            best = q;
            if (at) *at = Vec3(d.x, d.y, d.z);
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    // -----------------------------------------------------------------------
    // THAT ONE IS DEAD -- this population's half of a kill. See the same method
    // in render/butterflies.h for the whole of the reasoning; `i` is the index
    // within THIS population's run of the instance band and App::killLifeAt
    // does the arithmetic.
    // -----------------------------------------------------------------------
    // FOUR POPULATIONS IN ONE RUN, in publish()'s own order: fish, pads,
    // dragonflies, ducks. A PAD IS NOT ALIVE and lifeAtSlot never offers one,
    // but this refuses it anyway -- the two tables are in different files and
    // only one of them can be the authority on what a slot holds.
    // Where a tear should be born this tick, and the drain that empties it.
    // See cryTick.
    const std::vector<Vec3> &tearsThisTick() const { return tears_; }

    // -----------------------------------------------------------------------
    // ...AND WHAT THE DUCKS ARE DOING, FOR --duck-test.
    //
    // A family is the one population whose members depend on each OTHER, so
    // "the brood outlived the mother" is not a question any general life probe
    // can answer -- it needs the mother index, the brood index and the orphan
    // flag together.
    // -----------------------------------------------------------------------
    // The ducks_ index of a live mother whose whole brood is live, or -1.
    int firstFamily() const {
        for (int i = 0; i < kDuckCount; ++i) {
            if (!ducks_[size_t(i)].live || ducks_[size_t(i)].mom >= 0) continue;
            int n = 0;
            for (int b = 0; b < kBabyPerDuck; ++b)
                if (ducks_[size_t(kDuckCount) + size_t(i) * kBabyPerDuck + size_t(b)].live) ++n;
            if (n == kBabyPerDuck) return i;
        }
        return -1;
    }
    // The ducks_ index of one of a mother's ducklings.
    static int babyIndex(int mom, int sib) {
        return kDuckCount + mom * kBabyPerDuck + sib;
    }
    bool duckProbe(int i, bool *live, bool *orphan, bool *crying, Vec3 *at) const {
        if (i < 0 || size_t(i) >= ducks_.size()) return false;
        const Duck &d = ducks_[size_t(i)];
        *live = d.live;
        *orphan = d.orphan;
        *crying = d.cryTo > 0.0f;
        *at = Vec3(d.x, d.y, d.z);
        return true;
    }
    // Where a duck sits in the LAKE's own slot run -- the publish order is
    // fish, pads, dragonflies, then ducks. The caller adds the band's base.
    static int duckLocalSlot(int i) {
        return kSalmonCount + kBassCount + kKoiCount + kMinnowCount + kCatfishCount +
               kBluegillCount + kBettaCount + kLilyCount + kDflyCount + i;
    }

    // Every one HELD WHERE IT DIED -- see KillHold in core/noise.h -- at its
    // own vector's index.
    bool killSlot(int i) {
        if (i < 0) return false;
        if (size_t(i) < fish_.size()) {
            Fish &f = fish_[size_t(i)];
            if (!f.live) return false;
            fishHold_.hold(i, f.x, f.z);
            // -- AND ITS SCHOOL TAKES THE LOSS --------------------------------
            //
            // A school short of its `want` is a school fillFish tops up, and it
            // does it BESIDE THE LEADER with no birth floor at all -- so a
            // salmon shot out of a shoal was replaced by a new one growing in at
            // the next station, in the water you were looking at. The school is
            // one smaller now, and stays that way.
            //
            // A dead LEADER passes its want on through its own empty slot:
            // promoteSchools reads it from there when it crowns the heir.
            if (f.lead >= 0) {
                Fish &L = fish_[size_t(f.lead)];
                if (L.live && L.want > 1) --L.want;
                f = Fish{};
            } else {
                const int want = f.want;
                f = Fish{};
                f.want = want > 1 ? want - 1 : want;
            }
            return true;
        }
        i -= int(fish_.size());
        if (size_t(i) < pads_.size()) return false;   // a lily pad is not life
        i -= int(pads_.size());
        if (size_t(i) < flies_.size()) {
            if (!flies_[size_t(i)].live) return false;
            dflyHold_.hold(i, flies_[size_t(i)].x, flies_[size_t(i)].z);
            flies_[size_t(i)] = Dfly{};
            return true;
        }
        i -= int(flies_.size());
        if (size_t(i) < ducks_.size()) {
            if (!ducks_[size_t(i)].live) return false;
            // -- HER BROOD OUTLIVES HER ------------------------------------
            //
            // (user 2026-09-15: "the babies should not dissapeare when the
            // mother dies ... the babies should cry".)
            //
            // v1's startCrying, armed HERE for v1's own stated reason: "at the
            // moment she is confirmed dead ... armed HERE, not at the hit, so a
            // wounded-but-alive mother never sets them off".
            //
            // THE WAIT IS NOT DECORATION. CRY_WAIT is 900 ms and v1 says why --
            // the tears start "after the mother's death poof has cleared". A
            // duckling weeping through the smoke of the kill reads as part of
            // the explosion; weeping once it has blown away reads as grief.
            if (ducks_[size_t(i)].mom < 0) {
                for (int b = 0; b < kBabyPerDuck; ++b) {
                    const size_t bi = size_t(kDuckCount) + size_t(i) * kBabyPerDuck + size_t(b);
                    if (bi >= ducks_.size()) break;
                    Duck &k = ducks_[bi];
                    if (!k.live || k.mom != i) continue;
                    k.orphan = true;
                    k.cryFrom = clock_ + kCryWaitSec;
                    k.cryTo = k.cryFrom + kCrySec;
                    k.cryNext = k.cryFrom;
                    k.cryEye = b;   // the three of them do not weep in step
                }
            }
            duckHold_.hold(i, ducks_[size_t(i)].x, ducks_[size_t(i)].z);
            ducks_[size_t(i)] = Duck{};
            return true;
        }
        return false;
    }

  private:
    // -- one creature each ---------------------------------------------------
    struct Fish {
        bool live = false;
        // -- HOW LONG IT HAS BEEN HERE, AND HOW LONG IT HAS BEEN LEAVING ----
        //
        // The butterflies' own pair (see kFlyFadeSec). age drives the grow-in,
        // dying the shrink-out, and dying < 0 means "not leaving" rather than
        // zero, because zero is the first frame of a departure.
        float age = 0.0f, dying = -1.0f;
        float x = 0, y = 0, z = 0;
        float th = 0;        // heading
        // -- HOW FAR IT TURNED THIS FRAME -----------------------------------
        //
        // (user 2026-09-21: "apply motion vectors to the life that does use
        //  them".)
        //
        // place() differences the model's box centre, which says where the
        // fish WENT and nothing about where it FACED -- and a fish turns
        // constantly. See the same field on March and the spin channel in
        // setFlyerInstance; the perched songbirds have used it since they were
        // reported for exactly this.
        float dth = 0.0f;
        float om = 0;        // turn rate, eased
        float spd = kFishCruise;
        float pitch = 0;
        float vy = 0;
        float hold = 0;      // the depth it is holding, 0..1 of the column
        float animClk = 0;   // frames, on the SIM clock -- see stepFish
        float fleeUntil = -1.0f;
        float thrX = 0, thrZ = 0;
        float senseAt = 0, holdAt = 0, fleeAt = 0;
        float smTh = 0.0f;        // the heading the SCHOOL sees -- see stepFish
        float schoolErr = 0.0f;   // metres off station -- see kSchoolCloseRate
        float schoolSpd = kFishCruise;   // ...and what the station controller asked for
        float navTh = 0;     // long-range intent, bent by the whiskers
        // -- THE LEAP. jumpV is only meaningful while airborne; jumpOn says so
        //    rather than a sentinel value, because 0 is a real vertical speed
        //    at the top of the arc.
        bool jumpOn = false;
        float jumpV = 0.0f;
        float jumpTop = 0.0f;  // the launch speed being eased onto -- see kJumpRiseRate
        float jumpAt = 0.0f;   // when the next attempt is allowed
        int pose = 0, poseWas = 0;
        // -- THE SCHOOL. lead is the INDEX of the fish this one follows, or -1
        //    for a leader. want is how many the school is trying to be and is
        //    only meaningful on a leader; slotR/slotF are this fish's station in
        //    the leader's own frame.
        int lead = -1;
        int want = 0;
        int species = 0;          // 0 salmon, 1 bass -- see kBassCount
        int cx = 0, cz = 0;       // the lattice cell it was born in, if it owns one
        bool owns = false;
        // The BASE offset in the leader's frame -- see kSchoolSpreadM. wan* are
        // this fish's own drift about it, and shuffleAt is when the base is
        // re-rolled so it changes places with the rest of the shoal.
        float slotR = 0.0f, slotF = 0.0f;
        float wanRA = 0.0f, wanRB = 0.0f, wanPA = 0.0f, wanPB = 0.0f;
        float shuffleAt = 0.0f;
        float holdOff = 0.0f;   // its own depth inside the shoal
    };
    struct Pad {
        bool live = false;
        int cx = 0, cz = 0;        // the site it belongs to -- see siteOf
        float sx = 0.0f, sz = 0.0f;
        // -- HOW LONG IT HAS BEEN HERE, AND HOW LONG IT HAS BEEN LEAVING ----
        //
        // The butterflies' own pair (see kFlyFadeSec). age drives the grow-in,
        // dying the shrink-out, and dying < 0 means "not leaving" rather than
        // zero, because zero is the first frame of a departure.
        float age = 0.0f, dying = -1.0f;
        // GIVING UP A SLOT ON PURPOSE, which is not the same as being out of
        // range -- see yieldSite. It has to be a field of its own because
        // recycle() CANCELS a fade whenever the creature is back inside the
        // drop radius, and a retiring pad never left it.
        bool retire = false;
        float stuck = 0.0f;  // seconds aground with nowhere to go -- kLilyStuckSec
        float x = 0, y = 0, z = 0;
        float th = 0;        // the MODEL's spin -- independent of where it drifts
        float spin = 0;
        float mth = 0;       // the drift heading
        float turnAt = 0;
        int model = 0;
    };
    struct Duck {
        bool live = false;
        int cx = 0, cz = 0;        // the family's lattice cell -- mothers only
        int mom = -1;              // -1 for a mother; otherwise her index in ducks_
        int sib = 0;               // 0, 1 or 2 -- its place in the line
        float x = 0, y = 0, z = 0;
        float th = 0, om = 0, omT = 0;
        float turnAcc = 0.0f;      // signed wind-up -- see kDuckWindUp
        float tRe = 0.0f;          // when the wander may pick a new heading
        float age = 0.0f, dying = -1.0f;
        // -- ORPHANED (user 2026-09-15: "the babies should not dissapeare when
        //    the mother dies, but stay on the field, and wander in random
        //    directions without their mother") ---------------------------
        //
        // v1 calls this `mom5.slain` and asks it in four places; it is kept on
        // the DUCKLING here rather than on the mother for one reason, and it is
        // the reason v1's version is fragile: a mother slot is REFILLED. Ask
        // "is my mother dead" of a slot and the answer flips back to no the
        // moment a stranger is placed in it, and a brood that had been paddling
        // the lake alone for a minute falls in behind her.
        //
        // Once true this is never read back off `mom` again -- see every
        // `orphan ||` below, which is the same list of four.
        bool orphan = false;
        // ...and the weeping. cryTo is the moment it stops, cryNext the moment
        // the next tear is due, cryEye a free-running counter that alternates
        // cheeks. v1's CRY_WAIT/CRY_MS/CRY_GAP, and its note on why cryEye is
        // NOT wrapped to the eye list: baby.vox has ONE black head voxel, so
        // `% length` pinned the alternation to a single cheek for ever.
        float cryFrom = -1.0f, cryTo = -1.0f, cryNext = -1.0f;
        int cryEye = 0;
    };

    struct Dfly {
        bool live = false;
        int cx = 0, cz = 0;        // the site it belongs to -- see siteOf
        float age = 0.0f, dying = -1.0f;   // see the note on Fish
        bool retire = false;               // ...and see Pad::retire
        float x = 0, y = 0, z = 0;
        float hx = 0, hz = 0;   // its home stretch of water
        float th = 0, wantTh = 0;
        float turnAt = 0;
        float phase = 0;
        float fleeUntil = -1.0f;   // see kDflyFleeHold
        float stuck = 0.0f;        // seconds of getting nowhere -- see kDflyStuckSec
        // -- THE LANDING. ph is 0 flying / 1 descending / 2 sitting / 3 climbing
        //    out; pad is the leaf it has claimed, or -1. pox/poz and pth are its
        //    place and heading IN THE PAD'S OWN FRAME, which is what it rides.
        int ph = 0;
        int pad = -1;
        float pox = 0.0f, poz = 0.0f, pth = 0.0f;
        float phAt = 0.0f, dropAt = 0.0f;
        int pose = 0, poseWas = 0; // for the wing motion vector
    };

    // =======================================================================
    // THE FISH.
    // =======================================================================
    // -----------------------------------------------------------------------
    // NO TWO FISH OCCUPY THE SAME WATER.
    //
    // A POSITIONAL PASS, AFTER EVERYTHING HAS MOVED, and that is the point of
    // it: steering is an intention and this is a fact. A fish mid-leap is not
    // steering, a fleeing fish is steering somewhere else, and a fish already
    // inside another one has nowhere useful to point -- none of those cases can
    // be fixed by a term in a desired velocity, and all of them are fixed by
    // moving the two apart.
    //
    // HALF THE OVERLAP EACH, which is what makes it symmetric: push one fish
    // the whole way and the pair drifts, because whichever was second in the
    // loop wins. Ten fish, so the n^2 is sixty compares.
    //
    // ...AND IT WILL NOT PUSH ANYTHING ONTO A BANK. The horizontal half is only
    // taken if the destination is water a fish fits in; the vertical half is
    // always safe, because the depth servo clamps it on the next step.
    // -----------------------------------------------------------------------
    void separateFish() {
        for (size_t i = 0; i < fish_.size(); ++i) {
            Fish &a = fish_[i];
            if (!a.live) continue;
            for (size_t j = i + 1; j < fish_.size(); ++j) {
                Fish &b = fish_[j];
                if (!b.live) continue;
                float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                float d2 = dx * dx + dy * dy + dz * dz;
                if (d2 >= kFishBodyM * kFishBodyM) continue;
                // EXACTLY COINCIDENT IS A REAL CASE, not a theoretical one: two
                // fish spawned into the same cell have no direction to be
                // pushed along, and a normalise there is a divide by zero. The
                // pair's own indices pick a direction, so it is stable rather
                // than random -- they part the same way every frame instead of
                // vibrating.
                if (d2 < 1e-6f) {
                    const float a2 = float((i * 7u + j * 13u) % 32u) * 0.19634954f;
                    dx = cosf(a2);
                    dz = sinf(a2);
                    dy = 0.0f;
                    d2 = 1.0f;
                }
                const float d = sqrtf(d2);
                const float push = (kFishBodyM - d) * 0.5f;
                const float ux = dx / d, uy = dy / d, uz = dz / d;
                auto shove = [&](Fish *f, float sgn) {
                    const float nx = f->x + ux * push * sgn;
                    const float nz = f->z + uz * push * sgn;
                    float t = 0.0f, bd = 0.0f;
                    if (field_.at(nx, nz, &t, &bd) && (t - bd) >= kFishShallowM) {
                        f->x = nx;
                        f->z = nz;
                    }
                    f->y += uy * push * sgn;
                };
                shove(&a, -1.0f);
                shove(&b, 1.0f);
            }
        }
    }

    // -----------------------------------------------------------------------
    // A FISH THAT IS NOT OVER WATER SWIMS TO SOME.
    //
    // It keeps its height and glides -- it does NOT fall, because there is no
    // terrain height in the field for a dry cell and dropping it blind would
    // put it inside a hill. The moment it is over water again the ordinary step
    // takes it, and the depth servo brings it down at its own rate, which reads
    // as a dive rather than as a teleport.
    //
    // AND IF THERE IS NO WATER WITHIN REACH IT IS RECYCLED. Thirty-two metres
    // of searching is well past anywhere a leap could have thrown it; beyond
    // that the fish is somewhere it has no business being and the honest thing
    // is to give the slot back rather than to fly it across the wood.
    // -----------------------------------------------------------------------
    void recoverFish(Fish *f, float dt) {
        f->jumpOn = false;
        float wx = 0.0f, wz = 0.0f;
        if (!field_.nearestWet(f->x, f->z, 16, &wx, &wz)) {
            f->live = false;
            return;
        }
        const float ex = wx - f->x, ez = wz - f->z;
        const float e = sqrtf(ex * ex + ez * ez);
        if (e > 1e-4f) f->th = atan2f(ex, ez);
        const float sp = clampf(e * 2.0f, kFishCruise, 6.0f);
        f->x += sinf(f->th) * sp * dt;
        f->z += cosf(f->th) * sp * dt;
        f->pitch += (0.0f - f->pitch) * (1.0f - expf(-4.0f * dt));
        f->animClk += dt * kFishAnimFps;
        f->vy = 0.0f;
    }

    // -----------------------------------------------------------------------
    // A PLACE IN THE SHOAL, AND HOW THIS FISH DRIFTS ABOUT IT.
    //
    // The disc is FLATTENED ALONG THE SWIM and pushed back, which is the shape
    // a travelling shoal actually has -- wider than it is deep would be a wall
    // of fish, and centred on the leader would put half of them in front of it.
    // sqrt on the radius is what makes the draw uniform over the AREA; without
    // it everything bunches at the middle, which is the clumping this whole
    // exercise is about.
    // -----------------------------------------------------------------------
    void rollSlot(Fish *f, uint32_t h) {
        const float a = hashUnit(0x5C1u, h) * 6.2831853f;
        const float r = sqrtf(hashUnit(0x5C2u, h));
        f->slotR = cosf(a) * r * kSchoolSpreadM;
        f->slotF = sinf(a) * r * kSchoolDeepM - kSchoolBackM;
        f->wanRA = kSchoolWanLo + hashUnit(0x5C3u, h) * (kSchoolWanHi - kSchoolWanLo);
        f->wanRB = kSchoolWanLo + hashUnit(0x5C4u, h) * (kSchoolWanHi - kSchoolWanLo);
        f->wanPA = hashUnit(0x5C5u, h) * 6.2831853f;
        f->wanPB = hashUnit(0x5C6u, h) * 6.2831853f;
        f->holdOff = (hashUnit(0x5C7u, h) - 0.5f) * 0.34f;
        f->shuffleAt = clock_ + kSchoolShuffleMin +
                       hashUnit(0x5C8u, h) * (kSchoolShuffleMax - kSchoolShuffleMin);
    }

    void stepFish(Fish *f, const Fish *lead, float dt, const Vec3 &player) {
        // -- AIRBORNE: PURE BALLISTICS, AND NOTHING ELSE RUNS ---------------
        //
        // No steering, no whiskers, no depth servo. v1 is explicit that the
        // arc is validated at launch and then flown: bending the heading in
        // the air "would curve the arc off its validated splash-down".
        if (f->jumpOn) {
            float topM = 0.0f, bedM = 0.0f;
            const bool over = field_.at(f->x, f->z, &topM, &bedM);
            // Off the water entirely: the arc is abandoned rather than flown
            // into a bank. It keeps whatever height it had and the servo below
            // takes it back on the next frame.
            if (!over) { f->jumpOn = false; return; }

            // -- UNDER THE SURFACE IT IS STILL SWIMMING ---------------------
            //
            // So it accelerates toward the launch speed instead of carrying it
            // from nowhere, and gravity does not touch it until the body is
            // actually out. Crossing the surface is then a change in what acts
            // on it, not a change in where it is.
            const bool out = f->y > topM - kJumpBreakM;
            if (out) f->jumpV -= kJumpGrav * dt;
            else f->jumpV += (f->jumpTop - f->jumpV) * (1.0f - expf(-kJumpRiseRate * dt));

            f->y += f->jumpV * dt;
            // The forward speed is the one it was swimming at, floored at the
            // arc's own: a leap that starts by changing pace reads as a cut.
            const float fwd = maxf(f->spd, kJumpFwd);
            f->x += sinf(f->th) * fwd * dt;
            f->z += cosf(f->th) * fwd * dt;

            // -- THE NOSE IS EASED ONTO THE ARC, NOT SET TO IT --------------
            //
            // Up on the way up and down on the way in, as before, but arrived
            // at over a tenth of a second. Assigned, it was a sixty-degree
            // snap on the launch frame -- the corner the report was about.
            const float pT = clampf(atan2f(f->jumpV, fwd), -1.2f, 1.2f);
            f->pitch += (pT - f->pitch) * (1.0f - expf(-kJumpPitchRate * dt));

            // Beating harder on the way up: the tail is what is doing this.
            f->animClk += dt * kFishAnimFps * (out ? 1.0f : 2.0f);
            f->vy = f->jumpV;

            // REENTRY. The pitch is NOT zeroed -- it is steeply down at this
            // moment and the depth servo below eases it back over the next
            // second, which is the fish levelling out under water. Zeroing it
            // here was the second snap in the same manoeuvre.
            if (f->jumpV < 0.0f && f->y <= topM - kJumpBreakM) {
                f->jumpOn = false;
                f->y = minf(f->y, topM - kJumpBreakM);
            }
            return;
        }

        // -- THE THREAT SCAN, AND IT IS A SPHERE ----------------------------
        //
        // The JS engine includes the vertical gap with the player's own offset,
        // so swimming above a fish spooks it and so does standing on the bank
        // over one. A ground circle would let you lean over a shallow and have
        // the fish ignore you.
        const float dx = f->x - player.x, dz = f->z - player.z;
        const float dy = f->y - (player.y - 1.0f);
        if (dx * dx + dy * dy + dz * dz < kFishThreatM * kFishThreatM) {
            f->fleeUntil = clock_ + kFishFleeHold;
            f->thrX = player.x;
            f->thrZ = player.z;
        }
        const bool fleeing = clock_ < f->fleeUntil;

        float wantTh = f->navTh;
        if (fleeing) {
            // -- THE ESCAPE FAN, RE-PLANNED AT 8 Hz -------------------------
            //
            // Away from the remembered threat, then bent onto whichever of five
            // offsets has the most open water -- "never bolt into a bank". The
            // penalty on |off| is the JS engine's: all else equal, straight
            // away from the threat wins.
            if (clock_ > f->fleeAt) {
                f->fleeAt = clock_ + 0.125f;
                const float away = atan2f(f->x - f->thrX, f->z - f->thrZ);
                float best = -1e9f, bestOff = 0.0f;
                static const float kOff[5] = {0.0f, 0.7f, -0.7f, 1.6f, -1.6f};
                for (float off : kOff) {
                    const float sc = field_.reach(f->x, f->z, away + off, 12.0f, kFishShallowM) -
                                     fabsf(off) * 0.6f;
                    if (sc > best) { best = sc; bestOff = off; }
                }
                f->navTh = away + bestOff;
            }
            wantTh = f->navTh;
        } else if (clock_ > f->senseAt) {
            // -- THE WHISKER FAN ------------------------------------------
            //
            // "sensors constantly scanning the water". A sweep around the
            // current intent; each whisker measures how far the body could
            // travel down it, and the freest one wins with a small bias toward
            // carrying straight on so a cruising fish does not weave.
            f->senseAt = clock_ + kFishSenseSec;
            float best = -1e9f, bestTh = f->th;
            for (int k = -3; k <= 3; ++k) {
                const float off = float(k) * 0.42f;
                const float sc = field_.reach(f->x, f->z, f->th + off, 10.0f, kFishShallowM) -
                                 fabsf(off) * 0.5f;
                if (sc > best) { best = sc; bestTh = f->th + off; }
            }
            f->navTh = bestTh;
            wantTh = bestTh;
        }

        // -- ...UNLESS IT IS HOLDING A STATION IN A SCHOOL ------------------
        //
        // AFTER the whiskers and BEFORE the turn, so a follower still pays for
        // the scan -- navTh stays current, and the moment the school breaks up
        // or the leader leaps it has a heading of its own to fall back on
        // rather than a stale one.
        //
        // NOT WHILE FLEEING. A school scattering is the correct response to a
        // person wading into it, and it is also v1's rule for every other
        // grouped behaviour in the engine.
        if (lead && !fleeing) {
            // -- IT TAKES ITS TURN AT THE FRONT -----------------------------
            //
            // A new place in the cloud, every half minute or so. Not a snap:
            // the base is a target the velocity controller below swims toward,
            // so a reshuffle reads as one fish working its way through the
            // shoal, which is the single most recognisable thing fish do in a
            // group.
            if (clock_ > f->shuffleAt)
                rollSlot(f, hashU32(uint32_t(clock_ * 71.0f), uint32_t(f->x * 41.0f)));

            const float c = cosf(lead->smTh), sn = sinf(lead->smTh);
            // The base, plus this fish's own drift about it. The two rates are
            // per fish and do not divide into each other, so nothing in the
            // shoal moves in step with anything else.
            const float wr = f->slotR + kSchoolWanderM * sinf(clock_ * f->wanRA + f->wanPA);
            const float wf = f->slotF + kSchoolWanderM * sinf(clock_ * f->wanRB + f->wanPB);
            // right = (cos, -sin), forward = (sin, cos). The station is the
            // leader plus that offset in that basis -- built on the SMOOTHED
            // heading, see the note where smTh is stepped.
            const float sx = lead->x + wr * c + wf * sn;
            const float sz = lead->z - wr * sn + wf * c;
            const float ex = sx - f->x, ez = sz - f->z;
            const float e = sqrtf(ex * ex + ez * ez);
            // THE VELOCITY IT WANTS: the leader's, plus a correction toward the
            // station. See the note over kSchoolCloseRate for why this and not
            // a heading at the station -- the short version is that the second
            // one overshoots and this one cannot.
            const float lv = maxf(lead->spd, kFishCruise);
            float vx = sn * lv + ex * kSchoolCloseRate;
            float vz = c * lv + ez * kSchoolCloseRate;

            // -- ...AND NOBODY SWIMS THROUGH ANYBODY ------------------------
            //
            // Random offsets WILL put two fish in the same place eventually,
            // and two salmon occupying one spot is the kind of fault that only
            // ever shows up in a screenshot. This is also where most of the
            // jostling comes from: the shoal is slightly over-packed, so it is
            // always gently sorting itself out.
            //
            // Against the WHOLE school including the leader -- a follower that
            // avoids its neighbours and swims through the fish it is following
            // would be a strange kind of polite.
            for (const Fish &o : fish_) {
                if (&o == f || !o.live) continue;
                if (&o != lead && !(o.lead >= 0 && o.lead == f->lead)) continue;
                const float dxs = f->x - o.x, dzs = f->z - o.z;
                const float d2s = dxs * dxs + dzs * dzs;
                if (d2s >= kSchoolSepM * kSchoolSepM || d2s < 1e-6f) continue;
                const float ds = sqrtf(d2s);
                const float push = kSchoolSepRate * (1.0f - ds / kSchoolSepM);
                vx += dxs / ds * push;
                vz += dzs / ds * push;
            }

            const float vm = sqrtf(vx * vx + vz * vz);
            wantTh = (vm > 1e-4f) ? atan2f(vx, vz) : lead->smTh;
            f->schoolSpd = minf(vm, lv + kSchoolCloseMax);
            // ...and the depth is the school's plus its own offset, so the
            // shoal has a thickness rather than being a sheet of fish.
            const float hT = clampf(lead->hold + f->holdOff, 0.08f, 0.92f);
            f->hold += (hT - f->hold) * (1.0f - expf(-kSchoolDepthRate * dt));
            f->holdAt = clock_ + 1.0f;   // and it does not re-roll its own
            f->schoolErr = e;
        } else {
            f->schoolErr = 0.0f;
        }

        // -- THE TURN, CAPPED, AND THE CAP IS DIFFERENT WHILE FLEEING -------
        //
        // Double speed needs sharper banking to keep clear of a bank, which is
        // why the JS engine carries two rates rather than one.
        const float cap = fleeing ? kFishFleeYaw : kFishYawRate;
        const float err = atan2f(sinf(wantTh - f->th), cosf(wantTh - f->th));
        const float omT = clampf(err * 5.0f, -cap, cap);
        f->om += (omT - f->om) * (1.0f - expf(-8.0f * dt));
        f->th += f->om * dt;
        // -- AND A SECOND, CALMER HEADING THAT ONLY THE SCHOOL READS --------
        //
        // The formation hangs off the leader's frame, so anything the leader's
        // heading does, the whole school does -- amplified by the offsets, so a
        // fish three ranks back on the outside of a turn travels several times
        // further than the leader. A flick of the whiskers becomes a crack of
        // the whip.
        //
        // This is the leader's heading with the flicks taken out: a 2.5/s ease,
        // which is slow enough that a bank correction does not reach the
        // formation and fast enough that a real turn does. The leader itself
        // still steers on the live one -- this is what the school SEES, not
        // what the leader does.
        {
            const float d = atan2f(sinf(f->th - f->smTh), cosf(f->th - f->smTh));
            f->smTh += d * (1.0f - expf(-2.5f * dt));
        }

        // -- SPEED: THE KICK IS FAST, THE BLEED-OFF SLOW --------------------
        //
        // The JS engine's asymmetric ease, and its comment is the design:
        // "dart, then coast". A symmetric one makes a startled fish look like
        // it is being towed.
        // A follower in formation swims at the speed the station controller
        // asked for; everything else swims at cruise. Fleeing overrides both,
        // because a scattering school is not a school.
        const float spdT = (lead && !fleeing) ? f->schoolSpd
                                              : kFishCruise * (fleeing ? kFishFleeMul : 1.0f);
        f->spd += (spdT - f->spd) * (1.0f - expf(-(spdT > f->spd ? 6.0f : 1.4f) * dt));

        // -- DEPTH WANDER ---------------------------------------------------
        if (clock_ > f->holdAt) {
            f->holdAt = clock_ + 2.0f + hashUnit(0x5A1u, hashU32(uint32_t(clock_ * 97.0f),
                                                                 uint32_t(f->x * 13.0f))) * 4.0f;
            f->hold = 0.25f + hashUnit(0x5A2u, hashU32(uint32_t(clock_ * 131.0f),
                                                      uint32_t(f->z * 17.0f))) * 0.6f;
        }

        // -- AND THE STEP, WHICH IS REFUSED IF IT LEAVES THE WATER ----------
        const float nx = f->x + sinf(f->th) * f->spd * dt;
        const float nz = f->z + cosf(f->th) * f->spd * dt;
        float topM = 0.0f, bedM = 0.0f;
        // A SHALLOW CELL COUNTS AS A BANK. Wet is not the test -- a fish needs
        // water it fits in, and the shelf at the edge of a lake is wet.
        auto swimmable = [&](float ax, float az, float *t, float *b) {
            return field_.at(ax, az, t, b) && (*t - *b) >= kFishShallowM;
        };
        if (swimmable(nx, nz, &topM, &bedM)) {
            f->x = nx;
            f->z = nz;
        } else {
            // -- A BANK IN ITS FACE: SLIDE ALONG IT, DO NOT SPIN ON IT ------
            //
            // The old answer was to add a fixed turn every refused frame, and
            // against the depth test above that turned out to be a spin: in a
            // pocket of shallow water EVERY heading is refused, so the fish
            // rotated at 6.6 rad/s on the spot. For a lone fish that reads as
            // agitation and is nearly harmless. For a SCHOOL it is fatal --
            // the stations are built in the leader's frame, so a spinning
            // leader whips the whole formation round itself, and the average
            // follower measured 7.59 m off station.
            //
            // So it tries either side first, at a quarter turn and then a half,
            // and only gives up and turns if both are blocked. A fish following
            // a shore is what this produces, which is also what a fish does.
            bool slid = false;
            for (int k = 0; k < 2 && !slid; ++k) {
                const float off = (k == 0) ? 0.8f : 1.6f;
                for (int sgn = -1; sgn <= 1 && !slid; sgn += 2) {
                    const float a = f->th + float(sgn) * off;
                    const float sxp = f->x + sinf(a) * f->spd * dt;
                    const float szp = f->z + cosf(a) * f->spd * dt;
                    if (!swimmable(sxp, szp, &topM, &bedM)) continue;
                    f->x = sxp;
                    f->z = szp;
                    f->th = a;
                    slid = true;
                }
            }
            if (!slid) f->th += 2.2f * dt * 3.0f;
            field_.at(f->x, f->z, &topM, &bedM);
        }

        // THE BODY STAYS UNDER THE SURFACE AND OFF THE BED. See kFishBedClearM
        // for why the clearance below is twice what it was.
        const float lo = bedM + kFishBedClearM, hi = topM - 0.14f;
        const float prevY = f->y;
        if (hi > lo) {
            f->y += (lo + (hi - lo) * f->hold - f->y) * (1.0f - expf(-1.8f * dt));
            // AND THEN IT IS CLAMPED, which is not the same thing as easing
            // toward a target inside the band. A fish arriving from a leap, or
            // from a cell whose bed was a metre lower, is OUTSIDE the band on
            // the frame it arrives -- and an ease only ever gets it most of the
            // way back, which over a shelving bed is a fish with its belly in
            // the sand for a second at a time.
            f->y = clampf(f->y, lo, hi);
        } else {
            // A column too thin to hold it. It cannot be IN this water, so it
            // sits on top of the bed rather than inside it and the bank turn
            // above takes it off. Never the midpoint: on a 20 cm column the
            // midpoint is 10 cm inside the sand.
            f->y = bedM + kFishBedClearM;
        }
        f->vy = (dt > 1e-5f) ? (f->y - prevY) / dt : 0.0f;

        // -- THE NOSE TIPS WITH THE CLIMB, AND ONLY A LITTLE ----------------
        const float pT = clampf(f->vy * kFishPitchGain, -kFishPitchMax, kFishPitchMax);
        f->pitch += (pT - f->pitch) * (1.0f - expf(-6.0f * dt));

        // -- THE TAIL BEAT IS LOCKED TO THE SWIM SPEED ----------------------
        //
        // "baseSpeed -> animFps, double speed -> EXACTLY 2x animFps. Render fps
        // never enters the equation." This is what makes the flee read as
        // effort rather than as the same fish moved faster.
        f->animClk += dt * (f->spd / kFishCruise) * kFishAnimFps;

        // -- ...AND EVERY SO OFTEN IT COMES OUT OF THE WATER ----------------
        //
        // NOT WHILE FLEEING. A fish breaking away from you is doing one thing
        // and a fish showing off is doing another; v1 keeps them apart for the
        // same reason, and a leap that fires mid-flee reads as the flee having
        // been a run-up.
        //
        // DEEP WATER ONLY -- v1: "never from a shelf it could land back onto".
        // The check is the depth HERE, so a fish that has wandered into the
        // shallows simply does not leap until it is back out.
        if (clock_ > f->jumpAt) {
            const bool deep = (topM - bedM) >= kJumpMinDepthM;
            // -- ...AND THE WHOLE ARC IS OVER WATER, OR IT IS NOT TAKEN ----
            //
            // Reported as "a fish tried to jump out of water bounds and got
            // stuck in the air". v1 has the rule and this file even quotes it
            // -- "a salmon's leap was validated at launch and must fly its arc
            // at full speed" -- but only the second half was implemented. The
            // arc was flown without ever checking where it came down.
            //
            // The range is the ballistic one: 2v/g seconds aloft times the
            // forward speed. Sampled at five points rather than only at the
            // splash-down, because a lake with an island in it can be wet at
            // both ends of a jump that lands on a rock in the middle.
            const float fwd0 = maxf(f->spd, kJumpFwd);
            const uint32_t h = hashU32(uint32_t(clock_ * 311.0f), uint32_t(f->x * 53.0f));
            const float v0 = kJumpVMin + hashUnit(0x7A1u, h) * (kJumpVMax - kJumpVMin);
            const float range = fwd0 * (2.0f * v0 / kJumpGrav);
            bool clear = true;
            for (int k = 1; k <= 5 && clear; ++k) {
                const float d = range * float(k) / 5.0f;
                float t2 = 0.0f, b2 = 0.0f;
                clear = field_.at(f->x + sinf(f->th) * d, f->z + cosf(f->th) * d, &t2, &b2) &&
                        (t2 - b2) >= kFishShallowM;
            }
            if (!fleeing && deep && clear) {
                f->jumpOn = true;
                // IT LEAVES FROM WHERE IT IS, at the speed it already had. The
                // launch speed is a TARGET now (jumpTop) and the run-up above
                // eases onto it -- nothing is assigned to y, which is the whole
                // of the smooth transition.
                f->jumpTop = v0;
                f->jumpV = f->vy;
            }
            // The cooldown is reset whether or not the attempt fired, or a fish
            // in the shallows would launch on the first frame it reached depth.
            f->jumpAt = clock_ + kJumpCoolMin +
                        hashUnit(0x7A2u, hashU32(uint32_t(clock_ * 131.0f),
                                                 uint32_t(f->z * 71.0f))) *
                            (kJumpCoolMax - kJumpCoolMin);
        }
    }

    // =======================================================================
    // THE DUCKS.
    // =======================================================================
    //
    // v1's mother: eight rays of edge avoidance, a gentle wander when nothing is
    // in range, and a wind-up guard so a duck in an inlet does not circle. See
    // the note over kDuckCount for the argument behind each.
    // How wide a body is, for the one thing it can bump into. See kDuckPadPushM.
    float duckRadius(const Duck &d) const { return d.mom < 0 ? duckR_ : babyR_; }

    // A duckling's eye voxels in MODEL space, metres from its centre, and the
    // tear positions queued this tick. See scanBabyEyes and cryTick.
    std::vector<Vec3> babyEyes_;
    std::vector<Vec3> tears_;

    // IS A LILY PAD IN THIS CIRCLE? Asked of every live leaf, which is
    // twenty-four distance tests -- the pads are a flat array and there is
    // nothing here worth a grid for.
    bool padAt(float x, float z, float r) const {
        for (const Pad &q : pads_) {
            if (!q.live) continue;
            const float dx = x - q.x, dz = z - q.z;
            const float rr = r + padRadius(q.model);
            if (dx * dx + dz * dz < rr * rr) return true;
        }
        return false;
    }

    // ...AND THE SAME QUESTION THE OTHER WAY ROUND, for the spawn. Neither of
    // these two may be PUT DOWN on the other -- the push would sort it out over
    // half a second, but half a second of a duck sitting in a leaf is exactly
    // the picture this is here to prevent, and a site nobody is standing on is
    // one cell away.
    bool duckAt(float x, float z, float r) const {
        for (const Duck &e : ducks_) {
            if (!e.live) continue;
            const float dx = x - e.x, dz = z - e.z;
            const float rr = r + duckRadius(e);
            if (dx * dx + dz * dz < rr * rr) return true;
        }
        return false;
    }

    void stepDuck(Duck *d, uint32_t i, float dt, const Vec3 &player) {
        // -- PROACTIVE EDGE AVOIDANCE ----------------------------------------
        //
        // The first DRY sample on each ray pushes back, weighted so a near bank
        // shoves harder than a far one. Summing eight of those gives a direction
        // into open water without any one of them having to be right.
        //
        // ...AND A LILY PAD IS A BANK AS FAR AS THIS FAN IS CONCERNED (user
        // 2026-09-14: "have ducks go around lillypads, not clip right through
        // them"). One clause, in the one place that already asks "can I go this
        // way" -- so a leaf turns her a metre and a half out, with the same
        // near-shoves-harder weighting, and the line behind her follows her
        // round it. Adding a second, separate avoider for pads would have given
        // the mother two opinions about her heading to average.
        //
        // THE PROBE CARRIES HER OWN WIDTH, so a gap between two leaves she does
        // not fit through is not read as a way out.
        float rx = 0.0f, rz = 0.0f;
        const float body = duckRadius(*d);
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) * 0.785398f;
            const float sa = sinf(a), ca = cosf(a);
            for (float q = kDuckSeeMin; q <= kDuckSeeMax; q += kDuckSeeStep) {
                const float px = d->x + sa * q, pz = d->z + ca * q;
                if (field_.at(px, pz) && !padAt(px, pz, body)) continue;
                const float w = (kDuckSeeMax + 0.2f) - q;
                rx -= sa * w;
                rz -= ca * w;
                break;
            }
        }

        if (rx * rx + rz * rz > 0.02f) {
            float dth = atan2f(rx, rz) - d->th;
            dth = atan2f(sinf(dth), cosf(dth));
            // ...AND IT UNWINDS RATHER THAN COMPLETING THE CIRCLE. v1's guard:
            // past four radians the same way, take the long way to the same
            // heading. Without it a duck in a narrow inlet turns for ever,
            // because every ray on one side stays blocked.
            if (fabsf(d->turnAcc) > kDuckWindUp &&
                ((dth > 0.0f) == (d->turnAcc > 0.0f)))
                dth -= (dth > 0.0f ? 1.0f : -1.0f) * 6.2831853f;
            d->omT = clampf(dth * kDuckAvoidGain, -kDuckYaw, kDuckYaw);
        } else if (clock_ > d->tRe) {
            d->tRe = clock_ + kDuckWanderMin +
                     hashUnit(0xD11u, hashU32(i, uint32_t(clock_ * 37.0f))) *
                         (kDuckWanderMax - kDuckWanderMin);
            d->omT = (hashUnit(0xD12u, hashU32(i * 7u, uint32_t(clock_ * 53.0f))) - 0.5f) * 1.0f;
        }

        stepDuckBody(d, dt, kDuckSpeed);
        // A PERSON IS NOT A THREAT TO A DUCK IN v1 and is not one here: it has
        // no flee state at all. What it has is the shore, which it is already
        // avoiding -- adding a flee would send it straight at one.
        (void)player;
    }

    // -----------------------------------------------------------------------
    // THE WEEPING -- v1's cry block, one tear at a time, alternating cheeks.
    //
    // (user 2026-09-15: "the babies should cry".)
    //
    // IT ONLY QUEUES POINTS. Particles belongs to the app and a lake has no
    // business reaching into it; this is the same shape as
    // Arrows::landedThisTick, and it also puts the spawn outside the tick --
    // which matters, because a tear is a band slot and the band is published
    // once a frame.
    //
    // THE COUNTER IS FREE-RUNNING, NOT WRAPPED TO THE EYE LIST, and that is the
    // whole of v1's own bug report on this line: baby.vox is one voxel wide at
    // the head, so it has exactly ONE black voxel, and `% length` pinned the
    // alternation to a single cheek for ever. The cheek comes from the parity
    // of the counter; the eye comes from the counter modulo however many eyes
    // the art actually has.
    void cryTick() {
        // ITS OWN LIFETIME. Cleared at the top of the one function that fills
        // it, so a frame on which the app forgets to drain cannot leave a tear
        // to be born twice -- and a frame on which the lake does not tick
        // cannot leave a stale one either.
        tears_.clear();
        for (Duck &d : ducks_) {
            if (!d.live || !d.orphan || d.cryTo < 0.0f) continue;
            if (clock_ > d.cryTo) {
                d.cryTo = -1.0f;   // wept out
                continue;
            }
            if (clock_ < d.cryNext || babyEyes_.empty()) continue;
            d.cryNext = clock_ + kCryGapSec;
            const Vec3 &e = babyEyes_[size_t(d.cryEye) % babyEyes_.size()];
            // ...AND IT WELLS OUT OF THE SIDE OF THE EYE, alternating cheeks.
            // Half a cell along the model's own X puts the droplet on the eye's
            // OUTER face, so it is visible the instant it is born instead of
            // spending its first frames buried inside the head -- v1's words.
            const float ex = e.x + ((d.cryEye & 1) ? 0.5f * VOXEL_M : -0.5f * VOXEL_M);
            ++d.cryEye;
            float m[9];
            yawMat(d.th, m);
            // The duckling's own model centre, which is what putDuck draws it
            // about: the translation there is (x - ox, y - oy + hy*0.45 + ride,
            // z - oz) and the centre is that plus m*(hx,hy,hz), so the ox/oy/oz
            // cancel and what is left is this.
            const float cyw = d.y + babyHY_ * 0.45f + kDuckRideM;
            tears_.push_back(Vec3(d.x + m[0] * ex + m[1] * e.y + m[2] * e.z,
                                  cyw + m[3] * ex + m[4] * e.y + m[5] * e.z,
                                  d.z + m[6] * ex + m[7] * e.y + m[8] * e.z));
        }
    }

    // A duckling holds a spot behind its LEADER: the mother for the first, the
    // sibling ahead for the rest. One rule, and it is what makes a LINE rather
    // than three ducklings converging on the same point.
    //
    // -- ...AND THE LINE CLOSES UP WHEN A LINK DIES --------------------------
    //
    // (user 2026-09-20: "if you shoot one of the baby ducks behind the mother
    //  duck, the rest of the baby ducks lose there tracking. make sure that
    //  even if one of the babies dies, they still follow the mother.")
    //
    // THIS USED TO BE ONE INDEX AND A `return`. A duckling heeled behind the
    // sibling immediately ahead, and if that sibling was shot the lookup found
    // a dead slot and the function gave up -- so the survivors were not merely
    // following the wrong body, they were not STEERED AT ALL: no heading, no
    // speed, no step. Shooting the first of three stranded the other two.
    //
    // So the leader is SEARCHED FOR rather than computed: back up the line to
    // the nearest sibling still alive, and the mother if none of them is. v1
    // says the same thing twice -- its duckling takes `mom5` the moment
    // `wbf[wk - 1]` is not init, and its ant lines (the same chain, reused)
    // describe the rule as "falling back up the chain when a link dies".
    //
    // A DEAD MOTHER IS NOT THIS CASE and never reaches here: her ducklings are
    // marked orphan and go down the adult's path, which is the branch above.
    void stepDuckling(Duck *d, uint32_t i, float dt) {
        size_t li = size_t(d->mom);
        for (int s = d->sib - 1; s >= 0; --s) {
            const size_t cand = size_t(babyIndex(d->mom, s));
            if (cand < ducks_.size() && ducks_[cand].live) {
                li = cand;
                break;
            }
        }
        if (li >= ducks_.size() || !ducks_[li].live) return;
        const Duck &lead = ducks_[li];
        const float tx = lead.x - sinf(lead.th) * kDuckHeelM;
        const float tz = lead.z - cosf(lead.th) * kDuckHeelM;
        const float ex = tx - d->x, ez = tz - d->z;
        const float e = sqrtf(ex * ex + ez * ez);

        // STRANDED: put it back in the line rather than letting it swim across
        // the lake to rejoin. v1 recycles at the same distance and for the same
        // reason -- a duckling crossing open water alone is not a duckling.
        const float mx = d->x - ducks_[size_t(d->mom)].x;
        const float mz = d->z - ducks_[size_t(d->mom)].z;
        if (mx * mx + mz * mz > kBabyLostM * kBabyLostM) {
            d->x = tx;
            d->z = tz;
            d->th = lead.th;
            d->om = 0.0f;
            return;
        }

        float dth = atan2f(ex, ez) - d->th;
        dth = atan2f(sinf(dth), cosf(dth));
        d->omT = clampf(dth * kDuckFollowGain, -kDuckYaw, kDuckYaw);
        // v1's three-step table, by distance to the heel spot: scrambling,
        // keeping up, or idling on the spot.
        const float spd = (e > kBabyFarM) ? kBabyFast : (e > kBabyNearM ? kBabyKeep : kBabyIdle);
        stepDuckBody(d, dt, spd);
    }

    // What both of them do once a heading and a speed are chosen.
    void stepDuckBody(Duck *d, float dt, float spd) {
        d->om += (d->omT - d->om) * (1.0f - expf(-6.0f * dt));
        d->th += d->om * dt;
        // The wind-up, leaked per second so it means "how far it has turned
        // lately" rather than "since it was born".
        d->turnAcc = d->turnAcc * expf(-0.48f * dt) + d->om * dt;

        // -- THE STEP, AND WHERE IT GOES IF IT CANNOT BE TAKEN -------------
        //
        // The avoidance above is a SOFT term and can be out-voted by the line;
        // this is the hard rule, and it keeps a duck off the bank AND off a
        // lily pad. A duckling has no fan at all -- one rule is what makes a
        // line rather than three ducklings converging on a point -- so for
        // three of every four bodies in a family this is the whole of it.
        //
        // IT SLIDES BEFORE IT GIVES UP, which is the fish's rule and was
        // learned there the hard way: a refused step that only adds a turn
        // becomes a SPIN once several headings are refused at once, and a
        // spinning leader whips its whole formation round itself. Trying the
        // same speed a little either side is what turns "stopped dead against a
        // leaf" into "went round it".
        const float off[5] = {0.0f, kDuckSlideA, -kDuckSlideA, kDuckSlideB, -kDuckSlideB};
        const float r = duckRadius(*d);
        float topM = 0.0f;
        bool moved = false;
        for (int k = 0; k < 5 && !moved; ++k) {
            const float th = d->th + off[k];
            const float nx = d->x + sinf(th) * spd * dt;
            const float nz = d->z + cosf(th) * spd * dt;
            if (!field_.at(nx, nz, &topM)) continue;
            if (padAt(nx, nz, r)) continue;
            d->x = nx;
            d->z = nz;
            moved = true;
        }
        if (!moved) {
            d->th += 2.5f * dt;
            field_.at(d->x, d->z, &topM);
        }
        // IT SITS ON THE WATER AND RIDES IT. kDuckSway is v1's: half the swell,
        // so it moves with the surface without pumping.
        //
        // ...AND ON THE SURFACE AT HER OWN COLUMN, for the reason the lily pad
        // does -- see WaterField::exactTop. She floats, so a two-metre sample
        // of a stepped surface sinks her by the size of the step.
        {
            float exact = topM;
            if (terrain_ && WaterField::exactTop(*terrain_, padMemo_, d->x, d->z, &exact))
                topM = exact;
        }
        const float want = topM + kDuckSway * kDuckBobM * sinf(clock_ * 1.1f + float(d->sib));
        d->y += (want - d->y) * (1.0f - expf(-4.0f * dt));
    }

    // -----------------------------------------------------------------------
    // ...AND AFTER EVERYTHING HAS MOVED, NOTHING IS INSIDE A LEAF.
    //
    // SEPARATION HAS TO BE POSITIONAL, NOT STEERED. The salmon taught this file
    // that once already -- a steering term is what makes a body LEAN away and is
    // most of what reads as life, but it cannot promise anything, because a duck
    // turns at a finite rate and a pad drifts along underneath it at its own
    // pace with no idea anyone is there.
    //
    // THE DUCK MOVES AND THE PAD DOES NOT, deliberately. Shoving the leaf would
    // look better for exactly one frame and would then have to answer for where
    // it put it: the whole of scene/shore.h exists to guarantee a pad is never
    // over sand, and a duck herding leaves onto a bank would be arguing with it.
    //
    // RATE-LIMITED, because resolving a big overlap in one frame is a teleport.
    // In the steady state the fan and the step test keep the overlap to about a
    // centimetre a frame and the cap never binds.
    // -----------------------------------------------------------------------
    void duckOffPads(float dt) {
        if (pads_.empty()) return;
        // EVERY LIVE PAIR IS MEASURED, not only the overlapping ones -- see
        // duckPads. The push below still only touches the ones that overlap.
        const float cap = kDuckPadPushM * dt;
        for (Duck &d : ducks_) {
            if (!d.live) continue;
            const float r = duckRadius(d);
            for (const Pad &q : pads_) {
                if (!q.live) continue;
                const float dx = d.x - q.x, dz = d.z - q.z;
                const float rr = r + padRadius(q.model);
                const float d2 = dx * dx + dz * dz;
                // -- AND THIS IS WHERE IT IS COUNTED ------------------------
                //
                // A clash is a body-ticks number, not an event: "how often is
                // some part of a duck inside some leaf" is the thing that was
                // reported and the thing that has to go to zero. The ENCOUNTERS
                // are counted beside it, because 0 clashes over a run where
                // nothing ever came near a pad says nothing at all -- it is the
                // ratio that means something. See the offline lake report.
                padClosest_ = minf(padClosest_, maxf(0.0f, sqrtf(d2) - rr));
                if (d2 < (rr + kDuckNearPadM) * (rr + kDuckNearPadM)) ++padNear_;
                if (d2 >= rr * rr) continue;
                const float l = sqrtf(d2);
                ++padClash_;
                padWorst_ = maxf(padWorst_, rr - l);
                // Dead centre has no direction to leave by; her own heading is
                // as good an answer as any and it does not divide by zero.
                const float ux = l > 1e-4f ? dx / l : sinf(d.th);
                const float uz = l > 1e-4f ? dz / l : cosf(d.th);
                const float push = minf(rr - l, cap);
                const float nx = d.x + ux * push, nz = d.z + uz * push;
                // ONTO WATER ONLY. Pushing a duck out of a leaf and into the
                // bank is not an improvement.
                if (!field_.at(nx, nz)) continue;
                d.x = nx;
                d.z = nz;
            }
        }
    }

    // =======================================================================
    // THE LILY PADS.
    // =======================================================================

    float padRadius(int model) const {
        const size_t mi = size_t(model);
        return mi < lilyR_.size() ? lilyR_[mi] : 0.3f;
    }

    void stepPad(Pad *p, float dt, const VoxelTerrain &terrain) {
        // The spin is FREE and has nothing to do with the drift -- the JS
        // engine keeps `th` and `mth` apart for exactly this, and a pad whose
        // nose follows its drift reads as a boat.
        p->th += p->spin * dt;

        if (clock_ > p->turnAt) {
            p->turnAt = clock_ + kLilyTurnMin +
                        hashUnit(0x71Du, hashU32(uint32_t(clock_ * 61.0f),
                                                 uint32_t(p->x * 29.0f))) *
                            (kLilyTurnMax - kLilyTurnMin);
            p->mth += (hashUnit(0x71Eu, hashU32(uint32_t(clock_ * 83.0f),
                                                uint32_t(p->z * 31.0f))) -
                       0.5f) * 1.2f;
        }
        // SHORE AHEAD: CURL AWAY. Five voxels of lookahead in the JS engine,
        // which is half a metre; ours looks a pad's width ahead instead,
        // because the field is metre-grained and half a metre is inside one
        // cell.
        // -- THE LEASH ON ITS SITE, WHICH OVERRIDES THE WANDER -------------
        //
        // A pad drifts at 0.11 m/s, which is 40 m an hour -- slow enough to
        // read as still and fast enough that, over a session, a pad ends up
        // nowhere near the cell that owns it. The slot is recycled on the SITE
        // now (see recycle), so an un-leashed pad would be taken away while it
        // was still in plain view, and a new one would appear at the site it
        // had abandoned.
        //
        // Same shape as the butterflies' leash: it does not stop the drift, it
        // turns it round when it has gone far enough.
        {
            const float lx = p->x - p->sx, lz = p->z - p->sz;
            if (lx * lx + lz * lz > kLilyLeashM * kLilyLeashM) {
                const float home = atan2f(-lx, -lz);
                const float e = atan2f(sinf(home - p->mth), cosf(home - p->mth));
                p->mth += clampf(e * 1.6f, -kLilyShoreTurn, kLilyShoreTurn) * dt;
            }
        }

        // THREE METRES OF LOOKAHEAD, not one and a half. The field's cell went
        // from one metre to two (see WaterField::kCellM), and a lookahead
        // shorter than a cell samples the cell the pad is already in -- which
        // is always wet, so the curl never fired and pads drifted onto banks.
        if (!field_.at(p->x + sinf(p->mth) * 3.0f, p->z + cosf(p->mth) * 3.0f))
            p->mth += kLilyShoreTurn * dt;

        const float wantX = p->x + sinf(p->mth) * kLilyDrift * dt;
        const float wantZ = p->z + cosf(p->mth) * kLilyDrift * dt;

        // -- THE BROAD PHASE: IS THERE A BANK ANYWHERE NEAR THIS LEAF -------
        //
        // Eight field samples at three metres. If every one of them is water
        // the nearest shore is further off than a pad travels in a frame (0.11
        // m/s, so under two millimetres), and the exact test below -- nine
        // terrain columns, twice -- is not worth running.
        bool nearBank = false;
        for (int k = 0; k < kShoreRimSamples && !nearBank; ++k) {
            const float a = float(k) * (6.2831853f / float(kShoreRimSamples));
            if (!field_.at(p->x + sinf(a) * kLilyNearBankM, p->z + cosf(a) * kLilyNearBankM))
                nearBank = true;
        }

        float topM = 0.0f;
        if (!nearBank) {
            if (field_.at(wantX, wantZ, &topM)) {
                p->x = wantX;
                p->z = wantZ;
                // IT FLOATS, so its underside sits ON the surface rather than
                // in it. The model's own half height is what keeps a thick pad
                // from being half drowned.
                p->y = topM;
            }
            return;
        }

        // -- THE NARROW PHASE: EXACTLY WHERE THE WATER ENDS -----------------
        //
        // Asked of the terrain, per column, over the leaf's whole footprint --
        // and asked by scene/shore.h, which is where it lives so that a test can
        // drive it without a device. A leaf is a Floater and nothing about this
        // is particular to a lily: anything that drifts on water and must not
        // end up in the sand wants exactly this.
        Floater fl;
        fl.x = p->x;
        fl.z = p->z;
        fl.mth = p->mth;
        fl.spin = p->spin;
        const ShoreHit hit = driftFloater(terrain, padMemo_, &fl, padRadius(p->model),
                                          kLilyDrift * dt, kLilyPushM * dt, kLilyBounceSpin,
                                          kLilySpinMax * 1.5f);
        // A LEAF THAT CANNOT GET BACK ON THE WATER IS NOT SOMETHING TO KEEP
        // PUSHING. See kLilyStuckSec.
        p->stuck = (hit == ShoreHit::Aground) ? p->stuck + dt : 0.0f;
        if (p->stuck > kLilyStuckSec) p->retire = true;
        p->x = fl.x;
        p->z = fl.z;
        p->mth = fl.mth;
        p->spin = fl.spin;
        // THE SURFACE COMES FROM THE TERRAIN, AT THE LEAF'S OWN COLUMN.
        // See WaterField::exactTop for why the field's two-metre cell is not
        // good enough for something that FLOATS. The field is still what says
        // whether there is water here at all -- a pad off the edge of it keeps
        // the height it had rather than dropping to the bed.
        if (field_.at(p->x, p->z, &topM)) {
            float exact = topM;
            if (WaterField::exactTop(terrain, padMemo_, p->x, p->z, &exact)) topM = exact;
            p->y = topM;
        }
    }

    // =======================================================================
    // THE DRAGONFLY -- the butterfly's wander with a water home.
    // =======================================================================
    void stepDfly(Dfly *d, float dt, const Vec3 &player) {
        // -- IT BREAKS WHEN YOU GET CLOSE, AND THE HOLD IS WHY IT DOES NOT
        //    FLICKER AT THE RIM. Same shape as the fish's, same constants as
        //    the butterflies'.
        const float px = d->x - player.x, py = d->y - player.y, pz = d->z - player.z;
        if (px * px + py * py + pz * pz < kDflyThreatM * kDflyThreatM)
            d->fleeUntil = clock_ + kDflyFleeHold;
        const bool fleeing = clock_ < d->fleeUntil;
        const float spd = kDflySpeed * (fleeing ? kDflyFleeMul : 1.0f);

        // -- THE LANDING SUSPENDS THE FLIGHT, AND RETURNS -------------------
        //
        // v1's rule and its reason: the altitude servo and the wander below are
        // what hold a flyer in the air, and there is no way to reach a surface
        // with them running. So everything from here to the end of the descent
        // is handled by stepPerch and nothing else gets a say.
        if (d->ph != 0) {
            stepPerch(d, dt, fleeing);
            return;
        }
        // ...and while it is flying, it decides whether to come down.
        if (clock_ > d->phAt) {
            if (fleeing) d->phAt = clock_ + kDflyRetryMin;
            else tryLand(d);
        }

        if (clock_ > d->turnAt) {
            d->turnAt = clock_ + 0.6f + hashUnit(0x3C1u, hashU32(uint32_t(clock_ * 53.0f),
                                                                uint32_t(d->x * 37.0f))) * 1.4f;
            d->wantTh = hashUnit(0x3C2u, hashU32(uint32_t(clock_ * 71.0f),
                                                uint32_t(d->z * 41.0f))) * 6.2831853f;
        }
        // THE LEASH IS A GOAL, NOT A WALL. Past its radius the home simply
        // becomes the thing it wants to fly at, so it turns back of its own
        // accord instead of hitting an invisible edge -- which is how the
        // butterflies' leash works and why they never bounce.
        const float hx = d->hx - d->x, hz = d->hz - d->z;
        if (hx * hx + hz * hz > kDflyLeashM * kDflyLeashM) d->wantTh = atan2f(hx, hz);

        const float err = atan2f(sinf(d->wantTh - d->th), cosf(d->wantTh - d->th));
        d->th += clampf(err * 3.0f, -3.0f, 3.0f) * dt;

        const float nx = d->x + sinf(d->th) * spd * dt;
        const float nz = d->z + cosf(d->th) * spd * dt;
        float topM = 0.0f;
        if (field_.at(nx, nz, &topM)) {
            d->x = nx;
            d->z = nz;
            d->stuck = 0.0f;
        } else {
            // -- OVER THE WATER, NOT OVER THE BANK, AND IT MUST NOT STOP -----
            //
            // A dragonfly that wanders onto dry land is a fly; the one thing
            // that makes it read as a dragonfly is that it works the surface.
            // But refusing the step and setting an intent is not enough on its
            // own, and that is what stuck them: if the home direction also left
            // the water it was refused every frame for ever, and a dragonfly
            // sitting exactly ON its home asked atan2(0, 0), which does not
            // even turn.
            //
            // So it TURNS HARD, every frame it is blocked -- the same thing the
            // fish does at a bank -- and after kDflyStuckSec of getting
            // nowhere it stops trusting its heading at all and takes a fresh
            // one from the hash. Between the two there is no way to sit still.
            d->stuck += dt;
            d->th += 4.0f * dt;
            if (hx * hx + hz * hz > 1e-4f) d->wantTh = atan2f(hx, hz);
            if (d->stuck > kDflyStuckSec) {
                d->stuck = 0.0f;
                d->wantTh = hashUnit(0x3C9u, hashU32(uint32_t(clock_ * 977.0f),
                                                     uint32_t(d->x * 13.0f))) * 6.2831853f;
                d->th = d->wantTh;
            }
        }
        if (field_.at(d->x, d->z, &topM)) {
            const float want = topM + kDflyLoM +
                               (kDflyHiM - kDflyLoM) *
                                   (0.5f + 0.5f * sinf(clock_ * 1.7f + d->phase));
            d->y += (want - d->y) * (1.0f - expf(-3.0f * dt));
        }
        d->phase += dt * 0.0f;   // the phase is fixed per insect; see fill()
    }

    // -----------------------------------------------------------------------
    // IS THERE A LEAF WORTH SETTLING ON?
    //
    // ONE DRAGONFLY TO A PAD. Two of them on the same leaf would occupy the
    // same few centimetres of it -- there is no room on a lily pad for a
    // spacing rule, so the rule is simply that the leaf is taken.
    //
    // IT COMMITS ONLY FROM NEARLY OVERHEAD. v1 proves its column clear before
    // committing and then comes straight down it; the analogue here is that the
    // pad has to be within reach ALREADY, so the descent is a drop rather than
    // an approach. A dragonfly that had to fly to its pad while descending is
    // the same bug v1 measured as "arrived high".
    // -----------------------------------------------------------------------
    void tryLand(Dfly *d) {
        int best = -1;
        float bestD2 = kDflyReachM * kDflyReachM;
        for (size_t i = 0; i < pads_.size(); ++i) {
            const Pad &p = pads_[i];
            if (!p.live || p.dying >= 0.0f) continue;   // not one that is leaving
            bool taken = false;
            for (const Dfly &o : flies_)
                if (o.live && o.pad == int(i)) { taken = true; break; }
            if (taken) continue;
            const float dx = p.x - d->x, dz = p.z - d->z;
            const float q = dx * dx + dz * dz;
            if (q >= bestD2) continue;
            bestD2 = q;
            best = int(i);
        }
        if (best < 0) {
            d->phAt = clock_ + kDflyRetryMin +
                      hashUnit(0x3D1u, hashU32(uint32_t(clock_ * 89.0f), uint32_t(d->x * 31.0f))) *
                          (kDflyRetryMax - kDflyRetryMin);
            return;
        }

        // PIN THE FRAME, NOT THE PLACE. Where it is standing relative to the
        // leaf RIGHT NOW, expressed in the leaf's own basis -- clamped onto the
        // leaf, so it sets down on the pad rather than beside it.
        const Pad &p = pads_[size_t(best)];
        const float c = cosf(p.th), sn = sinf(p.th);
        const float dx = d->x - p.x, dz = d->z - p.z;
        float ox = dx * c - dz * sn;
        float oz = dx * sn + dz * c;
        // Clamped onto THIS leaf, not onto a constant -- see kDflyOnPadFrac.
        const size_t mi = size_t(p.model);
        const float lim = kDflyOnPadFrac * minf(mi < lilyHX_.size() ? lilyHX_[mi] : 0.2f,
                                                mi < lilyHZ_.size() ? lilyHZ_[mi] : 0.2f);
        const float r = sqrtf(ox * ox + oz * oz);
        if (r > lim && r > 1e-5f) {
            ox *= lim / r;
            oz *= lim / r;
        }
        d->ph = 1;
        d->pad = best;
        d->pox = ox;
        d->poz = oz;
        d->pth = d->th - p.th;   // the heading it arrives on is the one it keeps
        d->dropAt = clock_ + kDflyDropMaxS;
    }

    // -----------------------------------------------------------------------
    // DESCENDING, SITTING, OR CLIMBING BACK OUT.
    //
    // THE LEAF IS THE FRAME OF REFERENCE for all three, which is the whole of
    // "glide with the lillypad": the offset pinned at commit is rotated by the
    // pad's CURRENT heading every frame, so the dragonfly is carried by the
    // pad's drift and turned by its spin without either being simulated here.
    // -----------------------------------------------------------------------
    void stepPerch(Dfly *d, float dt, bool fleeing) {
        // THE LEAF CAN GO AWAY UNDER IT -- recycled, or fading out at the edge
        // of the drop radius. That is not a special case, it is the same abort
        // as being startled.
        const bool lost = d->pad < 0 || size_t(d->pad) >= pads_.size() ||
                          !pads_[size_t(d->pad)].live;
        if (lost) {
            d->pad = -1;
            if (d->ph != 3) d->ph = 3;
        }

        if (d->ph != 3 && d->pad >= 0) {
            const Pad &p = pads_[size_t(d->pad)];
            const float c = cosf(p.th), sn = sinf(p.th);
            // The inverse of the pin in tryLand: out of the pad's basis and
            // back into the world.
            const float wx = p.x + d->pox * c + d->poz * sn;
            const float wz = p.z - d->pox * sn + d->poz * c;
            const float wy = p.y + kDflyPerchM;

            if (d->ph == 1) {
                // STRAIGHT DOWN ONTO IT. Horizontally it is already locked to
                // the leaf -- it does not fly TO the pad, it rides the pad and
                // loses height.
                d->x = wx;
                d->z = wz;
                d->y += (wy - d->y) * (1.0f - expf(-kDflyDropRate * dt));
                d->th = p.th + d->pth;
                if (fleeing || clock_ > d->dropAt) {
                    d->ph = 3;   // startled, or took too long -- v1's two aborts
                } else if (fabsf(d->y - wy) < 0.06f) {
                    d->y = wy;
                    d->ph = 2;
                    d->phAt = clock_ + kDflySitMin +
                              hashUnit(0x3D2u, hashU32(uint32_t(clock_ * 61.0f),
                                                       uint32_t(d->x * 43.0f))) *
                                  (kDflySitMax - kDflySitMin);
                }
                return;
            }

            // SITTING: dead still ON the leaf, in the leaf's frame. v1 holds
            // its landed ladybug perfectly rigid and kills the bob for exactly
            // this reason -- anything left moving reads as a hover.
            d->x = wx;
            d->z = wz;
            d->y = wy;
            d->th = p.th + d->pth;
            d->stuck = 0.0f;
            if (fleeing || clock_ > d->phAt) d->ph = 3;
            return;
        }

        // CLIMBING OUT, back into the band it cruises in. It is flying again
        // the moment it gets there, and the next landing is a fly-interval away.
        float topM = 0.0f;
        const float want = field_.at(d->x, d->z, &topM) ? topM + kDflyLoM : d->y + 1.0f;
        d->y += (want - d->y) * (1.0f - expf(-kDflyDropRate * dt));
        d->pad = -1;
        if (d->y >= want - 0.08f) {
            d->ph = 0;
            d->phAt = clock_ + kDflyFlyMin +
                      hashUnit(0x3D3u, hashU32(uint32_t(clock_ * 97.0f), uint32_t(d->z * 29.0f))) *
                          (kDflyFlyMax - kDflyFlyMin);
        }
    }

    // =======================================================================
    // RECYCLING, AND IT FOLLOWS THE BIRDS' HARD-WON RULE.
    //
    // A slot is given up only when it is well outside where it can be made out,
    // and it is NEVER given up before its replacement exists -- see
    // v2-birds-popped-and-flickered, which is the same mistake made once
    // already in this engine. fill() below only ever writes a slot it has a
    // real spot for.
    // =======================================================================
    // Uniform, so the 3x3 stays a rotation times a number -- the tracer
    // normalises it back for the normal. Exactly the butterflies' form.
    // -- IT SHRINKS OUT AND IT DOES NOT GROW IN -----------------------------
    //
    // "The lillypads are still just appearing in. They're doing this effect
    // where they start off small and grow in size until full sized has been
    // reached. Fix." (user 2026-09-14.)
    //
    // The grow-in was the butterflies' own treatment, copied here when the
    // complaint was about things VANISHING, and the two halves are not
    // symmetrical. A departure happens at a hundred and eighty metres, where
    // the whole animal is a pixel and the fade is genuinely invisible. An
    // arrival happens wherever the nearest free site is -- and eight tenths of a
    // second of something swelling from 8% of its size is MOTION, which is the
    // one thing the eye is built to catch. It made every birth more visible
    // than the pop it was hiding.
    //
    // So a creature arrives at its full size and leaves by shrinking, and what
    // stops the arrival being seen is the spawn rule instead: outside thirty
    // metres and outside the view cone, which is where the guarantee belongs.
    // See kBirthConeCos.
    static float fadeOf(float age, float dying) {
        (void)age;
        const float out = dying >= 0.0f ? saturate(1.0f - dying / kLakeFadeSec) : 1.0f;
        return kLakeFadeMin + (1.0f - kLakeFadeMin) * out;
    }

    // -- ...AND IT LEAVES OVER MOST OF A SECOND, NOT BETWEEN TWO FRAMES -----
    //
    // "give them the same treatment as the butterflies." A slot that is simply
    // switched off is a thing that VANISHES, however far away it is, and the
    // eye is very good at a discontinuity it did not cause. Shrinking it out
    // turns the same event into something that reads as distance.
    //
    // COMING BACK INSIDE CANCELS IT, for the reason the butterflies' does: at
    // the boundary a creature crosses the line repeatedly, and a departure that
    // could not be called off would make it flicker instead of vanish -- which
    // is worse.
    void recycle(const Vec3 &player, float dt) {
        const float d2 = kLakeDropM * kLakeDropM;
        const float pd2 = padReachM * padReachM;
        auto far2 = [&](float x, float z) {
            const float dx = x - player.x, dz = z - player.z;
            return dx * dx + dz * dz > d2;
        };
        // ...AND THE PADS GET THE GROUND'S OWN RADIUS -- see padReachM.
        auto padFar2 = [&](float x, float z) {
            const float dx = x - player.x, dz = z - player.z;
            return dx * dx + dz * dz > pd2;
        };
        auto age = [&](bool gone, float *a, float *dying, bool *live) {
            *a += dt;
            if (gone && *dying < 0.0f) *dying = 0.0f;
            if (!gone && *dying >= 0.0f) *dying = -1.0f;
            if (*dying >= 0.0f) {
                *dying += dt;
                if (*dying >= kLakeFadeSec) *live = false;
            }
        };
        for (Fish &f : fish_) if (f.live) age(far2(f.x, f.z), &f.age, &f.dying, &f.live);
        promoteSchools();
        // MEASURED FROM THE SITE, NOT FROM THE ANIMAL -- for pads and flies,
        // which have one. The butterflies' rule, and their note says why: "a
        // slot is given up when its HOME leaves range, never when the insect
        // does". A pad that has drifted five metres downwind is still the pad
        // belonging to that cell, and recycling it on its own position would
        // free a site that is plainly still in view. Fish have no site to
        // return to once they are swimming, so they are judged where they are.
        for (Pad &p : pads_)
            if (p.live) age(p.retire || padFar2(p.sx, p.sz), &p.age, &p.dying, &p.live);
        for (Dfly &d : flies_)
            if (d.live) age(d.retire || far2(d.hx, d.hz), &d.age, &d.dying, &d.live);
        // THE FAMILY IS JUDGED BY THE MOTHER. A duckling holds a spot behind
        // her, so its distance from the player is hers to within a metre --
        // and letting the two be recycled independently is how you get three
        // ducklings following nothing.
        for (size_t i = 0; i < ducks_.size(); ++i) {
            Duck &d = ducks_[i];
            if (!d.live) continue;
            // ...AND AN ORPHAN IS ITS OWN MOTHER FOR THIS PURPOSE. THIS is
            // where the brood used to vanish: a killed mother is Duck{}, which
            // sits at the world ORIGIN, and far2(0,0) is true everywhere a
            // player ever stands -- so the instant she died every one of her
            // ducklings was judged to be thirty metres away and aged out. They
            // did not wander off; they were retired by a test asked about a
            // corpse's coordinates.
            const Duck &m = (d.mom < 0 || d.orphan) ? d : ducks_[size_t(d.mom)];
            age(far2(m.x, m.z), &d.age, &d.dying, &d.live);
        }
    }

    // -----------------------------------------------------------------------
    // A SCHOOL WHOSE LEADER HAS GONE PICKS ANOTHER ONE.
    //
    // Without this the followers keep pointing at a dead slot, fall back to
    // their whiskers, and the school quietly dissolves into five fish that
    // happen to be near each other -- which is what it was there not to be.
    // Worse, fill() would then refill that slot as a BRAND NEW leader and the
    // old school would be orphaned for good.
    //
    // The lowest-numbered follower takes the lead and keeps the school's target
    // size; the rest re-rank behind it, so the V closes up rather than leaving
    // a hole where the front fish was.
    // -----------------------------------------------------------------------
    void promoteSchools() {
        for (size_t li = 0; li < fish_.size(); ++li) {
            if (fish_[li].live && fish_[li].lead == -1) continue;
            // Anyone still following this index has lost their leader -- either
            // it died, or it was refilled as a follower of someone else.
            int heir = -1;
            for (size_t i = 0; i < fish_.size(); ++i)
                if (fish_[i].live && fish_[i].lead == int(li)) { heir = int(i); break; }
            if (heir < 0) continue;
            const int want = fish_[li].want > 0 ? fish_[li].want : kSchoolMin;
            fish_[size_t(heir)].lead = -1;
            fish_[size_t(heir)].want = want;
            fish_[size_t(heir)].slotR = 0.0f;
            fish_[size_t(heir)].slotF = 0.0f;
            for (size_t i = 0; i < fish_.size(); ++i) {
                Fish &f = fish_[i];
                if (!f.live || f.lead != int(li)) continue;
                f.lead = heir;
                // NEW PLACES ALL ROUND. The old offsets were relative to a fish
                // that has gone, and the heir was one of the followers -- so
                // keeping them would leave a shoal arranged around a hole, with
                // one fish trying to occupy the spot it is now standing in.
                rollSlot(&f, hashU32(uint32_t(li) * 2654435761u, uint32_t(i)));
            }
        }
    }

    // -----------------------------------------------------------------------
    // EVERY CANDIDATE SITE OF ONE POPULATION THAT IS IN RANGE AND OVER WATER.
    //
    // Gathered ONCE per fill pass and popped from as slots are taken, which is
    // the butterflies' own arrangement and for their reason: two slots can then
    // never claim the same site, and the scan is paid once rather than per slot.
    // It is only gathered at all when something is actually waiting.
    //
    // NEAREST FIRST. Not because near sites are better -- they are all equally
    // real -- but because a fixed population should fill the water you are
    // looking at before the water behind the hill, and because a slot freed at
    // the far edge should come back at the far edge rather than jump to your
    // feet.
    // -----------------------------------------------------------------------
    struct Site {
        float x, z, top, bed, d2;
        int cx, cz;
    };

    // `birthMinM` is this population's own birth floor -- see BirthGate::mayAt.
    // Defaulted, so every caller that has not thought about it keeps the
    // engine-wide thirty metres.
    // `placeM` is how far out sites are looked for. Defaulted to kLakePlaceM,
    // which is what every population had when there was one number -- the LILY
    // PADS take the chunk ring instead, see padReachM.
    void gatherSites(const Vec3 &player, float cellM, uint32_t salt, float minDepthM,
                     float birthMinM = kBirthMinM, float birthFarM = kBirthFarM,
                     float placeM = kLakePlaceM, uint8_t woods = 0) {
        sites_.clear();
        const int r = int(placeM / cellM) + 1;
        const int c0x = int(floorf(player.x / cellM)), c0z = int(floorf(player.z / cellM));
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                Site st;
                st.cx = c0x + dx;
                st.cz = c0z + dz;
                siteOf(cellM, salt, st.cx, st.cz, &st.x, &st.z);
                const float ex = st.x - player.x, ez = st.z - player.z;
                st.d2 = ex * ex + ez * ez;
                if (st.d2 > placeM * placeM) continue;
                // ...AND NOT UNDER YOUR NOSE. See kBirthMinM: a site inside the
                // floor is a real site and stays a candidate -- it is simply
                // not one a creature may be BORN into while you are stood next
                // to it. Waived for a tick after a teleport, when there is no
                // previous frame to pop against.
                if (!birth_.mayAt(ex, ez, birthMinM, birthFarM)) continue;
                // -- NOT A CHERRY LAKE, 2026-09-19 ---------------------
                //
                // (user: "only the worm, pink bird, flamingos and pink
                //  butterflies should be in the cherry forest".)
                //
                // ONE GATE FOR THE WHOLE LAKE. Every population in this file
                // -- six fish, the ducks and their brood, the lily pads and the
                // dragonflies -- draws its home from this list, so refusing the
                // column here is refusing all of them, and there is no roster
                // to keep in step with the ask.
                //
                // The cherry band has water because its ground IS the oak's by
                // construction (see VoxelTerrain::woodWeights); it is the one
                // thing the fold gave it that the blossom's roster does not
                // want.
                // -- ...UNLESS THIS POPULATION IS THE BLOSSOM'S OWN --------
                //
                // (user 2026-09-21: "not seeing the pink betta fish still".)
                //
                // THIS LINE IS WHY. The note above is about keeping the lake's
                // ordinary roster out of a band whose roster was authored by
                // name -- and it refuses the COLUMN, so it refuses every
                // population that draws a home from this list. The betta was
                // given a kWoodCherry gate in the last batch and then had every
                // one of its candidate sites thrown away here before the gate
                // could ever see one.
                //
                // A population that BELONGS to the cherry is the one exception
                // the old rule never had to express, because until the betta
                // there was not one.
                if (terrain_ && !(woods & kWoodCherry) &&
                    terrain_->cherryMix(st.x) >= 0.5f)
                    continue;
                if (!field_.at(st.x, st.z, &st.top, &st.bed)) continue;
                if (st.top - st.bed < minDepthM) continue;
                sites_.push_back(st);
            }
        // -- IN HASH ORDER, NOT NEAREST FIRST ----------------------------
        //
        // (user 2026-09-21: "the fish and lillypads are not spawning in
        //  correctly ... they are only spawning in certain spots. along with
        //  the ducks. the spawning rate should be consistent over the entire
        //  body of water." And, separately, "life is clustering".)
        //
        // THIS SORTED BY DISTANCE AND freeSite TAKES THE FRONT, so every fish,
        // duck, pad and dragonfly was placed at the NEAREST free site it could
        // find. A fixed number of slots handed out nearest-first is a ring of
        // life around the player and an empty lake past it -- which is the
        // screenshot exactly: pads thick in the near water and not one in the
        // far half.
        //
        // THE LAND POPULATIONS ALREADY KNEW. Bunnies::fill takes its sites in
        // HASH order and says why in as many words: "two of an animal taken
        // nearest-first are two of it at your feet, and none of it anywhere
        // else". siteOrder is that hash -- a value per cell, stable, with no
        // reference to where the player is standing -- so the same lake fills
        // the same way from wherever it is looked at, and the density is the
        // lattice's rather than the viewer's.
        //
        // THE BIRTH FLOOR STILL APPLIES: mayAt above already refused anything
        // too close to be watched arriving, so ordering by hash cannot spawn
        // something in your face.
        std::sort(sites_.begin(), sites_.end(), [&](const Site &a, const Site &b) {
            return siteOrder(salt, a.cx, a.cz) < siteOrder(salt, b.cx, b.cz);
        });
    }

    // Is this cell already somebody's? Checked against the population that owns
    // the lattice, so a pad and a dragonfly may share a cell index without ever
    // colliding -- they are different grids with different salts.
    template <typename V, typename F>
    static bool claimed(const V &v, int cx, int cz, F owns) {
        for (const auto &e : v)
            if (e.live && owns(e) && e.cx == cx && e.cz == cz) return true;
        return false;
    }

    // How many fish are in the school led by `li`, counting the leader.
    int schoolSize(int li) const {
        int n = fish_[size_t(li)].live ? 1 : 0;
        for (const Fish &f : fish_)
            if (f.live && f.lead == li) ++n;
        return n;
    }

    // A live leader whose school is under strength, or -1. The FIRST one found,
    // so schools fill in order and a new one is only founded when every
    // existing school is complete.
    //
    // ...AND THEN A SECOND PASS UP TO kSchoolMax, WHICH IS THE REMAINDER RULE.
    //
    // Ten fish do not divide into schools of three to six without a remainder,
    // and an early build printed "6 + 3 + 1": the last fish founded a school of
    // its own and there was nothing left to fill it with. A school of one is not
    // a school. So a fish that cannot found a VIABLE school -- one with at least
    // kSchoolMin slots free to grow into -- joins the smallest school still
    // under kSchoolMax instead, and "6 + 3 + 1" becomes "6 + 4".
    //
    // `want > 0` IS WHAT MAKES A BASS A BASS. It is the only test in this file
    // that separates the two species, and it appears twice below because both
    // passes could otherwise hand a lone fish a school to lead.
    // PER SPECIES, AND THAT IS NOT A DETAIL. Two of the four fish school, and
    // a minnow that joined a salmon's school would be holding a station sized
    // for a fish three times its length -- and swimming at the salmon's cruise
    // to keep it. `lo`/`hi` bound the run of slots this species owns, so the
    // free-slot count below is its own and not the whole lake's.
    int shortSchool(int species, size_t lo, size_t hi) const {
        for (size_t i = lo; i < hi; ++i) {
            const Fish &f = fish_[i];
            if (!f.live || f.lead != -1 || f.want <= 0 || f.species != species) continue;
            if (schoolSize(int(i)) < f.want) return int(i);
        }
        int free = 0;
        // A killed fish's slot is not room -- see KillHold.
        for (size_t i = lo; i < hi; ++i) free += (fish_[i].live || fishHold_.held(int(i))) ? 0 : 1;
        if (free >= kSchoolMin) return -1;   // there is room to found a real one

        int best = -1, bestN = kSchoolMax;
        for (size_t i = lo; i < hi; ++i) {
            const Fish &f = fish_[i];
            if (!f.live || f.lead != -1 || f.want <= 0 || f.species != species) continue;
            const int n = schoolSize(int(i));
            if (n >= bestN) continue;
            bestN = n;
            best = int(i);
        }
        return best;
    }

    // The spacing test for a SCHOOLMATE. Inside a school, five metres apart is
    // not a school -- see kSchoolApartM.
    bool crowdedFish(float x, float z, float r) const {
        for (const Fish &f : fish_) {
            if (!f.live) continue;
            const float dx = x - f.x, dz = z - f.z;
            if (dx * dx + dz * dz < r * r) return true;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // ONE SPECIES' WORTH OF SLOTS.
    //
    // `schools` is the whole difference between a salmon and a bass. When it is
    // true a new fish JOINS BEFORE IT FOUNDS -- which is what makes three to six
    // actually happen, since found-first would give ten schools of one and then
    // never fill any of them, because a slot only ever comes free one at a time.
    // Only the FOUNDER takes a lattice site; the rest of a school is placed
    // relative to its leader, because a school is one thing.
    // -----------------------------------------------------------------------
    // `woods` is a kWood* mask, or 0 for "any water" -- which is what every
    // fish but the betta is. See LakeLife::wood.
    void fillFish(const Vec3 &player, size_t lo, size_t hi, int species,
                  const std::vector<FishFrame> &strip, float cellM, uint32_t salt, bool schools,
                  uint8_t woods = 0) {
        if (strip.empty()) return;
        // A BAND THIS WORLD DOES NOT HAVE IS NOT AN EMPTY POPULATION, it is a
        // sweep of the site lattice every frame for nothing. Asked once here
        // rather than per candidate.
        if (woods && !wood) return;
        bool gathered = false;
        for (size_t i = lo; i < hi; ++i) {
            Fish &f = fish_[i];
            if (f.live || fishHold_.held(int(i))) continue;   // see KillHold
            const uint32_t fh = hashU32(salt ^ uint32_t(i), uint32_t(clock_ * 7.0f));

            if (schools) {
                const int li = shortSchool(species, lo, hi);
                if (li >= 0) {
                    const Fish &L = fish_[size_t(li)];
                    // NOT INTO WATER YOU HAVE JUST FISHED -- see KillHold. The
                    // want is cut at the kill as well; this is for a school that
                    // was short already.
                    if (fishHold_.within(L.x, L.z, kKillQuietM, int(lo), int(hi))) continue;
                    Fish n{};
                    rollSlot(&n, fh);
                    const float c = cosf(L.smTh), sn = sinf(L.smTh);
                    const float x = L.x + n.slotR * c + n.slotF * sn;
                    const float z = L.z - n.slotR * sn + n.slotF * c;
                    float topM = 0, bedM = 0;
                    if (!field_.at(x, z, &topM, &bedM)) continue;   // station on a bank
                    if (topM - bedM < kFishShallowM) continue;
                    if (crowdedFish(x, z, kSchoolApartM)) continue;
                    // ...AND STILL IN ITS OWN WOOD. A school stations off its
                    // LEADER, so without this a betta whose leader swam to the
                    // seam would post its followers over the next band.
                    if (woods && !(wood(x) & woods)) continue;
                    f = n;
                    f.live = true;
                    f.species = species;
                    f.lead = li;
                    f.x = x;
                    f.z = z;
                    f.y = clampf(L.y, bedM + kFishBedClearM, topM - 0.14f);
                    f.th = L.smTh;
                    f.navTh = L.smTh;
                    f.smTh = L.smTh;
                    f.hold = clampf(L.hold + f.holdOff, 0.08f, 0.92f);
                    f.animClk = hashUnit(0x12u, fh) * float(strip.size());
                    continue;
                }
            }

            if (!gathered) {
                gatherSites(player, cellM, salt, kFishShallowM, kFishBirthMinM,
                            kBirthFarM, kLakePlaceM, woods);
                gathered = true;
            }
            const Site *st = freeSite([&](int cx, int cz) {
                return claimed(fish_, cx, cz, [](const Fish &e) { return e.owns; });
            }, woods, &fishHold_, int(lo), int(hi));
            if (!st) break;
            bornFish(&f, *st, species, fh);
            f.lead = -1;
            // want > 0 IS WHAT MAKES A FISH A SCHOOLING FISH. It is the only
            // test in this file that separates the four -- see shortSchool.
            if (!schools) {
                f.want = 0;
            } else {
                f.want =
                    kSchoolMin + int(hashUnit(0x13u, fh) * float(kSchoolMax - kSchoolMin + 1));
                if (f.want > kSchoolMax) f.want = kSchoolMax;
            }
        }
    }

    void fill(const VoxelTerrain &terrain, const Vec3 &player) {
        if (!field_.any()) return;

        // ---- the four fish, two of which school -------------------------
        //
        // ONE FUNCTION AND FOUR CALLS. The salmon and the minnows school; the
        // bass and the koi do not; and that is the ONLY thing that differs
        // between them in this file -- see fillFish, where it is one argument.
        // Writing it four times would be four places for a fifth fish to be
        // half-added.
        size_t at = 0;
        fillFish(player, at, at + kSalmonCount, 0, salmon_, kSchoolCellM, kSchoolSalt, true);
        at += kSalmonCount;
        fillFish(player, at, at + kBassCount, 1, bass_, kBassCellM, kBassSalt, false);
        at += kBassCount;
        fillFish(player, at, at + kKoiCount, 2, koi_, kKoiCellM, kKoiSalt, false);
        at += kKoiCount;
        fillFish(player, at, at + kMinnowCount, 3, minnow_, kMinnowCellM, kMinnowSalt, true);
        at += kMinnowCount;
        fillFish(player, at, at + kCatfishCount, 4, catfish_, kCatfishCellM, kCatfishSalt,
                 false);
        at += kCatfishCount;
        fillFish(player, at, at + kBluegillCount, 5, bluegill_, kBluegillCellM, kBluegillSalt,
                 false);
        at += kBluegillCount;
        // ...AND THE CHERRY WOOD'S OWN, which schools like the salmon and is
        // the only fish here that belongs to a band -- see kBettaCount.
        fillFish(player, at, at + kBettaCount, 6, betta_, kBettaCellM, kBettaSalt, true,
                 kWoodCherry);

        // ---- the ducks, a family at a time --------------------------------
        //
        // THE WHOLE FAMILY OR NONE OF IT. A mother without ducklings is a
        // different animal, and ducklings without a mother have no line to hold
        // -- so the three are written in the same pass that writes her, and the
        // site is only taken if all four can be placed.
        {
            bool gatheredDucks = false;
            for (size_t i = 0; i < size_t(kDuckCount); ++i) {
                Duck &m = ducks_[i];
                if (m.live || duckM_.empty() || duckHold_.held(int(i))) continue;   // see KillHold
                if (!gatheredDucks) {
                    // DEEPER WATER THAN A LILY PAD WANTS. A duck family needs a
                    // few metres of room to swim a line in, and the shallow rim
                    // of a pond is where the edge avoidance would spend all its
                    // time fighting.
                    gatherSites(player, kDuckCellM, kDuckSalt, 0.5f);
                    gatheredDucks = true;
                }
                // ...AND NOT ON TOP OF A LEAF. Her brood is laid out behind
                // her and may still land on one; that is what duckOffPads is
                // for, and a duckling easing out of a leaf over half a second
                // as it is born is invisible in a way a mother sitting in one
                // is not.
                const Site *st = nullptr;
                while ((st = freeSite([&](int cx, int cz) {
                            return claimed(ducks_, cx, cz,
                                           [](const Duck &e) { return e.mom < 0; });
                        }, 0, &duckHold_)) != nullptr)
                    if (!padAt(st->x, st->z, duckR_)) break;
                if (!st) break;
                const uint32_t h = hashU32(kDuckSalt ^ uint32_t(i), uint32_t(clock_ * 7.0f));
                m = Duck{};
                m.live = true;
                m.mom = -1;
                m.cx = st->cx;
                m.cz = st->cz;
                m.x = st->x;
                m.z = st->z;
                m.y = st->top;
                m.th = hashUnit(0xD21u, h) * 6.2831853f;
                // ...and her brood, in line order right behind her. v1:
                // "ducklings hatch right behind their mother, in line order".
                for (int b = 0; b < kBabyPerDuck; ++b) {
                    Duck &k = ducks_[size_t(kDuckCount) + i * kBabyPerDuck + size_t(b)];
                    // AN ORPHAN KEEPS ITS SLOT. The brood layout is fixed --
                    // three slots per mother index -- so a fresh mother placed
                    // in a slot whose last occupant was killed would stamp
                    // straight over the ducklings still paddling the lake from
                    // that death, and they would vanish a second time by a
                    // completely different route. It hatches one fewer instead.
                    if (k.live && k.orphan) continue;
                    // ...AND A DUCKLING THAT WAS KILLED STAYS KILLED, the same
                    // way: one fewer until you have left. See KillHold.
                    if (duckHold_.held(int(kDuckCount + i * kBabyPerDuck + size_t(b)))) continue;
                    k = Duck{};
                    k.live = true;
                    k.mom = int(i);
                    k.sib = b;
                    k.th = m.th;
                    k.x = m.x - sinf(m.th) * kDuckHeelM * float(b + 1);
                    k.z = m.z - cosf(m.th) * kDuckHeelM * float(b + 1);
                    k.y = m.y;
                }
            }
        }

        // ---- the lily pads -----------------------------------------------
        bool gatheredPads = false;
        for (size_t i = 0; i < pads_.size(); ++i) {
            Pad &p = pads_[i];
            if (p.live || lily_.empty()) continue;
            if (!gatheredPads) {
                gatherSites(player, kPadCellM, kPadSalt, 0.15f, kPadBirthMinM, kPadBirthFarM,
                            padReachM);
                gatheredPads = true;
            }
            // -- AND THE LEAF HAS TO FIT, WHICH IS NOT WHAT THE SITE SAYS ---
            //
            // A site is accepted on the field's two-metre cell, so a site can
            // be wet and still be a place where half a lily pad is over sand --
            // the same gap stepPad's narrow phase exists for. A pad put down
            // there would spend its life being pushed back out, so it is not
            // put down there: the next site along is tried instead.
            //
            // THE MODEL IS DRAWN FIRST, because the radius that has to fit is
            // that model's. It is a hash of the CELL, so drawing it here gives
            // the same leaf the claim below does.
            const Site *st = nullptr;
            uint32_t h = 0;
            int model = 0;
            while ((st = freeSite([&](int cx, int cz) {
                        return claimed(pads_, cx, cz, [](const Pad &) { return true; });
                    })) != nullptr) {
                h = hashU32(kPadSalt ^ uint32_t(st->cx), uint32_t(st->cz));
                model = int(hashUnit(0x21u, h) * float(lily_.size())) % int(lily_.size());
                if (discOnWater(terrain, padMemo_, st->x, st->z, padRadius(model), nullptr,
                                nullptr) &&
                    !duckAt(st->x, st->z, padRadius(model)))
                    break;
            }
            if (!st) break;
            p = Pad{};
            p.live = true;
            p.cx = st->cx;
            p.cz = st->cz;
            p.sx = st->x;
            p.sz = st->z;
            p.x = st->x;
            p.z = st->z;
            p.y = st->top;
            p.model = model;
            p.th = hashUnit(0x22u, h) * 6.2831853f;
            // Its own rate AND its own direction -- two pads turning together
            // reads as a mechanism.
            p.spin = (hashUnit(0x23u, h) - 0.5f) * 2.0f * kLilySpinMax;
            p.mth = hashUnit(0x24u, h) * 6.2831853f;
        }

        // ---- ...and every few seconds, one slot moves to better water -----
        //
        // AFTER the pads and BEFORE the flies only because it has to be
        // somewhere; each population is judged against its own lattice. It runs
        // on a clock rather than per frame because gatherSites is a 39 x 39
        // sweep and nothing here changes in a tenth of a second.
        if (clock_ >= yieldAt_) {
            yieldAt_ = clock_ + kYieldEverySec;
            if (!lily_.empty()) yieldSite(pads_, player, kPadCellM, kPadSalt,
                                          [](const Pad &) { return true; }, kPadBirthMinM,
                                          kPadBirthFarM);
            if (!dfly_.empty()) yieldSite(flies_, player, kDflyCellM, kDflySalt,
                                          [](const Dfly &) { return true; }, kBirthMinM,
                                          kBirthFarM, &dflyHold_);
        }

        // ---- and the dragonflies ------------------------------------------
        bool gatheredFlies = false;
        for (size_t i = 0; i < flies_.size(); ++i) {
            Dfly &d = flies_[i];
            if (d.live || dfly_.empty() || dflyHold_.held(int(i))) continue;   // see KillHold
            if (!gatheredFlies) {
                gatherSites(player, kDflyCellM, kDflySalt, 0.15f);
                gatheredFlies = true;
            }
            const Site *st = freeSite([&](int cx, int cz) {
                return claimed(flies_, cx, cz, [](const Dfly &) { return true; });
            }, 0, &dflyHold_);
            if (!st) break;
            const uint32_t h = hashU32(kDflySalt ^ uint32_t(st->cx), uint32_t(st->cz));
            d = Dfly{};
            d.live = true;
            d.cx = st->cx;
            d.cz = st->cz;
            d.hx = st->x;
            d.hz = st->z;
            d.x = st->x;
            d.z = st->z;
            d.y = st->top + kDflyLoM;
            d.th = hashUnit(0x31u, h) * 6.2831853f;
            d.wantTh = d.th;
            d.phase = hashUnit(0x32u, h) * 6.2831853f;
        }
    }

    // -----------------------------------------------------------------------
    // ...AND ONE SLOT MAY MOVE TO BETTER WATER. See kYieldMarginM.
    //
    // The floor alone is only half the fix. A population is a fixed number of
    // slots over an unbounded lattice: walk from one pond to another and the
    // first pond's pads hold every slot until they pass the drop radius, so the
    // pond you are standing in is EMPTY -- and then, the moment those pads
    // finally die, the freed slots take the nearest free sites, which are the
    // ones at your feet. That is the whole mechanism behind "I saw lillypads
    // just appear in front of me", and the floor would merely have moved the
    // arrival from twelve metres to thirty.
    //
    // So a holder that is much further away than an unclaimed site gives up:
    // it starts the ordinary fade, at a distance where that fade is what it was
    // measured for, and its slot comes back for the near site a second later.
    //
    // ONE PER PASS, on a timer rather than per frame. A lake that is suddenly
    // better than the one behind you should refill over a few seconds, the way
    // it would have if you had walked to it -- not swap its whole population
    // between two frames.
    //
    // MEASURED FROM THE SITE for a pad and a dragonfly, which is the same point
    // recycle() judges them at, or the two rules would disagree about which one
    // is furthest.
    // -----------------------------------------------------------------------
    template <typename V, typename OwnsF>
    void yieldSite(V &v, const Vec3 &player, float cellM, uint32_t salt, OwnsF owns,
                   float birthMinM = kBirthMinM, float birthFarM = kBirthFarM,
                   const KillHold *kills = nullptr) {
        for (int n = 0; n < kYieldPerPass; ++n)
            if (!yieldOne(v, player, cellM, salt, owns, birthMinM, birthFarM, kills)) return;
    }

    // One retirement, or false when there is nothing worth retiring for. The
    // caller runs it a few times a pass -- see kYieldPerPass.
    template <typename V, typename OwnsF>
    bool yieldOne(V &v, const Vec3 &player, float cellM, uint32_t salt, OwnsF owns,
                  float birthMinM = kBirthMinM, float birthFarM = kBirthFarM,
                  const KillHold *kills = nullptr) {
        // THE SAME FLOOR THE FILL USES, or this rule hunts for sites the fill
        // is not allowed to take and retires a pad for nothing.
        // THE SAME REACH THE FILL USED, or a pad yields a site it can never be
        // re-placed on -- see padReachM. The pads are the only caller with a
        // reach of their own, so the test is on the salt.
        gatherSites(player, cellM, salt, 0.15f, birthMinM, birthFarM,
                    salt == kPadSalt ? padReachM : kLakePlaceM);
        const Site *want = nullptr;
        for (const Site &st : sites_) {
            if (claimed(v, st.cx, st.cz, owns)) continue;   // sorted, nearest first
            // WATER YOU EMPTIED IS NOT "WATER WITH NOTHING IN IT". Without this
            // the rule below read a pond you had just shot clear as exactly the
            // lonely water it exists to fill, and sent it a replacement from
            // the far edge. See KillHold.
            if (kills && kills->within(st.x, st.z, kKillQuietM)) continue;
            // -- AND IT HAS TO BE WATER WITH NOTHING IN IT ------------------
            //
            // WITHOUT THIS THE RULE NEVER STOPS FIRING. The lattice is nine
            // metres, so on any lake there is ALWAYS an unclaimed site a few
            // metres from a claimed one -- and every one of them is nearer than
            // the far half of the population, so a slot was being retired and
            // re-placed every two seconds for ever. That is a pad quietly
            // appearing somewhere twice a minute with nothing wrong, which is
            // the other half of what "the lillypads are still just appearing
            // in" was.
            //
            // What the rule is FOR is water that has nothing in it at all --
            // the pond you walked to while the pond behind you held every slot.
            // So the site has to be LONELY before it is worth a slot, and a
            // lake that already has pads near that spot is left alone.
            bool lonely = true;
            for (const auto &e : v) {
                if (!e.live) continue;
                const float dx = siteXOf(e) - st.x, dz = siteZOf(e) - st.z;
                if (dx * dx + dz * dz < kYieldLonelyM * kYieldLonelyM) { lonely = false; break; }
            }
            if (!lonely) continue;
            want = &st;
            break;
        }
        if (!want) return false;
        const float ds = sqrtf(want->d2);

        int worst = -1;
        float worstD = 0.0f;
        for (size_t i = 0; i < v.size(); ++i) {
            if (!v[i].live || v[i].retire) continue;
            const float dx = siteXOf(v[i]) - player.x, dz = siteZOf(v[i]) - player.z;
            const float d = sqrtf(dx * dx + dz * dz);
            if (d > worstD) { worstD = d; worst = int(i); }
        }
        if (worst < 0 || worstD < ds + kYieldMarginM) return false;
        v[size_t(worst)].retire = true;
        return true;
    }

    // Where a pad and a dragonfly are JUDGED from -- their site, not where they
    // have drifted or flown to. One overload each so yieldSite stays one
    // function; recycle() makes the same distinction by hand.
    static float siteXOf(const Pad &e) { return e.sx; }
    static float siteZOf(const Pad &e) { return e.sz; }
    static float siteXOf(const Dfly &e) { return e.hx; }
    static float siteZOf(const Dfly &e) { return e.hz; }

    // The nearest site nobody has taken, or null. POPPED: taking one removes it
    // from the list, so the next slot in the same pass cannot pick it again
    // before the claim it made is visible.
    template <typename TakenF>
    // `woods` is a kWood* mask, or 0 for anywhere -- see LakeLife::wood. The
    // test is on the SITE, which is the same rule every banded population in
    // this engine uses.
    // `kills` and [lo, hi) are the population's KillHold and the slots of the
    // species being placed -- see KillHold in core/noise.h. Null for the pads,
    // which nothing kills.
    const Site *freeSite(TakenF taken, uint8_t woods = 0, const KillHold *kills = nullptr,
                         int lo = 0, int hi = INT_MAX) {
        while (!sites_.empty()) {
            const Site &st = sites_.front();
            if (taken(st.cx, st.cz) || (woods && wood && !(wood(st.x) & woods)) ||
                (kills && kills->within(st.x, st.z, kKillQuietM, lo, hi))) {
                sites_.erase(sites_.begin());
                continue;
            }
            held_ = st;
            sites_.erase(sites_.begin());
            return &held_;
        }
        return nullptr;
    }

    // Everything a new fish has in common whether it is a salmon or a bass.
    void bornFish(Fish *f, const Site &st, int species, uint32_t h) {
        *f = Fish{};
        f->live = true;
        f->species = species;
        f->owns = true;
        f->cx = st.cx;
        f->cz = st.cz;
        f->x = st.x;
        f->z = st.z;
        f->y = (st.top + st.bed) * 0.5f;
        f->th = hashUnit(0x11u, h) * 6.2831853f;
        f->navTh = f->th;
        f->smTh = f->th;
        f->hold = 0.4f;
        f->animClk = hashUnit(0x12u, h) * float(kSalmonFrames);
        // The strip length differs per species -- putFish takes the modulus, so
        // a koi with one frame is fine either way; this only spreads the phase.
    }

    // =======================================================================
    // THE INSTANCES.
    //
    // Every one of these is a turn about Y and a translation, which is the form
    // World::place is written against -- see the note there about why a
    // quarter-turn-only scene needs no adjugate. The fish is the one exception
    // and it earns it: a salmon climbing needs a nose, so it carries a PITCH as
    // well, and the product of two rotations is still orthonormal.
    // =======================================================================
    // -----------------------------------------------------------------------
    // WHICH WAY IS FORWARD, AND THE ANSWER IS -Z.
    //
    // `m` is read by rows (see World::place), so its COLUMNS are the images of
    // the model's own axes -- column 2 is where the model's local +z goes.
    // This maps local +z to the heading, which is right only if the model's
    // NOSE is at +z.
    //
    // IT IS NOT, AND BOTH STRIPS SWAM BACKWARDS BECAUSE OF IT. vox.h::toWorld
    // is explicit -- "the model's z becomes the world's y, and its y becomes
    // the world's z" -- and the JS engine says where the head is in the frame
    // BEFORE that swap: the fish's "long axis = model y, head at -y", and its
    // flyers' "the beak (model -depth) points along it". Model -y is world -z.
    //
    // So the nose is at local -z and the heading has to land there. Adding pi
    // to the angle is the whole fix and it keeps the matrix a proper rotation;
    // negating the column instead would flip the determinant and MIRROR the
    // model, which on a fish is a different bug wearing the same symptom.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // WHERE A DUCKLING'S EYES ARE -- v1's DUCKB_EYES scan.
    //
    // (user 2026-09-15: "the babies should cry".)
    //
    // THE TEARS HAVE TO COME FROM SOMEWHERE ON THE HEAD and a model knows where
    // that is: the eye is the BLACK voxel. Found once, at load, in the model's
    // own cells, so nothing downstream has to carry a hand-measured offset that
    // a re-authored baby.vox would silently invalidate.
    //
    // ONLY THE UPPER HALF IS SEARCHED, which is v1's rule and v1's reason: "a
    // duckling's feet are black too, and tears from the feet would be a
    // different effect entirely".
    //
    // THE HALF-EXTENT IS SUBTRACTED HERE, which is the one thing v1 got wrong
    // the first time and wrote a paragraph about: the offset wanted is from the
    // model's CENTRE, and using the raw cell index put every tear "a half-model
    // up-and-across from the duckling instead of at its eye -- bigger than the
    // duckling". +0.5 puts it at the middle of the cell rather than its corner.
    //
    // The .vox layout is x,y,z with Z up -- see VoxModel::at and the flip in
    // addFlyerModel -- so the model-space vector handed back is built in the
    // world convention the publish uses: x across, y UP (the model's z), z
    // along.
    void scanBabyEyes(const VoxModel &mo) {
        babyEyes_.clear();
        const float cx = 0.5f * float(mo.sx), cy = 0.5f * float(mo.sy),
                    cz = 0.5f * float(mo.sz);
        for (int z = mo.sz / 2; z < mo.sz; ++z)          // the head half, never the feet
            for (int y = 0; y < mo.sy; ++y)
                for (int x = 0; x < mo.sx; ++x) {
                    const uint8_t v = mo.at(x, y, z);
                    if (!v) continue;
                    const std::array<uint8_t, 4> &c = mo.pal[size_t(v) - 1];
                    if (c[0] > 40 || c[1] > 40 || c[2] > 40) continue;   // black only
                    babyEyes_.push_back(Vec3((float(x) + 0.5f - cx) * VOXEL_M,
                                             (float(z) + 0.5f - cz) * VOXEL_M,
                                             (float(y) + 0.5f - cy) * VOXEL_M));
                }
        std::printf("  duckling eyes  %d\n", int(babyEyes_.size()));
    }

    static void yawMat(float th, float *m) {
        const float c = cosf(th + 3.14159265f), s = sinf(th + 3.14159265f);
        m[0] = c;  m[1] = 0; m[2] = s;
        m[3] = 0;  m[4] = 1; m[5] = 0;
        m[6] = -s; m[7] = 0; m[8] = c;
    }

    void putFish(World &world, int slot, const Fish &f) const {
        // ONE FUNCTION, TWO SPECIES. The only thing a bass does differently is
        // wear a different strip -- everything else about it, down to the basis
        // built from its swim direction, is the salmon's.
        const std::vector<FishFrame> &strip = f.species == 6   ? betta_
                                             : f.species == 5 ? bluegill_
                                             : f.species == 4 ? catfish_
                                             : f.species == 3 ? minnow_
                                             : f.species == 2 ? koi_
                                             : f.species == 1 ? bass_
                                                              : salmon_;
        if (!f.live || strip.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const int fi = int(f.animClk) % int(strip.size());
        // -- BUILT FROM THE SWIM DIRECTION, not from two angles -------------
        //
        // The nose is at local -z (see yawMat), and a climbing fish also needs
        // a body-up that is square to the water rather than to the world. Both
        // fall out of building the basis from the direction itself:
        //
        //     c2 = -fwd     the nose
        //     c1 = up'      fwd x right, so it rolls with the climb
        //     c0 = c1 x c2  which is what makes it a ROTATION and not a
        //                   mirror -- a mirrored salmon is a real bug and it
        //                   looks almost exactly like a backwards one
        const float cp = cosf(f.pitch), sp = sinf(f.pitch);
        const float fx = sinf(f.th) * cp, fy = sp, fz = cosf(f.th) * cp;
        // right = up x fwd, horizontal by construction and never degenerate:
        // swimming the pitch is capped at 0.30 rad, and LEAPING at 1.2 -- still
        // short of the pi/2 that would collapse this length to zero.
        const float rl = maxf(1e-4f, sqrtf(fz * fz + fx * fx));
        const float rx = fz / rl, rz = -fx / rl;
        // up' = fwd x right
        const float ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;
        const float k = fadeOf(f.age, f.dying);
        const float m[9] = {-rx * k, ux * k, -fx * k,
                            0.0f, uy * k, -fy * k,
                            -rz * k, uz * k, -fz * k};
        // ...and the same correction as the pads: a corner, not a centre. A
        // fish is a metre long, so half of it is half a metre of error in
        // whichever direction it happens to be swimming.
        //
        // THIS FRAME'S HALF-BOX, NOT THE SPECIES'. See the note over FishFrame:
        // a swim frame's box changes width with the tail, so one stored
        // half-box for the whole strip walks the fish sideways every beat.
        const FishFrame &fr = strip[size_t(fi)];
        const float ox = m[0] * fr.hx + m[1] * fr.hy + m[2] * fr.hz;
        const float oy = m[3] * fr.hx + m[4] * fr.hy + m[5] * fr.hz;
        const float oz = m[6] * fr.hx + m[7] * fr.hy + m[8] * fr.hz;
        // THE ANIMAL, NOT ITS BOX -- and here the two ALREADY AGREE. This
        // publish subtracts exactly the half-box place() adds back, frame by
        // frame, so the derived centre lands on this point to the last bit and
        // the anchor changes no pixel today. It is passed anyway, because that
        // agreement is a coincidence of two separate pieces of arithmetic
        // staying in step, and the perched songbirds are what it looks like
        // when they stop: 0.15 m of motion vector on a bird that had not moved.
        // See World::place.
        const float anchor[3] = {f.x, f.y, f.z};
        // ...AND THE TURN, which setFlyerInstance derives from `m` about this
        // anchor -- the leap's pitch included, which Fish::dth (a yaw) could
        // not carry, and which went stale on a fish skipped off the water
        // field (user 2026-09-24, "creatures are ghosting").
        world.setFlyerInstance(slot, fr.model, m, f.x - ox, f.y - oy, f.z - oz, nullptr, true,
                               nullptr, anchor);
    }

    void putPad(World &world, int slot, const Pad &p) const {
        if (!p.live || lily_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        float m[9];
        yawMat(p.th, m);
        const float k = fadeOf(p.age, p.dying);
        for (int i = 0; i < 9; ++i) m[i] *= k;

        // -- THE TRANSLATION IS A CORNER, AND THIS IS WHY A DRAGONFLY MISSED --
        //
        // place() writes tx/ty/tz straight into the transform, so the model's
        // OWN ORIGIN lands there -- handing it a centre draws the model half its
        // own size away. A lily pad is 0.5 to 0.9 m across, so the leaf was
        // being drawn up to 45 cm from the position everything else in this file
        // believes it is at. The dragonfly landed on the pad's POSITION, which
        // is exactly where the pad was not: "the dragonfly doesnt seem to be
        // landing on the lillypad at all, but next to it."
        //
        // The half-box is carried through the turn and taken off, as the
        // butterflies and the pause room's buttons already do.
        const size_t mi = size_t(p.model);
        const float hxx = mi < lilyHX_.size() ? lilyHX_[mi] : 0.0f;
        const float hzz = mi < lilyHZ_.size() ? lilyHZ_[mi] : 0.0f;
        const float ox = m[0] * hxx + m[2] * hzz;
        const float oz = m[6] * hxx + m[8] * hzz;
        // THE LEAF, NOT ITS BOX -- see the note in putFish. The y is the one
        // that does not cancel here: the pad is drawn from its underside rather
        // than its middle, so the derived centre sits half a leaf's thickness
        // above the water line. A constant, so it never moved anything; this
        // makes it the water line, which is where the pad is.
        const float anchor[3] = {p.x, p.y, p.z};
        world.setFlyerInstance(slot, lily_[mi], m, p.x - ox, p.y, p.z - oz, nullptr, true, nullptr,
                               anchor);
    }

    void putDfly(World &world, int slot, const Dfly &d) const {
        if (!d.live || dfly_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        // FRAME 0 WHILE IT SITS. v1's note on the landed ladybug is the whole
        // argument: "frame 00 is the wings-SHUT pose, and holding it is what
        // makes a landing read as a landing rather than a hover at ground
        // level". A dragonfly beating its wings while parked is a dragonfly
        // hovering a centimetre above a leaf.
        const int fi = (d.ph == 2) ? 0 : int(clock_ * kDflyFps + d.phase) % int(dfly_.size());
        float m[9];
        yawMat(d.th, m);
        const float k = fadeOf(d.age, d.dying);
        for (int i = 0; i < 9; ++i) m[i] *= k;
        // ITS OWN FRAME'S HALF-BOX TOO, though for the dragonfly it changes
        // NOTHING: all six of its frames are 7 voxels wide, so its box never
        // moved and it never had the fish's wobble. Measured, not assumed --
        // and it goes through the same path anyway so that a re-authored wing
        // beat that DID resize the box could not quietly reintroduce it.
        const FishFrame &dr = dfly_[size_t(fi)];
        const float ox = m[0] * dr.hx + m[1] * dr.hy + m[2] * dr.hz;
        const float oy = m[3] * dr.hx + m[4] * dr.hy + m[5] * dr.hz;
        const float oz = m[6] * dr.hx + m[7] * dr.hy + m[8] * dr.hz;
        // THE ANIMAL, NOT ITS BOX -- and here the two ALREADY AGREE. This
        // publish subtracts exactly the half-box place() adds back, frame by
        // frame, so the derived centre lands on this point to the last bit and
        // the anchor changes no pixel today. It is passed anyway, because that
        // agreement is a coincidence of two separate pieces of arithmetic
        // staying in step, and the perched songbirds are what it looks like
        // when they stop: 0.15 m of motion vector on a bird that had not moved.
        // See World::place.
        const float anchor[3] = {d.x, d.y, d.z};
        world.setFlyerInstance(slot, dr.model, m, d.x - ox, d.y - oy, d.z - oz, nullptr, true,
                               nullptr, anchor);
    }

    void putDuck(World &world, int slot, const Duck &d) const {
        const std::vector<int> &mdl = (d.mom < 0) ? duckM_ : duckB_;
        if (!d.live || mdl.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        float m[9];
        yawMat(d.th, m);
        const float k = fadeOf(d.age, d.dying);
        for (int i = 0; i < 9; ++i) m[i] *= k;
        // The corner, not the centre -- see putPad for the whole argument. The
        // Y half is taken off too: a duck's waterline is its middle, not its
        // keel, so it sits IN the surface rather than on top of it.
        const float hx = (d.mom < 0) ? duckHX_ : babyHX_;
        const float hy = (d.mom < 0) ? duckHY_ : babyHY_;
        const float hz = (d.mom < 0) ? duckHZ_ : babyHZ_;
        const float ox = m[0] * hx + m[1] * hy + m[2] * hz;
        const float oy = m[3] * hx + m[4] * hy + m[5] * hz;
        const float oz = m[6] * hx + m[7] * hy + m[8] * hz;
        // THE DUCK, NOT ITS BOX -- see the note in putFish. Hers carries the
        // deliberate 0.45 of a half-box that sits her IN the surface, which is
        // a constant and so never moved anything either; the anchor is her, at
        // the waterline.
        const float anchor[3] = {d.x, d.y, d.z};
        world.setFlyerInstance(slot, mdl[0], m, d.x - ox, d.y - oy + hy * 0.45f + kDuckRideM,
                               d.z - oz, nullptr, true, nullptr, anchor);
    }

    // -----------------------------------------------------------------------
    // base.vox IS SOURCE ART, NOT A FRAME. Both strips ship one and the JS
    // engine skips it by name; loading it would put a still pose in the middle
    // of the cycle once per second.
    // -----------------------------------------------------------------------
    // A STRIP OUT OF ONE FILE, for art keyframed in MagicaVoxel rather than
    // exported to numbered files. loadStrip's sibling: same output, same
    // half-box bookkeeping, different packaging.
    //
    // BOTH OF THEM RECORD THE HALF-BOX PER FRAME. There used to be one
    // `lastHX_` here, carrying whatever the LAST frame happened to measure, and
    // the caller copied it into a per-species slot -- so eleven frames of every
    // strip were centred on a twelfth frame's box. See the note over FishFrame
    // for what that looked like.
    // -----------------------------------------------------------------------
    static FishFrame frameOf(int model, int sx, int sy, int sz) {
        FishFrame f;
        f.model = model;
        f.hx = 0.5f * float(sx) * VOXEL_M;
        f.hy = 0.5f * float(sy) * VOXEL_M;
        f.hz = 0.5f * float(sz) * VOXEL_M;
        return f;
    }

    void loadFrames(World &world, const std::string &path, std::vector<FishFrame> *out,
                    const char *what) {
        std::vector<VoxModel> mo;
        std::string err;
        if (!voxLoadAll(path, &mo, &err)) {
            std::fprintf(stderr, "v2: %s %s: %s -- skipped\n", what, path.c_str(),
                         err.c_str());
            return;
        }
        for (const VoxModel &m : mo) {
            int sx = 0, sy = 0, sz = 0;
            const int id = world.addFlyerModel(m, what, &sx, &sy, &sz, true);
            if (id < 0) { out->clear(); return; }
            out->push_back(frameOf(id, sx, sy, sz));
        }
        std::printf("  lake     %s %zu frames out of one file\n", what, mo.size());
    }

    void loadStrip(World &world, const std::string &dir, int frames,
                   std::vector<FishFrame> *out, const char *what) {
        // NOT `mo(size_t(frames))`. That is the most vexing parse: `size_t(frames)`
        // reads as a PARAMETER DECLARATION, so `mo` becomes a function
        // declaration and every use of it below fails with errors that name
        // the uses rather than this line.
        std::vector<VoxModel> mo;
        mo.resize(size_t(frames));
        for (int f = 0; f < frames; ++f) {
            char path[600];
            std::snprintf(path, sizeof(path), "%s/%02d.vox", dir.c_str(), f);
            std::string err;
            if (!voxLoad(path, &mo[size_t(f)], &err)) {
                std::fprintf(stderr, "v2: %s %s: %s -- skipped\n", what, path, err.c_str());
                return;
            }
        }
        for (int f = 0; f < frames; ++f) {
            int sx = 0, sy = 0, sz = 0;
            const int m = world.addFlyerModel(mo[size_t(f)], what, &sx, &sy, &sz, true);
            if (m < 0) { out->clear(); return; }
            out->push_back(frameOf(m, sx, sy, sz));
        }
    }

    bool ready_ = false;
    float clock_ = 0.0f;
    WaterField field_;
    std::vector<Site> sites_;   // the candidate list, reused between passes
    Site held_{};               // what freeSite last handed back
    std::vector<FishFrame> salmon_, bass_, koi_, minnow_, catfish_, bluegill_, betta_, dfly_;
    // The pads are not a strip -- three separate models, one picked per pad --
    // and they already carry their own extents in lilyHX_/lilyHZ_ below.
    std::vector<int> lily_;
    std::vector<float> lilyHalf_, lilyHX_, lilyHZ_, lilyR_;
    // Kept across pads and across frames rather than made per call: it is a
    // cache that validates itself, and consecutive rim samples of one leaf are
    // centimetres apart.
    TerrainMemo padMemo_;
    // Borrowed for the length of one update() -- see the note there.
    const VoxelTerrain *terrain_ = nullptr;
    BirthGate birth_;
    float yieldAt_ = 0.0f;
    long padNear_ = 0, padClash_ = 0;
    float padWorst_ = 0.0f, padClosest_ = 1e9f;
    // HOW MANY KINDS OF FISH THERE ARE, and a power of two because the only
    // readers left mask with it -- see census(), which counts the live ones.
    //
    // IT USED TO SIZE A PER-SPECIES HALF-BOX, and the note here used to explain
    // why eight rather than four: a fifth fish folding onto the salmon's entry
    // would have been drawn half a salmon out of place with nothing to say so.
    // That table is gone -- the half-box belongs to the FRAME now, and the
    // frame carries it (see FishFrame) -- so a new fish cannot be mis-sized by
    // forgetting to widen anything.
    static constexpr int kFishSpecies = 8;
    std::vector<Fish> fish_;
    std::vector<Duck> ducks_;
    std::vector<int> duckM_, duckB_;   // the two models: mother, duckling
    float duckHX_ = 0.0f, duckHY_ = 0.0f, duckHZ_ = 0.0f;
    float duckR_ = 0.25f, babyR_ = 0.15f;
    float babyHX_ = 0.0f, babyHY_ = 0.0f, babyHZ_ = 0.0f;
    std::vector<Pad> pads_;
    std::vector<Dfly> flies_;
    // WHERE EACH WAS KILLED, by its own vector's index -- see KillHold. The
    // pads have none: a lily pad is not life.
    KillHold fishHold_, dflyHold_, duckHold_;
};

}  // namespace v2
