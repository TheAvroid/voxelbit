#pragma once
// ---------------------------------------------------------------------------
// particles.h -- v1'S SPARKS AND ITS DEATH SMOKE, ported.
//
// "when life is killing 4 spark voxels play. (emissive)" / "also on death the
// life plays smoke that rises" / "everytime a tool hits something, play 4
// sparks just like in v1. you can import the v1 spark settings."
// (user 2026-09-14.)
//
// THE SETTINGS ARE v1'S, TO THE NUMBER, at ten centimetres to the voxel -- that
// engine's world and this one share a voxel size, so every speed below is its
// own figure divided by ten and every time is unchanged:
//
//                       v1 (vox)            here (metres)
//     spark out         12 + rand*22        1.2 + rand*2.2 m/s
//     spark up          12..16 + rand*22    see kSparkUp0 -- v1 uses three
//                                           slightly different lifts at its
//                                           three call sites and they are kept
//     spark life        0.4 + rand*0.3 s    unchanged
//     spark gravity     85                  8.5 m/s^2
//     spark spin        7 rad/s             unchanged
//     smoke out         1 + rand*2          0.1 + rand*0.2 m/s
//     smoke up          12 + rand*6         1.2 + rand*0.6 m/s
//     smoke life        1.0 + rand*0.5 s    unchanged
//     smoke gravity     1.5                 0.15 m/s^2  -- it keeps rising
//     smoke spin        2 + rand*2.5 rad/s  unchanged
//     smoke wander      sin(t*1.6)*1.4      0.14 m, and cos(t*1.9)*1.4 on z
//     smoke column      (i-4)*0.55 vox      the sixteen are born stacked
//
// AND THE ART IS v1'S TOO -- three one-voxel models, its own RGB:
//
//     spark   (255, 208, 112)   the amber ember every blow throws
//     hit     (222, 38, 30)     ...and the RED one, when the thing struck is
//                               alive. v1: "striking a life form throws RED
//                               voxels rather than the rocks' amber embers.
//                               Identical particle in every other way."
//     smoke   (255, 255, 255)   white, and drawn at 20% alpha
//
// WHY A BURST IS FOUR AND NOT A COUNT. v1 stamps `sparks3d[0..3]` outright on
// every burst, so a second blow inside half a second replaces the first blow's
// embers rather than adding to them. That is not a budget, it is the effect:
// four bright specks at the point of impact, and at the rate a player can swing
// the replacement is never visible. The SMOKE is the other way round -- it
// takes free slots only, because a death mid-plume must not delete a column
// that is still rising.
//
// EMISSIVE IS A MATERIAL HERE. See V6Params::emitters: a spark is one voxel
// wearing a material that renders at its own radiance, lights nothing and costs
// no shadow ray -- the firefly's trick, which is what v1 does too. That means
// the two spark colours must be PRIVATE materials, and getting one is not a
// matter of registering the colour: see Critters::privateTint and the note over
// it. The smoke is not emissive; it is translucent, which is V6Material::alpha.
// ---------------------------------------------------------------------------
#include <cmath>
#include <cstdio>
#include <vector>

#include "../gpu/world.h"
#include "../scene/vox.h"

namespace v2 {

// v1's three particle colours, as authored.
inline constexpr uint8_t kSparkRgb[3] = {255, 208, 112};
inline constexpr uint8_t kSparkRedRgb[3] = {222, 38, 30};
inline constexpr uint8_t kSmokeRgb[3] = {255, 255, 255};

// ...AND THE CANDIDATES EACH MAY MOVE TO. A private material is one with
// nothing else within Palette::kModelMatch of it, and whether the authored
// colour has that is a property of everything else already loaded -- so each
// one carries alternatives a shade or two off, walked until one is free. Same
// machinery, same reason, as the firefly's kGlowCand.
inline constexpr uint8_t kSparkCand[][3] = {
    {255, 208, 112}, {252, 205, 118}, {255, 214, 104}, {248, 200, 120}, {255, 202, 96}};
inline constexpr uint8_t kSparkRedCand[][3] = {
    {222, 38, 30}, {228, 34, 24}, {216, 44, 36}, {232, 30, 38}, {210, 40, 22}};
inline constexpr uint8_t kSmokeCand[][3] = {
    {236, 236, 240}, {230, 232, 238}, {242, 240, 234}, {226, 230, 234}, {238, 234, 244}};

// HOW BRIGHT A SPARK RENDERS. The firefly is 26 working units and is a speck
// seen across a dark clearing; a spark is the same size and has to read in FULL
// SUN, which is the one thing the firefly never has to do. v1 draws its sparks
// with a hot core in a coloured envelope -- the same shape as its meteor note
// -- so this sits well above the daylight it competes with.
inline constexpr float kSparkNits = 42.0f;
// ...and the smoke's alpha. v1: "rendered TRANSLUCENT (20%)".
inline constexpr float kSmokeAlpha = 0.20f;

// v1's ballistics, in metres. See the table at the top of this file.
inline constexpr float kSparkG = 8.5f;
inline constexpr float kSmokeG = 0.15f;
inline constexpr float kSparkSpin = 7.0f;
// The three lifts v1 uses, kept apart because they are what its three call
// sites actually say: the rock clash throws hardest, a blow on flesh a little
// less, a death between the two.
inline constexpr float kSparkUpClash = 1.6f, kSparkUpHit = 1.4f, kSparkUpDeath = 1.2f;

// ---------------------------------------------------------------------------
class Particles {
  public:
    bool ready() const { return spark_ >= 0; }

    // -----------------------------------------------------------------------
    // THREE ONE-VOXEL MODELS, BUILT IN CODE.
    //
    // v1 builds them in code too (`items.push({w:1,d:1,h:1,cells:[[255,208,112]]})`)
    // and for the same reason: a single voxel of a named colour is a fact about
    // this file, not an asset somebody could re-author without knowing what it
    // is for. The .vox route would also snap the colour onto whatever the
    // palette already holds, which is exactly what a private material must not
    // do.
    // -----------------------------------------------------------------------
    bool load(World &world, Tracer *tracer) {
        spark_ = one(world, kSparkCand, 5, &sparkMtl_, &sparkShared_);
        red_ = one(world, kSparkRedCand, 5, &redMtl_, &redShared_);
        smoke_ = one(world, kSmokeCand, 5, &smokeMtl_, &smokeShared_);
        if (spark_ < 0) {
            std::fprintf(stderr, "v2: the spark model would not load -- no sparks\n");
            return false;
        }
        // THE TWO EMBERS ARE LIGHTS AND THE SMOKE IS NOT. A translucent grey
        // cube that also glowed would be a lamp in a puff of steam.
        if (tracer) {
            tracer->addEmitter(sparkMtl_, Vec3(kSparkNits * 1.00f, kSparkNits * 0.82f,
                                               kSparkNits * 0.44f));
            if (red_ >= 0)
                tracer->addEmitter(redMtl_, Vec3(kSparkNits * 0.95f, kSparkNits * 0.16f,
                                                 kSparkNits * 0.13f));
        }
        // ALPHA ONLY ON AN ENTRY THAT IS OURS ALONE. See the note over one():
        // alpha belongs to the ENTRY, so setting it on a shared one turns
        // whatever else wears it see-through. Solid smoke is a worse-looking
        // plume; see-through bark is a broken world.
        if (smoke_ >= 0 && smokeShared_ == 1) world.palette.setAlpha(smokeMtl_, kSmokeAlpha);
        else if (smoke_ >= 0)
            std::fprintf(stderr,
                         "v2: the smoke material is shared with %d others -- drawn solid\n",
                         smokeShared_);
        std::printf("  sparks   4 + %d smoke, materials %u/%u/%u "
                    "(%d/%d/%d within tolerance -- 1 is private)\n",
                    kParticleSmoke, unsigned(sparkMtl_), unsigned(redMtl_), unsigned(smokeMtl_),
                    sparkShared_, redShared_, smokeShared_);
        return true;
    }

    // -----------------------------------------------------------------------
    // FOUR EMBERS WHERE A BLOW LANDED.
    //
    // `red` is the whole of what v1 changes when the thing struck is alive.
    // -----------------------------------------------------------------------
    void hitSparks(const Vec3 &at, double nowMs, bool red) {
        burst(at, nowMs, red, kSparkUpHit);
    }

    // ...and the one the rock clash throws, which lifts hardest.
    void toolSparks(const Vec3 &at, double nowMs) { burst(at, nowMs, false, kSparkUpClash); }

    // -----------------------------------------------------------------------
    // THE DEATH POOF: four sparks and sixteen smoke voxels, together.
    //
    // v1 aims it at the body centre (`cy = wy + 2` voxels) rather than at the
    // animal's feet, so the plume comes off the middle of the thing that died.
    // -----------------------------------------------------------------------
    void deathBurst(const Vec3 &at, double nowMs) {
        const Vec3 c(at.x, at.y + 0.2f, at.z);
        burst(c, nowMs, false, kSparkUpDeath);
        for (int i = 0; i < kParticleSmoke; ++i) {
            const int slot = freeSmoke();
            if (slot < 0) break;   // the column is one voxel shorter; nothing is deleted
            const float a = rnd() * 2.0f * PI;
            const float r = 0.05f + rnd() * 0.18f;
            P &q = p_[size_t(slot)];
            q.live = true;
            q.smoke = true;
            // STACKED AT BIRTH, which is what makes it a column rather than a
            // puff: v1's (i-4)*0.55 voxels.
            q.p0 = Vec3(c.x + cosf(a) * r, c.y + float(i) * 0.055f, c.z + sinf(a) * r);
            q.v = Vec3(cosf(a) * (0.1f + rnd() * 0.2f), 1.2f + rnd() * 0.6f,
                       sinf(a) * (0.1f + rnd() * 0.2f));
            q.born = nowMs;
            q.life = 1.0f + rnd() * 0.5f;
            q.ph = rnd() * 2.0f * PI;
            q.spin = 2.0f + rnd() * 2.5f;
        }
    }

    // Retire whatever has run out. Kept apart from publish() so the count is
    // true whether or not anything drew this frame.
    void update(double nowMs) {
        for (P &q : p_) {
            if (!q.live) continue;
            if (float((nowMs - q.born) * 0.001) > q.life) q.live = false;
        }
    }

    int live() const {
        int n = 0;
        for (const P &q : p_)
            if (q.live) ++n;
        return n;
    }

    // Where a particle is right now, for the headless tests. Ballistics only --
    // the same arithmetic publish() draws with.
    bool at(int i, Vec3 *out, bool *smoke, double nowMs) const {
        if (i < 0 || i >= kParticleSlots || !p_[size_t(i)].live) return false;
        const P &q = p_[size_t(i)];
        if (out) *out = pos(q, float((nowMs - q.born) * 0.001));
        if (smoke) *smoke = q.smoke;
        return true;
    }

    // -----------------------------------------------------------------------
    // ...AND ONTO THE BAND.
    //
    // The same shape drops.h publishes with: a spin about the vertical, and the
    // mesh's own corner backed out of the centre so the turn is about the
    // voxel's middle rather than about one of its corners. v1 does that
    // subtraction by hand and says why; here the matrix carries VOXEL_M, so the
    // half-box is in MODEL units -- see the note in Drops::publish, which is the
    // same trap.
    // -----------------------------------------------------------------------
    void publish(World &world, double nowMs) const {
        for (int i = 0; i < kParticleSlots; ++i) {
            const P &q = p_[size_t(i)];
            const int model = q.smoke ? smoke_ : (q.red ? red_ : spark_);
            if (!q.live || model < 0) {
                world.setFlyerInstance(kParticleSlot0 + i, -1, nullptr, 0.0f, 0.0f, 0.0f, nullptr,
                                       false);
                continue;
            }
            const float t = float((nowMs - q.born) * 0.001);
            const Vec3 w = pos(q, t);
            const float a = q.ph + t * (q.smoke ? q.spin : kSparkSpin);
            const float c = cosf(a), s = sinf(a), v = VOXEL_M;
            const float m[9] = {c * v, 0.0f, s * v, 0.0f, v, 0.0f, -s * v, 0.0f, c * v};
            // One voxel, so the centre is (0.5, 0.5, 0.5) in model units.
            const float ox = (m[0] + m[1] + m[2]) * 0.5f;
            const float oy = (m[3] + m[4] + m[5]) * 0.5f;
            const float oz = (m[6] + m[7] + m[8]) * 0.5f;
            // THE SPIN CHANNEL, NOT THE FLAP CHANNEL -- see setFlyerInstance,
            // where the two are made exclusive. A particle turns about the
            // vertical through its own position, which is exactly what that
            // channel is: an axis and an angle.
            const float spin[4] = {w.x, w.y, w.z, q.smoke ? q.spin : kSparkSpin};
            world.setFlyerInstance(kParticleSlot0 + i, model, m, w.x - ox, w.y - oy, w.z - oz,
                                   nullptr, true, spin);
        }
    }

  private:
    struct P {
        bool live = false;
        bool smoke = false;
        bool red = false;
        Vec3 p0{0.0f, 0.0f, 0.0f};
        Vec3 v{0.0f, 0.0f, 0.0f};
        double born = 0.0;
        float life = 0.0f;
        float ph = 0.0f;
        float spin = 0.0f;
    };

    // v1's own integration, written the way v1 writes it: a closed form off the
    // birth point rather than a step. A particle that lives 0.7 s never needs
    // its state advanced, and this way a missed frame cannot change its arc.
    static Vec3 pos(const P &q, float t) {
        const float g = q.smoke ? kSmokeG : kSparkG;
        Vec3 w(q.p0.x + q.v.x * t, q.p0.y + q.v.y * t - g * t * t, q.p0.z + q.v.z * t);
        if (q.smoke) {
            // THE WANDER, which is what makes sixteen voxels a plume rather
            // than a column of dots: v1's sin/cos drift, at its own rates.
            w.x += sinf(t * 1.6f + q.ph) * 0.14f;
            w.z += cosf(t * 1.9f + q.ph * 1.7f) * 0.14f;
        }
        return w;
    }

    // THE FOUR ARE STAMPED, NOT ALLOCATED. See the note at the top.
    void burst(const Vec3 &at, double nowMs, bool red, float up) {
        for (int i = 0; i < kParticleSparks; ++i) {
            const float a = rnd() * 2.0f * PI;
            const float sp = 1.2f + rnd() * 2.2f;
            P &q = p_[size_t(i)];
            q.live = true;
            q.smoke = false;
            q.red = red && red_ >= 0;
            q.p0 = at;
            q.v = Vec3(cosf(a) * sp, up + rnd() * 2.2f, sinf(a) * sp);
            q.born = nowMs;
            q.life = 0.4f + rnd() * 0.3f;
            q.ph = rnd() * 2.0f * PI;
            q.spin = kSparkSpin;
        }
    }

    int freeSmoke() {
        for (int i = kParticleSparks; i < kParticleSlots; ++i)
            if (!p_[size_t(i)].live) return i;
        return -1;
    }

    // -----------------------------------------------------------------------
    // ONE 1x1x1 MODEL IN A COLOUR NOTHING ELSE IS NEAR, AND THE ID IT GOT.
    //
    // THE LISTED CANDIDATES ARE NOT ENOUGH ON THEIR OWN, and the smoke is what
    // proved it: five near-whites and all five had neighbours, because a wood
    // full of birch bark, cloud and bone has a great many light greys in it.
    // Measured: material 142 with FOUR entries inside tolerance -- and alpha is
    // a property of the ENTRY, so setting it there would have made three other
    // things in the world see-through. The failure is silent and it is not
    // subtle to look at.
    //
    // SO THE SEARCH CARRIES ON PAST THE LIST. Tolerance is a Euclidean sphere
    // of radius kModelMatch in sRGB bytes (see Palette::resolveModelColor), so
    // this walks the authored colour's own value downward in steps wider than
    // that sphere and tries five tints at each -- neutral first, so a colour
    // that IS free is taken unchanged, and the tints only to get out of a
    // crowded grey. A particle is one voxel of one colour; being a shade off
    // what was authored costs nothing and being see-through does not.
    // -----------------------------------------------------------------------
    static int one(World &world, const uint8_t (*cand)[3], int n, uint8_t *mtl, int *shared) {
        const uint8_t *rgb = cand[n - 1];
        uint8_t found[3] = {rgb[0], rgb[1], rgb[2]};
        bool got = false;
        for (int k = 0; k < n && !got; ++k)
            if (freeColour(world, cand[k], found)) got = true;
        if (!got) {
            // ...AND THE SWEEP. 20 steps of 12 is the whole 8-bit range from
            // the authored value down, and 12 is under the 16-byte sphere so no
            // gap in the walk can hide a free colour.
            static const int kTint[5][3] = {
                {0, 0, 0}, {-14, -6, 10}, {10, -8, -12}, {-10, 12, -8}, {12, 8, -14}};
            for (int step = 0; step < 20 && !got; ++step)
                for (int t = 0; t < 5 && !got; ++t) {
                    const int v = int(cand[0][0]) - step * 12;
                    const int g = int(cand[0][1]) - step * 12;
                    const int b = int(cand[0][2]) - step * 12;
                    const uint8_t try9[3] = {clamp8(v + kTint[t][0]), clamp8(g + kTint[t][1]),
                                             clamp8(b + kTint[t][2])};
                    if (freeColour(world, try9, found)) got = true;
                }
        }
        rgb = found;
        VoxModel mo;
        mo.sx = mo.sy = mo.sz = 1;
        mo.m.assign(1, 1);
        mo.pal[0] = {rgb[0], rgb[1], rgb[2], 255};
        int sx = 0, sy = 0, sz = 0;
        const int id = world.addFlyerModel(mo, "particle", &sx, &sy, &sz);
        if (id < 0) return -1;
        // ASKED AFTERWARDS WHAT IT ACTUALLY GOT, which is the half of the
        // private-material recipe that the firefly's wing cost a session to
        // learn: registering a colour is not the same as wearing it.
        *mtl = world.palette.resolveModelColor({rgb[0], rgb[1], rgb[2], 255},
                                               Palette::kModelMatch, shared);
        return id;
    }

    static uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : (v > 255 ? 255 : v)); }

    // Is this colour's own neighbourhood empty? Answered WITHOUT registering
    // anything -- resolveModelColor is a question, forModelColor is the write.
    static bool freeColour(const World &world, const uint8_t *c, uint8_t *out) {
        int near9 = 0;
        world.palette.resolveModelColor({c[0], c[1], c[2], 255}, Palette::kModelMatch, &near9);
        if (near9 != 0) return false;
        out[0] = c[0];
        out[1] = c[1];
        out[2] = c[2];
        return true;
    }

    static float rnd() { return float(std::rand()) / float(RAND_MAX); }

    std::vector<P> p_ = std::vector<P>(size_t(kParticleSlots));
    int spark_ = -1, red_ = -1, smoke_ = -1;
    uint8_t sparkMtl_ = 0, redMtl_ = 0, smokeMtl_ = 0;
    int sparkShared_ = 0, redShared_ = 0, smokeShared_ = 0;
};

}  // namespace v2
