// ---------------------------------------------------------------------------
// VoxelKey.slang -- the identity of a voxel FACE, and the one definition of it.
//
// Three separate caches in this engine want to remember something about a
// surface and find it again later: the SHaRC radiance cache (Sharc.slang), the
// world-space ReSTIR reservoirs (RestirWorld.slang) and the neural radiance
// cache (Nrc.slang). Every one of them is a hash map, and every one of them
// needs an answer to the same question -- WHICH PIECE OF THE WORLD IS THIS?
//
// In a triangle renderer that question has no exact answer, which is why the
// NVIDIA caches all quantise a world position onto a grid whose cell size grows
// with camera distance. It is the right choice when the geometry could be
// anything. It costs a quantisation error, a level that changes as you walk,
// and cells that straddle two surfaces and average them together.
//
// This world is an integer grid of 0.1 m voxels and every face is one of six
// axis-aligned planes, so the exact answer is available and cheap: the integer
// voxel coordinate plus a face index in 0..5. No quantisation, no level, no
// distance dependence. The entry is a face of a block, and it means that for as
// long as the block exists -- which is also why a key like this can be written
// to disk or generated ahead of the player, and a quantised one cannot.
//
// ---------------------------------------------------------------------------
// THE FIELD LAYOUT IS RTXGI's HashGridCommon.h, DELIBERATELY.
//
// 17 bits per axis, 10 of level, 3 of normal. Not because we need those widths,
// but because SHaRC's own code reads keys back out of the buffer -- the
// eviction path and the linear-probe recovery in SharcResolveEntry both do --
// and matching the layout means none of it has to change. The constants are
// repeated here rather than included so that ReSTIR and the NRC do not have to
// pull in the SHaRC SDK to ask what a face is; Sharc.slang checks they still
// agree.
//
// TWO FIELDS ARE REPURPOSED, AND BOTH CHOICES ARE LOAD-BEARING:
//
//   LEVEL IS PINNED TO 0, which is what makes SHARC_BLEND_ADJACENT_LEVELS
//   harmless. We cannot switch that off -- it is a bare #define inside the
//   SDK's constants block, not an #ifndef we can pre-empt -- and left alone it
//   would take one of our keys, halve the coordinate and look the result up.
//   But SharcGetAdjacentLevelHashKey always emits level 1 or higher, and no key
//   we ever insert carries a level but 0, so the lookup is guaranteed to miss.
//   The blend is inert by construction rather than by an #ifdef we are not
//   allowed.
//
//   THE FACE IS STORED AS face+1, so the field runs 1..6 and never 0. An
//   all-zero key is the invalid-key sentinel in every map here, and without the
//   bias the +Y face of the voxel at the world origin would hash to exactly
//   that and be silently dropped as an empty slot.
//
// AND THE 17-BIT WRAP IS SAFE. 2^17 voxels is 13107.2 m, so two keys alias only
// if their voxels are that far apart on an axis -- while the resident world is
// a 12-chunk radius, 307 m. Two live entries cannot reach each other. The world
// runs to kilometres; what matters is that the part of it in memory at any one
// instant does not.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"

// PREPROCESSOR DEFINES, not static consts, and the reason is one line in
// Sharc.slang: it checks these against RTXGI's own field widths with an #if,
// and #if cannot see a `static const`. A silent mismatch there would file every
// entry under a corrupt key, so the check has to be one the compiler can make.
#define V4_VOXKEY_POS_BITS 17
#define V4_VOXKEY_LEVEL_BITS 10

static const uint kVoxKeyPosBits = uint(V4_VOXKEY_POS_BITS);
static const uint kVoxKeyPosMask = (1u << kVoxKeyPosBits) - 1u;
static const uint kVoxKeyLevelBits = uint(V4_VOXKEY_LEVEL_BITS);
static const uint64_t kVoxKeyInvalid = 0ull;

// The six face directions, in the order faceNormal() in Trace.cs.slang emits
// them: +Y, -Y, +X, -X, +Z, -Z.
//
// READ BACK FROM THE WORLD NORMAL, because that is the space the key lives in.
// An instance's quarter turn about Y sends a +X face to +Z, and every pass that
// touches the key -- the one that inserts and the one that looks up -- has to
// agree on which face it is looking at.
//
// WHY AN INDEX AND NOT THE SDK'S THREE SIGN BITS. HashGridComputeSpatialHash
// stores the octant of the normal:
//
//     bits = (n.x >= 0 ? 0 : 1) + (n.y >= 0 ? 0 : 2) + (n.z >= 0 ? 0 : 4)
//
// For an axis-aligned face the two zero components are both >= 0, so +X, +Y and
// +Z ALL PRODUCE 0. The top of a voxel and its two positive sides share a key.
// That is light leaking between faces of one block, by construction, and it is
// not a tuning problem -- six faces need six values.
uint voxelFaceIndex(float3 n)
{
    const float3 a = abs(n);
    if (a.y >= a.x && a.y >= a.z)
        return (n.y >= 0.0f) ? 0u : 1u;
    if (a.x >= a.z)
        return (n.x >= 0.0f) ? 2u : 3u;
    return (n.z >= 0.0f) ? 4u : 5u;
}

// HALF A VOXEL ALONG THE NORMAL, so the sample lands INSIDE the voxel the face
// belongs to rather than on its boundary, where the floor of a coordinate is a
// coin toss and every edge in the frame would flicker between two cells as the
// camera moved.
//
// This is deliberately the same expression groundShade() uses in Trace.cs.slang
// to pick a voxel's colour: the shade of a block and the cache entries for its
// faces have to name the same block, or a cached entry lights a voxel it is not
// on.
int3 voxelCoordFromHit(float3 p, float3 n)
{
    return int3(floor((p - n * (0.5f * kVoxelM)) / kVoxelM));
}

uint64_t voxelFaceKey(float3 p, float3 n)
{
    const int3 v = voxelCoordFromHit(p, n);
    const uint face = voxelFaceIndex(n) + 1u; // 1..6, never the invalid key

    uint64_t key = ((uint64_t(uint(v.x)) & kVoxKeyPosMask) << (kVoxKeyPosBits * 0)) |
                   ((uint64_t(uint(v.y)) & kVoxKeyPosMask) << (kVoxKeyPosBits * 1)) |
                   ((uint64_t(uint(v.z)) & kVoxKeyPosMask) << (kVoxKeyPosBits * 2));
    // The level field stays 0 -- see the note above, that is what neuters
    // SHARC_BLEND_ADJACENT_LEVELS.
    key |= uint64_t(face) << (kVoxKeyPosBits * 3 + kVoxKeyLevelBits);
    return key;
}

// Jenkins 32-bit avalanche, folded over both halves. The same mix RTXGI uses,
// repeated here for the same reason the constants are: a map that is not
// SHaRC's should not have to include SHaRC's headers to hash a key.
uint voxelKeyHashJenkins(uint a)
{
    a = (a + 0x7ed55d16u) + (a << 12);
    a = (a ^ 0xc761c23cu) ^ (a >> 19);
    a = (a + 0x165667b1u) + (a << 5);
    a = (a + 0xd3a2646cu) ^ (a << 9);
    a = (a + 0xfd7046c5u) + (a << 3);
    a = (a ^ 0xb55a4f09u) ^ (a >> 16);
    return a;
}

uint voxelKeyHash(uint64_t key)
{
    return voxelKeyHashJenkins(uint(key & 0xFFFFFFFFull)) ^
           voxelKeyHashJenkins(uint((key >> 32) & 0xFFFFFFFFull));
}
