// ---------------------------------------------------------------------------
// Shared.slang -- the contract between the host and the GPU.
//
// Compiled twice: once by Slang as part of the tracer, once by cl.exe as a C++
// header. That is what the HOST_CODE switch below is for, and it is the same
// trick Falcor uses for its own shared types.
//
// THE ONLY HARD RULE IN THIS FILE IS THE PACKING, and it is worth stating
// because breaking it does not fail to compile -- it silently reads the wrong
// field and the picture goes wrong somewhere else entirely.
//
// A constant buffer lays its members out in 16-byte rows, and a member may not
// straddle a row boundary. C++ has no such rule: it packs on natural alignment,
// so a float3 followed by another float3 sits at offset 12 in C++ and at offset
// 16 on the GPU. Every struct here is therefore written so that EVERY 16-BYTE
// ROW IS EXACTLY FILLED -- a float3 is always followed by a float, and anything
// that would need an array is a float4 with an unused w. Where a field exists
// only to fill a row it is named pad and does nothing else.
//
// The static_asserts at the bottom are the enforcement. They check the sizes a
// correct layout produces, so a member added in the middle without its padding
// breaks the build here rather than the render later.
// ---------------------------------------------------------------------------
#pragma once

#if defined(__cplusplus)
#define HOST_CODE 1
#endif

#ifdef HOST_CODE
#include "Utils/Math/Vector.h"
namespace v4
{
using float2 = Falcor::float2;
using float3 = Falcor::float3;
using float4 = Falcor::float4;
using uint2 = Falcor::uint2;
using uint = uint32_t;
#endif

// The edge of a voxel, in metres. Duplicated from voxelworld.h rather than
// included, because that header is C++ and this one has to compile as Slang.
// A static_assert in world.h checks the two agree.
static const float kVoxelM = 0.1f;

// The sun subtends about 0.53 degrees; this is the cosine of its half-angle.
// Also duplicated, also asserted.
static const float kSunCosThetaMax = 0.99998932f;

// What a surface is, which is all the shading needs to know beyond its
// material. The terrain and the rocks are the same thing to the shader; a tree
// differs only in taking a per-instance tint, and water is its own BSDF.
static const uint KIND_TERRAIN = 0;
static const uint KIND_WATER = 1;
static const uint KIND_TREE = 2;

// ---------------------------------------------------------------------------
// The camera, as the tracer wants it: a basis and two half-extents, with none
// of the trigonometry left to do per pixel.
// ---------------------------------------------------------------------------
struct V4Camera
{
    float3 pos;
    float halfW;
    float3 u; // right
    float halfH;
    float3 v; // up
    float aperture;
    float3 w; // forward, unit
    float focusDist;
};

// ---------------------------------------------------------------------------
// Preetham's daylight model, already fitted.
//
// The fit is tan(), pow() and a dozen polynomial evaluations, and it depends
// only on the sun angle and the turbidity -- not on the ray. Doing it per ray
// would be several hundred wasted instructions on every escaping ray in the
// scene, so it happens once a frame on the host and only the coefficients cross
// the bus.
//
// A, B, C, D, E and the two normalisers are per-channel in xyY, so they are
// float4 with an unused w rather than float[3]: an array in a constant buffer
// has a 16-byte STRIDE, which would make a float[3] occupy 48 bytes on the GPU
// and 12 in C++.
// ---------------------------------------------------------------------------
struct V4Sky
{
    float3 sunDir;
    float skyScale;
    float3 sunRadiance; // constant across the disk
    float sunPad;
    float3 groundAlbedo;
    float groundPad;
    float4 A, B, C, D, E;
    float4 zenith; // Y in cd/m^2, then x and y
    float4 normF;
};

// ---------------------------------------------------------------------------
// Everything the BSDF asks a surface for, per material id. One of these per
// entry in the palette; the whole table is a few kilobytes and is uploaded once.
// ---------------------------------------------------------------------------
struct V4Material
{
    float3 albedo;
    float roughness;
    float specular;    // F0 at normal incidence
    float translucency; // fraction of the diffuse lobe that passes through
    float pad0, pad1;
};

// ---------------------------------------------------------------------------
// One per instance in the acceleration structure, indexed by InstanceID().
//
// This is the whole of what the shader binding table used to carry, and it
// collapsed to a structured buffer the moment the pipeline went inline: there
// are no hit groups to bind, so "which triangle array does this instance read"
// is just a number the instance carries.
//
// triOffset is in UINT16 UNITS, not bytes -- it indexes the packed-tri pool
// the same way a primitive index does, so the shader adds the two and is done.
// ---------------------------------------------------------------------------
struct V4Instance
{
    float3 tint;
    uint triOffset;
    uint kind; // one of KIND_*
    uint pad0, pad1, pad2;
};

// ---------------------------------------------------------------------------
// The per-launch constants.
// ---------------------------------------------------------------------------
struct V4Params
{
    V4Camera cam;

    // LAST FRAME'S CAMERA, and it exists for exactly one reason: a motion
    // vector. The world is static and only the camera moves, so where a surface
    // was on screen last frame is not something to store per surface -- it is
    // this frame's hit point projected through last frame's camera, exactly.
    // That is the whole of the motion vector pass, and it costs one dot product
    // per pixel because the primary hit is already in hand.
    V4Camera prevCam;

    V4Sky sky;

    uint2 dim;

    // TWO COUNTERS, AND THEY MEAN DIFFERENT THINGS.
    //
    // `frame` is the ACCUMULATION index: how many samples are already in the
    // film for this camera. It returns to zero the instant the camera moves,
    // because every sample in the film was drawn for a view that no longer
    // exists.
    //
    // `tick` only ever increases. It seeds the sampler and picks the jitter,
    // and it must NOT reset with the accumulation -- successive frames have to
    // draw different samples, and seeding from an index that keeps returning to
    // zero redraws the same one over and over.
    uint frame;
    uint tick;

    uint seed;
    uint maxAccum; // cap on how many samples the film averages, 0 for none
    int maxDepth;
    int rrStart;

    float clampIndirect; // firefly ceiling on indirect light, 0 = off
    float fogDensity;    // extinction per metre at y = 0
    float fogHeight;     // e-folding height of the haze, metres
    float paramsPad;

    float2 jitter; // sub-pixel offset in pixels, shared by the whole frame
    int shadowRays;
    int shadowRayDepth;

    // Non-zero when the frame is being handed to the denoiser rather than
    // accumulated. It does not change a single ray -- it only says whether the
    // guide buffers below are worth writing, and whether the film should
    // accumulate or start clean every frame.
    int writeGuides;
    // The near and far planes the guide depth is written against. Only DLSS
    // ever reads it, and only to tell one surface from another across a frame,
    // so the range is chosen to span the world rather than to be precise.
    float depthNear, depthFar;
    float guidePad;
};

#ifdef HOST_CODE
static_assert(sizeof(V4Camera) == 64, "V4Camera must fill whole 16-byte rows");
static_assert(sizeof(V4Sky) == 160, "V4Sky must fill whole 16-byte rows");
static_assert(sizeof(V4Material) == 32, "V4Material must fill whole 16-byte rows");
static_assert(sizeof(V4Instance) == 32, "V4Instance must fill whole 16-byte rows");
static_assert(sizeof(V4Params) == 368, "V4Params must fill whole 16-byte rows");
} // namespace v4
#endif
