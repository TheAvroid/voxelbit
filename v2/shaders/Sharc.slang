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

cbuffer gSharcCB
{
    float3 gSharcCameraPos;
    float gSharcSceneScale;

    float3 gSharcCameraPosPrev;
    uint gSharcCapacity;

    uint gSharcFrame;
    uint gSharcAccumFrames;   // temporal window, in frames
    uint gSharcStaleFrames;   // eviction threshold
    uint gSharcPad;
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
