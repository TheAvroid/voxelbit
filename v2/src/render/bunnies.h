#pragma once
// ---------------------------------------------------------------------------
// THE BUNNIES, PORTED FROM THE JS ENGINE'S LAND MAMMALS.
//
// That engine keeps four of them -- bunny, armadillo, skunk, porcupine -- and
// the bunny is the one that HOPS rather than walks. What makes it read as a
// rabbit is not the model, it is the gait: a rabbit is still most of the time,
// turns on the spot to look at something, and then crosses ground in a burst of
// discrete hops. A rabbit that slid along at a constant speed would be a white
// armadillo.
//
// SO THE STATE MACHINE IS THE ANIMAL. Three states and nothing else:
//
//   SIT    the default, and where most of a minute goes. It ends by choosing
//          one of the other two.
//   TURN   on the spot, playing the LEFT or RIGHT rotate strip, which is why
//          that strip exists as two separate sets of frames in the art.
//   HOP    one bound: the jump strip played through exactly once while the
//          body travels kBunnyHopM forward and rises through an arc.
//
// v1's own numbers where they carry over: MAM_APART is 110 voxels, which is the
// 11 m the lattice cell below is sized against, and the keep radius is the
// perched songbirds' -- its note is explicit that the land mammals "reach
// EXACTLY as far as the perched songbirds", so ours does too.
//
// EVERYTHING ELSE IS v2'S OWN AND DELIBERATELY SO. The spawn is the lattice
// every other population in this engine uses (see vox siteOf in render/lake.h)
// rather than v1's findBunnyHome, which is bound up with that engine's home
// grids and reservation caches; and the instances go in the flyer band, so a
// bunny is lit by exactly the shader that lights a pine.
// ---------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <functional>
#include <cstdio>
#include <string>
#include <vector>

#include "../gpu/world.h"
#include "../scene/vox.h"
#include "../scene/voxelworld.h"

namespace v2 {

// -- THE STRIPS -------------------------------------------------------------
// jump/00..10 and rotate/{left,right}/00..10. base.vox is SOURCE ART in every
// one of this engine's strips and is skipped by name; loading it would put a
// still pose in the middle of a cycle.
inline constexpr int kBunnyJumpFrames = 11;
inline constexpr int kBunnyTurnFrames = 11;
inline constexpr float kBunnyFps = 24.0f;   // the rate every other strip here plays at


// ---------------------------------------------------------------------------
// ...AND THE SKUNK, WHICH IS THE OTHER KIND OF LAND MAMMAL.
//
// The JS engine keeps four and they come in two gaits. The bunny is the one
// that HOPS -- a state machine, because a rabbit is still most of the time and
// then crosses ground in bursts. The armadillo, the porcupine and the skunk
// MARCH: "a continuous cardinal WALK on the forest floor -- marches ~9 vox/s,
// turns only 90 degrees (never diagonal), follows the terrain, breaks out of
// dead-ends (never spins)". One heading, held until something makes it change.
//
// ...AND IT DOES NOT TURN ON THE GRID, WHICH IS WHERE THIS PARTS FROM v1.
//
// That engine's marcher holds one of FOUR headings and snaps between them, and
// the first version here did the same, on the argument that a body turned 37
// degrees has no face parallel to any face around it. Rejected on sight (user
// 2026-09-14): "currently the skunk is changing direction on the grid. remove
// that mechanics and have the change of direction be off the grid ... turn the
// skunk like you turn the bunny".
//
// THE RABBIT WAS ALREADY THE ANSWER AND I HAD COPIED THE WRONG ENGINE. v2's
// bunny does not march on cardinals: it picks a FREE bearing out of a whisker
// fan and sweeps its heading toward it across the rotate strip. What is square
// to the world there is the ART, not the animal's path -- and a quarter turn
// snapped between two frames is not a turn at all, it is a different animal
// appearing facing the other way.
//
// So the skunk keeps every one of v1's numbers and loses v1's lattice of
// headings: an intent, the same fan the rabbit uses, and a heading that TURNS
// toward it at a rate. The walk cycle is untouched -- "keep the keyframe
// animations though" -- and it plays through the turn, which is what a walking
// animal does.
//
// v1's numbers, at ten centimetres to the voxel, which is what both engines
// use. The skunk is the FASTEST of that engine's marchers and the only one with
// a walk cycle slower than the house rate, which together are most of its
// character: it covers ground at a brisk trot with a slow, deliberate waddle.
// ---------------------------------------------------------------------------
// -- THE FOUR OF THEM, AS A TABLE --------------------------------------------
//
// One marcher, four species, and the ONLY things that differ are in this table.
// Writing the walk four times is how three of them end up with a bug the fourth
// one has already had fixed -- which is the argument fillFish makes next door in
// render/lake.h, where six fish come out of one function and `schools` is the
// whole of what separates them.
//
// EVERY NUMBER IS v1'S, at ten centimetres to the voxel:
//
//   skunk        24 -> 48 vox/s, SKUNK_ANIM_MUL halves its legs to 6 -> 12 fps
//   armadillo    a flat 9 vox/s and 24 fps -- the ONLY one with no second pace,
//                which is v1's `(else) 9` and is most of what an armadillo is
//   porcupine    9 -> 18, 12 -> 24 fps
//   mouse        DES_SPD 32 -> 64 with DES_DASH 2, and the fps doubles with it
//
// THE MOUSE COMES FROM THE DESERT AND v1 ALREADY MOVED IT. DES_OAK puts four
// desert_mice in the oak wood -- "two homes rather than one (user 2026-08-17)"
// -- so a mouse in the birch is that engine's own decision, not a new one.
//
// -- AND TWO OF THEM ARE NOT MAMMALS AT ALL ------------------------------
//
// The WORM and the GRASS SNAKE ride this table (user 2026-09-14), and they
// belong on it for the only reason any of the others do: what a marcher IS,
// here, is an animal that walks a strip of authored frames along the ground on
// a lattice of its own. A worm crawling and a skunk marching differ in speed,
// cadence and where they are allowed to be -- which is four columns of this
// table -- and in nothing else the code has to know about.
//
//   worm    v1's kind-2 crawl, 16 vox/s and no second pace: a worm does not
//           bolt. BOTH woods, which is what BIO_ANY means there.
//   snake   v1's grass_snake, DES_SPD's default 16 with DES_DASH 2 inside
//           DES_DASH_R (70 vox = 7 m) -- so 1.6 m/s rising to 3.2 when you
//           get close. BIRCH only, as asked.
enum MarchKind {
    kMarchSkunk = 0,
    kMarchArmadillo,
    kMarchPorcupine,
    kMarchMouse,
    kMarchWorm,
    kMarchSnake,
    kMarchKinds
};

struct MarchSpec {
    const char *dir;      // under the life directory
    const char *name;     // for the loader's warning and the offline report
    int frames;
    int count;            // how many of them at once
    float cellM;          // its OWN lattice -- v1 reserves each species separately
    uint32_t salt;        // ...which is what makes it its own
    float speed, fleeSpeed;
    float fps, fleeFps;
    float fleeInM, fleeOutM;   // the sphere, and the hysteresis under it
    // WHICH WOODS, as a set of kWoodPine/kWoodBirch/kWoodOak -- see
    // VoxelTerrain::woodBit for why this is a mask and not an index, and for
    // the two-wood bug it replaces.
    uint8_t woods;
};

// COUNTS ARE SIX ACROSS THE BOARD -- see the note over kBunnyCount for the
// arithmetic, which is the whole argument: at two apiece the median distance to
// the nearest one was 58 m and a wood is thirty metres deep, so they existed
// and were never seen. Four species at six is twenty-four animals, but a wood
// only ever sees the ones that belong in it: eighteen in the pine, twelve in
// the birch.
//
// THE WORM AND THE SNAKE ARE NOT RAISED WITH THEM. The worm is already at six
// on a 7 m lattice -- twice these species' density -- and the snake's three is
// v1's own rarity for it. Neither was part of what read as empty.
// THE WORM IS THE ONE THAT IS NOT TWO. v1 asks for 11-22 worms across a disc
// 104 m across, which is an order more than any mammal on this table, and it is
// the right shape for the animal: worms are what a forest floor has a lot of.
// SIX is that density brought down to v2's own lattice -- its cell is 7 m
// against the mammals' 11, so six worms cover about the ground two skunks do.
// The snake keeps v1's own rarity rather than its count (5 over that disc), at
// THREE, because it is birch-only and the birch band is a slice of the world.
// The load line's wording, from the mask. Every combination the table actually
// uses has a name a reader recognises; anything else prints the bits rather
// than guessing, so a new row cannot be quietly described as something it is
// not.
inline const char *woodsName(uint8_t w) {
    switch (w) {
        case kWoodAll:   return "all woods";
        case kWoodPine:  return "pine only";
        case kWoodBirch: return "birch only";
        case kWoodOak:   return "oak only";
        case kWoodBroad: return "birch and oak";
        case uint8_t(kWoodPine | kWoodOak):  return "pine and oak";
        case uint8_t(kWoodPine | kWoodBirch): return "pine and birch";
        default: return "some woods";
    }
}

inline constexpr MarchSpec kMarchSpec[kMarchKinds] = {
    // THE OAK'S ROSTER, SET 2026-09-17 ("add the grass snake, frog, and mouse
    // to the oak forest. remove the armadillo and porcupine from the oak
    // forest"). Until woodBit existed these rows could not say it: the oak
    // read as pine, so the two pine rows were IN it and the two birch rows
    // could not be. Both halves of the ask are this column.
    //
    // dir            name          fr  n  cell  salt      spd  flee  fps  ffps  in    out   woods
    {"skunk",         "skunk",      10, 6, 11.0f, 0x5C0Fu, 2.4f, 4.8f, 6.0f, 12.0f, 3.0f, 4.6f, kWoodAll},
    {"armadillo/walk","armadillo",   8, 6, 11.0f, 0xA2DAu, 0.9f, 0.9f, 24.0f, 24.0f, 3.0f, 4.6f, kWoodPine},
    {"porcupine",     "porcupine",   6, 6, 11.0f, 0x90C0u, 0.9f, 1.8f, 12.0f, 24.0f, 3.0f, 4.6f, kWoodPine},
    {"desert_mouse",  "mouse",       9, 6, 11.0f, 0x3005u, 3.2f, 6.4f, 24.0f, 48.0f, 7.0f, 8.6f, kWoodBroad},
    {"worm",          "worm",       12, 6,  7.0f, 0x7E2Bu, 1.6f, 1.6f, 24.0f, 24.0f, 3.0f, 4.6f, kWoodAll},
    {"grass_snake",   "snake",      12, 3, 13.0f, 0x4D91u, 1.6f, 3.2f, 24.0f, 48.0f, 7.0f, 8.6f, kWoodBroad},
};

// SUMMED OVER THE TABLE, not typed out. It was four terms added by hand and a
// fifth species would have been silently left out of the total -- which is the
// static_assert below going quiet on exactly the overrun it exists to catch.
inline constexpr int marchSum() {
    int n = 0;
    for (int k = 0; k < kMarchKinds; ++k) n += kMarchSpec[k].count;
    return n;
}
inline constexpr int kMarchCount = marchSum();

// HOW MANY FRAMES THE PORCUPINE WALKS IN, out of its own row rather than typed
// a second time. Its bake table is a fixed-size array and the two have to
// agree -- see kPorcupineWalkBake, which is the one marcher strip the asset
// editor stands on the deck.
inline constexpr int kPorcupineFrames = kMarchSpec[kMarchPorcupine].frames;

// A SPHERE, and with the player's own height in it -- flying over one does not
// spook it, standing over one does. v1's 30 in / 46 out, and the gap is the
// hysteresis: without it an animal on the rim flickers between two speeds.
// (the sphere itself is per species -- see MarchSpec::fleeInM)
inline constexpr float kMarchLeashM = 6.0f;      // v1's 60 vox from the cell that owns it
inline constexpr float kMarchWhimMin = 1.0f, kMarchWhimMax = 1.6f;   // v1's re-heading window
// ...AND THE SHORTEST GAP BETWEEN TWO OF THEM, WHICH IS v1'S 0.28 s AND IS
// STILL WHAT STOPS A SPIN. The window above is the routine wander; the think
// also runs EARLY the moment the way ahead closes, and without a floor under it
// that early path runs every frame -- re-rolling the random part of the intent
// sixty times a second while the animal is pressed against a trunk, which is a
// heading that dithers instead of a decision that is made.
inline constexpr float kMarchThinkCool = 0.28f;
inline constexpr float kMarchWhimRad = 1.2f;     // ...and how far the intent may wander in it
inline constexpr float kMarchEase = 6.0f;        // v1's dt*6 ramp, on the pace and the fps
// HOW FAST IT COMES ROUND, and it is the RABBIT'S OWN RATE: kBunnyTurnRad over
// kBunnyTurnSec is 1.96 rad/s. Matching it is the whole of "turn the skunk like
// you turn the bunny" -- a quarter circle takes four fifths of a second in both
// animals, whether or not the art is animating the turn.
inline constexpr float kMarchYaw = 2.0f;         // rad/s
// ...AND IT KEEPS WALKING WHILE IT TURNS, as long as there is room for the
// step. Half a metre is its own body length ahead of it, which at 2.4 m/s is a
// frame and a half: short enough that it does not stop for nothing, long enough
// that it never arrives at a trunk.
inline constexpr float kMarchStepClearM = 0.5f;
// HOW FAR AHEAD A HEADING HAS TO BE CLEAR. v1 asks five voxels of its own
// navmesh; there is no navmesh here, so the whisker IS the sensor and it is
// given a length worth having -- at 2.4 m/s, 1.2 m is half a second of warning,
// which is what a 2.8 rad turn needs to be smooth rather than a swerve.
inline constexpr float kMarchLookM = 1.2f;
// ...AND IF IT CANNOT GO ANYWHERE AT ALL. v1 arms a relaxed-advance window and
// respawns after three failures; this engine's idiom for the same thing is to
// give the slot back and let fill() find ground that works -- see kLilyStuckSec
// in render/lake.h, which is the same rule for a leaf in a puddle.
inline constexpr float kMarchStuckSec = 4.0f;

// -- WHICH STRIP ------------------------------------------------------------
// An index rather than four names, because the asset editor cycles through
// them and the bake tables are looked up by it.
//
// THE FOURTH IS THE PORCUPINE'S WALK, and it is here rather than in a table of
// its own because the editor asks exactly two questions of this class --
// frames(si) and pose(si, ...) -- and answering them for a marcher's strip is
// the whole of putting a marcher on the deck. The rabbit's three are still
// listed: the WILD RABBITS are drawn from them, and taking them out of this
// enum would take the animation out of the wood. What changed is which of them
// the deck offers, which is assetedit.h's kEditSubjects and not this.
inline constexpr int kBunnyStrips = 4;
enum BunnyStrip {
    kStripHop = 0,
    kStripTurnL = 1,
    kStripTurnR = 2,
    kStripPorcupine = 3,
};
inline const char *bunnyStripName(int s) {
    static const char *kN[kBunnyStrips] = {"jump", "rotate/left", "rotate/right", "porcupine"};
    return kN[(s < 0 || s >= kBunnyStrips) ? 0 : s];
}

// ---------------------------------------------------------------------------
// THE BAKE -- WHAT THE ASSET EDITOR EXPORTS AND THIS FILE CONSUMES.
//
// v1 has exactly this and calls it BUNNY_JUMP_BAKE. Its whole reason for
// existing is that ALIGNING A STRIP IS AUTHORING, and the .vox files are not
// where that authoring can live: the frames come out of MagicaVoxel one file at
// a time, each with its own origin, and nothing in the art says that frame 4 of
// a bound should sit two voxels higher and one further forward than frame 3.
// Somebody has to look at it and say so. The editor is where you say it and
// this table is where the answer is kept, which is why the tool's one export is
// a block of C++ you paste over the rows below rather than a file it writes.
//
// ONE ROW PER SLOT OF THE PLAYED STRIP, and `src` is what makes reordering
// expressible: the slot is WHEN, `src` is WHICH FRAME PLAYS THEN. Swapping two
// rows' `src` reorders the animation without touching a single .vox.
//
// ox / oy / oz ARE VOXELS IN THE ANIMAL'S OWN FRAME -- right, up, and BACK,
// since the nose is at local -z everywhere in this engine. They are v1's
// [ox, oy, oz] unchanged, including the sign: its jump bake travels to -6 over
// the cycle because -z is forward there too.
//
// yaw / pitch ARE QUARTER TURNS OF THE POSE, applied pitch first and then yaw
// -- tilt it, then spin it. v1 records an ordered list of 90 degree steps
// ('y+', 'p-') instead, which can express orders these two counts cannot; two
// integers are kept here because the table is meant to be READ in a diff, and
// no pose any of these strips needs has wanted the order that is lost.
// ---------------------------------------------------------------------------
struct BunnyBake {
    int src;               // which frame of the strip plays in this slot
    int ox, oy, oz;        // voxels: right, up, back
    int yaw, pitch;        // quarter turns of the pose
};

// -- PASTE FROM THE ASSET EDITOR BETWEEN HERE ------------------------------
// [I] on the deck, then [C]. ALL THREE STRIPS CARRY THE SAME LIFT: slots 3..7
// at oy 1,2,2,2,1, so the middle of the cycle is 0.2 m up and both ends are
// planted.
//
// THE TURNS MATCH THE HOP BECAUSE THEY ARE THE SAME ELEVEN FRAMES. Measured
// off the .vox files: jump, rotate/left and rotate/right have identical
// bounding boxes, identical centroids and 54 voxels on every frame. A turn is
// a bound that goes nowhere, so a lift baked into one and not the others is
// the same animal jumping two different ways -- which is what the user saw.
//
// So RE-BAKE ALL THREE when the hop changes. [C] already exports all three
// together for this reason; the trap is pasting only the block you edited.
inline constexpr BunnyBake kBunnyHopBake[kBunnyJumpFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 1, 0, 0, 0},
    {4, 0, 2, 0, 0, 0},  {5, 0, 2, 0, 0, 0},  {6, 0, 2, 0, 0, 0},  {7, 0, 1, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
inline constexpr BunnyBake kBunnyTurnLBake[kBunnyTurnFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 1, 0, 0, 0},
    {4, 0, 2, 0, 0, 0},  {5, 0, 2, 0, 0, 0},  {6, 0, 2, 0, 0, 0},  {7, 0, 1, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
inline constexpr BunnyBake kBunnyTurnRBake[kBunnyTurnFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 1, 0, 0, 0},
    {4, 0, 2, 0, 0, 0},  {5, 0, 2, 0, 0, 0},  {6, 0, 2, 0, 0, 0},  {7, 0, 1, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
// -- THE PORCUPINE'S WALK, which is what the deck edits now ----------------
//
// SIX FRAMES OF LEGS, and it starts at identity because nobody has aligned it
// yet -- that is the job the deck is for. It is the FIRST marcher strip to
// have a bake at all: putSkunk used to hand poseOf a `BunnyBake{}` with the
// note "no bake table, because a march has nothing baked into it". That was
// true of a strip nobody could edit and is exactly what stopped being true.
inline constexpr BunnyBake kPorcupineWalkBake[kPorcupineFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 0, 0, 0, 0},
    {4, 0, 0, 0, 0, 0},  {5, 0, 0, 0, 0, 0},
};
// -- ...AND HERE -----------------------------------------------------------

inline const BunnyBake *bunnyBake(int strip) {
    return strip == kStripTurnL ? kBunnyTurnLBake
         : strip == kStripTurnR ? kBunnyTurnRBake
         : strip == kStripPorcupine ? kPorcupineWalkBake
                                : kBunnyHopBake;
}
// How many rows that table actually has. The strips and the tables agree
// today; this is what keeps a folder that grows a twelfth frame from reading
// off the end of one of them.
inline constexpr int bunnyBakeCount(int strip) {
    return strip == kStripHop        ? kBunnyJumpFrames
         : strip == kStripPorcupine  ? kPorcupineFrames
                                     : kBunnyTurnFrames;
}

// -----------------------------------------------------------------------
// WHERE ONE BAKED FRAME STANDS -- the model, its 3x3 and its translation.
//
// This is the ONE piece of arithmetic that turns a bake row into an instance,
// and it is a type rather than four out-parameters because the editor draws
// through it too. A tool that previews a bake with its own copy of this sum
// can agree with the render on the deck and still be wrong about the wood,
// which is the one failure a bake table exists to make impossible.
// -----------------------------------------------------------------------
struct BunnyPose {
    int model = -1;
    float m[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    float tx = 0.0f, ty = 0.0f, tz = 0.0f;
    // THE WORLD BOX THE POSE ACTUALLY OCCUPIES, which pose() has to work out
    // anyway in order to place the body. Handed back rather than thrown away
    // because the editor hangs its gizmos off the model's faces, and a second
    // derivation of where those faces are is a second answer that can disagree
    // -- v1 hit exactly that ("clicking the frog stopped selecting it") when
    // its pick box was rebuilt from the frame's dims instead of from the
    // extent the stamp had measured.
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    // WHERE THE ANIMAL IS, as opposed to where its box is. The motion vector is
    // measured here -- see World::place and the note at the bottom of pose().
    float ax = 0.0f, ay = 0.0f, az = 0.0f;
};

// Exact, not trigonometric: a quarter turn's entries are 0 and +-1, and a
// cosf(1.5707964f) that comes back at -4.4e-8 puts that number in an
// "orthonormal" 3x3 the tracer's normal transform trusts.
inline void bunnyQuarter(int q, int axis, float *m) {
    const int k = ((q % 4) + 4) % 4;
    const float c = (k == 0) ? 1.0f : (k == 2) ? -1.0f : 0.0f;
    const float s = (k == 1) ? 1.0f : (k == 3) ? -1.0f : 0.0f;
    if (axis == 1) {  // yaw, about +Y
        const float r[9] = {c, 0, s, 0, 1, 0, -s, 0, c};
        for (int i = 0; i < 9; ++i) m[i] = r[i];
    } else {          // pitch, about +X
        const float r[9] = {1, 0, 0, 0, c, -s, 0, s, c};
        for (int i = 0; i < 9; ++i) m[i] = r[i];
    }
}

inline void bunnyMul3(const float *a, const float *b, float *out) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out[r * 3 + c] = a[r * 3 + 0] * b[0 * 3 + c] + a[r * 3 + 1] * b[1 * 3 + c] +
                             a[r * 3 + 2] * b[2 * 3 + c];
}

// HOW MANY, AND OVER WHAT. One candidate per cell, so density is a property of
// the grid -- 11 m is v1's MAM_APART (110 voxels) read as a spacing.
//
// -- HALVED (user 2026-09-13: "reduce the frequency of bunnies in half") ----
//
// THE COUNT IS THE LEVER AND THE CELL IS NOT, which is worth writing down
// because the cell is the one that looks like a density. The spawn disc is 96 m
// and the lattice is 11 m, so there are about 240 candidate sites inside it and
// only ever ten slots to put a rabbit in -- the population is capped by the
// SLOTS every time, and widening the lattice would move the rabbits around
// without removing any of them. Ten to five is what halves what you see.
//
// -- AND HALVED AGAIN (user 2026-09-14: "reduce the land mammals in half
//    (skunk and bunnies)") -----------------------------------------------
//
// FIVE DOES NOT HALVE, so this is a judgement: TWO, which is the reduction
// asked for rather than a rounding away from it. Three is the other reading
// and it is one character away if this is now too sparse.
//
// The BAND is untouched at ten. A reservation is not a population -- see
// kBunnySlots, which is ten because the asset editor addresses its three gizmo
// arrows through kBunnySlot0 + 1..3, and because shrinking a band costs a
// structure rebuild to change your mind.
// ---------------------------------------------------------------------------
// SIX, UP FROM TWO -- AND THE HALVING WAS NOT THE MISTAKE, THE ARITHMETIC WAS.
//
// "The forest seems very sparse ... in terms of life" (user 2026-09-14), a few
// hours after "reduce the land mammals in half" took these from four to two.
// Both readings are correct and they are about different things, which is worth
// writing down because the next person to touch this number will otherwise undo
// one of them.
//
// WHAT MAKES AN ANIMAL VISIBLE IS NOT ITS COUNT, IT IS ITS NEAREST DISTANCE,
// and the two are not proportional. Nothing is born inside kBirthMinM (30 m) and
// between there and kBirthFarM only behind you, so each species lives on an
// ANNULUS from 30 m out to kBunnySpawnM. For n of them scattered over it, the
// median distance to the closest is
//
//     ((R^2 - r^2) / (R^2 - 30^2))^n = 0.5
//
// which at n=2, R=96 is 57.8 m. MEASURED at exactly that: armadillo 36 m,
// mouse 46, porcupine 57, bunny 62, skunk 74 -- median 57. You can see perhaps
// thirty metres through a wood, so at two apiece they were never once on
// screen, and "there are no animals" is the correct report of that.
//
// SIX PUTS THE MEDIAN AT 43 m and twelve of them inside the disc rather than
// four. It is still well under the JS engine, which runs FOURTEEN of each over
// a disc of the same size -- nActD 16 * 0.405 * LIFE_DENS_K 2.3391 = 15, forced
// even -- so this is not a reversal of the halving so much as the halving
// applied to a number that was too small to halve.
//
// AND IT IS NOT FREE, WHICH IS WORTH RECORDING BECAUSE IT WAS ASSUMED TO BE.
// A/B at one pinned spawn, three profiled runs of four hundred frames each:
//
//     two of each    5.14 / 4.98 / 5.14 ms mean    ~197 fps
//     six of each    7.22 / 7.34 / 7.01 ms mean    ~139 fps
//
// +2.1 ms for twenty more animals, which is ABOUT A TENTH OF A MILLISECOND
// EACH and far more than a handful of small models should cost to draw. The
// first single-sample comparison looked worse still (4.54 -> 7.2) and was
// partly noise: chunk meshing and BLAS builds moved with it, and neither can
// be touched by adding an animal. BLAS alone measured 1.65, 2.09 and 2.94
// ms/batch across three runs of the SAME binary -- so take three samples here
// or measure nothing.
//
// WHERE IT GOES IS ALMOST CERTAINLY THE SENSING, not the drawing. Every marcher
// runs a seven-whisker fan (see stepSkunk) and every whisker walks the solids
// list, which is hundreds of trunks in a closed wood -- so this is
// O(marchers x whiskers x trunks) on the frame thread, and tripling the first
// term triples it. If v1's own fourteen-per-species is ever wanted, THAT is
// what has to be paid down first; the band, the palette and the traversal all
// have room for it now.
// ---------------------------------------------------------------------------
inline constexpr int kBunnyCount = 6;
inline constexpr float kBunnyCellM = 11.0f;
inline constexpr uint32_t kBunnySalt = 0xB0DDu;

// The songbirds' reach, for the reason v1 gives: the land mammals and the
// perched birds are the same tier and a mismatch dilutes one of them over a
// disc it was not sized for.
inline constexpr float kBunnyKeepM = 105.0f;
inline constexpr float kBunnySpawnM = 96.0f;   // ...with the usual hysteresis under it
inline constexpr float kBunnyFadeSec = 0.8f, kBunnyFadeMin = 0.08f;

// -- THE GAIT ---------------------------------------------------------------
//
// A HOP IS A FIXED DISTANCE IN A FIXED TIME, not a speed. That is the whole
// difference between a bound and a walk, and it is why the position is driven
// from the strip's own progress rather than from a velocity that happens to be
// paused sometimes.
inline constexpr float kBunnyHopM = 0.85f;     // how far one bound covers
inline constexpr float kBunnyHopSec = 0.46f;   // ...and how long it takes
inline constexpr float kBunnyHopRiseM = 0.20f; // how far off the ground it gets
inline constexpr float kBunnyTurnSec = 0.46f;  // one turn, one rotate cycle
inline constexpr float kBunnyTurnRad = 0.9f;   // ...and how far it gets through
inline constexpr float kBunnySitMin = 0.7f, kBunnySitMax = 3.4f;
// How likely a sit ends in a hop rather than a turn. Rabbits mostly travel;
// turning is what they do when something needs looking at.
inline constexpr float kBunnyHopChance = 0.68f;

// -- AND WHAT MAKES IT RUN --------------------------------------------------
//
// The engine's standing rule for every animal in it: a person this close means
// double speed for this long. See kFlyThreatM, kFishThreatM, kDflyThreatM --
// all of them are the same idea and this is the ground-dwelling version.
inline constexpr float kBunnyThreatM = 6.5f;
inline constexpr float kBunnyFleeHold = 2.6f;
inline constexpr float kBunnyFleeMul = 2.0f;
// It does not run for ever in one direction: a leash on its own birth site, so
// a startled rabbit describes a wide arc and ends up back near its patch. The
// slot is recycled on that site, exactly as a lily pad's and a butterfly's are.
inline constexpr float kBunnyLeashM = 16.0f;

// A hop that would land more than this far above or below is refused and the
// bunny turns instead. A rabbit does not bound off a cliff or into a wall.
inline constexpr float kBunnyStepM = 0.55f;

// -- ...AND IT CAN SEE THINGS, WHICH IT COULD NOT BEFORE --------------------
//
// Reported as "the bunny seems to go inside of rocks", and the cause was blunt:
// this animal had NO sensor for objects at all. It probed the terrain height
// under itself and nothing else, so a boulder was not something it could bump
// into -- a boulder was not something it could perceive.
//
// THE FAN IS THE FISH'S. That is the most developed sensor in this engine (see
// WaterField::reach and the seven whiskers in stepFish) and it is the right
// shape here too: sweep a few headings, measure how far the body could travel
// down each, and take the freest with a small bias toward carrying straight on
// so a hopping animal does not weave. The butterflies' obstacle probes are the
// same idea at three headings.
//
// WHAT COUNTS AS AN OBSTACLE IS A STEP IT CANNOT TAKE. A Solid carries its own
// top (see scene/collide.h), so a flat stone a rabbit could hop onto is not an
// obstacle and a trunk or a boulder is -- one test, and it is the same
// kBunnyStepM the terrain already used. Nothing here is a special case for
// rocks; it is the general rule applied to whatever the chunk placed.
inline constexpr float kBunnySenseM = 2.2f;    // how far ahead a whisker reaches
inline constexpr float kBunnySenseStep = 0.28f; // ...and how finely it is walked
inline constexpr int kBunnyWhiskers = 3;       // either side of the intended heading
inline constexpr float kBunnyWhiskerRad = 0.5f; // the spread between them

// ---------------------------------------------------------------------------
// THE POPULATION.
// ---------------------------------------------------------------------------
class Bunnies {
  public:
    // ONE REGISTERED FRAME OF A STRIP. Up here rather than beside the strips
    // themselves because poseOf() takes one by reference, and a member type has
    // to be declared before a signature can name it -- v2::Frame is also the
    // BSDF shading frame in vecmath, so getting this wrong does not fail to
    // compile, it silently binds the wrong type.
    struct Frame {
        int model = -1;
        int sx = 0, sy = 0, sz = 0;
    };

    // -----------------------------------------------------------------------
    // Three strips out of one folder. A missing one is a warning and an empty
    // population, never a crash -- the same contract every other strip loader
    // in this engine keeps.
    // -----------------------------------------------------------------------
    bool load(World &world, const std::string &dir) {
        loadStrip(world, dir + "/jump", kBunnyJumpFrames, &hop_, "bunny jump", &hx_, &hz_);
        loadStrip(world, dir + "/rotate/left", kBunnyTurnFrames, &left_, "bunny turn left", &hx_,
                  &hz_);
        loadStrip(world, dir + "/rotate/right", kBunnyTurnFrames, &right_, "bunny turn right",
                  &hx_, &hz_);
        // ...AND THE SECOND LAND MAMMAL. Its art is a sibling of the rabbit's
        // -- assets/life/skunk/00..09 beside assets/life/bunny -- so it is
        // loaded from the same directory the caller already hands in, one level
        // up. A missing strip disables the skunk and nothing else, exactly as a
        // missing songbird species does.
        {
            const size_t cut = dir.find_last_of("/\\");
            const std::string life = (cut == std::string::npos) ? dir : dir.substr(0, cut);
            for (int k = 0; k < kMarchKinds; ++k) {
                const MarchSpec &sp = kMarchSpec[k];
                loadStrip(world, life + "/" + sp.dir, sp.frames, &march_[k], sp.name,
                          &marchHX_[k], &marchHZ_[k]);
            }
        }
        marchers_.assign(size_t(kMarchCount), March{});
        // ...AND EACH ONE KNOWS WHICH ANIMAL IT IS FROM THE START, so a slot
        // that has never been filled still reports the right species to the
        // band and to the census.
        for (int k = 0; k < kMarchKinds; ++k)
            for (int q = 0; q < kMarchSpec[k].count; ++q)
                marchers_[size_t(marchBase(k) + q)].kind = k;
        buns_.assign(size_t(kBunnyCount), Bunny{});
        ready_ = !hop_.empty();
        if (ready_)
            std::printf("  bunny    %zu hop frames, %zu + %zu turn frames, %d slots\n",
                        hop_.size(), left_.size(), right_.size(), kBunnyCount);
        for (int k = 0; k < kMarchKinds; ++k) {
            if (march_[k].empty()) continue;
            std::printf("  %-8s %zu walk frames, %d slots, %.2f x %.2f m body, %s\n",
                        kMarchSpec[k].name, march_[k].size(), kMarchSpec[k].count,
                        double(marchHX_[k] * 2.0f), double(marchHZ_[k] * 2.0f),
                        woodsName(kMarchSpec[k].woods));
        }
        return ready_;
    }

    // -----------------------------------------------------------------------
    // ONE TICK. `ground` is handed in rather than reached for, exactly as
    // BirdFlock takes it: the generator's own height, not the walk's, because a
    // rabbit does not care about a hole somebody dug and asking the edit layer
    // would cost a scan per probe per bunny per frame.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // `wet` ANSWERS A QUESTION THE GROUND CANNOT.
    //
    // "Have the bunny avoid the water." It could not, and a height probe was
    // never going to give it to us: the ground under a lake is its BED, so as
    // far as the terrain function is concerned a pond is a gentle dip with a
    // perfectly good floor. The bunny was not ignoring the water -- it could
    // not perceive water at all, exactly as it could not perceive boulders
    // before it was given the solid list.
    //
    // HELD AS A std::function, NOT THREADED THROUGH AS A TEMPLATE PARAMETER.
    // blocked() is called from the whisker fan, the hop test and the spawn, and
    // making all three templates on a second callable would spread it over the
    // whole class to save an indirect call on a probe that already does an
    // ellipse test against every solid in range.
    // -----------------------------------------------------------------------
    // `birch` answers "which wood is this point in", 0 pine .. 1 birch, and is
    // what keeps the armadillo out of the birches and the mouse out of the
    // pines. Optional: a caller that passes nothing gets every species in every
    // wood, which is what the asset editor's empty deck wants.
    // `sand` is the beach test -- see blocked(). A std::function like `birch`
    // and NOT a third template parameter: GroundF and WetF are deduced from the
    // lambdas passed in, so a defaulted `const WetF &` would want to
    // default-construct a closure type, which no lambda has. Optional, so the
    // asset deck and the offline report tick exactly as they did.
    template <typename GroundF, typename WetF>
    void update(float dt, const Vec3 &player, const GroundF &ground, const WetF &wet,
                const std::vector<Solid> &solids,
                std::function<uint8_t(float)> wood = nullptr,
                std::function<bool(float, float)> sand = nullptr) {
        if (!ready_) return;
        wet_ = wet;
        sand_ = std::move(sand);
        wood_ = std::move(wood);
        // BORROWED, NOT COPIED, and only for this call. It is the same list the
        // perched birds are handed -- gathered once every half second because
        // trees and boulders do not move, and re-gathering it per animal per
        // frame is the wide query this engine already went out of its way to
        // avoid paying twice.
        solids_ = &solids;
        clock_ += dt;
        // BEFORE fill, which asks it -- see kBirthMinM.
        birth_.tick(dt, player.x, player.z);
        recycle(player, dt);
        fill(player, ground);
        for (size_t i = 0; i < buns_.size(); ++i)
            if (buns_[i].live) step(&buns_[i], uint32_t(i), dt, player, ground);
        // ...AND THE OTHER GAIT, on the same sensors, the same lattice rule and
        // the same fade. Everything that differs between the two animals is in
        // stepSkunk; everything they share is shared rather than copied.
        recycleSkunks(player, dt);
        fillSkunks(player, ground);
        for (size_t i = 0; i < marchers_.size(); ++i)
            if (marchers_[i].live) stepSkunk(&marchers_[i], uint32_t(i), dt, player, ground);
    }

    // ...and onto the band. Every slot, empty ones included: a slot that has
    // just been vacated has to be told it is empty or it keeps what was in it.
    // The marchers' own run of the band. A SEPARATE CALL rather than a second
    // loop inside publish(), because the two runs are not adjacent: the band is
    // a fixed layout and the rabbits' slots were spent before these animals
    // existed. See kMarchSlot0 in gpu/world.h.
    //
    // THE RESERVATION IS A CONTRACT, and lake.h learned what it costs to get
    // wrong: 44 slots written into a 26-slot band silently overwrote nine
    // songbirds and three room buttons, and the only symptom anyone saw was the
    // buttons going missing. This one is checked at compile time.
    static_assert(kMarchCount <= kMarchSlots,
                  "kMarchSlots in gpu/world.h must reserve at least kMarchCount -- the sum of "
                  "every species' count in kMarchSpec");


    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        // A GROUND ANIMAL IS PROBED AT KNEE HEIGHT, not at its feet. Its feet
        // are ON the ground, and a point on the surface reads as inside the
        // terrain in any test that is honest about the terrain -- which is the
        // same 0.2 m offset blocked() uses and for the same reason.
        for (const Bunny &b : buns_)
            if (b.live) out->push_back({Vec3(b.x, b.y + 0.2f, b.z), "bunny", hx_, false});
        for (const March &m : marchers_)
            if (m.live)
                out->push_back({Vec3(m.x, m.y + 0.2f, m.z),
                                kMarchSpec[m.kind < kMarchKinds ? m.kind : 0].name,
                                marchHX_[m.kind < kMarchKinds ? m.kind : 0], false});
    }

    void publishSkunks(World &world, int slot0) {
        if (!ready_) return;
        for (size_t i = 0; i < marchers_.size(); ++i) putSkunk(world, slot0 + int(i), marchers_[i]);
        world.flushFlyerInstances();
    }

    void publish(World &world, int slot0) {
        if (!ready_) return;
        for (size_t i = 0; i < buns_.size(); ++i) put(world, slot0 + int(i), buns_[i]);
        // WITHOUT THIS NOTHING IS DRAWN AND NOTHING SAYS SO. Fourth system in
        // this engine to need the reminder -- birds.h, birdflock.h and lake.h
        // all carry the same note.
        world.flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // WHERE ONE BAKED FRAME STANDS. Public because THE ASSET EDITOR DRAWS
    // THROUGH IT -- see the note over BunnyPose. `bake` overrides the compiled
    // table, which is exactly what the editor is: a working copy of one of
    // those tables that you can see before you paste it.
    //
    // THE TRANSLATION IS A CORNER, AND THE MODEL TURNS ABOUT IT.
    // setFlyerInstance writes tx/ty/tz straight into the transform, so the
    // model's OWN ORIGIN lands there and the rotation is about that origin --
    // not about the animal. Handing it the bunny's position raw swung the body
    // up to eighty centimetres to one side as it turned, which on any slope
    // walks it into the hillside.
    //
    // So the pose's BOX is rotated and the body placed by it: centred in x and
    // z, and RESTING on `at.y` rather than centred in y, because this animal is
    // standing on something and what has to land on the ground is its feet.
    // Doing it off the rotated box rather than off a stored half-width is what
    // lets the bake's pitch quarters work at all -- a frame tipped on its nose
    // has its feet nowhere near its object-space floor.
    // -----------------------------------------------------------------------
    BunnyPose pose(int si, int slotIdx, float heading, float fade, const Vec3 &at,
                   const BunnyBake *bake) const {
        const std::vector<Frame> &st = stripOf(si);
        if (st.empty()) return BunnyPose{};
        const int n = int(st.size());
        const int slot = maxi(0, mini(n - 1, slotIdx));
        const BunnyBake bk = bake ? bake[slot] : bunnyBake(si)[slot];
        return poseOf(st[size_t(maxi(0, mini(n - 1, bk.src)))], bk, heading, fade, at);
    }

    // -----------------------------------------------------------------------
    // ...AND THE PART OF IT THAT IS NOT ABOUT BUNNIES AT ALL.
    //
    // Everything below this line is "stand THIS model on THIS point facing THIS
    // way": the rotated box, the feet on the ground rather than the middle, the
    // corner the transform actually wants, and the anchor the motion vector is
    // measured at. None of it knows what animal it is holding, and the skunk
    // needs every word of it -- so it is one function rather than a second copy
    // that would drift the first time one of them was fixed.
    // -----------------------------------------------------------------------
    BunnyPose poseOf(const Frame &f, const BunnyBake &bk, float heading, float fade,
                     const Vec3 &at) const {
        BunnyPose p;
        p.model = f.model;

        // The nose is at local -z, the same as every other animal in this
        // engine -- see the note over LakeLife::yawMat for why adding pi is the
        // fix and negating a column is not.
        const float c = cosf(heading + 3.14159265f), sn = sinf(heading + 3.14159265f);
        const float H[9] = {c, 0.0f, sn, 0.0f, 1.0f, 0.0f, -sn, 0.0f, c};
        float qy[9], qp[9], q[9], m[9];
        bunnyQuarter(bk.yaw, 1, qy);
        bunnyQuarter(bk.pitch, 0, qp);
        bunnyMul3(qy, qp, q);   // pitch first, then yaw -- tilt it, then spin it
        bunnyMul3(H, q, m);
        for (int i = 0; i < 9; ++i) p.m[i] = m[i] * fade;

        // The rotated box. Eight corners rather than a closed form because the
        // fade puts a scale in the matrix and a pitch quarter permutes the
        // axes, and this is exact under both.
        const float ex[3] = {float(f.sx) * VOXEL_M, float(f.sy) * VOXEL_M,
                             float(f.sz) * VOXEL_M};
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (int k = 0; k < 8; ++k) {
            const float v[3] = {(k & 1) ? ex[0] : 0.0f, (k & 2) ? ex[1] : 0.0f,
                                (k & 4) ? ex[2] : 0.0f};
            for (int r = 0; r < 3; ++r) {
                const float w =
                    p.m[r * 3 + 0] * v[0] + p.m[r * 3 + 1] * v[1] + p.m[r * 3 + 2] * v[2];
                lo[r] = minf(lo[r], w);
                hi[r] = maxf(hi[r], w);
            }
        }
        p.tx = at.x - 0.5f * (lo[0] + hi[0]);
        p.ty = at.y - lo[1];
        p.tz = at.z - 0.5f * (lo[2] + hi[2]);
        // ...and the same box in world metres, once the translation is known.
        for (int r = 0; r < 3; ++r) {
            const float t = (r == 0) ? p.tx : (r == 1) ? p.ty : p.tz;
            p.lo[r] = t + lo[r];
            p.hi[r] = t + hi[r];
        }

        // -- AND THE BAKE'S OWN NUDGE, IN THE ANIMAL'S FRAME ----------------
        //
        // H, not the pose matrix: "forward" has to mean the way the rabbit is
        // going, whatever pose it happens to be holding. A hop that travelled
        // along the tipped-over axis of frame 7 would be a bake nobody could
        // reason about. The fade is deliberately not applied either -- this is
        // where the animal is, not part of its body.
        const float d[3] = {float(bk.ox) * VOXEL_M, float(bk.oy) * VOXEL_M,
                            float(bk.oz) * VOXEL_M};
        const float sh[3] = {H[0] * d[0] + H[1] * d[1] + H[2] * d[2],
                             H[3] * d[0] + H[4] * d[1] + H[5] * d[2],
                             H[6] * d[0] + H[7] * d[1] + H[8] * d[2]};
        p.tx += sh[0];
        p.ty += sh[1];
        p.tz += sh[2];
        // -- AND THAT IS THE POINT THE MOTION VECTOR IS MEASURED AT ---------
        //
        // The animal's own position plus the bake's nudge, which is where the
        // rabbit IS this frame: the nudge is a translation of the ANIMAL (its
        // own note says so) and belongs in the vector, and everything else that
        // separates `at` from the translation does not. What place() would
        // derive instead is the middle of this frame's BOX, which rises and
        // falls with the frame's own height through strips that are not one
        // size -- and again with the fade -- on an animal that has not moved.
        p.ax = at.x + sh[0];
        p.ay = at.y + sh[1];
        p.az = at.z + sh[2];
        for (int r = 0; r < 3; ++r) {
            p.lo[r] += sh[r];
            p.hi[r] += sh[r];
        }
        return p;
    }

    // What the editor has to know about a strip to lay a bake over it.
    bool ready() const { return ready_; }
    int frames(int si) const { return int(stripOf(si).size()); }

    // EVERY SLOT EMPTY. The editor's deck is not the wood -- see World::setStage
    // -- so the population that was following you around the pines has to be
    // told to let go of its slots rather than left holding ten instances of a
    // rabbit four kilometres away. publish() writes empty slots as empty, so
    // one call after this clears the band.
    void despawnAll() {
        for (Bunny &b : buns_) b = Bunny{};
        for (March &s : marchers_) s = March{};
    }

    int skunksLiving(int kind) const {
        int n = 0;
        for (const March &s : marchers_) n += (s.live && s.kind == kind) ? 1 : 0;
        return n;
    }

    // Where one is, and how far, for the offline report -- the same shape
    // nearest() has for the rabbits, and it is here for the same reason: there
    // is no way to tell "no skunks placed" from "a skunk ninety metres away and
    // four pixels across" out of a picture.
    //
    // ANY OF THE FOUR MARCHERS, not just the skunk -- `kind` is a MarchKind,
    // and the name is the file's own shorthand for the family (publishSkunks,
    // skunksLiving). /locate <animal> reads it too, which is why the species
    // is asked for rather than found: a porcupine two metres nearer is the
    // wrong answer to "take me to the skunk".
    //
    // `at` and `dist` are written unconditionally on a hit, so neither may be
    // null -- unlike nearest() above, which tolerates both.
    bool nearestSkunk(int kind, const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e9f;
        for (const March &s : marchers_) {
            if (!s.live || s.kind != kind) continue;
            const float dx = s.x - p.x, dz = s.z - p.z;
            const float d = sqrtf(dx * dx + dz * dz);
            if (d >= best) continue;
            best = d;
            *at = Vec3(s.x, s.y, s.z);
        }
        if (best > 1e8f) return false;
        *dist = best;
        return true;
    }

    int living() const {
        int n = 0;
        for (const Bunny &b : buns_) n += b.live ? 1 : 0;
        return n;
    }

    // The nearest one, for a headless check: "no bunnies placed" and "bunnies
    // placed ninety metres away and two pixels across" are very different bugs
    // and look identical in a frame.
    bool nearest(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Bunny &b : buns_) {
            if (!b.live) continue;
            const float dx = b.x - p.x, dz = b.z - p.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = Vec3{b.x, b.y, b.z};
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    // -----------------------------------------------------------------------
    // ONE ANIMAL, FOR A HEADLESS CHECK TO READ.
    //
    // THE ARC IS COMPUTED IN step() AND STORED NOWHERE A CALLER CAN SEE, which
    // is most of why "the one hopping in place does not match the one hopping
    // forward" sat in the wood unreported: it is a difference between two
    // states of the same animal, and a still frame of one of them looks
    // correct. This is the smallest surface that makes the two comparable from
    // outside -- which cycle, how far through it, and how far off its own floor
    // the body has got.
    //
    // `lift` is measured against b.g rather than the terrain, so it is the arc
    // alone and not the arc plus whatever slope the animal is crossing.
    struct Probe {
        int state = 0;     // 0 sit, 1 turn, 2 hop -- stateName() spells it
        float u = 0.0f;    // 0..1 through this state's own cycle
        float lift = 0.0f; // metres above its own floor
    };
    static const char *stateName(int st) {
        return st == kSit ? "sit" : st == kTurn ? "turn" : "hop";
    }
    bool probe(int k, Probe *out) const {
        if (k < 0 || k >= int(buns_.size())) return false;
        const Bunny &b = buns_[size_t(k)];
        if (!b.live) return false;
        const float cyc = (b.state == kTurn) ? kBunnyTurnSec : kBunnyHopSec;
        out->state = b.state;
        out->u = (b.state == kSit) ? 0.0f : saturate(b.t / cyc);
        out->lift = b.y - b.g;
        return true;
    }
    int slots() const { return int(buns_.size()); }


    // -----------------------------------------------------------------------
    // THAT ONE IS DEAD -- this population's half of a kill. See the same method
    // in render/butterflies.h for the whole of the reasoning; `i` is the index
    // within THIS population's run of the instance band and App::killLifeAt
    // does the arithmetic.
    // -----------------------------------------------------------------------
    bool killSlot(int i) {
        if (i < 0 || size_t(i) >= buns_.size() || !buns_[size_t(i)].live) return false;
        buns_[size_t(i)] = Bunny{};
        return true;
    }

    bool killMarcher(int i) {
        if (i < 0 || size_t(i) >= marchers_.size() || !marchers_[size_t(i)].live) return false;
        marchers_[size_t(i)] = March{};
        return true;
    }

    // WHICH OF THE SIX A MARCHER IS. One run holds all of them -- see the note
    // over MarchKind -- so the meat rule cannot be answered from the band's
    // layout alone: a worm and a skunk are neighbours in it and one of them
    // leaves a carcass. Returns kMarchKinds for a slot with nothing in it.
    int marchKindAt(int i) const {
        if (i < 0 || size_t(i) >= marchers_.size() || !marchers_[size_t(i)].live)
            return kMarchKinds;
        return int(marchers_[size_t(i)].kind);
    }

  private:
    enum State { kSit = 0, kTurn = 1, kHop = 2 };

    // ONE FRAME OF ONE STRIP: the model the flyer band knows it by, and the
    // size it meshed to. The DIMS ARE PER FRAME and that is not caution -- the
    // frames of these strips differ in height (8 rows and 10), and a quarter
    // turn out of the bake turns the footprint with it, so the box a pose is
    // centred on is a property of the pose and not of the strip.
    //
    // DECLARED UP HERE, ABOVE EVERY USE, because v2 already has a `Frame` at
    // NAMESPACE scope: a member declaration written `std::vector<Frame> *`
    // further down binds to that one -- member types are visible inside
    // function BODIES wherever they are declared, but not in the signatures.
    // THE PORCUPINE'S WALK IS ONE OF THESE NOW, which is the whole of how a
    // marcher reaches the asset editor: the editor asks this class for
    // frames(si) and pose(si, ...) and nothing else, and both of them come
    // through here. No fourth vector -- march_[kMarchPorcupine] is the strip
    // the wood is already walking on, so the deck and the wood cannot be
    // holding two different animations of the same animal.
    const std::vector<Frame> &stripOf(int si) const {
        return si == kStripTurnL     ? left_
             : si == kStripTurnR     ? right_
             : si == kStripPorcupine ? march_[kMarchPorcupine]
                                     : hop_;
    }

    struct Bunny {
        bool live = false;
        int cx = 0, cz = 0;          // the lattice cell it belongs to
        float sx = 0.0f, sz = 0.0f;  // ...and that cell's site, which it is leashed to
        float x = 0, y = 0, z = 0;
        float g = 0;                 // the ground under it
        float th = 0;                // heading; 0 looks down -Z, as everything here does
        float th0 = 0, th1 = 0;      // a turn's start and end
        int state = kSit;
        int dirRight = 0;            // which rotate strip a turn is playing
        float t = 0;                 // seconds into the current state
        float hold = 0;              // ...and how long it lasts
        float hopFromX = 0, hopFromZ = 0;
        float fleeUntil = -1.0f;
        float age = 0.0f, dying = -1.0f;
    };

    // -- ...AND THE MARCHER ------------------------------------------------
    struct March {
        bool live = false;
        // WHICH OF THE FOUR. Its slot decides it -- the band is laid out by
        // species in table order -- but carrying it is what lets one step
        // function read one row of kMarchSpec and be four animals.
        int kind = 0;
        int cx = 0, cz = 0;          // the lattice cell it belongs to
        float sx = 0.0f, sz = 0.0f;  // ...and that cell's site, which it is leashed to
        float x = 0, y = 0, z = 0;
        float g = 0;                 // the ground under its own footprint
        // WHERE IT FACES AND WHERE IT WOULD LIKE TO. Both FREE bearings -- the
        // heading turns toward the want at kMarchYaw, so every angle between
        // the two is a frame the animal is really drawn at.
        float th = 0, thWant = 0;
        float whimAt = 0;            // when it next re-picks an intent, at the latest
        float holdAt = 0;            // ...and the soonest -- see kMarchThinkCool
        bool boxed = false;          // the last think found nowhere to go at all
        float spd = 0, fps = 0;      // both eased, never snapped -- v1's ramp
        float frame = 0;             // where it is in the walk cycle
        bool flee = false;           // hysteretic; see kSkunkFleeInM
        float stuck = 0.0f;          // seconds with nowhere to go -- see kMarchStuckSec
        float age = 0.0f, dying = -1.0f;
    };

    // -----------------------------------------------------------------------
    // IS THIS SPOT INSIDE SOMETHING?
    //
    // THE SAME TEST THE PLAYER AND THE BUTTERFLIES USE -- see insideWorld in
    // render/player.h. Where a Solid carries its own volume it is asked
    // VOXEL-ACCURATELY (solidAtWorld); only where it does not does it fall back
    // to the ellipse inscribed in its half extents. That is the whole of
    // "hitboxes in relation to everything else": a bunny and a person agree
    // about where a boulder is because they are asking the same question of the
    // same data, not because two approximations happen to be close.
    //
    // Grown by the animal's own half-width, which turns "is this point inside"
    // into "does the body overlap" without needing a second shape. The volume
    // path cannot be grown that way, so it is probed at the body's rim instead.
    //
    // A LOW STONE IS NOT AN OBSTACLE. Its top is within one hop's step of the
    // ground the bunny is standing on, so it can simply get up onto it -- which
    // is what a rabbit does with a rock. The test is the height, not the kind.
    // -----------------------------------------------------------------------
    bool blocked(float x, float z, float feetY) const { return blocked(x, z, feetY, hx_, hz_); }

    // ...AND THE SAME QUESTION FOR A BODY OF ANOTHER SIZE. The skunk is a
    // different animal on the same ground, and a sensor it does not share with
    // the rabbit is a sensor that can disagree with the rabbit -- which is the
    // whole argument in the note above, one species further on.
    bool blocked(float x, float z, float feetY, float hx, float hz) const {
        // WATER IS AN OBSTACLE LIKE ANY OTHER, and saying it here rather than in
        // each caller is what makes it apply to all of them at once: the
        // whisker fan will not point at a lake, a hop will not be taken into
        // one, and a site on one is never claimed.
        if (wet_ && wet_(x, z)) return true;
        // -- ...AND SO IS THE BEACH (user 2026-09-14: "avoid putting
        //    landmammals on the sand banks") -----------------------------
        //
        // IN HERE RATHER THAN IN THE SPAWN, for the reason the water is: the
        // ask is about where they are PUT, and a gate on the spawn alone would
        // let one wander onto the sand a few seconds later and stand there,
        // which is the same picture arrived at the slow way. One line covers
        // the site claim, the whisker fan and every step.
        //
        // AND IT CANNOT TRAP ONE THAT IS ALREADY THERE. This is asked of a
        // DESTINATION, never of where the animal is, so a marcher that somehow
        // finds itself on a bank has every inland heading open to it -- and
        // stepSkunk's `boxed` counter is the backstop if it does not.
        if (sand_ && sand_(x, z)) return true;
        const float probeY = feetY + 0.2f;   // knee height: what a body would meet
        for (const Solid &s : solidList()) {
            if (s.top <= feetY + kBunnyStepM) continue;   // low enough to hop onto
            if (s.hx <= 0.0f || s.hz <= 0.0f) continue;
            if (s.vol) {
                // -- THE WHOLE FOOTPRINT, NOT FIVE POINTS OF IT --------------
                //
                // This was the centre and the four rim extremes, on the
                // argument that a single centre probe lets an overhang sit
                // inside the animal. True, and not enough: five points leave
                // the space BETWEEN them unasked, and a birch trunk is 40 cm
                // across while a mouse is 50 by 140. A trunk fitted neatly
                // into the gap beside the nose -- the animal walked with the
                // tree through its flank, its centre a third of a metre clear,
                // and every probe honestly reported air.
                //
                // solidBoxOverlap is the exact answer and was already in the
                // file, written for the player's own body. It costs a range
                // scan over the one or two models a body is actually near and
                // nothing at all for the rest, which is what its early
                // rejections are for.
                if (solidBoxOverlap(s, x, probeY, z, probeY + 0.1f, hx, hz, VOXEL_M)) return true;
                continue;
            }
            const float ex = s.hx + hx, ez = s.hz + hz;
            const float dx = (x - s.cx) / ex, dz = (z - s.cz) / ez;
            if (dx * dx + dz * dz < 1.0f) return true;
        }
        return false;
    }

    // HOW FAR THE BODY COULD TRAVEL DOWN A HEADING before something stops it.
    // The fish's reach(), walked in steps rather than in cells because there is
    // no grid here -- the solids are an unsorted list of ellipses.
    template <typename GroundF>
    float clearAhead(const Bunny &b, float th, const GroundF &ground) const {
        return clearAhead(b.x, b.z, b.g, th, hx_, hz_, kBunnySenseM, ground);
    }

    // The same walk, for a body of any size and a reach of any length -- see
    // the note over blocked().
    template <typename GroundF>
    float clearAhead(float px, float pz, float g, float th, float hx, float hz, float reach,
                     const GroundF &ground) const {
        const float sn = sinf(th), c = cosf(th);
        for (float d = kBunnySenseStep; d <= reach; d += kBunnySenseStep) {
            const float x = px + sn * d, z = pz + c * d;
            if (blocked(x, z, g, hx, hz)) return d - kBunnySenseStep;
            // A wall of terrain stops it just as a boulder does, and for the
            // animal there is no difference worth drawing between the two.
            if (fabsf(ground(x, z) - g) > kBunnyStepM) return d - kBunnySenseStep;
        }
        return reach;
    }

    // -----------------------------------------------------------------------
    // THE GROUND UNDER A BUNNY, WHICH IS NOT THE GROUND UNDER ITS CENTRE.
    //
    // A bunny is 50 cm across and 80 long. Sampling one column and standing the
    // whole body on it buries the uphill half of it in any ground that is not
    // level -- which is most ground in a wood. The HIGHEST of five samples over
    // its own footprint is what keeps every part of it out of the dirt; the
    // cost is that it stands a little proud on a slope, which is invisible, and
    // the alternative is a rabbit with its shoulder in a hillside.
    //
    // This is the trees' own rule (see groundDrop in scene/collide.h, which
    // sinks a trunk until nothing of it is standing clear) read the other way
    // up -- and for the same reason: a footprint is not a point.
    // -----------------------------------------------------------------------
    template <typename GroundF>
    float groundUnder(const Bunny &b, const GroundF &ground) const {
        return groundUnder(b.x, b.z, b.th, hx_, hz_, ground);
    }

    // ...for a body of any size. See the note over blocked().
    template <typename GroundF>
    float groundUnder(float px, float pz, float th, float hx, float hz,
                      const GroundF &ground) const {
        const float c = cosf(th), sn = sinf(th);
        // Half the body, in its own frame: across, and along the nose.
        const float ax = hx, az = hz;
        float g = standOn(px, pz, ground(px, pz));
        const float ox[4] = {ax, -ax, ax, -ax};
        const float oz[4] = {az, az, -az, -az};
        for (int i = 0; i < 4; ++i) {
            const float wx = px + ox[i] * c + oz[i] * sn;
            const float wz = pz - ox[i] * sn + oz[i] * c;
            g = maxf(g, standOn(wx, wz, ground(wx, wz)));
        }
        return g;
    }

    // -----------------------------------------------------------------------
    // THE TERRAIN HERE, RAISED TO THE TOP OF ANYTHING STANDABLE ON IT.
    //
    // blocked() has always let a body walk onto a stone whose top is within one
    // step -- "a low stone is not an obstacle... which is what a rabbit does
    // with a rock" -- and nothing ever raised the body when it did. So the
    // rabbit walked onto the rock at the height of the DIRT the rock is
    // standing in, which is the stone passing through the animal, and the
    // taller the stone the worse it looked right up to the step limit.
    //
    // It survived because it is invisible from anywhere but beside it: the two
    // halves of the rule live four hundred lines apart and each is right on its
    // own. --clip-test found it at 83 mouse-frames a minute in the birch wood.
    // -----------------------------------------------------------------------
    float standOn(float x, float z, float g) const {
        const std::vector<Solid> &sl = solidList();
        if (sl.empty()) return g;
        // CAPPED AT THE STEP THE ANIMAL CAN TAKE, which is the same number
        // blocked() refuses a solid by -- so the two rules agree by
        // construction: anything this raises the body onto is something
        // blocked() was already letting it walk onto, and nothing else.
        return solidsFloor(sl.data(), int(sl.size()), x, z, VOXEL_M, g, kBunnyStepM);
    }


    // =======================================================================
    // THE SKUNK: ONE HEADING, HELD UNTIL SOMETHING MAKES IT CHANGE.
    // =======================================================================

    // WHERE A SPECIES' RUN OF THE ARRAY BEGINS. The band is one fixed layout
    // and every population in it owns a contiguous run -- see kFlyerInstances
    // in gpu/world.h, which makes the same point for the same reason.
    static int marchBase(int kind) {
        int at = 0;
        for (int k = 0; k < kind && k < kMarchKinds; ++k) at += kMarchSpec[k].count;
        return at;
    }

    // Its slot is given up when its SITE leaves range, exactly as the rabbit's
    // is, and it shrinks out rather than vanishing.
    void recycleSkunks(const Vec3 &player, float dt) {
        for (March &s : marchers_) {
            if (!s.live) continue;
            const float dx = s.sx - player.x, dz = s.sz - player.z;
            const bool gone = s.stuck > kMarchStuckSec ||
                              dx * dx + dz * dz > kBunnyKeepM * kBunnyKeepM;
            s.age += dt;
            if (gone && s.dying < 0.0f) s.dying = 0.0f;
            if (!gone && s.dying >= 0.0f) s.dying = -1.0f;
            if (s.dying >= 0.0f) {
                s.dying += dt;
                if (s.dying >= kBunnyFadeSec) s.live = false;
            }
        }
    }

    // ...AND ONLY AGAINST ITS OWN KIND. v1 is explicit that each land mammal
    // reserves "ONLY against other skunks" on a grid of its own -- four species
    // on four salted lattices do not push each other around, and two of them
    // standing in the same clearing is a wood, not a collision.
    bool skunkClaimed(int kind, int cx, int cz) const {
        for (const March &s : marchers_)
            if (s.live && s.kind == kind && s.cx == cx && s.cz == cz) return true;
        return false;
    }

    // THE RABBIT'S FILL, ON THE SKUNK'S OWN LATTICE. v1 is explicit that each
    // land mammal reserves "ONLY against other skunks" on a grid of its own, so
    // the two species do not push each other around -- which is what the salt
    // buys. Everything else here is the same rule and the same order: nearest
    // free site, flat enough to stand on, not inside anything, not where you
    // can watch it arrive.
    template <typename GroundF>
    void fillSkunks(const Vec3 &player, const GroundF &ground) {
        for (size_t i = 0; i < marchers_.size(); ++i) {
            March &s = marchers_[i];
            if (s.live) continue;
            const MarchSpec &sp = kMarchSpec[s.kind];
            if (march_[s.kind].empty()) continue;

            const int r = int(kBunnySpawnM / sp.cellM) + 1;
            const int c0x = int(floorf(player.x / sp.cellM));
            const int c0z = int(floorf(player.z / sp.cellM));
            // IN HASH ORDER, for the reason the rabbits' fill gives: two of an
            // animal taken nearest-first are two of it at your feet, and none
            // of it anywhere else.
            int bcx = 0, bcz = 0;
            float bx = 0, bz = 0, best = 2.0f;
            bool found = false;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const int cx = c0x + dx, cz = c0z + dz;
                    if (skunkClaimed(s.kind, cx, cz)) continue;
                    float sx = 0, sz = 0;
                    siteOf(sp.cellM, sp.salt, cx, cz, &sx, &sz);
                    const float ex = sx - player.x, ez = sz - player.z;
                    const float d2 = ex * ex + ez * ez;
                    if (d2 > kBunnySpawnM * kBunnySpawnM) continue;
                    const float ord = siteOrder(sp.salt, cx, cz);
                    if (ord >= best) continue;
                    if (!birth_.may(d2)) continue;
                    // -- AND IN THE RIGHT WOOD --------------------------------
                    //
                    // The armadillo and the porcupine are the pine's, the mouse
                    // is the birch's, and the skunk is in both. Asked of the
                    // SITE rather than of the animal: a marcher may wander
                    // across the seam afterwards and that is fine -- what would
                    // not be fine is a population that thins out every time one
                    // of them crosses it.
                    if (sp.woods != kWoodAll && wood_ && !(wood_(sx) & sp.woods)) continue;
                    const float g0 = ground(sx, sz);
                    const float sl = maxf(maxf(fabsf(ground(sx + 1.0f, sz) - g0),
                                               fabsf(ground(sx - 1.0f, sz) - g0)),
                                          maxf(fabsf(ground(sx, sz + 1.0f) - g0),
                                               fabsf(ground(sx, sz - 1.0f) - g0)));
                    if (sl > kBunnyStepM) continue;
                    if (blocked(sx, sz, g0, marchHX_[s.kind], marchHZ_[s.kind]))
                        continue;   // water included
                    best = ord;
                    bcx = cx;
                    bcz = cz;
                    bx = sx;
                    bz = sz;
                    found = true;
                }
            if (!found) continue;

            const int kind = s.kind;
            s = March{};
            s.kind = kind;
            s.live = true;
            s.cx = bcx;
            s.cz = bcz;
            s.sx = bx;
            s.sz = bz;
            s.x = bx;
            s.z = bz;
            // ANY BEARING, and it is already where it wants to be facing --
            // a skunk that spent its first second turning out of the direction
            // it was born in would look like it had been put down wrong.
            s.th = rnd(uint32_t(i), 0x5A1u) * 6.2831853f;
            s.thWant = s.th;
            s.g = groundUnder(s.x, s.z, s.th, marchHX_[kind], marchHZ_[kind], ground);
            s.y = s.g;
            s.spd = sp.speed;
            s.fps = sp.fps;
            s.frame = rnd(uint32_t(i), 0x5A2u) * float(sp.frames);
            s.whimAt = clock_ + kMarchWhimMin;
        }
    }

    // -----------------------------------------------------------------------
    // ONE MARCH.
    //
    // v1's branch, in its own order: the ground first (one number, shared by
    // the lookahead, the step and the servo, so all three agree about where the
    // floor is), then the flee test, then EITHER a blocked turn OR a clear-path
    // decision, then the advance, then the floor.
    // -----------------------------------------------------------------------
    template <typename GroundF>
    void stepSkunk(March *s, uint32_t i, float dt, const Vec3 &player, const GroundF &ground) {
        const MarchSpec &sp = kMarchSpec[s->kind];
        const float hx = marchHX_[s->kind], hz = marchHZ_[s->kind];
        s->g = groundUnder(s->x, s->z, s->th, hx, hz, ground);

        // -- IT BREAKS WHEN YOU GET CLOSE, AND THE TWO RADII ARE WHY IT DOES
        //    NOT FLICKER AT THE RIM. A sphere, with your own height in it.
        const float px = s->x - player.x, py = player.y - s->y, pz = s->z - player.z;
        const float d2 = px * px + py * py + pz * pz;
        const float rr = s->flee ? sp.fleeOutM : sp.fleeInM;
        s->flee = d2 < rr * rr;

        // How far it could walk down a bearing before something stopped it --
        // the RABBIT'S OWN WHISKER, so the two animals cannot disagree about
        // what a boulder or a lake is.
        auto reach = [&](float t) {
            return clearAhead(s->x, s->z, s->g, t, hx, hz, kMarchLookM, ground);
        };
        const float ahead = reach(s->th);

        // -- WHAT IT WOULD LIKE TO DO, RE-ASKED ON A CLOCK ------------------
        //
        // v1's whim window, and the clock is what makes this an animal with an
        // intention rather than a servo: a marcher picks a direction and then
        // walks in it for a second or two, whatever the ground in front of it
        // is doing. It re-asks EARLY when the way ahead closes, because an
        // intention that walks you into a trunk is not one worth keeping.
        if (clock_ > s->holdAt && (clock_ > s->whimAt || ahead < kMarchStepClearM)) {
            s->holdAt = clock_ + kMarchThinkCool;
            s->whimAt =
                clock_ + kMarchWhimMin + rnd(i, 0x5A3u) * (kMarchWhimMax - kMarchWhimMin);

            float want = s->th;
            const float lx = s->x - s->sx, lz = s->z - s->sz;
            if (s->flee) {
                // Directly away, with a little scatter -- the rabbit's own
                // line, so two startled together do not leave along one.
                want = atan2f(px, pz) + (rnd(i, 0x5A4u) - 0.5f) * 0.8f;
            } else if (lx * lx + lz * lz > kMarchLeashM * kMarchLeashM) {
                // HOME BEATS THE WANDER. The slot is recycled on the SITE, and
                // v1's own note says what the leash is really for: without it
                // the marchers drift together into a bunch.
                want = atan2f(-lx, -lz) + (rnd(i, 0x5A5u) - 0.5f) * 0.7f;
            } else {
                want = s->th + (rnd(i, 0x5A6u) - 0.5f) * kMarchWhimRad;
            }

            // -- THE FAN, SWEPT ABOUT WHAT IT WANTED --------------------
            //
            // The intent above is what it would LIKE; this is what the world
            // will let it have. The freest of seven, biased toward the intent
            // so a skunk with open ground all round still goes where it meant
            // to. Character for character the rabbit's, which is the point.
            float best = -1e9f;
            float bestTh = want;
            for (int k = -kBunnyWhiskers; k <= kBunnyWhiskers; ++k) {
                const float off = float(k) * kBunnyWhiskerRad;
                const float sc = reach(want + off) - fabsf(off) * 0.35f;
                if (sc > best) { best = sc; bestTh = want + off; }
            }
            s->thWant = bestTh;
            // NOWHERE AT ALL TO GO is the one case a turn cannot answer.
            s->boxed = best <= 0.0f;
        }
        // ...and a marcher that stays wedged gives its slot back rather than
        // treading the same voxel for the rest of the session. COUNTED IN
        // SECONDS, not in thinks: the think runs on a cooldown, so counting
        // those would make the timeout depend on how often it happened to ask.
        s->stuck = s->boxed ? s->stuck + dt : 0.0f;

        // -- AND THE HEADING COMES ROUND TO IT, AT A RATE -------------------
        //
        // The whole of what was asked for. angleTo keeps the difference in
        // (-pi, pi], so it always turns the short way round, and the step is
        // capped by kMarchYaw rather than assigned -- which is the difference
        // between an animal turning and an animal being replaced by one facing
        // the other way.
        {
            const float err = angleTo(s->thWant - s->th);
            const float step = kMarchYaw * dt;
            s->th += clampf(err, -step, step);
        }

        // -- THE PACE AND THE LEGS, ON ONE FLAG AND ONE RAMP ----------------
        //
        // Both eased at v1's dt*6 rather than switched, so being startled reads
        // as an animal accelerating instead of an animal teleported to a new
        // speed -- and because they share the flag and the ramp, the legs can
        // never be running at a rate the body is not travelling at.
        const float k = minf(1.0f, dt * kMarchEase);
        s->spd += ((s->flee ? sp.fleeSpeed : sp.speed) - s->spd) * k;
        s->fps += ((s->flee ? sp.fleeFps : sp.fps) - s->fps) * k;
        s->frame = fmodf(s->frame + dt * s->fps, float(maxi(1, int(march_[s->kind].size()))));

        // ADVANCE ONLY WHEN THE WAY IS CLEAR, which is what makes "never walks
        // into a rock" true rather than nearly true -- and asked of the heading
        // it is FACING NOW, part way through a turn, not of the one it is
        // turning toward.
        if (reach(s->th) >= kMarchStepClearM) {
            s->x += sinf(s->th) * s->spd * dt;
            s->z += cosf(s->th) * s->spd * dt;
        }

        // -- AND THE FLOOR, LAST AND UNCONDITIONALLY -----------------------
        //
        // Up at once, down gently, for the reason the rabbit's is: a thing
        // standing ON the ground may never be BELOW it, and a symmetric ease
        // lags behind rising ground. Last, so no heading change is left for it
        // to be stale against -- the footprint it samples is rotated into the
        // frame the animal is facing NOW.
        const float gh = groundUnder(s->x, s->z, s->th, hx, hz, ground);
        s->g = maxf(gh, s->g + (gh - s->g) * (1.0f - expf(-14.0f * dt)));
        s->y = s->g;
    }

    // Its instance: a free yaw, a walk frame, and -- for the porcupine only --
    // a bake table.
    //
    // THE NOTE THAT USED TO BE HERE SAID "no bake table, because a march has
    // nothing baked into it", and it was right about a strip nobody could
    // edit. The porcupine stands on the asset editor's deck now, so the wood
    // has to READ what that deck exports or the tool is nudging a table
    // nothing consumes -- which is the one failure a bake table exists to
    // prevent, and the reason the deck and the wood both draw through poseOf.
    //
    // The other three keep the identity bake, which is byte-for-byte what
    // BunnyBake{} was: src comes from the slot, offsets and turns are zero.
    void putSkunk(World &world, int slot, const March &s) const {
        if (!s.live || march_[s.kind].empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const int n = int(march_[s.kind].size());
        const int fi = maxi(0, mini(n - 1, int(s.frame)));
        // THE SLOT IS WHEN, `src` IS WHICH FRAME PLAYS THEN -- see BunnyBake.
        // So the walk clock picks the SLOT and the table picks the frame, and
        // a reorder exported off the deck is expressible without touching a
        // .vox. Bounded by the table's own length, not the strip's, for the
        // reason the editor's enter() is.
        BunnyBake bk{fi, 0, 0, 0, 0, 0};
        if (s.kind == kMarchPorcupine && fi < bunnyBakeCount(kStripPorcupine))
            bk = bunnyBake(kStripPorcupine)[fi];
        const int src = maxi(0, mini(n - 1, bk.src));
        const BunnyPose p = poseOf(march_[s.kind][size_t(src)], bk, s.th,
                                   fadeOf(s.age, s.dying), Vec3(s.x, s.y, s.z));
        if (p.model < 0) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        // THE ANIMAL, NOT ITS BOX -- see World::place. Ten frames that are not
        // one size, under a transform that also carries the fade.
        const float anchor[3] = {p.ax, p.ay, p.az};
        world.setFlyerInstance(slot, p.model, p.m, p.tx, p.ty, p.tz, nullptr, true, nullptr,
                               anchor);
    }

    // A hash stream per bunny that moves with the clock, so two tenancies of
    // one slot do not produce the same animal.
    float rnd(uint32_t i, uint32_t salt) const {
        return hashUnit(salt, hashU32(i * 2654435761u, uint32_t(clock_ * 1000.0f)));
    }

    // -- THE FADE, WHICH IS THE BUTTERFLIES' ------------------------------
    static float fadeOf(float age, float dying) {
        const float in = saturate(age / kBunnyFadeSec);
        const float sm = in * in * (3.0f - 2.0f * in);
        const float out = dying >= 0.0f ? saturate(1.0f - dying / kBunnyFadeSec) : 1.0f;
        return kBunnyFadeMin + (1.0f - kBunnyFadeMin) * sm * out;
    }

    // The slot is given up when its SITE leaves range, never when the animal
    // does -- and it shrinks out over most of a second rather than vanishing.
    void recycle(const Vec3 &player, float dt) {
        for (Bunny &b : buns_) {
            if (!b.live) continue;
            const float dx = b.sx - player.x, dz = b.sz - player.z;
            const bool gone = dx * dx + dz * dz > kBunnyKeepM * kBunnyKeepM;
            b.age += dt;
            if (gone && b.dying < 0.0f) b.dying = 0.0f;
            if (!gone && b.dying >= 0.0f) b.dying = -1.0f;
            if (b.dying >= 0.0f) {
                b.dying += dt;
                if (b.dying >= kBunnyFadeSec) b.live = false;
            }
        }
    }

    bool claimed(int cx, int cz) const {
        for (const Bunny &b : buns_)
            if (b.live && b.cx == cx && b.cz == cz) return true;
        return false;
    }

    // -----------------------------------------------------------------------
    // FILL FROM THE LATTICE, which is this engine's rule for every population:
    // a site is a fact about the world, computed from its cell's coordinates,
    // and what a population does is CLAIM sites rather than invent them. So a
    // bunny you walk away from and come back to is the same bunny in the same
    // patch of wood.
    // -----------------------------------------------------------------------
    BirthGate birth_;

    template <typename GroundF>
    void fill(const Vec3 &player, const GroundF &ground) {
        int free = 0;
        for (const Bunny &b : buns_) free += b.live ? 0 : 1;
        if (!free) return;

        const int r = int(kBunnySpawnM / kBunnyCellM) + 1;
        const int c0x = int(floorf(player.x / kBunnyCellM));
        const int c0z = int(floorf(player.z / kBunnyCellM));
        for (size_t i = 0; i < buns_.size(); ++i) {
            Bunny &b = buns_[i];
            if (b.live) continue;
            // IN HASH ORDER, NOT NEAREST FIRST -- see siteOrder in
            // core/noise.h. Nearest-first put both rabbits inside nine metres
            // of the player on a ninety-six metre disc, which is "the life
            // clusters around the player on spawn" and "further out it is
            // sparse" in one rule.
            int bcx = 0, bcz = 0;
            float bx = 0, bz = 0, best = 2.0f;
            bool found = false;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const int cx = c0x + dx, cz = c0z + dz;
                    if (claimed(cx, cz)) continue;
                    float sx = 0, sz = 0;
                    siteOf(kBunnyCellM, kBunnySalt, cx, cz, &sx, &sz);
                    const float ex = sx - player.x, ez = sz - player.z;
                    const float d2 = ex * ex + ez * ez;
                    if (d2 > kBunnySpawnM * kBunnySpawnM) continue;
                    const float ord = siteOrder(kBunnySalt, cx, cz);
                    if (ord >= best) continue;
                    // NOT WHERE YOU CAN WATCH IT ARRIVE -- see kBirthMinM. It
                    // matters less than it did now the claim is not
                    // nearest-first, but a hash order still lands on a near
                    // cell sometimes and thirty metres is thirty metres.
                    if (!birth_.may(d2)) continue;
                    // FLAT ENOUGH TO STAND ON. One probe and its four
                    // neighbours: a rabbit on a cliff face is worse than no
                    // rabbit, and this is the cheapest honest test there is.
                    const float g0 = ground(sx, sz);
                    const float sl = maxf(maxf(fabsf(ground(sx + 1.0f, sz) - g0),
                                               fabsf(ground(sx - 1.0f, sz) - g0)),
                                          maxf(fabsf(ground(sx, sz + 1.0f) - g0),
                                               fabsf(ground(sx, sz - 1.0f) - g0)));
                    if (sl > kBunnyStepM) continue;
                    // ...NOR INSIDE ANYTHING. A site is a fact about the world
                    // and so is the boulder standing on it; the lattice does not
                    // know about the scatter, so this is where the two meet.
                    if (blocked(sx, sz, g0)) continue;   // water included -- see blocked()
                    best = ord;
                    bcx = cx;
                    bcz = cz;
                    bx = sx;
                    bz = sz;
                    found = true;
                }
            if (!found) break;

            b = Bunny{};
            b.live = true;
            b.cx = bcx;
            b.cz = bcz;
            b.sx = bx;
            b.sz = bz;
            b.x = bx;
            b.z = bz;
            // BEFORE groundUnder, which reads it: the footprint it samples is
            // rotated into the animal's own frame.
            b.th = rnd(uint32_t(i), 0xB1u) * 6.2831853f;
            b.g = groundUnder(b, ground);
            b.y = b.g;
            b.state = kSit;
            b.hold = kBunnySitMin + rnd(uint32_t(i), 0xB2u) * (kBunnySitMax - kBunnySitMin);
        }
    }

    // -----------------------------------------------------------------------
    // ONE BUNNY.
    // -----------------------------------------------------------------------
    template <typename GroundF>
    void step(Bunny *b, uint32_t i, float dt, const Vec3 &player, const GroundF &ground) {
        // -- A PERSON TOO CLOSE ------------------------------------------
        const float px = b->x - player.x, pz = b->z - player.z;
        const float py = b->y - (player.y - 1.0f);
        if (px * px + py * py + pz * pz < kBunnyThreatM * kBunnyThreatM)
            b->fleeUntil = clock_ + kBunnyFleeHold;
        const bool fleeing = clock_ < b->fleeUntil;

        b->t += dt * (fleeing ? kBunnyFleeMul : 1.0f);

        switch (b->state) {
            case kSit:
                // A STARTLED RABBIT DOES NOT SIT. It is the one state the flee
                // cuts short outright rather than merely speeding up.
                if (b->t < b->hold && !fleeing) break;
                choose(b, i, player, fleeing, ground);
                break;

            case kTurn: {
                const float u = saturate(b->t / kBunnyTurnSec);
                // Eased, so the turn starts and finishes at rest -- a rabbit
                // swivels, it does not spin at a constant rate.
                b->th = b->th0 + (b->th1 - b->th0) * (u * u * (3.0f - 2.0f * u));
                if (u >= 1.0f) {
                    b->th = b->th1;
                    // Straight into a hop when running: a turn that ends in a
                    // sit is how a fleeing animal gets caught.
                    if (fleeing) choose(b, i, player, true, ground);
                    else sit(b, i);
                }
                break;
            }

            case kHop: {
                const float u = saturate(b->t / kBunnyHopSec);
                b->x = b->hopFromX + sinf(b->th) * kBunnyHopM * u;
                b->z = b->hopFromZ + cosf(b->th) * kBunnyHopM * u;
                // THE ARC IS A SINE AND THE GROUND IS FOLLOWED UNDER IT, so a
                // bound across sloping ground lands on the slope rather than
                // at the height it took off from.
                if (u >= 1.0f) {
                    if (fleeing) choose(b, i, player, true, ground);
                    else sit(b, i);
                }
                break;
            }
        }

        // -- THE FLOOR, LAST AND UNCONDITIONALLY ----------------------------
        //
        // UP AT ONCE, DOWN GENTLY. A symmetric ease lags behind rising ground,
        // so every hop up a slope put the body under the terrain for a fraction
        // of a second -- and a wood is all slope, so that is a rabbit
        // permanently half buried. A thing standing ON the ground may never be
        // BELOW it, so the rise is not eased at all and only the fall is
        // smoothed. The butterflies' ground memory (kFlyGRefFallM) and the
        // songbirds' terrain follow are the same asymmetry for the same reason.
        //
        // AND IT IS THE LAST THING THAT HAPPENS, which is the other half. It
        // used to run inside the switch, BEFORE choose() could change the
        // heading -- so on the frame a hop began, the floor had been computed
        // for the footprint of the direction the animal was facing a moment
        // ago. Measured: 17 samples in 90,000 sat up to one voxel under the
        // ground, every one of them that frame. Applied here there is no state
        // change left for it to be stale against.
        // -- AND IF IT IS SOMEHOW INSIDE SOMETHING, IT COMES OUT -----------
        //
        // The fan refuses to hop into a boulder and the spawn refuses to be
        // born in one, which between them should make this unreachable. It is
        // here for the case neither covers: a chunk streaming in UNDER a bunny
        // that is already standing there. Steering cannot fix that -- the
        // animal is already inside -- so it is pushed out along the shortest
        // way, which is the same argument separateFish makes for the salmon.
        for (const Solid &sd : solidList()) {
            if (sd.top <= b->g + kBunnyStepM) continue;
            const float ex = sd.hx + hx_, ez = sd.hz + hz_;
            if (ex <= 0.0f || ez <= 0.0f) continue;
            const float ux = (b->x - sd.cx) / ex, uz = (b->z - sd.cz) / ez;
            const float q = ux * ux + uz * uz;
            if (q >= 1.0f || q < 1e-8f) continue;
            const float k = 1.0f / sqrtf(q);
            b->x = sd.cx + ux * ex * k;
            b->z = sd.cz + uz * ez * k;
        }

        const float gh = groundUnder(*b, ground);
        b->g = maxf(gh, b->g + (gh - b->g) * (1.0f - expf(-14.0f * dt)));
        // -- THE RISE, AND A TURN GETS IT TOO ------------------------------
        //
        // A TURN IS A HOP THAT GOES NOWHERE. The three strips are the SAME
        // ELEVEN FRAMES -- measured off the .vox files, identical bounding
        // boxes, identical centroids and 54 voxels on every frame of all
        // three -- so a swivelling rabbit is already DRAWING itself leaving
        // the ground. Only the body was not moving with it, and the result is
        // the one the user reported: the animal that hops in place does not
        // match the one that hops forward.
        //
        // PHASED ON THE STATE'S OWN CYCLE rather than on kBunnyHopSec for
        // both, so the two constants happening to be equal today is not
        // load-bearing -- if a turn is ever given its own duration the arc
        // still starts and ends with it instead of drifting out of phase.
        //
        // kSit is the only state left on the floor, which is right: a sitting
        // rabbit holds frame 0, and frame 0 is the crouch.
        const float cyc = (b->state == kTurn) ? kBunnyTurnSec : kBunnyHopSec;
        const float arc =
            (b->state == kSit) ? 0.0f : sinf(saturate(b->t / cyc) * 3.14159265f);
        b->y = b->g + arc * kBunnyHopRiseM;
    }

    void sit(Bunny *b, uint32_t i) {
        b->state = kSit;
        b->t = 0.0f;
        b->hold = kBunnySitMin + rnd(i, 0xB3u) * (kBunnySitMax - kBunnySitMin);
    }

    // -----------------------------------------------------------------------
    // WHAT TO DO NEXT: hop if the ground ahead takes one, turn if it does not.
    //
    // THE HEADING IS DECIDED HERE AND NOWHERE ELSE, which is what keeps the
    // three forces on it -- the leash, the flee and the plain wander -- from
    // fighting each other frame by frame. A rabbit commits to a direction and
    // then executes it.
    // -----------------------------------------------------------------------
    template <typename GroundF>
    void choose(Bunny *b, uint32_t i, const Vec3 &player, bool fleeing, const GroundF &ground) {
        float want = b->th;
        const float lx = b->x - b->sx, lz = b->z - b->sz;
        const bool outside = lx * lx + lz * lz > kBunnyLeashM * kBunnyLeashM;
        if (fleeing) {
            // Directly away, with a little scatter so a pair startled together
            // does not leave along one line.
            want = atan2f(b->x - player.x, b->z - player.z) +
                   (rnd(i, 0xB4u) - 0.5f) * 0.8f;
        } else if (outside) {
            // HOME BEATS THE WANDER. The slot is recycled on the SITE, so a
            // bunny that had wandered a hundred metres off would be taken away
            // while it was still in plain view -- the same trap the lily pads
            // needed a leash for.
            want = atan2f(-lx, -lz) + (rnd(i, 0xB5u) - 0.5f) * 0.7f;
        } else {
            want = b->th + (rnd(i, 0xB6u) - 0.5f) * 2.4f;
        }

        // -- THE WHISKER FAN, SWEPT ABOUT WHAT IT WANTED ------------------
        //
        // The intent above is what it would LIKE to do; this is what the world
        // will let it. Seven headings measured, the freest wins, and a bias
        // toward the intent so a bunny with open ground all round still goes
        // where it meant to.
        {
            float best = -1e9f, bestTh = want;
            for (int k = -kBunnyWhiskers; k <= kBunnyWhiskers; ++k) {
                const float off = float(k) * kBunnyWhiskerRad;
                const float sc = clearAhead(*b, want + off, ground) - fabsf(off) * 0.35f;
                if (sc > best) { best = sc; bestTh = want + off; }
            }
            want = bestTh;
        }

        // Is the ground a bound away actually landable, and is anything in it?
        const float nx = b->x + sinf(want) * kBunnyHopM;
        const float nz = b->z + cosf(want) * kBunnyHopM;
        const bool landable =
            fabsf(ground(nx, nz) - b->g) <= kBunnyStepM && !blocked(nx, nz, b->g);

        const float roll = rnd(i, 0xB7u);
        const bool wantHop = fleeing || roll < kBunnyHopChance;
        // A TURN IS ALSO THE ANSWER TO A WALL, which is why the two are decided
        // together: a bunny facing a step it cannot take turns away from it
        // rather than standing there re-rolling a hop it will never be allowed.
        if (wantHop && landable && fabsf(angleTo(want - b->th)) < 0.5f) {
            b->state = kHop;
            b->t = 0.0f;
            b->th = want;
            b->hopFromX = b->x;
            b->hopFromZ = b->z;
            return;
        }

        b->state = kTurn;
        b->t = 0.0f;
        b->th0 = b->th;
        const float err = angleTo(want - b->th);
        // ...capped to one cycle of the strip, so several turns in a row is how
        // a big change of heading happens. The strip is what it is.
        const float d = clampf(err, -kBunnyTurnRad, kBunnyTurnRad);
        b->th1 = b->th + (fabsf(d) < 0.05f ? (rnd(i, 0xB8u) < 0.5f ? -kBunnyTurnRad : kBunnyTurnRad)
                                           : d);
        b->dirRight = (b->th1 > b->th) ? 1 : 0;
    }

    static float angleTo(float d) { return atan2f(sinf(d), cosf(d)); }

    // -----------------------------------------------------------------------
    // THE INSTANCE.
    //
    // WHICH STRIP IS PLAYING IS THE STATE, and the frame within it is the
    // state's own progress rather than a free-running clock -- a hop has to
    // land on the last frame exactly as the body lands, or the animal snaps.
    // -----------------------------------------------------------------------
    void put(World &world, int slot, const Bunny &b) const {
        if (!b.live || hop_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        int si = kStripHop;
        float u = 0.0f;
        if (b.state == kHop) {
            u = saturate(b.t / kBunnyHopSec);
        } else if (b.state == kTurn) {
            si = b.dirRight ? kStripTurnR : kStripTurnL;
            if (stripOf(si).empty()) si = kStripHop;
            u = saturate(b.t / kBunnyTurnSec);
        }
        const int n = frames(si);
        const int fi = maxi(0, mini(n - 1, int(u * float(n))));
        const BunnyPose p =
            pose(si, fi, b.th, fadeOf(b.age, b.dying), Vec3(b.x, b.y, b.z), nullptr);
        if (p.model < 0) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        // THE ANIMAL, NOT ITS BOX, AND HERE IT MATTERS. pose() takes the
        // horizontal half-box off and deliberately leaves the VERTICAL one on,
        // because what has to land on b.y is the rabbit's FEET -- so the centre
        // place() derived rose and fell with each frame's own height through
        // the hop and turn strips, which are not one size, and did it again
        // with the fade. None of that is the animal moving. See World::place,
        // and pose(), which works this point out with the bake's nudge IN it.
        const float anchor[3] = {p.ax, p.ay, p.az};
        world.setFlyerInstance(slot, p.model, p.m, p.tx, p.ty, p.tz, nullptr, true, nullptr,
                               anchor);
    }

    // -----------------------------------------------------------------------
    // base.vox IS SOURCE ART, NOT A FRAME -- the same rule the salmon, the
    // dragonfly and the songbirds' strips follow.
    // -----------------------------------------------------------------------
    void loadStrip(World &world, const std::string &dir, int frames, std::vector<Frame> *out,
                   const char *what, float *hxOut, float *hzOut) {
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
            // MEASURED, NOT ASSUMED. Only the horizontal half is wanted and it
            // is the same in every frame of every strip -- the frames differ in
            // HEIGHT (8 and 10 rows), which is precisely why the vertical half
            // must not be used for anything.
            // ...AND INTO WHICHEVER ANIMAL ASKED. It used to write the class's
            // own pair, which was fine while there was one animal in this file
            // and silently wrong the moment there were two: the skunk's strip
            // loads last, so the rabbit would have gone hunting for boulders
            // with a skunk's shoulders.
            if (hxOut) *hxOut = 0.5f * float(sx) * VOXEL_M;
            if (hzOut) *hzOut = 0.5f * float(sz) * VOXEL_M;
            Frame fr;
            fr.model = m;
            fr.sx = sx;
            fr.sy = sy;
            fr.sz = sz;
            out->push_back(fr);
        }
    }

    // Not owned: see update(). Null until the first tick, which is why every
    // reader goes through the accessor rather than touching it -- a sensor that
    // dereferences a null list on frame one is a sensor nobody would trust.
    const std::vector<Solid> *solids_ = nullptr;
    // Is the ground here a beach? See blocked(). Optional -- an empty one means
    // "no beaches anywhere", which is what the offline report and the deck want.
    std::function<bool(float, float)> sand_;
    // Not owned in spirit -- rebound every tick from update(). Empty until the
    // first one, and blocked() checks it before calling.
    std::function<bool(float, float)> wet_;
    const std::vector<Solid> &solidList() const {
        static const std::vector<Solid> kNone;
        return solids_ ? *solids_ : kNone;
    }
    std::vector<Frame> hop_, left_, right_;
    std::vector<Bunny> buns_;
    // ...AND THE MARCHER'S OWN STRIP AND BODY. Kept apart from the rabbit's for
    // the reason loadStrip's out-params exist: two animals, two footprints, and
    // a sensor asked with the wrong one is a sensor that lies.
    // ONE STRIP AND ONE BODY PER SPECIES. Not one of each for the class: the
    // strips load in table order, so a shared half-box would leave every animal
    // wearing the last one's shoulders -- the bug loadStrip's out-params were
    // added to prevent when there were two of them.
    std::vector<Frame> march_[kMarchKinds];
    std::vector<March> marchers_;
    float marchHX_[kMarchKinds] = {0.2f, 0.2f, 0.2f, 0.2f};
    float marchHZ_[kMarchKinds] = {0.45f, 0.45f, 0.45f, 0.45f};
    // WHICH WOOD THIS POINT IS IN, 0 pine .. 1 birch. Held the way `wet_` is
    // and for the same reason: the fill asks it per candidate site and nothing
    // else in this file cares, so threading a third template parameter through
    // every sensor to save an indirect call would spread it over the class.
    std::function<uint8_t(float)> wood_;
    float clock_ = 0.0f;
    float hx_ = 0.25f, hz_ = 0.4f;   // half the body, across and along -- see loadStrip
    bool ready_ = false;
};

}  // namespace v2
