// ---------------------------------------------------------------------------
// stores/Null.slang -- the store that holds nothing.
//
// Every ray misses. That is not a stub waiting to be filled in: it is the
// reference backend, and it stays in the tree after real ones arrive.
//
// WHAT IT IS FOR.
//
//   IT MAKES THE ENGINE RUNNABLE WITH NO WORLD. Sky, sun, moon, atmosphere,
//   clouds, volumetric fog, exposure, bloom, tonemap and DLSS all run and are
//   all visible. Anything wrong in that picture is wrong in the lighting, and
//   no amount of staring at a traversal loop will find it.
//
//   IT IS THE ZERO OF EVERY MEASUREMENT. Profile this and you get what the
//   frame costs with the geometry query removed entirely -- ray setup, the
//   passes, the denoiser. Every backend's cost is its own profile minus this
//   one, which is the only way to say "the march costs 2x" and mean something.
//
//   IT IS THE INTERFACE'S TEST. If a change to the lighting stack stops
//   compiling against this file, the change reached through the seam for
//   something a store is not obliged to provide.
//
// A REAL BACKEND REPLACES THIS FILE AND NOTHING ELSE. Copy it to
// stores/<name>.slang, implement the two functions, declare whatever buffers
// and constants it wants at the top of it, bind them from its own C++ store,
// and point V4_STORE_IMPL at it.
// ---------------------------------------------------------------------------
#ifndef V4_STORE_NULL_SLANG
#define V4_STORE_NULL_SLANG

// The parameters are named rather than dropped so the signature reads as the
// contract a backend has to meet, and so a compiler warning about an unused
// one points at this file rather than at the caller.
VoxelHit storeTrace(float3 o, float3 d, float tMax, bool insideWater)
{
    return storeMiss();
}

// NOT storeTrace().hit. An occlusion query is allowed to be a different and
// much cheaper walk -- stop at the first solid thing, never resolve a normal,
// a material or a coordinate -- and in a dense scene that is most of the
// frame's rays. Keeping it a separate entry point is what lets a backend spend
// its effort there; collapsing it into the closest-hit form is the single
// easiest way to give away half the performance of a good store.
bool storeOccluded(float3 o, float3 d, float tMax)
{
    return false;
}

#endif  // V4_STORE_NULL_SLANG
