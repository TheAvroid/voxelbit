// ---------------------------------------------------------------------------
// StoreProbe.cs.slang -- a scratch entry point, for checkstore.bat only.
//
// NOT PART OF THE ENGINE, and deliberately not listed in CMakeLists.txt, so it
// is never staged beside the exe and nothing ever loads it. It exists because
// Falcor compiles Slang at LOAD: a type error in a store does not appear until
// the engine is running, and then it appears as a program that failed to create
// several layers down, naming neither the store nor the seam.
//
// slangc needs something with an entry point to compile the seam INTO, and this
// is the smallest thing that touches both of its functions and every field of
// what they return -- so nothing in VoxelHit can quietly stop being written
// without this failing to build.
// ---------------------------------------------------------------------------
#include "Store.slang"

RWStructuredBuffer<float4> gProbeOut;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const float3 o = float3(float(tid.x), 40.0f, float(tid.y));
    const float3 d = normalize(float3(0.3f, -1.0f, 0.2f));
    const VoxelHit v = storeTrace(o, d, 1e30f, false);
    const bool occ = storeOccluded(o, float3(0.0f, 1.0f, 0.0f), 100.0f);
    gProbeOut[tid.x + tid.y * 64] =
        float4(v.t, float(v.mtl), float(v.ijk.x), occ ? 1.0f : 0.0f) + float4(v.ng, 0.0f);
}
