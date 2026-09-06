// ---------------------------------------------------------------------------
// VolFogInject.cs.slang -- light every froxel, once.
//
// One thread per froxel. For each one: where is it, how dense is the air there,
// can it see the sun, and how much of that light bounces toward the camera.
// The answer goes into the grid as RGB = in-scattered radiance, A = density,
// and the march pass turns that into what the pixel actually sees.
//
// ---------------------------------------------------------------------------
// THE SHADOW RAY IS THE WHOLE POINT.
//
// Everything else here is bookkeeping around one question -- is this parcel of
// air lit -- and the reference answers it with a shadow map, because it is a
// rasteriser and that is the only tool it has. v7 is a path tracer with a
// top-level structure over the entire resident wood, so it can ask the
// geometry directly.
//
// That is not merely tidier. A shadow map covering a 625-chunk ring at a
// resolution fine enough to resolve pine needles does not exist; a ray does not
// care how far the ring reaches, needs no cascade, no bias, and no filtering.
// The beams this produces are shaped by actual needles rather than by a depth
// texture's resolution.
// ---------------------------------------------------------------------------
#include "VolFog.slang"
#include "Sky.slang"
#include "Vecmath.slang"

RaytracingAccelerationStructure gScene;
RWTexture3D<float4> gFogGrid;      // RGB in-scatter, A density
Texture3D<float4> gFogHistory;     // last frame's, for the temporal blend
SamplerState gFogSampler;

cbuffer VolFogCB {
    V6Camera gCam;
    V6Camera gPrevCam;
    V6Sky gSky;
    float gNear;
    float gFar;
    float gDensity;
    float gHeight;
    float gAnisotropy;
    float gAmbient;      // how much of the sky dome reaches a froxel
    uint gFrame;
    uint gAccumulate;    // 0 on the first frame and after a camera cut
};

// A cheap per-froxel hash, standing in for the reference's blue-noise texture.
//
// Blue noise would be better distributed, but it is a texture to ship and load
// for a value that is immediately averaged over eight frames by the temporal
// blend below. White noise that decorrelates across frames gets to the same
// place; what matters is that neighbouring froxels do not share a jitter and
// that the sequence moves every frame.
float fogJitter(int3 coord, uint frame)
{
    uint h = uint(coord.x) * 73856093u ^ uint(coord.y) * 19349663u ^ uint(coord.z) * 83492791u;
    h ^= frame * 0x9E3779B9u;
    h ^= h >> 16;
    h *= 0x85EBCA6Bu;
    h ^= h >> 13;
    return float(h & 0xFFFFFFu) / float(0x1000000u) - 0.5f;
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kFogX) || tid.y >= uint(kFogY) || tid.z >= uint(kFogZ))
        return;
    const int3 coord = int3(tid);

    const float jitter = fogJitter(coord, gFrame);
    const float3 p = fogFroxelWorld(coord, jitter, gCam, gNear, gFar);

    const float density = fogDensityAt(p, gDensity, gHeight);
    if (density <= 0.0f) {
        gFogGrid[coord] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    // Toward the camera: the direction the scattered light has to leave in.
    const float3 wo = normalize(gCam.pos - p);

    // -- the sun, if this parcel of air can see it ---------------------------
    float3 inscatter = float3(0.0f);
    const float3 sunDir = gSky.sunDir;
    const float3 sunL = gSky.sunRadiance;
    if (!isBlack(sunL)) {
        // ANY-HIT, NOT CLOSEST-HIT. Nothing here needs to know WHAT occluded
        // the sun, only whether anything did, so the query is allowed to stop
        // at the first hit -- which for a canopy is the difference between
        // traversing to the ground and stopping at the first needle.
        RayDesc r;
        r.Origin = p;
        r.Direction = sunDir;
        r.TMin = 0.05f;
        r.TMax = 4000.0f;
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_CULL_NON_OPAQUE> q;
        q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
        q.Proceed();
        const bool lit = q.CommittedStatus() == COMMITTED_NOTHING;
        if (lit) {
            // IRRADIANCE, NOT THE DISK'S RADIANCE, and the difference is a
            // factor of about fifteen thousand.
            //
            // sunRadiance is what a ray sees when it HITS the sun's disk -- a
            // huge number, correct for that purpose. What a parcel of air
            // receives is that radiance integrated over the solid angle the
            // disk actually subtends, which is 6.8e-5 steradians. Using the
            // radiance directly is exactly the mistake the old analytic fog
            // warned about in its own comment: "the sun's irradiance through a
            // forward-peaked phase is two orders of magnitude above the dome,
            // which washes the whole frame to white". It does; the first render
            // of this pass was solid white.
            inscatter += sunL * skySolidAngle() * fogPhase(wo, -sunDir, gAnisotropy);
        }
    }

    // -- and the sky, which reaches everywhere ------------------------------
    //
    // Isotropic and unshadowed, which is a deliberate approximation: the dome
    // is a hemisphere of low-intensity light and tracing it properly would cost
    // as much as the rest of the renderer. Without it the air in shadow goes
    // black, which is far more wrong than the approximation is.
    inscatter += skyDome(gSky, float3(0.0f, 1.0f, 0.0f)) * gAmbient;

    float4 result = float4(inscatter * density, density);

    // -- temporal blend ------------------------------------------------------
    //
    // The jitter above turns slice banding into noise; this is what turns the
    // noise into an answer. Five percent per frame is the reference's number
    // and it is a long tail on purpose -- the fog is a low-frequency thing and
    // nobody notices it converging, whereas they very much notice it flickering.
    //
    // REPROJECTED THROUGH LAST FRAME'S CAMERA, so the history follows the world
    // rather than the screen. Outside the old frustum there is no history and
    // this frame's value stands alone.
    if (gAccumulate != 0) {
        const float3 pn = fogFroxelWorld(coord, 0.0f, gCam, gNear, gFar);
        const float3 rel = pn - gPrevCam.pos;
        const float fwd = dot(rel, gPrevCam.w);
        if (fwd > gNear) {
            const float2 prevNdc = float2(dot(rel, gPrevCam.u) / (fwd * gPrevCam.halfW),
                                          dot(rel, gPrevCam.v) / (fwd * gPrevCam.halfH));
            const float3 uvw = float3(prevNdc * 0.5f + 0.5f,
                                      fogDistanceToSlice(fwd, gNear, gFar) / float(kFogZ));
            if (all(uvw >= 0.0f) && all(uvw <= 1.0f)) {
                const float4 hist = gFogHistory.SampleLevel(gFogSampler, uvw, 0.0f);
                result = lerp(hist, result, 0.05f);
            }
        }
    }

    gFogGrid[coord] = result;
}
