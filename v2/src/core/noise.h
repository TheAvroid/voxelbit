// ---------------------------------------------------------------------------
// noise.h -- the procedural basis for terrain, scatter and bark.
//
// v3 inherited voxelbit's f64 JS-parity noise because its height field was
// quantised to whole voxels: a last-bit difference there moved a shoreline by a
// voxel, so bit-exactness was worth the cost. The Embree engine rendered a smooth mesh
// with no quantisation step anywhere, so nothing is thresholded on a lattice
// boundary and f32 is both sufficient and roughly twice the speed. The lattice
// hash is kept in u32 integer maths regardless, so a coordinate far from the
// origin never loses precision the way a float-multiply hash would.
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdint>

#include "vecmath.h"

namespace v2 {

// A 2D integer hash with a full 32-bit avalanche. Integer in, float out, so a
// cell a million units from the origin hashes as well as the one at it.
inline float ihash2(int32_t x, int32_t z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) * 2.3283064e-10f;  // / 2^32
}

inline float ihash3(int32_t x, int32_t y, int32_t z) {
    uint32_t h = uint32_t(x) * 374761393u + uint32_t(y) * 1103515245u + uint32_t(z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h = h ^ (h >> 16);
    return float(h) * 2.3283064e-10f;
}

// A seeded scalar hash, for per-tree and per-branch decisions.
inline uint32_t hashU32(uint32_t a, uint32_t b) {
    uint32_t h = a * 374761393u + b * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
inline float hashUnit(uint32_t a, uint32_t b) { return float(hashU32(a, b)) * 2.3283064e-10f; }

// 2D value noise: bilinear smoothstep over the hash lattice, one unit per cell.
inline float vnoise(float x, float z) {
    const float fx = std::floor(x), fz = std::floor(z);
    const int32_t ix = int32_t(fx), iz = int32_t(fz);
    const float tx = sstep(x - fx), tz = sstep(z - fz);
    const float a = ihash2(ix, iz), b = ihash2(ix + 1, iz);
    const float c = ihash2(ix, iz + 1), d = ihash2(ix + 1, iz + 1);
    return lerpf(lerpf(a, b, tx), lerpf(c, d, tx), tz);
}

// ---------------------------------------------------------------------------
// THE SAME NOISE, WITH THE LATTICE REMEMBERED.
//
// The terrain is sampled along ROWS of voxel columns, and a voxel is 10 cm
// while the lowest frequency in the height field has a wavelength of about
// eighty metres. So consecutive samples land in the SAME lattice cell hundreds
// of times over -- and the old vnoise hashed all four corners of that cell
// again for every one of them. Meshing a chunk is 67 600 columns of roughly
// twenty-four octaves, so those four hashes were being recomputed about six
// million times per chunk to produce numbers that had not changed.
//
// A memo of the cell fixes it, and the fix is FREE OF CONSEQUENCE: the corner
// values are the same values, tx and tz are computed the same way, and the
// lerps are in the same order, so this returns bit-for-bit what the function
// above returns. The world is not moved by a millimetre -- which matters more
// here than it looks, because every decision downstream (where a tree may
// stand, whether ground is rock or soil) is a THRESHOLD on this number, and a
// last-bit difference moves a tree.
//
// One memo per octave per call site. Sharing one between two call sites would
// still be correct -- it checks the cell before trusting it -- but it would
// miss every time and cost a branch for nothing.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// AND THE ROW REMEMBERED TOO, which is a second saving on top of the first and
// a larger one than it looks.
//
// The memo above stops the four corner hashes being recomputed. It does not
// stop anything else: floor(z), the integer row, and sstep(z - fz) were still
// evaluated on EVERY sample, per octave, even though the mesher walks i on the
// inside and z therefore holds for a whole row of 260 columns. That is a floor,
// a convert, a subtract, a saturate and a cubic -- roughly as much arithmetic
// as the interpolation it feeds -- thrown away and redone hundreds of times to
// arrive at the number it already had.
//
// Cached on the VALUE of z rather than on a row index the caller would have to
// keep, so nothing above this line has to know that rows exist and the
// memo-less form below stays exactly as it was.
//
// THE SENTINEL IS FINITE ON PURPOSE. The obvious choice is a NaN, which never
// compares equal and so always misses first time -- but this builds with
// /fp:fast, and "there are no NaNs" is precisely the assumption fast math is
// licensed to make. 1e30f is unreachable for a coordinate in this world (the
// largest z any call site produces is world metres times 0.55) and it compares
// like an ordinary number under any float model.
//
// STILL BIT-EXACT. The cached values come from the same expressions applied to
// the same z; tx is computed where it always was; the two lerps and the third
// that blends them are in the order they always were. Nothing here reassociates
// anything, which is the whole reason this version was worth having over the
// faster one that hoists the vertical lerp -- see the note above on what a last
// bit costs when every decision downstream is a threshold.
// ---------------------------------------------------------------------------
struct NoiseCell {
    // Row state: everything that is a function of z alone.
    float zKey = 1e30f;  // no coordinate reaches this, so the first query misses
    float tz = 0.0f;
    int32_t rz = 0;

    // Cell state: the four corner hashes. A cell index no real coordinate
    // reaches, so the first query always misses.
    int32_t cx = 0x7FFFFFFF, cz = 0x7FFFFFFF;
    float a = 0.0f, b = 0.0f, c = 0.0f, d = 0.0f;
};

inline float vnoise(NoiseCell &k, float x, float z) {
    if (z != k.zKey) {
        const float fz = std::floor(z);
        k.zKey = z;
        k.rz = int32_t(fz);
        k.tz = sstep(z - fz);
    }
    const float fx = std::floor(x);
    const int32_t ix = int32_t(fx);
    if (ix != k.cx || k.rz != k.cz) {
        k.cx = ix;
        k.cz = k.rz;
        k.a = ihash2(ix, k.rz);
        k.b = ihash2(ix + 1, k.rz);
        k.c = ihash2(ix, k.rz + 1);
        k.d = ihash2(ix + 1, k.rz + 1);
    }
    const float tx = sstep(x - fx);
    return lerpf(lerpf(k.a, k.b, tx), lerpf(k.c, k.d, tx), k.tz);
}

// Eight octaves of memo, which is more than any call site in the engine asks
// for and small enough to sit on the stack without thinking about it.
struct FbmMemo {
    NoiseCell o[8];
};

// Five octaves. The offsets decorrelate the lattices, which would otherwise all
// pass through the origin together and leave a visible cross there.
inline float fbm(FbmMemo &m, float x, float z, int octaves = 5) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    float ox = 0.0f, oz = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += vnoise(m.o[i & 7], x * freq + ox, z * freq + oz) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;  // not exactly 2, so octaves never re-align on the lattice
        ox += 17.3f;
        oz += 9.7f;
    }
    return sum / norm;
}

// The memo-less form, for the handful of callers that ask about one scattered
// point and would never hit a memo anyway.
inline float fbm(float x, float z, int octaves = 5) {
    FbmMemo m;
    return fbm(m, x, z, octaves);
}

// Ridged multifractal: 1 - |2n - 1|, squared. This is what puts creases along
// the tops of the ridges instead of the rounded domes plain fbm gives, and a
// conifer landscape reads as ridge-and-valley more than as dunes.
inline float ridged(FbmMemo &m, float x, float z, int octaves = 5) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    float ox = 0.0f, oz = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        float n = vnoise(m.o[i & 7], x * freq + ox, z * freq + oz);
        n = 1.0f - std::fabs(2.0f * n - 1.0f);
        sum += n * n * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
        ox += 23.1f;
        oz += 31.9f;
    }
    return sum / norm;
}

inline float ridged(float x, float z, int octaves = 5) {
    FbmMemo m;
    return ridged(m, x, z, octaves);
}

// Displacing the sample point by another noise field before evaluating. Without
// it every ridge runs along the lattice axes; with it they meander the way an
// eroded ridge does.
inline float warpedFbm(FbmMemo &mx, FbmMemo &mz, FbmMemo &mm, float x, float z, float warp,
                       int octaves = 5) {
    const float wx = fbm(mx, x * 0.5f + 5.2f, z * 0.5f + 1.3f, 3) - 0.5f;
    const float wz = fbm(mz, x * 0.5f + 9.1f, z * 0.5f + 7.7f, 3) - 0.5f;
    // The warped sample point does not step evenly, so this memo hits less
    // often than the others -- but the warp field itself is two octaves at a
    // hundred and fiftieth of the sample rate, so the displacement crawls and
    // it still hits nearly always.
    return fbm(mm, x + wx * warp, z + wz * warp, octaves);
}

inline float warpedFbm(float x, float z, float warp, int octaves = 5) {
    FbmMemo mx, mz, mm;
    return warpedFbm(mx, mz, mm, x, z, warp, octaves);
}

// ---------------------------------------------------------------------------
// WHERE A THING IS BORN IS A PROPERTY OF THE WORLD, NOT OF THE PLAYER.
//
// "Have the lillypads and fish spawn in like the song birds. procedurally
// generated. all entities should follow the same spawning mechanics. everything
// generates with the terrain itself."
//
// THE OLD WAY WAS A DRAW, AND THAT IS THE WHOLE PROBLEM WITH IT. fill() picked
// a random wet cell out of the lake nearest the player, which means a lily pad
// had no place in the world until somebody walked up to it -- the same pond
// grew different pads every time you came back, only ONE lake was ever
// populated however many you could see, and nothing could be drawn far away
// because nothing far away existed yet.
//
// THE LATTICE IS THE BUTTERFLIES' ANSWER AND IT IS THE RIGHT ONE. One candidate
// site per cell of a fixed grid, its exact position a hash of the CELL's
// coordinates -- so a site is a fact about the world, computed identically
// wherever the player happens to be standing and whether or not anything is
// occupying it. What the population does is CLAIM sites, not invent them.
//
// Three things follow that are worth having on purpose:
//   * Every lake in range gets life, not just the one under your feet.
//   * A pad you swim away from and come back to is the same pad in the same
//     place -- because the cell decided, and the cell has not moved.
//   * Density is a property of the grid, so it is uniform over water by
//     construction rather than by a rejection rule that has to be tuned.
//
// A SITE IS INSET FROM ITS CELL'S EDGE. Without that, two sites in adjoining
// cells can land against the shared boundary and be centimetres apart, which
// puts two pads in the same place and reintroduces the clumping the grid is
// there to prevent.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// ...AND IN WHAT ORDER THEY ARE CLAIMED, WHICH IS NOT "NEAREST FIRST".
//
// "The life seems to cluster around the player on spawn, but then further out
// the life is sparse" (user 2026-09-14). Both halves of that are one bug, and
// the butterflies already carry the diagnosis -- they are the only population
// that was built with it:
//
//   "handing the flock the N closest homes packs every one of them into the
//    inner fifth of the disc's AREA, and since homes are never re-rolled,
//    standing still freezes that fill in place."
//
// THE ARITHMETIC IS BRUTAL AT SMALL COUNTS. Take the nearest N sites of an 11 m
// lattice and they all fall inside a radius of about 11 * sqrt(N/pi): for TWO
// rabbits that is nine metres, out of a spawn disc of ninety-six. Every land
// animal in the wood is therefore standing on top of you the moment the world
// loads, and there is nothing at all past the first few strides -- which is
// exactly the two things reported, from one cause.
//
// AN ARBITRARY PER-CELL KEY FIXES IT. The candidate list is already uniform
// over the disc (it is a lattice), so taking the smallest key picks uniformly
// FROM it. The key is a hash of the cell and the population's own salt, so it
// is stable: the same site wins every time, a population does not re-shuffle
// while you stand still, and two species do not queue for the same cells.
// ---------------------------------------------------------------------------
inline float siteOrder(uint32_t salt, int cx, int cz) {
    return hashUnit(salt ^ 0x51D3u,
                    hashU32(uint32_t(cx) * 421u + 17u, uint32_t(cz) * 419u + 31u));
}

inline void siteOf(float cellM, uint32_t salt, int cx, int cz, float *x, float *z) {
    const uint32_t h = hashU32(salt ^ (uint32_t(cx) * 2654435761u), uint32_t(cz) * 40503u);
    const float inset = cellM * 0.2f;
    const float span = cellM - 2.0f * inset;
    *x = float(cx) * cellM + inset + hashUnit(0xA71u, h) * span;
    *z = float(cz) * cellM + inset + hashUnit(0xA72u, h) * span;
}

// ---------------------------------------------------------------------------
// ...AND NOTHING IS BORN WHERE YOU CAN WATCH IT ARRIVE.
//
// "I saw lillypads just appear in front of me. they seemed to just grow out of
// nowhere ... every biome should share similar mechanics. make sure there are
// no entities that do this, they all share the same spawning mechanics."
//
// THE LATTICE ABOVE SAYS WHERE A THING MAY BE. THIS SAYS WHEN IT MAY START
// BEING THERE, and it is the half that was missing. A site is a fact about the
// world at every distance, so the nearest unclaimed one is as likely to be at
// your feet as at the horizon -- and a slot that frees up, because its holder
// passed the drop radius behind you, is handed straight to it.
//
// A FADE DOES NOT COVER THIS. Eight tenths of a second growing up from 8% scale
// is an arrival, and an arrival six metres in front of you is an event however
// smoothly it is drawn. The fade is for a thing LEAVING at a hundred and eighty
// metres, which is the distance it was measured at.
//
// THIRTY METRES, AND THE SAME THIRTY FOR EVERY POPULATION. That is the whole
// request: a lily pad, a butterfly, a rabbit and a perched songbird are all
// born outside it, so no creature in the world has a spawn rule of its own to
// go wrong on its own.
//
// ONE EXEMPTION, AND IT IS NOT "THE POPULATION IS EMPTY". That was the first
// form of this and it re-creates the very bug it is here to stop: walk out of a
// dry birch wood -- where no pad is live anywhere, so the population IS empty --
// up to a pond, and the rule is waived at exactly the moment you are looking at
// the water. What the exemption is really for is being PUT somewhere: the world
// loading, the pause room closing, the editor deck closing. There is no
// previous frame to pop against then, and refusing to fill would leave the
// place you were just dropped into bare.
//
// So it is a TELEPORT that waives it, measured as a jump no walk could make in
// one tick, and it holds for a second afterwards -- a population that fills
// over many frames rather than in one pass (the perched birds get six attempts
// a frame) needs more than the single tick the jump happens on.
// ---------------------------------------------------------------------------
inline constexpr float kBirthMinM = 30.0f;
inline constexpr float kBirthJumpM = 40.0f;   // in ONE tick; a sprint is 2.3 m/s
inline constexpr float kBirthWaiveSec = 1.0f;
// -- ...AND THE HALF THAT A DISTANCE CANNOT DO ------------------------------
//
// "The lillypads are still just appearing in." A floor is a PROXY for "where
// you can watch it arrive" and it is a poor one, because the thing that decides
// whether you see an arrival is not how far away it is, it is whether you are
// looking at it. A lily pad is 0.9 m across: at the thirty-metre floor it is
// still fifty pixels, and fifty pixels appearing out of nothing is an event
// wherever it happens.
//
// So a population that can afford it refuses a birth inside the VIEW CONE as
// well. Seventy degrees either side of the heading, which covers a ninety-
// degree field of view with margin -- and leaves two hundred and twenty degrees
// of world to be born into, so nothing starves. A pad born behind you is simply
// THERE when you turn round, which is what scenery is supposed to be.
//
// IT IS OPT-IN, and that is not timidity. The cone is only worth its cost where
// the thing being born is static and big enough to notice; a butterfly at
// thirty metres is two pixels and its own flight is a bigger event than its
// arrival. Pass no heading and the cone is not tested at all -- which is also
// what the offline renders do, so their reports are unchanged.
inline constexpr float kBirthConeCos = 0.342f;   // cos 70 degrees
// -- ...AND THE CONE HAS TO STOP SOMEWHERE, OR THE LAKE NEVER FILLS ---------
//
// "The life in the water has a very delayed spawn" (user 2026-09-14), and the
// cone above is the whole of it. Walk toward a lake LOOKING AT IT -- which is
// what anyone does -- and every site on that water is inside the cone, so
// every birth is refused. You arrive at an empty lake, turn away, and it fills
// behind you. The rule meant to stop a pad appearing in view had quietly become
// a rule that stops water in front of you having anything in it.
//
// So the cone applies only where an arrival could actually be SEEN as an
// arrival. Past this, a lily pad is 0.9 m at ninety metres -- a dozen pixels,
// in a wood, while you are walking -- and the population fills whatever you are
// walking toward long before you get there. The place radius is 170 m, so a
// lake entering range is claimed the moment it does, whichever way you face.
//
// THE THREE RULES TOGETHER, in the order they are asked: nothing inside thirty
// metres ever; anything past ninety, whatever you are looking at; and between
// the two, only behind you.
inline constexpr float kBirthFarM = 90.0f;

class BirthGate {
  public:
    // Once per update, before anything asks. It takes dt so the waiver is a
    // DURATION rather than a frame count -- at 200 fps a tick-counted one would
    // be over in five milliseconds.
    // fx/fz: which way the player is FACING, normalised, or zero for a
    // population that does not use the cone. Only the horizontal part matters
    // -- a site is a place on the ground and looking up at the sky does not
    // make the water in front of you unwatched.
    void tick(float dt, float px, float pz, float fx = 0.0f, float fz = 0.0f) {
        const float dx = px - lx_, dz = pz - lz_;
        if (!had_ || dx * dx + dz * dz > kBirthJumpM * kBirthJumpM)
            waive_ = kBirthWaiveSec;
        else
            waive_ = maxf(0.0f, waive_ - dt);
        lx_ = px;
        lz_ = pz;
        const float fl = sqrtf(fx * fx + fz * fz);
        fx_ = fl > 1e-4f ? fx / fl : 0.0f;
        fz_ = fl > 1e-4f ? fz / fl : 0.0f;
        had_ = true;
    }

    // May something be born this far from the player? SQUARED, because every
    // caller has the square already and none of them wants a sqrt to compare.
    bool may(float d2) const { return waive_ > 0.0f || d2 >= kBirthMinM * kBirthMinM; }

    // ...and not in front of you, when a heading was given. See kBirthConeCos.
    bool mayAt(float dx, float dz) const {
        const float d2 = dx * dx + dz * dz;
        if (waive_ > 0.0f) return true;
        if (d2 < kBirthMinM * kBirthMinM) return false;
        // FAR ENOUGH IS FAR ENOUGH, whichever way you are facing -- see
        // kBirthFarM. This is what keeps a lake you are walking toward from
        // being empty when you reach it.
        if (d2 >= kBirthFarM * kBirthFarM) return true;
        if (fx_ == 0.0f && fz_ == 0.0f) return true;
        const float d = sqrtf(d2);
        if (d <= 1e-4f) return false;
        return (dx * fx_ + dz * fz_) / d <= kBirthConeCos;
    }
    bool waived() const { return waive_ > 0.0f; }

  private:
    float lx_ = 0.0f, lz_ = 0.0f, waive_ = 0.0f;
    float fx_ = 0.0f, fz_ = 0.0f;
    bool had_ = false;
};

// -- ...AND A SLOT HELD FAR AWAY YIELDS TO THE WATER YOU ARE STANDING IN -----
//
// The floor above is only half of it, and on its own it trades one wrong
// picture for another. A population is a FIXED number of slots over an
// unbounded lattice, so the slots go to whatever was nearest when they were
// claimed -- and walking from one pond to the next, the first pond holds every
// slot until its pads pass the drop radius, while the pond at your feet has
// nothing in it at all. That is what the twelve-metre births actually were: the
// moment the old holders finally died, the freed slots took the nearest free
// sites, and the nearest free sites were the ones in front of you.
//
// So a live member much farther away than an unclaimed site RETIRES: it starts
// the ordinary fade-out, at a distance where that fade is what it was measured
// for, and its slot comes back for the near site on a later frame.
//
// THE MARGIN IS WHAT KEEPS IT FROM THRASHING. The holder has to be this much
// farther than the site, not merely farther, or two sites at similar ranges
// would hand one slot back and forth every frame. One retirement per population
// per pass, for the same reason: a lake that is suddenly better than the one
// behind you should refill over a few seconds rather than swap its whole
// population in a frame.
inline constexpr float kYieldMarginM = 55.0f;
// ...and the site has to be somewhere the population is not already, or the
// rule fires for ever on a lattice finer than the margin. See yieldSite.
inline constexpr float kYieldLonelyM = 30.0f;


}  // namespace v2
