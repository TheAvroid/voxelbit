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

namespace v4 {

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
// THE SAME NOISE, IN THREE DIMENSIONS -- what a cave is cut out of.
//
// The height field can say where the ground stops and nothing else, which is
// why this world had no caves and no overhangs: solidity was `y <= h`, and a
// comparison against a surface cannot describe a room under it. A 3D field
// can, and it is the only new primitive the volume needs -- everything else in
// the generator already answers per-voxel questions (see materialAt).
//
// TRILINEAR OVER THE HASH LATTICE, exactly as vnoise is bilinear over it, so
// the two read as the same world. No memo form: the carve is sampled down a
// COLUMN rather than along a row, so consecutive samples differ in y and share
// the x/z pair -- which is the axis a NoiseCell does not cache. The saving that
// would matter here is skipping the field entirely for columns nowhere near a
// cave, and that is what caveNear does.
// ---------------------------------------------------------------------------
inline float vnoise3(float x, float y, float z) {
    const float fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int32_t ix = int32_t(fx), iy = int32_t(fy), iz = int32_t(fz);
    const float tx = sstep(x - fx), ty = sstep(y - fy), tz = sstep(z - fz);
    const float a = ihash3(ix, iy, iz),         b = ihash3(ix + 1, iy, iz);
    const float c = ihash3(ix, iy, iz + 1),     d = ihash3(ix + 1, iy, iz + 1);
    const float e = ihash3(ix, iy + 1, iz),     f = ihash3(ix + 1, iy + 1, iz);
    const float g = ihash3(ix, iy + 1, iz + 1), h = ihash3(ix + 1, iy + 1, iz + 1);
    const float lo = lerpf(lerpf(a, b, tx), lerpf(c, d, tx), tz);
    const float hi = lerpf(lerpf(e, f, tx), lerpf(g, h, tx), tz);
    return lerpf(lo, hi, ty);
}

// Octaves of it. The same 2.03 ratio and the same decorrelating offsets the 2D
// form uses, so a cave system and the hillside over it never re-align on the
// lattice and leave a seam along an axis.
inline float fbm3(float x, float y, float z, int octaves = 4) {
    float sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += vnoise3(x * freq + ox, y * freq + oy, z * freq + oz) * amp;
        norm += amp;
        amp *= 0.5f;
        freq *= 2.03f;
        ox += 17.3f;
        oy += 23.9f;
        oz += 9.7f;
    }
    return sum / norm;
}

}  // namespace v4
