#pragma once
// ---------------------------------------------------------------------------
// THE FOUR SMALL ONES: THE ANT, THE FLY, THE LADYBUG AND THE FROG.
//
// Ported from the JS engine on 2026-09-14 ("import the ant to all forests",
// "import the fly just to the pine forest", "import the ladybug all forests",
// "import the frog in birch only ... import all the mechanics too"). Two more
// of that batch -- the worm and the grass snake -- are NOT here: they are
// strip-walking ground animals and went straight onto the marcher table in
// render/bunnies.h, which is exactly what that table is for.
//
// WHY THESE FOUR SHARE A FILE. Each has a motion model the marcher does not
// have and none of them is big enough to earn a file of its own:
//
//   ant       walks the COMPASS and the rest of the column walks HIS PATH
//   fly       orbits a knot of air that is itself drifting
//   ladybug   flies the butterfly's lane and commits to a landing
//   frog      does not travel continuously at all -- it leaps, and the leap is
//             a table of per-frame offsets
//   firefly   only exists after dark, and is the one thing in this engine that
//             EMITS -- see kGlowRgb and V6Params::glowMtl
//
// What they DO share is the boilerplate: one lattice claim, one birth gate, one
// run of the flyer band, one report. That is the whole argument for one file --
// four copies of the claiming machinery is four places for it to drift, which
// is the lesson kMarchSpec already records for the mammals.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "core/noise.h"
#include "core/vecmath.h"
#include "world/world.h"
#include "player/collide.h"
#include "voxel/vox.h"

namespace v2 {

// ---------------------------------------------------------------------------
// HOW MANY OF EACH, AND WHY.
//
// v1's numbers are head-counts over a disc 104 m across; v2 spawns off a
// lattice and holds a much smaller live set at any moment, so these are its
// densities brought onto that lattice rather than its counts copied over. The
// exception is the FROG, which is 2 in both because 2 is what the user set
// ("halve the rate of the frogs") and a waterside animal is not diluted by
// anything -- it lives on the bank, and there is only so much bank.
// ---------------------------------------------------------------------------
// -- HOW WIDE A SMALL ANIMAL IS, for the one question that needs a size ----
//
// Not a hitbox and not used for anything but solidsTouch: it is how far off its
// own centre a body is probed when asking whether it may be somewhere. A
// housefly is 0.3 m across and a frog is about 0.5 m, so these are their half
// widths and nothing is tuned about them -- a value too big reads as an insect
// that will not go near a rock, one too small lets a corner of the model sit in
// the stone, and both are visible.
inline constexpr float kCritBodyR = 0.15f;
// HOW BIG A THING A FLYING INSECT GOES OVER RATHER THAN ROUND -- see
// solidsFloor. Three metres is the ladybug's own drop limit (kLbugDropMax) and
// about twice the tallest cruise line in this file, so a stone an insect could
// plausibly rise over is ground and a boulder is a wall.
inline constexpr float kCritRiseM = 3.0f;
inline constexpr float kFrogBodyR = 0.25f;
// AND HOW MUCH AIR IT OCCUPIES: its own height plus the rise in kFrogHop, whose
// tallest frame is five voxels. Taken as one number rather than read off the
// table per frame -- the guard has to hold for the whole leap it is allowing,
// not for the frame it happens to be on.
inline constexpr float kFrogTallM = 0.8f;

inline constexpr int kAntCount = 6;    // v1's DES_OAK ant:4, freed of the desert's shared band
inline constexpr int kHouseflyCount = 8;    // v1 gives the fly its WHOLE band in the forest
// THREE (user 2026-09-14: "reduce the amount of ladybugs in half"). v1's
// DES_OAKONLY ladybug:6 was the port's number and it is a DESERT density; this
// wood has five other small species sharing the same floor.
inline constexpr int kLbugCount = 3;
// SIX, NOT v1'S TWO (user 2026-09-14: "I dont see the frog on the field").
//
// v1's 2 is a DESERT number -- DES_OAKONLY frog:2 -- and over there the pond is
// a landmark you walk to. Here the frog is gated three ways at once: birch wood
// only, within kFrogShoreM of a bank, and two slots to cover every shore in the
// streaming ring. Any one of those is reasonable; all three together is an
// animal that exists and is never met, which is what was reported.
//
// The bank is still the only place it will stand, so this makes shores busier
// rather than putting frogs in the wood.
// THREE (user 2026-09-14: "reduce the amount of frogs in half"). Six was set
// two hours earlier to make the frog findable at all -- and it was not findable
// because it was never DRAWN, which was a slot-band fault rather than a count
// one. With that fixed, six on one shoreline is a chorus.
inline constexpr int kFrogCount = 3;
// HALF THE BUTTERFLIES, WHICH IS v1's RULE. Its flyer band carries butterflies
// by day and fireflies after dark, and the night target is `nActD >> 1` against
// the day's `nActD * 3.125` -- so the night sky is deliberately thinner than
// the day one. Six against the twelve butterflies v2 settled on.
inline constexpr int kFireflyCount = 6;

inline constexpr int kCritterCount =
    kAntCount + kHouseflyCount + kLbugCount + kFrogCount + kFireflyCount;

// ---------------------------------------------------------------------------
// THE ANT, AND IT WALKS ON THE COMPASS.
//
// v1: "just have it move forward. when it wants to turn, rotate the ant 90
// degrees". The heading is an INTEGER 0-3 into kAntDir and never an angle, so a
// diagonal is not expressible; a turn is +/-1 on that integer and lands in one
// frame. A committed turn is held for kAntTurnHold so a corner can never become
// a spin.
//
// AND THE COLUMN IS A PATH, NOT A FORMATION. "them following eachother": the
// leader drops a CRUMB every kAntCrumb metres and each follower walks the trail
// at kAntGap behind the one in front. A follower therefore traces the leader's
// corner rather than cutting it, which is the whole difference between a column
// and four ants steering at each other.
// ---------------------------------------------------------------------------
inline constexpr float kAntSpeed = 1.6f;      // v1's shared 16 vox/s
inline constexpr float kAntLookM = 0.5f;      // ANT_LOOK 5 vox of clear ground ahead
inline constexpr float kAntTurnHold = 0.35f;  // ANT_TURN_HOLD
inline constexpr float kAntWhim = 1.5f;       // rolls every 1.5..3.0 s
inline constexpr float kAntWhimP = 0.35f;     // ...and turns on 35% of them
inline constexpr float kAntGap = 0.6f;        // ANT_GAP 6 vox -- the model is 0.2 m long
inline constexpr float kAntCrumb = 0.075f;    // ANT_CRUMB 0.75 vox
inline constexpr int kAntTrail = 220;         // crumbs kept: kAntGap*kAntCount/kAntCrumb, doubled
inline constexpr float kAntCellM = 26.0f;     // one column per cell -- they are a GROUP
inline constexpr uint32_t kAntSalt = 0x1A47u;
inline constexpr int kAntDir[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};

// ---------------------------------------------------------------------------
// THE FLY, WHICH IS A BUNCH OF FLIES AND NEVER ONE FLY.
//
// v1 measured this one twice and the note is worth carrying over: "the flys
// just seem to be moving in one location" -- over 37.6 s each fly travelled
// 13 x 13 voxels, which is exactly the orbit diameter, and the anchor moved
// 0.00 on both axes. The ring was right and the post it hangs on was nailed
// down. So the ANCHOR owns its own heading and wanders; the ring is drawn
// around wherever the anchor has got to.
// ---------------------------------------------------------------------------
inline constexpr int kHouseflyFrames = 4;
inline constexpr int kHouseflyPerBunch = 5;        // FLY_BUNCH
inline constexpr float kHouseflyBunchSpeed = 1.0f; // FLY_BN_SPD 10 vox/s
inline constexpr float kHouseflyBunchTurn = 0.85f; // FLY_BN_TURN rad/s
inline constexpr float kHouseflyBunchRoamM = 4.4f; // FLY_BN_R -- then it is steered home
inline constexpr float kHouseflyOrbitM = 0.65f;    // BEE_ORBIT_R, which v1 shares with the bee
inline constexpr float kHouseflyOrbitW = 1.9f;     // BEE_ORBIT_W rad/s
inline constexpr float kHouseflyOrbitY = 0.22f;    // BEE_ORBIT_Y -- the swarm's vertical spread
// -- HOW HIGH A BUNCH HANGS, AND IT IS v1'S OWN LIFT ------------------------
//
// (user 2026-09-14: "also make the flys higher up on average while flying.")
//
// 1.1 m was the bare glide line with nothing added, and v1 does not fly its
// housefly there: `DES_FLY_UP = 16` -- "rides this much higher than a
// butterfly's glide line" -- is sixteen voxels of lift that exist for this
// species alone, and the port left it out. This is that lift, 1.6 m, on the
// line v2 already had. A swarm now hangs at about head height instead of at
// knee height, which is where you meet one.
inline constexpr float kHouseflyCruiseM = 2.7f;    // 1.1 glide + v1's DES_FLY_UP
inline constexpr float kHouseflyCellM = 30.0f;
inline constexpr uint32_t kHouseflySalt = 0x6F13u;
inline constexpr float kHouseflyFps = 24.0f;
// -- THE WINGS ARE HALF THERE ----------------------------------------------
//
// "Make their wings 50% transparent." fly/00.vox is three voxels: one body at
// (23,23,23) with a wing either side of it at pure white, and the four frames
// are those two wings beating. So the wing IS a colour, which is what makes
// this a one-entry change -- see Palette::setAlpha and V6Material::alpha.
//
// REGISTERED EXACTLY, and it has to be. White is the most shared colour in the
// world -- cloud, snow, birch bark, the asset deck -- and forModelColor snaps
// anything within kModelMatch onto an entry that already exists. Snapped, this
// would have made every white surface in the wood half see-through. The
// firefly's emissive yellow is claimed the same way for the same reason.
//
// AND IT IS NOT PURE WHITE ANY MORE. It was, because that is what the file is
// authored in -- and pure white is the most crowded colour in the palette. The
// snap takes the FIRST strictly-nearest entry scanning upward, so the asset
// deck's own white (registered long before this) won every time and the wing's
// material sat in the table wearing nothing: renders at alpha 0 and alpha 1
// came back pixel-for-pixel identical, which is what an orphaned material looks
// like.
//
// A FAINT BLUE-WHITE INSTEAD, far enough from every other entry that the snap
// cannot land anywhere else -- 47 units from pure white against a kModelMatch
// of 16 -- and close enough to white that a wing is still a wing. The loader
// repaints the model's own voxels to it, so the colour in the table and the
// colour on the model are the same fact rather than two that must agree.
//
// AND IT IS CHECKED RATHER THAN ASSUMED: load() resolves this through
// Palette::resolveModelColor afterwards and prints how many other entries are
// within tolerance. If that number is ever not 1, this colour has to move.
inline constexpr uint8_t kHouseflyWingRgb[3] = {214, 232, 255};
// -- ...AND NOW IT IS SIMPLY A WHITE VOXEL (user 2026-09-14) ---------------
//
// "can you just make the fly wings a solid white voxel. just like everything
// else in the world."
//
// SO EVERYTHING ABOVE IS GONE, and that is the point rather than a loss. Two
// attempts at a special surface came before this -- a 50% stochastic
// pass-through, then a Fresnel dielectric -- and the wing needed, between them,
// a private palette entry it was not allowed to share, a repaint of the model
// on the way in to keep it off pure white, a runtime material setter, a
// per-entry alpha in two structs and a branch in the camera loop. All of that
// so that one three-voxel model could be unlike the rest of the world.
//
// A white voxel is white. The model's own colour registers through
// forModelColor like every other model's, shares the world's white with the
// cloud and the birch bark, and is shaded by the same materials as everything
// else -- which is exactly what was asked for and needs no code at all.
//
// WHAT IS LEFT BEHIND: V6Material::alpha and the block in Trace.cs.slang that
// reads it are still there and NOTHING NOW SETS alpha BELOW 1. Kept because it
// is a general facility that cost one compare, not because anything uses it;
// if it is still unused the next time somebody reads this, delete it.
// -- ...AND THEY DO NOT SIT INSIDE ONE ANOTHER -----------------------------
//
// "It looks like there are flys stuck in eachother." They were: every fly in a
// bunch rides the same ring at the same radius, and its place on it was a HASH
// of its slot index -- so nothing stopped two of them drawing the same angle.
// Five random phases on a 4.1 m circumference put two within 0.13 m often
// enough to see, and a fly is 0.3 m across.
//
// EVENLY SPACED BY RANK, with a little jitter so the ring is not a clock face,
// and each fly on its OWN radius as well -- two flies that do pass each other
// in angle are then at different distances from the anchor and still miss.
// -- WHAT MADE THE SWARM JUDDER (user 2026-09-14: "the flies are very glitchy
//    whwn flying") -----------------------------------------------------------
//
// THREE THINGS, AND THE FIRST IS MOST OF IT.
//
// 1. THE ANCHOR'S HEIGHT WAS WRITTEN, NOT EASED, AND THE GROUND IS A STAIRCASE.
//    `groundAt` is (heightVox + 1) * VOXEL_M -- a step function with a tread of
//    ten centimetres. The bunch's post drifts at 1 m/s, so it crossed a column
//    boundary about ten times a second and the height under it JUMPED each
//    time. Every fly on the ring is hung off that post, so all five popped a
//    tenth of a metre together, ten times a second. That is not a flight model
//    problem; it is a quantised input read as though it were continuous.
//
//    Everything else in this file that holds a line already eases it -- the
//    ladybug's altitude servo, the butterfly's gRef -- and the fly was the one
//    that assigned.
//
// 2. THE RING POINT WAS A TELEPORT. Each fly's position was ASSIGNED from the
//    orbit every frame, so the flySlide guard added last week could only hold
//    it still and then drop it back on the ring when the lane cleared, which is
//    a jump of up to the orbit's diameter.
//
// 3. AND THE YAW CAME FROM THE FRAME'S OWN DELTA, so any of the above spun the
//    model as well as moving it.
inline constexpr float kHouseflyRiseRate = 3.5f;   // the anchor's altitude ease, per second
inline constexpr float kHouseflyTrackM = 3.2f;     // how fast a fly may close on its ring point
inline constexpr float kHouseflyYawRate = 9.0f;    // ...and how fast it may turn to face it
// ...and past this it is not tracking, it is arriving. See the ring loop.
inline constexpr float kHouseflySnapM = 2.0f;
inline constexpr float kHouseflyPhaseJit = 0.35f;   // radians, of a 1.26 rad gap
inline constexpr float kHouseflyRingSpread = 0.22f; // +/- this fraction of the radius

// ---------------------------------------------------------------------------
// THE LADYBUG, WHICH FLIES THE BUTTERFLY'S LANE AND THEN LANDS.
//
// A descent SUSPENDS the altitude servo that keeps a flyer over the canopy,
// which is the only way it can reach the ground at all -- and is exactly how
// v1's ended up wedged in crowns, "worst in the pine forest where they are
// tallest and densest". So a descent is only ever committed down a column
// PROVEN CLEAR, from no higher than kLbugDropMax, and it gives up after
// kLbugDropSec if it has not arrived.
//
// AND ITS HEAD IS AT +y. v1 keeps a table for this (DES_BACKWARDS) because a
// species baked by its own tool points one way and one adopted from the asset
// editor's scene-graph loader points the other. ladybug.vox is the second kind,
// so its yaw carries an extra half turn.
// ---------------------------------------------------------------------------
inline constexpr int kLbugFrames = 6;
inline constexpr float kLbugCruiseM = 1.4f;   // LBUG_CRUISE 14 vox above ground
inline constexpr float kLbugDropMax = 3.0f;   // LBUG_DROP_MAX 30 vox
inline constexpr float kLbugDropSec = 8.0f;   // LBUG_DROP_MAX_S
inline constexpr float kLbugSpeed = 2.2f;
inline constexpr float kLbugSitSec = 3.5f, kLbugSitJit = 4.0f;
inline constexpr float kLbugTrySec = 2.2f;    // "keeps flying and asks again in a couple of seconds"
// ...AND HOW LONG IT MUST FLY BEFORE IT MAY LOOK AGAIN. Without this it takes
// off and commits to the next clear column two seconds later, which measured at
// 66% of its life spent sitting. A ladybug is a flyer that lands, not a walker
// that occasionally hops.
inline constexpr float kLbugFlySec = 11.0f;
inline constexpr float kLbugCellM = 22.0f;
inline constexpr uint32_t kLbugSalt = 0x2C8Bu;
// 24, WHICH IS THE HOUSE RATE (user 2026-09-15: "make sure that the ladybug is
// going through its frames at 24 fps"). It was 18 -- the odd one out among the
// housefly's 24, the frog's 24 and every strip v1 steps at 24 -- and at six
// frames a cycle that is a third of a second against a quarter, which reads as
// a beetle labouring rather than one whirring.
inline constexpr float kLbugFps = 24.0f;

// -- ...AND IT RUNS AT DOUBLE RATE WHEN YOU ARE STANDING OVER IT ------------
//
// (user 2026-09-15: "if the player is near, the ladybug moves at twice the
// rate, double in both fps but also movement speed".)
//
// BOTH HALVES COME FROM ONE NUMBER, because "twice the rate" is one idea:
// stepBugs scales its own dt, so the flight speed, the descent and the
// wingbeat all double together and cannot drift apart the way three separate
// multipliers would. What is deliberately NOT scaled is the phase clock -- how
// long it sits, and the flying time it owes before it may look for the next
// landing. Those are decisions about what to do next, not rates of motion, and
// halving them would have the insect landing and taking off twice as often
// rather than moving twice as fast.
//
// A BAND, NOT A LINE. Switching at a radius puts a step in the speed of
// something you are looking at from two metres away -- it would read as the
// ladybug flinching. Full rate inside kLbugNearM, ordinary beyond
// kLbugFarM, smoothstepped between, so walking up to one speeds it up
// continuously and walking away slows it the same way.
inline constexpr float kLbugRateMul = 2.0f;
inline constexpr float kLbugNearM = 5.0f;   // inside this it is at full double rate
inline constexpr float kLbugFarM = 10.0f;   // ...and beyond this it is its ordinary self

// -- ...AND CLOSER STILL, IT LEAVES ----------------------------------------
//
// (user 2026-09-18: "make ladybugs scare away from the player if the player
// gets too close. it should go from landing still to flying away at 2x speed.
// it was supposed to have this mechanic already.")
//
// AND IT HALF DID, WHICH IS WHY IT LOOKED BROKEN. The double rate above was
// already here and already working -- but it only scales the dt of whatever the
// insect is DOING, and what a landed one is doing is sitting. Walking up to a
// ladybug ran its sit clock at double speed and left it sitting there. The
// missing piece is not a speed, it is a state change.
//
// INSIDE kLbugNearM, so it is at the full double rate on the frame it bolts --
// "flying away at 2x speed" is one behaviour, and a scare radius outside the
// rate band would have it leave at ordinary pace and then speed up, which is
// the wrong way round. 3 m is about two paces: close enough to be crowding it,
// far enough that they do not all leave as you cross a clearing.
//
// A DESCENT IS ABORTED TOO. A bug committed to a column you have since walked
// into would otherwise land at your feet and only then take fright.
inline constexpr float kLbugScareM = 3.0f;

// -- AND THEY ALL HAVE TO FIT IN THE BAND ---------------------------------
//
// THIS IS THE CHECK THAT WAS MISSING, and its absence cost the frog its entire
// existence on screen: kCritterSlots was two short of the population before the
// frog count was ever touched, the frog publishes LAST, and a write past the
// band is dropped in silence. Every other part of the engine agreed the frog
// was there -- it spawned, it hopped, /locate found it, the census counted it.
//
// The lake and the marchers have carried an assert like this from the start
// (see LakeLife's slot sum and kMarchCount); this file did not, and it is the
// one with five populations in it.
static_assert(kFireflyCount + kAntCount + kHouseflyCount + kLbugCount + kFrogCount <=
                  kCritterSlots,
              "the critters do not fit in their run of the flyer band -- see kCritterSlots");

// ---------------------------------------------------------------------------
// THE FROG, AND THE LEAP IS NOT IN THE FILE.
//
// frog.vox holds three cycles on three shape nodes and EVERY HOP FRAME SITS
// WITH ITS LOWEST VOXEL AT MODEL-Z 0 -- played as authored the frog crouches,
// stretches and lands without ever leaving the ground. kFrogHop below is v1's
// hand-authored bake (FROG_OFF.hop), [up, forward] per frame in voxels, and the
// sim applies it as MOTION. That same table is what carries the frog forward a
// metre per leap, so it is not decoration: drop it and the frog hops on the
// spot forever.
//
// WHICH SHAPE IS WHICH, and it is answered by GEOMETRY rather than by a name,
// because v2's .vox reader keeps the shape a piece came from but not its label:
//
//     shape  5   14 frames   box 9 x  8 x  6   compact          -> ribbet
//     shape  9   24 frames   box 9 x 13 x  6   reaches FORWARD  -> tongue
//     shape 13   17 frames   box 9 x 10 x 10   reaches UP       -> hop
//
// and the 17 cross-checks against v1, which documents the hop as 17 frames.
// ---------------------------------------------------------------------------
inline constexpr int kFrogHopFrames = 17;
inline constexpr int kFrogRibbetFrames = 14;
inline constexpr int kFrogTongueFrames = 24;
inline constexpr int kFrogHopShape = 13, kFrogRibbetShape = 5, kFrogTongueShape = 9;
inline constexpr float kFrogFps = 24.0f;
// [up, forward] in VOXELS, per hop frame. Forward is negative because the
// model's nose is at -y; the leap is 10 voxels, which is the metre v1 measured.
inline constexpr float kFrogHop[kFrogHopFrames][2] = {
    {0, 0}, {0, 0}, {0, 0}, {0, -1}, {2, -3}, {3, -4}, {4, -5}, {5, -6}, {5, -7},
    {3, -8}, {1, -8}, {0, -8}, {0, -8}, {0, -9}, {0, -10}, {0, -10}, {0, -10}};
// The turning hop IS the hop, frame for frame, with the FORWARD column dropped
// -- so it keeps the leap's rise and lands back on its own square. Derived
// rather than typed out again: two tables that must stay identical are two
// tables that will not.
inline constexpr int kFrogTurnFrame = 8;      // the top of the leap, so the pivot scrapes nothing
inline constexpr int kFrogMix[4] = {40, 40, 10, 10};   // hop, ribbet, tongue, rotate
inline constexpr float kFrogShoreM = 1.4f;    // DES_WATER 14 vox -- ON the bank, never in the lake
inline constexpr float kFrogApartM = 2.0f;    // DES_WATER_APART 20 vox
inline constexpr uint32_t kFrogSalt = 0x3B7Du;
// -- AND THE FROG DOES NOT USE THE LATTICE AT ALL ---------------------------
//
// It was on one, with a 9 m cell and a shore test on the site, and it NEVER
// SPAWNED: a lattice cell inside the 46 m claim radius is almost never within
// 1.4 m of a lake, so every candidate was refused and the wood had no frogs in
// it. The measurement that caught it is /locate's own -- "frog birch - NONE,
// AND IT SHOULD BE HERE" on a birch spawn whose nearest water was 80 m away.
//
// A WATERSIDE ANIMAL IS PLACED FROM THE WATER, NOT FROM THE GROUND. The lake
// already keeps a 420 m field of sampled columns and already knows how to draw
// a point out of it, so the app hands a few BANK SPOTS in on the same
// half-second clock the bees get their hives on -- see LakeLife::bankSpots.
// The dragonfly reaches the same conclusion by the same route in v1, which is
// what the user meant by "around water, similar to the dragonflies in that way".
//
// ITS REACH IS THE LAKE'S, NOT THE CRITTERS'. A frog belongs to a body of
// water that may be a hundred metres off, and a population that only exists
// when you are standing on top of it is a population nobody ever sees.
inline constexpr float kFrogSpawnM = 130.0f;
inline constexpr float kFrogDropM = 165.0f;

// ---------------------------------------------------------------------------
// THE FIREFLY, WHICH IS THE NIGHT'S OWN LIGHT.
//
// v1: the flyer band is butterflies by day and fireflies after dark, and the
// swap is the whole of what a firefly is -- "the FIREFLIES, which only exist at
// night, are the night's own light source". So this population is empty while
// the sun is up and costs exactly the instance masks its slots occupy.
//
// AND IT ACTUALLY EMITS, which nothing else in this engine does. v2 has two
// lights, the sun and the sky dome, and no emissive materials at all -- the
// pause room's bulb was given a hook of its own for exactly this reason and
// the note on it says so: "this is the hook to reuse for any torch, campfire or
// lantern". A firefly is that hook's second user. It is EMISSIVE ONLY: the
// bulb's half of the mechanism traces a shadow ray to one point light, which
// means nothing for six insects drifting apart, so the firefly takes the
// material test and not the estimator. See V6Params::glowMtl.
//
// THE COLOUR IS REGISTERED EXACTLY, and it has to be. forModelColor snaps a
// model's colours onto ones the world already owns when they are within
// Palette::kModelMatch -- which is what kept all seven of these species inside
// seven palette entries -- and a snapped glow would hand its emissive material
// to whatever yellow it landed on. A flower that glows in the dark is the bug
// this prevents.
// ---------------------------------------------------------------------------
inline constexpr int kFireflyFrames = 4;
inline constexpr float kFireflySpeed = 2.6f;   // v1's kind-1 26 vox/s
// 12 -> 24 (user 2026-09-17: "make sure the firefly is running at 24 fps").
// 24 is the house rate every other strip in the browser engine is played at --
// see its EAT_FPS -- so this stops being the one population with a rate of its
// own.
inline constexpr float kFireflyFps = 24.0f;
// -- AND IT BLINKS ---------------------------------------------------------
//
// (user 2026-09-17: "have the fire fly blink its spark voxel from emmitting to
//  off".)
//
// ON FOR kFireflyOnS, OFF FOR kFireflyOffS, on its OWN phase per insect -- a
// field of them blinking in step is a string of fairy lights, not fireflies.
//
// IT IS THE INSTANCE THAT GOES, NOT THE MATERIAL. The firefly wears the spark's
// voxel now, and that voxel is emissive for everything that wears it -- a
// material is shared by definition, so there is no per-insect way to dim one.
// Not drawing the slot is the only switch that belongs to ONE firefly, and it
// takes the point light with it because publishSparkLights asks the same flag.
inline constexpr float kFireflyOnS = 0.55f;
inline constexpr float kFireflyOffS = 0.85f;
inline constexpr float kFireflyCruiseM = 1.3f;
inline constexpr float kFireflyBobM = 0.45f;
inline constexpr float kFireflyBobSec = 3.1f;
inline constexpr float kFireflyTurn = 1.1f;    // rad/s of wander on its heading
inline constexpr float kFireflyCellM = 16.0f;
inline constexpr uint32_t kFireflySalt = 0x4E29u;
// The lamp in the tail, as authored in firefly/00.vox.
// CANDIDATES, NOT ONE COLOUR. The lamp has to own its material outright -- a
// shared one would hand the emissive to whatever else is wearing it, and a
// butterfly that glows in the dark is the bug. Measured: the authored
// (252, 215, 5) has a neighbour inside kModelMatch, so it cannot be private;
// privateTint walks these until one has NOTHING within tolerance and the
// loader repaints the model to it. They are all a firefly's yellow.
inline constexpr uint8_t kGlowCand[][3] = {
    {252, 215, 5}, {255, 200, 0}, {248, 232, 24}, {255, 184, 12}, {236, 246, 40}};
inline constexpr uint8_t kGlowRgb[3] = {252, 215, 5};   // the authored one, for the repaint FROM
// ...and how bright it renders. The pause room's bulb is 12 working units and
// is meant to be stared into; a firefly is a speck you see across a clearing,
// so it is brighter per voxel and there is almost no voxel of it.
inline constexpr float kGlowNits = 26.0f;

// How far out a critter may be born and how far out it is given up, shared by
// all of them. The near floor is the engine's own kBirthMinM.
inline constexpr float kCritSpawnM = 46.0f;
inline constexpr float kCritDropM = 62.0f;
inline constexpr float kCritFadeSec = 0.7f, kCritFadeMin = 0.08f;

// ---------------------------------------------------------------------------
class Critters {
  public:
    using GroundF = std::function<float(float, float)>;
    using WetF = std::function<bool(float, float)>;
    // Which wood a column is in, as a kWood* bit -- see VoxelTerrain::woodBit.
    using BirchF = std::function<uint8_t(float)>;

    bool ready() const { return ready_; }

    // -----------------------------------------------------------------------
    // LOAD. A missing species disables THAT species and nothing else, which is
    // the rule every other loader in this engine follows -- see the songbirds.
    // -----------------------------------------------------------------------
    // -- ...AND THE FIREFLY IS THE SPARK ---------------------------------
    //
    // (user 2026-09-17: "have the lightning bug at night share the same lit
    //  voxel as the spark voxel.")
    //
    // `sparkRgb` / `sparkMtl` are what Particles actually took (its private
    // tint is chosen at runtime, so neither is a constant anybody can write
    // down). Given them, the firefly repaints onto that exact voxel instead of
    // claiming a lamp of its own -- which is the ask, and which also gives it
    // the spark's emitter entry and its volumetric glow for nothing.
    //
    // IT ALSO HANDS BACK A PALETTE ENTRY. The firefly used to claim a private
    // yellow of its own; two things that glow the same way no longer cost two
    // slots on a 255-entry table.
    bool load(World &world, const std::string &lifeDir, const uint8_t *sparkRgb = nullptr,
              uint8_t sparkMtl = 0) {
        // THE GLOW IS CLAIMED BEFORE THE MODEL THAT WEARS IT. Registered
        // exactly, so the snap below finds this entry rather than minting a
        // near-miss -- and so nothing else in the world can be handed it.
        // THE LAMP PICKS A COLOUR NOBODY IS NEAR, then the model is repainted
        // to it on the way in -- see privateTint.
        // THE SPARK'S OWN VOXEL WHEN THERE IS ONE, and a lamp of its own only
        // if the particles never loaded -- so a world with no embers still has
        // fireflies rather than invisible ones.
        const uint8_t fallback[3] = {kGlowCand[0][0], kGlowCand[0][1], kGlowCand[0][2]};
        const uint8_t *glow = sparkRgb
                                  ? sparkRgb
                                  : privateTint(world, kGlowCand,
                                                int(sizeof(kGlowCand) / sizeof(kGlowCand[0])));
        if (!glow) glow = fallback;
        sharesSpark_ = sparkRgb != nullptr;
        sparkMtl_ = sparkMtl;
        const uint8_t glowPaint[6] = {kGlowRgb[0], kGlowRgb[1], kGlowRgb[2],
                                      glow[0],     glow[1],     glow[2]};
        glowWant_[0] = glow[0];
        glowWant_[1] = glow[1];
        glowWant_[2] = glow[2];
        // THE ONE STRIP THAT MAY NOT FOLD. Its yellow must come back out of
        // the table as an id of its own -- if it snapped onto the butterflies'
        // yellow, every butterfly in the wood would light up after dark, which
        // is the failure the resolve below was written for. See loadStrip's
        // matchTol.
        loadStrip(world, lifeDir + "/firefly", kFireflyFrames, "firefly", &ffly_, glowPaint,
                  /*matchTol=*/0);

        loadStrip(world, lifeDir + "/ant", 1, "ant", &ant_);
        // NO REPAINT AND NO PRIVATE MATERIAL. The wing is the white the file
        // was authored in, snapped onto whichever entry already holds it -- see
        // the note over kHouseflyWingAlpha's grave.
        loadStrip(world, lifeDir + "/fly", kHouseflyFrames, "fly", &fly_);
        // BY SHAPE ORDER, NOT BY MODEL ORDER -- see loadShape. -1 is "every
        // piece": the file has one shape and six frames of it, and what was
        // wanted from it was never a filter, only the right order.
        loadShape(world, lifeDir + "/ladybug.vox", -1, "ladybug", &lbug_);
        loadShape(world, lifeDir + "/frog.vox", kFrogHopShape, "frog hop", &frogHop_);
        loadShape(world, lifeDir + "/frog.vox", kFrogRibbetShape, "frog ribbet", &frogRib_);
        loadShape(world, lifeDir + "/frog.vox", kFrogTongueShape, "frog tongue", &frogTon_);

        fireflies_.assign(size_t(kFireflyCount), Firefly{});
        ants_.assign(size_t(kAntCount), Ant{});
        flies_.assign(size_t(kHouseflyCount), Fly{});
        bugs_.assign(size_t(kLbugCount), Lbug{});
        frogs_.assign(size_t(kFrogCount), Frog{});
        clearHolds();

        ready_ = !ant_.empty() || !fly_.empty() || !lbug_.empty() || !frogHop_.empty() ||
                 !ffly_.empty();
        // ...AND THE GLOW IS ASKED THE SAME QUESTION. It was registered exactly
        // and the id kept, which is the assumption the wing disproved -- if the
        // firefly's yellow had snapped onto the butterflies' instead, every
        // butterfly in the wood would light up after dark.
        if (!ffly_.empty()) {
            // RESERVED FIRST, ASKED SECOND. Under exact-per-model the wood is
            // full by the time this runs, so the glow colour was refused into
            // the firefly's own private table -- which a lookup that must
            // return a GLOBAL id cannot see. Measured `firefly material 0`:
            // nothing glowed. World::reserveBehaviourMaterials mints it before
            // the band instead, and slot 3 is the glow.
            //
            // 0 there means nothing was reserved -- the old tolerance -- and
            // the search below is still the right answer.
            const uint8_t glowRes = world.behaviourMtl(3);
            if (glowRes != mat::AIR) {
                glowMtl_ = glowRes;
                glowShared_ = 1;
            } else {
                glowMtl_ = world.palette.resolveModelColor(
                    {glowWant_[0], glowWant_[1], glowWant_[2], 255}, Palette::kModelMatch,
                    &glowShared_);
            }
            // AN ID THAT MEANS "THIS GLOWS" MUST MEAN IT IN ONE PLACE ONLY --
            // it reaches the shader as V6Params::glowMtl and is tested against
            // the raw material id, which cannot know the arcade has a table of
            // its own. Reserved out of that table exactly as the emitters are;
            // see Particles::load, where the same collision lit up a cliff.
            world.noteHeldMtl(glowMtl_);
            std::printf("  firefly  %zu frames, %d slots, after dark only, material %u "
                        "(%d entr%s within tolerance -- 1 is private)\n",
                        ffly_.size(), kFireflyCount, unsigned(glowMtl_), glowShared_,
                        glowShared_ == 1 ? "y" : "ies");
        }
        if (!ant_.empty())
            std::printf("  ant      1 frame, %d in a column, %.2f m apart, all forests\n",
                        kAntCount, double(kAntGap));
        if (!fly_.empty()) {
            std::printf("  fly      %zu frames, %d slots, bunches of %d, wings solid white, "
                        "pine and oak\n",
                        fly_.size(), kHouseflyCount, kHouseflyPerBunch);
            // THE WING SWEEP, MEASURED OFF THE ART. Four poses that read
            // 2 / 1 / 0 / 1 are the housefly's wings going back, through, and
            // forward again; all-equal numbers mean the strip does not sweep
            // and the motion vector below costs nothing. See Frame::wingZ.
            std::printf("           wing z per pose:");
            for (const Frame &fr : fly_) std::printf(" %.2f", double(fr.wingZ));
            std::printf("  (voxels)\n");
        }
        if (!lbug_.empty())
            std::printf("  ladybug  %zu frames, %d slots, lands from %.1f m, all forests\n",
                        lbug_.size(), kLbugCount, double(kLbugDropMax));
        if (!frogHop_.empty())
            std::printf("  frog     %zu hop + %zu ribbet + %zu tongue frames, %d slots, "
                        "%.1f m of bank, birch and oak\n",
                        frogHop_.size(), frogRib_.size(), frogTon_.size(), kFrogCount,
                        double(kFrogShoreM));
        return ready_;
    }

    // -----------------------------------------------------------------------
    // ONE TICK. The predicates are handed in rather than reached for, exactly
    // as the marchers' are: `ground` is the walk height, `wet` answers a
    // question the height cannot (the bed of a lake is perfectly good floor),
    // and `birch` is which wood a point is in.
    // -----------------------------------------------------------------------
    // `water` is the SURFACE of any lake over this column, or a large negative
    // number where the ground is dry -- not a bool like `wet`, because a flyer
    // has to hold a line above it and a bool cannot say where that is. Defaulted
    // to nothing, so the asset deck and the offline report tick as they did.
    // -----------------------------------------------------------------------
    // WHAT ONE LADYBUG IS DOING, FOR --lbug-test.
    //
    // A rate change cannot be seen in a picture and cannot be read off a
    // position: it is a SPEED, and the only honest way to check it is to watch
    // one insect over many frames and divide. `cruising` is what makes the
    // sample valid -- a ladybug sitting on a stone has a speed of zero at every
    // distance, and averaging those in would hide the whole effect.
    // -----------------------------------------------------------------------
    bool lbugProbe(int i, bool *live, bool *cruising, Vec3 *at, float *frame) const {
        if (i < 0 || size_t(i) >= bugs_.size()) return false;
        const Lbug &b = bugs_[size_t(i)];
        *live = b.live;
        *cruising = b.ph == kCruise;
        *at = Vec3(b.x, b.y, b.z);
        *frame = b.frame;
        return true;
    }
    static int lbugCount() { return kLbugCount; }

    // -- `sand` IS THE BEACH GATE, AND Bunnies HAS TAKEN ONE ALL ALONG ----
    //
    // (user 2026-09-18: "keep life off the beaches/sand near water unless
    // otherwise said so".)
    //
    // The predicate already existed -- ForestApp::sandAt, the surface material
    // asked exactly the way the mesher asks it -- and Bunnies has been refusing
    // sand with it since the marchers landed. Critters never took one, which is
    // why the ants, the flies and the ladybugs were the population still on the
    // shore. Same callback, same signature, same source; nothing new is being
    // decided, it is being asked in one more place.
    void update(float dt, const Vec3 &player, const GroundF &ground, const WetF &wet,
                const BirchF &birch, const std::vector<Vec3> &banks,
                const std::vector<Solid> &solids, bool night = false,
                const Vec3 &look = Vec3(0.0f, 0.0f, 0.0f), const GroundF &water = GroundF(),
                WetF sand = nullptr) {
        if (!ready_) return;
        ground_ = ground;
        water_ = water;
        wet_ = wet;
        sand_ = std::move(sand);
        birch_ = birch;
        // BORROWED, NOT COPIED, and only for this call -- the same list the
        // perched birds and the marchers are handed. See Bunnies::update.
        solids_ = &solids;
        clock_ += dt;
        birth_.tick(dt, player.x, player.z, look.x, look.z);
        // A KILL IS GIVEN BACK ON THE RADIUS ITS SPECIES RECYCLES AT -- see
        // KillHold. The frog's is the lake's; everything else is kCritDropM.
        fireflyHold_.release(player.x, player.z, kCritDropM);
        antHold_.release(player.x, player.z, kCritDropM);
        flyHold_.release(player.x, player.z, kCritDropM);
        bugHold_.release(player.x, player.z, kCritDropM);
        frogHold_.release(player.x, player.z, kFrogDropM);

        night_ = night;
        if (!ffly_.empty()) { recycleFlies2(player, dt); if (night) fillFireflies(player); stepFireflies(dt); }
        if (!ant_.empty()) { recycleAnts(player, dt); fillAnts(player); stepAnts(dt); }
        if (!fly_.empty()) { recycleFlies(player, dt); fillFlies(player); stepFlies(dt); }
        if (!lbug_.empty()) { recycleBugs(player, dt); fillBugs(player); stepBugs(dt, player); }
        if (!frogHop_.empty()) { recycleFrogs(player, dt); fillFrogs(player, banks); stepFrogs(dt); }
    }

    // -----------------------------------------------------------------------
    // PUBLISH. One contiguous run, in the order the counts are declared, and
    // EVERY slot is written including the empty ones -- a slot nobody writes
    // keeps whatever was in it, which is the rule the whole band is built on.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        for (const Firefly &f : fireflies_)
            if (f.live) out->push_back({Vec3(f.x, f.y, f.z), "firefly", kCritBodyR, false});
        for (const Ant &a : ants_)
            if (a.live) out->push_back({Vec3(a.x, a.y + 0.05f, a.z), "ant", kCritBodyR, false});
        for (const Fly &f : flies_)
            if (f.live) out->push_back({Vec3(f.x, f.y, f.z), "fly", kCritBodyR, false});
        for (const Lbug &b : bugs_)
            if (b.live) out->push_back({Vec3(b.x, b.y, b.z), "ladybug", kCritBodyR, false});
        for (const Frog &f : frogs_)
            if (f.live) out->push_back({Vec3(f.x, f.y + 0.1f, f.z), "frog", kFrogBodyR, false});
    }

    void publish(World &world, int slot0) {
        if (!ready_) return;
        int s = slot0;
        // DARK FOR PART OF ITS CYCLE -- see kFireflyOnS.
        //
        // HIDDEN, NOT SKIPPED (user 2026-09-17: "when the firefly blinks, the
        // blinked spark voxel freezes in place"). Leaving the slot alone does
        // not turn it off -- an instance keeps whatever transform it last had,
        // so a firefly that stopped being placed stayed exactly where it was
        // and stopped moving, which is the report. The slot has to be told.
        //
        // THE SAME CALL place() MAKES FOR A DEAD BODY, so there is one way to
        // switch a flyer slot off rather than two.
        for (const Firefly &f : fireflies_) {
            if (f.live && f.blink >= kFireflyOnS)
                world.setFlyerInstance(s++, 0, nullptr, 0, 0, 0, nullptr, false);
            else
                place(world, s++, ffly_, f, int(f.frame), 0.0f);
        }
        for (const Ant &a : ants_) putAnt(world, s++, a);
        for (const Fly &f : flies_) putFly(world, s++, f);
        for (const Lbug &b : bugs_) putBug(world, s++, b);
        for (const Frog &g : frogs_) putFrog(world, s++, g);
        // The sixth system in this engine to need this line. Forget it and
        // nothing is drawn and nothing complains -- see birds.h.
        world.flushFlyerInstances();
    }

    void despawnAll() {
        for (Firefly &f : fireflies_) f = Firefly{};
        for (Ant &a : ants_) a = Ant{};
        for (Fly &f : flies_) f = Fly{};
        for (Lbug &b : bugs_) b = Lbug{};
        for (Frog &g : frogs_) g = Frog{};
        clearHolds();   // a new world, or the deck: nothing died in either
    }

    // ---- the offline report, and /locate ---------------------------------
    // -- IS THE COLUMN A COLUMN? -------------------------------------------
    //
    // Mean and worst gap between consecutive ants, measured along the trail's
    // own ORDER rather than by nearest neighbour -- a heap has short nearest
    // neighbours too. Returns how many links were measured; 0 means there is no
    // column to measure.
    int antSpacing(float *meanM, float *worstM) const {
        float sum = 0.0f, worst = 0.0f;
        int n = 0;
        const Ant *prev = nullptr;
        for (const Ant &a : ants_) {
            if (!a.live) continue;
            if (a.lead < 0) { prev = &a; continue; }   // the leader heads the line
            if (!prev) { prev = &a; continue; }
            const float dx = a.x - prev->x, dz = a.z - prev->z;
            const float d = sqrtf(dx * dx + dz * dz);
            sum += d;
            if (d > worst) worst = d;
            ++n;
            prev = &a;
        }
        if (meanM) *meanM = n ? sum / float(n) : 0.0f;
        if (worstM) *worstM = worst;
        return n;
    }

    // -- ARE THEY FLYING ALONE? --------------------------------------------
    //
    // The NEAREST-NEIGHBOUR distance, smallest and mean, over whichever
    // population is asked. A head-count cannot answer this and neither can a
    // screenshot taken from the wrong side: six ladybugs born on one cell and
    // six spread over a wood report the same six.
    template <class T>
    static int spreadOf(const std::vector<T> &pop, float *minM, float *meanM) {
        float sum = 0.0f, lo = 1e30f;
        int n = 0;
        for (size_t i = 0; i < pop.size(); ++i) {
            if (!pop[i].live) continue;
            float best = 1e30f;
            for (size_t j = 0; j < pop.size(); ++j) {
                if (i == j || !pop[j].live) continue;
                const float dx = pop[i].x - pop[j].x, dz = pop[i].z - pop[j].z;
                const float d2 = dx * dx + dz * dz;
                if (d2 < best) best = d2;
            }
            if (best > 1e29f) continue;
            const float d = sqrtf(best);
            sum += d;
            if (d < lo) lo = d;
            ++n;
        }
        if (minM) *minM = n ? lo : 0.0f;
        if (meanM) *meanM = n ? sum / float(n) : 0.0f;
        return n;
    }
    int bugSpread(float *minM, float *meanM) const { return spreadOf(bugs_, minM, meanM); }
    // The FLIES are meant to be close -- they are a bunch -- so what matters
    // here is the MINIMUM, which has to stay clear of the model's own 0.3 m or
    // two of them are drawn inside each other. See kHouseflyPhaseJit.
    int flySpread(float *minM, float *meanM) const { return spreadOf(flies_, minM, meanM); }
    int fireflySpread(float *minM, float *meanM) const {
        return spreadOf(fireflies_, minM, meanM);
    }

    int livingFireflies() const { return count(fireflies_); }
    // Where each lit one is, for the point-light publish -- a firefly that
    // wears the spark's voxel should light the air the way an ember does. See
    // App::publishSparkLights.
    bool fireflyAt(int i, Vec3 *out) const {
        if (i < 0 || i >= int(fireflies_.size())) return false;
        const Firefly &f = fireflies_[size_t(i)];
        // ...AND A DARK ONE LIGHTS NOTHING. The blink has to reach the light
        // list as well as the draw, or a firefly you cannot see still throws a
        // glow on the grass beneath it.
        if (!f.live || f.blink >= kFireflyOnS) return false;
        if (out) *out = Vec3(f.x, f.y, f.z);
        return true;
    }
    int fireflySlots() const { return int(fireflies_.size()); }
    // WHICH PALETTE ENTRY GLOWS, for App to hand the tracer. 0 until the
    // firefly has loaded, and 0 is mat::AIR -- which the shader reads as "no
    // glow" without a special case.
    uint8_t glowMtl() const { return glowMtl_; }
    // TRUE WHEN THE FIREFLY WEARS THE SPARK'S MATERIAL. The caller then leaves
    // V6Params::glowMtl OFF: the emitter table already lights that voxel, and
    // two mechanisms claiming one material means whichever the shader tests
    // first decides how bright every ember in the world is.
    bool sharesSpark() const { return sharesSpark_; }
    int livingAnts() const { return count(ants_); }
    int livingFlies() const { return count(flies_); }
    int livingBugs() const { return count(bugs_); }
    int livingFrogs() const { return count(frogs_); }
    // How many ladybugs are DOWN rather than cruising. A snapshot of a state
    // machine proves nothing on its own -- see Bees::modeShare -- so this is
    // reported beside the ticks below.
    int landedBugs() const {
        int n = 0;
        for (const Lbug &b : bugs_) n += (b.live && b.ph == kSit) ? 1 : 0;
        return n;
    }
    void frogCycleShare(long *out4) const {
        for (int k = 0; k < 4; ++k) out4[k] = frogTicks_[k];
    }
    void lbugPhaseShare(long *out3) const {
        for (int k = 0; k < 3; ++k) out3[k] = lbugTicks_[k];
    }

    bool nearestFirefly(const Vec3 &from, Vec3 *at, float *d) const {
        return nearest(fireflies_, from, at, d);
    }
    bool nearestAnt(const Vec3 &from, Vec3 *at, float *d) const { return nearest(ants_, from, at, d); }
    bool nearestFly(const Vec3 &from, Vec3 *at, float *d) const { return nearest(flies_, from, at, d); }
    bool nearestBug(const Vec3 &from, Vec3 *at, float *d) const { return nearest(bugs_, from, at, d); }
    bool nearestFrog(const Vec3 &from, Vec3 *at, float *d) const { return nearest(frogs_, from, at, d); }

    // -----------------------------------------------------------------------
    // THAT ONE IS DEAD -- this population's half of a kill. See the same method
    // in render/butterflies.h for the whole of the reasoning; `i` is the index
    // within THIS population's run of the instance band and App::killLifeAt
    // does the arithmetic.
    // -----------------------------------------------------------------------
    // FIVE POPULATIONS IN ONE RUN, in publish()'s own order -- which is the
    // order this walks, so the two cannot disagree about which slot is a frog.
    //
    // Each one HELD WHERE IT DIED, at its own species' index -- see KillHold in
    // core/noise.h. The housefly came straight back into its bunch on the next
    // tick, the same way the ant used to (see fillAnts).
    bool killSlot(int i) {
        if (i < 0) return false;
        if (size_t(i) < fireflies_.size()) {
            if (!fireflies_[size_t(i)].live) return false;
            fireflyHold_.hold(i, fireflies_[size_t(i)].x, fireflies_[size_t(i)].z);
            fireflies_[size_t(i)] = Firefly{};
            return true;
        }
        i -= int(fireflies_.size());
        if (size_t(i) < ants_.size()) {
            if (!ants_[size_t(i)].live) return false;
            antHold_.hold(i, ants_[size_t(i)].x, ants_[size_t(i)].z);
            ants_[size_t(i)] = Ant{};
            return true;
        }
        i -= int(ants_.size());
        if (size_t(i) < flies_.size()) {
            if (!flies_[size_t(i)].live) return false;
            flyHold_.hold(i, flies_[size_t(i)].x, flies_[size_t(i)].z);
            flies_[size_t(i)] = Fly{};
            return true;
        }
        i -= int(flies_.size());
        if (size_t(i) < bugs_.size()) {
            if (!bugs_[size_t(i)].live) return false;
            bugHold_.hold(i, bugs_[size_t(i)].x, bugs_[size_t(i)].z);
            bugs_[size_t(i)] = Lbug{};
            return true;
        }
        i -= int(bugs_.size());
        if (size_t(i) < frogs_.size()) {
            if (!frogs_[size_t(i)].live) return false;
            frogHold_.hold(i, frogs_[size_t(i)].x, frogs_[size_t(i)].z);
            frogs_[size_t(i)] = Frog{};
            return true;
        }
        return false;
    }

  private:
    struct Frame {
        int model = -1;
        int sx = 0, sy = 0, sz = 0;
        // -- WHERE THIS POSE PUTS THE WINGS, in voxels along the model's Z --
        //
        // Measured at load from the art rather than written down beside it, so
        // re-authoring the strip cannot leave this describing the old one --
        // the same rule V6Instance::flap states for the butterfly.
        //
        // A WING IS A VOXEL THAT IS NOT IN THE MIDDLE COLUMN. The housefly is
        // three across with the body at x = 1, and its four poses move the two
        // outer voxels to z = 2, 1, 0, 1. Species whose art does not do that
        // measure a constant here and a constant differences to zero, so they
        // pay nothing and say nothing.
        float wingZ = 0.0f;
    };

    // Everything alive carries these; the species add their own on top.
    struct Body {
        bool live = false;
        float x = 0, y = 0, z = 0;
        float th = 0;          // drawn yaw
        float age = 0, dying = -1.0f;
        int cx = 0, cz = 0;    // the lattice cell it claimed
        float frame = 0;
    };

    struct Firefly : Body {
        float vx = 0, vz = 0;
        float bob = 0;
        // Where in its own on/off cycle this one is -- see kFireflyOnS.
        // SEEDED AT BIRTH off the insect's site, never left at zero: nightfall
        // fills the whole population in one pass, so without a seed all six are
        // born in the same frame and blink in step for ever. See fillFireflies.
        float blink = 0;
    };

    struct Ant : Body {
        int head = 0;          // 0-3 into kAntDir; NEVER an angle
        float turnLock = 0;    // seconds left of the committed turn
        float whim = 0;        // when it next rolls for a corner
        int lead = -1;         // which ant leads this column; -1 = it is the leader
        int trail0 = 0;        // ring head, leaders only
    };

    struct Fly : Body {
        // The strip index that was DRAWN last frame, so putFly can difference
        // the wing positions. Kept beside `frame` and stepped with it.
        float framePrev = 0.0f;
        float ax = 0, ay = 0, az = 0;   // the bunch's anchor
        float ath = 0;                  // ...and the anchor's OWN heading
        float hx = 0, hz = 0;           // where the bunch formed, for the roam leash
        float ph = 0;                   // phase on the ring
        float rad = kHouseflyOrbitM;    // ...and its own radius on it
        int leadr = -1;                 // which fly owns the anchor
    };

    // Where fly `rank` of a bunch sits on the ring, and how far out. Evenly
    // spaced first and jittered second, so no two can ever draw the same angle
    // the way two hashes could -- see kHouseflyPhaseJit.
    static void ringSeat(Fly *f, int rank, uint32_t slot) {
        const float step = 6.2831853f / float(kHouseflyPerBunch);
        f->ph = float(rank) * step +
                (hashUnit(0x9Au, slot * 37u + 5u) - 0.5f) * 2.0f * kHouseflyPhaseJit;
        f->rad = kHouseflyOrbitM *
                 (1.0f + (hashUnit(0xA6u, slot * 53u + 11u) - 0.5f) * 2.0f * kHouseflyRingSpread);
    }

    enum LbugPhase { kCruise = 0, kDown, kSit };
    struct Lbug : Body {
        int ph = kCruise;
        float vx = 0, vz = 0;
        float t = 0;           // phase deadline
        float gy = 0;          // the ground it committed to
    };

    enum FrogCycle { kHop = 0, kRibbet, kTongue, kRotate };
    struct Frog : Body {
        int cyc = kRibbet;
        int head = 0;          // 0-3, cardinal like the ant
        int turn = 0;          // -1 / +1 for a rotate, applied at kFrogTurnFrame
        bool turned = false;
        float bx = 0, bz = 0;  // where the current cycle began
        float t = 0;
    };

    // ===================== FIREFLY ========================================
    //
    // NO WOOD GATE AND NO WATER GATE: "import the firefly and its mechanics to
    // all biomes", which is also what v1 does with it -- the flyer band's night
    // half is BIO_ANY and is thinned by nothing but the dark.
    void fillFireflies(const Vec3 &player) {
        for (size_t i = 0; i < fireflies_.size(); ++i) {
            Firefly &f = fireflies_[i];
            if (f.live || fireflyHold_.held(int(i))) continue;   // see KillHold
            float sx = 0, sz = 0;
            int cx = 0, cz = 0;
            if (!claim(fireflies_, fireflyHold_, kFireflyCellM, kFireflySalt, player, kWoodGreen, 0.0f,
                       &sx, &sz, &cx, &cz))
                break;
            f = Firefly{};
            f.live = true;
            f.cx = cx;
            f.cz = cz;
            f.x = sx;
            f.z = sz;
            f.y = flyFloor(sx, sz) + kFireflyCruiseM;
            const float a = hashUnit(kFireflySalt, hashU32(uint32_t(cx), uint32_t(cz))) * 6.2831853f;
            f.vx = sinf(a);
            f.vz = cosf(a);
            f.bob = hashUnit(0x2Fu, uint32_t(i) * 31u) * 6.2831853f;
            // -- ...AND NOT ALL AT ONCE ----------------------------------
            //
            // (user 2026-09-17: "fireflies seem to appear all at the same
            //  time. make it random across fireflies".)
            //
            // THE PHASE HAS TO BE SEEDED, NOT LEFT TO DRIFT. The first cut
            // started every insect at zero and relied on them being born at
            // different moments to spread out -- but nightfall fills the whole
            // population in ONE pass of this loop, so they are all born in the
            // same frame and then walked by the same dt for ever. Six lamps on
            // one switch. Seeded here they never agree in the first place.
            //
            // OFF ITS CELL, not off `i`: the slot index is recycled and would
            // hand the next tenant of a slot the same phase, while the cell is
            // the site this insect actually belongs to.
            f.blink = hashUnit(0x6Bu, hashU32(uint32_t(cx), uint32_t(cz)) + uint32_t(i)) *
                      (kFireflyOnS + kFireflyOffS);
        }
    }

    void stepFireflies(float dt) {
        for (size_t i = 0; i < fireflies_.size(); ++i) {
            Firefly &f = fireflies_[i];
            if (!f.live) continue;
            f.age += dt;
            // DAYBREAK RETIRES THEM, on the same fade every other critter uses
            // rather than by vanishing: a light that switches off mid-air is
            // the pop this engine keeps finding and fixing.
            if (!night_ && f.dying < 0.0f) f.dying = 0.0f;
            f.vx += (hashUnit(0xE1u, uint32_t(f.age * 37.0f) + uint32_t(i)) - 0.5f) *
                    kFireflyTurn * dt * 2.0f;
            f.vz += (hashUnit(0xE2u, uint32_t(f.age * 41.0f) + uint32_t(i)) - 0.5f) *
                    kFireflyTurn * dt * 2.0f;
            const float n = sqrtf(f.vx * f.vx + f.vz * f.vz);
            if (n > 1e-4f) { f.vx /= n; f.vz /= n; }
            f.bob += dt * 6.2831853f / kFireflyBobSec;
            // THE HEIGHT IS TAKEN AT THE DESTINATION, not at where it has got
            // to -- so the step and the line it lands on are one decision and
            // flySlide can refuse both together. Sampling the floor afterwards
            // is how a refused step still ends up at a refused height.
            const float fnx = f.x + f.vx * kFireflySpeed * dt;
            const float fnz = f.z + f.vz * kFireflySpeed * dt;
            // EASED FOR THE FLY'S OWN REASON -- see kHouseflyRiseRate. A
            // firefly drifts at 2.6 m/s over the same ten-centimetre staircase,
            // so it was juddering harder than the swarm was and only got away
            // with it by being a speck in the dark.
            const float fwant = flyFloor(fnx, fnz) + kFireflyCruiseM + sinf(f.bob) * kFireflyBobM;
            const float fny = f.y + (fwant - f.y) * (1.0f - expf(-kHouseflyRiseRate * dt));
            // A refused step is a TURN, or it pushes at the same face for ever.
            if (!flyTo(&f.x, &f.y, &f.z, fnx, fny, fnz, kCritBodyR)) {
                const float a = f.vx;
                f.vx = f.vz;
                f.vz = -a;
            }
            f.th = atan2f(f.vx, f.vz) + 3.14159265f;
            f.frame = fmodf(f.frame + kFireflyFps * dt, float(maxi(1, int(ffly_.size()))));
            f.blink = fmodf(f.blink + dt, kFireflyOnS + kFireflyOffS);
        }
    }

    void recycleFlies2(const Vec3 &p, float dt) { recycleRun(fireflies_, p, dt); }

    // ===================== ANT ============================================
    //
    // WHY THE LEADER IS A SLOT AND NOT A FLAG. Every ant in a column shares one
    // lattice cell, and the first one to claim it becomes the leader: the rest
    // adopt it by index. A follower that outlives its leader is retired with it
    // (see recycleAnts), because a column whose head has gone is four ants
    // walking a trail nobody is extending.
    void fillAnts(const Vec3 &player) {
        int leader = -1;
        for (size_t i = 0; i < ants_.size(); ++i)
            if (ants_[i].live && ants_[i].lead < 0) { leader = int(i); break; }

        for (size_t i = 0; i < ants_.size(); ++i) {
            Ant &a = ants_[i];
            if (a.live || antHold_.held(int(i))) continue;   // see KillHold
            if (leader >= 0) {
                // Joins the column that exists, at the back of the trail.
                const Ant &L = ants_[size_t(leader)];
                // -- ...AND A JOIN IS A BIRTH, WHICH HAS A FLOOR -------------
                //
                // (user 2026-09-17: "when attacking the ant, it does not die".)
                //
                // IT DID DIE. --kill-test has always killed an ant in one blow
                // and still does. What the player was watching was the
                // REPLACEMENT: this branch was the one spawn in the file that
                // never asked the birth gate, so a squashed follower was reborn
                // AT THE LEADER on the very next tick and stepAnts put it
                // straight back on the crumb trail at its rank -- a few
                // centimetres from the body, a sixtieth of a second later.
                // There is no way to read that except as a blow that did
                // nothing, and no amount of looking at the kill path would have
                // found it, because the kill path is right.
                //
                // THE GATE IS ASKED AT THE LEADER, because that is where this
                // ant appears -- not at the player, who is only the reference
                // point. Column formation is untouched: the leader's own claim
                // has already passed this same gate, so every follower it
                // gathers in that moment passes it too.
                if (!birth_.mayAt(L.x - player.x, L.z - player.z)) continue;
                // ...AND A COLUMN THAT LOST ANTS TO YOU STAYS SHORT while you
                // are there. The floor alone gave the dead ant back the moment
                // the column had walked thirty metres off. See KillHold.
                if (antHold_.within(L.x, L.z, kKillQuietM)) continue;
                // ...and not on a beach. Asked at the LEADER for the reason the
                // note above gives: that is where this ant appears, and every
                // follower it gathers has passed the same gate.
                if (sand_ && sand_(L.x, L.z)) continue;
                a = Ant{};
                a.live = true;
                a.lead = leader;
                a.head = L.head;
                a.x = L.x;
                a.y = L.y;
                a.z = L.z;
                // THE LEADER'S CELL, DELIBERATELY SHARED. It is what ties the
                // column to one lattice site so the whole of it recycles
                // together -- and `held` above only ever matters to a LEADER's
                // claim, which no follower makes.
                a.cx = L.cx;
                a.cz = L.cz;
                continue;
            }
            float sx = 0, sz = 0;
            int cx = 0, cz = 0;
            if (!claim(ants_, antHold_, kAntCellM, kAntSalt, player, kWoodGreen, 0.0f, &sx, &sz, &cx,
                       &cz))
                break;
            a = Ant{};
            a.live = true;
            a.lead = -1;
            a.cx = cx;
            a.cz = cz;
            a.x = sx;
            a.z = sz;
            a.y = groundAt(sx, sz);
            a.head = int(hashUnit(kAntSalt, hashU32(uint32_t(cx), uint32_t(cz))) * 4.0f) & 3;
            a.whim = kAntWhim;
            trail_.assign(size_t(kAntTrail) * 2, 0.0f);
            trailN_ = 0;
            trailLast_[0] = sx;
            trailLast_[1] = sz;
            leader = int(i);
        }
    }

    void stepAnts(float dt) {
        // -- THE LEADER WALKS, AND ONLY THE LEADER DECIDES ------------------
        for (size_t i = 0; i < ants_.size(); ++i) {
            Ant &a = ants_[i];
            if (!a.live || a.lead >= 0) continue;
            a.age += dt;
            a.turnLock = maxf(0.0f, a.turnLock - dt);
            a.whim -= dt;

            const float fx = float(kAntDir[a.head][0]), fz = float(kAntDir[a.head][1]);
            const bool ahead = antOK(a.x + fx * kAntLookM, a.z + fz * kAntLookM, a.y);
            bool turn = false;
            if (!ahead && a.turnLock <= 0.0f) {
                turn = true;
            } else if (a.whim <= 0.0f) {
                // An unblocked ant rolls for a corner every kAntWhim..2x, and
                // takes it on 35% of those rolls -- roughly a corner every
                // 6.4 s, which is about a hundred voxels of straight march.
                a.whim = kAntWhim * (1.0f + hashUnit(0x31u, uint32_t(a.age * 97.0f)));
                turn = hashUnit(0x77u, uint32_t(a.age * 131.0f) + 9u) < kAntWhimP;
            }
            if (turn && a.turnLock <= 0.0f) {
                const int side = (hashUnit(0x5Du, uint32_t(a.age * 211.0f)) < 0.5f) ? 1 : 3;
                const int want = (a.head + side) & 3;
                const float wx = float(kAntDir[want][0]), wz = float(kAntDir[want][1]);
                if (antOK(a.x + wx * kAntLookM, a.z + wz * kAntLookM, a.y)) a.head = want;
                else a.head = (a.head + (side == 1 ? 3 : 1)) & 3;   // the other way, then
                a.turnLock = kAntTurnHold;
            }
            const float hx = float(kAntDir[a.head][0]), hz = float(kAntDir[a.head][1]);
            if (antOK(a.x + hx * kAntLookM, a.z + hz * kAntLookM, a.y)) {
                a.x += hx * kAntSpeed * dt;
                a.z += hz * kAntSpeed * dt;
            }
            a.y = groundAt(a.x, a.z);
            // atan2(dx, dz) is the engine's yaw convention -- see LakeLife.
            a.th = atan2f(hx, hz) + 3.14159265f;
            dropCrumb(a.x, a.z);
        }
        // -- ...AND THE REST WALK HIS PATH ----------------------------------
        for (size_t i = 0; i < ants_.size(); ++i) {
            Ant &a = ants_[i];
            if (!a.live || a.lead < 0) continue;
            a.age += dt;
            int rank = 0;
            for (size_t j = 0; j < i; ++j)
                if (ants_[j].live && ants_[j].lead >= 0) ++rank;
            float px = 0, pz = 0;
            if (!crumbAt(kAntGap * float(rank + 1), &px, &pz)) continue;
            const float dx = px - a.x, dz = pz - a.z;
            if (dx * dx + dz * dz > 1e-6f) a.th = atan2f(dx, dz) + 3.14159265f;
            a.x = px;
            a.z = pz;
            a.y = groundAt(px, pz);
        }
    }

    bool antOK(float x, float z, float y) const {
        if (wet_ && wet_(x, z)) return false;
        const float g = groundAt(x, z);
        if (fabsf(g - y) > 0.45f) return false;   // the one-step rule the marchers walk by
        // ...AND A BOULDER IS AS GOOD A REASON TO TURN AS A LAKE. An ant walks
        // its 40 cm lookahead straight into a rock's skirt and the step rule
        // cannot see it: the TERRAIN either side of a rock is the same height,
        // which is what "the ground is flat here" means and is why it read as
        // clear ground all the way through the stone.
        return !inSolid(x, g + 0.05f, z, kCritBodyR);
    }

    // The leader's path, as a ring of crumbs kAntCrumb apart. A follower reads
    // it by ARC LENGTH, so it traces a corner instead of cutting it.
    void dropCrumb(float x, float z) {
        const float dx = x - trailLast_[0], dz = z - trailLast_[1];
        if (dx * dx + dz * dz < kAntCrumb * kAntCrumb) return;
        trailLast_[0] = x;
        trailLast_[1] = z;
        if (trail_.size() < size_t(kAntTrail) * 2) trail_.assign(size_t(kAntTrail) * 2, 0.0f);
        const size_t s = size_t(trailN_ % kAntTrail) * 2;
        trail_[s] = x;
        trail_[s + 1] = z;
        ++trailN_;
    }

    bool crumbAt(float back, float *x, float *z) const {
        const int have = trailN_ < kAntTrail ? trailN_ : kAntTrail;
        const int want = int(back / kAntCrumb);
        if (have <= 1 || want >= have) return false;
        const int idx = ((trailN_ - 1 - want) % kAntTrail + kAntTrail) % kAntTrail;
        *x = trail_[size_t(idx) * 2];
        *z = trail_[size_t(idx) * 2 + 1];
        return true;
    }

    // ===================== FLY ============================================
    void fillFlies(const Vec3 &player) {
        for (size_t i = 0; i < flies_.size(); ++i) {
            Fly &f = flies_[i];
            if (f.live || flyHold_.held(int(i))) continue;   // see KillHold
            // Join the emptiest bunch that has room before opening a new one --
            // v1's rule, and it is what stops a lone fly ever existing.
            int host = -1, hostN = kHouseflyPerBunch;
            for (size_t j = 0; j < flies_.size(); ++j) {
                if (!flies_[j].live || flies_[j].leadr >= 0) continue;
                // -- NOT A BUNCH YOU HAVE JUST SWATTED ONE OUT OF -----------
                //
                // THIS JOIN HAD NO GATE AT ALL, not even the floor the ant's
                // has: the dead fly's slot joined the emptiest bunch, which was
                // the one it had died in, on the very next tick, at a seat
                // beside the body. See KillHold.
                if (flyHold_.within(flies_[j].x, flies_[j].z, kKillQuietM)) continue;
                int n = 1;
                for (size_t k = 0; k < flies_.size(); ++k)
                    if (flies_[k].live && flies_[k].leadr == int(j)) ++n;
                if (n < hostN) { hostN = n; host = int(j); }
            }
            if (host >= 0) {
                const Fly &H = flies_[size_t(host)];
                f = Fly{};
                f.live = true;
                f.leadr = host;
                // THE LEADER'S CELL, DELIBERATELY SHARED -- see the ant's own
                // note. `held` only ever matters to the anchor's claim.
                f.cx = H.cx;
                f.cz = H.cz;
                ringSeat(&f, hostN, uint32_t(i));
                continue;
            }
            float sx = 0, sz = 0;
            int cx = 0, cz = 0;
            // PINE ONLY, as asked. v1 has the fly in its broadleaf wood; this
            // is the one species of the seven whose home the user MOVED.
            if (!claim(flies_, flyHold_, kHouseflyCellM, kHouseflySalt, player,
                       uint8_t(kWoodPine | kWoodOak), 0.0f, &sx, &sz, &cx, &cz))
                break;
            f = Fly{};
            f.live = true;
            f.leadr = -1;
            f.cx = cx;
            f.cz = cz;
            f.hx = f.ax = f.x = sx;
            f.hz = f.az = f.z = sz;
            f.ay = f.y = flyFloor(sx, sz) + kHouseflyCruiseM;
            f.ath = hashUnit(kHouseflySalt, hashU32(uint32_t(cx), uint32_t(cz))) * 6.2831853f;
            ringSeat(&f, 0, uint32_t(i));   // the anchor's owner takes seat zero
        }
    }

    void stepFlies(float dt) {
        for (size_t i = 0; i < flies_.size(); ++i) {
            Fly &f = flies_[i];
            if (!f.live || f.leadr >= 0) continue;
            f.age += dt;
            // The DRIFT owns its own heading. Integrating the wander on the
            // per-fly heading just re-draws the circle -- v1 measured exactly
            // that and the anchor never moved.
            f.ath += (hashUnit(0xB3u, uint32_t(f.age * 53.0f)) - 0.5f) * 2.0f * kHouseflyBunchTurn * dt;
            float dx = sinf(f.ath), dz = cosf(f.ath);
            const float lx = f.ax - f.hx, lz = f.az - f.hz;
            if (lx * lx + lz * lz > kHouseflyBunchRoamM * kHouseflyBunchRoamM) {
                const float il = 1.0f / sqrtf(lx * lx + lz * lz);
                dx -= lx * il;
                dz -= lz * il;
                const float n = sqrtf(dx * dx + dz * dz);
                if (n > 1e-4f) { dx /= n; dz /= n; }
            }
            // THE ANCHOR IS GUARDED AT THE RING'S OWN RADIUS, which is what
            // makes the whole bunch clear instead of five flies tested one at a
            // time: a swarm whose post is 65 cm from any stone has no member
            // inside it. This anchor is what the report was about -- it wandered
            // on pure noise, and groundAt put it a metre inside whatever was
            // standing on the terrain it sampled.
            const float anx = f.ax + dx * kHouseflyBunchSpeed * dt;
            const float anz = f.az + dz * kHouseflyBunchSpeed * dt;
            // ...AND THE HEIGHT IS APPROACHED, NEVER SNAPPED TO. See
            // kHouseflyRiseRate: the floor under a drifting post is a staircase
            // and writing it straight in is what made the swarm judder.
            const float want = flyFloor(anx, anz) + kHouseflyCruiseM;
            const float any = f.ay + (want - f.ay) * (1.0f - expf(-kHouseflyRiseRate * dt));
            if (!flyTo(&f.ax, &f.ay, &f.az, anx, any, anz, kHouseflyOrbitM))
                f.ath += 2.4f * dt;   // blocked: turn, the way the dragonfly does
            else
                f.ay = any;
        }
        for (size_t i = 0; i < flies_.size(); ++i) {
            Fly &f = flies_[i];
            if (!f.live) continue;
            const Fly &A = (f.leadr >= 0) ? flies_[size_t(f.leadr)] : f;
            if (f.leadr >= 0) f.age += dt;
            f.ph += kHouseflyOrbitW * dt;
            const float px = A.ax + cosf(f.ph) * f.rad;
            const float pz = A.az + sinf(f.ph) * f.rad;
            const float py = A.ay + sinf(f.ph * 1.7f) * kHouseflyOrbitY;
            // -- THE RING IS A TARGET IT FLIES AT, NOT A SEAT IT IS PUT IN ---
            //
            // The ring point moves at omega * rad = 1.2 m/s and the cap is
            // 3.2, so in the ordinary case the fly is exactly on it and this
            // changes nothing at all. What it changes is every case where it is
            // NOT on it -- a step the guard refused, a bunch whose post has
            // just been turned away from a trunk, a fly that has only just been
            // given its slot -- each of which used to be closed in one frame,
            // which is a teleport.
            const float ex = px - f.x, ey = py - f.y, ez = pz - f.z;
            const float e = sqrtf(ex * ex + ey * ey + ez * ez);
            // -- A JUMP THIS BIG IS A PLACEMENT, NOT A STEP -----------------
            //
            // AND LEAVING IT OUT PUT THE WHOLE SWARM UNDERGROUND. A follower is
            // created with its ring seat (its radius and its phase) and NOT
            // with a position -- the old code assigned it one on its first
            // frame, so it simply appeared on the ring. Tracking at 3.2 m/s
            // instead means it has to FLY there from wherever the zeroed struct
            // left it, which is y = 0: sixty metres below the pine floor, taking
            // twenty seconds, for every fly, every time one is recycled.
            //
            // --clip-test measured it at -21.70 m above ground and ten thousand
            // frames under the lake, one build after the smoothing went in.
            //
            // kHouseflyOrbitM is 0.65 and the ring point moves at 1.2 m/s, so
            // two metres is far outside anything the orbit can do in a frame
            // and safely inside "this fly is not on its ring at all yet".
            if (e > kHouseflySnapM) {
                f.x = px;
                f.y = py;
                f.z = pz;
                f.th = atan2f(-sinf(f.ph), cosf(f.ph)) + 3.14159265f;
                f.frame = fmodf(f.frame + kHouseflyFps * dt, float(maxi(1, int(fly_.size()))));
                // -- AND THE WING SWEEP IS CLEARED WITH IT ------------------
                //
                // THIS BRANCH ADVANCED THE FRAME AND LEFT framePrev BEHIND,
                // and putFly builds the wings' MOTION VECTOR out of exactly
                // that pair: sweep = wingZ[frame] - wingZ[framePrev]. So every
                // seated fly published a delta measured against whatever frame
                // it happened to hold last -- on a recycled slot, against a
                // DIFFERENT FLY's wings.
                //
                // A wrong motion vector is not a wrong pixel, it is a smear:
                // Ray Reconstruction is told the wings travelled somewhere
                // they did not and drags them there. It is the same fault the
                // flyers' turn vector had, and it looks the same.
                //
                // framePrev = frame AFTER the advance, not before: this fly
                // was just TELEPORTED onto its ring, so the honest report for
                // the frame is that nothing moved. One still frame of wing is
                // invisible at 24 fps; a smear is not.
                f.framePrev = f.frame;
                continue;   // seated this frame; nothing to track toward
            }
            const float cap = kHouseflyTrackM * dt;
            const float k = (e > cap && e > 1e-6f) ? (cap / e) : 1.0f;
            // ...AND THE HEADING IS THE ORBIT'S TANGENT, EASED. It used to be
            // atan2 of the frame's own position delta, so anything that moved
            // the fly by an unusual amount -- including the two smoothings
            // above -- span the model as well. The tangent is what the fly is
            // actually doing and is continuous by construction.
            const float tth = atan2f(-sinf(f.ph), cosf(f.ph)) + 3.14159265f;
            float dth = tth - f.th;
            while (dth > 3.14159265f) dth -= 6.2831853f;
            while (dth < -3.14159265f) dth += 6.2831853f;
            f.th += dth * (1.0f - expf(-kHouseflyYawRate * dt));
            flyTo(&f.x, &f.y, &f.z, f.x + ex * k, f.y + ey * k, f.z + ez * k, kCritBodyR);
            f.framePrev = f.frame;
            f.frame = fmodf(f.frame + kHouseflyFps * dt, float(maxi(1, int(fly_.size()))));
        }
    }

    // ===================== LADYBUG ========================================
    void fillBugs(const Vec3 &player) {
        for (size_t i = 0; i < bugs_.size(); ++i) {
            Lbug &b = bugs_[i];
            if (b.live || bugHold_.held(int(i))) continue;   // see KillHold
            float sx = 0, sz = 0;
            int cx = 0, cz = 0;
            if (!claim(bugs_, bugHold_, kLbugCellM, kLbugSalt, player, kWoodGreen, 0.0f, &sx, &sz, &cx,
                       &cz))
                break;
            b = Lbug{};
            b.live = true;
            b.cx = cx;
            b.cz = cz;
            b.x = sx;
            b.z = sz;
            b.y = flyFloor(sx, sz) + kLbugCruiseM;
            const float a = hashUnit(kLbugSalt, hashU32(uint32_t(cx), uint32_t(cz))) * 6.2831853f;
            b.vx = sinf(a);
            b.vz = cosf(a);
            b.ph = kCruise;
            b.t = kLbugTrySec;
        }
    }

    void stepBugs(float dt, const Vec3 &player) {
        for (size_t i = 0; i < bugs_.size(); ++i) {
            Lbug &b = bugs_[i];
            if (!b.live) continue;
            // -- HOW FAST THIS ONE IS LIVING -- see kLbugRateMul ------------
            //
            // HORIZONTAL, because the player's eye is over a metre above the
            // cruise lane and a 3D distance would hold a ladybug at your feet
            // outside its own near band.
            const float ldx = b.x - player.x, ldz = b.z - player.z;
            const float ld = sqrtf(ldx * ldx + ldz * ldz);
            const float u = (ld <= kLbugNearM)  ? 0.0f
                            : (ld >= kLbugFarM) ? 1.0f
                                                : (ld - kLbugNearM) / (kLbugFarM - kLbugNearM);
            const float ease = u * u * (3.0f - 2.0f * u);   // smoothstep, no step in the speed
            const float rate = kLbugRateMul + (1.0f - kLbugRateMul) * ease;
            // THE MOTION'S OWN dt. Everything below that MOVES the insect --
            // the flight, the drop, the wingbeat -- reads this one; the phase
            // clock below keeps the real dt, for the reason in the note.
            const float mdt = dt * rate;
            b.age += dt;
            b.t -= dt;
            ++lbugTicks_[b.ph];
            // THE ROCK'S TOP IS THE GROUND HERE, and saying that once at the
            // top of the tick settles both halves at once: the cruise line
            // rises over a boulder, and a descent aims at the boulder's top
            // instead of at terrain that is under three metres of stone. A
            // ladybug now lands ON the rock, which is also the right answer.
            const float g = flyFloor(b.x, b.z);
            if (b.ph == kCruise) {
                b.vx += (hashUnit(0xC1u, uint32_t(b.age * 41.0f) + uint32_t(i)) - 0.5f) * dt * 2.0f;
                b.vz += (hashUnit(0xC2u, uint32_t(b.age * 43.0f) + uint32_t(i)) - 0.5f) * dt * 2.0f;
                const float n = sqrtf(b.vx * b.vx + b.vz * b.vz);
                if (n > 1e-4f) { b.vx /= n; b.vz /= n; }
                const float bnx = b.x + b.vx * kLbugSpeed * mdt;
                const float bnz = b.z + b.vz * kLbugSpeed * mdt;
                // The altitude servo: it flies the butterfly's lane.
                const float bny = b.y + ((flyFloor(bnx, bnz) + kLbugCruiseM) - b.y) *
                                            mineF(1.0f, dt * 2.0f);
                if (!flyTo(&b.x, &b.y, &b.z, bnx, bny, bnz, kCritBodyR)) {
                    const float a = b.vx;
                    b.vx = b.vz;
                    b.vz = -a;
                }
                if (b.t <= 0.0f) {
                    b.t = kLbugTrySec;
                    // ONLY DOWN A COLUMN PROVEN CLEAR, and from no higher than
                    // kLbugDropMax. Anything else and it keeps flying.
                    if ((b.y - g) < kLbugDropMax && !(wet_ && wet_(b.x, b.z)) &&
                        columnClear(b.x, b.z, g, b.y)) {
                        b.ph = kDown;
                        b.gy = g;
                        b.t = kLbugDropSec;   // ...and it gives up if it has not arrived
                    }
                }
            } else if (b.ph == kDown) {
                b.y -= kLbugSpeed * 0.7f * mdt;
                if (b.y <= b.gy + 0.05f) {
                    b.y = b.gy + 0.05f;
                    b.ph = kSit;
                    b.t = kLbugSitSec + hashUnit(0xD4u, uint32_t(i) * 17u) * kLbugSitJit;
                } else if (b.t <= 0.0f) {
                    // Gave up on the way down -- back to the lane, and it may
                    // try again on the short clock rather than the long one.
                    b.ph = kCruise;
                    b.t = kLbugTrySec;
                }
            } else {
                // Off again, and it owes the air kLbugFlySec before it may
                // start looking for the next landing.
                if (b.t <= 0.0f) { b.ph = kCruise; b.t = kLbugFlySec; }
            }
            // -- ...AND IT DOES NOT WAIT FOR THAT CLOCK IF YOU ARE ON TOP OF IT
            //
            // See kLbugScareM. Anything not already cruising drops what it is
            // doing and goes, heading straight away from the player -- a bug
            // that took off on its old bearing would half the time fly at you,
            // which does not read as being scared off.
            //
            // IT OWES THE AIR THE FULL kLbugFlySec, the same debt an ordinary
            // take-off carries, so it cannot bounce: land, flee, re-land beside
            // your other foot. Reusing that constant rather than inventing a
            // flee time is deliberate -- there is one answer in this file to
            // "how long before it may look for the ground again".
            if (b.ph != kCruise) {
                if (ld < kLbugScareM) {
                    // ldx/ldz are BUG MINUS PLAYER, so away from the player
                    // is straight down them and no sign flip is wanted here.
                    const float n = maxf(1e-4f, ld);
                    b.vx = ldx / n;
                    b.vz = ldz / n;
                    b.ph = kCruise;
                    b.t = kLbugFlySec;
                }
            }
            if (b.vx * b.vx + b.vz * b.vz > 1e-6f) b.th = atan2f(b.vx, b.vz) + 3.14159265f;
            // -- FRAME 0 IS THE ONE THAT LANDS (user 2026-09-14) -----------
            //
            // "frame 0 of the ladybug.vox file is the frame that lands on the
            // ground." It is the wings-SHUT pose, and it was already held while
            // the insect SAT -- v1 holds it there for the same reason ("or the
            // body twitches on the spot"). What it was not held through was the
            // DESCENT, which is the part you actually watch: the ladybug came
            // down mid-flap and snapped its wings closed on touchdown.
            //
            // So the rule is the simpler one, and it reads better than the one
            // it replaces: it flaps in the AIR. Everything else -- committing
            // to a column, falling down it, sitting on it -- is frame 0.
            b.frame = (b.ph == kCruise)
                          ? fmodf(b.frame + kLbugFps * mdt, float(maxi(1, int(lbug_.size()))))
                          : 0.0f;
        }
    }

    // -----------------------------------------------------------------------
    // IS THERE ANYTHING BETWEEN THE FLYER AND THE FLOOR?
    //
    // Asked ONCE, at the moment of commitment, and never again -- which is the
    // whole reason a descent is safe to make at all: it suspends the altitude
    // servo, so if the answer changes afterwards the bug has no way to climb
    // back out.
    //
    // THE FIRST VERSION OF THIS ASKED THE GROUND, AND THE GROUND CANNOT ANSWER
    // IT. It walked the column comparing groundAt(x, z) against g -- the same
    // value, sampled twice, at the same (x, z) -- so it was constant-true and
    // every descent was "proven clear". Nothing about that is visible in a
    // render: the bug lands, which is what it is supposed to do, just also
    // under a canopy. The OFFLINE REPORT is what caught it, at 66% of ladybug
    // ticks spent sitting -- a flyer that is on the ground two thirds of the
    // time is not flying the butterfly's lane.
    //
    // THE TRUNKS ARE A SEPARATE LIST and always were. World::collidersNear is
    // where a tree exists as far as any animal in this engine is concerned --
    // the marchers walk by it and the perched birds sit in it -- and it is the
    // only thing that knows a crown is over this square metre.
    // -----------------------------------------------------------------------
    bool columnClear(float x, float z, float g, float y) const {
        if ((y - g) >= kLbugDropMax) return false;
        if (!solids_) return true;
        for (const Solid &s : *solids_) {
            // A ROCK IS NOT A CEILING. You can stand on one and so can a
            // ladybug; it is the TRUNKS, whose tops are a canopy twenty metres
            // up, that a descent must not be committed under. That is exactly
            // the distinction Solid::standable draws.
            if (s.standable) continue;
            const float dx = (x - s.cx) / maxf(0.05f, s.hx);
            const float dz = (z - s.cz) / maxf(0.05f, s.hz);
            if (dx * dx + dz * dz <= 1.0f) return false;
        }
        return true;
    }

    // ===================== FROG ===========================================
    void fillFrogs(const Vec3 &player, const std::vector<Vec3> &banks) {
        if (banks.empty()) return;
        for (size_t i = 0; i < frogs_.size(); ++i) {
            Frog &f = frogs_[i];
            if (f.live || frogHold_.held(int(i))) continue;   // see KillHold
            for (const Vec3 &b : banks) {
                const float ex = b.x - player.x, ez = b.z - player.z;
                const float d2 = ex * ex + ez * ez;
                if (d2 > kFrogSpawnM * kFrogSpawnM) continue;
                if (!birth_.mayAt(ex, ez)) continue;
                if (frogHold_.within(b.x, b.z, kKillQuietM)) continue;
                if (wet_ && wet_(b.x, b.z)) continue;   // ON the bank, never in it
                // BIRCH AND OAK (user 2026-09-17: "add the grass snake, frog, and
                // mouse to the oak forest"). This read `birchMix < 0.5`, which is
                // true everywhere in the oak band, so the frog was refused there --
                // see VoxelTerrain::woodBit. v1 agrees: its frog is BIO_OAKF, and
                // BIO_OAKF means EITHER broadleaf band.
                // BIRCH AND OAK, AND NOT THE BLOSSOM. kWoodBroad includes the
                // cherry band -- see kWoodBroadGreen -- so the frog was in it
                // by inclusion, on a shore under trees it has no business
                // under. "only the worm, pink bird, flamingos and pink
                // butterflies should be in the cherry forest."
                if (birch_ && !(birch_(b.x) & kWoodBroadGreen)) continue;
                // ...AND NOT UP AGAINST A TRUNK. A bank spot is chosen from
                // the shoreline and the shoreline runs right past the trees on
                // it. The hop guard cannot help here -- it refuses a leap INTO
                // a trunk and a frog that was put in one has not leapt.
                if (inSolidBox(b.x, b.z, groundAt(b.x, b.z) + 0.02f,
                               groundAt(b.x, b.z) + kFrogTallM, kFrogBodyR))
                    continue;
                // ...AND NOT ON TOP OF THE OTHER ONE. DES_WATER_APART: a bank
                // is a narrow thing and two frogs will find the same metre of it.
                bool crowd = false;
                for (const Frog &o : frogs_)
                    if (o.live) {
                        const float dx = o.x - b.x, dz = o.z - b.z;
                        if (dx * dx + dz * dz < kFrogApartM * kFrogApartM) crowd = true;
                    }
                if (crowd) continue;
                f = Frog{};
                f.live = true;
                f.x = f.bx = b.x;
                f.z = f.bz = b.z;
                f.y = groundAt(b.x, b.z);
                f.head = int(hashUnit(kFrogSalt, hashU32(uint32_t(b.x * 4.0f),
                                                        uint32_t(b.z * 4.0f))) * 4.0f) & 3;
                pickCycle(&f, uint32_t(i));
                break;
            }
        }
    }

    void stepFrogs(float dt) {
        for (size_t i = 0; i < frogs_.size(); ++i) {
            Frog &f = frogs_[i];
            if (!f.live) continue;
            f.age += dt;
            ++frogTicks_[f.cyc];
            const int n = cycleLen(f.cyc);
            f.frame += kFrogFps * dt;
            const int fi = int(f.frame);

            // THE 90 DEGREES GOES IN AT THE TOP OF THE LEAP. The frog is
            // airborne on frame 8, so the pivot has nothing to scrape against
            // and it reads as the animal turning in the air.
            if (f.cyc == kRotate && !f.turned && fi >= kFrogTurnFrame) {
                f.head = (f.head + (f.turn > 0 ? 1 : 3)) & 3;
                f.turned = true;
            }
            if (fi >= n) {
                // The cycle is over: the travel it carried is now where the
                // frog stands, and the next one starts from there.
                f.bx = f.x;
                f.bz = f.z;
                pickCycle(&f, uint32_t(i));
                continue;
            }
            // ...AND THE TRAVEL IS THE TABLE. Only the hop carries the frog
            // forward; the rotate keeps its rise and lands on its own square.
            float up = 0.0f, fwd = 0.0f;
            if (f.cyc == kHop || f.cyc == kRotate) {
                const int k = mini(kFrogHopFrames - 1, fi);
                up = kFrogHop[k][0] * VOXEL_M;
                fwd = (f.cyc == kHop) ? -kFrogHop[k][1] * VOXEL_M : 0.0f;
            }
            const float hx = float(kAntDir[f.head][0]), hz = float(kAntDir[f.head][1]);
            const float nx = f.bx + hx * fwd, nz = f.bz + hz * fwd;
            // A leap that would land in the lake is simply not taken: the frog
            // sits on the bank, which is the whole of what DES_WATER means.
            // A leap INTO A ROCK is refused by the same line and for the same
            // reason -- a hop is a whole cycle's travel replayed from f.bx, so
            // refusing it leaves the frog on its own square rather than part
            // way into the stone.
            const bool wetAt = wet_ && wet_(nx, nz);
            const float ng = groundAt(nx, nz);
            if (!wetAt && !inSolidBox(nx, nz, ng + 0.02f, ng + kFrogTallM, kFrogBodyR)) {
                f.x = nx;
                f.z = nz;
            }
            f.y = groundAt(f.x, f.z) + up;
            f.th = atan2f(hx, hz) + 3.14159265f;
        }
    }

    void pickCycle(Frog *f, uint32_t seed) {
        int sum = 0;
        for (int k = 0; k < 4; ++k) sum += kFrogMix[k];
        // OVER THE WHOLE TABLE. v1 added three literal entries by hand, so its
        // fourth cycle was listed, weighted, and never once came up -- measured
        // at zero rotates in ninety seconds.
        const float r = hashUnit(0x6Bu, hashU32(seed + 11u, uint32_t(f->age * 97.0f))) * float(sum);
        int acc = 0, pick = 0;
        for (int k = 0; k < 4; ++k) {
            acc += kFrogMix[k];
            if (r < float(acc)) { pick = k; break; }
        }
        f->cyc = pick;
        f->frame = 0.0f;
        f->turned = false;
        f->turn = (hashUnit(0x8Eu, hashU32(seed + 23u, uint32_t(f->age * 131.0f))) < 0.5f) ? 1 : -1;
    }

    int cycleLen(int cyc) const {
        if (cyc == kRibbet) return int(frogRib_.empty() ? frogHop_.size() : frogRib_.size());
        if (cyc == kTongue) return int(frogTon_.empty() ? frogHop_.size() : frogTon_.size());
        return int(frogHop_.size());
    }

    const std::vector<Frame> &frogStrip(int cyc) const {
        if (cyc == kRibbet && !frogRib_.empty()) return frogRib_;
        if (cyc == kTongue && !frogTon_.empty()) return frogTon_;
        return frogHop_;
    }

    // ===================== shared =========================================
    //
    // ONE CLAIM FOR ALL OF THEM, on the population's own lattice and in HASH
    // ORDER rather than nearest-first. Nearest-first is what put every
    // population in this engine inside nine metres of the player on a
    // ninety-six metre disc, which reads as "the life clusters around you on
    // spawn and is sparse further out" -- see siteOrder in core/noise.h.
    //
    // -- AND IT MUST SKIP THE CELLS ITS OWN KIND ALREADY HOLD ---------------
    //
    // This had no such test when it was written, and the result was reported as
    // "you have ladybugs flying together in a pack. have them fly alone". They
    // were not flocking -- there is no flocking code here at all. HASH ORDER IS
    // DETERMINISTIC, so six empty slots filling on one frame all walked the
    // same cells in the same order, all found the same lowest-hash cell, and
    // all were born at the same point. Six bodies at one site look exactly like
    // a swarm and are actually a missing line.
    //
    // The marchers have had `skunkClaimed` since they were written and this is
    // the same test, taken over whichever population is asking. The ant and the
    // fly are unaffected in the way that matters: only a column's LEADER and a
    // bunch's ANCHOR ever call this, and those two SHOULD take separate cells.
    template <class T>
    // `woods` is a set of kWood* bits, or kWoodAll for "anywhere" -- which is
    // not what a forest creature means and no longer what it says: see
    // kWoodForest, and the five rabbits that were found in Death Valley.
    bool claim(const std::vector<T> &pop, const KillHold &kills, float cellM, uint32_t salt,
               const Vec3 &player, uint8_t woods,
               float shoreM, float *ox, float *oz, int *ocx, int *ocz) {
        const int r = int(kCritSpawnM / cellM) + 1;
        const int c0x = int(floorf(player.x / cellM));
        const int c0z = int(floorf(player.z / cellM));
        float best = 2.0f;
        bool found = false;
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const int cx = c0x + dx, cz = c0z + dz;
                if (held(pop, cx, cz)) continue;
                float sx = 0, sz = 0;
                siteOf(cellM, salt, cx, cz, &sx, &sz);
                const float ex = sx - player.x, ez = sz - player.z;
                const float d2 = ex * ex + ez * ez;
                if (d2 > kCritSpawnM * kCritSpawnM) continue;
                const float ord = siteOrder(salt, cx, cz);
                if (ord >= best) continue;
                if (!birth_.mayAt(ex, ez)) continue;
                // NOT WHERE ONE WAS JUST KILLED -- see KillHold.
                if (kills.within(sx, sz, kKillQuietM)) continue;
                if (woods != kWoodAll && birch_ && !(birch_(sx) & woods)) continue;
                if (wet_ && wet_(sx, sz)) continue;   // never IN the water
                // ...NOR ON THE BEACH BESIDE IT. See the note over update().
                // The frog is deliberately exempt -- it is a bank animal and is
                // placed from its own shoreline list below, which never comes
                // through here.
                if (sand_ && sand_(sx, sz)) continue;
                // ...NOR INSIDE A TREE OR A ROCK. A site is a point on a
                // lattice and nothing about the lattice avoids the wood, so one
                // site in the body of a boulder is a creature that spends its
                // whole life in the stone -- a guard on MOVEMENT never fires
                // for something that was already there.
                if (inSolid(sx, groundAt(sx, sz) + 0.3f, sz)) continue;
                if (shoreM > 0.0f && !shoreNear(sx, sz, shoreM)) continue;
                best = ord;
                found = true;
                *ox = sx;
                *oz = sz;
                *ocx = cx;
                *ocz = cz;
            }
        return found;
    }

    // -----------------------------------------------------------------------
    // A COLOUR NOTHING ELSE IS NEAR, so the entry it mints is its own.
    //
    // THIS IS THE ONLY WAY TO GET A PRIVATE MATERIAL out of a palette that
    // snaps. Registering a colour "exactly" does not do it: exact stores under
    // a key of its own, while the MODEL's voxels come back through the snap,
    // and the snap will hand them any entry within Palette::kModelMatch --
    // whichever it reaches first. Ask first, and mint a colour that has no
    // neighbours at all, and the snap has nowhere else to go.
    //
    // Walks the candidates in order and takes the first with NOTHING within
    // tolerance. Falls back to the last one, and the caller's report prints how
    // many entries ended up sharing -- so a palette that has filled in around
    // every candidate says so rather than quietly going wrong.
    static const uint8_t *privateTint(const World &world, const uint8_t (*cand)[3], int n) {
        for (int k = 0; k < n; ++k) {
            int near9 = 0;
            world.palette.resolveModelColor({cand[k][0], cand[k][1], cand[k][2], 255},
                                            Palette::kModelMatch, &near9);
            if (near9 == 0) return cand[k];
        }
        return cand[n - 1];
    }

    // Is one of ours already on this cell? A body that is FADING still holds
    // its cell: it is visibly there, and handing the cell over would put its
    // replacement inside it.
    template <class T>
    static bool held(const std::vector<T> &pop, int cx, int cz) {
        for (const T &b : pop)
            if (b.live && b.cx == cx && b.cz == cz) return true;
        return false;
    }

    // Wet ground within `r`, asked on a ring of eight. The frog is the only
    // caller: v1 gives it a SHORE radius rather than a wet test precisely
    // because the animal sits beside the water and not in it.
    bool shoreNear(float x, float z, float r) const {
        if (!wet_) return false;
        for (int k = 0; k < 8; ++k) {
            const float a = float(k) * 0.7853982f;
            if (wet_(x + cosf(a) * r, z + sinf(a) * r)) return true;
        }
        return false;
    }

    template <class T>
    void recycleRun(std::vector<T> &v, const Vec3 &player, float dt) {
        for (T &b : v) {
            if (!b.live) continue;
            if (b.dying >= 0.0f) {
                b.dying += dt;
                if (b.dying >= kCritFadeSec) b = T{};
                continue;
            }
            const float dx = b.x - player.x, dz = b.z - player.z;
            if (dx * dx + dz * dz > kCritDropM * kCritDropM) b.dying = 0.0f;
        }
    }

    void recycleAnts(const Vec3 &p, float dt) {
        recycleRun(ants_, p, dt);
        // A column whose leader has gone is a column walking a trail nobody is
        // extending, so the followers go with him.
        bool haveLead = false;
        for (const Ant &a : ants_)
            if (a.live && a.lead < 0 && a.dying < 0.0f) haveLead = true;
        if (!haveLead)
            for (Ant &a : ants_)
                if (a.live && a.lead >= 0 && a.dying < 0.0f) a.dying = 0.0f;
    }
    void recycleFlies(const Vec3 &p, float dt) {
        recycleRun(flies_, p, dt);
        for (Fly &f : flies_)
            if (f.live && f.leadr >= 0 && f.dying < 0.0f &&
                (!flies_[size_t(f.leadr)].live || flies_[size_t(f.leadr)].dying >= 0.0f))
                f.dying = 0.0f;
    }
    void recycleBugs(const Vec3 &p, float dt) { recycleRun(bugs_, p, dt); }
    // ...ON THE LAKE'S RADIUS. See kFrogDropM: a frog belongs to a body of
    // water and walking twenty metres from it must not delete the frog.
    void recycleFrogs(const Vec3 &player, float dt) {
        for (Frog &f : frogs_) {
            if (!f.live) continue;
            if (f.dying >= 0.0f) {
                f.dying += dt;
                if (f.dying >= kCritFadeSec) f = Frog{};
                continue;
            }
            const float dx = f.x - player.x, dz = f.z - player.z;
            if (dx * dx + dz * dz > kFrogDropM * kFrogDropM) f.dying = 0.0f;
        }
    }

    float groundAt(float x, float z) const { return ground_ ? ground_(x, z) : 0.0f; }


    // -----------------------------------------------------------------------
    // WHAT A SMALL ANIMAL MAY NOT BE INSIDE.
    //
    // THE SAME LIST, THE SAME TEST AND THE SAME VOXELS AS THE PLAYER, the
    // bunnies and the butterflies -- see solidsContain in scene/collide.h.
    // `solids_` has been handed to this class all along and exactly one caller
    // read it: columnClear, for the ladybug's descent, which deliberately
    // ignores rocks. So every species in this file flew or crawled through
    // boulders, and the report -- "the flys were caught flying inside a big
    // rock" -- is one sighting of five bugs.
    //
    // WHY A ROCK AND NOT A TREE IS THE HARD CASE. A trunk is thin and a flyer
    // that never turns still mostly misses one; a boulder is five metres wide
    // and sits exactly in the lane these insects hold. And it is not entered by
    // flying into it -- see flyFloor.
    // -----------------------------------------------------------------------
    bool inSolid(float x, float y, float z, float r = 0.0f) const {
        if (!solids_ || solids_->empty()) return false;
        return solidsTouch(solids_->data(), int(solids_->size()), x, y, z, r, VOXEL_M);
    }

    // THE FLOOR A FLYER HOLDS ITS LINE OVER: the ground where there is nothing
    // on it, the top of the rock where there is.
    //
    // THIS IS THE HALF OF THE FIX THAT MATTERS MOST, and it is not a collision
    // test at all. `ground_` is the TERRAIN height field, which does not know a
    // model exists -- so a fly asked to hang 1.1 m over the ground hangs 1.1 m
    // over the ground UNDERNEATH a five-metre boulder, which is a metre inside
    // the stone. It never took a step that could have been refused. It was born
    // at that height and held it.
    float flyFloor(float x, float z) const {
        float g = groundAt(x, z);
        // -- AND A LAKE HAS A TOP (user 2026-09-14: "a lady bug was caught
        //    swimming in water") -------------------------------------------
        //
        // `ground_` is the top of the SOLID column, which over a lake is the
        // BED. A ladybug holding its 1.4 m lane over that is 1.4 m above the
        // bottom of the lake -- under the surface of anything deeper than that,
        // which is most of one. It was not swimming; it was flying, at the
        // height it was told to.
        //
        // THE LANDING TEST WAS ALREADY RIGHT and that is what hid this: the
        // descent refuses a wet column (see kCruise below), so nothing ever
        // landed in water and the only way in was the cruise line itself.
        if (water_) {
            const float t = water_(x, z);
            if (t > g) g = t;
        }
        if (!solids_ || solids_->empty()) return g;
        return solidsFloor(solids_->data(), int(solids_->size()), x, z, VOXEL_M, g, kCritRiseM);
    }

    // -----------------------------------------------------------------------
    // THE WHOLE COLUMN A BODY OCCUPIES, NOT ONE POINT IN IT.
    //
    // A FROG IS NOT WHERE ITS FEET ARE. It leaps half a metre into the air and
    // a metre forward, and the guard on that leap tested one point a hand's
    // breadth off the ground -- so it asked whether the frog's ankles would
    // clear the trunk and never asked about the rest of the animal. Seven
    // frames a minute of a frog passing through bark, its own position clear
    // every time, which is what a point test reports when the body is not a
    // point.
    //
    // solidBoxOverlap is the exact answer where a model carries its volume --
    // one range over the grid, cheap because its early rejections throw out
    // every model the body is not near -- and the point test is kept for the
    // handful that do not, exactly as solidsContain does.
    // -----------------------------------------------------------------------
    bool inSolidBox(float x, float z, float y0, float y1, float r) const {
        if (!solids_ || solids_->empty()) return false;
        for (const Solid &sd : *solids_) {
            if (sd.vol) {
                if (solidBoxOverlap(sd, x, y0, z, y1, r, r, VOXEL_M)) return true;
                continue;
            }
            if (sd.hx <= 0.0f || sd.hz <= 0.0f || y0 > sd.top) continue;
            const float dx = (x - sd.cx) / (sd.hx + r), dz = (z - sd.cz) / (sd.hz + r);
            if (dx * dx + dz * dz < 1.0f) return true;
        }
        return false;
    }

    // Move, slide, or stay -- see flySlide. True if the whole step was taken.
    bool flyTo(float *x, float *y, float *z, float nx, float ny, float nz,
               float r = 0.0f) const {
        if (!solids_ || solids_->empty()) {
            *x = nx; *y = ny; *z = nz;
            return true;
        }
        return flySlide(solids_->data(), int(solids_->size()), VOXEL_M, r, x, y, z, nx, ny, nz);
    }

    static float mineF(float a, float b) { return a < b ? a : b; }

    template <class T>
    static int count(const std::vector<T> &v) {
        int n = 0;
        for (const T &b : v) n += b.live ? 1 : 0;
        return n;
    }

    template <class T>
    static bool nearest(const std::vector<T> &v, const Vec3 &from, Vec3 *at, float *d) {
        float best = 1e30f;
        bool got = false;
        for (const T &b : v) {
            if (!b.live) continue;
            const float dx = b.x - from.x, dz = b.z - from.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 >= best) continue;
            best = d2;
            got = true;
            *at = Vec3(b.x, b.y, b.z);
        }
        if (got && d) *d = sqrtf(best);
        return got;
    }

    // ---- drawing --------------------------------------------------------
    static float fadeOf(float dying) {
        const float out = dying >= 0.0f ? saturate(1.0f - dying / kCritFadeSec) : 1.0f;
        return kCritFadeMin + (1.0f - kCritFadeMin) * out;
    }

    // -----------------------------------------------------------------------
    // `onGround` -- b.y IS THE FLOOR IT STANDS ON, NOT THE MIDDLE OF ITS BOX.
    //
    // THIS IS WHY THE ANTS WERE HALF IN THE SAND. Every caller here sets y from
    // the ground -- `a.y = groundAt(a.x, a.z)` -- and this function then centred
    // the model's BOX on that point, so exactly half of every ant, frog and
    // landed ladybug was under the surface. It is invisible on a flyer, which
    // is what the seat was written for, and unmissable on anything that walks.
    //
    // ...AND IT IS ALSO WHY A LADYBUG LANDED AT THE WRONG HEIGHT. Its frames
    // are not all the same height -- frame 00 is the wings-SHUT pose -- so a
    // centred seat moves the animal vertically every time the strip advances,
    // and the touchdown frame is the one that shows it. v1 hit this on the frog
    // and wrote the answer down: "the seat has to be PER-FRAME, because the box
    // is", and the seat that reproduces its editor is the model's own row zero
    // on the floor. That is what this is.
    //
    // THE FADE STILL SHRINKS IT TOWARD ITS FEET rather than toward its middle,
    // which is the right way for a thing standing on something to leave.
    // -----------------------------------------------------------------------
    // `zSweep` is how far this pose's wings moved along the model's own Z
    // since the pose drawn last frame, in METRES. Zero for everything that
    // does not sweep, which is everything but the housefly.
    void place(World &world, int slot, const std::vector<Frame> &strip, const Body &b, int fi,
               float extraYaw, bool onGround = false, float zSweep = 0.0f) const {
        if (!b.live || strip.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const Frame &fr = strip[size_t(maxi(0, mini(int(strip.size()) - 1, fi)))];
        const float k = fadeOf(b.dying);
        const float th = b.th + extraYaw;
        const float c = cosf(th) * k, s = sinf(th) * k;
        const float m[9] = {c, 0.0f, s, 0.0f, k, 0.0f, -s, 0.0f, c};
        const float hx = 0.5f * float(fr.sx) * VOXEL_M;
        const float hy = 0.5f * float(fr.sy) * VOXEL_M;
        const float hz = 0.5f * float(fr.sz) * VOXEL_M;
        const float ox = m[0] * hx + m[1] * hy + m[2] * hz;
        const float oy = m[3] * hx + m[4] * hy + m[5] * hz;
        const float oz = m[6] * hx + m[7] * hy + m[8] * hz;
        // THE ANIMAL, NOT ITS BOX -- the motion vector is anchored to the body,
        // never to the corner of the pose it happens to be wearing. See
        // World::place, and the third bird flicker that taught it.
        const float anchor[3] = {b.x, b.y, b.z};
        // THE WING'S OWN STEP, which no transform describes: the pose changed
        // underneath one that did not. flap.x is the step for the voxels one
        // bucket out from the middle -- the wings -- and flap.z is the model's
        // centre along its own X, which is what the tracer buckets against.
        // Times the fade, because the instance is drawn at that scale.
        const float sweep[3] = {zSweep * k, 0.0f, 0.5f * float(fr.sx) * VOXEL_M};
        const bool sweeping = zSweep != 0.0f;
        world.setFlyerInstance(slot, fr.model, m, b.x - ox, onGround ? b.y : (b.y - oy),
                               b.z - oz, sweeping ? sweep : nullptr, true, nullptr, anchor,
                               sweeping);
    }

    void putAnt(World &w, int s, const Ant &a) const { place(w, s, ant_, a, 0, 0.0f, true); }
    void putFly(World &w, int s, const Fly &f) const {
        const int cur = int(f.frame), was = int(f.framePrev);
        const float sweep =
            (fly_.empty() || cur == was)
                ? 0.0f
                : (fly_[size_t(mini(int(fly_.size()) - 1, cur))].wingZ -
                   fly_[size_t(mini(int(fly_.size()) - 1, was))].wingZ) *
                      VOXEL_M;
        place(w, s, fly_, f, cur, 0.0f, false, sweep);
    }
    // ...AND THE LADYBUG IS BACKWARDS. v1 keeps a table for this
    // (DES_BACKWARDS): its head is at +y where a baked species' is at -y, so
    // its yaw carries a half turn the others do not.
    void putBug(World &w, int s, const Lbug &b) const {
        place(w, s, lbug_, b, int(b.frame), 3.14159265f, true);
    }
    void putFrog(World &w, int s, const Frog &f) const {
        place(w, s, frogStrip(f.cyc), f, int(f.frame), 0.0f, true);
    }

    // ---- loading --------------------------------------------------------
    // `repaint`, when given, is SIX bytes -- a from-colour and a to-colour --
    // and rewrites that one palette entry of the loaded models before they are
    // registered. Two models want it: the fly, whose wings must own a material
    // and cannot while they are the same white as half the world, and the
    // firefly, whose lamp must own one for the same reason. Done to the MODEL
    // rather than to the table, so the colour the art carries and the colour
    // the material wears are one fact rather than two that have to agree.
    // `matchTol` is handed on to addFlyerModel. Zero means MINT EXACTLY, and
    // the firefly is the one caller that needs it: its glow has to be an id
    // nothing else in the world wears -- see the resolve in load(), and
    // [[v2-private-material]]. Everything else on this path is an ordinary
    // animal and takes the folding default.
    void loadStrip(World &world, const std::string &dir, int frames, const char *what,
                   std::vector<Frame> *out, const uint8_t *repaint = nullptr,
                   int matchTol = World::kLifeMatch) {
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
        if (repaint)
            for (VoxModel &m : mo)
                for (auto &e : m.pal)
                    if (e[0] == repaint[0] && e[1] == repaint[1] && e[2] == repaint[2]) {
                        e[0] = repaint[3];
                        e[1] = repaint[4];
                        e[2] = repaint[5];
                    }
        adopt(world, mo, what, out, matchTol);
    }

    // ONE FILE, EVERY MODEL IN IT -- the koi's reader. ladybug.vox is keyframed
    // onto a single shape node, so its six frames are the file's own models.
    void loadWhole(World &world, const std::string &path, const char *what,
                   std::vector<Frame> *out) {
        std::vector<VoxModel> mo;
        std::string err;
        if (!voxLoadAll(path, &mo, &err)) {
            std::fprintf(stderr, "v2: %s %s: %s -- skipped\n", what, path.c_str(), err.c_str());
            return;
        }
        adopt(world, mo, what, out);
    }

    // ...AND ONE FILE, ONE CYCLE OUT OF IT.
    //
    // voxLoadAll IS THE WRONG READER FOR THE FROG and the reason is worth
    // writing down: frog.vox has 37 MODEL chunks and 55 shape REFERENCES,
    // because its three cycles share frames. voxLoadAll returns the 37 unique
    // models with nothing to say which cycle wanted which, so the hop cannot be
    // recovered from it at all. The scene keeps the shape a piece came from --
    // that is what Piece::shape is for, and the bow's draw strip is built out
    // of the same field.
    // -----------------------------------------------------------------------
    // ...AND `shape < 0` MEANS EVERY PIECE, IN THE ORDER THE FILE ANIMATES
    // THEM, which is a different order from the one voxLoadAll returns.
    //
    // (user 2026-09-14: "frame 0 of the ladybug.vox file is the frame that
    // lands on the ground" -- twice, because the first fix was to the wrong
    // half.)
    //
    // MEASURED, and the numbers are the whole argument. ladybug.vox holds six
    // frames and the two readers disagree about what order they are in:
    //
    //     voxLoadAll (MODEL chunks)      8x4  4x5  6x5  8x4  6x5  4x5
    //     voxLoadScene (SHAPE nodes)     4x5  6x5  8x4  8x4  6x5  4x5
    //
    // The second is a flap -- shut, half, open, open, half, shut. The first is
    // the order the chunks happen to sit in the file, and its index 0 is the
    // WIDEST frame: wings fully spread. So the landed ladybug, which has been
    // correctly holding frame 0 the whole time, was holding a ladybug with its
    // wings out -- and the flap itself was playing 8-4-6-8-6-4, which is not a
    // cycle at all.
    //
    // THE FROG ALREADY KNEW. loadShape exists because "voxLoadAll IS THE WRONG
    // READER FOR THE FROG"; what was missed is that it is the wrong reader for
    // any file whose frames are shape nodes, which is every file the asset
    // editor exports. The frog needed the filter because its three cycles share
    // one file; the ladybug needs only the ORDER, so -1 takes them all.
    // -----------------------------------------------------------------------
    void loadShape(World &world, const std::string &path, int shape, const char *what,
                   std::vector<Frame> *out) {
        std::vector<uint8_t> raw;
        VoxScene sc;
        std::string err;
        if (!voxLoadScene(path, &raw, &sc, &err)) {
            std::fprintf(stderr, "v2: %s %s: %s -- skipped\n", what, path.c_str(), err.c_str());
            return;
        }
        std::vector<VoxModel> mo;
        for (const VoxScene::Piece &p : sc.pieces) {
            if (shape >= 0 && p.shape != shape) continue;
            VoxModel m;
            m.sx = p.sx;
            m.sy = p.sy;
            m.sz = p.sz;
            m.pal = sc.pal;
            m.m.assign(size_t(p.sx) * size_t(p.sy) * size_t(p.sz), 0);
            for (size_t q = 0; q + 4 <= p.voxelBytes; q += 4) {
                const int x = p.voxels[q], y = p.voxels[q + 1], z = p.voxels[q + 2];
                if (x < 0 || y < 0 || z < 0 || x >= p.sx || y >= p.sy || z >= p.sz) continue;
                m.m[size_t(x) + size_t(y) * size_t(p.sx) +
                    size_t(z) * size_t(p.sx) * size_t(p.sy)] = p.voxels[q + 3];
            }
            mo.push_back(m);
        }
        if (mo.empty()) {
            std::fprintf(stderr, "v2: %s: no shape %d in %s -- skipped\n", what, shape,
                         path.c_str());
            return;
        }
        adopt(world, mo, what, out);
    }

    void adopt(World &world, const std::vector<VoxModel> &mo, const char *what,
               std::vector<Frame> *out, int matchTol = World::kLifeMatch) {
        for (const VoxModel &m : mo) {
            int sx = 0, sy = 0, sz = 0;
            const int id = world.addFlyerModel(m, what, &sx, &sy, &sz, true, matchTol);
            if (id < 0) { out->clear(); return; }
            Frame fr;
            fr.model = id;
            fr.sx = sx;
            fr.sy = sy;
            fr.sz = sz;
            // The mean Z of everything outside the middle column -- see
            // Frame::wingZ. Measured on the SOURCE model, which is the only
            // place the voxels are still readable.
            {
                double zs = 0.0;
                int n = 0;
                for (int z = 0; z < m.sz; ++z)
                    for (int y = 0; y < m.sy; ++y)
                        for (int x = 0; x < m.sx; ++x) {
                            if (m.at(x, y, z) == 0) continue;
                            if (m.sx >= 3 && x == m.sx / 2) continue;   // the body
                            zs += double(z);
                            ++n;
                        }
                fr.wingZ = n > 0 ? float(zs / double(n)) : 0.0f;
            }
            out->push_back(fr);
        }
    }

    // ---- state ----------------------------------------------------------
    std::vector<Frame> ffly_, ant_, fly_, lbug_, frogHop_, frogRib_, frogTon_;
    std::vector<Firefly> fireflies_;
    uint8_t glowMtl_ = 0;
    bool sharesSpark_ = false;
    uint8_t sparkMtl_ = 0;
    uint8_t wingMtl_ = 0;
    int glowShared_ = 0, wingShared_ = 0;
    uint8_t glowWant_[3] = {kGlowRgb[0], kGlowRgb[1], kGlowRgb[2]};
    bool night_ = false;
    std::vector<Ant> ants_;
    std::vector<Fly> flies_;
    std::vector<Lbug> bugs_;
    std::vector<Frog> frogs_;
    std::vector<float> trail_;
    float trailLast_[2] = {0.0f, 0.0f};
    int trailN_ = 0;
    const std::vector<Solid> *solids_ = nullptr;
    // The top of the lake over a column, if there is one -- see flyFloor.
    GroundF water_;
    GroundF ground_;
    WetF wet_;
    // See the note over update(): the beach gate, nullptr if none was passed.
    WetF sand_;
    BirchF birch_;
    BirthGate birth_;
    // WHERE EACH SPECIES WAS KILLED, by its own index -- see KillHold.
    KillHold fireflyHold_, antHold_, flyHold_, bugHold_, frogHold_;
    void clearHolds() {
        fireflyHold_.clear();
        antHold_.clear();
        flyHold_.clear();
        bugHold_.clear();
        frogHold_.clear();
    }
    float clock_ = 0.0f;
    long frogTicks_[4] = {0, 0, 0, 0};
    long lbugTicks_[3] = {0, 0, 0};
    bool ready_ = false;
};

}  // namespace v2
