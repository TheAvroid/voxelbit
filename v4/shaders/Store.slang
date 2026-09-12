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
//     VoxelHit storeTrace(float3 o, float3 d, float tMax, bool insideWater)
//     bool     storeOccluded(float3 o, float3 d, float tMax)
//
// and a C++ side that binds whatever resources those need (see gpu/store.h).
// There is no third entry point, and adding one should feel like a decision
// rather than a convenience: the reason this engine can swap its geometry
// representation at all is that the lighting never learned what geometry is.
//
// ---------------------------------------------------------------------------
// `insideWater` IS THE ONE THING THE SEAM KNOWS ABOUT A MATERIAL, and it is
// here rather than in the tracer because only the backend can act on it.
//
// Water is a voxel like any other, so a lake's interior is a stack of voxel
// FACES. A ray that has already refracted through the surface and is travelling
// inside the water meets one of them every ten centimetres, and each one looked
// like a fresh air-water boundary: a metre of water became eleven refractions,
// the path bent further at every one, a few per cent reflected off each, and it
// ran out of bounces before reaching the bed. The lake rendered as a dark
// mirror. THERE IS ONE INTERFACE -- where the water meets air -- and a march
// stepping from one water voxel to the next is not it.
//
// So a caller that knows its ray is already in the water says so, and the store
// walks past water instead of stopping on it, reporting the first thing that is
// NOT water. Absorption is then taken over the whole segment by the tracer,
// which is what Beer-Lambert wants anyway.
//
// A BACKEND MAY IGNORE IT. Ignoring it is not a correctness failure, it is the
// behaviour above -- so the null store takes the argument and does nothing with
// it, and a backend with no water in it never has to think about the flag.
//
// SHADOW RAYS DO NOT NEED TO ASK. storeOccluded passes through water
// unconditionally: water casts no shadow, and one that did left every lake bed
// black -- which no caustic can rescue, because the caustics are drawn by
// multiplying an albedo the sun was never reaching.
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
// SELECTING A BACKEND. V4_STORE_IMPL is a quoted path to the implementation,
// set from gpu/tracer.h as a shader define. This engine ships two:
//
//     stores/Aabb.slang   voxels as DXR PROCEDURAL AABBs -- an 8^3 brick is one
//                         AABB in a bottom-level acceleration structure, the ray
//                         tracing cores find which bricks a ray crosses, and the
//                         shader walks a 512-bit mask inside the one it is
//                         handed. This is what v4 IS.
//     stores/Null.slang   nothing. The zero every measurement is taken against.
//
// Without the define it is the NULL store, which misses every ray -- so an
// engine with no backend at all still renders its sky, its sun, its clouds and
// its fog, and every pass downstream of the trace goes on running with nothing
// to shade. That is the blank slate, and it is a legitimate thing to look at:
// if the sky is wrong, it was never the store's fault.
// ---------------------------------------------------------------------------
#ifndef V4_STORE_SLANG
#define V4_STORE_SLANG

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

#ifndef V4_STORE_IMPL
#define V4_STORE_IMPL "stores/Null.slang"
#endif

#include V4_STORE_IMPL

#endif  // V4_STORE_SLANG
