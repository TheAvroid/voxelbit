// ---------------------------------------------------------------------------
// NrcSdk.slang -- v2's side of NVIDIA's Neural Radiance Cache (RTXGI 2.x).
//
// The host half is gpu/nrcsdk.h. This is the shader half: the buffers the path
// tracer appends its records to, the constants the library fills in, and the
// small amount of translation between v2's Hit and the surface description the
// cache wants.
//
// NOT TO BE CONFUSED WITH Nrc.slang, which is v2's OWN network -- cooperative
// vectors, a voxel-identity encoding, and weights that transfer between worlds.
// The two are alternatives and only one may be on at a time; see the note on
// double counting at the bottom of this file.
//
// ---------------------------------------------------------------------------
// THE BUFFERS ARE DECLARED HERE RATHER THAN INSIDE NrcBuffers, and the SDK has
// a supported route for exactly that.
//
// Nrc.hlsli would normally put its five buffers in a struct and pass it around
// inside NrcContext. Slang can do that, but v2 binds through Falcor, and a
// struct of resources is the one shape Falcor's reflection makes awkward. The
// header anticipates this: define NRC_USE_CUSTOM_BUFFER_ACCESSORS and supply
// the five macros, and every access goes through names of our choosing instead.
//
// The macros must be defined BEFORE the include, which is why the declarations
// below sit above it and are forward-declared rather than written where they
// would naturally go.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"

#define ENABLE_NRC 1
#define NRC_USE_CUSTOM_BUFFER_ACCESSORS 1

// WHICH OF THE TWO PASSES THIS IS, and it is compile time on the SDK's side
// too -- g_nrcMode is a static const, so NrcIsUpdateMode() folds away and the
// two variants share no branches at all.
//
// The same shape v2 already uses for SHaRC: one source file, two programs,
// because the update pass has to shade exactly the way the query pass shades
// or the cache is trained on a different renderer from the one it serves.
#ifdef V2_NRCSDK_UPDATE
#define NRC_UPDATE 1
#else
#define NRC_QUERY 1
#endif

#define NRC_BUFFER_QUERY_PATH_INFO gNrcQueryPathInfo
#define NRC_BUFFER_TRAINING_PATH_INFO gNrcTrainingPathInfo
#define NRC_BUFFER_TRAINING_PATH_VERTICES gNrcTrainingPathVertices
#define NRC_BUFFER_QUERY_RADIANCE_PARAMS gNrcQueryRadianceParams
#define NRC_BUFFER_QUERY_COUNTERS_DATA gNrcCounters

#include "nrcsdk/Nrc.hlsli"

// The five records the library reads after the path tracer has run. Their
// contents are the SDK's business entirely -- v2 never reads them back.
RWStructuredBuffer<NrcPackedQueryPathInfo> gNrcQueryPathInfo;
RWStructuredBuffer<NrcPackedTrainingPathInfo> gNrcTrainingPathInfo;
RWStructuredBuffer<NrcPackedPathVertex> gNrcTrainingPathVertices;
RWStructuredBuffer<NrcRadianceParams> gNrcQueryRadianceParams;
RWStructuredBuffer<uint> gNrcCounters;

// Filled by Context::PopulateShaderConstants on the host every frame. It
// carries the scene bounds, the resolutions, the termination thresholds and the
// mode -- which is what NrcIsUpdateMode() below is reading.
cbuffer gNrcCB { NrcConstants gNrcConstants; }

// The translation from v2's Hit to NrcSurfaceAttributes lives in
// Trace.cs.slang, not here.
//
// It has to: `Hit` and MAT_DIELECTRIC are the TRACER's types, and this header is
// also included by NrcSdkResolve.cs.slang, which has no path tracer in it and
// no reason to know what a Hit is. Putting the helper here compiled perfectly
// well from the tracer and then failed to link the resolve pass -- with
// "undefined identifier 'Hit'", from a file that never mentions one.
