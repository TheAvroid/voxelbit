#pragma once
// ---------------------------------------------------------------------------
// lifehit.h -- v1'S KILL MECHANICS, ported.
//
// "1. when hitting life, it turns an emmisive red ... 2. when killing life, the
// life breaks apart into multiple pieces ... 5. then lastly, have life drop a
// raw steak. there are unique animals that dont drop anything, like the song
// birds and worms." (user 2026-09-14.)
//
// ---------------------------------------------------------------------------
// THE BAND IS THE SEAM, AND THAT IS THE WHOLE DESIGN
// ---------------------------------------------------------------------------
//
// v1 keeps every creature in ONE array (`wbf`) split into named bands, so
// "which animal is under the crosshair", "how many hits does it take" and "does
// it leave a carcass" are all one index into one list. v2 keeps seven
// populations in seven files with nothing in common -- but every one of them
// draws through the SAME instance band (see kFlyerInstances), because that is
// the engine's only route to an instance whose transform changes every frame.
//
// So the band is this engine's `wbf`. A slot in it is an animal, its transform
// is where that animal is, and World::flyerAt hands over both. That is what
// makes this file a table and an aim test rather than seven copies of both.
//
// WHAT IT STILL CANNOT DO is retire the animal: the population owns whether its
// member is alive, and nothing here can reach into it. That is one method per
// population (`killSlot`) and App::killLifeAt is the dispatcher -- the same
// shape as App::nearestLife, and for the same reason.
//
// ---------------------------------------------------------------------------
// WHICH ANIMALS LEAVE MEAT -- MEASURED OFF v1, NOT GUESSED
// ---------------------------------------------------------------------------
//
// v1's `dropsMeat(j)` is three rules, and the user asked for exactly this to be
// investigated rather than invented:
//
//   * TRUE for the whole land-mammal run, MAM_0..MAM_END -- bunny, armadillo,
//     skunk, porcupine (and its flamingo, which v2 has not got).
//   * TRUE for kind 6, the FISH, which its dropMeat floats to the waterline
//     rather than leaving on the seabed.
//   * TRUE for the desert band's named species only: DES_MEAT = {desert_mouse,
//     cobra, scorpion, gecko, grass_snake, frog}.
//   * FALSE for everything else, and its note says which and why: "ant, fly,
//     spider and now the BEE leave nothing, the same line the user drew asking
//     for no drops off the bugs".
//
// Falling out of that, and matching what the user said to expect:
//
//   MEAT  bunny, skunk, armadillo, porcupine, mouse, snake, frog,
//         and all six fish
//   NONE  worm, ant, fly, ladybug, firefly, butterfly, bee,
//         songbird (perched AND flying), duck, duckling, dragonfly
//
// THE DUCK IS THE ONE WORTH SAYING OUT LOUD. It is not a bug and it is plainly
// game, but v1 keeps its ducks in the DUCK_0 band -- BELOW MAM_0 -- so
// dropsMeat is false for them there, and the ask was to follow v1 rather than
// to redraw the line. One word in the table changes it.
//
// ---------------------------------------------------------------------------
// HOW MANY BLOWS -- ALSO v1'S
// ---------------------------------------------------------------------------
//
//   HITS_TO_KILL        3    any ordinary tool, or an empty hand
//   the AXE             1    "the axe is the killing tool and ignores this"
//   DES_FRAIL           1    ant, fly, scorpion, spider, bee, ladybug
//   the FLY_0 band      1    butterflies, moths, fireflies, dragonflies
//   the WORM band       1    "something that size surviving three blows reads
//                            as absurd for the same reason"
//
// v1's arrow and knife counts (1 and 2) are left out: v2's bow does not shoot
// at life yet and it has no knife. Adding either is one line here.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <vector>

#include "world/world.h"
#include "core/vecmath.h"
// FOR THE COUNTS, AND ONLY FOR THE COUNTS. The table below is the band's
// own layout read back, so it has to name the same constants the two
// populations with sub-runs publish with -- a literal here is a table that
// goes quietly wrong the day somebody adds a fish.
#include "ai/critters.h"
#include "ai/lake.h"

namespace v2 {

// v1's melee reach, shared with its own swing so the two can never drift:
// REACH_H 53 vox, REACH_3D 107, AIM_FORGIVE 1.6.
inline constexpr float kLifeReachM = 5.3f;
inline constexpr float kLifeReach3dM = 10.7f;
inline constexpr float kAimForgiveM = 0.16f;
// v1's HITS_TO_KILL and HURT_MS. "ONE blink, half a second" -- and its note on
// why not shorter: an 83 ms flash is inside the temporal filter's own blend and
// is very nearly invisible on screen. The same is true here, more so.
inline constexpr int kHitsToKill = 3;
inline constexpr double kHurtMs = 500.0;
// ...and v1 steps the fade in twelve phases at 24 fps rather than running it
// smooth, "so it reads as a deliberate blink".
inline constexpr int kHurtPhases = 12;

// ---------------------------------------------------------------------------
// WHAT LIVES IN A SLOT.
//
// `name` is null for a slot that is not an animal at all -- the pause room's
// buttons, the asset editor's gizmo arrows, the particles, and the lily pads,
// which are the one thing in the lake's own run that is not alive.
// ---------------------------------------------------------------------------
struct LifeKind {
    const char *name = nullptr;
    bool meat = false;    // v1's dropsMeat
    bool frail = false;   // v1's one-hit rule
    bool alive() const { return name != nullptr; }
};

// ---------------------------------------------------------------------------
// ...AND THE TABLE THAT SAYS SO, WHICH IS THE BAND'S OWN LAYOUT READ BACK.
//
// ONE RUN PER POPULATION, in the order gpu/world.h reserves them and each
// file's own publish() writes them. Every count below is the same constant that
// publish uses, never a literal -- a population that grows moves this with it.
// ---------------------------------------------------------------------------
inline LifeKind lifeAtSlot(int slot) {
    if (slot < 0 || slot >= kFlyerInstances) return {};
    int s = slot;
    // -- butterflies (render/butterflies.h) -- v1's FLY_0 band: one hit -----
    if (s < kButterflySlots) return {"butterfly", false, true};
    s -= kButterflySlots;
    // -- the songbirds in the trees (render/birds.h) ------------------------
    if (s < kBirdSlots) return {"songbird", false, false};
    s -= kBirdSlots;
    // -- the lake, in its publish order: fish, pads, dragonflies, ducks -----
    if (s < kLakeSlots) {
        const int fish = kSalmonCount + kBassCount + kKoiCount + kMinnowCount + kCatfishCount +
                         kBluegillCount;
        if (s < fish) return {"fish", true, false};   // v1: kind 6 leaves meat
        s -= fish;
        if (s < kLilyCount) return {};                // a lily pad is not alive
        s -= kLilyCount;
        if (s < kDflyCount) return {"dragonfly", false, true};
        s -= kDflyCount;
        return {"duck", false, false};                // ...and its ducklings
    }
    s -= kLakeSlots;
    // -- the songbirds in the air (render/birdflock.h) ----------------------
    if (s < kFlockSlots) return {"songbird", false, false};
    s -= kFlockSlots;
    // -- the bunnies. THE ASSET EDITOR DRAWS ITS GIZMO ARROWS THROUGH SLOTS
    //    1..3 OF THIS RUN (see assetedit.h), so the run is wider than the
    //    population and the spare slots are not animals. A slot nothing is
    //    drawn in answers false from flyerAt anyway, so this only has to be
    //    right about the ones that ARE drawn -- and the editor is not up while
    //    anything is being hit.
    if (s < kBunnySlots) return {"bunny", true, false};
    s -= kBunnySlots;
    // -- the four marchers, the worm and the snake (render/bunnies.h) -------
    //
    // ONE RUN, SIX SPECIES, and which one a slot holds is a property of the
    // MARCHER rather than of the slot: marchers_ is one vector of mixed kinds.
    // So this answers for the run and App::lifeKindAt refines it by asking
    // Bunnies what kind that marcher actually is -- the worm is the only one
    // of the six that leaves nothing, and it is also the only frail one.
    if (s < kMarchSlots) return {"marcher", true, false};
    s -= kMarchSlots;
    // -- the bees. v1's DES_FRAIL: one hit, no meat -------------------------
    if (s < kBeeSlots) return {"bee", false, true};
    s -= kBeeSlots;
    // -- the pause room's buttons ------------------------------------------
    if (s < kButtonSlots) return {};
    s -= kButtonSlots;
    // -- the small life (render/critters.h), in ITS publish order -----------
    if (s < kCritterSlots) {
        if (s < kFireflyCount) return {"firefly", false, true};
        s -= kFireflyCount;
        if (s < kAntCount) return {"ant", false, true};
        s -= kAntCount;
        if (s < kHouseflyCount) return {"fly", false, true};
        s -= kHouseflyCount;
        if (s < kLbugCount) return {"ladybug", false, true};
        s -= kLbugCount;
        if (s < kFrogCount) return {"frog", true, false};   // v1's DES_MEAT
        return {};                                          // the spare margin
    }
    // -- and the particles, which are not alive either ----------------------
    return {};
}

// ---------------------------------------------------------------------------
// ONE ANIMAL'S WOUND.
//
// v1 keeps `B.hits` on the creature and one global HURT box; here the animal is
// a band slot and the population knows nothing about being hit, so the whole of
// it lives in this file. `born` is what stops a wound outliving its owner: a
// slot that is recycled while a flash is running belongs to a different animal,
// and it must not inherit the last one's blood. See v1's own note on B.born,
// which is the same trick for the same reason.
// ---------------------------------------------------------------------------
struct Wound {
    int hits = 0;
    double flashT0 = -1e9;
    bool dying = false;
    Vec3 lastAt{0.0f, 0.0f, 0.0f};
};

// ---------------------------------------------------------------------------
class LifeHits {
  public:
    // =======================================================================
    // THE HITBOX IS THE ANIMAL'S OWN BOX.
    //
    // (user 2026-09-19: "adjust the hitboxes of the life and make sure they
    //  are accurate. I feel like Im shooting an arrow and its hitting the life
    //  but its not registering.")
    //
    // WHAT WAS HERE was v1's sphere of the MEAN half-extent, centred on what
    // World::flyerAt handed back. Three things were wrong with that and they
    // compound:
    //
    //   1. THE CENTRE WAS THE MOTION ANCHOR, which for a rabbit is its FEET
    //      and for a perched songbird is its toes. See World::flyerMid_ for
    //      how that happened -- it is the 2026-09-14 anchor fix, which was
    //      right, meeting a hit test that had always read the same field.
    //      Half of every marcher's hitbox was underground.
    //
    //   2. A SPHERE OF THE MEAN HALF-EXTENT FITS ALMOST NOTHING. The grass
    //      snake is 0.5 x 1.7 m and the mean gives a 0.37 m ball: it reaches
    //      neither end of the animal and sticks out past both its sides. The
    //      species it fits worst are the long thin ones you have to aim at.
    //
    //   3. AN ARROW ASKED AT A POINT, once per 5 ms step -- 0.24 m of flight
    //      at full draw. A shaft can pass clean through a small animal between
    //      two samples and neither sample be inside it. That is a hit that
    //      does not register with nothing wrong at either end of it.
    //
    // So: the axis-aligned box the instance is actually drawn in (flyerBox),
    // grown by kAimForgiveM, tested as a SEGMENT for anything that moves and
    // as a ray for the crosshair. One slab routine, three entry points.
    // =======================================================================

    // The classic slab test, on a segment from a to b. Returns the parameter
    // of the entry point in [0, 1], or -1. A start INSIDE the box returns 0,
    // which is what makes the point test below a special case of this one
    // rather than a second piece of arithmetic that can disagree with it.
    static float hitBox(const Vec3 &a, const Vec3 &b, const Vec3 &mid, const Vec3 &half) {
        float t0 = 0.0f, t1 = 1.0f;
        const float o[3] = {a.x, a.y, a.z};
        const float d[3] = {b.x - a.x, b.y - a.y, b.z - a.z};
        const float c[3] = {mid.x, mid.y, mid.z};
        const float h[3] = {half.x, half.y, half.z};
        for (int k = 0; k < 3; ++k) {
            const float lo = c[k] - h[k], hi = c[k] + h[k];
            if (d[k] > -1e-9f && d[k] < 1e-9f) {
                // Parallel to this pair of planes: it is either between them
                // for the whole segment or it never was.
                if (o[k] < lo || o[k] > hi) return -1.0f;
                continue;
            }
            float ta = (lo - o[k]) / d[k], tb = (hi - o[k]) / d[k];
            if (ta > tb) { const float sw = ta; ta = tb; tb = sw; }
            if (ta > t0) t0 = ta;
            if (tb < t1) t1 = tb;
            if (t0 > t1) return -1.0f;
        }
        return t0;
    }

    // -----------------------------------------------------------------------
    // WHAT IS UNDER THE CROSSHAIR -- v1's aimedCreature, on the band.
    //
    // A RAY AGAINST THE ANIMAL, NOT A CONE. v1 replaced its own 35-degree cone
    // with exactly this and said why: "proj = how far along the view it sits;
    // perp = how far the view ray passes from it." What has changed since is
    // only the SHAPE being missed by: v1's own note calls a sphere round the
    // box corners "far wider than the animal actually looks", and the box
    // itself is narrower than that sphere on every axis while still covering
    // the parts of the animal a sphere of the mean never reached.
    //
    // NEAREST ALONG THE RAY, not nearest by centre. Two animals overlapping the
    // crosshair used to be settled by which middle was closer, which on a snake
    // lying in front of a rabbit picks the rabbit.
    // -----------------------------------------------------------------------
    int aim(const World &w, const Vec3 &eye, const Vec3 &dir) const {
        int best = -1;
        float bestT = 1e30f;
        for (int i = 0; i < kFlyerInstances; ++i) {
            if (!lifeAtSlot(i).alive()) continue;
            Vec3 mid{0.0f, 0.0f, 0.0f}, half{0.0f, 0.0f, 0.0f};
            if (!w.flyerBox(i, &mid, &half)) continue;   // not drawn: not there
            const float dx = mid.x - eye.x, dy = mid.y - eye.y, dz = mid.z - eye.z;
            const float dh = std::sqrt(dx * dx + dz * dz);
            const float d3 = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (dh > kLifeReachM || d3 > kLifeReach3dM) continue;
            // NO 0.2 m FLOOR ANY MORE. It was there because the centre could be
            // inside the player and a zero-length vector has no direction; the
            // box test needs no direction to the animal at all, and an animal
            // standing on your boots is one you can hit.
            // NOT `far`: windows.h still defines it as an empty macro from the
            // near/far pointer days, so `const Vec3 far{...}` compiles to
            // `const Vec3 {...}` and the error names neither the word nor the
            // reason. The SIXTH time in this project -- app.h, poi.h, birds.h
            // and lake.h all carry the same note.
            const Vec3 tip{eye.x + dir.x * kLifeReach3dM, eye.y + dir.y * kLifeReach3dM,
                           eye.z + dir.z * kLifeReach3dM};
            const Vec3 grown{half.x + kAimForgiveM, half.y + kAimForgiveM,
                             half.z + kAimForgiveM};
            const float t = hitBox(eye, tip, mid, grown);
            if (t < 0.0f || t >= bestT) continue;
            bestT = t;
            best = i;
        }
        return best;
    }

    // -----------------------------------------------------------------------
    // ...AND WHAT A SHAFT PASSED THROUGH BETWEEN TWO STEPS.
    //
    // aim() above is a ray from an eye; an arrow is a segment between where it
    // was and where it is about to be. Same band, same box, no reach -- a bow's
    // range is however far the arrow flies, which is the arrow's business and
    // not this file's.
    //
    // THE SEGMENT IS THE WHOLE POINT. arrows.h steps at 5 ms, which is 0.24 m
    // at a full draw, and a firefly is 0.3 m across: asked only at the sample
    // points, a shaft dead through one could miss on both sides of it. Asked
    // along the step, it cannot.
    // -----------------------------------------------------------------------
    int along(const World &w, const Vec3 &from, const Vec3 &to) const {
        int best = -1;
        float bestT = 1e30f;
        for (int i = 0; i < kFlyerInstances; ++i) {
            if (!lifeAtSlot(i).alive()) continue;
            Vec3 mid{0.0f, 0.0f, 0.0f}, half{0.0f, 0.0f, 0.0f};
            if (!w.flyerBox(i, &mid, &half)) continue;
            const Vec3 grown{half.x + kAimForgiveM, half.y + kAimForgiveM,
                             half.z + kAimForgiveM};
            const float t = hitBox(from, to, mid, grown);
            if (t < 0.0f || t >= bestT) continue;   // the first thing it reaches
            bestT = t;
            best = i;
        }
        return best;
    }

    // A shaft that is simply AT a point -- the same question with no travel in
    // it, kept because --kill-test and the level's own probes ask it that way.
    int at(const World &w, const Vec3 &p) const { return along(w, p, p); }

    // -----------------------------------------------------------------------
    // A BLOW LANDS.
    //
    // Returns what happened, so the caller can throw the sparks, the smoke and
    // the meat without this file having to know about any of them. v1 puts the
    // sparks, the sound and the spook ABOVE its own wound/kill split for
    // exactly that reason -- every blow that lands does those, whatever it does
    // to the animal.
    // -----------------------------------------------------------------------
    struct Blow {
        int slot = -1;
        bool landed = false;
        bool killed = false;
        Vec3 at{0.0f, 0.0f, 0.0f};
        LifeKind kind{};
        int hits = 0;
        int needed = 0;
    };

    Blow strike(const World &w, int slot, const LifeKind &kind, bool oneBlow, double nowMs) {
        Blow b;
        b.slot = slot;
        b.kind = kind;
        if (slot < 0 || !kind.alive()) return b;
        Vec3 at{0.0f, 0.0f, 0.0f};
        float r = 0.0f;
        if (!w.flyerAt(slot, &at, &r)) return b;
        Wound &q = wound(slot);
        if (q.dying) return b;   // already flashing its way out -- v1's own guard
        b.landed = true;
        b.at = at;
        q.lastAt = at;
        // THE FLASH IS ON EVERY HIT, not just the first. v1: "gating this to
        // the first one made hits two and three look like misses."
        q.flashT0 = nowMs;
        const int need = (oneBlow || kind.frail) ? 1 : kHitsToKill;
        q.hits += 1;
        b.hits = q.hits;
        b.needed = need;
        if (q.hits < need) return b;   // wounded, not dead
        q.dying = true;
        b.killed = true;
        return b;
    }

    // The flash is over and the slot is free again. Called once the kill has
    // been carried out, so a recycled slot starts clean.
    void clear(int slot) {
        if (slot < 0 || slot >= kFlyerInstances) return;
        w_[size_t(slot)] = Wound{};
    }

    // -----------------------------------------------------------------------
    // ...AND THE RED, PUBLISHED EVERY FRAME.
    //
    // v1's own fade: twelve phases over HURT_MS, stepped rather than smooth.
    // The write is one float per wounded slot and nothing at all for the rest,
    // so a world where nothing has been hit costs one compare per slot.
    // -----------------------------------------------------------------------
    void publish(World &world, double nowMs) {
        for (int i = 0; i < kFlyerInstances; ++i) {
            Wound &q = w_[size_t(i)];
            if (q.flashT0 < -1e8) continue;
            const double e = (nowMs - q.flashT0) / kHurtMs;
            if (e < 0.0 || e >= 1.0) {
                world.setFlyerHurt(i, 0.0f);
                q.flashT0 = -1e9;
                continue;
            }
            const int phase = int(e * double(kHurtPhases));
            world.setFlyerHurt(i, 1.0f - float(phase) / float(kHurtPhases));
        }
    }

    bool flashing(int slot) const {
        return slot >= 0 && slot < kFlyerInstances && w_[size_t(slot)].flashT0 > -1e8;
    }
    bool dying(int slot) const {
        return slot >= 0 && slot < kFlyerInstances && w_[size_t(slot)].dying;
    }
    int hitsOn(int slot) const {
        return (slot >= 0 && slot < kFlyerInstances) ? w_[size_t(slot)].hits : 0;
    }

  private:
    Wound &wound(int slot) { return w_[size_t(slot)]; }
    std::vector<Wound> w_ = std::vector<Wound>(size_t(kFlyerInstances));
};

}  // namespace v2
