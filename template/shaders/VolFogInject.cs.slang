// ---------------------------------------------------------------------------
// VolFogInject.cs.slang -- ask every cell of the world volume what it can see.
//
// One thread per cell, one dispatch per cascade. For each cell: where in the
// world is it, how dense is the air there, can it see the sun, can it see the
// sky. Not how bright it looks -- see the note in VolFog.slang on why this
// stores VISIBILITY and leaves the radiance to the march.
//
// ---------------------------------------------------------------------------
// THE SHADOW RAY IS STILL THE WHOLE POINT.
//
// Everything here is bookkeeping around one question -- is this parcel of air
// lit -- and a rasteriser has to answer it with a shadow map because that is
// the only tool it owns. v2 is a path tracer with a top-level structure over
// the whole resident wood, so it can ask the geometry directly. No shadow map
// covering a 625-chunk ring, no cascade to fit, no depth bias, and the beams
// are shaped by actual needles rather than by a depth texture's resolution.
//
// WHAT IS NEW IS THAT THE ANSWER KEEPS. In the froxel version this cell was a
// different piece of air every frame, so the shadow ray was re-asked from
// scratch each time and the temporal blend was averaging answers to different
// questions. Here the cell is a fixed piece of the world: the ray is re-asked
// about the SAME air, and the blend is a genuine average rather than a smear.
// ---------------------------------------------------------------------------
#include "VolFog.slang"

// THE WORLD, AND IT IS A VOLUME. This pass owned a private RayQuery against
// the TLAS and returned on COMMITTED_NOTHING, so once the ground stopped being
// triangles the fog's sun shadow stopped seeing it: shafts were cast THROUGH
// terrain, lighting fog inside a hill. It marches the same store the camera
// does now, so the shadow the fog uses and the shadow you can see cannot
// disagree.
#include "Store.slang"
RWTexture3D<float4> gFogVol;    // R sun vis, G sky vis, B spare, A density
Texture3D<float4> gFogHistory;  // last frame's, same cascade

cbuffer VolFogCB {
    float3 gOriginWs;   // this cascade's min corner, world metres, snapped
    float gDensity;
    float3 gCellSize;
    float gHeight;
    float3 gSunDir;
    float gSkyShadow;   // how much sky reaches a cell the up ray found covered
    int3 gHistShift;    // whole-cell offset from this volume to last frame's
    uint gFrame;
    uint gAccumulate;   // 0 on the first frame and after a teleport
    float gAlpha;       // how much of THIS frame survives the blend
    float gPad0;
    float gPad1;

    // THE WORLD-CELL INDEX OF THIS VOLUME.S MIN CORNER.
    //
    // The jitter below is hashed on (gOriginCell + c), not on c. A volume-local
    // hash looks equivalent and is not: the origin shifts by whole cells as the
    // player walks, so a fixed parcel of air lands on a DIFFERENT local index
    // from one frame to the next and its jitter jumps. The blend then smears
    // that jump over its whole tail, which reads as the fog dragging while you
    // run -- a camera-dependent artefact in a volume that is supposed to have
    // none. Anchoring the hash to the world is what actually removes it.
    int3 gOriginCell;
    uint gPad2;
};

// A cheap per-cell hash, moved every frame.
//
// The jitter is what turns a hard yes/no shadow ray into a soft edge: the
// sample wanders inside its own cell, the blend averages several frames of it,
// and a binary answer arrives as a gradient. The froxel version needed the same
// trick to hide slice banding; here it is buying penumbra instead.
float3 fogCellJitter(int3 c, uint frame)
{
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u;
    h ^= frame * 0x9E3779B9u;
    float3 o;
    h ^= h >> 16; h *= 0x85EBCA6Bu; o.x = float(h & 0xFFFFu) / 65536.0f - 0.5f;
    h ^= h >> 13; h *= 0xC2B2AE35u; o.y = float(h & 0xFFFFu) / 65536.0f - 0.5f;
    h ^= h >> 16; h *= 0x27D4EB2Fu; o.z = float(h & 0xFFFFu) / 65536.0f - 0.5f;
    return o;
}

bool fogOccluded(float3 origin, float3 dir, float tmax)
{
    RayDesc r;
    r.Origin = origin;
    r.Direction = dir;
    r.TMin = 0.05f;
    r.TMax = tmax;
    // ANY-HIT, NOT CLOSEST-HIT. Nothing here needs to know WHAT occluded the
    // light, only whether anything did, so the march may stop at the first
    // solid voxel -- which for a canopy is the difference between traversing to
    // the ground and stopping at the first needle.
    return storeOccluded(r.Origin, r.Direction, r.TMax);
}

[numthreads(4, 4, 4)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kFogCellsX) || tid.y >= uint(kFogCellsY) || tid.z >= uint(kFogCellsZ))
        return;
    const int3 c = int3(tid);

    const float3 jit = fogCellJitter(gOriginCell + c, gFrame);
    const float3 p = gOriginWs + (float3(c) + 0.5f + jit) * gCellSize;

    const float density = fogDensityAt(p, gDensity, gHeight);
    if (density <= 0.0f) {
        gFogVol[c] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    const float sunVis = fogOccluded(p, gSunDir, 4000.0f) ? 0.0f : 1.0f;

    // Straight up, and only far enough to clear the tallest pine. A proxy for
    // the hemisphere rather than an integral of it -- affordable where the
    // integral is not, and up is the direction that separates a clearing from a
    // canopy. NOT ZERO when covered: the dome is not a point source and light
    // reaches shadowed air by paths this one ray never tested.
    const float skyVis = fogOccluded(p, float3(0.0f, 1.0f, 0.0f), 60.0f) ? gSkyShadow : 1.0f;

    float4 result = float4(sunVis, skyVis, 0.0f, density);

    // -- temporal blend, by whole cells --------------------------------------
    //
    // THE ENTIRE REPROJECTION. Both volumes are snapped to the same lattice, so
    // last frame's copy of this cell is at a fixed integer offset -- no matrix,
    // no perspective divide, no filtering, and no way to land between texels
    // and blend two different parcels of air together. A cell either has
    // history or has walked off the edge of last frame's volume, and the edge
    // is the only case there is.
    if (gAccumulate != 0) {
        const int3 h = c + gHistShift;
        if (all(h >= int3(0, 0, 0)) &&
            h.x < kFogCellsX && h.y < kFogCellsY && h.z < kFogCellsZ) {
            result = lerp(gFogHistory[h], result, gAlpha);
        }
    }

    gFogVol[c] = result;
}
