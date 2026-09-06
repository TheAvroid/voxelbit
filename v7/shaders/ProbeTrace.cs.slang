// ---------------------------------------------------------------------------
// ProbeTrace.cs.slang -- PHASE A, the rays that feed the irradiance probes.
//
// One thread per (probe, ray). Each casts a ray from a probe's position in a
// direction from the volume's rotated spherical Fibonacci set, shades whatever
// it hits exactly as the camera path would, and writes radiance and distance
// into the ray-data texture. RTXGI's blend pass reads that texture and folds it
// into the octahedral irradiance atlas; nothing here ever touches the atlas.
//
// THE SHADING HERE IS DELIBERATELY ONE BOUNCE AND NO MORE. A probe ray is
// allowed to see direct sunlight, the sky, and the irradiance already stored in
// the probes -- and that last part is the entire trick. Feeding last frame's
// probes back into this frame's probe rays is what makes the cache
// MULTI-BOUNCE: light that took two bounces to arrive was one bounce last
// frame, and one the frame before. The series converges to the full transport
// without any single ray ever going more than one segment. That is why the
// camera path can stop at its first bounce and ask the probes instead.
//
// ---------------------------------------------------------------------------
// EVERY INDEXING CONVENTION IN THIS FILE BELONGS TO RTXGI, NOT TO v7, and each
// one is wrong in a way that does not look wrong.
//
// The probe order, the ray-data layout and the backface encoding are all read
// back by RTXGI's blend shader, which was compiled for RIGHT HAND, Y-UP (the
// -D RTXGI_COORDINATE_SYSTEM=2 in compile_ddgi_shaders.bat). Transposing an
// axis here does not produce a black screen -- it produces a wood lit from
// somewhere plausible but wrong, which is far harder to notice and far harder
// to find. So each convention below is written out with the SDK function it
// mirrors named beside it.
// ---------------------------------------------------------------------------
#include "Shared.slang"
#include "Vecmath.slang"
#include "Material.slang"
#include "Sky.slang"
#include "Ddgi.slang"

RaytracingAccelerationStructure gScene;
ByteAddressBuffer gTriPool;
StructuredBuffer<V6Instance> gInstances;
StructuredBuffer<V6Material> gMaterials;

// RGB radiance in xyz, distance in w. Unpacked float4 rather than RTXGI's
// packed two-channel form -- see the note on probeRayDataFormat in ddgi.h.
RWTexture2DArray<float4> gProbeRayData;

// Last frame's irradiance, so a probe ray can terminate into the cache instead
// of stopping at one bounce. Sampled, not written.
Texture2DArray<float4> gProbeIrradiance;
Texture2DArray<float4> gProbeDistance;
Texture2DArray<float4> gProbeDataTex;
SamplerState gProbeSampler;

cbuffer gProbeCB
{
    V6DdgiConsts gDdgi;
    V6Sky gProbeSky;
    uint gProbeSeed;
    uint gProbeTick;
    int gProbeUseCache;   // sample last frame's probes on the far end of a ray
    float gProbePad;
};

// ---------------------------------------------------------------------------
// Which probe a linear index is, in grid coordinates.
//
// Mirrors DDGIGetProbeCoords() in ProbeIndexing.hlsl for the RIGHT-hand Y-up
// branch. Note that Y -- the vertical axis, the one a forest cares about --
// strides by countX*countZ, NOT by countX. Getting this the obvious way round
// stacks the wood sideways.
// ---------------------------------------------------------------------------
int3 probeCoords(int probeIndex)
{
    int3 c;
    c.x = probeIndex % gDdgi.probeCounts.x;
    c.y = probeIndex / (gDdgi.probeCounts.x * gDdgi.probeCounts.z);
    c.z = (probeIndex / gDdgi.probeCounts.x) % gDdgi.probeCounts.z;
    return c;
}

// Mirrors DDGIGetProbeWorldPosition(). The grid is centred on the origin, so a
// coordinate is measured from the middle rather than the corner; the scroll
// offsets are how a moving volume renumbers its probes without moving them.
float3 probeWorldPosition(int3 coords)
{
    const float3 half = (float3(gDdgi.probeCounts) - 1.0f) * 0.5f;
    const float3 local = (float3(coords) - half) * gDdgi.probeSpacing;
    return gDdgi.origin + local + float3(gDdgi.probeScroll) * gDdgi.probeSpacing;
}

// Mirrors RTXGISphericalFibonacci() + the volume's ray rotation.
//
// THE ROTATION IS WHY 192 RAYS IS ENOUGH. The Fibonacci set is fixed, so on its
// own a probe would sample the same 192 directions forever and never see what
// falls between them. RTXGI hands over a fresh random quaternion every frame;
// the blend pass integrates over all of them, so the effective sample count is
// 192 times however many frames the hysteresis remembers.
float3 sphericalFibonacci(float i, float n)
{
    const float b = (sqrt(5.0f) * 0.5f + 0.5f) - 1.0f;
    const float phi = TWO_PI * frac(i * b);
    const float cosTheta = 1.0f - (2.0f * i + 1.0f) * (1.0f / n);
    const float sinTheta = sqrt(saturate(1.0f - cosTheta * cosTheta));
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float3 rotateByQuat(float3 v, float4 q)
{
    const float3 u = q.xyz;
    return v + 2.0f * cross(u, cross(u, v) + q.w * v);
}

float3 probeRayDirection(int rayIndex)
{
    return normalize(rotateByQuat(sphericalFibonacci(float(rayIndex), float(gDdgi.numRays)),
                                  gDdgi.rayRotation));
}

// ---------------------------------------------------------------------------
// The same hit as the camera path takes, minus everything only a camera needs.
// Shares loadPackedTri / groundShade / faceNormal semantics with
// Trace.cs.slang; kept as its own small copy rather than a shared include
// because a probe never needs the water BSDF, the tint, or the guides.
// ---------------------------------------------------------------------------
uint loadPackedTri(uint triOffset, uint primitiveIndex)
{
    const uint byteOfs = (triOffset + primitiveIndex) * 2u;
    const uint dword = gTriPool.Load(byteOfs & ~3u);
    return ((byteOfs & 2u) != 0u) ? (dword >> 16) : (dword & 0xFFFFu);
}

uint hashVoxel(int3 v)
{
    uint h = uint(v.x) * 374761393u + uint(v.y) * 1103515245u + uint(v.z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

uint groundShade(uint mtl, float3 p, float3 ng)
{
    uint base, count;
    if (mtl >= kGrass0 && mtl < kGrass0 + kGrassCount) { base = kGrass0; count = kGrassCount; }
    else if (mtl >= kSoil0 && mtl < kSoil0 + kSoilCount) { base = kSoil0; count = kSoilCount; }
    else if (mtl >= kLitter0 && mtl < kLitter0 + kLitterCount) { base = kLitter0; count = kLitterCount; }
    else return mtl;
    const int3 v = int3(floor((p - ng * (0.5f * kVoxelM)) / kVoxelM));
    return base + hashVoxel(v) % count;
}

float3 faceNormal(uint dir)
{
    switch (dir)
    {
    case 0: return float3(0.0f, 1.0f, 0.0f);
    case 1: return float3(0.0f, -1.0f, 0.0f);
    case 2: return float3(1.0f, 0.0f, 0.0f);
    case 3: return float3(-1.0f, 0.0f, 0.0f);
    case 4: return float3(0.0f, 0.0f, 1.0f);
    default: return float3(0.0f, 0.0f, -1.0f);
    }
}

float3 offsetRay(float3 p, float3 ng, float3 dir)
{
    const float scale = max(3e-5f, 2e-6f * maxComp(abs(p)));
    return p + faceforward3(ng, dir) * scale;
}

bool traceOccluded(float3 o, float3 d, float tmax)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-5f;
    ray.TMax = tmax;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
                     0xFF, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

[numthreads(32, 1, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    const int rayIndex = int(tid.x);
    const int probeIndex = int(tid.y);
    if (rayIndex >= gDdgi.numRays) return;

    const int numProbes = gDdgi.probeCounts.x * gDdgi.probeCounts.y * gDdgi.probeCounts.z;
    if (probeIndex >= numProbes) return;

    const int3 coords = probeCoords(probeIndex);

    // WHERE THE RESULT GOES. The ray-data texture is (rayIndex, probe-within-
    // slice, slice), and the slice is the VERTICAL axis -- mirrors
    // GetDDGIVolumeTextureDimensions(), which sets height to countX*countZ and
    // the array size to countY for a Y-up volume.
    const uint3 dst = uint3(uint(rayIndex),
                            uint(coords.x + coords.z * gDdgi.probeCounts.x),
                            uint(coords.y));

    // A probe classified as inactive is inside geometry and sees nothing worth
    // having. Skipping it is where most of the ray budget is won back -- in a
    // wood a good fraction of any grid is underground or inside a trunk.
    const float4 pdata = gProbeDataTex[uint3(dst.yz, 0)];
    // w < 0 marks inactive in RTXGI's probe-data encoding; the xyz is the
    // relocation offset, applied below.
    const bool inactive = (gDdgi.useClassification != 0) && (pdata.w < 0.0f);

    float3 origin = probeWorldPosition(coords) + pdata.xyz;
    const float3 dir = probeRayDirection(rayIndex);

    if (inactive)
    {
        gProbeRayData[dst] = float4(0.0f, 0.0f, 0.0f, gDdgi.maxRayDistance);
        return;
    }

    RayDesc ray;
    ray.Origin = origin;
    ray.Direction = dir;
    ray.TMin = 0.0f;
    ray.TMax = gDdgi.maxRayDistance;

    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed();

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
    {
        // Missed: the sky, at full distance. The distance matters as much as
        // the radiance -- the blend pass builds a depth map from it that later
        // lookups use to reject probes on the far side of a wall.
        gProbeRayData[dst] = float4(skyDome(gProbeSky, dir), gDdgi.maxRayDistance);
        return;
    }

    const float t = q.CommittedRayT();
    const float3 p = origin + dir * t;
    const V6Instance inst = gInstances[q.CommittedInstanceID()];
    const uint packed = loadPackedTri(inst.triOffset, q.CommittedPrimitiveIndex());
    const float3x4 o2w = q.CommittedObjectToWorld3x4();
    const float3 ng = mul((float3x3)o2w, faceNormal(packed >> 8));

    // A BACKFACE HIT IS A NEGATIVE DISTANCE, and that is a message rather than
    // a measurement. RTXGI counts the backfaces a probe sees and relocates any
    // probe that sees too many -- that is how a probe buried in a trunk climbs
    // out. The 0.2 scale is the SDK's own convention (DDGIStoreProbeRay-
    // BackfaceHit); the relocation pass expects exactly it.
    if (dot(ng, dir) > 0.0f)
    {
        gProbeRayData[dst] = float4(0.0f, 0.0f, 0.0f, -t * 0.2f);
        return;
    }

    V6Material L = gMaterials[groundShade(packed & 0xFFu, p, ng)];
    float3 albedo = L.albedo;
    if (inst.kind == KIND_TREE) albedo *= inst.tint;

    // -- direct sun, one shadow ray -----------------------------------------
    float3 radiance = float3(0.0f);
    const float3 sunDir = gProbeSky.sunDir;
    const float ndl = dot(ng, sunDir);
    if (ndl > 0.0f && !isBlack(gProbeSky.sunRadiance))
    {
        if (!traceOccluded(offsetRay(p, ng, sunDir), sunDir, INF))
        {
            // Lambert only. A probe stores IRRADIANCE -- a cosine-weighted
            // integral over the hemisphere -- so a specular lobe evaluated for
            // one outgoing direction would be meaningless to it, and a
            // view-dependent term stored in a view-independent cache is exactly
            // the sort of thing that makes probe GI look wrong when the camera
            // turns. Diffuse only, on purpose.
            radiance += albedo * INV_PI * gProbeSky.sunRadiance * ndl;
        }
    }

    // -- sky, as ambient from the visible hemisphere -------------------------
    radiance += albedo * INV_PI * skyDome(gProbeSky, ng) * PI * 0.5f;

    // -- last frame's probes: the bounce that makes the cache multi-bounce ----
    if (gProbeUseCache != 0)
    {
        radiance += albedo * ddgiSampleIrradiance(p, ng, dir, gDdgi, gProbeIrradiance,
                                                 gProbeDistance, gProbeDataTex,
                                                 gProbeSampler);
    }

    gProbeRayData[dst] = float4(radiance, t);
}
