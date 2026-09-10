// ---------------------------------------------------------------------------
// toolsound.h -- what the axe, the pick and the bow sound like.
//
// The JS engine's tool and weapon audio, ported: the same files out of
// game/sound, the same levels, the same pool depths, the same shuffle bags and
// the same rule about which sound a blow makes. Its side of it is spread across
// ui/audio.js (the voices, playToolHit, playBlocked, the bow's four), tools.js
// (toolTakesFor, which decides whether a tool can take a material at all) and
// tick-camera.js and life/reactions.js (the moments they fire on). This is that
// gathered into one place, because in v2 there is only one caller.
//
// ---------------------------------------------------------------------------
// THE RULE, AND IT IS THAT ENGINE'S RULE
//
// A swing that lands makes one of three sounds:
//
//   THE MATERIAL'S OWN TAKE, when the tool in the hand is the one that takes
//   that material -- an axe in wood, a pick in stone. Five takes each, drawn
//   from a bag so a held swing never says the same thing twice running.
//
//   THE KNOCK (sound/block.mp4), when it is not. "play block.mp4 when a tool
//   cant break something ... when the pick trys to break wood ... apply this to
//   all the tools" is the instruction that put it there on 2026-08-28, and its
//   note is worth keeping: a bounce and a wrong tool mean opposite things to
//   the player, and a convincing thud on a material you will never break
//   implies progress that is not coming.
//
//   NOTHING AT ALL, for a whiff, for a mushroom cap, and for the loose ground
//   TAKEN BY THE TOOL THAT TAKES IT. That is deliberate over there and is left
//   deliberate here: "make the hits silent ... I'll fill it in later with other
//   sounds" (2026-08-26). Three material families were ever recorded, and
//   everything outside them waits.
//
//   THE GROUND MOVED BETWEEN THE SECOND CASE AND THE THIRD when the shovel
//   arrived, and it is worth being exact about which. Soil and grass used to
//   KNOCK for every tool, and that was right while nothing in the kit could
//   break them. Now a shovel can: for the shovel they are silent, in the third
//   sense above -- a take whose sound has not been recorded -- and for an axe
//   or a pick they still knock, in the second. Which of the two a blow gets is
//   decided by toolTakes (render/helditem.h) and nothing else.
//
// ---------------------------------------------------------------------------
// WHAT DID NOT COME ACROSS, AND WHY
//
// THE LEAF TAKE. sound/impact_sounds/leaf/00.mp4 is the third family and it has
// no trigger here. In that engine a swing carves voxels and can therefore land
// on a fern frond or a canopy leaf; in v2 a tree is one collider the width of
// its trunk (scene/collide.h) and the swing's whole vocabulary is ground, trunk
// and rock. There is nothing that could report foliage, so wiring the file
// would mean inventing a hit that cannot happen. It is one `load` and one arm
// on the chain in `blow` the day a foliage probe exists.
//
// THE BOUNCE DUCK. That engine plays the material take at BLOCK_DUCK when the
// RIGHT tool fails to bite -- out of reach, or the carve sphere caught nothing.
// v2 has nothing to carve, so a landed blow is a landed blow and there is no
// second, quieter outcome to distinguish. Ported as one level rather than as a
// constant with no user.
//
// THE GENERIC TAKE is already gone from that engine (sound/tool_hit/, deleted
// 2026-08-26) and so is not missing here.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"
#include "../scene/voxelworld.h"
#include "audio.h"
#include "helditem.h"

#include <string>

namespace v2 {

// ---------------------------------------------------------------------------
// The levels are the JS engine's own and are NOT round numbers on purpose.
//
// Every one comes out of the levelling pass it ran on 2026-08-28 ("just make
// all sfx the same level"), which is measured rather than tuned:
//
//     base = 10^((-34 - LUFS) / 20)
//
// with the loudness read off the SUMMARY block of ffmpeg's ebur128, K-weighted,
// after an aformat so a positive gain is not applied in s16 and clipped. -34 is
// the centre of the mix that existed, chosen so the whole thing barely moves
// and only the outliers are pulled in -- bow/impact was 13 dB above everything
// else and is the one big correction.
//
// ROCK IS A DELIBERATE DEVIATION from that: 0.4898 x 0.75, "lower the hitting
// the rock sound by 25%", read as amplitude like every percentage over there,
// so -2.5 dB. Kept as the levelled value times the trim rather than folded into
// one number, so a future re-level moves it with the pack and the trim survives.
// ---------------------------------------------------------------------------
inline constexpr float kSfxWoodBase = 0.5888f;    // impact_sounds/wood/0N.mp4
inline constexpr float kSfxRockBase = 0.3674f;    // impact_sounds/rock/0N.mp4, trimmed
inline constexpr float kSfxBlockBase = 0.0902f;   // block.mp4
inline constexpr float kSfxStretchBase = 0.4842f; // bow/stretch.mp4
inline constexpr float kSfxSwishBase = 0.2042f;   // bow/swish.mp4
inline constexpr float kSfxImpactBase = 0.1396f;  // bow/impact.mp4
inline constexpr float kSfxReloadBase = 0.45f;    // bow/reload.mp4 -- absent, see below
// pick_up.mp4, and 0.0631 is the JS engine's own measured level rather than a
// guess: "MEASURED with ffmpeg volumedetect, not guessed: base =
// 10^((-40 - meanRMS)/20) lands every effect at the same -40 dB effective
// level". Taking its number keeps this cue level with the rest of the bank.
inline constexpr float kSfxPickUpBase = 0.0631f;

// Five takes each, and two voices per take. BOTH numbers are that engine's and
// the second is the one that is easy to think optional: a held swing repeats
// every 570 ms against takes that run about 550, so one voice per take would
// have every strike cutting off the one before it.
inline constexpr int kSfxTakes = 5;
inline constexpr int kSfxTakeVoices = 2;
// ...and four for the arrow's thud, because shafts land in twos and threes.
inline constexpr int kSfxImpactVoices = 4;

// WHAT A BLOW TURNED OUT TO SOUND LIKE. Returned by blow() so --swing-log can
// print it beside what the swing hit: "trunk" and "wood" on one line is the
// whole rule, visible, and the one way to check a tool is wired to the right
// material without standing in the wood listening.
enum class Blow { Silent, Wood, Rock, Knock };

// ---------------------------------------------------------------------------
class ToolSounds {
  public:
    // Never fatal, like everything else in the audio path. A missing file is
    // one cue that stays silent; a missing device is all of them.
    bool open(vb::AudioDevice &dev, const std::string &dir) {
        if (!sfx_.open(dev)) return false;
        int loaded = 0;
        for (int i = 0; i < kSfxTakes; ++i) {
            char p[512];
            std::snprintf(p, sizeof(p), "%s/impact_sounds/wood/0%d.mp4", dir.c_str(), i);
            wood_[i] = sfx_.load(p, kSfxWoodBase, kSfxTakeVoices);
            std::snprintf(p, sizeof(p), "%s/impact_sounds/rock/0%d.mp4", dir.c_str(), i);
            rock_[i] = sfx_.load(p, kSfxRockBase, kSfxTakeVoices);
            if (wood_[i] >= 0) ++loaded;
            if (rock_[i] >= 0) ++loaded;
        }
        block_ = sfx_.load(dir + "/block.mp4", kSfxBlockBase, kSfxTakeVoices);
        stretch_ = sfx_.load(dir + "/bow/stretch.mp4", kSfxStretchBase, 1);
        swish_ = sfx_.load(dir + "/bow/swish.mp4", kSfxSwishBase, 1);
        impact_ = sfx_.load(dir + "/bow/impact.mp4", kSfxImpactBase, kSfxImpactVoices);
        // THE RE-NOCK IS EXPECTED TO BE MISSING. sound/bow/reload.mp4 was taken
        // out of that engine's tree on 2026-08-07 while the rest of the bow was
        // being wired, and its play() has been rejected-and-caught ever since.
        // Asked for anyway, for exactly the reason its note gives: drop the file
        // back in and the bow speaks again with no code change.
        reload_ = sfx_.load(dir + "/bow/reload.mp4", kSfxReloadBase, 1, /*optional=*/true);
        // ONE VOICE, and that engine says why: "only one grab flight is ever in
        // the air". The same rule holds here -- Drops arms one absorb at a time.
        pickUp_ = sfx_.load(dir + "/pick_up.mp4", kSfxPickUpBase, 1);
        if (block_ >= 0) ++loaded;
        if (stretch_ >= 0) ++loaded;
        if (swish_ >= 0) ++loaded;
        if (impact_ >= 0) ++loaded;
        if (reload_ >= 0) ++loaded;
        if (pickUp_ >= 0) ++loaded;
        std::printf("v2: tool sounds %d cues from %s%s\n", loaded, dir.c_str(),
                    reload_ < 0 ? "  (no bow/reload -- the re-nock is silent)" : "");
        std::fflush(stdout);
        return loaded > 0;
    }

    void setGain(float g) { sfx_.gain = g; }
    float gain() const { return sfx_.gain; }
    bool ready() const { return sfx_.ready(); }
    void close() { sfx_.close(); }

    // -----------------------------------------------------------------------
    // A BLOW LANDS. Called on the one frame the swing's impact arrives, 250 ms
    // in -- the same moment that engine registers a hit, and for its reason:
    // the sound belongs to the frame the tool is at the bottom of its arc, not
    // to the click that started it.
    // -----------------------------------------------------------------------
    Blow blow(Takes takes, const Swing &s) {
        if (!s.hit) return Blow::Silent;  // a whiff is silent over there too
        // A mushroom cap is one of the materials that engine never recorded,
        // and softness is the audio's own question -- see toolTakes, which
        // deliberately does not ask it.
        if (s.soft) return Blow::Silent;

        // WHAT THE SWING DECIDED, NOT A SECOND OPINION OF IT. This used to
        // re-derive "is that wood, is that stone" from the Swing, alongside the
        // copy App::onFrame kept for the bite -- two answers to one question,
        // held in step by hand. See toolTakes in helditem.h.
        if (toolTakes(takes, s)) {
            switch (takes) {
                case Takes::Wood:
                    sfx_.play(wood_[size_t(woodBag_.next())]);
                    return Blow::Wood;
                case Takes::Stone:
                    sfx_.play(rock_[size_t(rockBag_.next())]);
                    return Blow::Rock;
                // SOIL, AND ITS SILENCE IS NOT A WHIFF'S.
                //
                // The header records grass, soil and needle litter as the
                // families nobody ever recorded -- "make the hits silent ...
                // I'll fill it in later with other sounds" -- and until a tool
                // could take them the knock below was right for every blow that
                // landed on them, because nothing in the kit could break them.
                // A shovel can, so falling through would play the WRONG-TOOL
                // sound on the one tool that is right for the material, which
                // is precisely what the knock exists to be told apart from.
                //
                // The day sound/impact_sounds/soil/ exists this is one `load`
                // and one bag, exactly like the leaf take above.
                default:
                    return Blow::Silent;
            }
        }
        // The wrong tool, and the materials no tool in the kit takes -- rock to
        // an axe, a trunk to a shovel, bedrock to anything. That engine answers
        // all of them with the knock.
        sfx_.play(block_);
        return Blow::Knock;
    }

    // -- the bow, one voice per stage of the shot ---------------------------
    void draw() { sfx_.play(stretch_); }
    // Cut the moment the string is released, so a half-draw never rings on over
    // the loose. That engine's stopBowStretch, and its reason verbatim.
    void release() { sfx_.stop(stretch_); }
    // ...and the whoosh only when a shaft actually leaves. An empty release
    // that whooshed would be the one way this could lie about what happened.
    void loosed() { sfx_.play(swish_); }
    void nocked() { sfx_.play(reload_); }

    // -- THE SNATCH, NOT THE LANDING ---------------------------------------
    //
    // Fired when the item LEAVES THE GROUND, not when it reaches the hand, and
    // that is the JS engine's own correction rather than a choice made here:
    //
    //     if (lev) playPickUp();   // THE SNATCH, NOT THE LANDING (user
    //     2026-08-08) -- this fired from the two ARRIVAL branches at first,
    //     which put it a full GRAB_MS (measured: 365 ms) after the grab, and
    //     the pickup read as late. It belongs HERE: the item leaves the air on
    //     this frame, the flight is just it travelling to the hand.
    //
    // v2's flight is 360 ms, near enough the same, so playing it on arrival
    // would be late by the same third of a second.
    void pickedUp() { sfx_.play(pickUp_); }

    // -----------------------------------------------------------------------
    // WHERE IT LANDED, and how far off.
    //
    // There is no positional audio in either engine, so this is one honest
    // distance term rather than a pan: full close to, floored so a long shot
    // still reads. The numbers are that engine's, over ten -- it counts in
    // voxels, this counts in metres.
    // -----------------------------------------------------------------------
    void arrowLanded(float distM) {
        const float g = maxf(0.14f, minf(1.0f, 1.0f - (distM - 1.2f) / 19.0f));
        sfx_.play(impact_, g);
    }

  private:
    vb::Sfx sfx_;
    int wood_[kSfxTakes] = {-1, -1, -1, -1, -1};
    int rock_[kSfxTakes] = {-1, -1, -1, -1, -1};
    int block_ = -1, stretch_ = -1, swish_ = -1, impact_ = -1, reload_ = -1, pickUp_ = -1;
    // Seeded apart, or the two sets would walk the same permutation and a
    // chop-then-mine would repeat the same index in both.
    vb::SfxBag woodBag_{kSfxTakes, 0x51ED270Bu};
    vb::SfxBag rockBag_{kSfxTakes, 0x27D4EB2Fu};
};

}  // namespace v2
