// ---------------------------------------------------------------------------
// birdflock.h -- the songbirds that are IN THE AIR, as opposed to the ones
// sitting in the trees (render/birds.h).
//
// Ported from the JS engine's `birdStep`, which is the reference. The two bird
// systems share their species and nothing else: a perched bird is a POSE on a
// branch that turns in place through eleven rotate/ frames, and a flying one is
// a body with a heading, an altitude and a bank, cycling a seven-frame flight/
// strip. They do not convert into one another and neither needs to know the
// other exists.
//
// ALL THE SONGBIRDS BUT THE PINK ONE, which is the same rule the perched flock
// already follows -- see kBirdSpecies in birds.h, whose note records the pink
// bird being held back for the cherry forest.
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "world/world.h"
#include "player/collide.h"
#include "voxel/vox.h"
#include "world/voxelworld.h"
#include "ai/birds.h"

namespace v2 {

// assets/life/<species>/flight/00..06, and base.vox is SOURCE ART -- the same
// trap the salmon and the dragonfly strips carry. See lake.h.
inline constexpr int kFlightFrames = 7;

// -- v1's OWN NUMBERS -------------------------------------------------------
//
// NAMED kSong*, NOT kFly*: butterflies.h already owns that prefix and the two
// share a namespace, so kFlySpeed and friends are a redefinition error rather
// than a shadow. Caught by the compiler, but only after the file was written.
// ---------------------------------------------------------------------------
//
// Its units are voxels per second and both engines are 10 cm voxels, so they
// divide by ten into metres.
inline constexpr float kSongSpeed = 5.5f;    // m/s, ground speed  (JS BIRD_SPD 55)
inline constexpr float kSongFlapFps = 24.0f; //                    (JS BIRD_FLAP 24)

// -- ...AND THE ONE NUMBER THAT COULD NOT BE COPIED -------------------------
//
// JS: `BIRD_ALT = 140` with a hard floor at `g + 122`, and its comment says
// exactly why -- "must beat the tallest pine (116 vox) + bob or it clips
// foliage". That is a statement about ITS trees, not about flight, and v2's are
// half again as tall: a felled pine measures 23 m end to end and a birch 25
// (--fell-test, collider longest side). Copied literally the flock would fly
// through every crown in the wood.
//
// So the same RULE with this engine's number: clear the tallest thing that
// grows, plus the bob, plus a margin.
inline constexpr float kSongFloorM = 28.0f;  // never closer to the ground than this
// How far off its own centre a bird is probed -- see solidsTouch.
inline constexpr float kSongBodyR = 0.25f;
inline constexpr float kSongCruiseM = 30.0f; // ...and the height it aims for

// The wander band, the soar band, and the dive -- all JS, all divided by ten.
inline constexpr float kSongWanderAlt = 3.4f;   // (JS altT 0..34)
inline constexpr float kSongSoarLo = 1.8f, kSongSoarHi = 4.4f;   // (JS 18..44)
inline constexpr float kSongSwoopSec = 3.2f;    // the dive-and-recover window
inline constexpr float kSongLookM = 4.5f;       // terrain lookahead (JS 45 vox)
// THE TWO ADMIT ENDS OF THE DESERT GATE, both v1's -- see the note in step().
// Out is where a live bird is recycled, in is where a new one is refused.
inline constexpr float kSongDesertOut = 0.85f;
inline constexpr float kSongDesertIn = 0.35f;
// -- ...AND A SONGBIRD BELONGS OVER WOOD ------------------------------------
//
// (user 2026-09-20: "only have flying song birds above trees".)
//
// THE SAME TWO-ENDED SHAPE THE DESERT GATE HAS, and for the same reason: one
// threshold would spawn a bird and recycle it on the same tick wherever the
// field sat on the line, which is the flicker the pink bird's note records at
// length. `In` is what a NEW bird needs under it, `Out` is where a live one
// finally gives up -- looser, so a bird crossing a clearing flies over it
// rather than vanishing above it.
//
// MEASURED IN VoxelTerrain::standDensity, the sixty-metre field the tree
// scatter itself is gated on, so "over trees" here means the same thing it
// means to the wood. kPetalStand (0.40) is the same field's "under a stand".
inline constexpr float kSongTreeIn = 0.42f;
inline constexpr float kSongTreeOut = 0.26f;
inline constexpr float kSongBobM = 0.30f;       // (JS sin(t*1.2) * 3.0)

// -- HOW MANY, AND WHERE THEY COME FROM -------------------------------------
//
// Nine, which divides by the three species so the round-robin comes out exact.
// The JS note is worth keeping: an uneven split "shows up as one species being
// rarer than the others rather than as anything subtle".
inline constexpr int kFlockBirds = 9;

// ---------------------------------------------------------------------------
// A QUARTER OF THEM FLY WITH SOMEBODY. THE REST FLY ALONE.
//
// "75% of the time the song birds fly alone and the other 25% of the time they
// fly with someone else ... max 2 together." Three things follow from that
// sentence and all three are rules rather than tendencies:
//
//   A PAIR IS TWO. Not a flock that happens to be small -- a chaser may chase
//   exactly one bird, a bird may be chased by exactly one, and a chaser may not
//   itself be chased. That last clause is the one that matters: without it the
//   pairs chain into a conga line of five, which is emphatically not "max 2".
//
//   SAME SPECIES ONLY. "A cardinal would chase a cardinal and a blue bird will
//   chase a blue bird" -- and the three strips are already the species, so this
//   is one equality test on Bird::sp.
//
//   THE QUARTER IS A CEILING ON THE POPULATION, not a die rolled per bird. A
//   per-bird 25% chance gives a binomial spread -- some minutes half the wood
//   is chasing -- and what was asked for is a proportion. So a pair is formed
//   only while (paired + 2) is still within a quarter of the living, which at
//   nine birds is exactly one pair: 2 of 9 is 22%, and 4 of 9 would be 44%.
//
// A CHASE IS A HEADING, NOT A LEASH. The chaser steers at where the leader IS,
// so it cuts corners and overshoots turns -- which is what a bird doing this
// actually looks like. Pointing it at an offset BEHIND the leader would give a
// tidy formation, and a tidy formation is the salmon's behaviour, not this one.
inline constexpr float kSongPairM = 26.0f;      // near enough to take an interest
inline constexpr float kSongPairDropM = 48.0f;  // ...and far enough to lose it
inline constexpr float kSongChaseMin = 5.0f, kSongChaseMax = 13.0f;   // seconds
inline constexpr float kSongChaseCool = 6.0f;   // before that bird may pair again
inline constexpr float kSongChaseYaw = 1.9f;    // rad/s -- harder than a wander
inline constexpr float kSongChaseSpd = 1.16f;   // and a little quicker, or it never closes

// -- AND THEY ARRIVE FROM THE FOG, NOT OUT OF CLEAR SKY ---------------------
//
// This is the JS engine's own correction to itself, reported as "the song birds
// seem to just appear out of the sky ... do they not have the same render
// distance as everything else?". It had placed them at 0.24-0.50 of the keep
// radius on the reasoning that a bird on the horizon is one pixel -- and the
// flaw is that a speck against OPEN SKY is exactly where a new one is easiest
// to catch, because there is nothing else up there to look at.
//
// Same band as everything else: it fades in at the far edge and flies toward
// you. It is also the identical lesson the PERCHED birds had to learn the hard
// way this week -- see v2-birds-popped-and-flickered.
// 105 m, which is v1's own `min(renderDist + 64, 1040)` voxels. 200 was tried
// first on the reasoning that v2 sees three times as far -- and measured, the
// nearest of nine birds was 157 m away, which is two pixels. The ring is not a
// draw distance, it is how often one crosses in front of you, and nine birds
// spread over 200 m almost never do.
inline constexpr float kSongKeepM = 105.0f;
inline constexpr float kSongRingLo = 0.78f, kSongRingHi = 0.94f;

// ---------------------------------------------------------------------------
class BirdFlock {
  public:
    bool ready() const { return ready_; }

    // -----------------------------------------------------------------------
    // One flight strip per species. A species whose strip is missing is simply
    // absent from the flock -- the JS loader does the same, "with a warn and no
    // other effect".
    // -----------------------------------------------------------------------
    bool load(World &world, const std::string &lifeDir) {
        // WHICH BAND A BIRD IS OVER -- see the desert gate in step(). Captured
        // here rather than handed to update() because update() already takes a
        // ground functor and a solids list, and this is the same kind of fact
        // about the world that birds.h captures at exactly this point.
        terrain_ = &world.terrain;
        // -- THREE, WHICH IS WHAT THIS ARRAY ACTUALLY HOLDS ---------------
        //
        // It was sized kBirdSpecies with three initialisers. That was exact
        // until the PINK BIRD took kBirdSpecies from 3 to 4 for the cherry
        // wood, and then the fourth slot was a null pointer the loop below
        // dutifully fed to snprintf -- every run of the engine printed
        //
        //   v2: flight .../life/(null)/flight/00.vox: cannot open -- skipped
        //
        // and carried on with three, which is why it was only ever noise.
        //
        // kBirdWoodSpecies is the count birds.h already keeps for exactly this
        // question -- "every bird EXCEPT the pink one" -- so the flying flock
        // asks it rather than holding a 3 of its own. The pink bird belongs to
        // the blossom and is dealt by the PERCHED birds alone; a flock of them
        // crossing every wood is the thing that constant exists to prevent.
        // -- ...AND THE PINK ONE, WHICH IS THE BLOSSOM'S AND ONLY THE BLOSSOM'S
        //
        // (user 2026-09-20: "there doesnt seem to be any flying pink song birds
        //  in the cherry forest".)
        //
        // THE NOTE ABOVE IS STILL RIGHT and is not being undone: "a flock of
        // them crossing every wood is the thing that constant exists to
        // prevent". What was missing is the other half of it -- the pink bird
        // was excluded from the AIR everywhere, including the one wood it
        // belongs to, so the cherry forest had no flyers of any colour (see the
        // refusal in spawn()).
        //
        // Loaded LAST so the wood's three keep indices 0..kBirdWoodSpecies-1
        // and `sp` can still be chosen by a plain modulo over them.
        static const char *kNames[kBirdWoodSpecies + 1] = {"blue_bird", "robin", "cardinal",
                                                           "pink_bird"};
        for (int s = 0; s < kBirdWoodSpecies + 1; ++s) {
            std::vector<VoxModel> mo;
            mo.resize(size_t(kFlightFrames));
            bool whole = true;
            for (int f = 0; f < kFlightFrames; ++f) {
                char path[600];
                std::snprintf(path, sizeof(path), "%s/%s/flight/%02d.vox", lifeDir.c_str(),
                              kNames[s], f);
                std::string err;
                if (!voxLoad(path, &mo[size_t(f)], &err)) {
                    std::fprintf(stderr, "v2: flight %s: %s -- species skipped\n", path,
                                 err.c_str());
                    whole = false;
                    break;
                }
            }
            if (!whole) continue;
            Strip st;
            for (int f = 0; f < kFlightFrames; ++f) {
                int sx = 0, sy = 0, sz = 0;
                const int m = world.addFlyerModel(mo[size_t(f)], kNames[s], &sx, &sy, &sz, true);
                if (m < 0) { st.model.clear(); break; }
                st.model.push_back(m);
            }
            if (st.model.size() == size_t(kFlightFrames)) {
                // WHICH INDEX THE PINK ONE LANDED ON. -1 if its art is missing,
                // and spawn() reads that as "keep the old refusal" -- a cherry
                // wood with no flyers is what this was before, so failing that
                // way costs nothing that was not already lost.
                if (s == kBirdWoodSpecies) pinkStrip_ = int(strips_.size());
                strips_.push_back(std::move(st));
            }
        }
        birds_.resize(kFlockBirds);
        ready_ = !strips_.empty();
        if (ready_)
            std::printf("  flock    %zu songbird species in the air, %d frames each\n",
                        strips_.size(), kFlightFrames);
        return ready_;
    }

    // -----------------------------------------------------------------------
    // `ground` answers "how high is the world under this point", in metres. It
    // is passed in rather than reached for so the flock cannot disagree with
    // whatever the caller already calls the ground -- the same discipline the
    // loose bodies' floor follows.
    // -----------------------------------------------------------------------
    template <typename GroundF>
    // -----------------------------------------------------------------------
    // ...AND THE SOLIDS, WHICH THIS POPULATION WAS ARGUED OUT OF NEEDING.
    //
    // THE ARGUMENT WAS RIGHT AND THE CONCLUSION WAS WRONG. kSongFloorM is 28 m
    // and the tallest pine is 22, so a flock bird cannot meet a tree -- except
    // that the floor is measured from `b->g`, which is the ground under the
    // BIRD, and the tree is standing on the ground under the TREE. Cross a
    // slope and the two are metres apart: a pine rooted twelve metres up the
    // hill puts its crown through a band that is 28 m over the valley.
    //
    // --clip-test measured three creature-frames a minute at 0.2 m in. Small,
    // and the kind of small that is a bird's head passing through a branch in
    // front of you.
    // -----------------------------------------------------------------------
    void update(float dt, const Vec3 &player, const GroundF &ground,
                const std::vector<Solid> *solids = nullptr) {
        if (!ready_) return;
        solids_ = solids;
        clock_ += dt;
        // BEFORE the steps, so a bird that was paired this frame chases on this
        // frame -- and, more usefully, so a bird whose partner has just been
        // recycled is not steering at a dead slot for one tick.
        pairUp();
        for (size_t i = 0; i < birds_.size(); ++i) {
            // Differenced around the step: the heading has more than one writer
            // inside it, and instrumenting each is how one gets missed.
            const float thWas = birds_[i].th;
            step(&birds_[i], uint32_t(i), dt, player, ground);
            const float d = birds_[i].th - thWas;
            birds_[i].dth = atan2f(sinf(d), cosf(d));
        }
    }


    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        for (const Bird &b : birds_)
            if (b.live) out->push_back({Vec3(b.x, b.y, b.z), "flock", 0.25f, false});
    }

    const Solid *list() const { return solids_ ? solids_->data() : nullptr; }
    int count() const { return solids_ ? int(solids_->size()) : 0; }

    void publish(World &world, int slot0) {
        if (!ready_) return;
        for (size_t i = 0; i < birds_.size(); ++i) put(world, slot0 + int(i), birds_[i]);
        // AND THE BAND IS FLUSHED, or none of that reaches the structure. This
        // is the third system in this engine to be written without it and the
        // symptom is always the same and always silent: the records are
        // correct, nothing is drawn. birds.h carries the same note -- "27
        // perched songbirds changed exactly zero pixels".
        world.flushFlyerInstances();
    }

    // How many are flying with somebody -- the offline report prints it, and it
    // is the only way to check a PROPORTION without watching the sky.
    int chasing() const {
        int n = 0;
        for (const Bird &b : birds_) n += (b.live && b.chase >= 0) ? 2 : 0;
        return n;
    }

    // The nearest one to a point, for a headless check -- there is no other way
    // to tell "no birds placed" from "birds placed 180 m away and two pixels
    // across", and those are very different bugs.
    bool nearest(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Bird &b : birds_) {
            if (!b.live) continue;
            const float dx = b.x - p.x, dy = b.y - p.y, dz = b.z - p.z;
            const float d = dx * dx + dy * dy + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = Vec3{b.x, b.y, b.z};
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    int flying() const {
        int n = 0;
        for (const Bird &b : birds_) n += b.live;
        return n;
    }

    // -----------------------------------------------------------------------
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
        if (i < 0 || size_t(i) >= birds_.size() || !birds_[size_t(i)].live) return false;
        birds_[size_t(i)] = Bird{};
        return true;
    }

  private:
    const VoxelTerrain *terrain_ = nullptr;   // see load -- which band a bird is over

    struct Strip { std::vector<int> model; };

    struct Bird {
        bool live = false;
        float x = 0, y = 0, z = 0;
        float th = 0;        // heading
        // How far it turned this frame -- see the spin channel in
        // setFlyerInstance. place() differences the box centre, which says
        // where the bird WENT and nothing about where it FACED.
        float dth = 0.0f;
        float om = 0, omT = 0;
        float g = 0;         // the smoothed ground it is following
        float altO = 0, altT = 0;   // the wander band, eased
        float swO = 0;       // the swoop offset, eased
        float swoopA = 0, swoopT0 = 0;
        float pyPrev = 0, vyS = 0;  // vertical speed, low-passed
        float reAt = 0;      // when to pick the next behaviour
        int mode = 0;        // 0 wander / 1 thermal soar / 2 swoop
        int sp = 0;          // species
        // -- WHO IT IS AFTER. An index into birds_, or -1. chaseT is when it
        //    gives up; pairAt is the earliest it may start another one, which
        //    is what stops a broken pair re-forming on the very next frame.
        int chase = -1;
        float chaseT = 0.0f, pairAt = 0.0f;
        float animClk = 0;
    };

    bool isChased(int i) const {
        for (const Bird &b : birds_)
            if (b.live && b.chase == i) return true;
        return false;
    }

    // -----------------------------------------------------------------------
    // BREAK THE FINISHED CHASES, THEN START AT MOST ONE.
    //
    // At most one per call and not per frame in any hurry: a pair is a thing
    // you notice over seconds, and forming several at once would spend the
    // whole quarter-share in a single tick and then leave nothing to happen for
    // the next ten.
    // -----------------------------------------------------------------------
    void pairUp() {
        for (size_t i = 0; i < birds_.size(); ++i) {
            Bird &b = birds_[i];
            if (b.chase < 0) continue;
            const Bird &o = birds_[size_t(b.chase)];
            const float dx = o.x - b.x, dz = o.z - b.z;
            const bool lost = !b.live || !o.live || o.sp != b.sp || o.chase >= 0 ||
                              dx * dx + dz * dz > kSongPairDropM * kSongPairDropM;
            if (lost || clock_ > b.chaseT) {
                b.chase = -1;
                b.pairAt = clock_ + kSongChaseCool;
            }
        }

        int live = 0, paired = 0;
        for (const Bird &b : birds_) {
            live += b.live ? 1 : 0;
            paired += (b.live && b.chase >= 0) ? 2 : 0;
        }
        if (live < 2 || (paired + 2) * 4 > live) return;

        for (size_t i = 0; i < birds_.size(); ++i) {
            Bird &b = birds_[i];
            if (!b.live || b.chase >= 0 || clock_ < b.pairAt || isChased(int(i))) continue;
            int best = -1;
            float bestD2 = kSongPairM * kSongPairM;
            for (size_t j = 0; j < birds_.size(); ++j) {
                if (j == i) continue;
                const Bird &o = birds_[j];
                if (!o.live || o.sp != b.sp || o.chase >= 0 || isChased(int(j))) continue;
                const float dx = o.x - b.x, dz = o.z - b.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 >= bestD2) continue;
                bestD2 = d2;
                best = int(j);
            }
            if (best < 0) continue;
            b.chase = best;
            b.chaseT = clock_ + kSongChaseMin +
                       rnd(uint32_t(i), 0xC1u) * (kSongChaseMax - kSongChaseMin);
            return;   // one pair a pass -- see the note above
        }
    }

    // A hash stream per bird that moves with the clock, so two recycles of the
    // same slot do not produce the same bird.
    float rnd(uint32_t i, uint32_t salt) const {
        return hashUnit(salt, hashU32(i * 2654435761u, uint32_t(clock_ * 1000.0f)));
    }

    template <typename GroundF>
    void step(Bird *b, uint32_t i, float dt, const Vec3 &player, const GroundF &ground) {
        // -- THE RING ------------------------------------------------------
        if (b->live) {
            const float dx = b->x - player.x, dz = b->z - player.z;
            if (dx * dx + dz * dz > kSongKeepM * kSongKeepM) b->live = false;
            // -- FORESTS, NEVER THE DESERT ---------------------------------
            //
            // v1's own rule, and over there it is a direct instruction: "the
            // birds should be oak and pine forests only. I only want them
            // disabled in the desert". This file had no biome test at all, so
            // nine songbirds circled Death Valley.
            //
            // RECYCLED at 0.85 rather than refused at the halfway line, which
            // is v1's BIRD_OUT and its reasoning is worth keeping: a bird is
            // the one creature that SHOULD be able to cross a treeline, and
            // culling it on the 0.5 line would read as an invisible wall in
            // open sky. It drifts out over the sand and is recycled once it is
            // properly out.
            if (terrain_ && terrain_->desertMix(b->x) >= kSongDesertOut) b->live = false;
            // A PINK BIRD OUT OVER THE PINES IS THE SAME MISTAKE REVERSED, and
            // the same 0.85 line handles it: once it is properly out of the
            // blossom it is recycled, exactly as a blue bird drifting into one
            // is. Without this the blossom's birds would spread across the map.
            if (terrain_ && pinkStrip_ >= 0 && b->sp == pinkStrip_ &&
                terrain_->cherryMix(b->x) <= 1.0f - kSongDesertOut)
                b->live = false;
            // ...AND NOT OVER THE BLOSSOM EITHER, on the same pair of lines
            // and for a reason v1 states twice: "pink belongs to the cherry
            // blossom and nowhere else, and nothing else belongs there". A
            // blue bird crossing a pink wood is the second half of that.
            // (user 2026-09-19: "remove all life that isnt pink in the cherry
            //  forest".) The PERCHED birds already knew -- birds.h deals the
            // pink one by name over a cherry crown -- and this file did not.
            // -- ...UNLESS IT IS THE PINK ONE, WHICH BELONGS THERE ---------
            //
            // (user 2026-09-20: "the pink song bird looks like it is just
            //  teleporting everywhere".)
            //
            // THIS LINE IS WHY, and it is the half of the feature I did not
            // touch. The blossom gained a SPAWN for the pink bird at
            // kSongDesertIn (0.35) while this CULL still killed anything over
            // the cherry at kSongDesertOut (0.85). In the middle of a cherry
            // band both are true at once, so the bird spawned, died on the same
            // tick, respawned on the ring, and died again -- which on screen is
            // a bird appearing in a different place every frame.
            //
            // The rule the comment above states is "pink belongs to the cherry
            // blossom and nothing else belongs there". A cull that also
            // removes the pink one gets that exactly backwards. It still culls
            // every OTHER species, which is the half that was right.
            if (terrain_ && terrain_->cherryMix(b->x) >= kSongDesertOut &&
                !(pinkStrip_ >= 0 && b->sp == pinkStrip_))
                b->live = false;
            // ...AND ONE THAT HAS LEFT THE WOOD ENTIRELY GOES WITH IT. The
            // loose end of the pair -- see kSongTreeOut.
            if (terrain_ && terrain_->standDensity(b->x, b->z) < kSongTreeOut)
                b->live = false;
        }
        if (!b->live) {
            const float a = rnd(i, 0x81u) * 6.2831853f;
            const float r = kSongKeepM * (kSongRingLo + (kSongRingHi - kSongRingLo) * rnd(i, 0x82u));
            *b = Bird{};
            b->live = true;
            b->x = player.x + sinf(a) * r;
            b->z = player.z + cosf(a) * r;
            // POINTED INBOARD. A bird spawned on the ring facing outward turns
            // round in front of you and leaves, which is the one arrival that
            // draws attention to itself.
            b->th = a + 3.14159265f + (rnd(i, 0x83u) - 0.5f) * 1.2f;
            // ...AND THE OTHER HALF OF v1's PAIR: a SPAWN is refused at 0.35,
            // tighter than the 0.85 recycle, so a fresh bird never appears
            // already most of the way to being culled -- which would flicker
            // the flock along the border. The slot simply stays empty and the
            // ring is rolled again next frame, so a bird walking out of the
            // wood loses its flock gradually rather than all at once.
            // -- THE DESERT REFUSES EVERY SPECIES. THE BLOSSOM REFUSES ALL BUT ONE.
            //
            // These were one test and they are not one rule. The desert has no
            // songbird in the air at all; the cherry wood has exactly the pink
            // one, which is the same species the PERCHED birds already deal
            // there. Without the split the blossom got the desert's answer and
            // had no flyers of any colour.
            if (terrain_ && terrain_->desertMix(b->x) >= kSongDesertIn) {
                b->live = false;
                return;
            }
            // -- OFF BY DEFAULT, 2026-09-20 -----------------------------
            //
            // (user: "the pink song birds are completely glitched out on the
            //  screen".)
            //
            // MY REGRESSION, AND UNDIAGNOSED. The slot base is a fixed offset
            // so this cannot be slot contention; the strip index is taken at
            // push time so it cannot be off by one; and pink_bird has the same
            // seven flight frames and the same dimensions as the other three.
            // I could not find it by reading and cannot see it from here.
            //
            // ON AGAIN, 2026-09-20. It was defaulted off for one build while
            // undiagnosed; the cause was the CULL above killing the bird on the
            // same tick it spawned -- see the note there. Fixed, so the feature
            // stands rather than hiding behind a flag.
            // NOTHING IS BORN OVER OPEN GROUND. The tight end of the pair, and
            // the slot simply stays empty so the ring is rolled again next
            // frame -- the same way the desert refusal below leaves it.
            if (terrain_ && terrain_->standDensity(b->x, b->z) < kSongTreeIn) {
                b->live = false;
                return;
            }
            const bool blossom = terrain_ && terrain_->cherryMix(b->x) >= kSongDesertIn;
            if (blossom && pinkStrip_ < 0) {
                b->live = false;   // no pink art loaded: the old refusal stands
                return;
            }
            // THE WOOD'S THREE ARE THE FIRST THREE -- see load(), which appends
            // the pink one last precisely so this modulo stays honest. Using
            // strips_.size() here would deal pink birds across every wood,
            // which is the thing the exclusion exists to prevent.
            const int woodStrips = mini(int(strips_.size()), kBirdWoodSpecies);
            b->sp = blossom ? pinkStrip_ : (woodStrips > 0 ? int(i) % woodStrips : 0);
            b->g = ground(b->x, b->z);
            b->y = b->g + kSongCruiseM;
            b->pyPrev = b->y;
            b->animClk = rnd(i, 0x84u) * float(kFlightFrames);
        }

        // -- PICK THE NEXT BEHAVIOUR, NOT JUST THE NEXT TURN ----------------
        //
        // The JS comment is the design: "pick the next BEHAVIOUR, not just a
        // turn rate -- that is what reads as intent instead of drift".
        if (clock_ > b->reAt) {
            const float r = rnd(i, 0x90u);
            if (r < 0.18f) {
                // THERMAL SOAR: a steady banked circle, drifting a little
                // higher. One sign for the whole circle, or it is not a circle.
                b->mode = 1;
                b->omT = (rnd(i, 0x91u) < 0.5f ? 1.0f : -1.0f) * (0.45f + rnd(i, 0x92u) * 0.35f);
                b->altT = kSongSoarLo + rnd(i, 0x93u) * (kSongSoarHi - kSongSoarLo);
                b->reAt = clock_ + 6.0f + rnd(i, 0x94u) * 7.0f;
            } else if (r < 0.34f) {
                // SWOOP: fold in, trade height for speed, bleed it back into
                // the climb-out. NEVER DEEPER THAN THE HEIGHT IT HAS IN HAND,
                // which is what the min() is for.
                b->mode = 2;
                b->swoopT0 = clock_;
                b->swoopA = 1.2f + rnd(i, 0x95u) * minf(2.6f, 0.6f + b->altO);
                b->omT = (rnd(i, 0x96u) - 0.5f) * 0.4f;
                b->reAt = clock_ + kSongSwoopSec;
            } else {
                b->mode = 0;
                b->omT = (rnd(i, 0x97u) - 0.5f) * 1.0f;
                b->altT = rnd(i, 0x98u) * kSongWanderAlt;
                b->reAt = clock_ + 1.5f + rnd(i, 0x99u) * 3.0f;
            }
        }

        // -- ...AND CHASING BEATS ALL THREE OF THEM ------------------------
        //
        // AFTER the behaviour block, so it overrides whatever was just picked,
        // and it forces mode 0: a soar is a circle and a swoop is a scripted
        // dive, and neither of them can be steered at a moving target.
        //
        // IT AIMS AT THE BIRD, NOT BEHIND IT. See the note over kSongPairM --
        // pointing at the leader's own position is what produces the overshoot
        // and the recovery turn that make this read as a chase rather than as
        // two birds in formation.
        if (b->chase >= 0 && size_t(b->chase) < birds_.size()) {
            const Bird &o = birds_[size_t(b->chase)];
            b->mode = 0;
            const float ex = o.x - b->x, ez = o.z - b->z;
            if (ex * ex + ez * ez > 1e-4f) {
                const float want = atan2f(ex, ez);
                float d = want - b->th;
                while (d > 3.14159265f) d -= 6.2831853f;
                while (d < -3.14159265f) d += 6.2831853f;
                b->omT = clampf(d * 2.2f, -kSongChaseYaw, kSongChaseYaw);
            }
            // Its height is the other bird's, expressed in the band this one
            // holds -- so it climbs and dives with it instead of chasing across
            // a fixed ceiling.
            b->altT = clampf((o.y - b->g) - kSongCruiseM, 0.0f, kSongSoarHi);
            b->reAt = clock_ + 0.4f;   // do not re-roll a behaviour under it
        }

        b->om += (b->omT - b->om) * (1.0f - expf(-2.5f * dt));
        b->th += b->om * dt;
        const float hx = sinf(b->th), hz = cosf(b->th);

        // -- ENERGY EXCHANGE ------------------------------------------------
        //
        // "a dive buys speed (up to +50%), a climb costs it -- the swoop reads
        // as physics, not animation". The coefficient is v1's own, rescaled for
        // metres: its -vyS * 0.045 on a voxel speed is -vyS * 0.45 on ours.
        const float spd = kSongSpeed * (1.0f + clampf(-b->vyS * 0.45f, -0.28f, 0.5f)) *
                          (b->chase >= 0 ? kSongChaseSpd : 1.0f);
        // A REFUSED STEP IS A TURN HERE TOO -- see the long note in
        // butterflies.h. A flock bird meets a crown far less often (it holds
        // 28 m and the tallest pine is 22), but when it does it is doing 5.5
        // m/s and the same thing happens: it slides along the branch until its
        // next behaviour roll, which is up to ten seconds away.
        if (!flySlide(list(), count(), VOXEL_M, kSongBodyR, &b->x, &b->y, &b->z,
                      b->x + hx * spd * dt, b->y, b->z + hz * spd * dt)) {
            const float side = (b->om != 0.0f) ? (b->om > 0.0f ? 1.0f : -1.0f) : 1.0f;
            b->om = side * 2.6f;
            b->omT = b->om;
            b->mode = 0;             // not mid-swoop: a dive cannot be steered
            b->reAt = clock_ + 0.5;  // ...and do not re-roll a behaviour over it
        }

        // -- TERRAIN FOLLOW, AND IT IS ASYMMETRIC ---------------------------
        //
        // Climb fast ahead of rising ground, sink only gently -- so crossing a
        // gorge is a mild swoop rather than a plunge below the rims. The
        // lookahead is what makes it anticipate rather than react.
        const float gT = maxf(ground(b->x, b->z), ground(b->x + hx * kSongLookM,
                                                         b->z + hz * kSongLookM));
        b->g += (gT - b->g) * (1.0f - expf(-(gT > b->g ? 3.0f : 0.35f) * dt));
        b->altO += (b->altT - b->altO) * (1.0f - expf(-0.4f * dt));

        // A HALF-SINE DIVE, eased -- so an interrupted swoop recovers smoothly
        // instead of popping back to level.
        const float swT = (b->mode == 2)
                              ? -b->swoopA * sinf(3.14159265f *
                                                  minf(1.0f, (clock_ - b->swoopT0) / kSongSwoopSec))
                              : 0.0f;
        b->swO += (swT - b->swO) * (1.0f - expf(-4.0f * dt));

        const float py = maxf(b->g + kSongFloorM,
                              b->g + kSongCruiseM + b->altO + b->swO +
                                  sinf(clock_ * 1.2f) * kSongBobM);
        // RAW, THEN LOW-PASSED. The ground samples step as voxel boundaries
        // cross, so the raw vertical speed is noisy -- and BOTH the attitude
        // and the bank read this, so unfiltered the pitch snaps.
        const float vy = (py - b->pyPrev) / maxf(dt, 1e-4f);
        b->pyPrev = py;
        b->vyS += (vy - b->vyS) * (1.0f - expf(-5.0f * dt));
        // THE HEIGHT IS A WRITE AND NOT A STEP, which is why it is guarded
        // separately: everything above computes where the bird SHOULD be and
        // this is the one line that puts it there. The escape term is
        // flySlide's -- a bird already in a crown must be able to fly out.
        if (!solidsTouch(list(), count(), b->x, py, b->z, kSongBodyR, VOXEL_M) ||
            solidsContain(list(), count(), b->x, b->y, b->z, VOXEL_M))
            b->y = py;

        // THE FLAP NEVER STOPS. The JS engine holds a glide frame on a flag
        // that is permanently false -- "COASTING REMOVED (user) -- the bird
        // never holds the spread-wing glide frame; it cycles its flap strip
        // continuously" -- so that is what this does, and the glide frame is
        // not loaded at all.
        b->animClk += dt * kSongFlapFps;
    }

    // -----------------------------------------------------------------------
    // THE POSE, WHICH IS A BANKED TURN AND NOT A YAW.
    //
    // Everything else in this scene is a quarter turn about Y; a bird is the
    // exception that earns a full basis. Built the way the JS engine builds it:
    //
    //     F    the 3D flight direction, pitch included
    //     Xw   the wingspan, up x F -- for om > 0 this side faces the turn
    //     Zw   the body's up, F x Xw
    //     bank v.om/g, rolled about F, so the wing on the inside DIPS
    //
    // It is still a rotation, so it is still orthonormal and World::place's
    // no-adjugate assumption holds.
    // -----------------------------------------------------------------------
    void put(World &world, int slot, const Bird &b) const {
        if (!b.live || strips_.empty()) {
            world.setFlyerInstance(slot, 0, nullptr, 0, 0, 0, nullptr, false);
            return;
        }
        const float hx = sinf(b.th), hz = cosf(b.th);
        const float fy = clampf(b.vyS / kSongSpeed * 3.0f, -0.6f, 0.6f);
        const float fl = sqrtf(1.0f + fy * fy);
        const float Fx = hx / fl, Fy = fy / fl, Fz = hz / fl;

        const float xl = maxf(1e-4f, sqrtf(Fx * Fx + Fz * Fz));
        const float ax = Fz / xl, az = -Fx / xl;                 // Xw0, up x F
        const float ux = Fy * az, uy = Fz * ax - Fx * az, uz = -Fy * ax;   // Zw0 = F x Xw0

        // ROLLS INTO THE TURN, and harder the faster it is going -- v.om/g.
        const float spd = kSongSpeed * (1.0f + clampf(-b.vyS * 0.45f, -0.28f, 0.5f));
        const float bank = clampf(spd * b.om / 9.81f * 1.8f, -0.5f, 0.5f);
        const float cb = cosf(bank), sb = sinf(bank);
        const float wx = ax * cb - ux * sb, wy = -uy * sb, wz = az * cb - uz * sb;
        const float bx = ux * cb + ax * sb, by = uy * cb, bz = uz * cb + az * sb;

        // COLUMNS ARE WHERE THE MODEL'S OWN AXES GO. World::place reads m by
        // ROWS (see the ox/oy/oz product in birds.h), so the image of the
        // model's local x is (m0, m3, m6).
        //
        // The strip is authored beak-along -z, which is the JS engine's "the
        // beak (model -depth) points along it" carried through toWorld's
        // Z-up-to-Y-up swap.
        const float m[9] = {wx, bx, -Fx,
                            wy, by, -Fy,
                            wz, bz, -Fz};
        const Strip &st = strips_[size_t(b.sp) % strips_.size()];
        const int fi = int(b.animClk) % int(st.model.size());
        // THE BIRD, NOT ITS BOX, AND HERE IT MATTERS. This publish hands
        // place() the bird's position as the TRANSLATION -- the model's corner
        // -- and takes no half-box off at all, so the centre it derived was the
        // corner plus half of whichever of the seven flight frames is up. They
        // are not one size, so that centre bobbed a voxel or two a beat on top
        // of the flight, and the bob went into the motion vector. See
        // World::place, where the perched songbirds' version of this was
        // measured.
        const float anchor[3] = {b.x, b.y, b.z};
        // ...AND THE TURN -- see Bird::dth. A flock wheels constantly, which is
        // the case this channel exists for.
        const float spin[4] = {b.x, b.y, b.z, b.dth};
        world.setFlyerInstance(slot, st.model[size_t(fi)], m, b.x, b.y, b.z, nullptr, true, spin,
                               anchor);
    }

    bool ready_ = false;
    float clock_ = 0.0f;
    std::vector<Strip> strips_;
    // WHICH STRIP IS THE PINK BIRD, or -1 if its flight art is missing.
    // The blossom flies this one and nothing else; every other wood flies
    // the first kBirdWoodSpecies and never this. See spawn().
    int pinkStrip_ = -1;
    std::vector<Bird> birds_;
    // Borrowed for the length of one update() -- the same contract every other
    // population in this engine has with this list.
    const std::vector<Solid> *solids_ = nullptr;
};

}  // namespace v2
