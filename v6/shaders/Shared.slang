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
namespace v6
{
using float2 = Falcor::float2;
using float3 = Falcor::float3;
using float4 = Falcor::float4;
using uint2 = Falcor::uint2;
// int3 is only used by V6DdgiConsts, but it has to be aliased here for the same
// reason every other vector type is: without it cl.exe parses the member as a
// bare int, the struct comes out short, and the only thing that notices is the
// static_assert at the bottom of this file -- which is exactly its job.
using int3 = Falcor::int3;
using uint = uint32_t;
#endif

// The edge of a voxel, in metres. Duplicated from voxelworld.h rather than
// included, because that header is C++ and this one has to compile as Slang.
// A static_assert in world.h checks the two agree.
static const float kVoxelM = 0.1f;

// WHERE THE GROUND RAMPS LIVE IN THE MATERIAL TABLE.
//
// Duplicated from mat:: in voxelworld.h for the same reason kVoxelM is -- this
// header has to compile as Slang and cannot include a C++ one -- and checked
// against it by static_assert in world.h, because a silent disagreement here
// would paint the floor in tree bark.
//
// The mesher stores only the BOTTOM of a ramp. Which shade a voxel actually
// takes is decided per voxel on the device; see groundShade() in Trace.cs.slang
// for why that is where it belongs.
static const uint kGrass0 = 7;
static const uint kGrassCount = 6;
static const uint kSoil0 = 13;
static const uint kSoilCount = 4;
static const uint kLitter0 = 17;
static const uint kLitterCount = 3;

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
struct V6Camera
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
struct V6Sky
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
struct V6Material
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
struct V6Instance
{
    float3 tint;
    uint triOffset;
    uint kind; // one of KIND_*
    uint pad0, pad1, pad2;
};

// ---------------------------------------------------------------------------
// Everything the probe passes need to know about the volume.
//
// A SECOND COPY OF NUMBERS RTXGI ALREADY HAS, and deliberately so. The SDK
// keeps its own packed volume descriptor in a structured buffer laid out for
// its own shaders; reading that from Slang would mean reproducing
// DDGIVolumeDescGPUPacked field for field and tracking it across SDK versions.
// These are the eleven values v6's own shaders actually use, filled from the
// volume's public getters each frame, so the two can never disagree about a
// field neither of them names the same way.
//
// The 16-byte row rule at the top of this file applies here exactly as it does
// to V6Params -- every row below is filled.
// ---------------------------------------------------------------------------
struct V6DdgiConsts
{
    float3 origin;
    float maxRayDistance;

    int3 probeCounts;
    int numRays;

    float3 probeSpacing;
    float normalBias;

    // The per-frame random rotation applied to the spherical Fibonacci ray set.
    // See the note in ProbeTrace.cs.slang for why 192 fixed directions would
    // not be enough without it.
    float4 rayRotation;

    // How far a SCROLLING volume has renumbered its probes. A scrolling volume
    // moves without moving its probes: the grid indices shift and only the
    // plane that just came into range is relearned.
    int3 probeScroll;
    // Non-zero when relocation OR classification is on -- which is what makes
    // the first RTXGI_DDGI_NUM_FIXED_RAYS of every probe "fixed". See
    // probeRayDirection() in ProbeTrace.cs.slang; it is not the same question
    // as "is classification on" and conflating them mis-aims 160 rays.
    int useFixedRays;

    float2 irradianceTexels;  // x: including border, y: interior only
    float2 distanceTexels;

    // The atlas is stored with a high gamma because irradiance spans orders of
    // magnitude and a 10-bit unorm cannot hold that linearly.
    float irradianceGamma;
    float viewBias;
    float ddgiPad0, ddgiPad1;
};

// ---------------------------------------------------------------------------
// The per-launch constants.
// ---------------------------------------------------------------------------
struct V6Params
{
    V6Camera cam;

    // LAST FRAME'S CAMERA, and it exists for exactly one reason: a motion
    // vector. The world is static and only the camera moves, so where a surface
    // was on screen last frame is not something to store per surface -- it is
    // this frame's hit point projected through last frame's camera, exactly.
    // That is the whole of the motion vector pass, and it costs one dot product
    // per pixel because the primary hit is already in hand.
    V6Camera prevCam;

    V6Sky sky;

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

    // Whether gAccum is a FILM or a formality.
    //
    // It is a film whenever more than one sample will land in it -- the
    // accumulating renderer, or several samples per displayed frame. Under Ray
    // Reconstruction at one sample a frame it is neither read nor written, and
    // saying so here is worth a full-resolution RGBA32F read and write per
    // frame. Took the padding word this struct was already carrying, so the
    // layout is unchanged.
    int keepFilm;

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

    // PHASE B. Non-zero when the tracer should also write the demodulated
    // channels -- the diffuse and specular radiance kept apart, the emission
    // held out, and the albedo to divide the diffuse one by. Costs four
    // full-resolution stores per pixel, so it is off on the Ray Reconstruction
    // path, which wants the composited colour and its guides and nothing else.
    //
    // Took the padding word this struct was already carrying, so the layout --
    // and the static_assert below -- is unchanged.
    int demodulate;

    // PHASE A, the indirect half. Non-zero when the camera path is allowed to
    // stop early and read the irradiance probes instead of bouncing again.
    //
    // ddgiDepth is WHERE it stops, and it is the one number that decides what
    // this whole cache is worth. At 0 the probes would supply even the first
    // bounce, which throws away the contact shading that makes a voxel look
    // like a voxel -- the probes are 6 m apart and cannot know about a needle.
    // At 1 the primary hit still gets a real traced bounce and only what lies
    // BEYOND it comes from the cache, which is exactly the part that was
    // noisiest and the part the eye is least able to localise.
    int useDdgi;
    int ddgiDepth;
    float ddgiIntensity;
    float ddgiParamPad;

    // PHASE C, the auxiliary half: ambient occlusion, for NRD to denoise.
    //
    // AO IS THE ONE SIGNAL RAY RECONSTRUCTION DOES NOT TAKE. RR is handed
    // radiance and reconstructs radiance; it has no input for a scalar
    // visibility term and no way to return one. So AO is traced separately,
    // denoised separately by NRD, and multiplied in afterwards -- which is
    // exactly the "NRD only for signals DLSS does not touch" rule, and why no
    // buffer in this engine is ever denoised twice.
    //
    // aoRays is per pixel per frame and 1 is the intended value: NRD is a
    // temporal denoiser and would rather have one sample a frame for thirty
    // frames than thirty in one.
    int aoRays;
    float aoRadius;   // metres; beyond this nothing is considered occluding
    // How much of the AO term to apply, 0..1. A control rather than a
    // constant because AO is the one effect here with no correct value --
    // it is an approximation to visibility the probes cannot resolve.
    float aoStrength;
    float aoPad0;

    // -- volumetric fog ------------------------------------------------------
    //
    // 0 falls back to no fog at all rather than to the old analytic model: the
    // analytic version is gone, and leaving a second implementation behind to
    // disagree with this one in edge cases is worse than the switch being
    // honest about what it does.
    //
    // The near and far limits are here because the tracer has to map a pixel's
    // DISTANCE into the same exponential slice distribution the grid was built
    // with, and getting that mapping out of step with volfog.h would show up as
    // fog at the wrong depth rather than as an error.
    int volFog;
    float fogNear;
    float fogFar;
    int volFogPad;
};


#ifdef HOST_CODE
static_assert(sizeof(V6Camera) == 64, "V6Camera must fill whole 16-byte rows");
static_assert(sizeof(V6Sky) == 160, "V6Sky must fill whole 16-byte rows");
static_assert(sizeof(V6Material) == 32, "V6Material must fill whole 16-byte rows");
static_assert(sizeof(V6Instance) == 32, "V6Instance must fill whole 16-byte rows");
static_assert(sizeof(V6Params) == 416, "V6Params must fill whole 16-byte rows");
static_assert(sizeof(V6DdgiConsts) == 112, "V6DdgiConsts must fill whole 16-byte rows");
} // namespace v6
#endif
