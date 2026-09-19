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

#include "world/world.h"
#include "voxel/vox.h"

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
// WHITE (user 2026-09-15: "make the smoke voxels white, not blue"), which is
// v1's own authored colour and which this had moved off on the way through grey.
inline constexpr uint8_t kSmokeCand[][3] = {
    {236, 238, 240}, {228, 232, 236}, {242, 240, 232}, {224, 228, 230}, {246, 242, 248}};

// -- WHAT A SPARK EMITS, AND WHY IT IS NOT SIMPLY "BRIGHT" ------------------
//
// (user 2026-09-15: "the sparks seem white. make them a yellowish/orange. make
// sure they are emmissive as well".)
//
// THE FIRST CUT WAS 42 WORKING UNITS AND IT CAME OUT WHITE. Not nearly white --
// (255,255,255), measured. The reason is that acesFilmic is applied PER CHANNEL
// (see Tonemap.cs.slang): at 42 the red, the green AND the blue are all far
// past the curve's shoulder, so all three saturate to 1 and the hue the
// radiance carried is gone. Brightness and colour are not separate dials here;
// past the knee there is only brightness.
//
// v1 LEARNED THIS ON ITS METEOR and wrote it down: "9.0 was too hot to BE
// orange: aces() desaturates highlights, so the head clipped to near-white
// (measured ~255/250/195) and only the dim tail kept any hue -- the opposite of
// the ask. 3.1 sits under that knee, so the whole streak stays orange." Its
// meteor emits (1.00, 0.26, 0.030) x 3.1, which is a far more saturated ratio
// than a glance at the art would suggest -- and that is the point: the ratio
// has to be extreme BECAUSE the curve will flatten it.
//
// So these are chosen so the channels land on DIFFERENT parts of the curve --
// red at the shoulder, green halfway up, blue near the toe -- which is what
// makes a bright thing orange instead of a white thing.
//
// STILL AN EMITTER, and nothing about that changed: a voxel wearing this
// material returns this radiance, is not lit, is not shadowed, and its own
// albedo is never asked for. See V6Params::emitters.
//
// -- AND THE NUMBERS ARE MEASURED OFF THE SCREEN, NOT CHOSEN ---------------
//
// Rendered with --spark-emit and read back out of the .png, because the curve
// between what is emitted and what is seen is steep and not worth predicting:
//
//     emitted (R, G, B)      rendered sRGB      reads as
//     5.20  1.75  0.16       (243, 219, 108)    pale gold
//     3.20  0.62  0.04       (250, 207,  64)    amber
//     3.40  0.38  0.03       (251, 172,  52)    THIS -- yellow-orange
//     3.40  0.20  0.02       (251, 142,  44)    orange, nearly red
//
// The GREEN CHANNEL IS THE WHOLE DIAL and it is brutally compressed: dropping
// it from 1.75 to 0.62 -- nearly a third -- moved the rendered green by ten
// values, and the last stretch from 0.38 to 0.20 moved it thirty. Anything
// above about 0.8 is a yellow whatever else is done to it.
//
// FOR SCALE, v1's own spark art is sRGB (255, 208, 112). This is a shade
// deeper, which is what "yellowish/orange" asks for over its plain amber.
//
// AND BRIGHTER IS NOT MORE EMISSIVE HERE, which is worth knowing before
// reaching for the magnitude again. Measured as the lift in a ring of pixels
// just outside the spark, against the same frame with no burst in it:
//
//     3.4  ->  halo +8.8 / 765        10.0  ->  halo +8.1 / 765
//
// Flat, and if anything it falls. So there is nothing to buy by going hotter --
// only hue to lose. What makes these read as emissive is not the bloom, it is
// that they are EMITTERS: unlit, unshadowed, and the same colour at midnight
// under a canopy as at noon in the open, which no surface in this world is.
//
// -- AND THEN YELLOW, NOT ORANGE (user 2026-09-15) ------------------------
//
// The first calibrated value was (3.4, 0.38, 0.03) -> sRGB (251, 181, 55),
// which is what "yellowish/orange" was read as and came back as plain orange.
// Yellow is the same dial the other way -- green UP, blue held down -- and the
// sweep continues cleanly past the point the earlier table stops:
//
//     5.00  2.60  0.06       (251, 243,  75)    a clean yellow
//     5.50  3.60  0.08       (250, 245,  83)    the same, a shade paler
//     6.00  5.00  0.10       (246, 244,  85)    starting toward white
//
// G/R IS THE WHOLE STORY: 0.72 at the orange, 0.97 there.
//
// -- ...AND THEN HALF AGAIN LIGHTER (user 2026-09-15) --------------------
//
// Once red and green are both at the ceiling there is only one dial left, and
// it is BLUE: it is what a colour has left to give before it is white, so it is
// what "lighter" means for a yellow that is already this bright. Measured with
// the sparks ALONE in frame (--spark-only, below):
//
//     0.06   (250, 244, 104)    where this started
//     0.45   (254, 248, 192)    THIS -- blue half way to white, still yellow
//     1.00   (254, 250, 228)    nearly cream, the yellow going
//     2.20   (255, 252, 246)    white
//
// 104 to 192 is as near half way to 255 as the curve puts a step, which is the
// most literal reading of the ask that still leaves a yellow spark.
inline constexpr float kSparkEmit[3] = {5.0f, 2.60f, 0.45f};      // a hot ember
// ...AND THE RED ONE IS RED, which took the same lesson: at 0.34 of green it
// rendered as an ORANGE barely distinguishable from the ember beside it, which
// defeats the only thing this second material is for -- v1 throws red when the
// thing struck is alive. A tenth is what puts it the other side of orange.
inline constexpr float kSparkRedEmit[3] = {4.6f, 0.10f, 0.06f};   // ...and blood
// -- ...AND IT IS SEE-THROUGH THE WAY THE WATER IS -----------------------
//
// (user 2026-09-15: "make them transparent like the water. they look scratchy
// with alot of noise".)
//
// THE NOISE WAS THE ALPHA. V6Material::alpha is a stochastic pass-through --
// with probability 1 - alpha the ray carries straight on -- so at v1's 20% four
// pixels in five missed the smoke entirely and the plume came out as speckle.
// The number was right and the mechanism was wrong: v1 composites its
// translucency, and this engine samples it.
//
// -- ...AND IT IS OPAQUE, WHICH IS THE THIRD ANSWER TO THE SAME QUESTION ---
//
// (user 2026-09-15: "make the smoke voxels white, not blue".)
//
// THE THREE WAYS A VOXEL CAN BE SEE-THROUGH HERE, AND WHAT EACH COST:
//
//   alpha        stochastic -- a coin flip per pixel. SPECKLE, which was the
//                first complaint.
//   dielectric   deterministic and smooth, and what you see is WHAT IS BEHIND
//                IT. Measured mean (104,143,169) against a bright sky: blue,
//                because the sky is blue, exactly as the lake is. That was the
//                second complaint and it is not a bug in the material -- it is
//                what transparency means.
//   opaque       it is its own colour, always, and there is no noise because
//                there is no coin and nothing behind it to read.
//
// Smoke is white because it SCATTERS light, and a path tracer with no
// participating media has no way to say that: the nearest true statement is a
// surface that is white. So the plume is sixteen small white cubes now, and
// what it loses is being able to see the trees through it.
//
// 0 MEANS ORDINARY, so the dielectric machinery added for the last attempt is
// still there and still costs one compare -- see V6Material::ior. It is the
// right way to make something water-like and the smoke simply is not that.
inline constexpr float kSmokeIor = 0.0f;

// -- ...AND IT IS WHITE BECAUSE IT EMITS, NOT BECAUSE IT IS PAINTED WHITE ---
//
// (user 2026-09-15, twice: "make the smoke voxels white, not blue" / "just have
// solid white voxels".)
//
// A WHITE SURFACE IS NOT A WHITE THING. The material is (236,238,240) and fully
// opaque, and it still rendered at sRGB (127,134,150) -- measured -- because an
// opaque surface shows the light that falls on it, and the light falling on a
// voxel under an open sky is blue. Painting it whiter only makes it a brighter
// blue-grey; there is no albedo that is white in shade.
//
// So the smoke joins the sparks in the emitter table: a voxel wearing this
// material RETURNS this radiance and is not lit, not shadowed and not tinted by
// anything. That is what "solid white voxels" actually asks for, and it is the
// same mechanism v1 gets for free by compositing its smoke rather than shading
// it.
//
// LOW, AND FLAT. A third of the spark's, which is enough to read as white
// against a sunlit wood and far too little to bloom -- a puff of smoke must not
// look like a flare going off.
inline constexpr float kSmokeEmit[3] = {1.7f, 1.7f, 1.75f};

// -- HOW SIXTEEN SOLID CUBES SHARE ONE PLUME WITHOUT SHARING A SPACE --------
//
// (user 2026-09-15, twice: "the smoke voxels seems to have multiple voxels
// inside of eachother??? ... it still looks scratchy".)
//
// SPACING THEM AT BIRTH WAS NOT ENOUGH, and that is the whole of why the first
// attempt failed. The golden-angle spiral put the closest pair 0.274 m apart on
// the frame they were born and then let them MIX: each had its own rise speed
// (1.2 to 1.8 m/s), so a fast voxel caught the one 0.105 m above it in a fifth
// of a second, and each had a 0.14 m wander on a random phase, which is wider
// than a voxel. Simulated over their whole life, the closest approach was
// 0.067 m -- two 0.1 m cubes almost entirely inside each other.
//
// AND THEY SPIN, so the clearance is not 0.100 m but 0.173 -- a turning cube
// sweeps its diagonal. Coplanar faces inside that distance are what z-fights,
// and z-fighting is the "scratchy".
//
// So the motion is made DIVERGENT rather than random. Measured over four seeds,
// closest approach across the full 1.6 s:
//
//     as shipped (mixed rise, 0.14 wander)     0.061 m   inside each other
//     same rise for all                        0.113 m   still touching
//     ...plus a 0.35 m/s radial spread         0.169 m   marginal
//     ...plus the wander cut to 0.03 m         0.242 m   CLEAR
//
// All three are needed and this is all three. One rise speed keeps the column's
// vertical order exactly; a radius that grows with time makes the plume open
// out, which is also what smoke does; and the wander is now small enough to be
// texture rather than travel.
inline constexpr float kSmokeRise = 1.35f;     // m/s, THE SAME for every voxel
inline constexpr float kSmokeSpread = 0.35f;   // m/s outward -- the plume opens
//
// ...AND THERE IS NO WANDER, because v1 has none. Its smoke is p + v*t - g*t*t
// and nothing else -- checked in index.html's particle update, which is a
// straight line with a twirl on top. A sin/cos drift had been added here on the
// idea that it would keep sixteen voxels from reading as machinery; what it
// actually did was steer them into each other, since neighbours on a random
// phase converge about a third of the time and its amplitude was wider than a
// voxel. Cut to 0.03 m it measured no better than not having it at all (1.35x
// clearance against 1.36x), so it is gone: the spiral is what separates them
// and the spin is what keeps them from looking stamped.

// v1's ballistics, in metres. See the table at the top of this file.
// -- A DUCKLING'S TEAR -- v1's spawnTear ----------------------------------
//
// (user 2026-09-15: "the babies should cry".)
//
// v1's own arc, converted out of voxels: it wells sideways at 1.6 vox/s and up
// at 1.5-2.5, "a gentle well-up, then gravity takes it straight down the
// cheek", under the ordinary 85 vox/s^2 -- so the rise is about a centimetre
// and the whole of the effect is the fall. Life is FIXED at 0.7 s and v1 says
// why: "a tear that lasted a random 0.75-1.0 read as flickering".
inline constexpr float kTearOut = 0.16f;                    // v1's 1.6 vox/s
inline constexpr float kTearUpLo = 0.15f, kTearUpHi = 0.40f;   // 1.5 + rand*2.5
inline constexpr float kTearLife = 0.70f;                   // TEAR_LIFE, fixed
// PALE, NOT BLUE. A tear read against a lake has to be lighter than the water
// or it is a hole in the duckling's head; these are the candidates, lightest
// first, and one of them gets an entry nothing else wears -- see one().
inline constexpr uint8_t kTearCand[][3] = {
    {214, 234, 246}, {206, 230, 244}, {222, 238, 248}, {198, 226, 242}, {230, 242, 250}};
// ...AND IT IS THE FOURTH EMITTER. kEmitSlots is 4 and the spark, the red spark
// and the smoke are the other three, so this is the last one there is. It is
// spent here for the same reason the smoke spends one: a single voxel lit by
// path-traced GI at one sample a pixel is a speckle, and a tear is the smallest
// thing in the game. Low, because a tear catches light -- it does not cast it.
inline constexpr float kTearEmit[3] = {1.15f, 1.35f, 1.5f};

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
    // THE SPARK'S OWN ONE-VOXEL MODEL, lent to whatever else wants exactly it.
    // The rifle's rounds are drawn with this -- "have it share the same
    // properties as the spark voxel" -- so a tracer costs no model, no material
    // and none of the 255-entry palette. See render/bullets.h.
    int sparkModel() const { return spark_; }
    // WHAT THE EMBER ACTUALLY IS, for anything that wants to BE one -- see
    // Critters::load, where the firefly repaints itself onto this exact voxel.
    uint8_t sparkMtl() const { return sparkMtl_; }
    const uint8_t *sparkRgb() const { return sparkRgb_; }

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
        spark_ = one(world, kSparkCand, 5, &sparkMtl_, &sparkShared_, sparkRgb_);
        red_ = one(world, kSparkRedCand, 5, &redMtl_, &redShared_);
        smoke_ = one(world, kSmokeCand, 5, &smokeMtl_, &smokeShared_);
        // -- ...AND THE TEAR IS THE SMOKE'S OWN VOXEL ----------------------
        //
        // (user 2026-09-15: "the wheat is missing all of its voxel besides the
        // red one".)
        //
        // A PRIVATE ENTRY FOR THE TEAR COST THE WHEAT ITS COLOURS. The palette
        // is 255 entries and there is no 256th -- a voxel's material is a uint8
        // -- and it was sitting at 252 with everything else already folded to
        // make room. The tear took 253, the held kit ran the table out during
        // its load, and forModelColor started answering AIR: the stalk lost all
        // but one shade and drew as a single voxel. It is silent by
        // construction, and this is the fourth time the table has taken
        // something away like that (the buttons, the seeds, the wheat, and now
        // the wheat again).
        //
        // SO THE TEAR SHARES, AND v1 SAYS IT SHOULD. v1's tear is not its own
        // asset at all: spawnTear sets `foam: true` and draws the SAME white
        // voxel its splash droplets and its pollen grains use. A droplet off a
        // duckling's cheek and a fleck of spray off the lake are the same thing
        // over there, and the one white voxel this pool already owns is that
        // voxel. Nothing is lost but an entry nobody could spare.
        tear_ = smoke_;
        tearMtl_ = smokeMtl_;
        tearShared_ = smokeShared_;
        // -- AND THESE THREE IDS CARRY BEHAVIOUR, SO THEY ARE RESERVED ------
        //
        // (user 2026-09-18: "cant you give me seperate tables? one palete
        // table for the sandbox world and one for the arcade with the fps
        // maps".) The arcade has its OWN 255 entries now, allocated top-down
        // from 254 -- which is exactly where a late loader like this one sits
        // in the wood's table. That is fine for a COLOUR and fatal for a
        // MATERIAL THAT MEANS SOMETHING.
        //
        // WHAT IT LOOKED LIKE: `Trace.cs.slang` tests emissiveness by material
        // ID against V6Params::emitters, and that test is not table-aware --
        // it cannot be, because `h.mtl` is the raw id. So the first canyon map
        // in the arcade minted a sandstone shade onto the same id as the
        // EMBER RED, and a few thousand rock voxels across the cliff faces
        // glowed. Nothing was wrong with the asset -- measured, zero saturated
        // red voxels in the .vox -- and nothing was wrong with the palette;
        // the rock was simply being asked "are you an ember?" and answering
        // yes.
        //
        // v1's own warning, quoted in Palette::nearestFoliage, is the same
        // one: an id is a MATERIAL. Over there a pink bird landed 5/255 from
        // the cactus flower and stung the player.
        world.noteHeldMtl(sparkMtl_);
        world.noteHeldMtl(redMtl_);
        world.noteHeldMtl(smokeMtl_);
        if (spark_ < 0) {
            std::fprintf(stderr, "v2: the spark model would not load -- no sparks\n");
            return false;
        }
        // THE TWO EMBERS ARE LIGHTS AND THE SMOKE IS NOT. A translucent grey
        // cube that also glowed would be a lamp in a puff of steam.
        if (tracer) {
            tracer->addEmitter(sparkMtl_,
                               Vec3(kSparkEmit[0], kSparkEmit[1], kSparkEmit[2]));
            if (red_ >= 0)
                tracer->addEmitter(redMtl_,
                                   Vec3(kSparkRedEmit[0], kSparkRedEmit[1], kSparkRedEmit[2]));
            // ...AND THE SMOKE, for the reason over kSmokeEmit -- it is the
            // only way a voxel is the colour it was given rather than the
            // colour of the sky above it.
            if (smoke_ >= 0 && smokeShared_ == 1)
                tracer->addEmitter(smokeMtl_,
                                   Vec3(kSmokeEmit[0], kSmokeEmit[1], kSmokeEmit[2]));
            // NO EMITTER FOR THE TEAR. It wears the smoke's material now --
            // see the note in load() -- so the smoke's own entry above is
            // already its emitter, and asking for a second one on the same
            // material would simply overwrite the first with a dimmer value and
            // take the death plume down with it.
        }
        // ON AN ENTRY THAT IS OURS ALONE. Being a dielectric belongs to the
        // ENTRY, so setting it on a shared one turns whatever else wears it to
        // glass. Solid smoke is a worse-looking plume; a glass tree is a broken
        // world.
        // ...AND ONLY IF IT IS ASKED FOR. kSmokeIor is 0 now -- see the note
        // over it -- so this does nothing unless --smoke-ior puts it back,
        // which is how the two were compared in the first place.
        if (smokeIor_ > 1.0f && smoke_ >= 0 && smokeShared_ == 1)
            world.palette.setDielectric(smokeMtl_, smokeIor_);
        std::printf("  sparks   4 + %d smoke, materials %u/%u/%u "
                    "(%d/%d/%d within tolerance -- 1 is private)\n",
                    kParticleSmoke, unsigned(sparkMtl_), unsigned(redMtl_), unsigned(smokeMtl_),
                    sparkShared_, redShared_, smokeShared_);
        std::printf("  tears    %d slots, sharing the smoke's material %u -- no entry of its own\n",
                    kParticleTears, unsigned(tearMtl_));
        return true;
    }

    // The smoke's index of refraction, before load(). For --smoke-ior: the
    // same argument as --spark-emit, for the same reason -- what a translucent
    // grey looks like against a wood is not a thing to predict.
    void setSmokeIor(float ior) { smokeIor_ = ior; }

    // Re-point the ember's emission at run time. For --spark-emit: see the
    // note over kSparkEmit for why this is a sweep and not a decision.
    void setSparkEmit(Tracer &tracer, const Vec3 &radiance) {
        if (spark_ < 0) return;
        tracer.addEmitter(sparkMtl_, radiance);
        std::printf("  sparks   emission overridden to (%.2f, %.2f, %.2f)\n", radiance.x,
                    radiance.y, radiance.z);
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
    // ONE TEAR, AT A DUCKLING'S EYE -- v1's spawnTear.
    //
    // SKIPPED RATHER THAN STOLEN when the four are busy, which is the smoke's
    // rule and the splash's before it: "same rule as the splash: skip this tear
    // rather than cut a live one short".
    // -----------------------------------------------------------------------
    void tear(const Vec3 &at, double nowMs) {
        if (tear_ < 0) return;
        int slot = -1;
        for (int i = kParticleSparks + kParticleSmoke; i < kParticleSlots; ++i)
            if (!p_[size_t(i)].live) { slot = i; break; }
        if (slot < 0) return;
        const float a = rnd() * 2.0f * PI;
        P &q = p_[size_t(slot)];
        q = P{};
        q.live = true;
        q.tear = true;
        q.p0 = at;
        q.v = Vec3(cosf(a) * kTearOut, kTearUpLo + rnd() * (kTearUpHi - kTearUpLo),
                   sinf(a) * kTearOut);
        q.born = nowMs;
        q.life = kTearLife;
        q.ph = rnd() * 2.0f * PI;
        q.spin = kSparkSpin;
    }

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
            // -- NO TWO OF THEM IN THE SAME PLACE ----------------------
            //
            // (user 2026-09-15: "the smoke voxels seems to have multiple voxels
            // inside of eachother??? ... they should be smooth and opac".)
            //
            // v1 SPACES ITS SIXTEEN 0.55 OF A VOXEL APART and they overlap
            // there too -- but v1 draws them at 20% and sixteen translucent
            // cubes piled into each other read as one soft column. Ours are
            // SOLID now, so the same spacing puts opaque cube inside opaque
            // cube: their shared faces are coplanar, which is z-fighting, and
            // z-fighting is exactly the "scratchy" that was reported.
            //
            // A GOLDEN-ANGLE SPIRAL is what separates them, and it does the
            // whole job on its own: consecutive voxels are 137 degrees apart,
            // so no two of them ever share an arm and the closest pair starts
            // 22 cm apart HORIZONTALLY -- see the note at kSmokeRise for what
            // that has to beat.
            //
            // NO JITTER ON THE ANGLE. It was rnd()*0.30, and that is a third of
            // the gap between two arms -- enough for two of them to start
            // closer than they were laid out to. The spiral is exact now, and
            // what varies between voxels is the spin, which moves nothing.
            const float a = float(i) * 2.39996323f;
            const float r = 0.13f + 0.012f * float(i);
            P &q = p_[size_t(slot)];
            q.live = true;
            q.smoke = true;
            // ...and V1'S OWN STACKING, 0.55 of a voxel, which is what makes
            // it a column rather than a puff. It had been pushed out to 0.105
            // to hold the voxels apart vertically as well, and that was a
            // 1.57 m tower of smoke off a dead rabbit. With the spiral exact
            // the height is not carrying any of the separation -- measured, the
            // tightest pair still clears by 1.35x at this spacing -- so it goes
            // back to the shape v1 has.
            q.p0 = Vec3(c.x + cosf(a) * r, c.y + float(i) * 0.055f, c.z + sinf(a) * r);
            // OUTWARD AT ONE RATE AND UP AT ONE RATE -- see kSmokeRise. The
            // only thing that differs between two smoke voxels now is WHERE on
            // the spiral they sit, which is the one difference that cannot
            // bring them together.
            q.v = Vec3(cosf(a) * kSmokeSpread, kSmokeRise, sinf(a) * kSmokeSpread);
            q.born = nowMs;
            q.life = 1.0f + rnd() * 0.5f;
            // THE SPIN'S STARTING ANGLE IS THE INDEX rather than a roll: with
            // the drift gone this only orients the cube, and off the index no
            // two of them start in the same attitude.
            q.ph = float(i);
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
    void publish(World &world, double nowMs) {
        // -- HOW FAR EVERYTHING TURNED SINCE THE LAST FRAME THAT DREW --------
        //
        // See the spin channel below. This is a DELTA and it has to be measured
        // here, because publish() is the only thing that knows when the last
        // one was: a frame that took 33 ms turned twice as far as one that took
        // 16, and a particle that is drawn after a hitch turned further still.
        //
        // CLAMPED, because the first publish after a load has no previous frame
        // and the subtraction is the whole session; a quarter of a second is
        // already a bad hitch, and past that the reprojection is worthless
        // anyway and zero is the honest answer.
        const float dt = (lastPub_ > 0.0 && nowMs > lastPub_)
                             ? float((nowMs - lastPub_) * 0.001)
                             : 0.0f;
        lastPub_ = nowMs;
        const float turnDt = dt > 0.25f ? 0.0f : dt;
        for (int i = 0; i < kParticleSlots; ++i) {
            const P &q = p_[size_t(i)];
            const int model = q.smoke ? smoke_ : (q.tear ? tear_ : (q.red ? red_ : spark_));
            if (!q.live || model < 0) {
                world.setFlyerInstance(kParticleSlot0 + i, -1, nullptr, 0.0f, 0.0f, 0.0f, nullptr,
                                       false);
                continue;
            }
            const float t = float((nowMs - q.born) * 0.001);
            const Vec3 w = pos(q, t);
            const float a = q.ph + t * (q.smoke ? q.spin : kSparkSpin);
            // -- A PURE ROTATION, AND THE SCALE IS NOT THIS MATRIX'S JOB ----
            //
            // (user 2026-09-15: "make the sparks 10cm voxels, not very tiny".)
            //
            // THE SCALE WAS BEING APPLIED TWICE. A flyer is meshed AT VOXEL_M
            // -- see addFlyerModel, and the half-box setFlyerInstance passes
            // place(), which is in metres -- so the model's object space is
            // ALREADY metres and its transform is a turn and a translation and
            // nothing else. Every other publisher in this band knows that: the
            // bee's matrix is cos/sin with no scale in it at all.
            //
            // Multiplying by VOXEL_M here on top made every spark and every
            // smoke voxel a TENTH of its size -- one centimetre rather than
            // ten, which is a speck at arm's length and invisible at five
            // metres. Drops::publish DOES carry VOXEL_M, which is where this
            // came from; a held model is meshed at unit scale and a flyer is
            // not, and the two publishes are not interchangeable.
            const float c = cosf(a), s = sinf(a);
            const float m[9] = {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};
            // One voxel, so the centre is half a voxel out along each axis --
            // in METRES, because that is what the model's object space is.
            const float h = 0.5f * VOXEL_M;
            const float ox = (m[0] + m[1] + m[2]) * h;
            const float oy = (m[3] + m[4] + m[5]) * h;
            const float oz = (m[6] + m[7] + m[8]) * h;
            // -- THE SPIN CHANNEL CARRIES A DELTA, NOT A RATE --------------
            //
            // (user 2026-09-15, three times: "the smoke voxels still seem
            // noise/scratchy ... it still looks scratchy".)
            //
            // THIS IS THE SCRATCHY, AND IT WAS NEVER THE GEOMETRY. flapPad is
            // "radians turned SINCE THE LAST RENDERED FRAME" -- birds.h says so
            // over `dth`, and drops.h builds its own by subtraction -- and the
            // tracer rotates a hit point back by exactly that much to find out
            // where the surface was, which is the motion vector DLSS-RR
            // reprojects on. This passed the ANGULAR RATE: 2 to 4.5 radians a
            // second, where the truth at 60 fps is a fortieth of that. Every
            // pixel on a smoke voxel was reported to have swung most of a turn
            // in one frame, so RR went looking for its history most of a
            // rotation away, found leaves and sky, and blended them into a face
            // that should have been flat white.
            //
            // Rendered with --no-dlss the same burst has not a mark on it. That
            // is the proof it was never the lighting, the material or the
            // spacing: a cube that lies to the reprojector gets someone else's
            // pixels back, and no amount of separating them fixes that.
            const float rate = q.smoke ? q.spin : kSparkSpin;
            const float spin[4] = {w.x, w.y, w.z, rate * turnDt};
            world.setFlyerInstance(kParticleSlot0 + i, model, m, w.x - ox, w.y - oy, w.z - oz,
                                   nullptr, true, spin);
        }
    }

  private:
    struct P {
        bool live = false;
        bool smoke = false;
        bool red = false;
        bool tear = false;
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
        // A TEAR FALLS LIKE A SPARK, which is v1's own table: `grav = smoke ?
        // 1.5 : (petal ? 0 : 85)` and a tear is neither smoke nor petal.
        const float g = q.smoke ? kSmokeG : kSparkG;
        // A STRAIGHT LINE AND A TWIRL, which is the whole of v1's -- see the
        // note where kSmokeWob used to be.
        const Vec3 w(q.p0.x + q.v.x * t, q.p0.y + q.v.y * t - g * t * t,
                     q.p0.z + q.v.z * t);
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

    // THE SMOKE STOPS WHERE THE TEARS BEGIN. This used to run to
    // kParticleSlots, which was the end of the pool when the pool was sparks
    // and smoke; with a tear band on the end of it a death poof would have
    // eaten the tears, and the one death that matters here -- a mother duck's
    // -- is the one that needs both at once.
    int freeSmoke() {
        for (int i = kParticleSparks; i < kParticleSparks + kParticleSmoke; ++i)
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
    // `chosen`, when given, comes back with the RGB this actually took --
    // which is NOT any of the candidates in general (see the sweep below).
    // The firefly needs it to repaint itself onto the same voxel; see
    // Critters::load.
    static int one(World &world, const uint8_t (*cand)[3], int n, uint8_t *mtl, int *shared,
                   uint8_t *chosen = nullptr) {
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
        if (chosen) {
            chosen[0] = rgb[0];
            chosen[1] = rgb[1];
            chosen[2] = rgb[2];
        }
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
    int spark_ = -1, red_ = -1, smoke_ = -1, tear_ = -1;
    // When publish() last ran, so the spin channel can be a delta. See there.
    double lastPub_ = 0.0;
    uint8_t sparkMtl_ = 0, redMtl_ = 0, smokeMtl_ = 0, tearMtl_ = 0;
    uint8_t sparkRgb_[3] = {kSparkRgb[0], kSparkRgb[1], kSparkRgb[2]};
    int sparkShared_ = 0, redShared_ = 0, smokeShared_ = 0, tearShared_ = 0;
    float smokeIor_ = kSmokeIor;
};

}  // namespace v2
