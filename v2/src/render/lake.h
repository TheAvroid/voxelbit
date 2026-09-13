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

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../gpu/world.h"
#include "../scene/vox.h"
#include "../scene/voxelworld.h"

namespace v2 {

// -- THE MODELS -------------------------------------------------------------
//
// assets/life/salmon holds 00..11 plus base.vox, and base is SOURCE ART rather
// than a frame -- the JS engine skips it by name and so does this. The
// dragonfly is six frames the same way. The pads are three separate models,
// not a strip.
inline constexpr int kSalmonFrames = 12;
inline constexpr int kDflyFrames = 6;
inline constexpr int kLilyModels = 3;

// -- HOW MANY OF EACH ARE ALIVE AT ONCE -------------------------------------
//
// Small, and deliberately: these are things you come across on one lake, not a
// population. The flyer band is a fixed reservation (see kFlyerInstances), so
// every slot here costs a TLAS entry whether or not anything is in it.
inline constexpr int kSalmonCount = 10;
inline constexpr int kLilyCount = 12;
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

// How far any of this lives from the player before its slot is recycled, and
// how far out water is looked for. The BIRDS' own lesson applies (see
// v2-birds-popped-and-flickered): the gather has to be wider than the drop or
// a thing is recycled for want of data rather than for want of water.
// WIDER THAN IT WAS. Thirty metres of a lake is not much water to put thirty
// creatures in, and the result was exactly what it sounds like -- see the
// separation rule in fill().
inline constexpr float kLakePlaceM = 45.0f;
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
inline constexpr float kLakeMinPlaceM = 30.0f;
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
inline constexpr float kLakeDropM = 100.0f;
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
inline constexpr float kLakeFieldM = 220.0f;  // the sampled square, per side

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
    static constexpr float kCellM = 1.0f;
    static constexpr int kN = int(kLakeFieldM / kCellM);   // 80 x 80 = 6,400 columns

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
    static constexpr float kSafeM = kLakeFieldM * 0.5f - kLakeDropM;

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
        built_ = true;
    }

    bool any() const { return any_; }
    bool built() const { return built_; }

    // Is there water here, and how deep? Outside the field the answer is NO --
    // which is the safe direction: a fish that swims off the edge of what we
    // know is turned back rather than allowed to leave the lake.
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

    // HOW FAR OPEN WATER RUNS ALONG A HEADING, in metres, to a limit. The JS
    // engine's fishReach, and it is the compass for every steering decision the
    // fish makes: cruise picks the freest whisker, flee picks the freest escape.
    float reach(float x, float z, float th, float maxM) const {
        const float sx = sinf(th), sz = cosf(th);
        float d = kCellM;
        while (d <= maxM) {
            if (!at(x + sx * d, z + sz * d)) return d - kCellM;
            d += kCellM;
        }
        return maxM;
    }

    // One wet cell, picked by a hash -- for spawning. Returns false on a dry
    // field, which is most of the world.
    bool pick(uint32_t h, float *x, float *z, float *topM, float *bedM) const {
        if (!any_) return false;
        for (int tries = 0; tries < 24; ++tries) {
            const uint32_t q = hashU32(h, uint32_t(tries));
            const int i = int(q % uint32_t(kN)), j = int((q / uint32_t(kN)) % uint32_t(kN));
            const size_t k = size_t(j) * kN + size_t(i);
            if (!wet_[k]) continue;
            *x = cx_ + (float(i) - kN * 0.5f) * kCellM;
            *z = cz_ + (float(j) - kN * 0.5f) * kCellM;
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
        }

        fish_.resize(kSalmonCount);
        pads_.resize(kLilyCount);
        flies_.resize(kDflyCount);
        ready_ = !salmon_.empty() || !lily_.empty() || !dfly_.empty();
        if (ready_)
            std::printf("  lake     %zu salmon frames, %zu lily models, %zu dragonfly frames\n",
                        salmon_.size(), lily_.size(), dfly_.size());
        return ready_;
    }

    // -----------------------------------------------------------------------
    // ONE TICK. The field first, because everything else reads it.
    // -----------------------------------------------------------------------
    void update(float dt, const VoxelTerrain &terrain, const Vec3 &player) {
        if (!ready_) return;
        clock_ += dt;
        if (field_.stale(player)) field_.rebuild(terrain, player);
        recycle(player);
        fill(player);
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
        for (Fish &f : fish_) if (f.live && inField(f.x, f.z)) stepFish(&f, dt, player);
        for (Pad &p : pads_) if (p.live && inField(p.x, p.z)) stepPad(&p, dt);
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
    void publish(World &world) {
        if (!ready_) return;
        const int base = kButterflySlots + kBirdSlots;
        int slot = base;
        for (const Fish &f : fish_) putFish(world, slot++, f);
        for (const Pad &p : pads_) putPad(world, slot++, p);
        for (const Dfly &d : flies_) putDfly(world, slot++, d);
        world.flushFlyerInstances();
    }

    int living() const {
        int n = 0;
        for (const Fish &f : fish_) n += f.live;
        for (const Pad &p : pads_) n += p.live;
        for (const Dfly &d : flies_) n += d.live;
        return n;
    }

  private:
    // -- one creature each ---------------------------------------------------
    struct Fish {
        bool live = false;
        float x = 0, y = 0, z = 0;
        float th = 0;        // heading
        float om = 0;        // turn rate, eased
        float spd = kFishCruise;
        float pitch = 0;
        float vy = 0;
        float hold = 0;      // the depth it is holding, 0..1 of the column
        float animClk = 0;   // frames, on the SIM clock -- see stepFish
        float fleeUntil = -1.0f;
        float thrX = 0, thrZ = 0;
        float senseAt = 0, holdAt = 0, fleeAt = 0;
        float navTh = 0;     // long-range intent, bent by the whiskers
        // -- THE LEAP. jumpV is only meaningful while airborne; jumpOn says so
        //    rather than a sentinel value, because 0 is a real vertical speed
        //    at the top of the arc.
        bool jumpOn = false;
        float jumpV = 0.0f;
        float jumpAt = 0.0f;   // when the next attempt is allowed
        int pose = 0, poseWas = 0;
    };
    struct Pad {
        bool live = false;
        float x = 0, y = 0, z = 0;
        float th = 0;        // the MODEL's spin -- independent of where it drifts
        float spin = 0;
        float mth = 0;       // the drift heading
        float turnAt = 0;
        int model = 0;
    };
    struct Dfly {
        bool live = false;
        float x = 0, y = 0, z = 0;
        float hx = 0, hz = 0;   // its home stretch of water
        float th = 0, wantTh = 0;
        float turnAt = 0;
        float phase = 0;
        float fleeUntil = -1.0f;   // see kDflyFleeHold
        float stuck = 0.0f;        // seconds of getting nowhere -- see kDflyStuckSec
        int pose = 0, poseWas = 0; // for the wing motion vector
    };

    // =======================================================================
    // THE FISH.
    // =======================================================================
    void stepFish(Fish *f, float dt, const Vec3 &player) {
        // -- AIRBORNE: PURE BALLISTICS, AND NOTHING ELSE RUNS ---------------
        //
        // No steering, no whiskers, no depth servo. v1 is explicit that the
        // arc is validated at launch and then flown: bending the heading in
        // the air "would curve the arc off its validated splash-down".
        if (f->jumpOn) {
            f->y += f->jumpV * dt;
            f->jumpV -= kJumpGrav * dt;
            f->x += sinf(f->th) * kJumpFwd * dt;
            f->z += cosf(f->th) * kJumpFwd * dt;
            // The nose follows the arc, which is what makes a leap read as one
            // -- up on the way up, down on the way in.
            f->pitch = clampf(atan2f(f->jumpV, kJumpFwd), -1.2f, 1.2f);
            // The tail keeps beating in the air. A fish that freezes mid-leap
            // is a prop.
            f->animClk += dt * kFishAnimFps;
            float topM = 0.0f, bedM = 0.0f;
            const bool over = field_.at(f->x, f->z, &topM, &bedM);
            // REENTRY: falling, and back through the surface. If it has drifted
            // off the water entirely the arc is abandoned at the top rather
            // than flown into a bank.
            if (!over) { f->jumpOn = false; f->y = f->y; return; }
            if (f->jumpV < 0.0f && f->y <= topM - 0.14f) {
                f->jumpOn = false;
                f->y = topM - 0.14f;
                f->vy = 0.0f;
                f->pitch = 0.0f;
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
                    const float sc = field_.reach(f->x, f->z, away + off, 12.0f) - fabsf(off) * 0.6f;
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
                const float sc = field_.reach(f->x, f->z, f->th + off, 10.0f) - fabsf(off) * 0.5f;
                if (sc > best) { best = sc; bestTh = f->th + off; }
            }
            f->navTh = bestTh;
            wantTh = bestTh;
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

        // -- SPEED: THE KICK IS FAST, THE BLEED-OFF SLOW --------------------
        //
        // The JS engine's asymmetric ease, and its comment is the design:
        // "dart, then coast". A symmetric one makes a startled fish look like
        // it is being towed.
        const float spdT = kFishCruise * (fleeing ? kFishFleeMul : 1.0f);
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
        if (field_.at(nx, nz, &topM, &bedM)) {
            f->x = nx;
            f->z = nz;
        } else {
            // A bank in its face. Turn hard rather than stop -- a fish that
            // stalls against a shore reads as stuck, and the whiskers will have
            // it pointed at open water within a tick or two anyway.
            f->th += 2.2f * dt * 3.0f;
            field_.at(f->x, f->z, &topM, &bedM);
        }

        // THE BODY STAYS UNDER THE SURFACE AND OFF THE BED. Half a body length
        // of clearance at each, or the strip's own voxels break the water.
        const float lo = bedM + 0.12f, hi = topM - 0.14f;
        const float wantY = (hi > lo) ? lo + (hi - lo) * f->hold : (lo + hi) * 0.5f;
        const float prevY = f->y;
        f->y += (wantY - f->y) * (1.0f - expf(-1.8f * dt));
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
            if (!fleeing && deep) {
                const uint32_t h = hashU32(uint32_t(clock_ * 311.0f), uint32_t(f->x * 53.0f));
                f->jumpOn = true;
                f->jumpV = kJumpVMin + hashUnit(0x7A1u, h) * (kJumpVMax - kJumpVMin);
                // OUT OF THE SURFACE, not out of wherever it was holding. A
                // salmon leaving from a metre down is a fish teleporting.
                f->y = topM - 0.14f;
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
    // THE LILY PADS.
    // =======================================================================
    void stepPad(Pad *p, float dt) {
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
        if (!field_.at(p->x + sinf(p->mth) * 1.5f, p->z + cosf(p->mth) * 1.5f))
            p->mth += kLilyShoreTurn * dt;

        const float nx = p->x + sinf(p->mth) * kLilyDrift * dt;
        const float nz = p->z + cosf(p->mth) * kLilyDrift * dt;
        float topM = 0.0f;
        if (field_.at(nx, nz, &topM)) {
            p->x = nx;
            p->z = nz;
            // IT FLOATS, so its underside sits ON the surface rather than in
            // it. The model's own half height is what keeps a thick pad from
            // being half drowned.
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
        const float spd = kDflySpeed * (clock_ < d->fleeUntil ? kDflyFleeMul : 1.0f);

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

    // =======================================================================
    // RECYCLING, AND IT FOLLOWS THE BIRDS' HARD-WON RULE.
    //
    // A slot is given up only when it is well outside where it can be made out,
    // and it is NEVER given up before its replacement exists -- see
    // v2-birds-popped-and-flickered, which is the same mistake made once
    // already in this engine. fill() below only ever writes a slot it has a
    // real spot for.
    // =======================================================================
    void recycle(const Vec3 &player) {
        const float d2 = kLakeDropM * kLakeDropM;
        auto far2 = [&](float x, float z) {
            const float dx = x - player.x, dz = z - player.z;
            return dx * dx + dz * dz > d2;
        };
        for (Fish &f : fish_) if (f.live && far2(f.x, f.z)) f.live = false;
        for (Pad &p : pads_) if (p.live && far2(p.x, p.z)) p.live = false;
        for (Dfly &d : flies_) if (d.live && far2(d.hx, d.hz)) d.live = false;
    }

    void fill(const Vec3 &player) {
        if (!field_.any()) return;
        const uint32_t t = uint32_t(clock_ * 7.0f);

        for (size_t i = 0; i < fish_.size(); ++i) {
            Fish &f = fish_[i];
            if (f.live || salmon_.empty()) continue;
            float x = 0, z = 0, topM = 0, bedM = 0;
            if (!pickNear(player, hashU32(0xF15Bu ^ uint32_t(i), t), kFish, &x, &z, &topM, &bedM))
                continue;
            // DEEP ENOUGH TO HOLD A FISH. Under thirty centimetres it would be
            // swimming with its back out, which is the one thing that makes a
            // fish read as a bug rather than as a fish.
            if (topM - bedM < 0.30f) continue;
            f = Fish{};
            f.live = true;
            f.x = x;
            f.z = z;
            f.y = (topM + bedM) * 0.5f;
            f.th = hashUnit(0x11u, hashU32(uint32_t(i), t)) * 6.2831853f;
            f.navTh = f.th;
            f.hold = 0.4f;
            f.animClk = hashUnit(0x12u, hashU32(uint32_t(i), t)) * float(kSalmonFrames);
        }

        for (size_t i = 0; i < pads_.size(); ++i) {
            Pad &p = pads_[i];
            if (p.live || lily_.empty()) continue;
            float x = 0, z = 0, topM = 0, bedM = 0;
            if (!pickNear(player, hashU32(0x11A9u ^ uint32_t(i), t), kPad, &x, &z, &topM, &bedM))
                continue;
            const uint32_t h = hashU32(0x11AAu ^ uint32_t(i), t);
            p = Pad{};
            p.live = true;
            p.x = x;
            p.z = z;
            p.y = topM;
            p.model = int(hashUnit(0x21u, h) * float(lily_.size())) % int(lily_.size());
            p.th = hashUnit(0x22u, h) * 6.2831853f;
            // Its own rate AND its own direction -- two pads turning together
            // reads as a mechanism.
            p.spin = (hashUnit(0x23u, h) - 0.5f) * 2.0f * kLilySpinMax;
            p.mth = hashUnit(0x24u, h) * 6.2831853f;
        }

        for (size_t i = 0; i < flies_.size(); ++i) {
            Dfly &d = flies_[i];
            if (d.live || dfly_.empty()) continue;
            float x = 0, z = 0, topM = 0, bedM = 0;
            if (!pickNear(player, hashU32(0xD91Fu ^ uint32_t(i), t), kDfly, &x, &z, &topM, &bedM))
                continue;
            const uint32_t h = hashU32(0xD920u ^ uint32_t(i), t);
            d = Dfly{};
            d.live = true;
            d.hx = x;
            d.hz = z;
            d.x = x;
            d.z = z;
            d.y = topM + kDflyLoM;
            d.th = hashUnit(0x31u, h) * 6.2831853f;
            d.wantTh = d.th;
            d.phase = hashUnit(0x32u, h) * 6.2831853f;
        }
    }

    // A wet spot inside the PLACE radius. The field covers the drop radius, so
    // this is what stops everything spawning at the far edge of what is known
    // and then immediately being recycled.
    // ...AND FAR ENOUGH FROM EVERYTHING THAT IS ALREADY THERE.
    //
    // FORTY TRIES, NOT TWELVE. A separation rule makes a spot much harder to
    // find -- the last of thirty has to miss twenty-nine neighbours -- and
    // giving up early does not clump them, it simply leaves slots empty, which
    // is the failure that is easy to miss.
    enum Kind { kFish = 0, kPad = 1, kDfly = 2 };

    bool pickNear(const Vec3 &player, uint32_t h, Kind kind, float *x, float *z, float *topM,
                  float *bedM) {
        for (int tries = 0; tries < 40; ++tries) {
            if (!field_.pick(hashU32(h, uint32_t(tries)), x, z, topM, bedM)) return false;
            const float dx = *x - player.x, dz = *z - player.z;
            const float d2 = dx * dx + dz * dz;
            if (d2 > kLakePlaceM * kLakePlaceM) continue;
            if (d2 < kLakeMinPlaceM * kLakeMinPlaceM) continue;   // see kLakeMinPlaceM
            if (crowded(*x, *z, kind)) continue;
            return true;
        }
        return false;
    }

    // Is anything living too close to this spot? Its OWN kind has to be a
    // proper distance away; a different one only has to not intersect.
    bool crowded(float x, float z, Kind kind) const {
        auto hit = [&](float ox, float oz, bool sameKind) {
            const float r = sameKind ? kLakeApartM : kLakeApartAnyM;
            const float dx = x - ox, dz = z - oz;
            return dx * dx + dz * dz < r * r;
        };
        for (const Fish &f : fish_)
            if (f.live && hit(f.x, f.z, kind == kFish)) return true;
        for (const Pad &p : pads_)
            if (p.live && hit(p.x, p.z, kind == kPad)) return true;
        for (const Dfly &d : flies_)
            if (d.live && hit(d.hx, d.hz, kind == kDfly)) return true;
        return false;
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
    static void yawMat(float th, float *m) {
        const float c = cosf(th + 3.14159265f), s = sinf(th + 3.14159265f);
        m[0] = c;  m[1] = 0; m[2] = s;
        m[3] = 0;  m[4] = 1; m[5] = 0;
        m[6] = -s; m[7] = 0; m[8] = c;
    }

    void putFish(World &world, int slot, const Fish &f) const {
        if (!f.live || salmon_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const int fi = int(f.animClk) % int(salmon_.size());
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
        // the pitch is capped at 0.30 rad so fwd can never point straight up.
        const float rl = maxf(1e-4f, sqrtf(fz * fz + fx * fx));
        const float rx = fz / rl, rz = -fx / rl;
        // up' = fwd x right
        const float ux = fy * rz, uy = fz * rx - fx * rz, uz = -fy * rx;
        const float m[9] = {-rx, ux, -fx,
                            0.0f, uy, -fy,
                            -rz, uz, -fz};
        world.setFlyerInstance(slot, salmon_[size_t(fi)], m, f.x, f.y, f.z, nullptr, true);
    }

    void putPad(World &world, int slot, const Pad &p) const {
        if (!p.live || lily_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        float m[9];
        yawMat(p.th, m);
        world.setFlyerInstance(slot, lily_[size_t(p.model)], m, p.x, p.y, p.z, nullptr, true);
    }

    void putDfly(World &world, int slot, const Dfly &d) const {
        if (!d.live || dfly_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const int fi = int(clock_ * kDflyFps + d.phase) % int(dfly_.size());
        float m[9];
        yawMat(d.th, m);
        world.setFlyerInstance(slot, dfly_[size_t(fi)], m, d.x, d.y, d.z, nullptr, true);
    }

    // -----------------------------------------------------------------------
    // base.vox IS SOURCE ART, NOT A FRAME. Both strips ship one and the JS
    // engine skips it by name; loading it would put a still pose in the middle
    // of the cycle once per second.
    // -----------------------------------------------------------------------
    void loadStrip(World &world, const std::string &dir, int frames, std::vector<int> *out,
                   const char *what) {
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
            const int m = world.addFlyerModel(mo[size_t(f)], what, &sx, &sy, &sz);
            if (m < 0) { out->clear(); return; }
            out->push_back(m);
        }
    }

    bool ready_ = false;
    float clock_ = 0.0f;
    WaterField field_;
    std::vector<int> salmon_, dfly_, lily_;
    std::vector<float> lilyHalf_;
    std::vector<Fish> fish_;
    std::vector<Pad> pads_;
    std::vector<Dfly> flies_;
};

}  // namespace v2
