// ---------------------------------------------------------------------------
// Store.slang -- THE SEAM. Everything the renderer knows about geometry.
//
// This engine has no world. It has a LIGHTING STACK -- path tracing, ReSTIR,
// SHaRC, NRC, DDGI, volumetric fog, clouds, atmosphere, post and DLSS Ray
// Reconstruction -- and a single pair of functions through which every one of
// those asks "what is in front of this ray". A voxel store plugs in here and
// nowhere else.
//
// THE WHOLE INTERFACE IS TWO FUNCTIONS. A backend supplies:
//
//     VoxelHit storeTrace(float3 o, float3 d, float tMax)
//     bool     storeOccluded(float3 o, float3 d, float tMax)
//
// and a C++ side that binds whatever resources those need (see gpu/store.h).
// There is no third entry point, and adding one should feel like a decision
// rather than a convenience: the reason this engine can swap its geometry
// representation at all is that the lighting never learned what geometry is.
//
// ---------------------------------------------------------------------------
// `t` IS IN METRES, AND THAT IS LOAD-BEARING.
//
// It has to be directly comparable with a hardware RayQuery's t, because a
// backend is allowed to BE a RayQuery -- an AABB BLAS over bricks, say, with
// the march running only inside a hit brick. A backend that works in voxel
// units and forgets to scale back produces a world whose shadows are the right
// shape and the wrong length, which reads as a lighting bug and is not one.
//
// ---------------------------------------------------------------------------
// WHY VoxelHit CARRIES ijk.
//
// This engine has no material textures: surface variation is a palette lookup
// plus a hash of the voxel coordinate. So a backend must be able to name the
// voxel it hit as an integer, not merely the point where the ray landed --
// hashing a float position instead gives a surface that swims under the camera.
// The same key feeds SHaRC and NRC when their voxel-key paths are enabled.
//
// ---------------------------------------------------------------------------
// SELECTING A BACKEND. TPL_STORE_IMPL is a quoted path to the implementation,
// set from gpu/tracer.h as a shader define. It defaults to the null store,
// which misses every ray -- so an engine with no backend at all still renders
// its sky, its sun, its clouds and its fog, and every pass downstream of the
// trace goes on running with nothing to shade. That is the blank slate, and it
// is a legitimate thing to look at: if the sky is wrong, it was never the
// store's fault.
// ---------------------------------------------------------------------------
#ifndef TPL_STORE_SLANG
#define TPL_STORE_SLANG

#include "Shared.slang"

// ---------------------------------------------------------------------------
// WHAT A HIT IS. Deliberately NOT the tracer's Hit: this carries only what the
// store knows, and Trace.cs.slang turns it into a Hit through the same
// groundShade and gMaterials path for every backend. Two stores producing the
// same surface have to agree about its shade, and the cheapest way to
// guarantee that is for neither of them to have an opinion.
// ---------------------------------------------------------------------------
struct VoxelHit
{
    bool hit;
    float t;   // METRES along d, comparable with a RayQuery's t
    float3 p;  // world position of the hit
    float3 ng; // outward face normal, exact, one axis for a voxel face
    int3 ijk;  // world voxel coordinate -- the key the shade hash is drawn from
    uint mtl;  // material id, indexing the same gMaterials palette for everyone
};

VoxelHit storeMiss()
{
    VoxelHit v;
    v.hit = false;
    v.t = 0.0f;
    v.p = float3(0.0f);
    v.ng = float3(0.0f, 1.0f, 0.0f);
    v.ijk = int3(0, 0, 0);
    v.mtl = 0u;
    return v;
}

#ifndef TPL_STORE_IMPL
#define TPL_STORE_IMPL "stores/Null.slang"
#endif

#include TPL_STORE_IMPL

#endif  // TPL_STORE_SLANG
