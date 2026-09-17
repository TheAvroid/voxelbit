// ---------------------------------------------------------------------------
// Vecmath.slang -- the device half of core/vecmath.h.
//
// v2 had ONE copy of this, compiled for both processors, because nvcc could be
// made to accept the same source as the host compiler. Slang cannot: it has no
// uint64_t arithmetic on every target, no C++ operator overloading rules to
// borrow, and its float3 already carries the operators the host file had to
// define by hand. So the file is split.
//
// WHAT SURVIVED THE SPLIT UNCHANGED is everything that decides what a sample
// looks like: the Duff frame, PCG32, the concentric disk, the cosine
// hemisphere, the cone, the power heuristic, Halton. Those are the parts where
// a difference between the two copies would show as a different picture rather
// than a compile error, so they were transcribed line by line rather than
// rewritten. The host copy keeps them too -- it does not use most of them any
// more, but a sampler that exists in one place only is a sampler nobody can
// check.
// ---------------------------------------------------------------------------
#pragma once

static const float PI = 3.14159265358979323846f;
static const float INV_PI = 0.31830988618379067154f;
static const float TWO_PI = 6.28318530717958647692f;

// Larger than any distance the world can produce, and it survives being added
// to -- which a real infinity does not, once a NaN gets into the path.
static const float INF = 1e30f;

float sqr(float x)
{
    return x * x;
}
float maxComp(float3 a)
{
    return max(a.x, max(a.y, a.z));
}
float minComp(float3 a)
{
    return min(a.x, min(a.y, a.z));
}
bool isBlack(float3 a)
{
    return all(a == float3(0.0f));
}
bool allFinite(float3 a)
{
    // isfinite() over a vector returns a bool3; a single NaN anywhere in the
    // throughput has to kill the path, so this is an all().
    return all(isfinite(a));
}

// Rec.709 luminance -- what Russian roulette and the firefly clamp weight by.
float luminance(float3 c)
{
    return dot(c, float3(0.2126f, 0.7152f, 0.0722f));
}

float3 faceforward3(float3 n, float3 v)
{
    return dot(n, v) < 0.0f ? -n : n;
}
float3 reflect3(float3 wo, float3 n)
{
    return -wo + n * (2.0f * dot(wo, n));
}

// Smoothstep on an already-normalised t.
float sstep(float t)
{
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Orthonormal basis
//
// Duff et al.'s branchless build. The naive "cross with up" version degenerates
// exactly at the poles, and a pine forest is full of surfaces whose normal is
// straight up -- the ground -- so the degenerate case there is the common case.
// ---------------------------------------------------------------------------
struct Frame
{
    float3 t, b, n;

    static Frame make(float3 normal)
    {
        Frame f;
        f.n = normal;
        const float sign = normal.z >= 0.0f ? 1.0f : -1.0f;
        const float a = -1.0f / (sign + normal.z);
        const float bb = normal.x * normal.y * a;
        f.t = float3(1.0f + sign * normal.x * normal.x * a, sign * bb, -sign * normal.x);
        f.b = float3(bb, sign + normal.y * normal.y * a, -normal.y);
        return f;
    }

    float3 toWorld(float3 v) { return t * v.x + b * v.y + n * v.z; }
    float3 toLocal(float3 v) { return float3(dot(v, t), dot(v, b), dot(v, n)); }
};

// ---------------------------------------------------------------------------
// PCG32 -- O'Neill's permuted congruential generator.
//
// Kept rather than swapped for one of the cheap GPU hashes. A path tracer that
// accumulates for minutes draws far enough into each pixel's stream that a
// 32-bit hash of (pixel, frame) starts to repeat visibly, and the repeat shows
// as fixed-pattern noise that no amount of further accumulation removes. PCG's
// period is 2^64 per stream, so it never gets there.
//
// The 64-bit state is the one thing that had to change shape. Slang has
// uint64_t on D3D12 shader model 6.0 and up, which is well below what inline
// ray tracing already requires, so the arithmetic is the same as the host's --
// but it is spelled out here rather than shared, so the two are checkable
// against each other by eye.
// ---------------------------------------------------------------------------
struct Rng
{
    uint64_t state;
    uint64_t inc;

    [mutating]
    void init(uint64_t seq, uint64_t seed)
    {
        state = 0ull;
        inc = (seq << 1u) | 1ull;
        nextUInt();
        state += seed;
        nextUInt();
    }

    [mutating]
    uint nextUInt()
    {
        const uint64_t old = state;
        state = old * 6364136223846793005ull + inc;
        const uint xorshifted = uint(((old >> 18u) ^ old) >> 27u);
        const uint rot = uint(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    // [0,1), clamped just below one so it can never round up to exactly 1.0f.
    [mutating]
    float nextFloat()
    {
        return min(0.99999994f, float(nextUInt()) * 2.3283064e-10f);
    }

    [mutating]
    float2 next2D()
    {
        const float a = nextFloat();
        return float2(a, nextFloat());
    }
};

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
float2 sampleConcentricDisk(float2 u)
{
    const float ox = 2.0f * u.x - 1.0f, oy = 2.0f * u.y - 1.0f;
    if (ox == 0.0f && oy == 0.0f)
        return float2(0.0f, 0.0f);
    float r, theta;
    if (abs(ox) > abs(oy))
    {
        r = ox;
        theta = 0.25f * PI * (oy / ox);
    }
    else
    {
        r = oy;
        theta = 0.5f * PI - 0.25f * PI * (ox / oy);
    }
    return float2(r * cos(theta), r * sin(theta));
}

// Cosine-weighted hemisphere about +z. pdf = cos/pi.
float3 sampleCosineHemisphere(float2 u)
{
    const float2 d = sampleConcentricDisk(u);
    const float z = sqrt(max(0.0f, 1.0f - d.x * d.x - d.y * d.y));
    return float3(d.x, d.y, z);
}

// A cone about +z of half-angle acos(cosThetaMax). This is how the sun is
// sampled: it is a disk about half a degree across, not a point, and sampling
// it as a cone is what gives a pine's shadow the soft edge it has in life.
float3 sampleUniformCone(float2 u, float cosThetaMax)
{
    const float cosTheta = (1.0f - u.x) + u.x * cosThetaMax;
    const float sinTheta = sqrt(max(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = TWO_PI * u.y;
    return float3(cos(phi) * sinTheta, sin(phi) * sinTheta, cosTheta);
}

float uniformConePdf(float cosThetaMax)
{
    return 1.0f / (TWO_PI * (1.0f - cosThetaMax));
}

// Balance heuristic, power 2 -- the sun-versus-BSDF MIS weight.
float powerHeuristic(float nf, float fPdf, float ng, float gPdf)
{
    const float f = nf * fPdf, g = ng * gPdf;
    if (f * f + g * g <= 0.0f)
        return 0.0f;
    return (f * f) / (f * f + g * g);
}
