// ---------------------------------------------------------------------------
// shore.h -- where the water actually ends, and what a floating thing does
// when it gets there.
//
// "Have the lillypads bounce off the sandy shore, instead of clipping through
// it." They clipped for two reasons that compound, and only one of them is the
// missing bounce:
//
//   * A PAD WAS A POINT. The drift asked whether the water was wet three metres
//     ahead of its CENTRE, so half a leaf could be over sand and every test in
//     the file still said yes. The leaves are 0.5 to 0.9 m across.
//
//   * THE WATER FIELD CANNOT ANSWER THIS. It is sampled on a two-metre lattice
//     and each cell carries the wetness of its own corner column, so "is it wet
//     here" is only right to within a cell -- a pad can be most of two metres
//     inland with its cell still reading wet. No amount of lookahead fixes
//     that, because the lookahead is asking something that does not know where
//     the shore is either.
//
// So the field stays the BROAD phase -- is there a bank anywhere near this
// thing -- and everything in this file is the NARROW one, asked of the terrain
// per column, which is the same question the mesher answered when it decided
// where to put the sand. The two cannot disagree, because there is only one of
// them.
//
// GPU-FREE ON PURPOSE. It is in scene/ rather than beside the lake because
// nothing here needs a device, which is what lets tests/lily_shore_test.cpp
// drive the real thing over the real terrain in a second instead of through a
// four-gigabyte build.
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>

#include "voxelworld.h"

namespace v2 {

// Rim points round a floating disc. Eight is a 45-degree gap, which on a 0.45 m
// leaf is 34 cm of arc -- three voxels, so no shore feature wide enough to
// matter can hide between two of them.
inline constexpr int kShoreRimSamples = 8;

// IS THIS COLUMN UNDER WATER? The terrain's own answer, exact to the voxel
// column, which is the resolution the shore is actually drawn at.
inline bool wetColumnAt(const VoxelTerrain &t, TerrainMemo &memo, float x, float z) {
    const int ci = int(floorf(x / VOXEL_M)), cj = int(floorf(z / VOXEL_M));
    int wl = 0;
    return t.lakeColumn(ci, cj, memo, &wl);
}

// ---------------------------------------------------------------------------
// IS THE WHOLE DISC OVER WATER, AND IF NOT, WHICH WAY IS THE WATER?
//
// The middle and eight points round the rim. A lily pad spins freely, so the
// only footprint that is true at every heading is the circle round its longest
// half.
//
// THE NORMAL IS THE SUM OF THE RIM POINTS THAT ARE WET. For a shore that is
// locally a straight edge that points square out of the bank; in a corner or a
// narrow inlet, where a single "shore normal" does not really exist, it points
// at whatever open water there is, which is the useful answer rather than the
// tidy one. It comes back zero only when nothing on the rim is wet at all --
// something properly aground -- and the caller has a different job to do then.
// ---------------------------------------------------------------------------
inline bool discOnWater(const VoxelTerrain &t, TerrainMemo &memo, float x, float z, float r,
                        float *nx, float *nz) {
    bool clear = wetColumnAt(t, memo, x, z);
    float ax = 0.0f, az = 0.0f;
    for (int k = 0; k < kShoreRimSamples; ++k) {
        const float a = float(k) * (6.2831853f / float(kShoreRimSamples));
        const float dx = sinf(a), dz = cosf(a);
        if (wetColumnAt(t, memo, x + dx * r, z + dz * r)) {
            ax += dx;
            az += dz;
        } else {
            clear = false;
        }
    }
    const float l = sqrtf(ax * ax + az * az);
    if (nx) *nx = l > 1e-4f ? ax / l : 0.0f;
    if (nz) *nz = l > 1e-4f ? az / l : 0.0f;
    return clear;
}

// The nearest column that is under water, searched in rings. For getting
// something that should be floating back off dry land. Rings rather than a
// square, or the cost goes from O(r) to O(r^2) for no new answers -- the same
// shape WaterField::nearestWet uses, one lattice finer.
inline bool nearestWetColumn(const VoxelTerrain &t, TerrainMemo &memo, float x, float z,
                             float maxM, float *wx, float *wz) {
    const int i0 = int(floorf(x / VOXEL_M)), j0 = int(floorf(z / VOXEL_M));
    const int maxR = maxi(1, int(maxM / VOXEL_M));
    // Every fourth column: a lake is tens of metres and this is a recovery, so
    // 40 cm of precision is ample and it is sixteen times the columns otherwise.
    const int step = 4;
    for (int r = step; r <= maxR; r += step)
        for (int dj = -r; dj <= r; dj += step)
            for (int di = -r; di <= r; di += step) {
                if (abs(di) != r && abs(dj) != r) continue;   // the ring only
                const int i = i0 + di, j = j0 + dj;
                int wl = 0;
                if (!t.lakeColumn(i, j, memo, &wl)) continue;
                *wx = t.wx(i);
                *wz = t.wx(j);
                return true;
            }
    return false;
}

// What happened to a floating thing this step.
enum class ShoreHit {
    Moved,     // clear water ahead; it went where it was going
    Bounced,   // the bank is in front of it: reflected, and it did not move
    Aground,   // it is already over land and is being walked out
};

// A thing that floats and drifts. Position, heading, and the spin it carries.
struct Floater {
    float x = 0.0f, z = 0.0f;
    float mth = 0.0f;    // the drift heading: x is sin, z is cos
    float spin = 0.0f;   // the model's own turn, which a bounce nudges
};

// ---------------------------------------------------------------------------
// ONE STEP OF SOMETHING THAT FLOATS AND BOUNCES.
//
//   stepM      how far it would go this tick
//   r          the radius that has to stay over water
//   pushM      how far it walks out per tick when it is aground
//   spinKick   how much of a glancing bounce turns the model
//   spinMax    ...and the ceiling on the spin that can ever accumulate
//
// THE DESTINATION IS TESTED FIRST, because a thing that may go where it is
// headed is the common case even at a bank, and that answer settles it on its
// own -- nine terrain columns rather than eighteen.
//
// REFLECTING RATHER THAN CURLING IS THE WHOLE POINT. The old behaviour turned
// away at 2.6 rad/s, which is an eighth of a radian a frame: a pad could push
// most of a metre into the sand before it had turned, and a metre is inside one
// water-field cell, which is exactly why nothing ever caught it.
// ---------------------------------------------------------------------------
inline ShoreHit driftFloater(const VoxelTerrain &t, TerrainMemo &memo, Floater *f, float r,
                             float stepM, float pushM, float spinKick, float spinMax) {
    const float wantX = f->x + sinf(f->mth) * stepM;
    const float wantZ = f->z + cosf(f->mth) * stepM;
    float nx = 0.0f, nz = 0.0f;

    if (discOnWater(t, memo, wantX, wantZ, r, &nx, &nz)) {
        f->x = wantX;
        f->z = wantZ;
        return ShoreHit::Moved;
    }

    // -- ALREADY AGROUND, WHICH IS NOT A BOUNCE -----------------------------
    //
    // A lake can be re-cut under a leaf: an axe, a chunk streaming in, a field
    // rebuilt somewhere new. Every heading is refused in that state, so
    // bouncing would leave it sitting in the sand for ever -- it has to be
    // walked out. Same net as recoverFish, at the same rate of a slow drift.
    if (!discOnWater(t, memo, f->x, f->z, r, &nx, &nz)) {
        if (nx == 0.0f && nz == 0.0f) {
            float wx = 0.0f, wz = 0.0f;
            if (nearestWetColumn(t, memo, f->x, f->z, 32.0f, &wx, &wz)) {
                const float dx = wx - f->x, dz = wz - f->z;
                const float l = sqrtf(dx * dx + dz * dz);
                if (l > 1e-4f) {
                    nx = dx / l;
                    nz = dz / l;
                }
            }
        }
        if (nx != 0.0f || nz != 0.0f) {
            f->x += nx * pushM;
            f->z += nz * pushM;
            // ...and it leaves the way it was pushed, or the next tick walks it
            // straight back in.
            f->mth = atan2f(nx, nz);
        }
        return ShoreHit::Aground;
    }

    // -- THE BOUNCE ---------------------------------------------------------
    //
    // Clear where it is, not clear where it was going: it is touching the bank.
    // Reflect about the shore normal and do NOT take the step.
    //
    // A heading that already points at the water and is still refused means it
    // is in a corner -- facing the open water is the only move there that is
    // not straight back into something.
    float dx = sinf(f->mth), dz = cosf(f->mth);
    if (nx != 0.0f || nz != 0.0f) {
        const float dn = dx * nx + dz * nz;
        if (dn < 0.0f) {
            dx -= 2.0f * dn * nx;
            dz -= 2.0f * dn * nz;
        } else {
            dx = nx;
            dz = nz;
        }
    } else {
        dx = -dx;
        dz = -dz;
    }
    f->mth = atan2f(dx, dz);
    // AND A LITTLE TURN OUT OF IT. A leaf that glances off a bank and carries on
    // spinning at exactly its old rate reads as a sprite on a track. The kick is
    // the tangential part of the drift it just lost, clamped to the spin it is
    // allowed anyway -- character, never a new kind of motion.
    f->spin = clampf(f->spin + (dx * -nz + dz * nx) * spinKick, -spinMax, spinMax);
    return ShoreHit::Bounced;
}

}  // namespace v2
