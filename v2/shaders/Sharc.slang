// ---------------------------------------------------------------------------
// Sharc.slang -- v2's side of the SHaRC spatial hash radiance cache.
//
// WHAT SHaRC IS, AND WHY IT SITS BESIDE DDGI RATHER THAN REPLACING IT. Both
// answer the same question -- "how much light arrives at this point" -- and both
// answer it from a structure that remembers across frames. What differs is the
// structure. DDGI is a fixed grid of probes that has to be scrolled along
// underneath an endless world, and it is D3D12 only because RTXGI's probe
// blending is a D3D12 library. SHaRC is a hash map keyed on quantised world
// position, so it has no extent to scroll, no probes to relocate out of trunks,
// and it is pure shader code that runs on both backends.
//
// The cost is that a hash map is sparse and stochastic where a probe grid is
// dense and interpolated: SHaRC is exact where samples have landed and empty
// where they have not, so it needs its update pass to keep finding the places
// the camera is about to look at.
//
// THE TWO CANNOT BOTH BE ON. They return the same quantity in the same units,
// and the tracer adds whichever it is given -- so running both would count the
// indirect light twice with nothing to notice. gParams.giMode is one value, not
// two flags, and that is why.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"
#include "Vecmath.slang"
#include "VoxelKey.slang"

// ---------------------------------------------------------------------------
// The SDK's compile-time configuration, set BEFORE its headers are included.
//
// SHARC_ENABLE_64_BIT_ATOMICS 0 -- and this is a portability choice, not a
// performance one. The 64-bit path needs uint64 InterlockedCompareExchange,
// which on D3D12 means shader model 6.6 and on Vulkan means the
// shaderBufferInt64Atomics feature actually being enabled by whoever created
// the device -- and slang-gfx creates v2's Vulkan device, not v2. The 32-bit
// path trades that for a per-entry lock word, which costs one more buffer and
// nothing else, and it runs identically on both backends.
// ---------------------------------------------------------------------------
#define SHARC_ENABLE_64_BIT_ATOMICS 0

// One radiance value is accumulated into 32-bit integers per channel, so the
// scale sets where that overflows. 1e3 is the SDK's suggested starting point;
// the sun in this world is a five-figure radiance, but what reaches the cache
// is radiance leaving a 0.19-albedo surface, which is orders of magnitude below
// the disk itself.
static const float kSharcRadianceScale = 1e3f;

#include "sharc/SharcCommon.h"

// The buffers, bound by every pass that touches the cache. Declared here so the
// update pass, the resolve pass and the tracer cannot disagree about their
// order or their types.
//
// Bound unconditionally, like the probe atlases, because a declared resource
// with nothing behind it fails the dispatch rather than the branch.
RWStructuredBuffer<uint64_t> gSharcHashEntries;
RWStructuredBuffer<uint> gSharcLock;
RWStructuredBuffer<SharcAccumulationData> gSharcAccum;
RWStructuredBuffer<SharcPackedData> gSharcResolved;

// ---------------------------------------------------------------------------
// FOUR COUNTERS, so a claim about the cache can be checked rather than argued.
//
//   0  entries live in the map after the resolve
//   1  queries the camera path attempted
//   2  queries that found a populated entry
//   3  inserts that failed because the bucket was full
//
// Occupancy answers whether the map is big enough; the hit rate answers the
// question this cache actually exists to answer -- how much of the frame it is
// carrying. Both were guesses until now, and one of them (occupancy) had
// already been guessed wrong once.
//
// Behind a runtime flag rather than a define: an atomic in the query path is
// real cost and nobody should pay it to render a frame, but a second compiled
// variant of the tracer costs a startup and a way for the two to disagree.
// ---------------------------------------------------------------------------
RWStructuredBuffer<uint> gSharcStats;

cbuffer gSharcCB
{
    float3 gSharcCameraPos;
    float gSharcSceneScale;

    float3 gSharcCameraPosPrev;
    uint gSharcCapacity;

    uint gSharcFrame;
    uint gSharcAccumFrames;   // temporal window, in frames
    uint gSharcStaleFrames;   // eviction threshold
    uint gSharcStatsOn;       // count queries and hits -- see gSharcStats
};

// Everything the SDK needs, assembled once. A function rather than a global so
// each pass builds it from the same code and none can leave a field unset --
// an uninitialised capacity is a hash map that indexes anywhere.
SharcParameters sharcParams()
{
    HashGridParameters grid;
    grid.cameraPosition = gSharcCameraPos;
    grid.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    grid.sceneScale = gSharcSceneScale;
    grid.levelBias = SHARC_GRID_LEVEL_BIAS;

    HashMapData map;
    map.capacity = gSharcCapacity;
    map.hashEntriesBuffer = gSharcHashEntries;
    map.lockBuffer = gSharcLock;

    SharcParameters p;
    p.gridParameters = grid;
    p.hashMapData = map;
    p.radianceScale = kSharcRadianceScale;
    // A single path through a gap in the canopy can carry thousands of times
    // the mean, and unlike the film the cache has no later average to hide it
    // in -- one spike becomes a lit voxel that stays lit.
    p.enableAntiFireflyFilter = true;
    p.accumulationBuffer = gSharcAccum;
    p.resolvedBuffer = gSharcResolved;
    return p;
}

// The cache is keyed on quantised world position, so a hit is only usable once
// the path segment that found it is longer than the voxel it would be filed
// under. Shorter than that and neighbouring shading points land in the same
// cell and the cache hands them all one answer -- which is exactly the blocky
// look SHaRC gets blamed for.
bool sharcSegmentLongEnough(float3 worldPos, float segmentLength)
{
    HashGridParameters grid;
    grid.cameraPosition = gSharcCameraPos;
    grid.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    grid.sceneScale = gSharcSceneScale;
    grid.levelBias = SHARC_GRID_LEVEL_BIAS;
    return segmentLength > HashGridGetVoxelSize(HashGridGetLevel(worldPos, grid), grid);
}

// ===========================================================================
// THE VOXEL-FACE KEY, over the SDK's hash map.
//
// The key itself is in VoxelKey.slang -- it is shared with the world-space
// ReSTIR reservoirs and the neural cache, which have to agree with this one
// about which face is which or the three caches disagree about the world.
// That file also carries the argument for why an exact key is possible here
// and what the SDK's quantised one costs.
//
// THE LAYOUT CONSTANTS ARE REPEATED THERE, so this is where they are checked
// against the SDK we actually compile with. If RTXGI ever changes a field
// width, this is the line that stops the build rather than silently filing
// every entry under a corrupt key.
// ===========================================================================
#if (V2_VOXKEY_POS_BITS != HASH_GRID_POSITION_BIT_NUM) || (V2_VOXKEY_LEVEL_BITS != HASH_GRID_LEVEL_BIT_NUM)
#error "VoxelKey.slang field widths no longer match RTXGI HashGridCommon.h"
#endif

// The two map operations, in v2's key. HashMapInsert and HashMapFind take a
// key and know nothing about how it was made, so this is the entire injection
// -- the SDK's own HashMapInsertEntry/HashMapFindEntry are simply not called.
HashGridIndex voxelFaceInsert(in HashMapData map, float3 p, float3 n)
{
    HashGridIndex cacheIndex = HASH_GRID_INVALID_CACHE_INDEX;
    if (!HashMapInsert(map, voxelFaceKey(p, n), cacheIndex))
    {
        // The bucket was full. The entry is simply not recorded, which is
        // silent -- so it is counted.
        if (gSharcStatsOn != 0) InterlockedAdd(gSharcStats[3], 1u);
        return HASH_GRID_INVALID_CACHE_INDEX;
    }
    return cacheIndex;
}

HashGridIndex voxelFaceFind(in HashMapData map, float3 p, float3 n)
{
    HashGridIndex cacheIndex = HASH_GRID_INVALID_CACHE_INDEX;
    uint collisions;
    const bool found = HashMapFind(map, voxelFaceKey(p, n), cacheIndex, collisions);
    if (gSharcStatsOn != 0)
    {
        InterlockedAdd(gSharcStats[1], 1u);
        if (found) InterlockedAdd(gSharcStats[2], 1u);
    }
    return found ? cacheIndex : HASH_GRID_INVALID_CACHE_INDEX;
}

// ---------------------------------------------------------------------------
// A HIT IS USABLE ONCE THE SEGMENT THAT FOUND IT IS LONGER THAN ONE VOXEL.
//
// This does NOT replace sharcSegmentLongEnough -- it is applied with it. An
// entry is a face of a block, so a segment shorter than the block is filing a
// shading point under a face it has barely left; that is what this rules out,
// and it is a constant because a cell is now always one voxel.
//
// The old distance-dependent test stays because it turns out to answer a
// different question -- see the note at the call site in Trace.cs.slang, where
// dropping it cost 15% RMSE against the reference.
// ---------------------------------------------------------------------------
bool voxelSegmentLongEnough(float segmentLength)
{
    return segmentLength > kVoxelM;
}

// ===========================================================================
// THE TWO SDK ENTRY POINTS, MIRRORED.
//
// SharcUpdateHit and SharcGetCachedRadiance are the only places in the SDK that
// turn a hit into a cache index, and each does it on ONE line:
//
//     cacheIndex = HashMapInsertEntry(map, positionWorld, normalWorld, grid);
//     cacheIndex = HashMapFindEntry  (map, positionWorld, normalWorld, grid);
//
// Everything else in them -- the anti-firefly weighting, the propagation of a
// vertex's radiance back down the path, the resampling early-out -- is logic we
// want unchanged. There is no way to substitute just that line: the headers
// carry no include guards, so we cannot include HashGridCommon.h first and
// redefine the entry points between the two files, and a function-like macro
// would rename the SDK's own DEFINITION along with its call site.
//
// So these are faithful copies of SharcCommon.h 281-363 (SHaRC 1.6.5.0) with
// the key line replaced and nothing else touched. If the SDK is updated, diff
// those two functions against these -- that is the whole maintenance burden,
// and it is why the copies are kept adjacent and named after their originals.
// ===========================================================================

#if defined(SHARC_UPDATE) && SHARC_UPDATE
// Mirror of SharcUpdateHit. Delta: cacheIndex comes from voxelFaceInsert.
bool sharcVoxelUpdateHit(in SharcParameters sharcParameters, inout SharcState sharcState, SharcHitData sharcHitData, float3 directLighting, float random)
{
    bool continueTracing = true;

    HashGridIndex cacheIndex = voxelFaceInsert(sharcParameters.hashMapData, sharcHitData.positionWorld, sharcHitData.normalWorld);

    float3 sharcRadiance = directLighting;
    float3 materialDemodulation = float3(1.0f, 1.0f, 1.0f);
#if SHARC_MATERIAL_DEMODULATION
    materialDemodulation = sharcHitData.materialDemodulation;
#endif // SHARC_MATERIAL_DEMODULATION

#if SHARC_ENABLE_CACHE_RESAMPLING
    uint resamplingDepth = uint(round(lerp(SHARC_RESAMPLING_DEPTH_MIN, SHARC_PROPAGATION_DEPTH - 1, random)));
    if (resamplingDepth <= sharcState.pathLength)
    {
        SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, cacheIndex);
        if (voxelData.accumulatedSampleNum > SHARC_SAMPLE_NUM_THRESHOLD)
        {
            sharcRadiance = voxelData.accumulatedRadiance;
            sharcRadiance *= materialDemodulation;
            continueTracing = false;
        }
    }
#endif // SHARC_ENABLE_CACHE_RESAMPLING

    if (continueTracing)
        SharcAddVoxelData(sharcParameters, cacheIndex, directLighting / materialDemodulation, float3(1.0f, 1.0f, 1.0f), 1);

#if SHARC_SEPARATE_EMISSIVE
    sharcRadiance += sharcHitData.emissive;
#endif // SHARC_SEPARATE_EMISSIVE

    uint i;
    for (i = 0; i < sharcState.pathLength; ++i)
        SharcAddVoxelData(sharcParameters, sharcState.cacheIndices[i], sharcRadiance, sharcState.sampleWeights[i], 0);

    for (i = sharcState.pathLength; i > 0; --i)
    {
        sharcState.cacheIndices[i] = sharcState.cacheIndices[i - 1];
        sharcState.sampleWeights[i] = sharcState.sampleWeights[i - 1];
    }

    sharcState.cacheIndices[0] = cacheIndex;
    sharcState.sampleWeights[0] = SharcSampleWeight(1.0f / materialDemodulation);
    sharcState.pathLength = min(++sharcState.pathLength, SHARC_PROPAGATION_DEPTH - 1);

    return continueTracing;
}
#endif // SHARC_UPDATE

// Mirror of SharcGetCachedRadiance. Delta: cacheIndex comes from voxelFaceFind.
bool sharcVoxelGetCachedRadiance(in SharcParameters sharcParameters, in SharcHitData sharcHitData, out float3 radiance, bool debug)
{
    radiance = float3(0.0f, 0.0f, 0.0f);
    const uint sampleThreshold = debug ? 0 : SHARC_SAMPLE_NUM_THRESHOLD;

    HashGridIndex cacheIndex = voxelFaceFind(sharcParameters.hashMapData, sharcHitData.positionWorld, sharcHitData.normalWorld);
    if (cacheIndex == HASH_GRID_INVALID_CACHE_INDEX)
        return false;

    SharcVoxelData voxelData = SharcGetVoxelData(sharcParameters.resolvedBuffer, cacheIndex);
    if (voxelData.accumulatedSampleNum > sampleThreshold)
    {
        radiance = voxelData.accumulatedRadiance;
#if SHARC_MATERIAL_DEMODULATION
        radiance *= sharcHitData.materialDemodulation;
#endif // SHARC_MATERIAL_DEMODULATION
#if SHARC_SEPARATE_EMISSIVE
        radiance += sharcHitData.emissive;
#endif // SHARC_SEPARATE_EMISSIVE

        return true;
    }

    return false;
}
