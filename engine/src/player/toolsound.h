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
//   break them. Now a shovel can: for the shovel they play the soil take
//   (impact_sounds/soil/, recorded 2026-09-24), the hoe plays the same take
//   but only when it tills or cuts wheat (see ToolSounds::soil), and for an
//   axe or a pick they still knock, in the second sense above. Which of the two a blow gets is
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

#include "core/vecmath.h"
#include "world/voxelworld.h"
#include "platform/audio.h"
#include "player/helditem.h"

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
// impact_sounds/soil/0N.mp4 -- THE SHOVEL IN DIRT (user 2026-09-24: "take the
// 2026-09-24 09-46-50 located in videos and make those shovel sounds the
// shovel sound when digging in the dirt ... isolate each sound bite").
//
// FIVE TAKES, ONE PER KEPT HIT IN THE CLIP. The clip is 19.6 s with seven clear
// strikes at 1.06, 3.78, 7.15, 8.79, 11.52, 14.43 and 17.07 s; each is cut
// from 5 ms before its onset to where it falls below -68 dB (175-250 ms), with
// a 2 ms fade in and a 40 ms fade out. The two softer taps at 1.95 and 15.2 s
// and the low scraping between strikes were left out. The raw strikes spread
// over 8 dB, so each was gained to the loudest one's mean first -- a bag that
// draws a quiet take at random reads as a missed blow. The 14.43 s strike was
// then dropped (user 2026-09-24: "theres one sound bite ... shorter then the
// rest, can you remove it") -- 175 ms against 185-245 -- and so, after the
// user heard all six in order, was 17.07 s ("remove 6"). The five kept are
// 1.06, 3.78, 7.15, 8.79 and 11.52 s as 00-04; they measure -33.0 LUFS, and
// 10^((-34 - -33.0)/20) = 0.8913.
// The source clip is kept at source/audio/shovel/ for a re-cut.
inline constexpr float kSfxSoilBase = 0.8913f;
inline constexpr float kSfxStretchBase = 0.4842f; // bow/stretch.mp4
inline constexpr float kSfxSwishBase = 0.2042f;   // bow/swish.mp4
inline constexpr float kSfxImpactBase = 0.1396f;  // bow/impact.mp4
inline constexpr float kSfxReloadBase = 0.45f;    // bow/reload.mp4 -- absent, see below
// pick_up.mp4, and 0.0631 is the JS engine's own measured level rather than a
// guess: "MEASURED with ffmpeg volumedetect, not guessed: base =
// 10^((-40 - meanRMS)/20) lands every effect at the same -40 dB effective
// level". Taking its number keeps this cue level with the rest of the bank.
inline constexpr float kSfxPickUpBase = 0.0631f;
// -- THE BITE -----------------------------------------------------------
//
// (user 2026-09-17: "play the eating sound when eating something.")
//
// 0.1084 IS THE BROWSER ENGINE'S OWN LEVELLED VALUE for this exact file, and
// it is measured rather than chosen: every effect there was put through
// ffmpeg volumedetect and given base = 10^((-40 - meanRMS)/20), so they all
// land at the same -40 dB. Picking a fresh number here would put the bite out
// of line with the kit it sits beside.
inline constexpr float kSfxEatBase = 0.1084f;
// -- THE SHOT -----------------------------------------------------------
//
// (user 2026-09-18: "take the bullet.mp4 file located in videos and make it
// the guns bullet sounds when firing.")
//
// 0.6310 IS THE LEVELLING PASS'S OWN FORMULA, not a taste: the cut measures
// -30.0 LUFS integrated, and 10^((-34 - -30.0)/20) = 0.6310. That is the same
// arithmetic that produced kSfxBlockBase and kSfxImpactBase EXACTLY, which is
// how it was checked -- measure block.mp4 (-13.1 LUFS) and bow/impact.mp4
// (-16.9) and the formula hands back 0.0902 and 0.1396, the two shipped
// numbers. A gunshot that is level with the rest of the bank is the point; it
// is loud because it is a gunshot, not because its cue was turned up.
//
// THE FILE IN THE BANK IS A CUT OF THE ONE THAT WAS HANDED OVER. Videos/
// bullet.mp4 is a 2.95 s screen capture with a video track, and the bang is
// 1.184 s to 1.502 s of it (ffmpeg silencedetect at -60 dB) -- so played whole
// the shot would arrive a second and a fifth AFTER the trigger. The decoder's
// own trim cannot save it: audio.h strips at most 100 ms of padding, by
// design. sound/gun/bullet.mp4 is `-ss 1.17 -to 1.58 -vn -ac 2 -ar 48000
// -c:a aac`, 410 ms, audio only, 11 KB. Re-cut it with those numbers if the
// original is ever re-recorded.
inline constexpr float kSfxBulletBase = 0.6310f;
// high_score.mp4 -- v1's achievement jingle. Its 0.09 is the number that
// engine arrived at over three cuts (0.5 -> 0.25 -> 0.15 -> 0.09, the last two
// at the user's asking), and it is carried over rather than re-picked: this
// plays over whatever the wood is doing and it was tuned to sit under it.
inline constexpr float kSfxDiscoveryBase = 0.09f;
// -- THE LIFE: A BLOW THAT WOUNDS, AND THE ONE THAT KILLS -----------------
//
// (user 2026-09-24: "take the sound Ui Retro 8 Bit Close Back Quit 13 ... and
// make that sound the sound when the player kills the life ... take the Ui
// Retro 8 Bit Close Back Quit 14 file and play that sound when the player hits
// the life.") Copied out of source/audio/8bit/ as life/kill.wav (13) and
// life/hit.wav (14).
//
// THE LEVELLING PASS'S FORMULA again, 10^((-34 - LUFS)/20): kill.wav measures
// -13.9 LUFS integrated and hit.wav -13.0, so 0.0989 and 0.0891.
inline constexpr float kSfxLifeKillBase = 0.0989f;
inline constexpr float kSfxLifeHitBase = 0.0891f;

// Five takes each, and two voices per take. BOTH numbers are that engine's and
// the second is the one that is easy to think optional: a held swing repeats
// every 570 ms against takes that run about 550, so one voice per take would
// have every strike cutting off the one before it.
inline constexpr int kSfxTakes = 5;
inline constexpr int kSfxSoilTakes = 5;   // see kSfxSoilBase
inline constexpr int kSfxTakeVoices = 2;
// ...and four for the arrow's thud, because shafts land in twos and threes.
inline constexpr int kSfxImpactVoices = 4;
// FOUR FOR THE GUN, AND THE ARITHMETIC IS THE WHOLE REASON: the rifle repeats
// every kBulletIntervalMs (300 ms) against a 410 ms shot, so a held trigger
// always has the tail of the last round still sounding. One voice would clip
// every shot in a burst to 300 ms and a burst would read as a stutter.
inline constexpr int kSfxGunVoices = 4;

// WHAT A BLOW TURNED OUT TO SOUND LIKE. Returned by blow() so --swing-log can
// print it beside what the swing hit: "trunk" and "wood" on one line is the
// whole rule, visible, and the one way to check a tool is wired to the right
// material without standing in the wood listening.
enum class Blow { Silent, Wood, Rock, Knock, Soil };

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
        for (int i = 0; i < kSfxSoilTakes; ++i) {
            char p[512];
            std::snprintf(p, sizeof(p), "%s/impact_sounds/soil/0%d.mp4", dir.c_str(), i);
            soil_[i] = sfx_.load(p, kSfxSoilBase, kSfxTakeVoices);
            if (soil_[i] >= 0) ++loaded;
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
        // ONE VOICE. A bite is 900 ms and there is one mouth; a second voice
        // could only ever overlap the first with itself.
        eat_ = sfx_.load(dir + "/eat.mp4", kSfxEatBase, 1);
        // ONE SHOT FOR BOTH GUNS -- "the guns bullet sounds", and the rifle and
        // the pistol are the guns. A second file is what a second gun would
        // need if one is ever recorded; nothing here is keyed to which is up.
        bullet_ = sfx_.load(dir + "/gun/bullet.mp4", kSfxBulletBase, kSfxGunVoices);
        // ONE VOICE. A discovery retitles the banner rather than queueing, so
        // a second one cuts the first off -- which is what one voice does.
        discovery_ = sfx_.load(dir + "/high_score.mp4", kSfxDiscoveryBase, 1);
        // TWO VOICES EACH: a held swing lands every 570 ms and arrows land in
        // twos and threes, so one voice would cut its own tail off.
        lifeHit_ = sfx_.load(dir + "/life/hit.wav", kSfxLifeHitBase, 2);
        lifeKill_ = sfx_.load(dir + "/life/kill.wav", kSfxLifeKillBase, 2);
        if (block_ >= 0) ++loaded;
        if (stretch_ >= 0) ++loaded;
        if (swish_ >= 0) ++loaded;
        if (impact_ >= 0) ++loaded;
        if (reload_ >= 0) ++loaded;
        if (pickUp_ >= 0) ++loaded;
        if (bullet_ >= 0) ++loaded;

        if (discovery_ >= 0) ++loaded;
        if (lifeHit_ >= 0) ++loaded;
        if (lifeKill_ >= 0) ++loaded;
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
    // -----------------------------------------------------------------------
    // THE WRONG-TOOL KNOCK, ASKED FOR DIRECTLY.
    //
    // blow() decides this from a Swing, which is right for everything the bite
    // chain resolves -- but wheat is not resolved there. A blade is not solid,
    // so a swing at a stand of it lands on the DIRT behind and comes back as
    // soil: the Swing has no idea the player was aiming at a plant, and blow()
    // would answer Silent for a shovel or Wood for an axe.
    //
    // So the caller that DOES know says so. See App::breakWheat, which is the
    // only one -- "make it where only the hoe can break the wheat. if any other
    // tool does it, play the antibreak sound."
    // -----------------------------------------------------------------------
    void knock() { sfx_.play(block_); }

    // THE SHOVEL'S DIRT TAKE, ASKED FOR DIRECTLY -- for the same reason as
    // knock() above. A hoe cutting wheat never reaches blow() as a harvest (the
    // Swing sees the dirt behind the blades), so App::breakWheat says so here
    // once the crop has actually been cut (user 2026-09-24: "use the same
    // shovel sounds on the hoe when it hits wheat"), and App::tillGround once
    // a column has turned ("also when the hoe tills dirt/grass"). A hoe blow on
    // the ground stays Silent in blow() so neither plays twice. One bag with the shovel,
    // so the two tools never repeat a take back to back between them.
    void soil() { sfx_.play(soil_[size_t(soilBag_.next())]); }

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
        // -- THE HOE ON EARTH IT CAN TURN IS NOT A WRONG TOOL -----------------
        //
        // (user 2026-09-24: "the hoe currently has two sounds when tilling the
        //  ground. remove the sound thats not the dirt sound. its the 8 bit
        //  sound that needs to go".)
        //
        // toolTakes has no arm for Takes::Earth -- the hoe's work is a TILL,
        // decided by App::tillGround after this, not a bite -- so every hoe
        // blow on the ground fell through to the knock below, and the till then
        // played the dirt take over it: block.mp4 and a shovel strike on every
        // swing. The ground the hoe can till is answered here, silently, and
        // the till (or the wheat it cuts) says the rest -- see soil(). Sand,
        // snow and rock still knock: the hoe really is the wrong tool there.
        if (takes == Takes::Earth && s.kind == Swing::Ground && isTillableMat(s.material))
            return Blow::Silent;
        if (toolTakes(takes, s)) {
            switch (takes) {
                case Takes::Wood:
                    sfx_.play(wood_[size_t(woodBag_.next())]);
                    return Blow::Wood;
                case Takes::Stone:
                    sfx_.play(rock_[size_t(rockBag_.next())]);
                    return Blow::Rock;
                case Takes::Soil:
                    soil();
                    return Blow::Soil;
                // THE HOE, AND ITS SILENCE IS NOT A WHIFF'S. (This was the
                // shovel's note too, until its dirt was recorded -- see
                // kSfxSoilBase.)
                //
                // The header records grass, soil and needle litter as the
                // families nobody ever recorded -- "make the hits silent ...
                // I'll fill it in later with other sounds" -- and until a tool
                // could take them the knock below was right for every blow that
                // landed on them, because nothing in the kit could break them.
                // A hoe can, so falling through would play the WRONG-TOOL
                // sound on the one tool that is right for the material, which
                // is precisely what the knock exists to be told apart from.
                //
                // The day the hoe has its own recording this is one `load`
                // and one bag, exactly like the soil take above.
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
    // A MOUTHFUL. Played when the bite STARTS rather than when it finishes:
    // the sound IS the eating, and a chew that arrives after the food has gone
    // is a sound effect for something that already happened.
    void eat() { sfx_.play(eat_); }
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

    // -- THE GUNS -----------------------------------------------------------
    //
    // (user 2026-09-18: "make it the guns bullet sounds when firing ... for the
    // reload sound use the pick up sound from the sandbox".)
    //
    // FIRED ON THE FRAME THE ROUND LEAVES THE BARREL, beside bullets_.launch
    // and held_.kick -- the same rule the arrow's whoosh follows, and for the
    // same reason: a gun that is dry, busy reloading or between repeats never
    // reaches that line, so this cannot report a shot that did not happen.
    void gunFired() { sfx_.play(bullet_); }

    // THE DISCOVERY JINGLE, under v1's banner. See App::unlockAch.
    void discovered() { sfx_.play(discovery_); }

    // A BLOW ON AN ANIMAL: the hit for one that wounds, the kill for the one
    // that finishes it -- never both on the same blow.
    void lifeHit() { sfx_.play(lifeHit_); }
    void lifeKilled() { sfx_.play(lifeKill_); }

    // -- ...AND A ROUND GOING IN IS THE SANDBOX'S OWN PICKUP ---------------
    //
    // THE SAME HANDLE, NOT A SECOND LOAD OF THE SAME FILE, which is a decision
    // about VOICES and not about memory. A revolver's six rounds arrive 270 ms
    // apart against a 933 ms sample: on one voice each round retriggers the
    // cue, which is a crisp tick per chamber, and on four they would ring over
    // each other into one long chime. One voice is also all the pickup ever
    // wanted -- "only one grab flight is ever in the air" -- so sharing it
    // costs that cue nothing. The two cannot collide in practice anyway: the
    // guns exist only in nuketown and nothing there levitates into the hand.
    //
    // ONCE PER ROUND, WHICH IS ONCE PER RELOAD FOR THE RIFLE, and that falls
    // out of where it is called rather than from asking which gun is up -- see
    // the reloadDone() poll in App::onFrameRender. A magazine change is one
    // turn of the strip and a cylinder is six. See [[v2-rifle-ammo-and-reload]].
    void gunLoaded() { sfx_.play(pickUp_); }

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
    int soil_[kSfxSoilTakes] = {-1, -1, -1, -1, -1};
    int block_ = -1, stretch_ = -1, swish_ = -1, impact_ = -1, reload_ = -1, pickUp_ = -1;
    int eat_ = -1, bullet_ = -1, discovery_ = -1;
    int lifeHit_ = -1, lifeKill_ = -1;
    // Seeded apart, or the two sets would walk the same permutation and a
    // chop-then-mine would repeat the same index in both.
    vb::SfxBag woodBag_{kSfxTakes, 0x51ED270Bu};
    vb::SfxBag rockBag_{kSfxTakes, 0x27D4EB2Fu};
    vb::SfxBag soilBag_{kSfxSoilTakes, 0x165667B1u};
};

}  // namespace v2
