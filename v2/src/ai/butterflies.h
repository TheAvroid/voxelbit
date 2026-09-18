// ---------------------------------------------------------------------------
// butterflies.h -- the flock that crosses the wood.
//
// Ported from the JS engine's flyers: the home grid and the leash in
// src/main/tick-nav.js (flyHome / findFlyHome / FLY_CELL / FLY_LEASH), the
// wander, the obstacle turn and the altitude servo in src/main/tick-creatures.js
// (the kind-0 branch), and the eight-frame flap and its six colours in
// src/assets/held-items.js. The numbers below are that engine's own, converted
// once: its world unit is the voxel and this one's is the metre, at ten voxels
// to it, so every length here is one tenth of the number it is named after.
//
// ---------------------------------------------------------------------------
// THE ONE BIG CHANGE: IT IS IN THE WORLD, NOT OVER IT
//
// The engine this comes from cannot put a butterfly in its scene. Its creatures
// are EMITTED -- a pose and a model id handed to the tracer, DDA-walked in
// camera space at the primary vertex, composited with a lighting model written
// for them (the ITEMN blocks in src/render/wgsl/trace.js and composite.js). A
// surface reached that way exists for exactly one ray: nothing bounces off it,
// nothing is shadowed by it, and no second ray can find it at all.
//
// Here a butterfly is a MODEL IN THE SCENE. Meshed by the same mesher as a
// pine, in a bottom-level structure like a pine's, indexing the same material
// table, placed by an instance transform in the same top-level structure. To
// the tracer it is terrain that happens to have moved since the last frame. So
// it casts a shadow on the needles under it, it picks up the green bounce off
// the canopy it is flying through, and a butterfly crossing a sunbeam is lit by
// the sunbeam -- none of which is a feature that had to be written, because all
// of it is what the wood already does to everything in it.
//
// This is the same move render/helditem.h made for the axe, and the same one
// render/arrows.h made for a shaft in the air; the difference is only that
// there are up to sixty-four of these and they never stop moving.
//
// ---------------------------------------------------------------------------
// ON THE GRID, AND OFF IT
//
// A voxel animation is a set of MODELS, not a rig: the eight flap frames are
// eight little grids, authored on the voxel lattice, and stepping the flap is
// choosing which of eight bottom-level structures the slot points at. That is
// the on-the-grid half, and it is why the wings stay crisp -- every frame is
// exactly the art, never a resampling of it.
//
// The TURN is not on the grid and must not be. Everything else placed in this
// world is rotated by a quarter turn, which is what keeps a voxel silhouette
// aligned to the world's own lattice; a butterfly quantised to four headings
// would fly in ninety-degree steps and skid between them. So its transform
// carries a free turn about Y, and its position is a continuous point rather
// than a lattice one. The tracer needs nothing for this: a free turn about Y is
// still orthonormal, which is the only property the note over the normal
// transform in Trace.cs.slang actually depends on.
//
// The model is meshed at VOXEL_M rather than at one unit per voxel (see
// World::addFlyerModel) so that transform is a turn and a translation and not a
// turn, a translation and a scale.
//
// ---------------------------------------------------------------------------
// THE HOME IS PROCEDURAL, THE BUTTERFLY IS NOT
//
// A butterfly wanders, so it cannot be pinned to a point in the world the way a
// tree is. What IS fixed is its HOME: one deterministic point per 12.8 m cell,
// about half the cells occupied. A slot takes the nearest unclaimed home, the
// insect is loosely leashed to it, and the slot is recycled when the HOME
// leaves range rather than when the insect does.
//
// That is the JS engine's design and its reasoning holds exactly: the meadow
// you walked through has the same butterflies in it when you come back, and
// how many there are is a property of the world rather than of when you
// happened to look.
//
// HOMES ARE TAKEN IN HASH ORDER, NOT NEAREST FIRST, and that engine's note is
// worth repeating because the bug is invisible until it is measured: handing
// the flock the N closest homes packs every one of them into the inner fifth of
// the disc's AREA, and since homes are never re-rolled, standing still freezes
// that fill in place. An arbitrary per-cell key picks uniformly from a
// candidate list that is itself uniform over the disc.
// ---------------------------------------------------------------------------
#pragma once

#include "core/noise.h"
#include "core/vecmath.h"
#include "world/world.h"
#include "player/collide.h"
#include "world/voxelworld.h"
#include "player/player.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace v2 {

// -- the flight, from the kind-0 branch of tick-creatures.js ----------------
// Every one of these is that engine's number over ten: it counts in voxels and
// this counts in metres. They are written as the converted value with the
// original named beside it, so a change there can still be found here.
inline constexpr float kFlyCellM = 12.8f;    // FLY_CELL 128 -- one home per cell
inline constexpr float kFlyInsetM = 1.6f;    // the 16 either side of it
inline constexpr float kFlyLeashM = 8.4f;    // FLY_LEASH 84 -- how far it may drift
inline constexpr float kFlySpeed = 5.6f;     // 56 vox/s
inline constexpr float kFlyThreatM = 3.0f;   // FLY_THREAT_R 30 -- a person this close
inline constexpr float kFlyFleeHold = 1.2f;  // FLY_FLEE_HOLD, seconds
inline constexpr float kFlyFleeMul = 2.0f;   // FLY_FLEE_MULT
inline constexpr float kFlyFloorM = 0.6f;    // gAir + 6, the hard floor under it
inline constexpr float kFlyGRefFallM = 0.9f; // 9 vox/s -- the ground memory's leak
inline constexpr float kFlyClimbM = 3.4f;    // 34 vox/s -- floors are approached, not snapped
inline constexpr float kFlyUpCapM = 3.0f;    // 30 vox/s
inline constexpr float kFlyDownCapM = 2.6f;  // 26 vox/s
// -- HOW FAR AHEAD IT LOOKS, AND WHY 1.3 WAS NOT FAR ENOUGH ----------------
//
// (user 2026-09-14: "they are ramming into trees endlessly.")
//
// 1.3 m is v1's la5, and at v1's speed it was most of a second of warning. Here
// it is 0.23 s: the butterfly flies at 5.6 m/s, decides at 15 Hz, and eases its
// turn in over about a tenth of a second on purpose -- so by the time the bank
// has any authority the trunk is already inside the look. It would refuse the
// step (see flySlide) and then press against the bark while its heading came
// round, which is exactly what "ramming into trees" looks like.
//
// A TIME, NOT A DISTANCE. What a flyer needs is a fixed number of SECONDS of
// warning, so a chase at 1.5x speed sees proportionally further -- see lookM().
// 0.55 s is a little over twice the old warning at cruise and about three turn
// time-constants, which is enough for the ease to finish rather than start.
inline constexpr float kFlyLookSec = 0.55f;
inline constexpr float kFlyLookMinM = 1.3f;  // la5 13 -- and still the floor
inline constexpr float kFlyLookM = 1.3f;     // the GATHER radius; see the update loop
// How far off its own centre a butterfly is probed -- see solidsTouch. Its
// wings are the widest part of it and they are about 40 cm across.
inline constexpr float kFlyBodyR = 0.2f;
// ...and how big a thing it goes OVER rather than round -- see solidsFloor.
// Its cruise is 2.0 m and its band is 2.4 deep, so four metres is a stone it
// would fly over and anything taller is scenery it goes round.
inline constexpr float kFlyRiseM = 4.0f;
inline constexpr float kFlyFadeSec = 0.7f;   // grow in over 0.7 s rather than pop
inline constexpr float kFlyFadeMin = 0.12f;  // ...from this much of full size

// ---------------------------------------------------------------------------
// HOW HIGH IT FLIES, AND THIS IS THE ONE PLACE THE PORT DEPARTS FROM v1.
//
// That engine flies every butterfly at gRef + 12 voxels and that is the whole
// of its altitude: one line, the same line for all of them, ankle to knee
// height. Ported straight it reads exactly as the user described it on
// 2026-09-07 -- "they seem to just fly at the same height" -- and it does,
// because they do. A flock on one contour is a flock that looks stamped.
//
// So the glide line becomes a BAND, and three numbers make it:
//
//   THE FLOOR OF THE BAND is higher than v1's line. At 1.2 m a butterfly is
//   down in the fern and the litter, behind everything; at 2.0 m it is at eye
//   height, which is where you actually see one.
//
//   ITS OWN SHARE OF THE BAND, drawn once per butterfly and never changed, so
//   no two of them cruise at the same height. This is what breaks the contour.
//
//   AND A SLOW RISE AND FALL on top of that, so a single one is not on a line
//   either. TWO swells rather than one, at rates that do not divide into each
//   other: one sine is a wave you can read the period of after ten seconds of
//   watching, and the sum of two is not.
//
// It stays SMOOTH for free, and that is why it is written as a moving target
// rather than as a write to y. The altitude servo below already eases toward
// the cruise line at 4/s under a rate cap; moving the line slowly is a change
// the servo simply follows. Worst case the two swells peak together at
// 0.60*2*pi/(5.5*0.75) + 0.21*2*pi/(2.3*0.75) = 1.7 m/s, against a 3.0 m/s cap
// -- so the target can never outrun the servo and nothing here becomes a step.
//
// The canopy still has the last word: the same roofed probe that stopped the
// old line easing up into the branches stops this one, and a pine's crown
// starts nine metres up, so a band topping out near five has room under it.
// ---------------------------------------------------------------------------
inline constexpr float kFlyCruiseM = 2.0f;    // the bottom of the band (v1: 1.2)
inline constexpr float kFlyLiftM = 2.4f;      // ...and how much of it is above that
inline constexpr float kFlyBobMinM = 0.25f;   // the swell, at its shallowest
inline constexpr float kFlyBobMaxM = 0.60f;   // ...and at its deepest
inline constexpr float kFlyBobSlowSec = 5.5f; // the long swell's period
inline constexpr float kFlyBobFastSec = 2.3f; // and the short one across it

// -- the flap ---------------------------------------------------------------
// Eight frames at the house rate of 24 fps, desynced per butterfly so a meadow
// of them does not beat as one wing.
inline constexpr int kFlyFrames = 8;
inline constexpr float kFlyFps = 24.0f;

// -- the flock --------------------------------------------------------------
// WHERE THE SLOTS ARE FILLED AND EMPTIED, which is not where the view ends. A
// butterfly is fifty centimetres of animal; past eighty metres it is a pixel,
// and the ring streams out past three hundred. So the flock lives in a disc
// around the player rather than out to the horizon, and the gap between the two
// radii is hysteresis -- without it a home sitting exactly on the line would be
// claimed and dropped on alternate frames.
inline constexpr float kFlyKeepM = 80.0f;
inline constexpr float kFlySpawnM = 76.0f;

// How often one butterfly LOOKS -- probes the ground under it, the trunks ahead
// of it and the leash on it. Its motion is integrated every frame; only the
// decisions run on this clock, and they are spread across frames by slot so the
// whole flock never thinks on the same one.
//
// Fifteen a second is the JS engine's own planning cadence to within a hair
// (NAV_HZ there is ~70 ms), and the reason to keep it slow is not this
// machine's frame budget but the same one that engine gives: a heading eased at
// 9/s cannot act on a decision faster than this anyway, and re-deciding inside
// the ease only adds jitter.
inline constexpr float kFlyThinkSec = 1.0f / 15.0f;

// ---------------------------------------------------------------------------
// AND A QUARTER OF THEM CHASE EACH OTHER, TWO AT A TIME.
//
// The same rule the songbirds got and for the same sentence -- see kSongPairM
// in birdflock.h for the argument. What differs is only what "same species"
// means: a butterfly's species is its COLOUR, so a yellow chases a yellow.
//
// THE LEASH STILL WINS. A chase sets the heading; the home leash overrides it
// and the wood overrides that. So a pair can spiral around each other for as
// long as they like and neither of them ends up in a trunk or eight cells from
// the flower it belongs to -- which is what would happen if a chase were
// allowed to be the outermost authority.
//
// THE DISTANCES ARE SHORT, BUT NOT SHORTER THAN THE FLOCK IS SPARSE. The first
// build used six metres, on the argument that a butterfly is half a metre of
// animal and should not be noticing things across a clearing. It measured ZERO
// chases in a flock of twenty-four, and the reason is arithmetic rather than
// behaviour: there is one home per kFlyCellM, which is 12.8 m, so two
// butterflies are almost never six metres apart -- and they have to be the same
// COLOUR as well, which is another one in six.
//
// Fifteen clears one home cell with room -- and measured TWO chases in
// twenty-four, which is 8% against the quarter that was asked for. Clearing the
// cell is not the bar. THE LATTICE IS ONLY A FIFTH OCCUPIED: the flock claims
// 24 homes out of the ~111 that fall inside the spawn radius, so a 15 m circle
// holds about five cells and only one of them has anybody in it -- and then the
// colour has to match, which is another one in six. About an 18% chance of
// having any eligible partner at all, and one pair is what that gives.
//
// THIRTY. Seventeen cells, three or four of them occupied, so roughly half of
// the flock can find a partner and the quarter-share is actually reachable.
// It is four seconds of flying to close, which is why the chase lasts longer
// below than the songbirds' does -- a chase that expires before it arrives is
// just two butterflies going the same way.
inline constexpr float kFlyPairM = 30.0f;
inline constexpr float kFlyPairDropM = 45.0f;
inline constexpr float kFlyChaseMin = 6.0f, kFlyChaseMax = 14.0f;
inline constexpr float kFlyChaseCool = 5.0f;
inline constexpr float kFlyChaseYaw = 4.2f;
// HOW HARD IT TURNS WHEN SOMETHING ACTUALLY STOPPED IT. Above the 5.0 the
// look-ahead uses, because that one is an anticipation and this one is a
// collision: at 5.6 m/s and 6.5 rad/s the turn radius is 0.86 m, which clears a
// birch trunk in a third of a turn.
inline constexpr float kFlyBlockedOm = 6.5f;
inline constexpr float kFlyChaseSpd = 1.22f;

// ---------------------------------------------------------------------------
// A YELLOW BUTTERFLY, BUILT RATHER THAN AUTHORED.
//
// Straight out of assets/held-items.js, including the reason. That engine has
// six authored colours and a rule the user set on 2026-08-22: pink belongs to
// the cherry blossom and nowhere else -- "make the pink butterfly a yellow one
// instead. remove the pink butterflies in the pine and oak forests." There is
// no yellow/ folder, so the yellow is the PINK SET with its saturated cells
// remapped and its four near-black body cells left alone: same geometry, same
// eight frames, same flap.
//
// v2 HAS NO CHERRY WOOD, so the rule is not a condition here -- it is simply
// what pink becomes. The pink models are never built at all.
//
// Keyed on SATURATION rather than on the two exact pinks, so a re-authored pink
// still yields a yellow: max - min > 24 is true of every wing cell in the file
// and false of every grey one. The cell's own lightness is carried through so
// the two wing shades stay distinct.
// ---------------------------------------------------------------------------
inline void yellowFromPink(VoxModel *mo) {
    for (auto &c : mo->pal) {
        const int mx = maxi(int(c[0]), maxi(int(c[1]), int(c[2])));
        const int mn = mini(int(c[0]), mini(int(c[1]), int(c[2])));
        if (mx - mn <= 24) continue;  // body grey -- untouched
        const float l = float(mx) / 255.0f;
        c[0] = uint8_t(250.0f * l + 0.5f);
        c[1] = uint8_t(214.0f * l + 0.5f);
        c[2] = uint8_t(86.0f * l + 0.5f);
    }
}

// ---------------------------------------------------------------------------
class Butterflies {
  public:
    // How many are wanted in the air. Slots past what the world can house
    // simply stay empty; see kFlyerInstances for the ceiling the structure has.
    // The app sets this from Options::butterflies at load (12, halved on
    // request 2026-09-14); this default is what a caller that never asks gets.
    int wanted = 12;
    // Where a butterfly may START being. See kBirthMinM in core/noise.h.
    BirthGate birth_;

    // -----------------------------------------------------------------------
    // Six colours of eight frames, and a colour is committed WHOLE.
    //
    // The no-orphan-half-sets rule is the JS engine's and the reason is its
    // own: the colours are addressed by index, so a set that loaded five frames
    // and gave up would shift every colour after it and hand the flock a flap
    // that reads off the end of its own strip. Every frame is read before
    // anything is built.
    // -----------------------------------------------------------------------
    bool init(World &world, const std::string &dir) {
        // The JS engine's folder order. Only the ORDER matters -- the colour is
        // picked by a hash and the distribution is uniform either way -- but
        // keeping it means a given cell wears the colour it wears there.
        static const char *kColours[] = {"orange", "red", "blue", "lime", "pink", "purple"};
        static const int kPink = 4;

        for (int c = 0; c < 6; ++c) {
            std::vector<VoxModel> frames;
            bool whole = true;
            for (int f = 0; f < kFlyFrames; ++f) {
                char path[600];
                std::snprintf(path, sizeof(path), "%s/%s/%02d.vox", dir.c_str(), kColours[c], f);
                VoxModel mo;
                std::string err;
                if (!voxLoad(path, &mo, &err)) {
                    std::fprintf(stderr, "v2: butterfly %s: %s -- colour skipped\n", path,
                                 err.c_str());
                    whole = false;
                    break;
                }
                if (c == kPink) yellowFromPink(&mo);
                frames.push_back(std::move(mo));
            }
            if (!whole) continue;

            std::vector<int> ids;
            for (int f = 0; f < kFlyFrames; ++f) {
                int sx = 0, sy = 0, sz = 0;
                const std::string what = dir + "/" + kColours[c];
                const int m = world.addFlyerModel(frames[size_t(f)], what, &sx, &sy, &sz, true);
                if (m < 0) break;
                // THE FRAMES SHARE A BOX or the body walks sideways as the
                // wings beat. They are authored that way -- every frame of
                // every colour is 5 x 6 x 4 -- and this is the check that says
                // so rather than the assumption that they are.
                if (sx_ == 0) {
                    sx_ = sx;
                    sy_ = sy;
                    sz_ = sz;
                } else if (sx != sx_ || sy != sy_ || sz != sz_) {
                    std::fprintf(stderr,
                                 "v2: butterfly %s frame %d is %dx%dx%d, not %dx%dx%d -- "
                                 "colour skipped\n",
                                 what.c_str(), f, sx, sy, sz, sx_, sy_, sz_);
                    break;
                }
                ids.push_back(m);
            }
            if (int(ids.size()) != kFlyFrames) continue;
            colours_.push_back(ids);
            wings_.push_back(measureFlap(frames));
        }

        if (colours_.empty()) {
            std::fprintf(stderr, "v2: no butterflies loaded from %s\n", dir.c_str());
            return false;
        }
        // A silent overflow is the failure this can actually have: the palette
        // is 235 model entries for the whole world, and past the end
        // forModelColor returns AIR -- which does not fail to load, it loads a
        // butterfly with holes in it.
        if (world.palette.overflowed() > 0)
            std::fprintf(stderr, "v2: palette full -- %d colours went unregistered\n",
                         world.palette.overflowed());
        std::printf("v2: butterflies %d colours x %d frames, %dx%dx%d voxels\n",
                    int(colours_.size()), kFlyFrames, sx_, sy_, sz_);
        std::fflush(stdout);
        // ITS OWN RUN OF THE BAND, not all of it -- the perched songbirds
        // own the rest. See kButterflySlots.
        flies_.assign(size_t(kButterflySlots), Fly{});
        return true;
    }

    bool ready() const { return !colours_.empty(); }

    // -----------------------------------------------------------------------
    // One tick of the flock.
    //
    // dt is the frame's own, and everything continuous -- the heading ease, the
    // travel, the altitude servo, the flap -- runs on it. The probes do not:
    // see kFlyThinkSec.
    // -----------------------------------------------------------------------
    void update(float dt, World &world, const Vec3 &player) {
        if (!ready()) return;
        t_ += double(dt);
        // A stalled frame -- a chunk build, a shader compile -- must not
        // teleport the flock across the wood. The same guard arrows.h puts on
        // its own integration, for the same reason.
        const float h = minf(dt, 0.25f);

        // BEFORE fill, which asks it. See kBirthMinM: the same floor the lake,
        // the rabbits and the perched songbirds are born outside.
        birth_.tick(dt, player.x, player.z);
        recycle(player);
        fill(world, player);
        pairUp();

        for (int i = 0; i < int(flies_.size()); ++i) {
            Fly &b = flies_[size_t(i)];
            if (!b.live) continue;
            // -- ONE GATHER, TWO READERS, AND IT MOVED UP HERE TO GET THAT ---
            //
            // This used to be the first line of decide(), which runs on the
            // slow clock -- so by the time fly() ran for this butterfly,
            // solids_ held the wood around whichever OTHER butterfly last
            // thought. That was harmless while only decide() read it and is
            // not harmless now that the flight itself has to be stopped by
            // something.
            //
            // IT IS THE GATHER THAT IS CHEAP AND THE PROBE THAT IS NOT. This
            // is a handful of chunk lookups and a distance test per solid;
            // insideWorld walks the list it produces, five or six times per
            // think. Widening the list to save gathers would have made the
            // probes cost ten times what the gathers ever did, which is the
            // trade the 2.8 m radius exists to keep.
            world.collidersNear(b.p, lookM(b) + 1.5f, &solids_);
            if (t_ >= b.think) {
                b.think = t_ + double(kFlyThinkSec);
                decide(world, player, b);
            }
            fly(h, b);
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
        for (const Fly &b : flies_)
            if (b.live) out->push_back({b.p, "butterfly", kFlyBodyR, false});
    }

    // How many are flying with somebody. The offline report prints it: a
    // proportion is not a thing you can check by looking at one frame.
    int chasing() const {
        int n = 0;
        for (const Fly &b : flies_) n += (b.live && b.chase >= 0) ? 2 : 0;
        return n;
    }

    // -----------------------------------------------------------------------
    // Every slot onto the pipeline, empty ones included -- the band is a fixed
    // size and a slot that has just been vacated has to be told it is empty.
    // -----------------------------------------------------------------------
    void publish(World &world) const {
        if (!ready()) return;
        const float cx = 0.5f * float(sx_) * VOXEL_M;
        const float cy = 0.5f * float(sy_) * VOXEL_M;
        const float cz = 0.5f * float(sz_) * VOXEL_M;

        for (int i = 0; i < int(flies_.size()); ++i) {
            const Fly &b = flies_[size_t(i)];
            if (!b.live) {
                world.setFlyerInstance(i, 0, nullptr, 0.0f, 0.0f, 0.0f, nullptr, false);
                continue;
            }
            // WHICH WAY IT FACES, and the half-turn in it is not a fudge.
            //
            // The model's head is at the LOW end of its own z -- the JS engine
            // says so where it builds this same frame ("model -y (the head end)
            // leads", tick-creatures.js), and that -y is this z after the
            // y-up conversion in scene/vox.h. So the axis that has to lie along
            // the heading is -z, which is the +z axis of a turn half a circle
            // further round.
            //
            // Taking the half turn rather than negating a column is what keeps
            // this a ROTATION. A matrix that maps +x to the right and +z to the
            // back has determinant -1: it is a mirror, it flips the winding of
            // every triangle in the flap, and the tracer's outward normals
            // would all point in. What the half turn costs instead is that the
            // butterfly's left wing is drawn where its right one was, on an
            // animal that is symmetric about that exact plane.
            const float ph = b.th + PI;
            const float c = cosf(ph), s = sinf(ph);
            // ...times the fade it materialises through. Uniform, so the 3x3
            // stays a rotation times a number and one normalise in the tracer
            // puts the normal back -- see KIND_FLYER in Shared.slang.
            const float k = fade(b);
            const float m[9] = {c * k, 0.0f, s * k, 0.0f, k, 0.0f, -s * k, 0.0f, c * k};

            // The mesh runs from its own corner and `p` is the middle of the
            // animal, so the translation is the centre less the model's own
            // half-box carried through the turn.
            const float ox = m[0] * cx + m[1] * cy + m[2] * cz;
            const float oy = m[3] * cx + m[4] * cy + m[5] * cz;
            const float oz = m[6] * cx + m[7] * cy + m[8] * cz;

            const int model = colours_[size_t(b.colour)][size_t(b.pose)];

            // -- AND HOW FAR THE WING ROSE INSIDE ALL THAT -------------------
            //
            // The instance's own motion is p - prev and covers the body. It
            // does not cover the WING, which stepped a voxel when the pose did,
            // and handing Ray Reconstruction the body's vector for a wing is
            // what smeared the whole flock -- see V6Instance::flap.
            //
            // A subtraction of two measured heights, so a pose that did not
            // change gives exactly zero and the flyer costs the tracer nothing
            // on the frames between steps. Times the fade, because the heights
            // are in the model's own metres and the instance is drawn at that
            // scale; and in world metres because the transform is a turn about
            // Y, which leaves a rise a rise.
            const WingBand &wb = wings_[size_t(b.colour)];
            const float flap[3] = {
                (wb.z[b.pose][1] - wb.z[b.poseWas][1]) * VOXEL_M * k,
                (wb.z[b.pose][2] - wb.z[b.poseWas][2]) * VOXEL_M * k,
                cx,  // the model's own centre, in object metres
            };
            // The FLIGHT is not passed: World::place differences the transform
            // and gets the same p - prev this used to hand over, from the one
            // place that cannot forget to. The FLAP is passed, because no
            // transform describes it -- the pose changed underneath one.
            // THE INSECT, NOT ITS BOX -- and here the two already agree: all
            // eight poses share one 5 x 6 x 4 box and the line above subtracts
            // exactly what place() adds back, fade included (it is a uniform
            // scale, so it divides out of both sides). Passed anyway, for the
            // reason the note in World::place gives: the perched songbirds are
            // what it looks like when that agreement quietly stops holding.
            const float anchor[3] = {b.p.x, b.p.y, b.p.z};
            world.setFlyerInstance(i, model, m, b.p.x - ox, b.p.y - oy, b.p.z - oz, flap, true,
                                   nullptr, anchor);
        }
        world.flushFlyerInstances();
    }

    // The one nearest a point, if there is one. Written for the offline
    // summary: a still of this wood either has a butterfly in it or it does
    // not, and that is worth one line of the log rather than a hunt through
    // the picture.
    bool nearest(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        const Fly *found = nullptr;
        for (const Fly &b : flies_) {
            if (!b.live) continue;
            const float d = lengthSq(b.p - p);
            if (d < best) {
                best = d;
                found = &b;
            }
        }
        if (!found) return false;
        if (at) *at = found->p;
        if (dist) *dist = sqrtf(best);
        return true;
    }

    // How high the flock actually ended up, above the ground under each of
    // them. Reported by the offline render: "they all fly at the same height"
    // is a thing you should be able to READ rather than squint at.
    bool band(float *lo, float *hi) const {
        float a = 1e30f, b = -1e30f;
        for (const Fly &f : flies_) {
            if (!f.live) continue;
            const float h = f.p.y - f.ground;
            a = minf(a, h);
            b = maxf(b, h);
        }
        if (a > b) return false;
        if (lo) *lo = a;
        if (hi) *hi = b;
        return true;
    }

    int flying() const {
        int n = 0;
        for (const Fly &b : flies_)
            if (b.live) ++n;
        return n;
    }
    int colourCount() const { return int(colours_.size()); }

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
        if (i < 0 || size_t(i) >= flies_.size() || !flies_[size_t(i)].live) return false;
        flies_[size_t(i)] = Fly{};
        return true;
    }

  private:
    // -- one insect ---------------------------------------------------------
    struct Fly {
        bool live = false;
        Vec3 p{0, 0, 0};
        float th = 0.0f;     // heading, radians, 0 = +z
        float om = 0.0f;     // turn rate now
        float omT = 0.0f;    // ...and what it is easing toward
        double tRe = 0.0;    // when the wander may pick a new one
        double think = 0.0;  // when it next probes the world
        float ground = 0.0f; // the terrain under it, from the last probe
        float gRef = 0.0f;   // ground MEMORY: up at once, down slowly
        // ITS OWN HEIGHT, and its own way of drifting through it -- see the
        // note over kFlyCruiseM. Drawn once, at the spawn, and never touched
        // again: a butterfly that re-rolled its altitude would jump.
        float lift = 0.0f;             // its share of the band above the floor
        float bobA = 0.0f, bobB = 0.0f;    // the two swells' amplitudes
        float bobRA = 0.0f, bobRB = 0.0f;  // ...their rates, radians a second
        float bobPA = 0.0f, bobPB = 0.0f;  // ...and where each one starts
        bool roofed = false; // something overhead -- do not ease up into it
        double fleeT = 0.0;  // fleeing until
        float hx = 0.0f, hz = 0.0f;  // its home
        int hcx = 0, hcz = 0;        // ...and the cell that home belongs to
        int colour = 0;
        // -- WHO IT IS AFTER, as an index into flies_, or -1. See kFlyPairM.
        int chase = -1;
        double chaseT = 0.0, pairAt = 0.0;
        float phase = 0.0f;  // frames of flap offset, so the flock is not in step
        // WHICH OF THE EIGHT IT IS WEARING, and which it wore last frame. This
        // is state and not a thing publish() can work out for itself, for the
        // same reason `prev` above is: what the motion vector needs is the
        // DIFFERENCE between two frames, and a const publish that recomputed
        // the pose from the clock would only ever have one of them. Held as a
        // pair here, stepped in fly(), exactly like the position.
        int pose = 0, poseWas = 0;
        float age = 0.0f;    // seconds since it appeared, for the fade
        float dying = -1.0f; // seconds into the fade OUT, or negative
        uint32_t rng = 1u;
    };

    bool isChased(int i) const {
        for (const Fly &b : flies_)
            if (b.live && b.chase == i) return true;
        return false;
    }

    // -----------------------------------------------------------------------
    // Break the finished chases, then start at most one. Identical in shape to
    // BirdFlock::pairUp -- deliberately, because they are answering the same
    // instruction and two different readings of "max 2 together" in one engine
    // would be a bug waiting for somebody to find it.
    // -----------------------------------------------------------------------
    void pairUp() {
        for (int i = 0; i < int(flies_.size()); ++i) {
            Fly &b = flies_[size_t(i)];
            if (b.chase < 0) continue;
            const Fly &o = flies_[size_t(b.chase)];
            const float dx = o.p.x - b.p.x, dz = o.p.z - b.p.z;
            const bool lost = !b.live || !o.live || o.colour != b.colour || o.chase >= 0 ||
                              dx * dx + dz * dz > kFlyPairDropM * kFlyPairDropM;
            if (lost || t_ > b.chaseT) {
                b.chase = -1;
                b.pairAt = t_ + double(kFlyChaseCool);
            }
        }

        int live = 0, paired = 0;
        for (const Fly &b : flies_) {
            live += b.live ? 1 : 0;
            paired += (b.live && b.chase >= 0) ? 2 : 0;
        }
        if (live < 2 || (paired + 2) * 4 > live) return;

        for (int i = 0; i < int(flies_.size()); ++i) {
            Fly &b = flies_[size_t(i)];
            if (!b.live || b.chase >= 0 || t_ < b.pairAt || isChased(i)) continue;
            int best = -1;
            float bestD2 = kFlyPairM * kFlyPairM;
            for (int j = 0; j < int(flies_.size()); ++j) {
                if (j == i) continue;
                const Fly &o = flies_[size_t(j)];
                if (!o.live || o.colour != b.colour || o.chase >= 0 || isChased(j)) continue;
                const float dx = o.p.x - b.p.x, dz = o.p.z - b.p.z;
                const float d2 = dx * dx + dz * dz;
                if (d2 >= bestD2) continue;
                bestD2 = d2;
                best = j;
            }
            if (best < 0) continue;
            b.chase = best;
            b.chaseT = t_ + double(kFlyChaseMin + rnd(&b.rng) * (kFlyChaseMax - kFlyChaseMin));
            return;
        }
    }

    // xorshift32, seeded off the home cell so a given home always flies the
    // same way. The JS engine reaches for Math.random here and its own comment
    // is that the result stopped being reproducible; this is that fixed.
    static float rnd(uint32_t *s) {
        uint32_t x = *s;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        *s = x ? x : 1u;
        return float(*s) * 2.3283064e-10f;
    }

    // -- the home grid, from tick-nav.js ------------------------------------
    // About half the cells hold one, at a deterministic point inset from the
    // cell's own edges.
    static bool homeOf(int cx, int cz, float *x, float *z) {
        if (ihash2(cx * 0x27D4 + 91, cz * 0x1656 + 37) >= 0.5f) return false;
        const float span = kFlyCellM - 2.0f * kFlyInsetM;
        *x = float(cx) * kFlyCellM + kFlyInsetM + ihash2(cx * 13 + 5, cz * 17 + 9) * span;
        *z = float(cz) * kFlyCellM + kFlyInsetM + ihash2(cx * 19 + 3, cz * 23 + 7) * span;
        return true;
    }

    // A cell's own scatter key -- see the note at the top on hash order.
    static float homeOrder(int cx, int cz) { return ihash2(cx * 421 + 17, cz * 419 + 31); }

    // WHICH OF THE COLOURS, by a spatial hash of the home cell. The JS engine's
    // exact mix, so adding a colour scatters it evenly with no other change.
    int colourOf(int cx, int cz) const {
        const uint32_t h = (uint32_t(cx) * 374761393u) ^ (uint32_t(cz) * 668265263u);
        return int(h % uint32_t(colours_.size()));
    }

    // -----------------------------------------------------------------------
    // HOW HIGH THE WING SITS IN EACH OF THE EIGHT GRIDS -- measured, not typed.
    //
    // This is the whole of what the motion vector needs to stop ghosting the
    // flock, and the reasoning is in the note over V6Instance::flap. What is
    // wanted per pose is the HEIGHT of the wing, so that the step between two
    // poses is a subtraction rather than a table somebody has to keep in step
    // with the art.
    //
    // BUCKETED BY HOW FAR OUT THE VOXEL IS, because that is the one thing the
    // height depends on. The flap is a roll about the body's long axis: the
    // column down the middle is the body and never moves, the voxels one out
    // are the inner wing, and the ones two out are the tips, which swing
    // furthest. Three buckets cover a model five voxels across, and anything
    // wider would join its outer voxels to the tips -- a blunter answer, never
    // a wrong-signed one.
    //
    // The mean is taken over whatever is in the bucket, so the body bucket
    // comes out the same number in all eight poses and subtracts to exactly
    // zero without being special-cased into doing so.
    //
    // Model coordinates, and the y-up conversion matters: MagicaVoxel is z-up
    // and toWorldWhole maps model z to the object's y, so the HEIGHT read here
    // is mo.at()'s third index and the SPAN is its first. Both without a flip,
    // which is why this can be measured on the file and used on the mesh.
    // -----------------------------------------------------------------------
    struct WingBand {
        // Mean height, in voxels, of each bucket in each pose.
        float z[kFlyFrames][3] = {};
    };

    static WingBand measureFlap(const std::vector<VoxModel> &frames) {
        WingBand w;
        for (int f = 0; f < kFlyFrames && f < int(frames.size()); ++f) {
            const VoxModel &mo = frames[size_t(f)];
            const float cx = 0.5f * float(mo.sx);  // the model's centre, in voxels
            double sum[3] = {0, 0, 0};
            int n[3] = {0, 0, 0};
            for (int z = 0; z < mo.sz; ++z)
                for (int y = 0; y < mo.sy; ++y)
                    for (int x = 0; x < mo.sx; ++x) {
                        if (!mo.at(x, y, z)) continue;
                        const float out = fabsf(float(x) + 0.5f - cx);
                        const int b = mini(2, int(out + 0.5f));
                        sum[b] += double(z) + 0.5;
                        ++n[b];
                    }
            for (int b = 0; b < 3; ++b)
                w.z[f][b] = n[b] ? float(sum[b] / double(n[b])) : 0.0f;
        }
        return w;
    }

    // Which of the eight it is wearing at this instant. The one place the flap
    // clock is read, so the pose publish() draws and the pose the motion vector
    // differences can never come from two different readings of it.
    int poseNow(const Fly &b) const {
        const int f = int(float(t_ * double(kFlyFps)) + b.phase) % kFlyFrames;
        return f < 0 ? 0 : f;
    }

    float fade(const Fly &b) const {
        const float in = sstep(saturate(b.age / kFlyFadeSec));
        const float out = b.dying >= 0.0f ? saturate(1.0f - b.dying / kFlyFadeSec) : 1.0f;
        return kFlyFadeMin + (1.0f - kFlyFadeMin) * in * out;
    }

    // -----------------------------------------------------------------------
    // A slot is given up when its HOME leaves range, never when the insect
    // does -- see the note at the top. It fades out on the way rather than
    // vanishing, which is the same 0.7 s the arrival uses.
    // -----------------------------------------------------------------------
    void recycle(const Vec3 &player) {
        for (Fly &b : flies_) {
            if (!b.live) continue;
            const float dx = b.hx - player.x, dz = b.hz - player.z;
            const bool gone = dx * dx + dz * dz > kFlyKeepM * kFlyKeepM;
            if (gone && b.dying < 0.0f) b.dying = 0.0f;
            // ...and back inside before the fade finished: it never left, so
            // let it fade back in rather than snapping to full size.
            if (!gone && b.dying >= 0.0f) b.dying = -1.0f;
            if (b.dying >= kFlyFadeSec) b.live = false;
        }
    }

    // -----------------------------------------------------------------------
    // Fill every empty slot from the unclaimed homes in range.
    //
    // The candidate list is gathered ONCE for the whole pass and popped from as
    // slots are filled, so two slots can never take the same home and the scan
    // is paid once rather than per slot. It is only gathered at all when
    // something is actually waiting, which on a still frame is nothing.
    // -----------------------------------------------------------------------
    void fill(World &world, const Vec3 &player) {
        int free = 0;
        const int want = mini(wanted, int(flies_.size()));
        int living = 0;
        for (const Fly &b : flies_)
            if (b.live) ++living;
        free = want - living;
        if (free <= 0) return;

        cand_.clear();
        const int r = int(kFlySpawnM / kFlyCellM) + 1;
        const int c0x = int(floorf(player.x / kFlyCellM)), c0z = int(floorf(player.z / kFlyCellM));
        TerrainMemo memo;
        for (int dz = -r; dz <= r; ++dz)
            for (int dx = -r; dx <= r; ++dx) {
                const int cx = c0x + dx, cz = c0z + dz;
                Home hm;
                hm.cx = cx;
                hm.cz = cz;
                if (!homeOf(cx, cz, &hm.x, &hm.z)) continue;
                const float ddx = hm.x - player.x, ddz = hm.z - player.z;
                const float dd2 = ddx * ddx + ddz * ddz;
                if (dd2 > kFlySpawnM * kFlySpawnM) continue;
                // ...AND NOT IN YOUR FACE. A home inside the floor is still a
                // real home; it is simply not one an insect may be born into
                // while you are stood at it. The keep radius is 80 m and this
                // is 30, so there is a band 46 m wide to be born in. See
                // kBirthMinM -- one number for every population in the engine.
                if (!birth_.may(dd2)) continue;
                bool taken = false;
                for (const Fly &b : flies_)
                    if (b.live && b.hcx == cx && b.hcz == cz) {
                        taken = true;
                        break;
                    }
                if (taken) continue;
                // ACROSS BOTH WOODS AND NEITHER LAKE. The bands are the only
                // thing that differs between the pine wood and the birch one
                // and a butterfly has no opinion about which it is over, so
                // there is no biome test here at all -- only the water, which
                // the JS engine also keeps them off ("a BUTTERFLY never starts
                // over water"; it may still drift out over one).
                hm.ground = world.terrain.heightM(hm.x, hm.z, memo);
                // waterSurfaceAt, not waterAt: the band line does not know
                // about a lake the imagery found, so this used to seat
                // butterflies over open water.
                if (hm.ground <= world.terrain.waterSurfaceAt(hm.x, hm.z) + 0.5f) continue;
                hm.ord = homeOrder(cx, cz);
                cand_.push_back(hm);
            }

        for (Fly &b : flies_) {
            if (free <= 0 || cand_.empty()) break;
            if (b.live) continue;
            // The smallest arbitrary key in the list, which picks uniformly
            // over the disc -- see the note at the top of the file.
            size_t k = 0;
            for (size_t q = 1; q < cand_.size(); ++q)
                if (cand_[q].ord < cand_[k].ord) k = q;
            const Home hm = cand_[k];
            cand_[k] = cand_.back();
            cand_.pop_back();

            // -- NOT INSIDE THE TREE STANDING ON IT ------------------------
            //
            // A home is a point on a 12.8 m lattice and the only thing ever
            // asked of it was whether it is over water. The butterfly is born
            // two to four metres up, which in a pine wood is trunk -- so a
            // handful of them started their lives inside one and flew out
            // under the escape clause in flySlide. Five frames of it per
            // minute, which is exactly the size of thing a check finds and an
            // eye does not.
            //
            // AT THE BIRTH POINT AND ONLY AT THE MOMENT OF BIRTH. fill()
            // returns before any of this when the population is full, so this
            // gather costs nothing on the frames where nothing is born, which
            // is nearly all of them. The list is gathered by COLUMN -- a
            // collidersNear is an XZ query -- so one call covers the whole
            // band this butterfly might be born anywhere in.
            world.collidersNear(Vec3(hm.x, hm.ground + kFlyCruiseM, hm.z), 3.0f, &solids_);

            b = Fly{};
            b.live = true;
            b.hx = hm.x;
            b.hz = hm.z;
            b.hcx = hm.cx;
            b.hcz = hm.cz;
            b.rng = hashU32(uint32_t(hm.cx * 2654435761u), uint32_t(hm.cz * 2246822519u)) | 1u;
            b.colour = colourOf(hm.cx, hm.cz);
            b.phase = rnd(&b.rng) * float(kFlyFrames);
            b.th = rnd(&b.rng) * TWO_PI;
            // Its height in the band, and the two swells it drifts through it
            // on. The rates are jittered by a quarter either way ON TOP of
            // being two: a flock that shares a period does not read as
            // individuals however deep the swell is.
            b.lift = rnd(&b.rng) * kFlyLiftM;
            const float amp = kFlyBobMinM + rnd(&b.rng) * (kFlyBobMaxM - kFlyBobMinM);
            b.bobA = amp;
            b.bobB = amp * 0.35f;
            b.bobRA = TWO_PI / (kFlyBobSlowSec * (0.75f + rnd(&b.rng) * 0.5f));
            b.bobRB = TWO_PI / (kFlyBobFastSec * (0.75f + rnd(&b.rng) * 0.5f));
            b.bobPA = rnd(&b.rng) * TWO_PI;
            b.bobPB = rnd(&b.rng) * TWO_PI;
            // ON ITS HOME AND AT THE GLIDE LINE, which is the JS engine's own
            // spawn height for a flyer (bfGlide + 14 voxels there; the ground
            // plus the cruise offset here, which is the same line the servo
            // below holds it on once it is flying).
            b.ground = hm.ground;
            b.gRef = hm.ground;
            b.p = Vec3(hm.x, hm.ground + kFlyCruiseM + b.lift, hm.z);
            // -- ...AND NOT INSIDE THE THING STANDING ON ITS HOME -----------
            //
            // TESTED AT THE HEIGHT IT IS ACTUALLY BORN AT, which is why this
            // sits here and not up beside the gather where it was written
            // first. `lift` is rolled eight lines above and is up to 2.4 m, so
            // a check at the bare cruise line is a check of a point the
            // butterfly is never at: it passed the clear air under a boulder's
            // dome and put the insect in the stone above it. Three frames a
            // minute, which is one butterfly born wrong and taking half a
            // second to climb out.
            //
            // THE SLOT IS GIVEN BACK RATHER THAN SPENT. `free` is not
            // decremented and the candidate has already been removed from the
            // list, so the next pass round this loop takes a different home --
            // the population fills a frame later instead of one short.
            if (solidsTouch(solids_.data(), int(solids_.size()), b.p.x, b.p.y, b.p.z, kFlyBodyR,
                            VOXEL_M)) {
                b = Fly{};
                continue;
            }
            // Materialises mid-flap wherever the clock happens to be, and with
            // no step behind it: its first frame has no history to describe.
            b.pose = b.poseWas = poseNow(b);
            // Staggered, so sixty-four butterflies never probe the world on
            // one frame: the first think lands somewhere inside the interval
            // rather than immediately.
            b.think = t_ + double(rnd(&b.rng) * kFlyThinkSec);
            --free;
        }
    }

    // -----------------------------------------------------------------------
    // WHAT IT LOOKS AT, on its own slow clock.
    //
    // The ground under it, the trunks in front of it, the leash on it and the
    // person near it. Everything here writes a TARGET; nothing here moves the
    // insect, which is what lets this run at a fifth of the frame rate without
    // the flight stepping.
    // -----------------------------------------------------------------------
    void decide(World &world, const Vec3 &player, Fly &b) {
        WalkWorld w;
        w.terrain = &world.terrain;
        w.solids = solids_.data();
        w.solidCount = int(solids_.size());

        // THE GROUND UNDER IT, AND A ROCK IS GROUND.
        //
        // terrain.heightM cannot see a model, so over a five-metre boulder this
        // answered the height of the dirt the boulder is standing in -- and
        // every one of the three altitude authorities below is written against
        // it. The butterfly then held a line two metres over that dirt, which
        // is three metres inside the stone, and its obstacle steering could not
        // help: turning away from something you are already inside only decides
        // which wall you leave through. Same fault, same one-line shape, in
        // four other species -- see Critters::flyFloor.
        b.ground = solidsFloor(solids_.data(), int(solids_.size()), b.p.x, b.p.z, VOXEL_M,
                               world.terrain.heightM(b.p.x, b.p.z), kFlyRiseM);
        // Never ease UP into the canopy. The JS engine probes twice here
        // because a gappy pine crown fooled one of them.
        b.roofed = insideWorld(w, Vec3(b.p.x, b.p.y + 0.3f, b.p.z)) ||
                   insideWorld(w, Vec3(b.p.x, b.p.y + 0.6f, b.p.z));

        // -- a person too close ---------------------------------------------
        const float px = b.p.x - player.x, pz = b.p.z - player.z, py = b.p.y - player.y;
        if (px * px + py * py + pz * pz < kFlyThreatM * kFlyThreatM)
            b.fleeT = t_ + double(kFlyFleeHold);

        // -- the wander ------------------------------------------------------
        if (t_ > b.tRe) {
            b.omT = (rnd(&b.rng) - 0.5f) * 4.0f;
            b.tRe = t_ + double(0.4f + rnd(&b.rng) * 0.8f);
        }

        // -- a chase, which overrides the wander ----------------------------
        //
        // ABOVE THE LEASH ON PURPOSE. The leash is what keeps a butterfly near
        // the flower it belongs to, and a chase that could beat it would tow
        // pairs across the wood until one of them was recycled for being out of
        // range -- which is a disappearing butterfly with extra steps.
        if (b.chase >= 0 && size_t(b.chase) < flies_.size()) {
            const Fly &o = flies_[size_t(b.chase)];
            const float ex = o.p.x - b.p.x, ez = o.p.z - b.p.z;
            if (ex * ex + ez * ez > 1e-4f)
                b.omT = clampf(angleTo(atan2f(ex, ez) - b.th) * 3.0f, -kFlyChaseYaw,
                               kFlyChaseYaw);
            b.tRe = t_ + 0.3;   // and the wander does not re-roll under it
        }

        // -- the leash, which overrides it ----------------------------------
        //
        // WHILE CHASING, THE PARTNER'S HOME IS THE HOME. Without this the two
        // rules are simply at war: the pair radius has to be wider than a home
        // cell for a pair to exist at all (see kFlyPairM), and the leash is
        // 8.4 m -- so every chase would be turned round before it got anywhere
        // and would read as a butterfly changing its mind twice a second.
        //
        // Borrowing the leash rather than suspending it is what keeps the
        // guarantee the leash exists for: the chaser is still tied to a home,
        // still cannot wander the wood, and is still inside the radius its slot
        // is recycled on -- which is measured from its OWN home and is
        // untouched by this.
        const float ahx = (b.chase >= 0 && size_t(b.chase) < flies_.size())
                              ? flies_[size_t(b.chase)].hx
                              : b.hx;
        const float ahz = (b.chase >= 0 && size_t(b.chase) < flies_.size())
                              ? flies_[size_t(b.chase)].hz
                              : b.hz;
        const float lx = b.p.x - ahx, lz = b.p.z - ahz;
        if (lx * lx + lz * lz > kFlyLeashM * kFlyLeashM) {
            const float homeTh = atan2f(-lx, -lz);
            b.omT = clampf(angleTo(homeTh - b.th) * 2.4f, -2.6f, 2.6f);
        }

        // -- and the wood, which overrides that -----------------------------
        //
        // A flyer sees an obstacle EARLY so the eased turn has room -- there is
        // no last-moment snap in this, and that is deliberate: a butterfly that
        // corners like a car reads as a machine.
        const float look = lookM(b);
        const float hx = sinf(b.th), hz = cosf(b.th);
        // -- PROBED ALONG THE LANE, NOT JUST AT THE END OF IT --------------
        //
        // Two points at 1.0 and 0.7 of the look left a hole in the middle: a
        // trunk at 0.4 of it was past the near probe and short of the far one,
        // so the butterfly saw nothing, flew on, and met it. Three evenly
        // spaced samples over the whole look close that -- and the thinnest
        // thing in this world is a 0.4 m birch trunk, so a third of a 3 m look
        // is still fine enough to catch one.
        const auto blocked = [&](float th, float f) {
            const Vec3 d = dirOf(th);
            for (int k = 1; k <= 3; ++k)
                if (insideWorld(w, b.p + d * (look * f * float(k) / 3.0f))) return true;
            return false;
        };
        const bool ahead = blocked(b.th, 1.0f) || insideWorld(w, b.p + Vec3(hx, 0.1f, hz) * 0.5f);
        if (ahead) {
            // THE WIDER FAN IS TRIED FIRST AND THE NARROW ONE SECOND. A
            // butterfly that only ever considers +-1 rad turns into the tree
            // NEXT to the one it is avoiding about as often as not, which reads
            // as bouncing down a row of trunks. Asking the open side at a full
            // right angle first gives it somewhere to actually go.
            const bool pWide = !blocked(b.th + 1.6f, 1.0f);
            const bool nWide = !blocked(b.th - 1.6f, 1.0f);
            const bool pFree = !blocked(b.th + 1.0f, 1.0f);
            const bool nFree = !blocked(b.th - 1.0f, 1.0f);
            if (pWide && !nWide) b.omT = 5.0f;
            else if (nWide && !pWide) b.omT = -5.0f;
            else if (pFree && !nFree) b.omT = 5.0f;
            else if (nFree && !pFree) b.omT = -5.0f;
            else if (!pFree && !nFree)
                // Cornered in a dense pocket of canopy -- swing back toward the
                // way it came, on whichever side is open at all.
                b.omT = !blocked(b.th + 2.4f, 0.4f) ? 6.4f : -6.4f;
            else
                b.omT = b.om >= 0.0f ? 5.0f : -5.0f;
            // AND IT DOES NOT DRIFT BACK ON THE NEXT WANDER ROLL. The wander
            // re-rolls omT every 0.4-1.2 s from pure noise, so a turn started
            // to miss a tree could be overwritten by a shrug before it had
            // finished -- and then the butterfly simply flew into the tree it
            // had begun to avoid. Holding the roll off for a third of a second
            // is long enough for the bank to have done its work.
            b.tRe = maxd(b.tRe, t_ + 0.35);
        }
    }

    // -----------------------------------------------------------------------
    // ...and what it DOES, every frame.
    // -----------------------------------------------------------------------
    void fly(float dt, Fly &b) {
        b.poseWas = b.pose;
        b.pose = poseNow(b);
        b.age += dt;
        if (b.dying >= 0.0f) b.dying += dt;

        // The heading is EASED into, never set: om chases omT and th integrates
        // om, so every decision above arrives as a bank rather than as a snap.
        b.om += (b.omT - b.om) * (1.0f - expf(-9.0f * dt));
        b.th += b.om * dt;

        const float spd = kFlySpeed * (t_ < b.fleeT ? kFlyFleeMul : 1.0f) *
                          (b.chase >= 0 ? kFlyChaseSpd : 1.0f);
        // -- ...AND THE STEERING IS NOT A GUARANTEE ------------------------
        //
        // The look-ahead above is the good part of this and it stays: seeing an
        // obstacle 1.3 m out and easing round it is what makes a butterfly read
        // as an animal rather than as a machine bouncing off a wall. But it
        // decides at 15 Hz and flies at the frame rate, it eases its turn on
        // purpose, and a chase overrides the leash -- so it CAN cut a corner,
        // and a rule that is usually obeyed is not the rule that was asked for.
        // The refusal below never fires on an open lane and costs a few voxel
        // reads when it does.
        const bool went =
            flySlide(solids_.data(), int(solids_.size()), VOXEL_M, kFlyBodyR, &b.p.x, &b.p.y,
                     &b.p.z, b.p.x + sinf(b.th) * spd * dt, b.p.y, b.p.z + cosf(b.th) * spd * dt);
        // -- A REFUSED STEP IS THE BEST INFORMATION THIS ANIMAL EVER GETS ---
        //
        // And it was being thrown away. The return value went unread, so a
        // butterfly with a trunk in front of it slid along the bark at full
        // speed, waited up to 67 ms for its next think, saw the obstacle with a
        // look that was too short, and began a turn it eased into over another
        // tenth of a second -- through all of which it was still pressed
        // against the tree. That is the whole of "ramming into trees
        // endlessly": not a steering failure, a steering system that was never
        // told the steering had failed.
        //
        // THREE THINGS AT ONCE, and all three are needed:
        //
        //   turn NOW      om is SET rather than eased toward omT. The ease is
        //                 what makes an ordinary course change read as a bank
        //                 and it stays for those; a wall is not a course
        //                 change.
        //   think NOW     the next decide() is pulled to this frame, so the
        //                 fan below picks a genuinely open heading instead of
        //                 the animal guessing for another 67 ms.
        //   pick a SIDE   and keep it. Alternating would rock it against the
        //                 trunk; the sign it already had is the side it was
        //                 leaning, and a stationary one gets the hash.
        if (!went) {
            const float side = (b.om != 0.0f) ? (b.om > 0.0f ? 1.0f : -1.0f)
                                              : ((rnd(&b.rng) < 0.5f) ? -1.0f : 1.0f);
            b.om = side * kFlyBlockedOm;
            b.omT = b.om;
            b.think = t_;   // decide() on the next tick of the loop, not in 67 ms
        }

        // -- the altitude, and it is three authorities in order --------------
        //
        // GROUND MEMORY first: it rises to the terrain at once and sinks back
        // slowly, so a butterfly crossing a gully stays at the height of the
        // rim it left instead of diving into it and climbing out.
        b.gRef = maxf(b.ground, b.gRef - kFlyGRefFallM * dt);
        // ...and the line it holds is this butterfly's own, drifting. Two
        // swells, summed -- see the note over kFlyCruiseM for why it is a
        // moving TARGET and not a write to y.
        const float bob = b.bobA * sinf(b.bobPA + float(t_) * b.bobRA) +
                          b.bobB * sinf(b.bobPB + float(t_) * b.bobRB);
        const float cruise = b.gRef + kFlyCruiseM + b.lift + bob;
        float step = (cruise - b.p.y) * (1.0f - expf(-4.0f * dt));
        step = clampf(step, -kFlyDownCapM * dt, kFlyUpCapM * dt);
        // The climb is refused by the canopy (roofed) and the descent by the
        // thing underneath it, which is what stops the servo easing a butterfly
        // down into the top of a rock it is legitimately flying over.
        if (!(step > 0.0f && b.roofed)) {
            // BOTH WAYS, and the first cut only guarded the descent on the
            // argument that `roofed` already stopped the climb. It does not
            // quite: roofed is decided on the slow clock and probes two fixed
            // points 30 and 60 cm up, so a branch that arrives between two
            // thinks is climbed into. The escape term is the same one flySlide
            // carries -- a butterfly that is already inside something must
            // always be allowed to move, or the guard becomes the trap.
            const float ny = b.p.y + step;
            if (!solidsTouch(solids_.data(), int(solids_.size()), b.p.x, ny, b.p.z, kFlyBodyR,
                             VOXEL_M) ||
                solidsContain(solids_.data(), int(solids_.size()), b.p.x, b.p.y, b.p.z, VOXEL_M))
                b.p.y = ny;
        }
        // Then the two floors, APPROACHED at climb speed and never snapped to:
        // the memory's, so it does not sink while the memory is still high, and
        // the local ground's, which is absolute.
        //
        // -- AND THESE TWO ARE STEPS, WHICH IS THE WHOLE POINT --------------
        //
        // They were the last unguarded writes to a butterfly's position, and
        // they were missed twice: once because they only ever RAISE the insect,
        // which sounds safe, and once more because the servo above them is the
        // line that looks like the altitude rule. Raising is not safe. `ground`
        // is sampled on the slow clock and both floors are absolute -- so a
        // butterfly that has drifted under a boulder's shoulder is lifted
        // toward a line above the stone, at 3.4 m/s, straight up through it.
        //
        // Three creature-frames a minute at 0.7 m in, at one rock in the pine
        // wood, in the same place in every run. Deterministic, invisible, and
        // the only reason it is written down here is that --clip-test kept
        // printing the same coordinate after two other theories had been tried
        // and were wrong.
        const float soft = b.gRef + kFlyFloorM + 0.1f;
        if (b.p.y < soft) b.p.y = riseTo(b, minf(soft, b.p.y + kFlyClimbM * dt));
        const float hard = b.ground + kFlyFloorM;
        if (b.p.y < hard) b.p.y = riseTo(b, minf(hard, b.p.y + kFlyClimbM * dt));
    }

    // A height this butterfly may be at, given where it is now: `want` if that
    // is clear, its own y if it is not, and `want` regardless if the insect is
    // already buried -- flySlide's escape clause, in the one place that cannot
    // call flySlide because it is moving on one axis only.
    float riseTo(const Fly &b, float want) const {
        if (!solidsTouch(solids_.data(), int(solids_.size()), b.p.x, want, b.p.z, kFlyBodyR,
                         VOXEL_M))
            return want;
        if (solidsContain(solids_.data(), int(solids_.size()), b.p.x, b.p.y, b.p.z, VOXEL_M))
            return want;
        return b.p.y;
    }

    // HOW FAR AHEAD IT LOOKS: a fixed number of SECONDS at whatever speed it is
    // actually doing. See kFlyLookSec -- a chase runs 1.5x and has to see 1.5x
    // as far, or the only time a butterfly is fast is the only time it is blind.
    float lookM(const Fly &b) const {
        const float spd = kFlySpeed * (t_ < b.fleeT ? kFlyFleeMul : 1.0f) *
                          (b.chase >= 0 ? kFlyChaseSpd : 1.0f);
        return maxf(kFlyLookMinM, spd * kFlyLookSec);
    }

    static double maxd(double a, double b) { return a > b ? a : b; }

    static Vec3 dirOf(float th) { return Vec3(sinf(th), 0.0f, cosf(th)); }
    // The shortest way round to an angle, in (-PI, PI].
    static float angleTo(float d) {
        while (d > PI) d -= TWO_PI;
        while (d < -PI) d += TWO_PI;
        return d;
    }

    struct Home {
        float x = 0.0f, z = 0.0f, ground = 0.0f, ord = 0.0f;
        int cx = 0, cz = 0;
    };

    // colours_[c][f] is the world's model id for frame f of colour c.
    std::vector<std::vector<int>> colours_;
    // One per colour, in step with colours_: how high its wing sits in each
    // pose. Pushed only for a colour that loaded WHOLE, so the two vectors
    // cannot drift apart -- see the no-orphan-half-sets rule at init().
    std::vector<WingBand> wings_;
    int sx_ = 0, sy_ = 0, sz_ = 0;  // the box every frame shares
    std::vector<Fly> flies_;
    std::vector<Home> cand_;
    std::vector<Solid> solids_;  // scratch, refilled per probe
    double t_ = 0.0;
};

}  // namespace v2
