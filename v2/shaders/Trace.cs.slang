// ---------------------------------------------------------------------------
// Trace.cs.slang -- the whole renderer, as one compute shader.
//
// THIS FILE IS THE POINT OF v2. The integrator is v2's, unchanged in every
// decision that affects the picture: unidirectional paths, next-event
// estimation to the sun's disk, MIS against the BSDF, analytic height fog on
// the primary segment. What changed is that it stopped being five programs and
// a binding table and became one kernel.
//
// WHY THAT IS WORTH DOING. OptiX -- and DXR's pipeline mode with it -- is built
// around a shader binding table: geometry is bound to hit-group records, the
// driver dispatches into them on a hit, and the shading code lives in a program
// the caller never calls directly. That design earns its keep when different
// geometry needs genuinely different shading code. This scene does not have
// that. It has ONE hit path, and every hit does the same three things: look up
// a packed uint16, expand it to a normal and a material, come back.
//
// So the whole table was overhead. It cost:
//
//   * A RECORD PER RESIDENT CHUNK, allocated from a fixed slot pool, published
//     over the bus whenever the ring moved, with the slot lifetime hand-managed
//     against eviction. That is gone entirely -- an instance now carries an
//     offset into one shared buffer, and the offset is just a number in the
//     instance record the acceleration structure already needed.
//
//   * A PIPELINE TO LINK AND A STACK TO SIZE. v2 had to flatten its path loop
//     into raygen specifically so the continuation stack would not be sized for
//     the deepest path times every thread in flight. Inline ray tracing has no
//     continuation stack at all: the loop below is an ordinary loop in an
//     ordinary function, and the ray is a call.
//
//   * A SEPARATE SHADOW RAY TYPE with its own miss program, existing only to
//     clear a payload register. Here it is a RayQuery with
//     ACCEPT_FIRST_HIT_AND_END_SEARCH and no shading of any kind.
//
// What is left reads like the CPU tracer this lineage started as, which is the
// real argument: the shape of the code now matches the shape of the algorithm.
//
// ALL GEOMETRY IS DECLARED OPAQUE, which is what lets Proceed() return
// immediately -- there is no any-hit work to run, so traversal completes inside
// the RT cores and the shader never re-enters it. Foliage is cut out by its
// voxels, not by an alpha test, so there is nothing an any-hit could do.
// ---------------------------------------------------------------------------

#include "Shared.slang"
#include "Vecmath.slang"
#include "BlueNoise.slang"
#include "Material.slang"
#include "Sky.slang"
#include "Restir.slang"
#include "VolFog.slang"
#include "Clouds.slang"
#include "Ddgi.slang"

// The hash cache. Included in BOTH of this file's compilations -- the query one
// that the camera uses and the update one that fills it -- because they share
// every helper below and only differ in which entry point they end at.
#if V2_SHARC_QUERY || V2_SHARC_UPDATE
#if V2_SHARC_UPDATE
#define SHARC_UPDATE 1
#else
#define SHARC_QUERY 1
#endif
#include "Sharc.slang"
#endif

// THE CACHE IS COMPILED IN ONLY WHERE IT CAN RUN. Nrc.slang uses CoopVec, which
// does not exist on a D3D12 target below Shader Model 6.10 -- including it
// unconditionally does not disable the cache, it makes the WHOLE TRACER fail to
// compile, and the engine dies at startup with a shader error that mentions
// nothing about neural anything. The host sets this define only after it has
// confirmed the capability; see gpu/neural.h and gpu/tracer.h.
#ifdef V2_NRC
#include "Nrc.slang"
#endif

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
RaytracingAccelerationStructure gScene;

// packTri(material, face), one uint16 per triangle, for every instance in the
// scene at once. An instance's triOffset plus the primitive index addresses it.
// See world.h for the allocator that hands out the ranges.
ByteAddressBuffer gTriPool;

StructuredBuffer<V6Instance> gInstances;
StructuredBuffer<V6Material> gMaterials;

// The radiance cache: 14 KB of weights, and the buffer the tracer writes its
// training records into. Both are bound every frame whether or not the cache is
// on -- see the note on gDiffRadiance below for why an unbound declared
// resource is a dispatch-time validation error rather than a dead branch.
#ifdef V2_NRC
ByteAddressBuffer gNrcWeights;
RWStructuredBuffer<NrcSample> gNrcSamples;
#endif

// ReSTIR needs no special hardware -- it is plain Slang and compiles on both
// backends -- so unlike the cache it is bound unconditionally and switched at
// runtime. gRestirFinal is LAST frame's resampled result, which is what this
// frame shades from; gRestirCandidate is where this frame's own bounce goes.
RWStructuredBuffer<Reservoir> gRestirCandidate;
StructuredBuffer<Reservoir> gRestirFinal;
RWTexture2D<float4> gPrimaryPos;

// The world-anchored fog volume, two cascades. R is sun visibility, G is sky
// visibility, A is density -- NOT radiance. What a parcel of air can see is a
// property of the air; how bright it looks is a property of where you are
// standing, so the radiance is assembled below, at march time, where the view
// direction exists. See the long note in VolFog.slang.
Texture3D<float4> gFogVol0;   // near cascade, fine cells
Texture3D<float4> gFogVol1;   // far cascade, coarse cells
SamplerState gFogSampler;

// The sky-view table, when the atmosphere path is on. Bound unconditionally --
// atmosphere.h creates the texture whether the feature is enabled or not -- so
// there is no unbound-resource case to guard; sky.atmoOn decides whether
// skyDome() actually reads it.
Texture2D<float4> gAtmoSkyView;
SamplerState gAtmoSamp;

// The cloud density cache -- a TILE of an endless deck, so the march samples it
// at frac(world / tile) and it never needs refilling as the player moves. Filled
// The moon's face. A photograph, sampled flat -- see moonDisk in Sky.slang.
Texture2D<float4> gMoonTex;
SamplerState gMoonSamp;

// once by CloudFill; this shader only ever reads it.
Texture3D<float> gCloudVol;
SamplerState gCloudSamp;

// -- the irradiance probes ---------------------------------------------------
//
// Bound every frame whether or not the volume is running, for the same reason
// the radiance cache is: a declared resource with nothing behind it is a
// validation error at dispatch time, not a dead branch. With DDGI off these
// point at a 1x1 placeholder the tracer owns and gParams.giMode is 0, so
// nothing ever reads them.
Texture2DArray<float4> gDdgiIrradiance;
Texture2DArray<float4> gDdgiDistance;
Texture2DArray<float4> gDdgiProbeData;
SamplerState gDdgiSampler;

cbuffer gDdgiCB
{
    V6DdgiConsts gDdgiConsts;
};

// The film. accum holds sums with the sample count in .w, so a resolve never
// has to consult a second buffer to know what to divide by.
RWTexture2D<float4> gAccum;
RWTexture2D<float4> gColor;

// ---------------------------------------------------------------------------
// The guide buffers DLSS Ray Reconstruction reads.
//
// EVERY ONE OF THESE IS A BY-PRODUCT OF THE PRIMARY HIT, and that is the whole
// reason this is cheap. A rasteriser has to draw the scene a second time to
// produce a G-buffer; here the first bounce of the path already knows the
// position, the normal, the material and the distance, and writing them out is
// six stores in a branch that runs once per pixel.
//
// Ray Reconstruction is not a denoiser bolted on after the fact -- it replaces
// the accumulation entirely. It is trained on exactly this: a one-sample path
// traced image plus the albedo and geometry that generated it, from which it
// can separate what is noise in the LIGHTING from what is detail in the
// SURFACE. That distinction is the thing a spatial denoiser cannot make, and it
// is why v2 removed every denoiser it tried: on a voxel canopy they turned
// thousands of needle-sized faces to felt, because the felt and the needles
// look the same once you have thrown the albedo away.
// ---------------------------------------------------------------------------
RWTexture2D<float4> gGuideAlbedo;    // diffuse albedo of the primary surface
RWTexture2D<float4> gGuideSpecular;  // specular albedo, Fresnel at this angle
RWTexture2D<float4> gGuideNormRough; // world normal in xyz, linear roughness in w
RWTexture2D<float> gGuideDepth;      // NDC depth, matching the projection below
RWTexture2D<float2> gGuideMotion;    // render pixels, this frame -> last frame

// ---------------------------------------------------------------------------
// PHASE B -- the demodulated channels.
//
// The guides above are what DLSS Ray Reconstruction reads, and RR does its own
// demodulation internally from them. These four are the OTHER consumer: NRD,
// and anything else that has to be handed lighting with the texture already
// divided out. They are written only when gParams.demodulate is set, so the
// Ray Reconstruction path -- which wants the composited colour and its guides,
// and nothing else -- pays nothing for them.
//
// WHY THE SPLIT HAS TO HAPPEN HERE AND NOT IN THE PASS. A demodulation pass
// downstream can divide a composited image by an albedo, but it cannot take
// radiance apart into the lobe that produced it: once diffuse and specular have
// been summed into one pixel, which fraction belonged to which is gone. That
// fraction is known exactly at the vertex where the BSDF was evaluated, and
// nowhere else -- so the SPLIT is made here and the DIVISION is the pass.
//
// gEmission is what no albedo modulates at all: sky seen directly down the
// primary ray, and the light the haze scatters in on the way. It has to be held
// out or the remultiply at the end would tint the sky with whatever surface
// happened to be behind it.
// ---------------------------------------------------------------------------
RWTexture2D<float4> gDiffRadiance; // diffuse-lobe radiance, still modulated
RWTexture2D<float4> gSpecRadiance; // specular-lobe radiance, still modulated
RWTexture2D<float4> gEmission;     // modulated by nothing: sky, and in-scatter
// The exact divisor for gDiffRadiance: the surface albedo with NO lobe weight
// folded in. Deliberately NOT gGuideAlbedo, which carries an extra (1 - kt) for
// Ray Reconstruction's benefit -- the translucency lobe is modulated by kt
// rather than (1 - kt), and both lobes land in this one channel, so the only
// factor they share is the albedo itself. Dividing by anything else would not
// invert.
RWTexture2D<float4> gDemodAlbedo;

cbuffer gParamsCB
{
    V6Params gParams;
};

// ---------------------------------------------------------------------------
// The face direction was recorded when the quad was emitted, so the normal is
// exact rather than reconstructed -- no cross product, and no need to keep a
// vertex buffer readable after the structure is built.
//
// Material in the low byte, face direction in the high byte, matching packTri()
// in voxelworld.h.
// ---------------------------------------------------------------------------
uint loadPackedTri(uint triOffset, uint primitiveIndex)
{
    // The pool is addressed in uint16 units; a ByteAddressBuffer loads 32 bits
    // from a 4-byte-aligned offset, so take the dword containing it and pick
    // the half.
    const uint byteOfs = (triOffset + primitiveIndex) * 2u;
    const uint dword = gTriPool.Load(byteOfs & ~3u);
    return ((byteOfs & 2u) != 0u) ? (dword >> 16) : (dword & 0xFFFFu);
}

// ---------------------------------------------------------------------------
// WHICH SHADE OF GROUND THIS VOXEL IS.
//
// A pine's bark is half a dozen browns scattered voxel by voxel, and that
// scatter is most of why a trunk reads as bark rather than as a painted
// cylinder. The ground used to pick its shade from a three-metre noise field
// instead, so it read as patches of one green beside patches of another -- the
// same six colours, arranged like a map instead of like a material.
//
// So the mesher stores only which FAMILY a face belongs to -- grass, soil,
// litter -- and the shade is drawn here, from a hash of the voxel the ray
// landed in. That is one integer hash on a path that already has the hit point
// in hand, and it is strictly cheaper than the alternative: a material that
// changed every three metres broke the top-face merge at every patch boundary,
// so naming only the family leaves longer runs and FEWER triangles than the
// patches cost.
// ---------------------------------------------------------------------------
uint hashVoxel(int3 v)
{
    // The same avalanche as ihash3 in core/noise.h. It does not have to agree
    // with the host -- nothing on the host asks this question -- but there is
    // no reason to invent a second one.
    uint h = uint(v.x) * 374761393u + uint(v.y) * 1103515245u + uint(v.z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

uint groundShade(uint mtl, float3 p, float3 ng)
{
    uint base, count;
    if (mtl >= kGrass0 && mtl < kGrass0 + kGrassCount)
    {
        base = kGrass0;
        count = kGrassCount;
    }
    else if (mtl >= kSoil0 && mtl < kSoil0 + kSoilCount)
    {
        base = kSoil0;
        count = kSoilCount;
    }
    else if (mtl >= kLitter0 && mtl < kLitter0 + kLitterCount)
    {
        base = kLitter0;
        count = kLitterCount;
    }
    else
    {
        return mtl; // rock, sand, water, and everything a model brought with it
    }

    // HALF A VOXEL ALONG THE NORMAL, so the sample lands INSIDE the voxel the
    // face belongs to rather than exactly on its boundary -- where the floor of
    // a coordinate is a coin toss and every edge in the frame would shimmer
    // between two shades as the camera moved.
    const int3 v = int3(floor((p - ng * (0.5f * kVoxelM)) / kVoxelM));
    return base + hashVoxel(v) % count;
}

float3 faceNormal(uint dir)
{
    switch (dir)
    {
    case 0: return float3(0.0f, 1.0f, 0.0f);  // +Y
    case 1: return float3(0.0f, -1.0f, 0.0f); // -Y
    case 2: return float3(1.0f, 0.0f, 0.0f);  // +X
    case 3: return float3(-1.0f, 0.0f, 0.0f); // -X
    case 4: return float3(0.0f, 0.0f, 1.0f);  // +Z
    default: return float3(0.0f, 0.0f, -1.0f);
    }
}

// ---------------------------------------------------------------------------
// Push a ray start off the surface along the geometric normal, on the side it
// is leaving by.
//
// THE OFFSET IS MEASURED IN FLOAT PRECISION, NOT IN METRES, and getting that
// wrong is how a wood ends up with black grass.
//
// A constant epsilon is wrong here: the world runs to kilometres and a float
// coordinate loses precision as it grows, so an offset that clears the surface
// at the origin sits inside it a mile out. The previous form scaled with the
// point's magnitude -- correct in shape -- but at 1e-4 per metre, which is
// enormous. At a kilometre from the origin it pushed the ray start a WHOLE
// VOXEL off the surface, and at three kilometres, three voxels.
//
// In open ground nobody would notice. In grass, a blade is one voxel wide and
// its neighbours are four away, so every shadow ray was starting inside or
// behind the next blade along -- occlusion that is not there, reported as
// shadow. The floor went black, and it went blacker the further you walked from
// the origin, which is why it only appeared once the spawn stopped being fixed.
//
// What actually has to be cleared is a few units in the last place. An ULP at
// distance d is about d * 2^-23, so 17 of them is d * 2e-6: six millimetres at
// three kilometres rather than thirty centimetres, and the same 17 ULP of
// margin everywhere. The floor keeps it safe close to the origin, where the
// relative term underflows.
// ---------------------------------------------------------------------------
float3 offsetRay(float3 p, float3 ng, float3 dir)
{
    const float scale = max(3e-5f, 2e-6f * maxComp(abs(p)));
    return p + faceforward3(ng, dir) * scale;
}

// ---------------------------------------------------------------------------
// Projecting a world point back into a camera, in pixels.
//
// The exact inverse of the ray construction in main(), and it lives beside it
// so the two cannot drift: if the camera basis ever changes shape, both ends
// fail to compile together rather than one end quietly producing motion vectors
// for a camera the renderer no longer has.
//
// Returns false behind the camera, where the perspective divide flips sign and
// the answer would be a plausible-looking pixel in the wrong half of the frame.
// ---------------------------------------------------------------------------
bool projectToPixel(V6Camera c, float3 p, uint2 dim, out float2 outPix)
{
    outPix = float2(0.0f, 0.0f);
    const float3 d = p - c.pos;
    const float zc = dot(d, c.w);
    if (zc <= 1e-4f)
        return false;
    const float sx = dot(d, c.u) / (zc * c.halfW);
    const float sy = dot(d, c.v) / (zc * c.halfH);
    outPix = float2((sx + 1.0f) * 0.5f * float(dim.x), (1.0f - sy) * 0.5f * float(dim.y));
    return true;
}

// Depth as a rasteriser would have left it: the standard DirectX perspective
// mapping of view-space z into [0,1]. DLSS is told it is hardware depth, so it
// has to be that shape and not linear metres -- and because the projection is
// written out here rather than inherited from a matrix nobody owns, it is
// guaranteed to be the same projection the motion vectors were made with.
float ndcDepth(float viewZ, float n, float f)
{
    if (viewZ <= n)
        return 0.0f;
    return saturate((f / (f - n)) * (1.0f - n / viewZ));
}

// ---------------------------------------------------------------------------
// What a hit turned out to be. The equivalent of v2's PathVertex, except that
// it is a local variable in the caller rather than something reached through
// two payload registers holding a pointer.
// ---------------------------------------------------------------------------
struct Hit
{
    bool hit;
    float t;
    float3 p;
    float3 ng; // geometric, for offsetting the next ray
    float3 ns; // shading, for the BSDF frame
    Surface mat;
};

Hit traceRadiance(float3 o, float3 d)
{
    Hit h;
    h.hit = false;
    h.t = 0.0f;
    h.p = float3(0.0f);
    h.ng = float3(0.0f, 1.0f, 0.0f);
    h.ns = float3(0.0f, 1.0f, 0.0f);
    h.mat.type = MAT_OPAQUE;
    h.mat.albedo = float3(0.0f);
    h.mat.roughness = 1.0f;
    h.mat.specular = 0.0f;
    h.mat.translucency = 0.0f;
    h.mat.ior = 1.0f;

    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-5f;
    ray.TMax = INF;

    // FORCE_OPAQUE IS A PROMISE, NOT A CHANGE. Every geometry in this scene is
    // already built with RtGeometryFlags::Opaque, so no any-hit shader can ever
    // run and not one ray behaves differently -- but the flag has to be on the
    // QUERY for the compiler to know that. Given it, the whole any-hit path
    // disappears: Proceed() is known to complete in one go, the state machine
    // it would otherwise have to keep for a re-entrant traversal is gone, and
    // the registers that state machine occupied go back to the shader.
    RayQuery<RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_FORCE_OPAQUE, 0xFF, ray);
    q.Proceed(); // opaque by construction: nothing to run, one call is enough

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return h;

    const V6Instance inst = gInstances[q.CommittedInstanceID()];
    const uint packed = loadPackedTri(inst.triOffset, q.CommittedPrimitiveIndex());

    h.hit = true;
    h.t = q.CommittedRayT();
    h.p = o + d * h.t;

    // Object to world for a normal. Every transform in this scene is a quarter
    // turn about Y plus a translation, so the 3x3 is orthonormal and IS its own
    // inverse transpose -- no adjugate needed, which is the other thing the
    // quarter-turn-only rule buys besides keeping the voxel silhouette crisp.
    const float3x4 o2w = q.CommittedObjectToWorld3x4();
    const float3 nObj = faceNormal(packed >> 8);
    // NO normalize(). Every transform in this scene is a quarter turn about Y
    // plus a translation, so the 3x3 is a signed permutation of the identity --
    // and nObj is a unit axis. The product is therefore another unit axis with
    // exactly one non-zero component of exactly +/-1, and normalising it is a
    // reciprocal square root of exactly 1.0 on every hit in the frame.
    const float3 ng = mul((float3x3)o2w, nObj);
    h.ng = ng;
    h.ns = ng;

    if (inst.kind == KIND_WATER)
    {
        // A shallow ripple, in the shading normal only. Two crossed wave trains
        // so it does not read as corduroy.
        const float3 p = h.p;
        const float w1 = sin(p.x * 1.7f + p.z * 0.9f) + sin(p.x * 0.6f - p.z * 2.1f) * 0.7f;
        const float w2 = cos(p.z * 1.9f - p.x * 0.4f) + cos(p.z * 0.8f + p.x * 1.5f) * 0.7f;
        h.ns = normalize(float3(w1 * 0.014f, 1.0f, w2 * 0.014f));
        h.mat.type = MAT_DIELECTRIC;
        h.mat.ior = 1.333f;
        h.mat.roughness = 0.0f;
        h.mat.albedo = float3(1.0f);
        return h;
    }

    const V6Material L = gMaterials[groundShade(packed & 0xFFu, h.p, ng)];
    h.mat.type = MAT_OPAQUE;
    h.mat.albedo = L.albedo;
    h.mat.roughness = L.roughness;
    h.mat.specular = L.specular;
    h.mat.translucency = L.translucency;

    if (inst.kind == KIND_TREE)
    {
        // A few percent of per-tree hue. Nine models over thousands of trees
        // would otherwise show their repeat; this is enough to break it without
        // making the stand look like a paint chart.
        h.mat.albedo *= inst.tint;
    }

    // Foliage is thin enough to light from behind, so its faces are treated as
    // two-sided: turn the normal to the viewer rather than letting a backfacing
    // needle voxel go black.
    if (L.translucency > 0.0f)
    {
        h.ns = faceforward3(h.ns, -d);
        h.ng = faceforward3(h.ng, -d);
    }
    return h;
}

// The cheapest occlusion query the hardware can express: first hit ends the
// search, and there is no shading of any kind attached to it. In a conifer
// canopy most shadow rays die inside the first metre of foliage, and this is
// most of the frame.
bool traceOccluded(float3 o, float3 d, float tmax)
{
    RayDesc ray;
    ray.Origin = o;
    ray.Direction = d;
    ray.TMin = 1e-5f;
    ray.TMax = tmax;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE, 0xFF,
                     ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sun.
// ---------------------------------------------------------------------------
void sampleSunSplit(Hit h, Frame fr, float3 wo, inout Rng rng, BlueNoise bn, int depth,
                    out float3 sumDiff, out float3 sumSpec)
{
    sumDiff = float3(0.0f);
    sumSpec = float3(0.0f);

    const float3 Lsun = keyRadiance(gParams.sky);
    if (isBlack(Lsun))
        return;

    // MORE THAN ONE RAY AT THE SAME LIGHT. The extra samples are spent only on
    // the vertices that can pay for them: by the second bounce the throughput
    // is small, roulette is already killing paths, and what those vertices
    // contribute is too dim for their variance to show.
    const int n = (depth < gParams.shadowRayDepth) ? max(1, gParams.shadowRays) : 1;

    const Frame sf = Frame.make(keyDir(gParams.sky));
    const float lightPdf = uniformConePdf(keyCosR(gParams.sky));

    for (int i = 0; i < n; ++i)
    {
        // THE SHADOW RAY IS THE DIMENSION THAT MATTERS MOST HERE. This scene
        // is sun-dominated and its noise is overwhelmingly the sun's own
        // penumbra through a canopy, so if only one sample in the whole path
        // were to get the mask it would be this one.
        const float2 uSun = bn.next2DIf(i < 8, kBnDimSun + uint(i), rng);
        const float3 wWorld = sf.toWorld(sampleUniformCone(uSun, keyCosR(gParams.sky)));
        const float3 wi = fr.toLocal(wWorld);

        // A needle transmits, so a direction on the far side is still a valid
        // light path; an opaque surface below its own horizon is not.
        if (wi.z <= 0.0f && h.mat.translucency <= 0.0f)
            continue;

        float pdf = 0.0f;
        float3 fD, fS;
        evalOpaqueSplit(h.mat, wo, wi, fD, fS, pdf);
        if (pdf <= 0.0f || (isBlack(fD) && isBlack(fS)))
            continue;

        if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF))
            continue;

        const float w = powerHeuristic(1.0f, lightPdf, 1.0f, pdf);
        const float3 common = Lsun * (abs(wi.z) * w / lightPdf);
        float3 cD = fD * common;
        float3 cS = fS * common;

        // Fireflies: a grazing specular lobe against a 10^5 sun radiance can
        // produce a single sample worth more than the rest of the pixel. Only
        // the indirect bounces are clamped -- clamping the first would visibly
        // dim every sunlit surface in the frame.
        //
        // Applied per sample rather than to the mean: the ceiling is on what a
        // single estimate may be worth, and averaging first would let one spike
        // drag the mean up to the limit and hold it there.
        //
        // The ceiling is on the SUM of the two lobes, exactly as it was before
        // they were told apart, and both are scaled by the one factor -- so
        // splitting the channels cannot change what the clamp does.
        if (depth > 0 && gParams.clampIndirect > 0.0f)
        {
            const float m = maxComp(cD + cS);
            if (m > gParams.clampIndirect)
            {
                const float k = gParams.clampIndirect / m;
                cD *= k;
                cS *= k;
            }
        }
        sumDiff += cD;
        sumSpec += cS;
    }

    // Rays that returned nothing -- below the horizon, occluded, black BSDF --
    // are zeros in the average, not skipped. Dividing by the number that got
    // through instead would turn a half-shadowed vertex into a fully lit one.
    const float inv = 1.0f / float(n);
    sumDiff *= inv;
    sumSpec *= inv;
}

// The composited form, for every vertex whose lobe split nobody downstream
// asks about. One implementation, so the two cannot disagree.
// PASSES A SWITCHED-OFF SAMPLER, and that is the whole reason this wrapper did
// not simply gain a parameter. It is called only from depth > 0, where a
// screen-space pattern describes nothing -- the paths reaching those vertices
// left the pixel long ago. Making that explicit here means the depth test lives
// in one place rather than being re-derived at each call site.
float3 sampleSun(Hit h, Frame fr, float3 wo, inout Rng rng, int depth)
{
    float3 dif, spc;
    sampleSunSplit(h, fr, wo, rng, BlueNoise.make(uint2(0u, 0u), 0u, 0u), depth, dif, spc);
    return dif + spc;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sky dome.
//
// WHY THIS EXISTS AT ALL, when the dome was already being collected. A path
// that escapes the canopy lands on the sky and the dome arrives at full
// weight. That is correct, and before this it was the ONLY way light ever
// reached a shadowed pixel -- under a canopy the sun is occluded by
// definition, so the dome is the whole of the light.
//
// The trouble is what reaching it that way costs. It takes a whole path
// segment, and that segment has to survive two things first:
//
//   RUSSIAN ROULETTE, which at a foliage albedo of 0.19 kills roughly four
//   paths in five at the first bounce. Unbiased, so the film converges -- but
//   what it converges FROM is a spike distribution, most pixels holding zero
//   and a few holding a large escape. A tone curve compresses the spikes and
//   leaves the zeros alone, so the eye reads the mean as far darker than it is.
//
//   THE DEPTH BUDGET, which at the last vertex draws a direction, updates the
//   throughput, and then exits the loop without ever tracing it. That one is
//   not variance at all -- it is energy the estimator simply drops, and it
//   drops it in exactly the places that are already darkest.
//
// A dome sample here is an occlusion ray and nothing more. It does not
// continue, so roulette cannot take it, and it is taken at every vertex
// INCLUDING the last one -- which is where the truncation loss lived.
//
// THE SAMPLER IS COSINE-WEIGHTED ABOUT THE SHADING NORMAL, deliberately the
// same shape the diffuse lobe draws from. As a DISTRIBUTION that buys nothing:
// MIS between two identical strategies is two half-weight copies of one
// estimator. What it buys is that the two copies are INDEPENDENT and one of
// them is unconditional, so the variance of the sky term halves and the half
// that does not depend on surviving roulette is the half the shadows were
// missing. Importance-sampling the Perez dome by luminance would do better
// still; it needs a CDF rebuilt whenever the sun moves, which this does not.
//
// THE DOME ONLY. The solar disk is next-event-sampled on its own cone in
// sampleSunSplit, and skyDome() excludes the disk entirely -- so the two light
// sources never overlap, and neither MIS pair needs to know about the other.
// ---------------------------------------------------------------------------

// The pdf the dome sampler would have used for a direction leaving a surface
// with this shading normal.
//
// It has to be evaluable for ANY direction, not just for ones this sampler
// drew, because the escape path asks it the reverse question: "could the light
// sampler have found the direction the BSDF chose?" Without an answer to that
// there is no MIS weight, and without the weight the dome gets counted twice.
float skyDomePdf(float3 ns, float3 wWorld)
{
    return max(0.0f, dot(ns, wWorld)) * INV_PI;
}

void sampleSkySplit(Hit h, Frame fr, float3 wo, inout Rng rng, BlueNoise bn, int depth,
                    out float3 sumDiff, out float3 sumSpec)
{
    sumDiff = float3(0.0f);
    sumSpec = float3(0.0f);
    if (gParams.skyRays <= 0)
        return;

    // The same staircase the sun uses, and for the same reason: by the second
    // bounce the throughput is small enough that this vertex's variance does
    // not show in the frame.
    const int n = (depth < gParams.skyRayDepth) ? max(1, gParams.skyRays) : 1;

    for (int i = 0; i < n; ++i)
    {
        // Drawn in the local frame, so the sample is already the direction the
        // diffuse lobe cares about and no rotation is needed to get its pdf.
        const float3 wi = sampleCosineHemisphere(bn.next2DIf(i < 8, kBnDimSky + uint(i), rng));
        const float lightPdf = wi.z * INV_PI;
        if (lightPdf <= 1e-6f)
            continue;

        const float3 wWorld = fr.toWorld(wi);

        float pdf = 0.0f;
        float3 fD, fS;
        evalOpaqueSplit(h.mat, wo, wi, fD, fS, pdf);
        if (pdf <= 0.0f || (isBlack(fD) && isBlack(fS)))
            continue;

        const float3 Lsky = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, wWorld);
        if (isBlack(Lsky))
            continue;

        if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF))
            continue;

        const float w = powerHeuristic(1.0f, lightPdf, 1.0f, pdf);
        const float3 common = Lsky * (abs(wi.z) * w / lightPdf);
        float3 cD = fD * common;
        float3 cS = fS * common;

        // The same indirect ceiling the sun term carries, applied the same way
        // -- per sample, on the sum of the two lobes. The dome is four orders
        // of magnitude below the solar disk so it will rarely reach it, but a
        // grazing specular lobe can still manufacture a spike and the clamp
        // costs nothing when it does not fire.
        if (depth > 0 && gParams.clampIndirect > 0.0f)
        {
            const float m = maxComp(cD + cS);
            if (m > gParams.clampIndirect)
            {
                const float k = gParams.clampIndirect / m;
                cD *= k;
                cS *= k;
            }
        }
        sumDiff += cD;
        sumSpec += cS;
    }

    // Occluded and below-horizon samples are zeros in the average, not skipped
    // -- the same rule as the sun, and for the same reason.
    const float inv = 1.0f / float(n);
    sumDiff *= inv;
    sumSpec *= inv;
}

float3 sampleSky(Hit h, Frame fr, float3 wo, inout Rng rng, int depth)
{
    float3 dif, spc;
    sampleSkySplit(h, fr, wo, rng, BlueNoise.make(uint2(0u, 0u), 0u, 0u), depth, dif, spc);
    return dif + spc;
}

// ---------------------------------------------------------------------------
// Aerial perspective, ray-marched through the world volume.
//
// WHY THIS IS A MARCH NOW AND USED TO BE A LOOKUP.
//
// The froxel grid was integrated front to back by a pass of its own, in a grid
// whose z axis WAS the view ray -- so a pixel's fog was a single fetch at its
// own depth. A world volume cannot do that: view rays cross it at every angle,
// so there is no axis to pre-integrate along. The integral has to be taken per
// pixel, here.
//
// That is the honest price of world-anchored fog, and it is not small: one
// fetch becomes kFogSteps of them. What it buys is fog that is the same fog
// from every direction, that does not swim when you turn, and whose brightness
// is not a function of how far you happen to be able to see.
//
// EXPONENTIAL STEPS, WHICH IS WHERE THE FROXEL GRID'S BEST PROPERTY COMES BACK.
// The volume is uniform in space, but the MARCH need not be uniform along the
// ray. Steps grow geometrically with distance, so the first few metres -- where
// a beam through the canopy is actually resolved -- take most of the samples,
// and the far haze, which is smooth, takes the few it needs. The same reasoning
// the old exponential slices used, moved from the storage to the integration.
//
// The transmittance is handed back separately from the composited result so the
// demodulated channels can be attenuated by the SAME number -- fog that
// disagreed between the two would show as a halo the moment the textures were
// multiplied back on.
// ---------------------------------------------------------------------------
static const int kFogSteps = 40;

// One sample of the volume: the near cascade if the point is inside it, the far
// one otherwise, nothing past the edge of both. The margin keeps a trilinear
// tap from reaching across the boundary and pulling in the clamped border.
bool fogSampleVolume(float3 p, out float4 s)
{
    s = float4(0.0f, 0.0f, 0.0f, 0.0f);
    const float3 uvw0 = fogWorldToUvw(p, gParams.fogOrigin0, float3(gParams.fogCell0));
    if (fogInside(uvw0, 0.002f)) {
        s = gFogVol0.SampleLevel(gFogSampler, uvw0, 0.0f);
        return true;
    }
    const float3 uvw1 = fogWorldToUvw(p, gParams.fogOrigin1, float3(gParams.fogCell1));
    if (fogInside(uvw1, 0.002f)) {
        s = gFogVol1.SampleLevel(gFogSampler, uvw1, 0.0f);
        return true;
    }
    return false;
}

void fogTerms(float2 pixelUv, float3 dir, float dist, out float T, out float3 inscatter)
{
    T = 1.0f;
    inscatter = float3(0.0f);
    if (gParams.fogDensity <= 0.0f || gParams.volFog == 0)
        return;

    const float tNear = 0.25f;
    const float tFar = min(dist, gParams.fogFar);
    if (tFar <= tNear)
        return;

    // THE LIGHTING THAT DOES NOT VARY ALONG THE RAY, hoisted out of the loop.
    //
    // The sun's irradiance through the phase lobe for this view direction, and
    // the dome. Neither depends on where along the ray we are -- only on the
    // VISIBILITY of each, which is exactly what the volume stores. This is the
    // whole reason it can store visibility instead of radiance.
    //
    // wo points back toward the camera, which is the convention fogPhase is
    // written for; handing it the inward vector flips the lobe silently.
    const float3 wo = -dir;
    const float3 sunTerm = keyRadiance(gParams.sky) * keySolidAngle(gParams.sky) *
                           fogPhase(wo, keyDir(gParams.sky), gParams.fogAniso);
    const float3 skyTerm = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, float3(0.0f, 1.0f, 0.0f)) * gParams.fogAmbient;

    // A SPATIAL DITHER, AND DELIBERATELY NOT A TEMPORAL ONE.
    //
    // The geometric steps have to be broken up per pixel or they print
    // themselves on the screen as concentric shells. What they must NOT be
    // broken up by is TIME, and doing that was a bug that showed as the fog
    // ghosting through the trees whenever the camera moved quickly.
    //
    // The march writes its result into Lemit, which is handed to Ray
    // Reconstruction. RR accumulates temporally along the SURFACE motion
    // vectors -- but fog lives in the volume in FRONT of the surface, and its
    // parallax is not the surface's. Feed that a fresh white-noise value every
    // frame and RR has to reproject noise it cannot place: the error trails
    // behind, and it trails worst exactly where motion vectors are least
    // reliable and disocclusion is constant, which is a canopy. With the
    // denoiser off the accumulator simply averaged the noise away, which is why
    // the artefact vanished with upscaling disabled and looked like an
    // upscaler bug rather than a fog one.
    //
    // The temporal filtering this medium needs already exists, in the right
    // place: the VOLUME is blended over frames in VolFogInject. The march on
    // top of it should be a deterministic function of where the camera is, so
    // that a still camera yields a still image and a moving one yields only the
    // change that actually moved.
    //
    // Interleaved gradient noise: high frequency, well distributed, and fixed
    // per pixel. It breaks the shells as well as the hash did and gives RR a
    // signal that holds still.
    const float2 pixf = pixelUv * float2(gParams.dim);
    const float jitter =
        frac(52.9829189f * frac(0.06711056f * pixf.x + 0.00583715f * pixf.y));

    const float ratio = tFar / tNear;
    float3 scattering = float3(0.0f);
    float trans = 1.0f;
    float tPrev = 0.0f;

    for (int i = 1; i <= kFogSteps; ++i) {
        const float t = tNear * pow(ratio, float(i) / float(kFogSteps));
        const float ds = t - tPrev;
        const float3 p = gParams.cam.pos + dir * (tPrev + ds * jitter);
        tPrev = t;

        float4 s;
        if (!fogSampleVolume(p, s))
            continue;

        const float density = s.a;
        if (density <= 1e-6f)
            continue;

        const float stepT = exp(-density * ds);

        // THE ANALYTIC INTEGRAL OF THE STEP, not radiance times thickness.
        // Scattering and absorption happen together: light scattered at the far
        // end of a step is attenuated by the near end of the same step. The
        // closed form saturates, where multiplying through would let the fog
        // gain energy with density instead.
        const float3 stepL = sunTerm * s.r + skyTerm * s.g;
        scattering += stepL * (1.0f - stepT) * trans;
        trans *= stepT;

        // Nothing behind this is going to be seen through it.
        if (trans < 0.003f)
            break;
    }

    inscatter = scattering;
    T = trans;
}

float3 applyFog(float2 pixelUv, float3 L, float3 dir, float dist)
{
    float T;
    float3 inscatter;
    fogTerms(pixelUv, dir, dist, T, inscatter);
    return L * T + inscatter;
}

// ---------------------------------------------------------------------------
// One camera path, start to finish.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gParams.dim.x || tid.y >= gParams.dim.y)
        return;
    const uint2 pix = tid.xy;
    const uint pixIndex = pix.y * gParams.dim.x + pix.x;

    // One PCG stream per pixel, advanced by the frame index. Seeded from the
    // monotonic tick, not the accumulation index: while the camera moves the
    // accumulation restarts every frame, and seeding from it would redraw the
    // SAME sample over and over.
    Rng rng;
    rng.init(
        uint64_t(pixIndex) + 1ull,
        uint64_t(gParams.tick) * 0x9E3779B97F4A7C15ull + uint64_t(gParams.seed)
    );

    // The blue-noise mask, keyed on the PIXEL and the TICK.
    //
    // The tick and not the accumulation index, for exactly the reason the rng
    // seed above uses it: while the camera moves the film restarts every frame,
    // and an index that keeps returning to zero would freeze the mask into a
    // fixed dither -- the one failure mode a blue-noise pattern has, and a far
    // uglier one than the white noise it replaced.
    const BlueNoise bn = BlueNoise.make(pix, gParams.tick, uint(gParams.blueNoise));

    // The jitter is a whole-frame offset from a Halton sequence, not a per-pixel
    // random one. It is what antialiases the image as samples accumulate: a
    // low-discrepancy sequence covers the pixel evenly in a few dozen frames,
    // where white noise leaves parts of it unvisited for runs at a time.
    const float px = float(pix.x) + 0.5f + gParams.jitter.x;
    const float py = float(pix.y) + 0.5f + gParams.jitter.y;

    const V6Camera c = gParams.cam;
    const float sx = (2.0f * px / float(gParams.dim.x) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(gParams.dim.y)) * c.halfH;

    float3 o = c.pos;
    float3 d = normalize(c.w + c.u * sx + c.v * sy);
    if (c.aperture > 0.0f)
    {
        // Everything on the focal plane stays put; only the origin moves, which
        // is what puts the plane in focus and blurs by distance from it.
        const float3 focal = o + d * (c.focusDist / dot(d, c.w));
        // THE JITTER ABOVE IS DELIBERATELY LEFT ALONE. It is one offset for the
        // whole frame, which is what DLSS is told it is; making it per-pixel
        // would be a blue-noise pattern the upscaler is actively compensating
        // for in the wrong direction. The lens is a different matter -- it is a
        // genuine per-pixel 2D sample and bokeh noise is exactly the kind the
        // mask disperses well.
        const float2 l = sampleConcentricDisk(bn.next2D(kBnDimLens, rng));
        o = o + c.u * (l.x * c.aperture * 0.5f) + c.v * (l.y * c.aperture * 0.5f);
        d = normalize(focal - o);
    }

    const float3 primaryDir = d;
    float3 L = float3(0.0f), beta = float3(1.0f);

    // -- PHASE B: the same radiance, kept in three parts ---------------------
    //
    // WHAT wDiff IS. Past the primary vertex nobody tracks lobes any more: the
    // path carries one throughput and the light that comes back down it is
    // whatever it is. But that light was let through the primary surface BY a
    // lobe, and which one decides whether the albedo or the Fresnel term is the
    // factor to divide out. sampleOpaque draws a direction from one lobe and
    // then evaluates BOTH -- one-sample MIS over the lobes -- so the honest
    // answer is not "diffuse" or "specular" but the fraction each contributed
    // at the sampled direction. That fraction is wDiff, computed once at depth
    // 0, and every later contribution is split by it.
    //
    // Set to 1 up front so a path that never gets a primary bounce -- the ray
    // escapes, or the BSDF is black -- puts what it has in the diffuse channel
    // rather than nowhere.
    float3 Ldiff = float3(0.0f), Lspec = float3(0.0f), Lemit = float3(0.0f);
    float wDiff = 1.0f;
    float3 demodAlbedo = float3(0.0f);

    // The guides, filled in from the primary hit below. Held here rather than
    // written where they are found so there is exactly one store per texture
    // per pixel, on a path that also has to handle the ray escaping.
    float3 gAlbedo = float3(0.0f), gSpecular = float3(0.0f), gNormal = -primaryDir;
    float gRough = 1.0f, gViewZ = 0.0f;
    bool specularBounce = true; // the camera ray itself carries no light pdf
    float bsdfPdf = 1.0f;
    // The pdf the DOME sampler would have had for the direction this path is
    // currently travelling, carried forward from the vertex that chose it so
    // the escape branch can weight the two strategies against each other.
    // Zero means "no light sampler could have produced this direction", which
    // is the truth for the camera ray, for a specular bounce, and for every
    // frame where sky NEE is switched off.
    float skyPdfPrev = 0.0f;
    bool inWater = false;
    float primaryDist = -1.0f;

    // Beer-Lambert extinction under water, from the per-metre transmittance the
    // material carries. Hoisted out of the loop: it is three logs of constants.
    const float3 waterSigma = float3(-log(0.42f), -log(0.62f), -log(0.55f));

    // -- the neural radiance cache: is this a training path? -----------------
    //
    // HASHED, NOT DRAWN FROM rng. Taking a number out of the pixel's PCG stream
    // would shift every random number after it, so switching the cache on would
    // change the image even in the frames where it predicts nothing. This has to
    // be a decision the renderer can make without disturbing anything else.
    // The primary vertex, kept for ReSTIR: resampling shades x1 using a sample
    // point that may have been found by a different pixel, so it needs to
    // re-evaluate the geometry from HERE rather than reuse the throughput the
    // path happened to carry.
    float3 primaryPos = float3(0.0f), primaryNormal = float3(0.0f, 1.0f, 0.0f);
    bool havePrimary = false;

    // The first indirect vertex, which is the candidate ReSTIR resamples and
    // also the point the radiance cache is asked about. One record serves both.
    bool giHaveVertex = false;
    float3 giPos = float3(0.0f), giNormal = float3(0.0f, 1.0f, 0.0f);
    float3 giBeta = float3(1.0f), giLBefore = float3(0.0f);

#ifdef V2_NRC
    const uint nrcHash = (uint(pixIndex) * 73856093u) ^ (uint(gParams.tick) * 19349663u);
    const bool nrcTraining =
        gParams.nrcMode != 0 && (nrcHash % uint(max(1, gParams.nrcTrainEvery))) == 0u;
    bool nrcHaveVertex = false;
    float3 nrcBeta = float3(1.0f), nrcLBefore = float3(0.0f);
    NrcQuery nrcQ;
#endif

    for (int depth = 0; depth < gParams.maxDepth; ++depth)
    {
        const Hit h = traceRadiance(o, d);

        if (inWater)
            beta *= exp(waterSigma * -(h.hit ? h.t : 60.0f));

        if (!h.hit)
        {
            // Escaped: the sky. The dome is never light-sampled, so it always
            // arrives at full weight; the disk may have been, so it is weighted
            // against the cone pdf that could have found it.
            float3 sky = skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, d);
            // THE DOME IS NOW REACHABLE TWO WAYS, so it can no longer arrive at
            // full weight. Before sky NEE existed this was correct because
            // continuing the path was the only strategy that could find the
            // dome; now sampleSkySplit finds it too, and a pair of strategies
            // that both pay in full is the textbook double count -- it would
            // show up as the whole sky getting brighter, not as an error.
            //
            // Guarded on skyPdfPrev rather than on the toggle so that turning
            // sky NEE off restores the old weight exactly, with no second place
            // to keep in step.
            if (!specularBounce && skyPdfPrev > 0.0f)
                sky *= powerHeuristic(1.0f, bsdfPdf, 1.0f, skyPdfPrev);
            // The moon rides in the same slot as the sun disk: same cone size,
            // same MIS weight, and exactly one of them is ever non-zero
            // because they sit at opposite points of the sky.
            const float3 disk = skyDisk(gParams.sky, d) + moonDisk(gParams.sky, d, gMoonTex, gMoonSamp);
            if (!isBlack(disk))
            {
                const float lightPdf = uniformConePdf(keyCosR(gParams.sky));
                const float w = specularBounce ? 1.0f : powerHeuristic(1.0f, bsdfPdf, 1.0f, lightPdf);
                sky += disk * w;
            }
            // -- THE DECK, over the sky and only down the PRIMARY ray --------
            //
            // Composited as a surface with alpha rather than blended against
            // the raw sky, so an opaque cloud hides the sun DISK completely
            // instead of letting a fifth of a very bright disk through as a
            // crisp circle. The brightness still arrives -- as the deck
            // scattering the sun, which is what the phase function and the
            // light march are for, so a cloud in front of the sun glows.
            //
            // depth == 0 ONLY. A miss at depth > 0 is an indirect bounce that
            // escaped, and putting a 28-step march with a 4-step light march
            // inside it on every one of those would cost more than the entire
            // path trace. The sky those bounces see is the clear-sky dome, which
            // is the same approximation every other cache in here already makes.
            if (depth == 0 && gParams.clouds != 0)
            {
                // THE UNBOOSTED DISC, THEN THE DECK'S OWN NIGHT KEY. Taking
                // keyRadiance here took moonKey with it -- 300, calibrated for
                // surfaces lit through NEE -- while ambC below comes from the
                // dome with no boost at all. sunC 1.62 against ambC 0.003 is
                // the white deck over the black sky: the moon's surface
                // calibration leaking into a consumer that never wanted it.
                const float3 sunC = keyDiscRadiance(gParams.sky) *
                                    keySolidAngle(gParams.sky) * gParams.cloudSun *
                                    (gParams.sky.sunDir.y > 0.0f ? 1.0f : gParams.cloudMoonKey);
                const float3 ambC =
                    skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, float3(0.0f, 1.0f, 0.0f)) * gParams.cloudAmb;
                // The colour a far cloud fades INTO: the dome just above the
                // horizon, with no sun disk in it. Taking the disk here would
                // let a distant cloud fade toward the brightness of the sun
                // itself, which is the hole-in-the-cloud the alpha composite
                // exists to prevent.
                const float3 hazeC =
                    skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, normalize(float3(d.x, max(d.y, 0.02f), d.z)));
                const float4 deck =
                    cloudMarch(gCloudVol, gCloudSamp, gParams.cam.pos, d, keyDir(gParams.sky),
                               sunC, ambC, hazeC, gParams.cloudWindT,
                               cgIGN(float2(pix), gParams.tick));
                sky = sky * (1.0f - deck.a) + deck.rgb;
            }
            L += beta * sky;
            // The sky down the PRIMARY ray is modulated by no surface at all,
            // so it is emission. Deeper than that it arrived through the
            // primary lobe like everything else and splits with it.
            if (depth == 0)
            {
                Lemit += beta * sky;
                primaryDist = 1e5f;
            }
            else
            {
                const float3 c = beta * sky;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
            }
            break;
        }

        if (depth == 0)
        {
            primaryDist = h.t;

            // -- the guides ---------------------------------------------------
            // Split into the two albedos the denoiser demodulates by. It is
            // reconstructing LIGHTING, so anything that is really surface
            // colour has to be handed over separately or it gets smoothed away
            // with the noise -- which on a voxel wood means the needles.
            gViewZ = dot(h.p - c.pos, c.w);
            gNormal = h.ns;
            if (h.mat.type == MAT_DIELECTRIC)
            {
                // Water reflects and refracts and has no albedo of its own.
                // Telling the denoiser it is a smooth mirror is what stops it
                // trying to average a reflection across the surface.
                gAlbedo = float3(0.0f);
                gSpecular = float3(1.0f);
                gRough = 0.0f;
                // Water has no albedo to divide out, and everything it returns
                // came off a delta specular lobe -- so the diffuse channel is
                // empty and its divisor never gets used. One, not zero, so a
                // demodulate pass that divides unconditionally stays finite.
                demodAlbedo = float3(1.0f);
                wDiff = 0.0f;
            }
            else
            {
                const float kt = h.mat.translucency;
                // The same split sampleOpaque makes: what is left of the
                // diffuse lobe after the part that passes through.
                gAlbedo = h.mat.albedo * (1.0f - kt);
                gSpecular = float3(schlick(h.mat.specular, saturate(dot(h.ns, -d))));
                gRough = h.mat.roughness;
                // The plain albedo -- see the note on gDemodAlbedo at the top.
                demodAlbedo = h.mat.albedo;
            }
        }

        // -- water: a delta surface, so no light sampling here at all ---------
        if (h.mat.type == MAT_DIELECTRIC)
        {
            const bool entering = dot(d, h.ns) < 0.0f;
            const Frame fr = Frame.make(entering ? h.ns : -h.ns);
            const float3 wo = fr.toLocal(-d);
            const BsdfSample bs = sampleDielectric(h.mat, wo, entering, rng);
            if (bs.pdf <= 0.0f)
                break;

            beta *= bs.f * abs(bs.wi.z) / bs.pdf;
            const float3 wi = fr.toWorld(bs.wi);
            if (bs.transmitted)
                inWater = entering;
            o = offsetRay(h.p, h.ng, wi);
            d = wi;
            specularBounce = true;
            skyPdfPrev = 0.0f;  // a delta surface: no light sampler drew this
            continue;
        }

        // -- the neural radiance cache ---------------------------------------
        //
        // PLACED HERE because this is the first point where the vertex is fully
        // known -- position, shading normal, and the material's albedo and
        // roughness -- and still before any of the light gathered at it has been
        // added. Both halves need exactly that state.
        //
        // A training path REMEMBERS the vertex and keeps going: its job is to
        // find out what the cache should have said, which is the radiance the
        // rest of the path gathers from here. A querying path asks the network
        // and stops, which is the entire saving.
        // The primary vertex, captured once. Everything ReSTIR does at shading
        // time is measured from here.
        if (depth == 0 && !havePrimary) {
            primaryPos = h.p;
            primaryNormal = h.ns;
            havePrimary = true;
        }

        // The first indirect vertex: the candidate ReSTIR will resample. Taken
        // at depth 1 -- the point the camera ray's bounce landed on -- because
        // that is the vertex whose choice is responsible for the noise, and the
        // one a neighbouring pixel could plausibly have found too.
        if (depth == 1 && !giHaveVertex) {
            giHaveVertex = true;
            giPos = h.p;
            giNormal = h.ns;
            giBeta = beta;
            giLBefore = L;
        }

#ifdef V2_NRC
        if (gParams.nrcMode != 0 && depth >= gParams.nrcQueryDepth && !nrcHaveVertex)
        {
            nrcQ.pos = (h.p - gParams.nrcOrigin) * gParams.nrcInvExtent;
            nrcQ.dir = -d;
            nrcQ.normal = h.ns;
            nrcQ.albedo = h.mat.albedo;
            nrcQ.roughness = h.mat.roughness;

            if (nrcTraining)
            {
                nrcHaveVertex = true;
                nrcBeta = beta;
                nrcLBefore = L;
            }
            else if (gParams.nrcMode == 2)
            {
                // Evaluated ONCE. The split channels have to be fed as well or
                // the demodulated route loses this energy entirely, but that is
                // the same radiance divided two ways -- not a second query.
                const float3 c = beta * nrcEval(gNrcWeights, nrcQ);
                L += c;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
                break;
            }
        }
#endif

#if V2_SHARC_QUERY
        // -- the hash cache ---------------------------------------------------
        //
        // Same placement and the same argument as the probe query below: what
        // comes back is the radiance LEAVING this point, everything included,
        // so the path is finished and this vertex's own next event estimation
        // must not also run.
        //
        // Radiance, not irradiance -- so unlike the probe lookup there is no
        // albedo and no 1/pi here. The update pass stored what left the surface,
        // not what arrived at it.
        //
        // THE SEGMENT LENGTH TEST IS NOT OPTIONAL. A cache entry covers a whole
        // voxel of the hash grid, so a path segment shorter than that voxel
        // lands several neighbouring shading points in one cell and hands them
        // all the same answer. That is the blocky look SHaRC is blamed for, and
        // it is avoidable by simply carrying on tracing until the segment is
        // long enough to be filed honestly.
        if (gParams.giMode == 2 && depth >= gParams.giDepth &&
            sharcSegmentLongEnough(h.p, h.t))
        {
            SharcHitData hd;
            hd.positionWorld = h.p;
            hd.normalWorld = h.ng;

            float3 cached;
            if (SharcGetCachedRadiance(sharcParams(), hd, cached, false))
            {
                const float3 cs = beta * cached * gParams.giStrength;
                L += cs;
                Ldiff += cs * wDiff;
                Lspec += cs * (1.0f - wDiff);
                break;
            }
        }
#endif

        // -- the irradiance cache ---------------------------------------------
        //
        // PLACED HERE, ABOVE THIS VERTEX'S NEXT EVENT ESTIMATION, and the
        // placement IS the correctness argument.
        //
        // A probe holds the TOTAL irradiance arriving at a point: the sun it
        // can see, the sky it can see, and every bounce that ever reached it.
        // So the cache does not supplement this vertex's direct lighting, it
        // REPLACES it -- query it and the path is finished, because everything
        // the path would have gone on to gather is already inside the number.
        //
        // Run the sun and sky NEE below as well and the direct light is counted
        // twice: once sharply from the shadow ray and once smeared across a
        // six-metre probe grid. That does not look like an error, it looks like
        // a wood that gets brighter the more of it is cached -- which is the
        // hardest kind of bug to attribute, so the structure rules it out
        // instead. This sits above the NEE block and ends in a break, exactly
        // where the neural cache sits and for exactly the same reason.
        //
        // Dielectrics never reach here: that branch continues above.
        if (gParams.giMode == 1 && depth >= gParams.giDepth)
        {
            const float3 E = ddgiSampleIrradiance(h.p, h.ns, -d, gDdgiConsts,
                                                  gDdgiIrradiance, gDdgiDistance,
                                                  gDdgiProbeData, gDdgiSampler);
            // Lambert: a diffuse surface under an irradiance E leaves with
            // albedo/pi * E. The specular lobe goes with the rest of the path.
            // A probe grid holds no directional detail to give a mirror, and
            // pretending it does is what makes cached reflections read as fog.
            const float3 c = beta * h.mat.albedo * INV_PI * E * gParams.giStrength;
            L += c;
            Ldiff += c * wDiff;
            Lspec += c * (1.0f - wDiff);
            break;
        }

        const Frame fr = Frame.make(h.ns);
        const float3 wo = fr.toLocal(-d);
        if (wo.z > 0.0f)
        {
            // At the primary vertex the two lobes are wanted apart, and this is
            // the one place the BSDF can still tell them apart for free -- it is
            // being evaluated anyway. Deeper down the split is inherited from
            // wDiff, so the cheaper composited form is used.
            if (depth == 0)
            {
                float3 neeD, neeS, skyD, skyS;
                sampleSunSplit(h, fr, wo, rng, bn, depth, neeD, neeS);
                // The dome, sampled as its own light. Summed into the sun's
                // channels rather than kept apart: they are the same two lobes
                // of the same surface, and everything downstream -- the
                // demodulated split, ReSTIR, the denoiser guides -- asks about
                // the lobe, not about which light it came from.
                sampleSkySplit(h, fr, wo, rng, bn, depth, skyD, skyS);
                neeD += skyD;
                neeS += skyS;
                L += beta * (neeD + neeS);
                Ldiff += beta * neeD;
                Lspec += beta * neeS;
            }
            else
            {
                const float3 c =
                    beta * (sampleSun(h, fr, wo, rng, depth) + sampleSky(h, fr, wo, rng, depth));
                L += c;
                Ldiff += c * wDiff;
                Lspec += c * (1.0f - wDiff);
            }
        }

        // THE TWO DRAWS ARE SEQUENCED BY HAND, not left as two arguments to one
        // call. Argument evaluation order is unspecified, and these two are not
        // interchangeable: the first picks the lobe and the second the direction
        // within it, so a compiler free to swap them would silently produce a
        // different image on a different driver.
        BsdfSample bs;
        if (depth == 0)
        {
            const float uLobe = rng.nextFloat();
            const float2 uDir = bn.next2D(kBnDimBsdf, rng);
            bs = sampleOpaqueU(h.mat, wo, uLobe, uDir);
        }
        else
        {
            bs = sampleOpaque(h.mat, wo, rng);
        }
        if (bs.pdf <= 0.0f || isBlack(bs.f))
            break;

        // WHICH LOBE LET THIS PATH THROUGH, as a fraction rather than a choice.
        // Weighted by luminance because the split has to be one scalar for a
        // three-channel radiance, and luminance is what the eye -- and every
        // denoiser downstream -- weights by.
        if (depth == 0)
        {
            float3 fD, fS;
            float dummyPdf;
            evalOpaqueSplit(h.mat, wo, bs.wi, fD, fS, dummyPdf);
            const float lD = luminance(fD), lS = luminance(fS);
            wDiff = (lD + lS > 1e-9f) ? (lD / (lD + lS)) : 1.0f;
        }

        const float3 contrib = bs.f * (abs(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib))
            break;
        beta *= contrib;
        bsdfPdf = bs.pdf;
        specularBounce = bs.specular;

        const float3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

        // What the dome sampler would have thought of this direction, recorded
        // here because this is the one place both the shading normal and the
        // chosen direction are in hand. A specular lobe gets zero: a delta
        // direction is not one any light sampler could have drawn, so the
        // escape branch must pay it in full.
        skyPdfPrev =
            (gParams.skyRays > 0 && !bs.specular) ? skyDomePdf(h.ns, wi) : 0.0f;

        // Russian roulette on throughput, from the SECOND bounce. Starting it
        // late is the textbook advice and it is wrong here: a conifer canopy is
        // dark -- foliage albedo about 0.19 -- so a ray that enters one carries
        // almost nothing, and a late rrStart guarantees it several full bounces
        // of tree geometry first. It is unbiased either way; what changes is
        // variance per unit time, and early roulette wins that comfortably.
        if (depth >= gParams.rrStart)
        {
            const float q = min(0.95f, maxComp(beta));
            if (rng.nextFloat() > q)
                break;
            beta /= q;
        }
        if (maxComp(beta) <= 0.0f)
            break;
    }

    if (!allFinite(L))
        L = float3(0.0f);
    if (primaryDist > 0.0f)
    {
        float T;
        float3 inscatter;
        fogTerms((float2(pix) + 0.5f) / float2(gParams.dim), primaryDir, primaryDist, T,
                 inscatter);
        L = L * T + inscatter;
        // The haze dims what the surface sent and adds light of its own. The
        // dimming is a property of the path, so it scales both lobes; the added
        // light never touched the surface, so it is emission.
        Ldiff *= T;
        Lspec *= T;
        Lemit = Lemit * T + inscatter;
    }

    if (!allFinite(Ldiff)) Ldiff = float3(0.0f);
    if (!allFinite(Lspec)) Lspec = float3(0.0f);
    if (!allFinite(Lemit)) Lemit = float3(0.0f);

    // -----------------------------------------------------------------------
    // The guides.
    //
    // THE MOTION VECTOR IS EXACT, and it is exact for a reason that is specific
    // to this world: nothing in it moves. Only the camera does. So where a
    // surface was on screen last frame is not something anyone has to store or
    // interpolate -- it is this frame's hit point projected through last
    // frame's camera, and that is a closed-form answer with no reprojection
    // error in it at all. A rasteriser has to carry a previous transform per
    // object to get the same thing approximately.
    //
    // BOTH ENDS ARE UNJITTERED, AND GETTING THAT WRONG IS WHAT GHOSTED.
    //
    // The previous end is unjittered for free: prevCam is a camera basis and
    // carries no sub-pixel offset, so projecting through it lands where a
    // jitter-free renderer would have drawn the surface. The current end is
    // NOT the pixel centre. This tracer builds the camera ray through
    // pix + 0.5 + jitter, so the surface's unjittered screen position IS
    // pix + 0.5 + jitter -- the jitter is already baked into which surface the
    // pixel found, and subtracting a bare centre leaves it in the vector.
    //
    // THE TEST THAT SETTLES IT: stand still. With the camera not moving,
    // prevCam == cam, so prevPix comes back as pix + 0.5 + jitter and the
    // motion vector must be exactly zero. Against a bare centre it came back
    // as the frame's jitter instead -- up to half a pixel of whole-screen
    // wobble, re-rolled from the Halton sequence every frame, handed to a
    // reconstruction that was standing perfectly still.
    //
    // WHY THE CANOPY WORE IT AND THE GROUND DID NOT. Half a pixel of history
    // offset costs nothing where the neighbouring pixel looks the same -- bark,
    // moss, a fogged ridge. A pine needle is one voxel wide against the sky, so
    // half a pixel is a FULL-CONTRAST error, and Ray Reconstruction cannot tell
    // a misregistered needle from a moving one: it blends, and the blend is the
    // trail. Worse, the error IS the jitter, which is the very quantity DLSS
    // uses to place the incoming sample -- so the sub-pixel accumulation that
    // makes reconstruction work was being fed offsets it had already been told.
    //
    // DLSS is told the vectors are jitter-free (the MVJittered flag is not set,
    // and Streamline is given motionVectorsJittered = eFalse); this is what
    // makes that claim true. The jitter still reaches DLSS on its own, with the
    // sign flip that converts this tracer's convention to a rasteriser's -- see
    // in.jitterX in tracer.h.
    // -----------------------------------------------------------------------
    // -- hand the cache what this path actually found ------------------------
    //
    // THE TARGET IS THE RADIANCE PER UNIT THROUGHPUT AT THAT VERTEX, which is
    // what the cache is defined to return. L - nrcLBefore is everything gathered
    // after the vertex, and it arrived multiplied by the throughput the path
    // carried into it, so dividing that back out is what makes the answer a
    // property of the VERTEX rather than of the path that happened to reach it.
    //
    // Clamped on the way in. One path through a specular caustic can carry a
    // value orders of magnitude above the batch, and in fp16 training that is
    // not an outlier, it is an inf that poisons the network permanently.
    // -- ReSTIR GI ------------------------------------------------------------
    //
    // THE CANDIDATE IS WHAT THIS PATH FOUND, expressed as a property of the
    // POINT rather than of the path: L - giLBefore is everything gathered after
    // the vertex, and it arrived scaled by the throughput the path carried into
    // it, so dividing that out gives the radiance leaving that point. Which is
    // the only form another pixel could use.
    if (gParams.restirMode != 0)
    {
        Reservoir cand = restirEmpty();
        if (giHaveVertex)
        {
            const float3 Ls = clamp((L - giLBefore) / max(giBeta, float3(1e-3f)),
                                    float3(0.0f), float3(64.0f));
            // The candidate's weight is its target over the pdf it was drawn
            // with. It was drawn by BSDF sampling and giBeta already carries
            // f*cos/pdf, so the balance has been paid; offering it at its own
            // target value is the unweighted-RIS form for a single candidate.
            const float pHat = luminance(Ls);
            cand.xs = giPos;
            cand.ns = giNormal;
            cand.Ls = Ls;
            cand.wSum = pHat;
            cand.W = 0.0f;
            cand.M = 1u;
        }
        gRestirCandidate[pixIndex] = cand;
        gPrimaryPos[pix] = float4(primaryPos, havePrimary ? 1.0f : 0.0f);

        // -- and shade from LAST frame's resampled reservoir ------------------
        //
        // ONLY THE DIFFUSE HALF. Specular indirect is not reusable between
        // pixels -- a mirror lobe from half a metre away is looking somewhere
        // else entirely -- so the split the tracer already computed is used to
        // route only the diffuse part through resampling and leave the rest as
        // it was traced. wDiff is that split; see the note where it is set.
        if (gParams.restirMode == 2 && havePrimary && giHaveVertex)
        {
            const Reservoir fr = gRestirFinal[pixIndex];
            const float3 toS = fr.xs - primaryPos;
            const float d2 = dot(toS, toS);
            const float3 wi = (d2 > 1e-8f) ? toS * rsqrt(d2) : primaryNormal;
            const bool usable = fr.M > 0u && fr.W > 0.0f && d2 > 1e-8f &&
                                dot(primaryNormal, wi) > 0.0f;

            // ONLY SWAP WHEN THERE IS SOMETHING TO SWAP IN.
            //
            // This is the bug that made the first version of this pass produce
            // a black wood. The subtraction was unconditional and the addition
            // was not, so every pixel whose reservoir was empty -- which is most
            // of them for the first few frames, and any pixel whose reprojection
            // and every spatial tap were rejected -- lost its indirect light
            // entirely and got nothing back. A pixel with no usable reservoir
            // must keep exactly what the path tracer gave it.
            if (usable)
            {
                // MATCHED TO THE FORM THE TRACER ALREADY PRODUCES, so that a
                // reservoir holding this pixel's own single candidate reproduces
                // the traced tail exactly and the estimator provably reduces to
                // the path tracer when there is no reuse.
                //
                // The tail is beta_v * Ls, and for a cosine-sampled diffuse
                // bounce beta_v is just the albedo -- the cosine and the 1/pi
                // cancel against the pdf. So the reused form is the same
                // product with the resampling weight carrying the aggregation.
                const float3 resampled =
                    clamp(gAlbedo * fr.Ls * fr.W, float3(0.0f), float3(32.0f));
                const float3 tail = L - giLBefore;
                L = L - tail * wDiff + resampled;
                Ldiff = Ldiff - tail * wDiff + resampled;
            }
        }
    }

#ifdef V2_NRC
    if (nrcHaveVertex)
    {
        NrcSample smp;
        smp.pos = nrcQ.pos;
        smp.dir = nrcQ.dir;
        smp.normal = nrcQ.normal;
        smp.albedo = nrcQ.albedo;
        smp.roughness = nrcQ.roughness;
        smp.target = clamp((L - nrcLBefore) / max(nrcBeta, float3(1e-3f)),
                           float3(0.0f), float3(64.0f));
        smp.valid = 1u;
        // Slots collide -- there are more pixels than slots -- and that is
        // harmless: a collision means one stochastic sample replaces another,
        // and both were equally valid. NrcTrain clears each slot as it consumes
        // it, so nothing here is ever trained on twice.
        gNrcSamples[uint(pixIndex) % kNrcSlots] = smp;
    }
#endif

    if (gParams.writeGuides != 0)
    {
        // An escaped ray has no surface, so the sky is given a point far down
        // its own direction. It reprojects correctly under rotation, which is
        // the only motion the sky has.
        const float3 worldP = (primaryDist > 0.0f && primaryDist < 1e4f)
                                  ? (c.pos + primaryDir * primaryDist)
                                  : (c.pos + primaryDir * 1e4f);

        // THE JITTER BELONGS IN curPix, and leaving it out was the ghost. The
        // ray for this pixel was built through pix + 0.5 + jitter, so that --
        // not the pixel centre -- is where this surface sits in the camera's
        // own unjittered screen space. projectToPixel is the exact inverse of
        // that construction, so putting the jitter back makes both ends of the
        // subtraction the same kind of quantity.
        const float2 curPix =
            float2(float(pix.x) + 0.5f, float(pix.y) + 0.5f) + gParams.jitter;
        float2 prevPix;
        const bool ok = projectToPixel(gParams.prevCam, worldP, gParams.dim, prevPix);
        gGuideMotion[pix] = ok ? (prevPix - curPix) : float2(0.0f, 0.0f);

        gGuideAlbedo[pix] = float4(gAlbedo, 0.0f);
        gGuideSpecular[pix] = float4(gSpecular, 0.0f);
        // Already unit: it is either a face normal (a unit axis, see above), a
        // normalised water ripple, or the negated primary direction. And the
        // texture is half precision, which cannot hold the last-bit difference
        // a re-normalise would make even if there were one.
        gGuideNormRough[pix] = float4(gNormal, gRough);
        gGuideDepth[pix] = (gViewZ > 0.0f)
                               ? ndcDepth(gViewZ, gParams.depthNear, gParams.depthFar)
                               : 1.0f;
    }

    // -----------------------------------------------------------------------
    // PHASE B, the half of it that belongs here: the SPLIT.
    //
    // Written still modulated -- albedo and Fresnel are left in. Dividing them
    // out is Demodulate.cs.slang's job, and keeping the two steps apart is what
    // makes the pass reusable: the same three channels feed a demodulating
    // denoiser and, summed, reproduce gColor exactly.
    //
    // THAT IDENTITY IS THE TEST. Ldiff + Lspec + Lemit == L, to floating point,
    // on every pixel -- run v2 with --check-demod and it is asserted on the
    // host against the composited buffer.
    // -----------------------------------------------------------------------
    if (gParams.demodulate != 0)
    {
        gDiffRadiance[pix] = float4(Ldiff, 1.0f);
        gSpecRadiance[pix] = float4(Lspec, 1.0f);
        gEmission[pix] = float4(Lemit, 1.0f);
        gDemodAlbedo[pix] = float4(demodAlbedo, 1.0f);
    }

    // -- accumulate ----------------------------------------------------------
    //
    // NOT ALWAYS. Under Ray Reconstruction at one sample a frame there is no
    // film at all: the reconstruction carries the history, the host throws the
    // accumulator away every frame, and `frame` is therefore always zero. The
    // sum of one sample is that sample. Reading and writing a full-resolution
    // RGBA32F surface to work that out is tens of megabytes a frame of pure
    // ceremony, so the host says when the film is real and it is skipped when
    // it is not.
    if (gParams.keepFilm == 0)
    {
        gColor[pix] = float4(L, 1.0f);
        return;
    }

    // frame 0 overwrites rather than adds, which is how an invalidation clears
    // the film without a separate clear dispatch.
    float4 acc = float4(L, 1.0f);
    if (gParams.frame > 0u)
    {
        const float4 p0 = gAccum[pix];
        acc = float4(p0.xyz + acc.xyz, p0.w + 1.0f);
    }

    // Roll the window forward once it is full. Scaling both parts together
    // keeps the mean exactly where it is; only the WEIGHT of what comes next
    // changes. This is what lets the film track a moving sun continuously
    // instead of being thrown away and restarted every time it shifts.
    if (gParams.maxAccum > 0u && acc.w > float(gParams.maxAccum))
    {
        const float k = float(gParams.maxAccum) / acc.w;
        acc *= k;
    }
    gAccum[pix] = acc;

    gColor[pix] = float4(acc.xyz / max(1.0f, acc.w), 1.0f);
}

#if V2_SHARC_UPDATE
// ---------------------------------------------------------------------------
// The SHaRC update pass: sparse paths whose only job is to fill the cache.
//
// A SECOND ENTRY POINT IN THIS FILE RATHER THAN A FILE OF ITS OWN, deliberately.
// The update has to shade exactly the way the camera path shades -- the same
// sun and sky next event estimation, the same BSDF, the same offsets -- or the
// cache is a record of a slightly different renderer and every path that
// terminates into it steps sideways into that other world. Sharing the file
// shares the helpers by construction; a separate file would share them by
// somebody remembering to.
//
// ONE PATH PER 5x5 BLOCK, which is 4 % of the pixels. The block's pixel is
// picked from the frame's hash, so successive frames walk over the whole screen
// and the cache ends up covering everything the camera can see rather than a
// fixed lattice of points.
//
// EVERY SEGMENT IS TREATED INDEPENDENTLY. This is the part of the algorithm
// that looks wrong and is not: the throughput is reset to one at every bounce
// and the radiance gathered is reset to zero, because what a cache entry stores
// is the radiance leaving THAT point -- a property of the point, not of the
// path that happened to arrive there. SharcSetThroughput is how the segment's
// throughput gets folded back in for the entries further up the path, and
// SharcUpdateHit is what propagates a vertex's radiance to the ones behind it.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void sharcUpdateMain(uint3 tid: SV_DispatchThreadID)
{
    const uint2 blocks = (gParams.dim + 4u) / 5u;
    if (tid.x >= blocks.x || tid.y >= blocks.y)
        return;

    const uint blockIndex = tid.y * blocks.x + tid.x;

    Rng rng;
    rng.init(uint64_t(blockIndex) + 1ull,
             uint64_t(gParams.tick) * 0x9E3779B97F4A7C15ull + uint64_t(gParams.seed) + 7919ull);

    // Somewhere in this block, different every frame.
    const uint2 pix = min(tid.xy * 5u + uint2(uint(rng.nextFloat() * 5.0f),
                                              uint(rng.nextFloat() * 5.0f)),
                          gParams.dim - 1u);

    const float px = float(pix.x) + rng.nextFloat();
    const float py = float(pix.y) + rng.nextFloat();

    const V6Camera c = gParams.cam;
    const float sx = (2.0f * px / float(gParams.dim.x) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(gParams.dim.y)) * c.halfH;

    float3 o = c.pos;
    float3 d = normalize(c.w + c.u * sx + c.v * sy);

    const SharcParameters sp = sharcParams();
    SharcState state;
    SharcInit(state);

    // Reset at every vertex, not carried: see the note above.
    float3 throughput = float3(1.0f);

    for (int depth = 0; depth < gParams.maxDepth; ++depth)
    {
        const Hit h = traceRadiance(o, d);

        if (!h.hit)
        {
            // The sky closes the path and is written back along it. The disk is
            // included at full weight here because this path did not light
            // sample it on the way out -- there is no MIS partner to halve.
            SharcUpdateMiss(sp, state, skyDome(gParams.sky, gAtmoSkyView, gAtmoSamp, d) + skyDisk(gParams.sky, d));
            break;
        }

        // Water is a delta surface: it has no diffuse radiance to file under a
        // world position, so the path passes through without touching the cache.
        if (h.mat.type == MAT_DIELECTRIC)
        {
            const bool entering = dot(d, h.ns) < 0.0f;
            const Frame wfr = Frame.make(entering ? h.ns : -h.ns);
            const BsdfSample ws = sampleDielectric(h.mat, wfr.toLocal(-d), entering, rng);
            if (ws.pdf <= 0.0f)
                break;
            const float3 wwi = wfr.toWorld(ws.wi);
            o = offsetRay(h.p, h.ng, wwi);
            d = wwi;
            continue;
        }

        const Frame fr = Frame.make(h.ns);
        const float3 wo = fr.toLocal(-d);
        if (wo.z <= 0.0f)
            break;

        // The direct term at this vertex, by the same two estimators the camera
        // path uses. Depth is passed as 0 so both spend their full sample count:
        // an update path is rare enough that its vertices are worth resolving
        // properly, and what it writes is shared by every later camera path that
        // lands in the same cell.
        const float3 direct = sampleSun(h, fr, wo, rng, 0) + sampleSky(h, fr, wo, rng, 0);

        SharcHitData hd;
        hd.positionWorld = h.p;
        hd.normalWorld = h.ng;

        // Returns false when this vertex resolved from the cache and there is
        // nothing further to learn down this path.
        if (!SharcUpdateHit(sp, state, hd, direct, rng.nextFloat()))
            break;

        const BsdfSample bs = sampleOpaque(h.mat, wo, rng);
        if (bs.pdf <= 0.0f || isBlack(bs.f))
            break;

        const float3 contrib = bs.f * (abs(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib))
            break;
        throughput = contrib;

        const float3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

        // Hand the segment's throughput to the cache, then start the next one
        // from one again.
        SharcSetThroughput(state, throughput);
        throughput = float3(1.0f);

        // Roulette on the SEGMENT throughput. A cache-filling path wants to
        // reach further than a camera path -- the whole point is to record the
        // tail the camera path throws away -- so this starts later than
        // gParams.rrStart deliberately.
        if (depth >= max(gParams.rrStart, 3))
        {
            const float q = min(0.95f, maxComp(contrib));
            if (rng.nextFloat() > q)
                break;
        }
    }
}
#endif  // V2_SHARC_UPDATE
