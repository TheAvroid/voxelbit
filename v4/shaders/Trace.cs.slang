// ---------------------------------------------------------------------------
// Trace.cs.slang -- the whole renderer, as one compute shader.
//
// THIS FILE IS THE POINT OF v4. The integrator is v2's, unchanged in every
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
#include "Material.slang"
#include "Sky.slang"

// ---------------------------------------------------------------------------
// Bindings
// ---------------------------------------------------------------------------
RaytracingAccelerationStructure gScene;

// packTri(material, face), one uint16 per triangle, for every instance in the
// scene at once. An instance's triOffset plus the primitive index addresses it.
// See world.h for the allocator that hands out the ranges.
ByteAddressBuffer gTriPool;

StructuredBuffer<V4Instance> gInstances;
StructuredBuffer<V4Material> gMaterials;

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

cbuffer gParamsCB
{
    V4Params gParams;
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
// is leaving by. A constant epsilon is wrong at this scale: the world runs to
// kilometres and a voxel face is ten centimetres, so the offset scales with the
// point's own magnitude instead.
// ---------------------------------------------------------------------------
float3 offsetRay(float3 p, float3 ng, float3 dir)
{
    const float scale = 1e-4f * max(1.0f, maxComp(abs(p)));
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
bool projectToPixel(V4Camera c, float3 p, uint2 dim, out float2 outPix)
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

    RayQuery<RAY_FLAG_NONE> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, ray);
    q.Proceed(); // every geometry is opaque: nothing to run, one call is enough

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        return h;

    const V4Instance inst = gInstances[q.CommittedInstanceID()];
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
    float3 ng = normalize(mul((float3x3)o2w, nObj));
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

    const V4Material L = gMaterials[packed & 0xFFu];
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

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, ray);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sun.
// ---------------------------------------------------------------------------
float3 sampleSun(Hit h, Frame fr, float3 wo, inout Rng rng, int depth)
{
    const float3 Lsun = gParams.sky.sunRadiance;
    if (isBlack(Lsun))
        return float3(0.0f);

    // MORE THAN ONE RAY AT THE SAME LIGHT. The extra samples are spent only on
    // the vertices that can pay for them: by the second bounce the throughput
    // is small, roulette is already killing paths, and what those vertices
    // contribute is too dim for their variance to show.
    const int n = (depth < gParams.shadowRayDepth) ? max(1, gParams.shadowRays) : 1;

    const Frame sf = Frame.make(gParams.sky.sunDir);
    const float lightPdf = uniformConePdf(kSunCosThetaMax);

    float3 sum = float3(0.0f);
    for (int i = 0; i < n; ++i)
    {
        const float3 wWorld = sf.toWorld(sampleUniformCone(rng.next2D(), kSunCosThetaMax));
        const float3 wi = fr.toLocal(wWorld);

        // A needle transmits, so a direction on the far side is still a valid
        // light path; an opaque surface below its own horizon is not.
        if (wi.z <= 0.0f && h.mat.translucency <= 0.0f)
            continue;

        float pdf = 0.0f;
        const float3 f = evalOpaque(h.mat, wo, wi, pdf);
        if (isBlack(f) || pdf <= 0.0f)
            continue;

        if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF))
            continue;

        const float w = powerHeuristic(1.0f, lightPdf, 1.0f, pdf);
        float3 c = f * Lsun * (abs(wi.z) * w / lightPdf);

        // Fireflies: a grazing specular lobe against a 10^5 sun radiance can
        // produce a single sample worth more than the rest of the pixel. Only
        // the indirect bounces are clamped -- clamping the first would visibly
        // dim every sunlit surface in the frame.
        //
        // Applied per sample rather than to the mean: the ceiling is on what a
        // single estimate may be worth, and averaging first would let one spike
        // drag the mean up to the limit and hold it there.
        if (depth > 0 && gParams.clampIndirect > 0.0f)
        {
            const float m = maxComp(c);
            if (m > gParams.clampIndirect)
                c *= gParams.clampIndirect / m;
        }
        sum += c;
    }

    // Rays that returned nothing -- below the horizon, occluded, black BSDF --
    // are zeros in the average, not skipped. Dividing by the number that got
    // through instead would turn a half-shadowed vertex into a fully lit one.
    return sum * (1.0f / float(n));
}

// ---------------------------------------------------------------------------
// Aerial perspective, integrated in closed form along the primary segment.
//
// Depth cues in a wood come almost entirely from haze: without it a trunk forty
// metres away has the same contrast as one at four, and the stand collapses
// into a flat wall. Analytic rather than a traced medium -- one exp() instead
// of a whole extra integration, and being deterministic it adds no variance.
// ---------------------------------------------------------------------------
float3 applyFog(float3 L, float3 dir, float dist)
{
    if (gParams.fogDensity <= 0.0f)
        return L;
    const float y0 = gParams.cam.pos.y;
    const float travelled = min(dist, 4000.0f);
    const float y1 = y0 + dir.y * travelled;
    const float H = max(1.0f, gParams.fogHeight);

    float tau;
    if (abs(dir.y) < 1e-4f)
        tau = gParams.fogDensity * exp(-y0 / H) * travelled;
    else
        tau = gParams.fogDensity * H / dir.y * (exp(-y0 / H) - exp(-y1 / H));
    tau = max(0.0f, tau);
    const float T = exp(-tau);

    // In-scattered light is just the sky in that direction.
    //
    // The obvious-looking thing here is a Henyey-Greenstein lobe around the sun
    // so the haze glows. That is wrong twice over: Preetham ALREADY models
    // aerosol scattering -- the bright warm region around the sun is what the
    // Perez C and D terms are for -- so a separate Mie term counts it twice,
    // and the sun's irradiance through a forward-peaked phase is two orders of
    // magnitude above the dome, which washes the whole frame to white.
    return L * T + skyDome(gParams.sky, dir) * (1.0f - T);
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

    // The jitter is a whole-frame offset from a Halton sequence, not a per-pixel
    // random one. It is what antialiases the image as samples accumulate: a
    // low-discrepancy sequence covers the pixel evenly in a few dozen frames,
    // where white noise leaves parts of it unvisited for runs at a time.
    const float px = float(pix.x) + 0.5f + gParams.jitter.x;
    const float py = float(pix.y) + 0.5f + gParams.jitter.y;

    const V4Camera c = gParams.cam;
    const float sx = (2.0f * px / float(gParams.dim.x) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(gParams.dim.y)) * c.halfH;

    float3 o = c.pos;
    float3 d = normalize(c.w + c.u * sx + c.v * sy);
    if (c.aperture > 0.0f)
    {
        // Everything on the focal plane stays put; only the origin moves, which
        // is what puts the plane in focus and blurs by distance from it.
        const float3 focal = o + d * (c.focusDist / dot(d, c.w));
        const float2 l = sampleConcentricDisk(rng.next2D());
        o = o + c.u * (l.x * c.aperture * 0.5f) + c.v * (l.y * c.aperture * 0.5f);
        d = normalize(focal - o);
    }

    const float3 primaryDir = d;
    float3 L = float3(0.0f), beta = float3(1.0f);

    // The guides, filled in from the primary hit below. Held here rather than
    // written where they are found so there is exactly one store per texture
    // per pixel, on a path that also has to handle the ray escaping.
    float3 gAlbedo = float3(0.0f), gSpecular = float3(0.0f), gNormal = -primaryDir;
    float gRough = 1.0f, gViewZ = 0.0f;
    bool specularBounce = true; // the camera ray itself carries no light pdf
    float bsdfPdf = 1.0f;
    bool inWater = false;
    float primaryDist = -1.0f;

    // Beer-Lambert extinction under water, from the per-metre transmittance the
    // material carries. Hoisted out of the loop: it is three logs of constants.
    const float3 waterSigma = float3(-log(0.42f), -log(0.62f), -log(0.55f));

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
            float3 sky = skyDome(gParams.sky, d);
            const float3 disk = skyDisk(gParams.sky, d);
            if (!isBlack(disk))
            {
                const float lightPdf = uniformConePdf(kSunCosThetaMax);
                const float w = specularBounce ? 1.0f : powerHeuristic(1.0f, bsdfPdf, 1.0f, lightPdf);
                sky += disk * w;
            }
            L += beta * sky;
            if (depth == 0)
                primaryDist = 1e5f;
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
            }
            else
            {
                const float kt = h.mat.translucency;
                // The same split sampleOpaque makes: what is left of the
                // diffuse lobe after the part that passes through.
                gAlbedo = h.mat.albedo * (1.0f - kt);
                gSpecular = float3(schlick(h.mat.specular, saturate(dot(h.ns, -d))));
                gRough = h.mat.roughness;
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
            continue;
        }

        const Frame fr = Frame.make(h.ns);
        const float3 wo = fr.toLocal(-d);
        if (wo.z > 0.0f)
            L += beta * sampleSun(h, fr, wo, rng, depth);

        const BsdfSample bs = sampleOpaque(h.mat, wo, rng);
        if (bs.pdf <= 0.0f || isBlack(bs.f))
            break;

        const float3 contrib = bs.f * (abs(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib))
            break;
        beta *= contrib;
        bsdfPdf = bs.pdf;
        specularBounce = bs.specular;

        const float3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

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
        L = applyFog(L, primaryDir, primaryDist);

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
    // Both ends are taken UNJITTERED: the previous camera never had a jitter
    // applied, so the current pixel is its own centre rather than where this
    // frame's sub-pixel offset actually landed. That is what DLSS expects
    // unless it is told the vectors are jittered, and it is why the flag is not
    // set on the host.
    // -----------------------------------------------------------------------
    if (gParams.writeGuides != 0)
    {
        // An escaped ray has no surface, so the sky is given a point far down
        // its own direction. It reprojects correctly under rotation, which is
        // the only motion the sky has.
        const float3 worldP = (primaryDist > 0.0f && primaryDist < 1e4f)
                                  ? (c.pos + primaryDir * primaryDist)
                                  : (c.pos + primaryDir * 1e4f);

        const float2 curPix = float2(float(pix.x) + 0.5f, float(pix.y) + 0.5f);
        float2 prevPix;
        const bool ok = projectToPixel(gParams.prevCam, worldP, gParams.dim, prevPix);
        gGuideMotion[pix] = ok ? (prevPix - curPix) : float2(0.0f, 0.0f);

        gGuideAlbedo[pix] = float4(gAlbedo, 0.0f);
        gGuideSpecular[pix] = float4(gSpecular, 0.0f);
        gGuideNormRough[pix] = float4(normalize(gNormal), gRough);
        gGuideDepth[pix] = (gViewZ > 0.0f)
                               ? ndcDepth(gViewZ, gParams.depthNear, gParams.depthFar)
                               : 1.0f;
    }

    // -- accumulate ----------------------------------------------------------
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
