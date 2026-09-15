// ---------------------------------------------------------------------------
// birds.h -- the songbirds perched in the wood.
//
// Ported from the JS engine's sim/life/stamped.js, which calls this population
// "the sole grid-stamped life": a bird that sits in a crown is not an animal
// that walks there, it is a thing the world places and then animates in one
// spot. That is the whole design over there and it is the whole design here.
//
// ---------------------------------------------------------------------------
// THE ELEVEN FRAMES ARE A QUARTER TURN, AND THAT IS THE ANIMATION.
//
// This is the part that is easy to get wrong, and that engine got it wrong
// twice before it got it right -- the notes it left are worth repeating,
// because both wrong answers look reasonable:
//
//   * It is NOT a spin bolted onto a perched bird. rotate/00..10 is a quarter
//     turn of the body, authored. Playing them is the rotation.
//   * At each cycle boundary the whole model is turned one further quarter, so
//     frame 00 re-seats at the orientation frame 10 finished in. Back to back
//     that is ONE CONTINUOUS ROTATION with no seam -- "thus creating an endless
//     rotation", in the user's own words over there.
//
// Taking the quarter turn for a once-a-second snap and replacing it with a
// fixed facing broke exactly the thing it was doing. So `q` below advances with
// the cycle and is not a facing.
//
// THE REST GOES ON EVERY FRAME, NOT AT THE BOUNDARY. A hold at the hand-off
// froze the bird on frame 10 before each cycle, which is what "the song birds
// are not playing correctly" was. Each pose is held frameMs + kRestMs equally,
// so the rotation steps rather than sweeps and never stalls. 24 fps + 25 ms
// makes a pose 1/15 s and a full revolution 2.93 s.
//
// ---------------------------------------------------------------------------
// ON THE OUTER EDGE, WHICH IS ASKED RATHER THAN ASSUMED.
//
// A perch is found by walking OUTWARD from the trunk along a ray until the
// crown stops answering -- solidColumnTop is false off the footprint or over an
// empty column -- and taking the last column that did. That is the silhouette
// of the tree, by construction, at whatever radius the crown happens to have at
// that height, and it needs no per-species number. A bird on the inside of a
// crown is a bird you cannot see.
//
// ---------------------------------------------------------------------------
// RENDERED LIKE EVERYTHING ELSE.
//
// Same flyer band as the butterflies, so a bird is an instance in the same
// acceleration structure as the terrain, lit by the same path trace, in one
// pass. See kBirdSlots in gpu/world.h for how the band is divided.
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../core/vecmath.h"
#include "../gpu/world.h"
#include "../scene/collide.h"
#include "../scene/voxelworld.h"

namespace v2 {

// The three that go in the pine and birch woods. The fourth the JS engine has,
// the pink bird, is deliberately not here -- it is being kept for the cherry
// forest (user 2026-09-07).
inline constexpr int kBirdSpecies = 3;
// rotate/00..10. Eleven, and they are a QUARTER turn -- see the header.
inline constexpr int kBirdFrames = 11;

// 24 fps as authored, plus a rest on every frame. Both numbers are the JS
// engine's, and the rest is the difference between a bird that steps round and
// one that sweeps like a turntable.
inline constexpr float kBirdFrameMs = 1000.0f / 24.0f;
inline constexpr float kBirdRestMs = 25.0f;
inline constexpr float kBirdStepMs = kBirdFrameMs + kBirdRestMs;

// -----------------------------------------------------------------------
// THREE RADII, AND ONE USED TO DO ALL THREE JOBS.
//
// Reported 2026-09-13: "the birds are disappearing when out of distance, then
// reappearing in a different spot ... the song birds are also flickering".
// Both symptoms are this one number, which was 42 m and meant, at once:
//
//   * how far out trees are GATHERED,
//   * how far a bird may be before it is given up,
//   * and therefore how far a new perch may be taken.
//
// A bird forty-two metres away is perfectly visible -- that is inside the
// nearest half-dozen trees -- so the recycle happened in plain sight. Worse,
// the gather runs twice a second while `stillPerched` was consulted every
// frame, so a tree that had merely dropped out of a STALE list read as a tree
// that had been felled, and the bird let go of a branch that was still there.
//
// Split, they can each be right:
//
//   PLACE   where a NEW perch may be taken. Near you, or the flock spreads
//           itself over the whole gather and the wood next to you is empty.
//   DROP    how far a bird may get before it is recycled. Far enough that it
//           is a couple of pixels when it happens.
//   GATHER  wider than DROP, so a live bird's own tree is ALWAYS in the list
//           and "is my branch still there" is a question we have the data to
//           answer. This is the one that was really broken.
// -----------------------------------------------------------------------
inline constexpr float kBirdPlaceM = 42.0f;
inline constexpr float kBirdDropM = 100.0f;
inline constexpr float kBirdKeepM = 115.0f;   // the GATHER -- see above
// ...and no two closer than this, or two models intersect on one branch.
inline constexpr float kBirdSepM = 1.2f;
// A tree has to be at least this tall to be worth perching in -- below it we
// are talking about a bush, and a bird standing in undergrowth does not read as
// perched. That engine refuses its own bush tier for the same reason.
inline constexpr float kBirdMinTreeM = 4.0f;
// The band of the tree a perch may be taken from, as a fraction of its height.
// Not the very top: a bird on the spire of a pine looks stuck on rather than
// sitting in it.
inline constexpr float kBirdLoFrac = 0.45f, kBirdHiFrac = 0.86f;

// -- WHICH FRAMES LEAVE THE BRANCH ------------------------------------------
//
// The JS engine's CARD_OFF, verbatim:
//
//     const CARD_OFF = { '04.vox': [0, 1, 0], '05.vox': [0, 1, 0] };
//     // baked per-frame alignment (user's copy-changes) -- same table the
//     // editor uses
//
// Two frames of the eleven sit one voxel higher; the other nine stand on the
// perch. That is the whole of the hop, and it is DATA rather than something to
// be inferred -- which matters, because it cannot be inferred: MagicaVoxel
// crops each frame to its own content, so the .vox files carry no shared origin
// and no nTRN offsets at all. I went looking for the lift in the art and it is
// not there; it is here, in a table someone typed while watching it play.
//
// buildBirdPoses applies it as `((p >> 16) & 255) + o[1]` -- model height plus
// the offset -- and takes only o[1], with its own note that "ox/oz are 0".
inline constexpr int kBirdLift[kBirdFrames] = {0, 0, 0, 0, 1, 1, 0, 0, 0, 0, 0};

// ---------------------------------------------------------------------------
class Birds {
  public:
    bool ready() const { return ready_; }
    int count() const {
        int n = 0;
        for (const Bird &b : birds_)
            if (b.live) ++n;
        return n;
    }

    // -----------------------------------------------------------------------
    // Three species, eleven frames each, all registered as flyer models.
    //
    // A species that fails to load is SKIPPED rather than fatal, exactly as a
    // butterfly colour is: two songbirds in the wood is a smaller loss than no
    // wood at all.
    // -----------------------------------------------------------------------
    bool init(World &world, const std::string &dir) {
        static const char *kNames[kBirdSpecies] = {"blue_bird", "robin", "cardinal"};

        for (int s = 0; s < kBirdSpecies; ++s) {
            std::vector<VoxModel> frames;
            bool whole = true;
            for (int f = 0; f < kBirdFrames; ++f) {
                char path[600];
                std::snprintf(path, sizeof(path), "%s/%s/rotate/%02d.vox", dir.c_str(), kNames[s],
                              f);
                VoxModel mo;
                std::string err;
                if (!voxLoad(path, &mo, &err)) {
                    std::fprintf(stderr, "v2: bird %s: %s -- species skipped\n", path, err.c_str());
                    whole = false;
                    break;
                }
                frames.push_back(std::move(mo));
            }
            if (!whole) continue;

            Species sp;
            for (int f = 0; f < kBirdFrames; ++f) {
                int sx = 0, sy = 0, sz = 0;
                const int m = world.addFlyerModel(frames[size_t(f)], std::string(kNames[s]), &sx,
                                                  &sy, &sz, true);
                if (m < 0) { sp.model[0] = -1; break; }
                sp.model[f] = m;
                // -- THE FRAMES DO NOT SHARE A BOX, AND CANNOT ----------------
                //
                // This took the MAX of the three dimensions across the strip,
                // on the assumption the butterflies' rule held here too. It
                // does not, and the reason is the thing that makes these frames
                // work at all: they ARE a quarter turn, so the bird's footprint
                // TURNS WITH THEM. Measured on all three species --
                //
                //     frame 00   3 x 7 x 6
                //     frame 05   6 x 7 x 3
                //     frame 10   6 x 7 x 3
                //
                // -- x and z swap through the cycle, and the ink fills its box
                // exactly in every one. A butterfly's frames genuinely are one
                // box (5 x 6 x 4 throughout) because a wing beat is not a turn.
                //
                // Taking the max gave every frame a 6 x 7 x 6 half-box, so the
                // centring offset was 1.5 voxels wrong in x for the narrow
                // frames and 1.5 wrong in z for the others -- and in DIFFERENT
                // directions, so the bird shuffled 15 cm around its perch as it
                // played instead of turning on the spot. Each frame carries its
                // own box now and is centred on it.
                sp.bx[f] = sx;
                sp.by[f] = sy;
                sp.bz[f] = sz;
            }
            if (sp.model[0] < 0) continue;
            sp.ok = true;
            species_[s] = sp;
            ++loaded_;
            std::printf("  bird     %-10s %dx%dx%d..%dx%dx%d voxels, models %d..%d\n", kNames[s],
                        sp.bx[0], sp.by[0], sp.bz[0], sp.bx[kBirdFrames - 1],
                        sp.by[kBirdFrames - 1], sp.bz[kBirdFrames - 1], sp.model[0],
                        sp.model[kBirdFrames - 1]);
        }

        birds_.assign(size_t(kBirdSlots), Bird{});
        ready_ = loaded_ > 0;
        if (ready_)
            std::printf("  birds    %d species x %d frames, %d perches\n", loaded_, kBirdFrames,
                        kBirdSlots);
        return ready_;
    }

    // -----------------------------------------------------------------------
    // Retire what is out of range, fill what is empty, and step the clock.
    //
    // `trees` is whatever the caller had to gather anyway for collision -- this
    // deliberately does not reach for the world itself, so the perch search
    // reads the same solids the player is walking into and the two cannot
    // disagree about which trees exist.
    // -----------------------------------------------------------------------
    void update(float dt, const std::vector<Solid> &trees, const Vec3 &player) {
        if (!ready_) return;
        nowMs_ += double(dt) * 1000.0;
        // BEFORE any perch is taken -- see kBirthMinM.
        birth_.tick(dt, player.x, player.z);

        for (size_t i = 0; i < birds_.size(); ++i) {
            Bird &b = birds_[i];
            // -------------------------------------------------------------
            // IT LETS GO ONLY ONCE IT HAS SOMEWHERE TO GO.
            //
            // This used to clear `live` and then call tryPerch, which gets six
            // attempts and is allowed to fail -- so on any frame where all six
            // missed, the bird was simply NOT DRAWN. Walking past a thin patch
            // of wood that failed intermittently is the reported flicker, and
            // it is a one-frame hole rather than anything DLSS did.
            //
            // Now the replacement is found FIRST, into a copy, and the bird is
            // moved only if it succeeded. A bird that wants to move and cannot
            // stays exactly where it is, which is always a better picture than
            // a gap.
            // -------------------------------------------------------------
            const float dx = b.p.x - player.x, dz = b.p.z - player.z;
            const float d2 = dx * dx + dz * dz;
            const bool tooFar = b.live && d2 > kBirdDropM * kBirdDropM;
            // ...AND THE BRANCH HAS TO STILL BE THERE.
            //
            // A perch is a position, taken once and then held. When the tree
            // under it is felled the position does not stop being valid on its
            // own, so the bird sits in the air where the crown used to be. This
            // asks the same question tryPerch asked -- is there a solid column
            // here, at about this height -- of whatever trees exist NOW, and
            // lets go if the answer has changed. A bird that lets go is not
            // deleted: the loop below re-perches it on the next tree it can
            // find, which is what a startled bird does anyway.
            // ...AND THE BRANCH TEST ONLY RUNS WHERE WE HAVE THE DATA.
            //
            // `trees` is gathered to kBirdKeepM and refreshed twice a second,
            // so a bird beyond that radius -- or one that has only just
            // crossed into it -- is ABSENT from the list for reasons that have
            // nothing to do with its tree. Judging it there is how a perfectly
            // good perch was condemned. The drop radius is well inside the
            // gather precisely so this question is always answerable for a
            // bird that is still live.
            const bool lost = b.live && d2 < kBirdDropM * kBirdDropM && !stillPerched(b, trees);

            // -------------------------------------------------------------
            // ...AND NONE OF THAT APPLIES TO THE ONE ON THE EDITOR'S DECK.
            //
            // IT HAD STOPPED BEING DRAWN AT ALL. stageOne PLACES a bird rather
            // than perching it -- there are no trees on the stage, which is the
            // whole point of the stage -- so the branch test above asks "is
            // there still a crown under this bird" of an empty list, gets no,
            // condemns it, fails to re-perch it and clears `live` on the FIRST
            // TICK after [I]. The deck has been empty ever since that test
            // landed, and silently: a bird that is never drawn prints nothing.
            //
            // A staged bird is exempt because the premise is different. The
            // test exists so a bird does not hang in the air where a felled
            // crown used to be (NOTHING FLOATS); this one is standing on a
            // floor, put there by hand, in a place with nothing to fell.
            // -------------------------------------------------------------
            if (b.staged) {
                if (b.live) tick(&b);
                continue;
            }

            if (!b.live || tooFar || lost) {
                Bird cand = b;
                cand.live = false;
                // -- A BIRD BEING BORN IS FLOORED. A BIRD MOVING HOUSE IS NOT --
                //
                // `lost` means the crown it was sitting in has just been felled,
                // and that is not a spawn -- the bird is already in the world,
                // you are watching it, and you are the one who took the tree
                // down. It has to be allowed the nearest branch there is.
                //
                // Everything else here IS a birth (an empty slot) or a recycle
                // from beyond a hundred metres, and neither of those may land
                // inside the floor. See kBirthMinM.
                tryPerch(&cand, trees, player, uint32_t(i), lost && b.live);
                if (cand.live) {
                    b = cand;
                } else if (lost) {
                    // Its tree really has gone and there is nowhere else: a
                    // bird sitting in the air where a crown used to be is the
                    // one case worth a gap. NOTHING FLOATS applies to birds.
                    b.live = false;
                }
            }
            if (b.live) tick(&b);
        }
    }


    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // EVERY LIVE MEMBER, FOR --clip-test. See LifeAt in scene/collide.h.
    //
    // Appends rather than assigns: the check wants every population in one
    // list, and a population that clears the vector is a population that hides
    // the eight before it.
    // -----------------------------------------------------------------------
    void livePoints(std::vector<LifeAt> *out) const {
        // FLAGGED inTree, and that flag is the whole reason this population is
        // in the check at all rather than left out of it. A perched songbird is
        // sitting on a branch inside a crown, so it reads as clipping and is
        // not -- leaving it out would be the same answer with nothing checking
        // that it is still the reason.
        for (const Bird &b : birds_)
            if (b.live && !b.staged) out->push_back({b.p, "songbird", 0.12f, true, false});
    }

    void publish(World &world) const {
        if (!ready_) return;
        for (int i = 0; i < kBirdSlots; ++i) {
            const int slot = kButterflySlots + i;
            const Bird &b = birds_[size_t(i)];
            const Species &sp = species_[size_t(b.species)];
            if (!b.live || !sp.ok) {
                world.setFlyerInstance(slot, 0, nullptr, 0.0f, 0.0f, 0.0f, nullptr, false);
                continue;
            }

            // Both are worked out in tick(), which is also where the turn
            // BETWEEN frames is measured -- see there. They moved out of this
            // function because publish is const and the step needs to remember
            // which frame was shown last.
            const int fi = b.fi;
            const int q = b.q;

            // -- TRIED AND REVERTED: A SWEPT TURN (2026-09-14) ---------------
            //
            // Asked for as "rotate it 90 degrees off the grid for 90 degree
            // turns, this will smooth the movement out", built, measured, and
            // taken back out the same day on sight of it.
            //
            // It worked as arithmetic: the drawn angle became a continuous
            // function of the clock and the instance carried the difference,
            //
            //     instance = th(t) + fi * (pi/2)/11
            //
            // the second term cancelling the rotation baked into the frame on
            // show. Measured at 60 fps that took the worst step from 8.18
            // degrees to 2.05 and moved the bird on every frame instead of one
            // in four, at the same rate and through the same poses.
            //
            // IT STILL LOOKED WRONG, and the likely reason is worth leaving
            // here: the eleven frames are not only a rotation. Sweeping the
            // instance underneath them turns the BODY continuously while the
            // pose it is wearing still steps, so the bird reads as an animal
            // being rotated rather than one turning itself. The stepping is
            // what makes it a bird looking about.
            //
            // If it is ever wanted again the change is four lines -- this angle,
            // the two fields on Bird and tick() -- and tests/bird_turn_test.cpp
            // in the history has the measurement.
            // -- QUARTER TURNS ONLY, SO IT SITS ON THE LATTICE ---------------
            //
            // This carried a continuous per-perch bearing, hashed anywhere in
            // the circle, and that is what put the birds off the grid: a voxel
            // model turned 37 degrees has no voxel face parallel to any world
            // voxel face, so it reads as a smooth object dropped into a blocky
            // wood -- and its shadow and its edges alias against everything
            // around it.
            //
            // The JS engine only ever turns one of these by a quarter, and says
            // so: "GRID-ALIGNED 4-way spin = the editor's edRotVox(-spin)". So
            // the perch's own facing is one of four, not one of infinity, and
            // it is ADDED to the animation's quarter -- the flock still faces
            // four different ways, and every one of them is square to the
            // world.
            // THE TURN ALONE, with no per-perch facing added to it. There was
            // one, and the JS engine has none -- its whole rotation is `q`. The
            // variety comes from the PHASE instead: two birds a random offset
            // apart in a 733 ms cycle are at different frames AND different
            // quarters at any instant, which is the same four-way spread
            // without a second source of truth about which way a bird faces.
            const float ang = float(q) * 1.5707963f;
            const float c = cosf(ang), s = sinf(ang);
            // NO VOXEL_M IN HERE, and that is the difference between a bird and
            // a bird a tenth of its size. A FLYER model is meshed at VOXEL_M --
            // see addFlyerModel -- so its object space is ALREADY metres and the
            // instance transform is a pure turn. That is not true of the drops
            // and the held item, whose matrices do carry the voxel scale, and
            // copying one of those is how the scale got applied twice.
            // setFlyerInstance says so over itself; the butterflies build the
            // same rotation with a fade in place of this 1.
            const float m[9] = {c, 0.0f, s, 0.0f, 1.0f, 0.0f, -s, 0.0f, c};

            // The mesh runs from its own corner and `p` is the middle of the
            // bird, so the translation is the centre less the half-box carried
            // through the turn -- the same correction the drops and the
            // butterflies make, and for the same reason.
            // THIS FRAME'S OWN BOX, not the strip's -- see the note in init.
            //
            // HALVED AS AN INTEGER, which is the JS engine's `rv.sx >> 1` and
            // not a rounding of 0.5 * sx. It matters twice over: it is what
            // that engine centres on, so the two agree; and it keeps the corner
            // ON THE LATTICE for every frame, odd dimensions included, which
            // the float half could not -- a 3-wide box has its float centre on
            // a half voxel and put the model half a voxel off the grid.
            const float cx = float(sp.bx[fi] >> 1) * VOXEL_M;
            const float cy = float(sp.by[fi] >> 1) * VOXEL_M;
            const float cz = float(sp.bz[fi] >> 1) * VOXEL_M;
            const float ox = m[0] * cx + m[1] * cy + m[2] * cz;
            const float oy = m[3] * cx + m[4] * cy + m[5] * cz;
            const float oz = m[6] * cx + m[7] * cy + m[8] * cz;

            // NO FLAP CHANNEL, and it is a judgement rather than an oversight.
            // That channel exists for a butterfly's wing, which SWAPS GRID
            // between poses and moves several voxels in one step while the
            // instance stands still. A bird's pose change is a rotation of the
            // whole body by 90/11 = 8.2 degrees, an eighth of what a wing does
            // and about its own axis, so the body's own transform already
            // describes nearly all of it. If it ever ghosts under Ray
            // Reconstruction the answer is an angular channel beside `flap`,
            // not a fudge here.
            // AND THE CORNER LANDS ON A VOXEL, not the centre. It is the
            // TRANSLATION that decides whether the model's lattice agrees with
            // the world's, and snapping the centre would not do it: a model an
            // odd number of voxels wide has its centre on a half voxel, so a
            // snapped centre puts every face half a voxel out. Snapping here,
            // after the half-box has been taken off, is the only place that
            // makes the two grids coincide -- which is the whole of what
            // "aligned to the grid" buys.
            //
            // Exact for a quarter turn: with ang a multiple of 90 degrees the
            // rotated half-box is itself a whole or half number of voxels, so
            // this moves the bird by at most half a voxel and never fights the
            // rotation.
            // EXACT, and the snap happens ONCE at the perch instead of here.
            //
            // Rounding the corner per frame looked right and cannot be: the
            // boxes alternate 3 and 6 voxels wide through the cycle, so two
            // frames whose CENTRES coincide have corners 1.5 voxels apart --
            // not a lattice offset. Snapping each would put one of them half a
            // voxel out and hand back a 5 cm wobble, which is the very thing
            // being fixed, just smaller.
            //
            // So the PERCH is snapped when it is chosen (see tryPerch) and the
            // half-box comes off exactly. The bird sits on the lattice, every
            // frame is centred on the same point, and a frame with an odd
            // dimension lands on a half voxel -- which is invisible, because
            // its faces are still square to the world.
            // -- THE FEET ON THE BRANCH, EXCEPT WHERE THE TABLE SAYS OTHERWISE
            //
            // b.p.y is the PERCH, so the y corner is it directly plus this
            // frame's lift -- nine frames stand on it and two are a voxel over
            // it. See kBirdLift.
            //
            // Deriving y from a CENTRE was the earlier bug and is worth keeping
            // written down: the frames are 6, 7 and 8 voxels tall across the
            // cycle, so a centre-based corner made a short frame rise and a
            // tall one SINK, which is neither the perch nor the hop -- it was
            // the model's own height leaking into its position.
            //
            // Only y is pinned. x and z still come off the centre, or the turn
            // would swing the bird round its corner instead of its middle.
            // -- AND THE TURN ITSELF, WHICH NO TRANSFORM OF IT DESCRIBES ------
            //
            // The rotation is in the ART: the instance stands perfectly still
            // while the surface turns 8.2 degrees under it, so a motion vector
            // derived from the transform says "did not move" about a bird that
            // did -- which is what was blurring them. The axis is the bird.s own
            // vertical through its perch. See V6Instance::flapPad.
            //
            // IT IS PUBLISHED ON EVERY FRAME AND dth IS ZERO ON MOST OF THEM,
            // which is fine and is handled where both fields are written:
            // setFlyerInstance drops the whole channel when the angle is zero.
            // It did not, once, and three frames in four of every perched bird
            // reported a vertical motion vector the size of its own altitude --
            // see the note there before changing either side of this.
            const float spin[4] = {b.p.x, b.p.y, b.p.z, b.dth};
            const float lift = float(kBirdLift[fi]) * VOXEL_M;
            // -- AND THE MOTION VECTOR IS MEASURED AT THE PERCH ---------------
            //
            // THE FLICKER THAT SURVIVED ROUND 2, and it was never in this file:
            // it was in what place() measured. prevOffset is the travel of the
            // model's BOUNDING-BOX CENTRE, and these eleven frames are a
            // quarter turn, so the box turns with them -- 3 x 7 x 6 voxels on
            // frame 00 against 6 x 7 x 3 on frame 05. The translation above
            // takes the box off so the BODY stays put; place() then put half of
            // a box that had just changed size back on.
            //
            // MEASURED over one full revolution of the real art, on all three
            // species, with the real publish arithmetic and a bird that does
            // not move: the box centre reports a WORST STEP OF 0.150 m and a
            // mean of 0.044 m, on every one of the 43 pose steps. Fifteen times
            // a second, about a bird sitting still.
            //
            // That is several pixels of motion vector on a bird at twenty
            // metres, aimed at the sky behind it. Reported as "the perched song
            // birds are flickering ... when I put DLSS on with no upscale the
            // flickering goes away", and that is the shape of a bad vector
            // rather than of a bad denoiser: at DLAA every output pixel has a
            // sample of its own this frame and the wrong history is outvoted;
            // at Balanced 42% of them do not and the history IS the answer.
            //
            // The perch is a point fixed to the bird -- it is chosen once and
            // snapped to the lattice (see tryPerch) -- so anchoring here
            // reports exactly zero for a sitting bird, which is the truth. The
            // turn inside the art is already described, separately and exactly,
            // by `spin`.
            //
            // WITH THE HOP IN IT, WHICH IS THE ONE THING THE BOX CENTRE GOT
            // RIGHT. Two of the eleven frames stand a voxel higher (kBirdLift),
            // and that is a real 10 cm of the whole bird -- anchoring at the
            // bare perch would report zero for it and smear the hop instead.
            // The perch plus this frame's lift is the bird's own foot, which is
            // exactly what moves and all that moves.
            const float anchor[3] = {b.p.x, b.p.y + lift, b.p.z};
            world.setFlyerInstance(slot, sp.model[fi], m, b.p.x - ox, b.p.y + lift, b.p.z - oz,
                                   nullptr, true, spin, anchor);
        }
        // AND THE BAND IS FLUSHED AFTER THIS HALF OF IT IS WRITTEN, not before.
        // The butterflies flush at the end of their own publish, and they run
        // first -- so without this the birds wrote 48 instance records that the
        // structure was never told about, and 27 perched songbirds changed
        // exactly zero pixels. Flushing twice is free: the second call finds
        // the dirty range already covered and does nothing.
        world.flushFlyerInstances();
    }

    // -----------------------------------------------------------------------
    // ONE CARDINAL ON THE EDITOR'S DECK.
    //
    // Everything else is cleared, because the stage is not the wood -- there
    // are no trees on it and nothing should be left perched in mid air from
    // wherever you pressed U. The bird is placed rather than perched: tryPerch
    // looks for a crown to sit on and there is not one, which is correct
    // behaviour in a place with no trees and exactly why the editor has to put
    // its own subject down by hand.
    //
    // It keeps its clock, so it turns on the deck the same way it turns on a
    // branch -- which is the point of looking at it here.
    // -----------------------------------------------------------------------
    void stageOne(const Vec3 &at) {
        if (!ready_) return;
        for (Bird &b : birds_) b = Bird{};
        // The cardinal is species 2 -- see kNames in init. Falls back to
        // whichever loaded, so a missing asset shows a bird rather than none.
        int sp = 2;
        for (int k = 0; k < kBirdSpecies && !species_[size_t(sp)].ok; ++k)
            sp = (sp + 1) % kBirdSpecies;
        if (!species_[size_t(sp)].ok) return;
        Bird &b = birds_[0];
        b.live = true;
        b.species = sp;
        b.p = Vec3(roundf(at.x / VOXEL_M) * VOXEL_M, roundf(at.y / VOXEL_M) * VOXEL_M,
                   roundf(at.z / VOXEL_M) * VOXEL_M);
        b.phase = 0.0f;
        b.started = false;
        b.staged = true;
    }

    // BACK TO THE WOOD. The flag has to go or the bird keeps its exemption
    // among real trees, where the branch test is the thing that stops it
    // hanging in the air over a stump.
    void unstage() {
        for (Bird &b : birds_) {
            b.staged = false;
            b.live = false;   // ...and it re-perches on the first tree it finds
        }
    }

    // Where one is, for a scripted shot to aim at. A perched bird is a small
    // thing high in a crown and finding one by eye in a 3820-wide frame is not
    // a verification method.
    bool nth(int n, Vec3 *out) const {
        for (const Bird &b : birds_) {
            if (!b.live) continue;
            if (n-- > 0) continue;
            *out = b.p;
            return true;
        }
        return false;
    }

    // ...AND THE NEAREST ONE, which is what /locate needs and nth(0) is not:
    // nth walks the slots in index order, and a slot's index says nothing
    // about where its bird ended up perching.
    //
    // XZ ONLY, and here that is a decision rather than a shortcut. A perched
    // songbird sits ten metres up a trunk, so counting the height in would
    // rank the bird directly overhead behind one twice as far off across the
    // clearing -- and the question /locate asks is which one you walk to.
    bool nearest(const Vec3 &p, Vec3 *at, float *dist) const {
        float best = 1e30f;
        for (const Bird &b : birds_) {
            if (!b.live) continue;
            const float dx = b.p.x - p.x, dz = b.p.z - p.z;
            const float d = dx * dx + dz * dz;
            if (d >= best) continue;
            best = d;
            if (at) *at = b.p;
        }
        if (best > 1e29f) return false;
        if (dist) *dist = sqrtf(best);
        return true;
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
    BirthGate birth_;

    struct Species {
        bool ok = false;
        int model[kBirdFrames] = {-1};
        // PER FRAME, because a quarter turn turns the footprint with it.
        int bx[kBirdFrames] = {0}, by[kBirdFrames] = {0}, bz[kBirdFrames] = {0};
    };
    struct Bird {
        bool live = false;
        int species = 0;
        Vec3 p{0, 0, 0};
        float phase = 0.0f;    // ...and where in the cycle it is, so a wood does not pulse as one
        // The animation clock.s output -- see tick().
        // Placed on the editor's deck rather than perched in a wood -- see the
        // exemption in update(). Cleared by unstage() on the way back out.
        bool staged = false;
        int fi = 0, q = 0, fiWas = 0;
        float dth = 0.0f;      // radians turned since the last rendered frame
        bool started = false;  // ...so the first frame does not report a step
    };

    // -----------------------------------------------------------------------
    // WHERE IT IS IN ITS TURN, and HOW FAR IT TURNED TO GET THERE.
    //
    // The frame index and the quarter both come off one clock so they cannot
    // drift apart -- that is what makes the hand-off seamless, since the instant
    // the frame wraps to 0 the quarter has advanced and put the model where
    // frame 10 left it.
    //
    // THE STEP IS MEASURED, NOT ASSUMED, and it is what the motion vector needs.
    // The total turn is continuous by construction: a quarter per cycle, and the
    // eleven frames divide it, so one frame step is 90/11 = 8.2 degrees. It is
    // reported only on the frames where the index actually CHANGED -- ten of
    // every eleven rendered frames the bird is holding a pose and has moved by
    // nothing at all, and saying otherwise would smear a stationary bird.
    //
    // The sign follows from continuity. At the wrap the quarter steps by -90 and
    // the frame index drops by ten, so 11 * step = -90: the bird turns the
    // negative way, 8.2 degrees at a time.
    void tick(Bird *b) {
        const double cyc = double(kBirdFrames) * double(kBirdStepMs);
        const double t = fmod(nowMs_ + b->phase, cyc);
        const int fi = mini(kBirdFrames - 1, int(t / double(kBirdStepMs)));
        const int q = (-(int(floor((nowMs_ + b->phase) / cyc)) % 4)) & 3;
        int steps = fi - b->fiWas;
        if (steps < 0) steps += kBirdFrames;   // it wrapped
        b->dth = (b->started && steps > 0)
                     ? -float(steps) * 1.5707963f / float(kBirdFrames)
                     : 0.0f;
        b->fi = fi;
        b->q = q;
        b->fiWas = fi;
        b->started = true;
    }

    // Is there still a branch under this bird?
    //
    // Half a metre of tolerance, because the perch was taken from a column top
    // and a crown re-meshed by an axe can lose a voxel without ceasing to be a
    // branch. A dropped tree has no columns at all -- dropSolid clears col --
    // so a bird on a felled tree finds nothing anywhere and lets go.
    bool stillPerched(const Bird &b, const std::vector<Solid> &trees) const {
        for (const Solid &s : trees) {
            if (!s.col || s.standable) continue;
            float y = 0.0f;
            if (!solidColumnTop(s, b.p.x, b.p.z, VOXEL_M, &y)) continue;
            if (fabsf(y - b.p.y) < 0.5f) return true;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // Find one perch on the OUTER EDGE of a crown.
    //
    // Walk outward along a ray from the trunk until the crown stops answering,
    // and take the last column that did. `solidColumnTop` is false off the
    // footprint or over an empty column, so the last true IS the silhouette --
    // no per-species radius, and it follows whatever shape the crown has at
    // that height.
    // -----------------------------------------------------------------------
    void tryPerch(Bird *b, const std::vector<Solid> &trees, const Vec3 &player, uint32_t salt,
                  bool displaced = false) {
        if (trees.empty()) return;
        const uint32_t seed = hashU32(salt * 2654435761u, uint32_t(int(nowMs_ * 0.001)));

        for (int attempt = 0; attempt < 6; ++attempt) {
            const uint32_t h = hashU32(seed, uint32_t(attempt));
            const Solid &s = trees[size_t(h % uint32_t(trees.size()))];
            if (!s.col || s.msx <= 0 || s.msz <= 0) continue;
            // NEAR THE PLAYER. The gather is 115 m wide so that a live bird's
            // own tree is always in it (see the radii note); a NEW perch must
            // not use that width, or the flock spreads itself over eight
            // hectares and the trees beside you are bare.
            {
                const float px = s.cx - player.x, pz = s.cz - player.z;
                const float pd2 = px * px + pz * pz;
                if (pd2 > kBirdPlaceM * kBirdPlaceM) continue;
                // ...AND NOT ON TOP OF YOU EITHER. 30 m against the 42 m
                // ceiling leaves a band of trees twelve metres deep, which over
                // a wood is a few dozen crowns for forty-eight birds -- and a
                // bird that misses gets six more attempts on the next frame
                // rather than being dropped, so a thin band costs a few frames
                // and never a gap. See kBirthMinM.
                if (!displaced && !birth_.may(pd2)) continue;
            }
            // A TRUNK, NOT A ROCK. `standable` is what tells them apart: a rock
            // you can climb, a trunk carries a canopy far over your head.
            if (s.standable) continue;
            const float tall = s.top - s.baseY;
            if (tall < kBirdMinTreeM) continue;

            const float ang = hashUnit(h, 11u) * 6.2831853f;
            const float dx = cosf(ang), dz = sinf(ang);
            // Out to the model's own diagonal, which is as far as its footprint
            // can possibly reach; the walk stops long before that.
            //
            // NOT named 'far'. windows.h still defines near and far as empty
            // macros from the segmented memory era, so `const float far =`
            // compiles as `const float =` and the error names neither of them.
            // app.h carries the same note over its own `nearby`.
            const float reach = 0.5f * (float(s.msx) + float(s.msz)) * VOXEL_M;

            // -- FROM THE TRUNK, WHICH IS cx/cz AND NOT tx/tz ---------------
            //
            // This walked from `s.tx`, and that is the model's CORNER: it is
            // the instance translation, the point solidModelSpace subtracts to
            // get model space, so it maps to model (0, 0). A ray leaving the
            // corner is inside the footprint only when BOTH model-space
            // components are positive -- one quadrant of the circle -- so
            // three rays in four left the tree on their first step and the
            // attempt was spent on nothing. Measured on the nine pines: 24.9%
            // of rays found a perch, which is 0.75^6 = 18% of calls failing
            // all six attempts and the bird not being drawn at all that frame.
            //
            // `s.cx`/`s.cz` is the collider's centre -- measureCollider takes
            // the widest cross-section of the first two metres, which on a tree
            // is the trunk. From there every direction crosses the crown, and
            // the same measurement gives 99.8%.
            float lastX = 0.0f, lastZ = 0.0f, lastY = 0.0f;
            bool any = false;
            for (float r = 0.3f; r <= reach; r += VOXEL_M) {
                // -- AND THE WALK RUNS ON THE LATTICE -----------------------
                //
                // The perch used to be snapped to the voxel grid AFTER it was
                // chosen, which meant the column that was TESTED and the point
                // that was STORED were up to half a voxel apart -- enough to
                // land in the next cell. On the outer silhouette, which is
                // where a perch is by construction, the next cell out is empty,
                // so `stillPerched` asked about the stored point, got nothing,
                // and condemned a branch that was there. Measured: 48.8% of
                // perches failed the test on the very next frame.
                //
                // Snapping HERE makes the tested point and the stored point the
                // same point, and the two questions can no longer disagree. Same
                // measurement afterwards: 0.0% lost.
                const float wx = roundf((s.cx + dx * r) / VOXEL_M) * VOXEL_M;
                const float wz = roundf((s.cz + dz * r) / VOXEL_M) * VOXEL_M;
                float y = 0.0f;
                if (!solidColumnTop(s, wx, wz, VOXEL_M, &y)) continue;
                const float frac = (y - s.baseY) / maxf(0.01f, tall);
                if (frac < kBirdLoFrac || frac > kBirdHiFrac) continue;
                lastX = wx;
                lastZ = wz;
                lastY = y;
                any = true;
            }
            if (!any) continue;

            // Not on top of another bird -- EXCEPT ITSELF.
            //
            // update() re-perches by copying the bird, clearing `live` on the
            // COPY and searching with that; the original is still live in
            // birds_ at its old position, so the bird occupied its own 1.2 m
            // sphere and could not be offered the branch it was already on, or
            // anything beside it. A bird that only wanted to stay put was
            // therefore forced to jump or to fail -- and failing is the frame
            // it is not drawn. `salt` is the slot, at both call sites.
            bool clash = false;
            for (size_t oi = 0; oi < birds_.size(); ++oi) {
                if (oi == size_t(salt)) continue;
                const Bird &o = birds_[oi];
                if (!o.live) continue;
                const float ax = o.p.x - lastX, ay = o.p.y - lastY, az = o.p.z - lastZ;
                if (ax * ax + ay * ay + az * az < kBirdSepM * kBirdSepM) { clash = true; break; }
            }
            if (clash) continue;

            // And within sight of the player, or it is a bird nobody will meet.
            const float px = lastX - player.x, pz = lastZ - player.z;
            if (px * px + pz * pz > kBirdDropM * kBirdDropM) continue;

            int sp = int(h >> 8) % kBirdSpecies;
            for (int k = 0; k < kBirdSpecies && !species_[size_t(sp)].ok; ++k)
                sp = (sp + 1) % kBirdSpecies;
            if (!species_[size_t(sp)].ok) return;

            b->live = true;
            b->species = sp;
            // ITS FEET ON THE CROWN, so the model sits on the needle rather
            // than half inside it: the perch is the TOP of that column and the
            // bird's own half-height lifts it clear.
            // ON THE LATTICE, ONCE. Everything downstream takes exact offsets
            // from this point, so snapping it here is what puts the bird on the
            // grid -- and doing it once is what keeps every frame of the turn
            // centred on the same place.
            // THE PERCH ITSELF -- the crown surface the feet stand on. Not
            // the body centre: the publish pins y to this and takes only the
            // horizontal half-box off, so the feet cannot drift with the
            // frame.s height. Snapped once, which is what puts it on the grid.
            // x and z are ALREADY on the lattice -- the walk above snapped them
            // before it tested them, which is the whole point. Re-rounding here
            // is what used to move the perch off the column it was found on.
            b->p = Vec3(lastX, roundf(lastY / VOXEL_M) * VOXEL_M, lastZ);
            b->phase = hashUnit(h, 37u) * float(kBirdFrames) * kBirdStepMs;
            return;
        }
    }

    std::vector<Bird> birds_;
    Species species_[kBirdSpecies];
    double nowMs_ = 0.0;
    int loaded_ = 0;
    bool ready_ = false;
};

}  // namespace v2
