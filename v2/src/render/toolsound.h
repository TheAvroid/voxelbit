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
//   NOTHING AT ALL, for a whiff, for soil and grass, and for a mushroom cap.
//   That is deliberate over there and is left deliberate here: "make the hits
//   silent ... I'll fill it in later with other sounds" (2026-08-26). Three
//   material families were ever recorded, and everything outside them waits.
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
        if (block_ >= 0) ++loaded;
        if (stretch_ >= 0) ++loaded;
        if (swish_ >= 0) ++loaded;
        if (impact_ >= 0) ++loaded;
        if (reload_ >= 0) ++loaded;
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
        // A mushroom cap is one of the materials that engine never recorded.
        if (s.soft) return Blow::Silent;

        const bool wood = (s.kind == Swing::Trunk);
        // A BOULDER AND A BARE HILLSIDE ARE THE SAME MATERIAL to a pick, which
        // is why the swing carries the ground's surface id: that engine's
        // pickOnlyTab is about stone, not about whether the stone is a model or
        // the terrain, and half the stone in this world is the terrain.
        const bool stone =
            (s.kind == Swing::Rock) || (s.kind == Swing::Ground && s.material == mat::ROCK);

        if (wood && takes == Takes::Wood) {
            sfx_.play(wood_[size_t(woodBag_.next())]);
            return Blow::Wood;
        }
        if (stone && takes == Takes::Stone) {
            sfx_.play(rock_[size_t(rockBag_.next())]);
            return Blow::Rock;
        }
        // Grass, soil and needle litter fall here as well as the wrong tool,
        // and that is right: neither an axe nor a pick can take them, and that
        // engine answers a tool that cannot break what it hit with the knock.
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
    int block_ = -1, stretch_ = -1, swish_ = -1, impact_ = -1, reload_ = -1;
    // Seeded apart, or the two sets would walk the same permutation and a
    // chop-then-mine would repeat the same index in both.
    vb::SfxBag woodBag_{kSfxTakes, 0x51ED270Bu};
    vb::SfxBag rockBag_{kSfxTakes, 0x27D4EB2Fu};
};

}  // namespace v2
