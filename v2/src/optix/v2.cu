// ---------------------------------------------------------------------------
// v2.cu -- every program that runs on the GPU.
//
// This is v4's integrator, moved across the bus and turned inside out. The
// algorithm is unchanged and deliberately so: unidirectional paths, next-event
// estimation to the sun's disk, MIS against the BSDF, analytic height fog on
// the primary segment. What changed is the shape it has to take.
//
// THE PATH LOOP LIVES IN RAYGEN, NOT IN CLOSEST-HIT. The natural OptiX
// translation of a recursive CPU tracer is for closest-hit to shade and trace
// the next ray itself, and it is the wrong one. Recursion costs continuation
// stack, and OptiX has to size that stack for the DEEPEST path any launch might
// take -- ten bounces of a stack frame each, reserved for every one of the two
// million threads in flight, whether they use it or not. Flattening the loop
// into raygen makes the trace depth 1: closest-hit does nothing but describe
// the surface it landed on, raygen decides what happens next. The stack shrinks
// to almost nothing and occupancy roughly doubles.
//
// THE SHADOW RAY HAS NO HIT PROGRAM AT ALL. It is traced with closest-hit and
// any-hit both disabled and TERMINATE_ON_FIRST_HIT set, so the only program
// that can run is the miss. The payload starts at "occluded" and only the miss
// clears it. That is the cheapest occlusion query OptiX can express -- pure RT
// core traversal with no SM shading -- and in a conifer canopy, where most
// shadow rays die inside the first metre of foliage, it is most of the frame.
// ---------------------------------------------------------------------------
#include <optix.h>

#include "../scene/material.h"
#include "params.h"

using namespace v2;

extern "C" {
__constant__ LaunchParams params;
}

// ---------------------------------------------------------------------------
// Payload plumbing
//
// Two payload registers carry a pointer to a struct in the caller's local
// memory. This is the standard OptiX idiom and it is here for the same reason
// as always: the surface description below is far wider than the eight payload
// registers a pipeline gets, and splitting it across a dozen would cost more
// register pressure than the one indirection does.
// ---------------------------------------------------------------------------
struct PathVertex {
    Vec3 p;
    Vec3 ng;  // geometric, for offsetting the next ray
    Vec3 ns;  // shading, for the BSDF frame
    float t;
    Material mat;
    unsigned int kind;
    bool hit;
};

static __forceinline__ __device__ void *unpackPtr(unsigned int hi, unsigned int lo) {
    return reinterpret_cast<void *>((static_cast<unsigned long long>(hi) << 32) | lo);
}
static __forceinline__ __device__ void packPtr(void *p, unsigned int &hi, unsigned int &lo) {
    const unsigned long long u = reinterpret_cast<unsigned long long>(p);
    hi = static_cast<unsigned int>(u >> 32);
    lo = static_cast<unsigned int>(u & 0xFFFFFFFFull);
}
static __forceinline__ __device__ PathVertex *currentVertex() {
    return reinterpret_cast<PathVertex *>(unpackPtr(optixGetPayload_0(), optixGetPayload_1()));
}

// ---------------------------------------------------------------------------
// Push a ray start off the surface along the geometric normal, on the side it
// is leaving by. A constant epsilon is wrong at this scale: the patch spans
// hundreds of metres and a voxel face is ten centimetres, so the offset scales
// with the point's own magnitude instead.
// ---------------------------------------------------------------------------
static __forceinline__ __device__ Vec3 offsetRay(Vec3 p, Vec3 ng, Vec3 dir) {
    const float scale = 1e-4f * maxf(1.0f, maxComp(vabs(p)));
    return p + faceforward(ng, dir) * scale;
}

static __forceinline__ __device__ float3 f3(Vec3 v) { return make_float3(v.x, v.y, v.z); }

static __forceinline__ __device__ void traceRadiance(Vec3 o, Vec3 d, PathVertex *pv) {
    unsigned int hi, lo;
    packPtr(pv, hi, lo);
    optixTrace(params.handle, f3(o), f3(d), 1e-5f, INF, 0.0f, OptixVisibilityMask(255),
               OPTIX_RAY_FLAG_NONE, RAY_RADIANCE, RAY_TYPE_COUNT, RAY_RADIANCE, hi, lo);
}

static __forceinline__ __device__ bool traceOccluded(Vec3 o, Vec3 d, float tmax) {
    unsigned int occluded = 1u;  // only the miss program can clear this
    unsigned int unused = 0u;
    optixTrace(params.handle, f3(o), f3(d), 1e-5f, tmax, 0.0f, OptixVisibilityMask(255),
               OPTIX_RAY_FLAG_TERMINATE_ON_FIRST_HIT | OPTIX_RAY_FLAG_DISABLE_ANYHIT |
                   OPTIX_RAY_FLAG_DISABLE_CLOSESTHIT,
               RAY_RADIANCE, RAY_TYPE_COUNT, RAY_SHADOW, occluded, unused);
    return occluded != 0u;
}

// ---------------------------------------------------------------------------
// Next event estimation: the sun.
// ---------------------------------------------------------------------------
//
// `mis` is false when the path is about to STOP at this vertex. Multiple
// importance sampling splits a light's contribution between two strategies and
// relies on both being collected; a path that terminates never fires the second
// one, so weighting the first would silently throw away whatever share the
// heuristic assigned to a BSDF bounce that is never going to happen. That is a
// systematic loss, not noise -- it turned up as a fifteen per cent deficit in
// the blue channel the first time the probe grid was measured against a
// reference, blue being where the sky lives.
static __forceinline__ __device__ Vec3 sampleSun(const PathVertex &h, const Frame &fr, Vec3 wo,
                                                 Rng &rng, int depth, bool mis = true) {
    const Vec3 Lsun = params.sky.sunRadiance;
    if (isBlack(Lsun)) return Vec3(0.0f);

    const Frame sf(params.sky.sunDir);
    const Vec3 wWorld = sf.toWorld(sampleUniformCone(rng.next2D(), SUN_COS_THETA_MAX));
    const Vec3 wi = fr.toLocal(wWorld);

    // A needle transmits, so a direction on the far side is still a valid light
    // path; an opaque surface below its own horizon is not.
    if (wi.z <= 0.0f && h.mat.translucency <= 0.0f) return Vec3(0.0f);

    float pdf = 0.0f;
    const Vec3 f = evalOpaque(h.mat, wo, wi, &pdf);
    if (isBlack(f) || pdf <= 0.0f) return Vec3(0.0f);

    if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF)) return Vec3(0.0f);

    const float lightPdf = uniformConePdf(SUN_COS_THETA_MAX);
    const float w = mis ? powerHeuristic(1.0f, lightPdf, 1.0f, pdf) : 1.0f;
    Vec3 c = f * Lsun * (fabsf(wi.z) * w / lightPdf);

    // Fireflies: a grazing specular lobe against a 10^5 sun radiance can
    // produce a single sample worth more than the rest of the pixel. Only the
    // indirect bounces are clamped -- clamping the first would visibly dim
    // every sunlit surface in the frame.
    if (depth > 0 && params.clampIndirect > 0.0f) {
        const float m = maxComp(c);
        if (m > params.clampIndirect) c *= params.clampIndirect / m;
    }
    return c;
}

// Next-event estimation against the sky DOME.
//
// The mirror of sampleSun, for the other light in the scene. It aims at the
// bright parts of the sky rather than wherever the surface happened to scatter,
// which is the whole reason it helps: a second strategy is only worth having if
// it differs from the first, and a cosine-weighted guess is what the BSDF was
// already doing.
//
// Split in two: the estimator itself, and the gate in front of it. The probe
// pass needs the estimator whatever the user has set the toggle to -- a probe
// that skipped the dome would bake a wood lit by the sun alone.
static __forceinline__ __device__ Vec3 domeNee(const PathVertex &h, const Frame &fr, Vec3 wo,
                                               Rng &rng, int depth, bool mis = true) {
    if (!params.skyCdf) return Vec3(0.0f);
    // ONLY THE BOUNCES THAT PAY FOR THEMSELVES. A shadow ray through a dense
    // canopy is not cheap, and doing one at every vertex of a six-bounce path
    // costs more than the variance it removes -- measured, not assumed. The
    // first bounces carry almost all of the visible ambient, so the ray budget
    // goes there and the deep ones keep relying on the BSDF sample.

    float lightPdf = 0.0f;
    const Vec3 wWorld = params.skyCdf->sample(rng.nextFloat(), rng.nextFloat(), &lightPdf);
    if (lightPdf <= 0.0f) return Vec3(0.0f);

    const Vec3 wi = fr.toLocal(wWorld);
    // A needle transmits, so the far side is still a valid path; an opaque
    // surface below its own horizon is not.
    if (wi.z <= 0.0f && h.mat.translucency <= 0.0f) return Vec3(0.0f);

    float bsdfPdf = 0.0f;
    const Vec3 f = evalOpaque(h.mat, wo, wi, &bsdfPdf);
    if (isBlack(f) || bsdfPdf <= 0.0f) return Vec3(0.0f);

    if (traceOccluded(offsetRay(h.p, h.ng, wWorld), wWorld, INF)) return Vec3(0.0f);

    // See the note on sampleSun: no MIS when the path stops here.
    const float w = mis ? powerHeuristic(1.0f, lightPdf, 1.0f, bsdfPdf) : 1.0f;
    Vec3 c = f * params.sky.domeRadiance(wWorld) * (fabsf(wi.z) * w / lightPdf);

    // Same firefly ceiling the sun estimator uses, and for the same reason:
    // the primary bounce is left alone so nothing visibly dims.
    if (depth > 0 && params.clampIndirect > 0.0f) {
        const float m = maxComp(c);
        if (m > params.clampIndirect) c *= params.clampIndirect / m;
    }
    return c;
}

// The gate. Kept separate so the estimator above can be called unconditionally
// by the probe pass, which has no business consulting a user preference.
static __forceinline__ __device__ Vec3 sampleSkyDome(const PathVertex &h, const Frame &fr,
                                                     Vec3 wo, Rng &rng, int depth,
                                                     bool mis = true) {
    if (!params.skyNee) return Vec3(0.0f);
    if (depth >= params.skyNeeDepth) return Vec3(0.0f);
    return domeNee(h, fr, wo, rng, depth, mis);
}

// ---------------------------------------------------------------------------
// Aerial perspective, integrated in closed form along the primary segment.
//
// Depth cues in a wood come almost entirely from haze: without it a trunk forty
// metres away has the same contrast as one at four, and the stand collapses
// into a flat wall. Analytic rather than a traced medium -- one exp() instead
// of a whole extra integration, and being deterministic it adds no variance.
// ---------------------------------------------------------------------------
static __forceinline__ __device__ Vec3 applyFog(Vec3 L, Vec3 dir, float dist) {
    if (params.fogDensity <= 0.0f) return L;
    const float y0 = params.cam.pos.y;
    const float travelled = minf(dist, 4000.0f);
    const float y1 = y0 + dir.y * travelled;
    const float H = maxf(1.0f, params.fogHeight);

    float tau;
    if (fabsf(dir.y) < 1e-4f) {
        tau = params.fogDensity * expf(-y0 / H) * travelled;
    } else {
        tau = params.fogDensity * H / dir.y * (expf(-y0 / H) - expf(-y1 / H));
    }
    tau = maxf(0.0f, tau);
    const float T = expf(-tau);

    // In-scattered light is just the sky in that direction.
    //
    // The obvious-looking thing here is a Henyey-Greenstein lobe around the sun
    // so the haze glows. That is wrong twice over: Preetham ALREADY models
    // aerosol scattering -- the bright warm region around the sun is what the
    // Perez C and D terms are for -- so a separate Mie term counts it twice,
    // and the sun's irradiance through a forward-peaked phase is two orders of
    // magnitude above the dome, which washes the whole frame to white.
    return L * T + params.sky.domeRadiance(dir) * (1.0f - T);
}

// ---------------------------------------------------------------------------
// Ray generation: one camera path, start to finish.
// ---------------------------------------------------------------------------
extern "C" __global__ void __raygen__pinhole() {
    const uint3 idx = optixGetLaunchIndex();
    const unsigned int pix = idx.y * params.width + idx.x;

    // One PCG stream per pixel, advanced by the frame index. Successive frames
    // therefore draw different samples rather than re-averaging the same one.
    // Seeded from the monotonic tick, not the accumulation index: while the
    // camera moves the accumulation restarts every frame, and seeding from it
    // would redraw the SAME sample over and over.
    Rng rng(static_cast<unsigned long long>(pix) + 1ull,
            static_cast<unsigned long long>(params.tick) * 0x9E3779B97F4A7C15ull +
                static_cast<unsigned long long>(params.seed));

    // The jitter is a whole-frame offset from a Halton sequence, not a per-pixel
    // random one. It is what antialiases the image as samples accumulate: a
    // low-discrepancy sequence covers the pixel evenly in a few dozen frames,
    // where white noise leaves parts of it unvisited for runs at a time.
    const float px = float(idx.x) + 0.5f + params.jitter.x;
    const float py = float(idx.y) + 0.5f + params.jitter.y;

    const CameraGPU &c = params.cam;
    const float sx = (2.0f * px / float(params.width) - 1.0f) * c.halfW;
    const float sy = (1.0f - 2.0f * py / float(params.height)) * c.halfH;

    Vec3 o = c.pos;
    Vec3 d = normalize(c.w + c.u * sx + c.v * sy);
    if (c.aperture > 0.0f) {
        // Everything on the focal plane stays put; only the origin moves, which
        // is what puts the plane in focus and blurs by distance from it.
        const Vec3 focal = o + d * (c.focusDist / dot(d, c.w));
        const Vec2 l = sampleConcentricDisk(rng.next2D());
        o = o + c.u * (l.x * c.aperture * 0.5f) + c.v * (l.y * c.aperture * 0.5f);
        d = normalize(focal - o);
    }

    const Vec3 primaryDir = d;
    Vec3 L(0.0f), beta(1.0f);
    bool specularBounce = true;  // the camera ray itself carries no light pdf
    float bsdfPdf = 1.0f;
    bool inWater = false;
    float primaryDist = -1.0f;

    // Beer-Lambert extinction under water, from the per-metre transmittance the
    // material carries. Hoisted out of the loop: it is three logs of constants.
    const Vec3 waterSigma(-logf(0.42f), -logf(0.62f), -logf(0.55f));

    PathVertex h;
    for (int depth = 0; depth < params.maxDepth; ++depth) {
        h.hit = false;
        traceRadiance(o, d, &h);

        if (inWater) beta *= vexp(waterSigma * -(h.hit ? h.t : 60.0f));

        if (!h.hit) {
            // Escaped: the sky. The dome is never light-sampled, so it always
            // arrives at full weight; the disk may have been, so it is weighted
            // against the cone pdf that could have found it.
            Vec3 sky = params.sky.domeRadiance(d);
            // The dome is now sampled from BOTH ends, so this end has to be
            // weighted or it would be counted twice. specularBounce means the
            // last vertex was a delta lobe -- nothing could have aimed a light
            // sample through it -- so that case keeps the full contribution,
            // and so does the camera ray, which is what makes directly-visible
            // sky look exactly as it did.
            if (params.skyNee && params.skyCdf && !specularBounce) {
                const float domePdf = params.skyCdf->pdf(d);
                if (domePdf > 0.0f) sky = sky * powerHeuristic(1.0f, bsdfPdf, 1.0f, domePdf);
            }
            const Vec3 disk = params.sky.diskRadiance(d);
            if (!isBlack(disk)) {
                const float lightPdf = uniformConePdf(SUN_COS_THETA_MAX);
                const float w =
                    specularBounce ? 1.0f : powerHeuristic(1.0f, bsdfPdf, 1.0f, lightPdf);
                sky += disk * w;
            }
            L += beta * sky;
            if (depth == 0) primaryDist = 1e5f;
            break;
        }

        if (depth == 0) primaryDist = h.t;

        // -- water: a delta surface, so no light sampling here at all ---------
        if (h.mat.type == MatType::Dielectric) {
            const bool entering = dot(d, h.ns) < 0.0f;
            const Frame fr(entering ? h.ns : -h.ns);
            const Vec3 wo = fr.toLocal(-d);
            const BsdfSample bs = sampleDielectric(h.mat, wo, entering, rng);
            if (bs.pdf <= 0.0f) break;

            beta *= bs.f * fabsf(bs.wi.z) / bs.pdf;
            const Vec3 wi = fr.toWorld(bs.wi);
            if (bs.transmitted) inWater = entering;
            o = offsetRay(h.p, h.ng, wi);
            d = wi;
            specularBounce = true;
            continue;
        }

        // Does the path stop here? It changes how the light samples below are
        // weighted, so it has to be settled before they are taken -- which
        // means asking the grid FIRST. Outside the volume, or in a pocket where
        // every surrounding probe is buried, the grid has no answer and the
        // path simply carries on as it always did. A boundary that degrades
        // into ordinary path tracing is invisible; one that drops the indirect
        // term is a dark ring around the player.
        Vec3 giE(0.0f);
        const bool terminal = params.giOn && depth + 1 >= params.giDepth &&
                              params.gi.lookup(h.p, h.ns, &giE);

        const Frame fr(h.ns);
        const Vec3 wo = fr.toLocal(-d);
        if (wo.z > 0.0f) {
            // The sun stays a traced light even at a terminal vertex, because
            // its shadow is the one hard edge in the frame and no grid at any
            // spacing would keep it. Its MIS weight is within a fraction of a
            // per cent of one anyway -- the cone it subtends is tiny, so light
            // sampling wins the heuristic outright -- which is why dropping the
            // partner here costs nothing measurable.
            L += beta * sampleSun(h, fr, wo, rng, depth, !terminal);

            // The DOME is not traced at a terminal vertex: the grid already
            // holds it. Tried the other way first -- keep the dome as a shadow
            // ray and store only bounced light -- and it was measurably worse
            // than no grid at all. Without a BSDF sample to share the estimate
            // with, a single ray at a whole hemisphere of sky is a noisy way to
            // measure a smooth quantity, and MIS is not available to a path
            // that stops. Sky in a wood is soft and low-frequency, which is
            // precisely what a probe grid is good at.
            if (!terminal) L += beta * sampleSkyDome(h, fr, wo, rng, depth);
        }

        // -- stop here and read the grid --------------------------------------
        // Everything past this vertex is INDIRECT, and the grid is a table of
        // exactly that. The direct lighting above is still traced, so shadow
        // edges stay as sharp as they ever were; only the bounced light is
        // approximated, and bounced light in a wood has no sharp edges in it.
        //
        // Diffuse only. These materials are F0 = 0.04 with a roughness near 1,
        // so the specular lobe carries very little indirect, and handing it the
        // same irradiance would be a worse lie than dropping it.
        if (terminal) {
            L += beta * h.mat.albedo * giE;
            break;
        }

        const BsdfSample bs = sampleOpaque(h.mat, wo, rng);
        if (bs.pdf <= 0.0f || isBlack(bs.f)) break;

        const Vec3 contrib = bs.f * (fabsf(bs.wi.z) / bs.pdf);
        if (!allFinite(contrib)) break;
        beta *= contrib;
        bsdfPdf = bs.pdf;
        specularBounce = bs.specular;

        const Vec3 wi = fr.toWorld(bs.wi);
        o = offsetRay(h.p, h.ng, wi);
        d = wi;

        // Russian roulette on throughput, from the SECOND bounce. Starting it
        // late is the textbook advice and it is wrong here: a conifer canopy is
        // dark -- foliage albedo about 0.19 -- so a ray that enters one carries
        // almost nothing, and a late rrStart guarantees it several full bounces
        // of tree geometry first. It is unbiased either way; what changes is
        // variance per unit time, and early roulette wins that comfortably.
        if (depth >= params.rrStart) {
            const float q = minf(0.95f, maxComp(beta));
            if (rng.nextFloat() > q) break;
            beta /= q;
        }
        if (maxComp(beta) <= 0.0f) break;
    }

    if (!allFinite(L)) L = Vec3(0.0f);
    if (primaryDist > 0.0f) L = applyFog(L, primaryDir, primaryDist);

    // -- accumulate --------------------------------------------------------
    // frame 0 overwrites rather than adds, which is how an invalidation clears
    // the film without a separate memset launch.
    float4 acc = make_float4(L.x, L.y, L.z, 1.0f);
    if (params.frame > 0u) {
        const float4 p0 = params.accum[pix];
        acc = make_float4(p0.x + acc.x, p0.y + acc.y, p0.z + acc.z, p0.w + 1.0f);
    }

    // Roll the window forward once it is full. Scaling both parts together
    // keeps the mean exactly where it is; only the WEIGHT of what comes next
    // changes. This is what lets the film track a moving sun continuously
    // instead of being thrown away and restarted every time it shifts.
    if (params.maxAccum > 0u && acc.w > float(params.maxAccum)) {
        const float k = float(params.maxAccum) / acc.w;
        acc = make_float4(acc.x * k, acc.y * k, acc.z * k, acc.w * k);
    }
    params.accum[pix] = acc;

    const float inv = 1.0f / maxf(1.0f, acc.w);
    params.color[pix] = make_float4(acc.x * inv, acc.y * inv, acc.z * inv, 1.0f);
}

// ---------------------------------------------------------------------------
// Probe pass: refill the irradiance grid.
//
// One thread per probe, a handful of rays each, blended into what was there
// last frame rather than replacing it. That is what makes it cheap AND smooth:
// 32 rays is a noisy estimate on its own, but 32 rays a frame at 0.94
// hysteresis is an exponential average over several hundred, and the average
// is what gets read.
//
// THE GRID HOLDS EVERYTHING EXCEPT THE SUN. Sky, bounced light, bounced sky --
// all of it. Only the sun disk is left out, because it is light-sampled at
// shading time and would otherwise be counted twice, and because its shadow is
// the one edge in the frame sharp enough that a grid could not hold it.
//
// The first version of this stored bounced light only and kept the sky as a
// shadow ray at the shading vertex. It measured WORSE than no grid: a path that
// stops has no BSDF sample to share the sky estimate with, and one ray aimed at
// a whole hemisphere is a noisy way to measure something smooth. Putting the
// sky in the grid removes that ray and the noise with it.
// ---------------------------------------------------------------------------
extern "C" __global__ void __raygen__probes() {
    const unsigned int pi = optixGetLaunchIndex().x;
    if (pi >= unsigned(kProbeCount) || !params.gi.data) return;

    const int ix = int(pi % unsigned(kProbeX));
    const int iy = int((pi / unsigned(kProbeX)) % unsigned(kProbeY));
    const int iz = int(pi / unsigned(kProbeX * kProbeY));
    const Vec3 p = params.gi.probePos(ix, iy, iz);

    // A different stream per probe, advanced by the tick, so the directions
    // rotate every frame and the exponential average sees the whole sphere
    // instead of re-measuring the same 32 directions for ever.
    Rng rng(static_cast<unsigned long long>(pi) + 1ull,
            static_cast<unsigned long long>(params.tick) * 0x9E3779B97F4A7C15ull +
                static_cast<unsigned long long>(params.seed) + 0xD1B54A32D192ED03ull);

    const int rays = maxi(1, params.probeRays);
    float acc[3][4] = {};
    int insideVotes = 0;

    // A hit this close that faces away means the probe is inside something.
    // Probes do end up buried -- the grid is a box dropped on a landscape, not
    // a fitted cage -- and a buried probe reads black, so it has to be found
    // and dropped rather than interpolated into the surface above it.
    const float nearHit = 0.12f * params.gi.spacing;

    for (int s = 0; s < rays; ++s) {
        const Vec3 d = sampleUniformSphere(rng.next2D());

        PathVertex h;
        h.hit = false;
        traceRadiance(p, d, &h);

        Vec3 L(0.0f);
        if (!h.hit) {
            // Escaped: the sky itself, and this is where it enters the grid.
            // The DISK is excluded -- the sun is light-sampled at shading time
            // and counting it here as well would double it.
            L = params.sky.domeRadiance(d);
            shAccumulate(L, d, 1.0f, acc);
            continue;
        }

        if (h.t < nearHit && dot(d, h.ng) > 0.0f) ++insideVotes;

        const Frame fr(h.ns);
        const Vec3 wo = fr.toLocal(-d);
        // Full weight, no MIS: a probe ray stops at the surface it hit.
        if (wo.z > 0.0f) L = L + sampleSun(h, fr, wo, rng, 0, false);

        // Everything that is not the sun -- sky and further bounces alike --
        // for the price of a lookup: whatever light this surface was told it
        // had LAST frame. It forms a geometric series in the albedo, so with
        // feedback below 1 it settles instead of running away.
        Vec3 E;
        if (params.probeFeedback > 0.0f && params.giPrev.data &&
            params.giPrev.lookup(h.p, h.ns, &E))
            L = L + h.mat.albedo * E * params.probeFeedback;

        if (allFinite(L)) shAccumulate(L, d, 1.0f, acc);
    }

    // Uniform sphere sampling, so the estimator is (4*pi / N) * sum.
    const float norm = 4.0f * PI / float(rays);
    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < 4; ++k) acc[ch][k] *= norm;

    // Validity is a geometric fact, not a lighting one, so it is measured
    // fresh each frame rather than smoothed -- a probe that has just scrolled
    // out from under a hill should be trusted immediately.
    ProbeSH out = {};
    out.valid = (float(insideVotes) > 0.55f * float(rays)) ? 0.0f : 1.0f;

    // History by world position. Both grids share a spacing and their origins
    // differ by a whole number of cells, so this is an integer offset: the
    // probe that stood here last frame, exactly, with no interpolation to blur.
    float hyst = 0.0f;
    const ProbeSH *hist = nullptr;
    if (params.giPrev.data && params.giPrev.spacing > 0.0f) {
        const Vec3 rel = (p - params.giPrev.origin) * (1.0f / params.giPrev.spacing);
        const int hx = int(floorf(rel.x + 0.5f));
        const int hy = int(floorf(rel.y + 0.5f));
        const int hz = int(floorf(rel.z + 0.5f));
        if (hx >= 0 && hx < kProbeX && hy >= 0 && hy < kProbeY && hz >= 0 && hz < kProbeZ) {
            const ProbeSH &q = params.giPrev.data[IrradianceGridGPU::index(hx, hy, hz)];
            if (q.valid > 0.0f) { hist = &q; hyst = params.probeHysteresis; }
        }
    }

    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < 4; ++k)
            out.c[ch][k] = hist ? lerpf(acc[ch][k], hist->c[ch][k], hyst) : acc[ch][k];

    params.gi.data[IrradianceGridGPU::index(ix, iy, iz)] = out;
}

// ---------------------------------------------------------------------------
// Miss programs
// ---------------------------------------------------------------------------
extern "C" __global__ void __miss__radiance() { currentVertex()->hit = false; }

// Reached only when nothing blocked the shadow ray; the payload started at 1.
extern "C" __global__ void __miss__shadow() { optixSetPayload_0(0u); }

// ---------------------------------------------------------------------------
// Closest hit: describe the surface and return. No shading, no further rays.
// ---------------------------------------------------------------------------
extern "C" __global__ void __closesthit__radiance() {
    const HitGroupData &g = *reinterpret_cast<HitGroupData *>(optixGetSbtDataPointer());
    PathVertex *pv = currentVertex();

    const float3 od = optixGetWorldRayDirection();
    const float3 oo = optixGetWorldRayOrigin();
    const Vec3 d(od.x, od.y, od.z);
    const float t = optixGetRayTmax();

    pv->hit = true;
    pv->t = t;
    pv->p = Vec3(oo.x, oo.y, oo.z) + d * t;
    pv->kind = g.kind;

    // The face direction was recorded when the quad was emitted, so the normal
    // is exact rather than reconstructed -- no cross product, and no dependence
    // on a vertex buffer the GAS would have to keep readable to provide it.
    const unsigned short packed = g.tri[optixGetPrimitiveIndex()];
    const Vec3 nObj = faceNormal(triFace(packed));
    const float3 nw = optixTransformNormalFromObjectToWorldSpace(f3(nObj));
    Vec3 ng = normalize(Vec3(nw.x, nw.y, nw.z));
    pv->ng = ng;
    pv->ns = ng;

    if (g.kind == KIND_WATER) {
        // A shallow ripple, in the shading normal only. Two crossed wave trains
        // so it does not read as corduroy.
        const Vec3 p = pv->p;
        const float w1 = sinf(p.x * 1.7f + p.z * 0.9f) + sinf(p.x * 0.6f - p.z * 2.1f) * 0.7f;
        const float w2 = cosf(p.z * 1.9f - p.x * 0.4f) + cosf(p.z * 0.8f + p.x * 1.5f) * 0.7f;
        pv->ns = normalize(Vec3(w1 * 0.014f, 1.0f, w2 * 0.014f));
        pv->mat.type = MatType::Dielectric;
        pv->mat.ior = 1.333f;
        pv->mat.roughness = 0.0f;
        pv->mat.albedo = Vec3(1.0f);
        pv->mat.attenuation = Vec3(0.42f, 0.62f, 0.55f);
        return;
    }

    const MaterialLook &L = params.materials[triMaterial(packed)];
    pv->mat.type = MatType::Opaque;
    pv->mat.albedo = L.albedo;
    pv->mat.roughness = L.roughness;
    pv->mat.specular = L.specular;
    pv->mat.translucency = L.translucency;

    if (g.kind == KIND_TREE) {
        // A few percent of per-tree hue. Nine models over hundreds of trees
        // would otherwise show their repeat; this is enough to break it without
        // making the stand look like a paint chart.
        pv->mat.albedo = pv->mat.albedo * params.instances[optixGetInstanceId()].tint;
    }

    // Foliage is thin enough to light from behind, so its faces are treated as
    // two-sided: turn the normal to the viewer rather than letting a backfacing
    // needle voxel go black.
    if (L.translucency > 0.0f) {
        pv->ns = faceforward(pv->ns, -d);
        pv->ng = faceforward(pv->ng, -d);
    }
}
