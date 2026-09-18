#pragma once
// ---------------------------------------------------------------------------
// bees.h -- THE BEE'S TWO ERRANDS, ported from the JS engine.
//
// "Add beehives to 5% of the birch trees. Import the bee mechanics from v1."
// (user 2026-09-14.) The hive rate is one constant in scene/chunks.h; this file
// is the other half, and what it ports is that engine's own summary of what
// makes a bee a bee rather than a fly with stripes:
//
//   "the bee flies the FLY's code path in every other respect; this is the
//    whole of what makes it a bee -- bees going to flowers and sitting on them
//    briefly, and swarming around a beehive."
//
// TWO ERRANDS AND FIVE STATES. Wander, go to a flower, sit on it, go to the
// hive, orbit it. Every timer, radius and rate below is v1's, at ten
// centimetres to the voxel:
//
//     BEE_FLOWER_R   72 vox   how far from home it will look for a bloom
//     BEE_SIT_R      2.4      ...and how near is near enough to settle
//     BEE_SIT_S/J    2.0/1.6  "briefly": 2.0-3.6 s on the flower
//     BEE_GIVE_S     10       the arrive-by deadline, which is the anti-grind
//     BEE_BAN_S      20       ...and how long a given-up bloom stays barred
//     BEE_LOOK_S     1.6      between looks
//     BEE_ORBIT_R/W  6.5/1.9  the swarm's radius and its rate
//     BEE_ORBIT_Y    2.2      ...and its vertical spread
//     BEE_HIVE_S/J   14/10    14-24 s at the hive, then back to the flowers
//     BEE_HIVE_GAP   3        ...and the gap before it may return
//     BEE_HIVE_N     5        bees to a hive
//
// ---------------------------------------------------------------------------
// WHAT IS DELIBERATELY NOT v1'S, AND WHY
//
// THE HIVE IS THE HOME. In that engine a bee is born on the FLY's home lattice
// and a hive is an errand it may notice within fifteen metres. Here the hive IS
// the home: bees exist where hives do and nowhere else. It is the same picture
// from the player's side -- walk up to a hive and there are bees around it --
// and it deletes a whole lattice, a salt and a leash that would only ever have
// been tuned to put bees where the hives already are. Hives are a birch thing
// in this engine (see the hive pass in scene/chunks.h), so bees are too, which
// is also where v1 keeps them: its bees are an oak-forest creature.
//
// THE RAGE HAS NOWHERE TO LAND. v1's sixth state is a hive that has been broken
// open: its bees swarm the player and sting, on a timer, with a reach scaled
// from the model's own footprint. v2 HAS NO PLAYER HEALTH -- there is nothing
// in this engine for a sting to do -- so porting it would be an animation of a
// consequence that does not exist. Left out on purpose rather than forgotten;
// it is worth having when there is something to damage.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../gpu/world.h"
#include "../scene/collide.h"
#include "../scene/vox.h"

namespace v2 {

// bee/00..03. bee.vox beside them is the source art, which every strip in this
// engine ships and every loader skips by name.
inline constexpr int kBeeFrames = 4;
inline constexpr float kBeeFps = 24.0f;      // the house rate; a settled bee still buzzes

// TEN, WHICH IS TWO HIVES' WORTH. v1's BEE_HIVE_N is five to a hive and the
// band is what caps the rest -- a third hive in range simply gets none until
// one of the first two leaves range, which is the same rule every population in
// this engine follows when its slots run out.
// How far off its own centre a bee is probed when asking whether it may be
// somewhere -- see solidsTouch. Its model is about 25 cm long.
inline constexpr float kBeeBodyR = 0.12f;
// -- ...AND HOW NEAR ITS OWN HIVE IT STOPS BEING STOPPED BY THE TREE -------
//
// A HIVE HANGS IN A CROWN. That is what a hive is here -- 5% of the birches
// carry one, several metres up, in the branches -- so the last few metres of
// every trip home are INSIDE the tree, and the orbit it settles into is inside
// it too. Guarding that is guarding the animal against its own house: measured
// at two of ten bees pinned against the foliage for a whole minute and the
// swarm's average speed halved, 0.66 m/s to 0.35.
//
// So this is the bee's version of the perched songbird's exemption -- the one
// case in this engine where being inside a solid is the behaviour and not the
// bug. Outside it a bee is guarded like anything else. 3 m covers the 0.65 m
// orbit and the approach that leads into it, and is far short of the 7.2 m it
// ranges for flowers.
inline constexpr float kBeeHomeFreeM = 3.0f;
inline constexpr int kBeeCount = 10;
inline constexpr int kBeePerHive = 5;        // v1's BEE_HIVE_N

// HOW FAR A HIVE MAY BE AND STILL HAVE ITS BEES DRAWN. v1's BEE_HIVE_R is 15 m
// and is the radius at which a bee NOTICES a hive; here it is the radius at
// which a hive has bees at all, so it is wider -- a swarm you can see across a
// clearing is most of what a hive is for. The gather is the app's, twice a
// second, exactly as the perched birds' tree list is.
inline constexpr float kBeeHiveM = 40.0f;
// ...and how far out it may get from that hive before it is recycled. Well over
// the flower radius so an errand is never cut short.
inline constexpr float kBeeDropM = 52.0f;

inline constexpr float kBeeFlowerM = 7.2f;   // v1's BEE_FLOWER_R
inline constexpr float kBeeSitM = 0.24f;     // BEE_SIT_R
inline constexpr float kBeeSitSec = 2.0f, kBeeSitJit = 1.6f;   // BEE_SIT_S / BEE_SIT_J
inline constexpr float kBeeGiveSec = 10.0f;  // BEE_GIVE_S -- the arrive-by deadline
inline constexpr float kBeeBanSec = 20.0f;   // BEE_BAN_S
inline constexpr float kBeeLookSec = 1.6f;   // BEE_LOOK_S
inline constexpr float kBeeOrbitM = 0.65f;   // BEE_ORBIT_R
inline constexpr float kBeeOrbitW = 1.9f;    // BEE_ORBIT_W, rad/s
inline constexpr float kBeeOrbitY = 0.22f;   // BEE_ORBIT_Y
inline constexpr float kBeeHiveSec = 14.0f, kBeeHiveJit = 10.0f;   // BEE_HIVE_S / J
inline constexpr float kBeeHiveGap = 3.0f;   // BEE_HIVE_GAP

// v1's fly speed, which the bee inherits: 56 vox/s. Fast, and it is what makes
// the difference between a bee and a butterfly visible at a glance -- one darts
// between two points, the other drifts.
inline constexpr float kBeeSpeed = 5.6f;

// -- AND WHAT HAPPENS WHEN YOU HIT ONE ---------------------------------------
//
// (user 2026-09-16: "when attacking a bee or the beehive, have all of the bees
// near start attacking the player".)
//
// THE WHOLE SWARM, NOT THE ONE YOU HIT. That is the ask and it is also the
// point of a hive: a bee on its own is an insect, and thirty of them coming off
// a broken hive is the reason you leave hives alone. The trigger is the ATTACK
// rather than the kill, so swinging at a hive and missing the bees still brings
// them -- and killing one outright brings the rest, which is the case a
// kill-only trigger would silently miss.
//
// 18 m IS WIDER THAN THE HIVE'S OWN 3 m HOME. A swarm that only answered within
// its orbit would be a swarm you could stand outside and dismantle; this
// reaches the foragers out on the flowers too, which are up to kBeeFlowerM away
// from a hive that may itself be 40 m off.
inline constexpr float kBeeAngerM = 18.0f;
// HOW LONG THEY STAY ANGRY. Long enough to be a consequence and short enough
// that a wood is not permanently hostile because of one swing an hour ago.
inline constexpr float kBeeAngrySec = 12.0f;
// FASTER WHEN ANGRY, which is most of what makes it read as an attack rather
// than as bees drifting toward you. v1's own hunt multiplier on the marchers is
// the same idea.
inline constexpr float kBeeAngryMul = 1.45f;
// ...AND THEY STOP SHORT RATHER THAN SITTING INSIDE THE CAMERA. A bee that
// reaches the eye has nowhere left to fly and jitters on the spot; holding at
// arm's length keeps it legible as a bee.
inline constexpr float kBeeAngryHoldM = 0.75f;
inline constexpr float kBeeEase = 3.5f;      // how sharply it comes onto a new bearing
inline constexpr float kBeeDownM = 1.2f;     // BEE_DOWN: it SETTLES onto a bloom, never snaps

// A FLOWER'S BLOOM IS NOT WHERE decorNear SAYS THE FLOWER IS. That query hands
// back the instance transform, which is the model's CORNER at its base, and a
// bee belongs on the HEAD. The models are five to eight voxels tall and the
// query cannot know which -- it carries no model -- so this is the one number
// in the file that is an estimate rather than a measurement.
inline constexpr float kBeeBloomLiftM = 0.45f;
// ...and the same for a hive, which hangs from a branch: its transform is the
// corner of a box about half a metre across.
inline constexpr float kBeeHiveHalfM = 0.25f;

inline constexpr float kBeeFadeSec = 0.7f, kBeeFadeMin = 0.08f;

// ---------------------------------------------------------------------------
class Bees {
  public:
    bool ready() const { return ready_; }

    bool load(World &world, const std::string &lifeDir) {
        std::vector<VoxModel> mo;
        mo.resize(size_t(kBeeFrames));
        for (int f = 0; f < kBeeFrames; ++f) {
            char path[600];
            std::snprintf(path, sizeof(path), "%s/bee/%02d.vox", lifeDir.c_str(), f);
            std::string err;
            if (!voxLoad(path, &mo[size_t(f)], &err)) {
                std::fprintf(stderr, "v2: bee %s: %s -- no bees\n", path, err.c_str());
                return false;
            }
        }
        for (int f = 0; f < kBeeFrames; ++f) {
            int sx = 0, sy = 0, sz = 0;
            const int m = world.addFlyerModel(mo[size_t(f)], "bee", &sx, &sy, &sz, true);
            if (m < 0) { model_.clear(); return false; }
            model_.push_back(m);
            hx_ = 0.5f * float(sx) * VOXEL_M;
            hy_ = 0.5f * float(sy) * VOXEL_M;
            hz_ = 0.5f * float(sz) * VOXEL_M;
        }
        bees_.assign(size_t(kBeeCount), Bee{});
        ready_ = !model_.empty();
        if (ready_)
            std::printf("  bee      %zu frames, %d slots, %d to a hive\n", model_.size(),
                        kBeeCount, kBeePerHive);
        return ready_;
    }

    // -----------------------------------------------------------------------
    // ONE TICK.
    //
    // `hives` and `blooms` are handed in rather than reached for, the way the
    // perched birds are handed their trees: the app already gathers them off
    // World::decorNear on a half-second clock, because a hive and a flower are
    // things that do not move and re-asking per bee per frame is the wide query
    // this engine goes out of its way to pay once.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // ...AND THE SOLIDS, WHICH ARE NEW AND ARE THE SAME LIST AS EVERYTHING
    // ELSE'S.
    //
    // A bee flies a STRAIGHT LINE between two placements -- a hive in a crown
    // and a flower on the ground -- and until this argument existed there was
    // nothing whatever between the two ends of that line. It went through
    // trunks and through boulders, and an errand that crosses a rock does it
    // twice a minute for as long as you stand there.
    //
    // DEFAULTED TO NOTHING rather than made compulsory, because the offline
    // report and the asset deck tick bees with no world around them and a bee
    // with an empty list behaves exactly as it always did.
    // -----------------------------------------------------------------------
    void update(float dt, const Vec3 &player, const std::vector<Vec3> &hives,
                const std::vector<Vec3> &blooms,
                const std::vector<Solid> *solids = nullptr) {
        if (!ready_) return;
        solids_ = solids;
        clock_ += dt;
        player_ = player;   // for kAngry, which is the one mode that chases
        recycle(hives, dt);
        fill(player, hives);
        for (size_t i = 0; i < bees_.size(); ++i)
            if (bees_[i].live) step(&bees_[i], uint32_t(i), dt, blooms);
    }


    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        // A bee at its hive is flagged the way a perched songbird is: it is in
        // the crown because that is where it lives. See kBeeHomeFreeM.
        for (const Bee &b : bees_)
            if (b.live)
                out->push_back({Vec3(b.x, b.y, b.z), "bee", kBeeBodyR,
                                atHome(b, b.x, b.y, b.z)});
    }

    void publish(World &world, int slot0) {
        if (!ready_) return;
        for (size_t i = 0; i < bees_.size(); ++i) put(world, slot0 + int(i), bees_[i]);
        // The fifth system in this engine to need this line -- see birds.h.
        world.flushFlyerInstances();
    }

    void despawnAll() {
        for (Bee &b : bees_) b = Bee{};
    }

    // Borrowed for the length of one update() and never held -- the same
    // contract Critters and Bunnies have with this list.
    const Solid *list() const { return solids_ ? solids_->data() : nullptr; }
    int count() const { return solids_ ? int(solids_->size()) : 0; }

    int living() const {
        int n = 0;
        for (const Bee &b : bees_) n += b.live ? 1 : 0;
        return n;
    }

    // -- WHERE THE TIME GOES, over the whole run -------------------------
    //
    // A SNAPSHOT OF A STATE MACHINE REPORTS NOTHING. At any instant most of a
    // swarm is orbiting, because the orbit is 14-24 s and the errand is a few
    // -- so "10 of 10 orbiting" is what a working machine looks like from one
    // frame, and also what a broken one looks like. The bunny's arc report
    // records the same lesson in the same words. These are BEE-TICKS per mode
    // since the bees loaded.
    void modeShare(long *out5) const {
        for (int k = 0; k < 5; ++k) out5[k] = modeTicks_[k];   // the caller's array is five
    }

    // How many are ON an errand rather than at the hive, for the offline
    // report: "ten bees" says nothing about whether the machine is running.
    int foraging() const {
        int n = 0;
        for (const Bee &b : bees_)
            if (b.live && (b.mode == kToFlower || b.mode == kSit)) ++n;
        return n;
    }
    int orbiting() const {
        int n = 0;
        for (const Bee &b : bees_)
            if (b.live && b.mode == kOrbit) ++n;
        return n;
    }

    bool nearest(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Bee &b : bees_) {
            if (!b.live) continue;
            const float dx = b.x - p.x, dz = b.z - p.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = Vec3(b.x, b.y, b.z);
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // SOMETHING HIT THE HIVE, OR ONE OF US -- every bee near it comes for you.
    //
    // (user 2026-09-16: "when attacking a bee or the beehive, have all of the
    // bees near start attacking the player".)
    //
    // POSITION, NOT SLOT. The two callers are a blow on a bee and a blow on a
    // hive, and only one of those has a bee to name -- so the thing they have
    // in common is WHERE it happened, and every bee near there answers. That
    // also makes it right for free when the hive a bee belongs to is out of
    // range of the swarm that comes: proximity to the ATTACK is what matters,
    // not shared ownership.
    //
    // RETURNS HOW MANY so a caller can say so, and so --bee-test can measure it
    // rather than infer it from behaviour.
    // -----------------------------------------------------------------------
    int anger(const Vec3 &at, float radiusM) {
        const float r2 = radiusM * radiusM;
        int woke = 0;
        for (Bee &b : bees_) {
            if (!b.live) continue;
            const float dx = b.x - at.x, dy = b.y - at.y, dz = b.z - at.z;
            if (dx * dx + dy * dy + dz * dz > r2) continue;
            b.mode = kAngry;
            b.angryTo = clock_ + kBeeAngrySec;
            ++woke;
        }
        return woke;
    }
    // ...and how many are on the warpath right now, for the test.
    int angryCount() const {
        int n = 0;
        for (const Bee &b : bees_)
            if (b.live && b.mode == kAngry) ++n;
        return n;
    }

    // THAT ONE IS DEAD -- the population's half of a kill.
    //
    // (user 2026-09-14: "when killing life, the life breaks apart into multiple
    // pieces".) render/lifehit.h owns the blow, the flash and the carcass; this
    // is the one thing it cannot do, because whether a member is alive is this
    // file's own business. `i` is the index within THIS population's run of the
    // instance band -- App::killLifeAt does the arithmetic.
    //
    // THE SLOT IS FREED, NOT BLANKED. Retiring it is what every other escape in
    // this file does, and the spawner refills it wherever it likes next -- v1
    // goes further and marks the slot slain for the session, which needs a
    // notion of a population roster this engine does not have.
    // -----------------------------------------------------------------------
    bool killSlot(int i) {
        if (i < 0 || size_t(i) >= bees_.size() || !bees_[size_t(i)].live) return false;
        bees_[size_t(i)] = Bee{};
        return true;
    }

  private:
    enum Mode { kWander = 0, kToFlower, kSit, kToHive, kOrbit, kAngry, kModeCount };

    struct Bee {
        bool live = false;
        int mode = kWander;
        // When the anger runs out, on the same clock everything else here uses.
        float angryTo = 0.0f;
        // ITS HIVE, by position rather than by index: the gather is rebuilt
        // twice a second and an index into it would name a different hive the
        // moment one drops out of range. Same trap the perched birds' stale
        // tree list was.
        //
        // TWO POINTS AND THEY ARE NOT INTERCHANGEABLE. hx/hy/hz is the hive's
        // own transform, and it is an IDENTITY -- hiveAlive compares it against
        // the gather to ask "is my hive still one of these", so it has to be
        // the number the gather hands back, to the bit. cx/cy/cz is the same
        // hive as a PLACE to fly to, which is that transform plus the model's
        // half-box, because the transform is a CORNER.
        //
        // Storing one and deriving the other is exactly what this got wrong
        // first: the home carried the offset, the identity test compared it
        // against the raw list, nothing ever matched, and every bee was
        // recycled and re-born every 0.7 s. It looked perfect -- ten bees at
        // nine hives, all orbiting -- because a bee that has just been born IS
        // orbiting. MEASURED: 100% of bee-ticks in the orbit, 0% on an errand.
        float hx = 0, hy = 0, hz = 0;
        float cx = 0, cy = 0, cz = 0;
        float x = 0, y = 0, z = 0;
        float vx = 0, vy = 0, vz = 0;   // eased, so a new bearing is a turn and not a cut
        float tx = 0, ty = 0, tz = 0;   // whatever it is flying at
        float until = 0;                // when the current mode gives up or ends
        float lookAt = 0;               // when it next looks for a bloom
        float hiveAt = 0;               // ...and when it may go back to the hive
        float banX = 0, banZ = 0, banUntil = -1.0f;   // the bloom it gave up on
        float orbit = 0;                // where it is round the hive
        float phase = 0;                // its own offset in every shared sine
        float frame = 0;
        float age = 0.0f, dying = -1.0f;
    };

    float rnd(uint32_t i, uint32_t salt) const {
        return hashUnit(salt, hashU32(i * 2654435761u, uint32_t(clock_ * 1000.0f)));
    }

    static float fadeOf(float age, float dying) {
        // NO GROW-IN, for the reason the lake's fadeOf gives: an arrival that
        // swells into place is more visible than the pop it hides.
        (void)age;
        const float out = dying >= 0.0f ? saturate(1.0f - dying / kBeeFadeSec) : 1.0f;
        return kBeeFadeMin + (1.0f - kBeeFadeMin) * out;
    }

    // Is this bee's hive still one of the ones in range?
    bool hiveAlive(const Bee &b, const std::vector<Vec3> &hives) const {
        for (const Vec3 &h : hives) {
            const float dx = h.x - b.hx, dy = h.y - b.hy, dz = h.z - b.hz;
            if (dx * dx + dy * dy + dz * dz < 0.01f) return true;
        }
        return false;
    }

    int atHive(const Vec3 &h) const {
        int n = 0;
        for (const Bee &b : bees_) {
            if (!b.live) continue;
            const float dx = h.x - b.hx, dy = h.y - b.hy, dz = h.z - b.hz;
            if (dx * dx + dy * dy + dz * dz < 0.01f) ++n;
        }
        return n;
    }

    // A slot is given up when its HIVE goes -- felled, or simply out of range.
    // Never when the bee is merely out on an errand, which is what kBeeDropM is
    // wider than kBeeFlowerM for.
    void recycle(const std::vector<Vec3> &hives, float dt) {
        for (Bee &b : bees_) {
            if (!b.live) continue;
            const float dx = b.x - b.cx, dz = b.z - b.cz;
            const bool gone = !hiveAlive(b, hives) ||
                              dx * dx + dz * dz > kBeeDropM * kBeeDropM;
            b.age += dt;
            if (gone && b.dying < 0.0f) b.dying = 0.0f;
            if (!gone && b.dying >= 0.0f) b.dying = -1.0f;
            if (b.dying >= 0.0f) {
                b.dying += dt;
                if (b.dying >= kBeeFadeSec) b.live = false;
            }
        }
    }

    // FIVE TO A HIVE, and the nearest hives first. A hive that already has its
    // five is skipped rather than shared out -- v1 splits the band by slot
    // number for the same reason, so that which bee belongs where is not
    // re-decided every frame.
    void fill(const Vec3 &player, const std::vector<Vec3> &hives) {
        for (size_t i = 0; i < bees_.size(); ++i) {
            Bee &b = bees_[i];
            if (b.live) continue;
            const Vec3 *want = nullptr;
            float best = kBeeHiveM * kBeeHiveM;
            for (const Vec3 &h : hives) {
                if (atHive(h) >= kBeePerHive) continue;
                // -- A DISC, BECAUSE THE GATHER IS A BOX ---------------------
                //
                // World::decorNear tests each axis separately, so a hive on the
                // diagonal comes back from a 40 m gather at 56 m -- and the
                // BLOOMS are gathered round the PLAYER at the hive radius plus
                // a bee's flower range. A hive outside this disc therefore has
                // flowers the list does not contain, and its bees would orbit
                // for ever with nothing to forage. MEASURED before this: ten
                // bees at nine hives, 0 of them ever foraging.
                //
                // NEAREST FIRST for the same reason every other population in
                // this engine claims nearest-first: the swarm you can see
                // should be the one that has bees in it.
                const float dx = h.x - player.x, dz = h.z - player.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 >= best) continue;
                best = d2;
                want = &h;
            }
            if (!want) return;

            b = Bee{};
            b.live = true;
            b.hx = want->x;
            b.hy = want->y;
            b.hz = want->z;
            b.cx = want->x + kBeeHiveHalfM;
            b.cy = want->y + kBeeHiveHalfM;
            b.cz = want->z + kBeeHiveHalfM;
            b.orbit = rnd(uint32_t(i), 0xBEE1u) * 6.2831853f;
            b.phase = rnd(uint32_t(i), 0xBEE2u) * 6.2831853f;
            b.frame = rnd(uint32_t(i), 0xBEE3u) * float(kBeeFrames);
            // ON THE RING IT WOULD BE ORBITING AT, not at the hive's own point:
            // a bee born inside the box it is meant to be circling spends its
            // first second flying out of the wood the hive is hung in.
            b.x = b.cx + sinf(b.orbit) * kBeeOrbitM;
            b.y = b.cy;
            // Born on its hive, which is in the crown -- see kBeeHomeFreeM.
            // Nothing to nudge out of: this is home.
            b.z = b.cz + cosf(b.orbit) * kBeeOrbitM;
            b.mode = kOrbit;
            b.until = clock_ + kBeeHiveSec + rnd(uint32_t(i), 0xBEE4u) * kBeeHiveJit;
            b.lookAt = clock_ + rnd(uint32_t(i), 0xBEE5u) * kBeeLookSec;
        }
    }

    // -----------------------------------------------------------------------
    // THE FIVE STATES.
    //
    // Every one of them ends by writing a TARGET and a deadline; the flight at
    // the bottom is the same three lines whatever the target is. That is v1's
    // own arrangement and its note says why: its fish once had a planner and a
    // mover that disagreed about what was reachable, and the fish swam at
    // terrain for ever. Nothing here writes a position directly except the
    // orbit, which is a position by definition.
    // -----------------------------------------------------------------------
    // Is this bee at its own hive, where the tree is the house and not a wall?
    // Asked of the DESTINATION as well as the origin, so the last step of an
    // approach is not the one that gets refused.
    static bool atHome(const Bee &b, float x, float y, float z) {
        const float dx = x - b.cx, dy = y - b.cy, dz = z - b.cz;
        return dx * dx + dy * dy + dz * dz < kBeeHomeFreeM * kBeeHomeFreeM;
    }

    void step(Bee *b, uint32_t i, float dt, const std::vector<Vec3> &blooms) {
        if (b->mode >= 0 && b->mode < kModeCount) ++modeTicks_[b->mode];
        switch (b->mode) {
            // -- STRAIGHT AT THE PLAYER -- see kBeeAngerM -----------------
            //
            // NO ERRAND AND NO HIVE. An angry bee abandons whatever it was
            // doing; letting it keep a flower target would have it breaking off
            // mid-attack to pollinate, which reads as the anger wearing off
            // early rather than as a bee with priorities.
            //
            // THE TARGET IS RE-READ EVERY TICK because the player moves. It is
            // the one mode here that chases something that does not stand
            // still, which is why it cannot use the fly-to-a-point helper the
            // other four share.
            case kAngry: {
                if (clock_ >= b->angryTo) {
                    // Calmed down. Back to the hive it came from, which is
                    // where a bee that has lost its errand belongs.
                    b->mode = kToHive;
                    b->until = clock_ + kBeeHiveSec;
                    break;
                }
                const float dx = player_.x - b->x, dy = player_.y - b->y, dz = player_.z - b->z;
                const float d = sqrtf(dx * dx + dy * dy + dz * dz);
                if (d <= kBeeAngryHoldM) break;   // at the face; hold station
                // THROUGH THE VELOCITY, NOT STRAIGHT ONTO THE POSITION. A
                // bee has no `th` -- publish derives its heading from vx/vz
                // (see the note there) -- so writing the step directly would
                // move an insect that went on facing the way it last flew.
                // Setting the velocity turns it and moves it with one number,
                // which is what every other mode here does.
                const float inv = 1.0f / maxf(0.001f, d);
                const float sp = kBeeSpeed * kBeeAngryMul;
                b->vx = dx * inv * sp;
                b->vy = dy * inv * sp;
                b->vz = dz * inv * sp;
                b->x += b->vx * dt;
                b->y += b->vy * dt;
                b->z += b->vz * dt;
                break;
            }
            case kWander:
                // Drifting near the hive, looking. The look is on a clock so a
                // bee that finds nothing is not searching every frame.
                b->tx = b->cx + sinf(clock_ * 0.7f + b->phase) * 1.6f;
                b->ty = b->cy + sinf(clock_ * 1.1f + b->phase) * 0.5f;
                b->tz = b->cz + cosf(clock_ * 0.6f + b->phase) * 1.6f;
                if (clock_ > b->lookAt) {
                    b->lookAt = clock_ + kBeeLookSec + rnd(i, 0xBE01u) * kBeeLookSec;
                    // THE HIVE FIRST, which is v1's errand skip: a bee with its
                    // hive to hand does not go to a flower. Without it the
                    // swarm is never at home and a hive is a landmark with
                    // nothing round it.
                    if (clock_ > b->hiveAt) {
                        b->mode = kToHive;
                        b->until = clock_ + kBeeGiveSec;
                        break;
                    }
                    const Vec3 *fl = pickBloom(*b, i, blooms);
                    if (fl) {
                        b->mode = kToFlower;
                        b->tx = fl->x;
                        b->ty = fl->y + kBeeBloomLiftM;
                        b->tz = fl->z;
                        b->until = clock_ + kBeeGiveSec;
                    }
                }
                break;

            case kToFlower: {
                const float dx = b->tx - b->x, dz = b->tz - b->z;
                if (dx * dx + dz * dz < kBeeSitM * kBeeSitM) {
                    b->mode = kSit;
                    b->until = clock_ + kBeeSitSec + rnd(i, 0xBE02u) * kBeeSitJit;
                } else if (clock_ > b->until) {
                    // GAVE UP, AND BANS THIS BLOOM. v1's note is the reason:
                    // without the ban the very next look re-picks the flower it
                    // just failed to reach and the bee grinds for ever on a
                    // lane it cannot solve.
                    b->banX = b->tx;
                    b->banZ = b->tz;
                    b->banUntil = clock_ + kBeeBanSec;
                    b->mode = kWander;
                    b->lookAt = clock_ + kBeeLookSec;
                }
                break;
            }

            case kSit:
                // PINNED OVER THE BLOOM AND EASED DOWN ONTO IT. The wings keep
                // flapping at the house rate throughout -- v1 is explicit that
                // a settled bee still buzzes and that freezing the strip reads
                // as a dead bee stuck to a petal.
                b->tx = b->tx;
                b->tz = b->tz;
                if (clock_ > b->until) {
                    b->mode = kWander;
                    b->lookAt = clock_ + kBeeLookSec;
                }
                break;

            case kToHive: {
                b->tx = b->cx;
                b->ty = b->cy;
                b->tz = b->cz;
                const float dx = b->cx - b->x, dz = b->cz - b->z;
                const float r = kBeeOrbitM * 1.6f;
                if (dx * dx + dz * dz < r * r) {
                    b->mode = kOrbit;
                    b->until = clock_ + kBeeHiveSec + rnd(i, 0xBE03u) * kBeeHiveJit;
                } else if (clock_ > b->until) {
                    b->mode = kWander;
                    b->hiveAt = clock_ + kBeeHiveGap;
                    b->lookAt = clock_ + kBeeLookSec;
                }
                break;
            }

            case kOrbit:
                // A SWARM, NOT A RACETRACK. 1.9 rad/s at 0.65 m is 1.2 m/s,
                // well under the bee's own 5.6 -- v1 makes exactly this point
                // about exactly these two numbers.
                b->orbit += kBeeOrbitW * dt;
                b->tx = b->cx + sinf(b->orbit) * kBeeOrbitM;
                b->ty = b->cy + sinf(clock_ * 1.7f + b->phase) * kBeeOrbitY;
                b->tz = b->cz + cosf(b->orbit) * kBeeOrbitM;
                if (clock_ > b->until) {
                    b->mode = kWander;
                    b->hiveAt = clock_ + kBeeHiveGap;
                    b->lookAt = clock_ + kBeeLookSec;
                }
                break;
        }

        // -- THE FLIGHT, WHICH IS THE SAME WHATEVER IT IS FLYING AT ---------
        //
        // A velocity eased toward the bearing of the target rather than a
        // position lerped at it: the ease is what makes a change of errand read
        // as a bee turning round, and it is also what keeps the model pointing
        // somewhere sensible, since the yaw is taken from the velocity.
        //
        // THE SIT IS THE ONE EXCEPTION AND IT IS v1'S. BEE_DOWN eases it onto
        // the bloom vertically at its own slow rate -- "a settle, never a snap"
        // -- so the last of the approach reads as a landing.
        const float ex = b->tx - b->x, ey = b->ty - b->y, ez = b->tz - b->z;
        const float d = sqrtf(ex * ex + ey * ey + ez * ez);
        const float want = (b->mode == kSit) ? kBeeDownM : kBeeSpeed;
        const float k = minf(1.0f, dt * kBeeEase);
        if (d > 1e-3f) {
            const float s = minf(want, d / maxf(dt, 1e-4f));
            b->vx += (ex / d * s - b->vx) * k;
            b->vy += (ey / d * s - b->vy) * k;
            b->vz += (ez / d * s - b->vz) * k;
        }
        // -- ...AND IT DOES NOT GO THROUGH THE WOOD ------------------------
        //
        // ONE GUARD FOR THE WHOLE STATE MACHINE, and that is deliberate: five
        // modes write five different targets, and a rule applied per mode is a
        // rule that the sixth mode will not have. Everything above only ever
        // sets a TARGET; this is the one place a bee actually moves, so this is
        // the one place it can be stopped.
        //
        // A BLOCKED BEE GIVES UP ON THE ERRAND RATHER THAN GRINDING AT THE
        // STONE. kBeeGiveSec and the bloom ban already exist for exactly this
        // shape of failure -- v1's note, "without the ban the very next look
        // re-picks the flower it just failed to reach and the bee grinds for
        // ever on a lane it cannot solve" -- and a boulder in the way is that
        // lane. So the slide keeps it moving and the deadline does the rest.
        const float nx = b->x + b->vx * dt, ny = b->y + b->vy * dt, nz = b->z + b->vz * dt;
        if (atHome(*b, nx, ny, nz)) {
            b->x = nx;
            b->y = ny;
            b->z = nz;
        } else {
            flySlide(list(), count(), VOXEL_M, kBeeBodyR, &b->x, &b->y, &b->z, nx, ny, nz);
        }
        b->frame = fmodf(b->frame + dt * kBeeFps, float(kBeeFrames));
    }

    // The nearest bloom to its HOME that it is not currently barred from. v1
    // throws darts at its voxel world; this walks a list the app has already
    // gathered, which is the same search with the answer already in memory.
    const Vec3 *pickBloom(const Bee &b, uint32_t i, const std::vector<Vec3> &blooms) const {
        const Vec3 *best = nullptr;
        float bd = kBeeFlowerM * kBeeFlowerM;
        // A JITTERED START, so ten bees at one hive do not queue at one flower.
        const size_t n = blooms.size();
        if (!n) return nullptr;
        const size_t off = size_t(rnd(i, 0xBE04u) * float(n)) % n;
        for (size_t q = 0; q < n; ++q) {
            const Vec3 &f = blooms[(q + off) % n];
            const float dx = f.x - b.cx, dz = f.z - b.cz;
            const float d2 = dx * dx + dz * dz;
            if (d2 >= bd) continue;
            if (clock_ < b.banUntil && fabsf(f.x - b.banX) < 0.2f && fabsf(f.z - b.banZ) < 0.2f)
                continue;
            bd = d2;
            best = &f;
        }
        return best;
    }

    void put(World &world, int slot, const Bee &b) const {
        if (!b.live || model_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const int n = int(model_.size());
        const int fi = maxi(0, mini(n - 1, int(b.frame)));
        // WHICH WAY IT IS POINTING IS WHICH WAY IT IS GOING, and the half turn
        // is the same one every animal in this engine needs: the nose is at
        // local -z. See LakeLife::yawMat for why adding pi is the fix and
        // negating a column is not -- a negated column is a MIRROR.
        const float th = atan2f(b.vx, b.vz) + 3.14159265f;
        const float k = fadeOf(b.age, b.dying);
        const float c = cosf(th) * k, s = sinf(th) * k;
        const float m[9] = {c, 0.0f, s, 0.0f, k, 0.0f, -s, 0.0f, c};
        // The mesh runs from its own corner and the bee's position is its
        // middle, so the translation is the centre less the half-box carried
        // through the turn.
        const float ox = m[0] * hx_ + m[1] * hy_ + m[2] * hz_;
        const float oy = m[3] * hx_ + m[4] * hy_ + m[5] * hz_;
        const float oz = m[6] * hx_ + m[7] * hy_ + m[8] * hz_;
        // THE ANIMAL, NOT ITS BOX -- see World::place.
        const float anchor[3] = {b.x, b.y, b.z};
        world.setFlyerInstance(slot, model_[size_t(fi)], m, b.x - ox, b.y - oy, b.z - oz, nullptr,
                               true, nullptr, anchor);
    }

    std::vector<int> model_;
    std::vector<Bee> bees_;
    float hx_ = 0.1f, hy_ = 0.1f, hz_ = 0.1f;
    float clock_ = 0.0f;
    long modeTicks_[kModeCount] = {0, 0, 0, 0, 0, 0};
    // Where the player was on the last update, so an angry bee has something to
    // fly at. Stored rather than threaded through step(): the other five modes
    // fly at fixed points and have never needed it.
    Vec3 player_{0.0f, 0.0f, 0.0f};
    bool ready_ = false;
    const std::vector<Solid> *solids_ = nullptr;
};

}  // namespace v2
