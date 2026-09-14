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

// -- WHICH STRIP ------------------------------------------------------------
// An index rather than three names, because the asset editor cycles through
// them and the bake tables are looked up by it.
inline constexpr int kBunnyStrips = 3;
enum BunnyStrip { kStripHop = 0, kStripTurnL = 1, kStripTurnR = 2 };
inline const char *bunnyStripName(int s) {
    static const char *kN[kBunnyStrips] = {"jump", "rotate/left", "rotate/right"};
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
// [I] on the deck, then [C]. Identity as it stands: every slot plays its own
// frame, unmoved and unturned, which is the strip exactly as authored.
inline constexpr BunnyBake kBunnyHopBake[kBunnyJumpFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 0, 0, 0, 0},
    {4, 0, 0, 0, 0, 0},  {5, 0, 0, 0, 0, 0},  {6, 0, 0, 0, 0, 0},  {7, 0, 0, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
inline constexpr BunnyBake kBunnyTurnLBake[kBunnyTurnFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 0, 0, 0, 0},
    {4, 0, 0, 0, 0, 0},  {5, 0, 0, 0, 0, 0},  {6, 0, 0, 0, 0, 0},  {7, 0, 0, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
inline constexpr BunnyBake kBunnyTurnRBake[kBunnyTurnFrames] = {
    {0, 0, 0, 0, 0, 0},  {1, 0, 0, 0, 0, 0},  {2, 0, 0, 0, 0, 0},  {3, 0, 0, 0, 0, 0},
    {4, 0, 0, 0, 0, 0},  {5, 0, 0, 0, 0, 0},  {6, 0, 0, 0, 0, 0},  {7, 0, 0, 0, 0, 0},
    {8, 0, 0, 0, 0, 0},  {9, 0, 0, 0, 0, 0},  {10, 0, 0, 0, 0, 0},
};
// -- ...AND HERE -----------------------------------------------------------

inline const BunnyBake *bunnyBake(int strip) {
    return strip == kStripTurnL ? kBunnyTurnLBake
         : strip == kStripTurnR ? kBunnyTurnRBake
                                : kBunnyHopBake;
}
// How many rows that table actually has. The strips and the tables agree
// today; this is what keeps a folder that grows a twelfth frame from reading
// off the end of one of them.
inline constexpr int bunnyBakeCount(int strip) {
    return strip == kStripHop ? kBunnyJumpFrames : kBunnyTurnFrames;
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
inline constexpr int kBunnyCount = 10;
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
    // -----------------------------------------------------------------------
    // Three strips out of one folder. A missing one is a warning and an empty
    // population, never a crash -- the same contract every other strip loader
    // in this engine keeps.
    // -----------------------------------------------------------------------
    bool load(World &world, const std::string &dir) {
        loadStrip(world, dir + "/jump", kBunnyJumpFrames, &hop_, "bunny jump");
        loadStrip(world, dir + "/rotate/left", kBunnyTurnFrames, &left_, "bunny turn left");
        loadStrip(world, dir + "/rotate/right", kBunnyTurnFrames, &right_, "bunny turn right");
        buns_.assign(size_t(kBunnyCount), Bunny{});
        ready_ = !hop_.empty();
        if (ready_)
            std::printf("  bunny    %zu hop frames, %zu + %zu turn frames, %d slots\n",
                        hop_.size(), left_.size(), right_.size(), kBunnyCount);
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
    template <typename GroundF, typename WetF>
    void update(float dt, const Vec3 &player, const GroundF &ground, const WetF &wet,
                const std::vector<Solid> &solids) {
        if (!ready_) return;
        wet_ = wet;
        // BORROWED, NOT COPIED, and only for this call. It is the same list the
        // perched birds are handed -- gathered once every half second because
        // trees and boulders do not move, and re-gathering it per animal per
        // frame is the wide query this engine already went out of its way to
        // avoid paying twice.
        solids_ = &solids;
        clock_ += dt;
        recycle(player, dt);
        fill(player, ground);
        for (size_t i = 0; i < buns_.size(); ++i)
            if (buns_[i].live) step(&buns_[i], uint32_t(i), dt, player, ground);
    }

    // ...and onto the band. Every slot, empty ones included: a slot that has
    // just been vacated has to be told it is empty or it keeps what was in it.
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
        BunnyPose p;
        const std::vector<Frame> &st = stripOf(si);
        if (st.empty()) return p;
        const int n = int(st.size());
        const int slot = maxi(0, mini(n - 1, slotIdx));
        const BunnyBake bk = bake ? bake[slot] : bunnyBake(si)[slot];
        const Frame &f = st[size_t(maxi(0, mini(n - 1, bk.src)))];
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

        // -- AND THE BAKE'S OWN NUDGE, IN THE ANIMAL'S FRAME ----------------
        //
        // H, not the pose matrix: "forward" has to mean the way the rabbit is
        // going, whatever pose it happens to be holding. A hop that travelled
        // along the tipped-over axis of frame 7 would be a bake nobody could
        // reason about. The fade is deliberately not applied either -- this is
        // where the animal is, not part of its body.
        const float d[3] = {float(bk.ox) * VOXEL_M, float(bk.oy) * VOXEL_M,
                            float(bk.oz) * VOXEL_M};
        p.tx += H[0] * d[0] + H[1] * d[1] + H[2] * d[2];
        p.ty += H[3] * d[0] + H[4] * d[1] + H[5] * d[2];
        p.tz += H[6] * d[0] + H[7] * d[1] + H[8] * d[2];
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
    struct Frame {
        int model = -1;
        int sx = 0, sy = 0, sz = 0;
    };
    const std::vector<Frame> &stripOf(int si) const {
        return si == kStripTurnL ? left_ : si == kStripTurnR ? right_ : hop_;
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
    bool blocked(float x, float z, float feetY) const {
        // WATER IS AN OBSTACLE LIKE ANY OTHER, and saying it here rather than in
        // each caller is what makes it apply to all of them at once: the
        // whisker fan will not point at a lake, a hop will not be taken into
        // one, and a site on one is never claimed.
        if (wet_ && wet_(x, z)) return true;
        const float probeY = feetY + 0.2f;   // knee height: what a body would meet
        for (const Solid &s : solidList()) {
            if (s.top <= feetY + kBunnyStepM) continue;   // low enough to hop onto
            if (s.hx <= 0.0f || s.hz <= 0.0f) continue;
            if (s.vol) {
                // The centre and four points on the body's rim. A single centre
                // probe lets a boulder's overhang sit inside the animal.
                if (solidAtWorld(s, x, probeY, z, VOXEL_M)) return true;
                if (solidAtWorld(s, x + hx_, probeY, z, VOXEL_M)) return true;
                if (solidAtWorld(s, x - hx_, probeY, z, VOXEL_M)) return true;
                if (solidAtWorld(s, x, probeY, z + hz_, VOXEL_M)) return true;
                if (solidAtWorld(s, x, probeY, z - hz_, VOXEL_M)) return true;
                continue;
            }
            const float ex = s.hx + hx_, ez = s.hz + hz_;
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
        const float sn = sinf(th), c = cosf(th);
        for (float d = kBunnySenseStep; d <= kBunnySenseM; d += kBunnySenseStep) {
            const float x = b.x + sn * d, z = b.z + c * d;
            if (blocked(x, z, b.g)) return d - kBunnySenseStep;
            // A wall of terrain stops it just as a boulder does, and for the
            // animal there is no difference worth drawing between the two.
            if (fabsf(ground(x, z) - b.g) > kBunnyStepM) return d - kBunnySenseStep;
        }
        return kBunnySenseM;
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
        const float c = cosf(b.th), sn = sinf(b.th);
        // Half the body, in its own frame: across, and along the nose.
        const float ax = hx_, az = hz_;
        float g = ground(b.x, b.z);
        const float ox[4] = {ax, -ax, ax, -ax};
        const float oz[4] = {az, az, -az, -az};
        for (int i = 0; i < 4; ++i) {
            const float wx = b.x + ox[i] * c + oz[i] * sn;
            const float wz = b.z - ox[i] * sn + oz[i] * c;
            g = maxf(g, ground(wx, wz));
        }
        return g;
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
            // Nearest first, so the population fills the wood you are looking
            // at before the wood behind the hill.
            int bcx = 0, bcz = 0;
            float bx = 0, bz = 0, best = kBunnySpawnM * kBunnySpawnM;
            bool found = false;
            for (int dz = -r; dz <= r; ++dz)
                for (int dx = -r; dx <= r; ++dx) {
                    const int cx = c0x + dx, cz = c0z + dz;
                    if (claimed(cx, cz)) continue;
                    float sx = 0, sz = 0;
                    siteOf(kBunnyCellM, kBunnySalt, cx, cz, &sx, &sz);
                    const float ex = sx - player.x, ez = sz - player.z;
                    const float d2 = ex * ex + ez * ez;
                    if (d2 >= best) continue;
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
                    best = d2;
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
        const float arc =
            (b->state == kHop) ? sinf(saturate(b->t / kBunnyHopSec) * 3.14159265f) : 0.0f;
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
        world.setFlyerInstance(slot, p.model, p.m, p.tx, p.ty, p.tz, nullptr, true);
    }

    // -----------------------------------------------------------------------
    // base.vox IS SOURCE ART, NOT A FRAME -- the same rule the salmon, the
    // dragonfly and the songbirds' strips follow.
    // -----------------------------------------------------------------------
    void loadStrip(World &world, const std::string &dir, int frames, std::vector<Frame> *out,
                   const char *what) {
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
            const int m = world.addFlyerModel(mo[size_t(f)], what, &sx, &sy, &sz);
            if (m < 0) { out->clear(); return; }
            // MEASURED, NOT ASSUMED. Only the horizontal half is wanted and it
            // is the same in every frame of every strip -- the frames differ in
            // HEIGHT (8 and 10 rows), which is precisely why the vertical half
            // must not be used for anything.
            hx_ = 0.5f * float(sx) * VOXEL_M;
            hz_ = 0.5f * float(sz) * VOXEL_M;
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
    // Not owned in spirit -- rebound every tick from update(). Empty until the
    // first one, and blocked() checks it before calling.
    std::function<bool(float, float)> wet_;
    const std::vector<Solid> &solidList() const {
        static const std::vector<Solid> kNone;
        return solids_ ? *solids_ : kNone;
    }
    std::vector<Frame> hop_, left_, right_;
    std::vector<Bunny> buns_;
    float clock_ = 0.0f;
    float hx_ = 0.25f, hz_ = 0.4f;   // half the body, across and along -- see loadStrip
    bool ready_ = false;
};

}  // namespace v2
