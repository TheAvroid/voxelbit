// ---------------------------------------------------------------------------
// vecmath.h -- the linear algebra and sampling core, host side.
//
// v2 compiled this file TWICE: once for MinGW host code and once for OptiX
// device code, which is why every function in it used to be V2_FN -- a macro
// expanding to __host__ __device__ under nvcc. That constraint is gone. Falcor
// shaders are Slang, not CUDA, so the device half of this file now lives in
// shaders/Vecmath.slang and this one is plain C++ that only ever runs on the
// CPU.
//
// THE TWO COPIES ARE STILL ONE DESIGN, and the risk that comes with that is
// real: a GPU port whose shading maths drifts away from its host maths is the
// classic way a picture changes for reasons nobody can find. What kept them in
// step before was sharing a translation unit; what keeps them in step now is
// that the host only needs a fraction of it. Nothing in this file shades
// anything -- the BSDFs went entirely to the shader side, and what is left here
// is what the world builder and the player use: vectors, the terrain's noise
// basis, and a frame. There is no second copy of the BSDF maths because there
// is no host copy at all.
//
// sqrtf and fabsf rather than std::sqrt is kept from the port. It no longer has
// to be that way, but it costs nothing and the file reads the same as the
// shader that grew out of it.
// ---------------------------------------------------------------------------
#pragma once

#include <math.h>
#include <stdint.h>

namespace v4 {

constexpr float PI = 3.14159265358979323846f;
constexpr float INV_PI = 0.31830988618379067154f;
constexpr float TWO_PI = 6.28318530717958647692f;

// Not std::numeric_limits::infinity(): that is a host-only constexpr call. This
// is larger than any distance the world can produce and survives being added to.
constexpr float INF = 1e30f;

inline float minf(float a, float b) { return a < b ? a : b; }
inline float maxf(float a, float b) { return a > b ? a : b; }
inline int mini(int a, int b) { return a < b ? a : b; }
inline int maxi(int a, int b) { return a > b ? a : b; }
inline int absi(int a) { return a < 0 ? -a : a; }
inline float clampf(float x, float a, float b) { return x < a ? a : (x > b ? b : x); }
inline float saturate(float x) { return clampf(x, 0.0f, 1.0f); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline float sqr(float x) { return x * x; }

// Smoothstep on an already-normalised t. Used all over the terrain shaping.
inline float sstep(float t) {
    t = saturate(t);
    return t * t * (3.0f - 2.0f * t);
}

// ---------------------------------------------------------------------------
// Vec3
// ---------------------------------------------------------------------------
// No default member initialisers, deliberately. LaunchParams lives in CUDA
// __constant__ memory, which may only be constant-initialised; a member with an
// NSDMI anywhere inside it makes the whole struct dynamically initialised and
// nvcc rejects it outright. So Vec3 is a plain aggregate and a bare `Vec3 v;`
// is uninitialised -- as it is for float.
struct Vec3 {
    float x, y, z;

    Vec3() = default;
    inline constexpr explicit Vec3(float v) : x(v), y(v), z(v) {}
    inline constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}

    inline Vec3 operator-() const { return {-x, -y, -z}; }
    inline float operator[](int i) const { return (&x)[i]; }
    inline float &operator[](int i) { return (&x)[i]; }
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator*(Vec3 a, Vec3 b) { return {a.x * b.x, a.y * b.y, a.z * b.z}; }
inline Vec3 operator/(Vec3 a, Vec3 b) { return {a.x / b.x, a.y / b.y, a.z / b.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }
inline Vec3 operator/(Vec3 a, float s) { return a * (1.0f / s); }
inline Vec3 &operator+=(Vec3 &a, Vec3 b) { a = a + b; return a; }
inline Vec3 &operator-=(Vec3 &a, Vec3 b) { a = a - b; return a; }
inline Vec3 &operator*=(Vec3 &a, Vec3 b) { a = a * b; return a; }
inline Vec3 &operator*=(Vec3 &a, float s) { a = a * s; return a; }
inline Vec3 &operator/=(Vec3 &a, float s) { a = a / s; return a; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float absDot(Vec3 a, Vec3 b) { return fabsf(dot(a, b)); }
inline Vec3 cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float lengthSq(Vec3 a) { return dot(a, a); }
inline float length(Vec3 a) { return sqrtf(dot(a, a)); }
inline Vec3 normalize(Vec3 a) {
    const float l = length(a);
    return l > 0.0f ? a / l : Vec3(0.0f, 1.0f, 0.0f);
}
inline Vec3 vmin(Vec3 a, Vec3 b) { return {minf(a.x, b.x), minf(a.y, b.y), minf(a.z, b.z)}; }
inline Vec3 vmax(Vec3 a, Vec3 b) { return {maxf(a.x, b.x), maxf(a.y, b.y), maxf(a.z, b.z)}; }
inline Vec3 lerp(Vec3 a, Vec3 b, float t) { return a + (b - a) * t; }
inline float maxComp(Vec3 a) { return maxf(a.x, maxf(a.y, a.z)); }
inline Vec3 vabs(Vec3 a) { return {fabsf(a.x), fabsf(a.y), fabsf(a.z)}; }
inline Vec3 vexp(Vec3 a) { return {expf(a.x), expf(a.y), expf(a.z)}; }
inline bool isBlack(Vec3 a) { return a.x == 0.0f && a.y == 0.0f && a.z == 0.0f; }

// isfinite is a macro on the host and an intrinsic on the device; both take a
// float and both are spelled without a namespace, so this one form serves.
inline bool allFinite(Vec3 a) { return isfinite(a.x) && isfinite(a.y) && isfinite(a.z); }

// Rec.709 luminance -- what Russian roulette and the firefly clamp weight by.
inline float luminance(Vec3 c) { return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z; }

inline Vec3 faceforward(Vec3 n, Vec3 v) { return dot(n, v) < 0.0f ? -n : n; }
inline Vec3 reflect(Vec3 wo, Vec3 n) { return -wo + n * (2.0f * dot(wo, n)); }

struct Vec2 {
    float x, y;  // see the note on Vec3: no NSDMIs below __constant__
    Vec2() = default;
    inline constexpr Vec2(float x_, float y_) : x(x_), y(y_) {}
};

// ---------------------------------------------------------------------------
// Orthonormal basis
//
// Duff et al.'s branchless build. The naive "cross with up" version degenerates
// exactly at the poles, and a pine forest is full of surfaces whose normal is
// straight up -- the ground -- so the degenerate case there is the common case.
// ---------------------------------------------------------------------------
struct Frame {
    Vec3 t, b, n;

    Frame() = default;
    inline explicit Frame(Vec3 normal) : n(normal) {
        const float sign = copysignf(1.0f, n.z);
        const float a = -1.0f / (sign + n.z);
        const float bb = n.x * n.y * a;
        t = Vec3(1.0f + sign * n.x * n.x * a, sign * bb, -sign * n.x);
        b = Vec3(bb, sign + n.y * n.y * a, -n.y);
    }

    inline Vec3 toWorld(Vec3 v) const { return t * v.x + b * v.y + n * v.z; }
    inline Vec3 toLocal(Vec3 v) const { return {dot(v, t), dot(v, b), dot(v, n)}; }
};

// ---------------------------------------------------------------------------
// PCG32 -- O'Neill's permuted congruential generator.
//
// Kept from the Embree engine rather than swapped for a cheap GPU hash. A path
// tracer that accumulates for minutes draws far enough into each pixel's stream
// that a 32-bit hash of (pixel, frame) starts to repeat visibly, and the repeat
// shows as fixed-pattern noise that no amount of further accumulation removes.
// PCG's period is 2^64 per stream, so it never gets there.
// ---------------------------------------------------------------------------
struct Rng {
    uint64_t state = 0x853c49e6748fea9bULL;
    uint64_t inc = 0xda3e39cb94b95bdbULL;

    Rng() = default;
    inline Rng(uint64_t seq, uint64_t seed) { init(seq, seed); }

    inline void init(uint64_t seq, uint64_t seed) {
        state = 0u;
        inc = (seq << 1u) | 1u;
        nextUInt();
        state += seed;
        nextUInt();
    }

    inline uint32_t nextUInt() {
        const uint64_t old = state;
        state = old * 6364136223846793005ULL + inc;
        const uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
        const uint32_t rot = uint32_t(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((~rot + 1u) & 31u));
    }

    // [0,1), clamped just below one so it can never round up to exactly 1.0f.
    inline float nextFloat() { return minf(0.99999994f, float(nextUInt()) * 2.3283064e-10f); }
    inline Vec2 next2D() {
        const float a = nextFloat();
        return {a, nextFloat()};
    }
};

// ---------------------------------------------------------------------------
// Sampling
// ---------------------------------------------------------------------------
inline Vec2 sampleConcentricDisk(Vec2 u) {
    const float ox = 2.0f * u.x - 1.0f, oy = 2.0f * u.y - 1.0f;
    if (ox == 0.0f && oy == 0.0f) return {0.0f, 0.0f};
    float r, theta;
    if (fabsf(ox) > fabsf(oy)) {
        r = ox;
        theta = 0.25f * PI * (oy / ox);
    } else {
        r = oy;
        theta = 0.5f * PI - 0.25f * PI * (ox / oy);
    }
    return {r * cosf(theta), r * sinf(theta)};
}

// Cosine-weighted hemisphere about +z. pdf = cos/pi.
inline Vec3 sampleCosineHemisphere(Vec2 u) {
    const Vec2 d = sampleConcentricDisk(u);
    const float z = sqrtf(maxf(0.0f, 1.0f - d.x * d.x - d.y * d.y));
    return {d.x, d.y, z};
}

inline Vec3 sampleUniformSphere(Vec2 u) {
    const float z = 1.0f - 2.0f * u.x;
    const float r = sqrtf(maxf(0.0f, 1.0f - z * z));
    const float phi = TWO_PI * u.y;
    return {r * cosf(phi), r * sinf(phi), z};
}

// A cone about +z of half-angle acos(cosThetaMax). This is how the sun is
// sampled: it is a disk about half a degree across, not a point, and sampling
// it as a cone is what gives a pine's shadow the soft edge it has in life.
inline Vec3 sampleUniformCone(Vec2 u, float cosThetaMax) {
    const float cosTheta = (1.0f - u.x) + u.x * cosThetaMax;
    const float sinTheta = sqrtf(maxf(0.0f, 1.0f - cosTheta * cosTheta));
    const float phi = TWO_PI * u.y;
    return {cosf(phi) * sinTheta, sinf(phi) * sinTheta, cosTheta};
}
inline float uniformConePdf(float cosThetaMax) { return 1.0f / (TWO_PI * (1.0f - cosThetaMax)); }

// Balance heuristic, power 2 -- the sun-versus-BSDF MIS weight.
inline float powerHeuristic(float nf, float fPdf, float ng, float gPdf) {
    const float f = nf * fPdf, g = ng * gPdf;
    if (f * f + g * g <= 0.0f) return 0.0f;
    return (f * f) / (f * f + g * g);
}

// ---------------------------------------------------------------------------
// Halton, for the sub-pixel jitter
//
// New in v2, and it is here for the upscaler rather than for the path tracer. A
// temporal upscaler reconstructs detail it was never shown by relying on
// successive frames landing on DIFFERENT sub-pixel positions. Feed it white
// noise and the offsets clump: parts of the pixel go unvisited for runs of
// frames, and the reconstruction stays soft exactly where it was meant to gain.
// A low-discrepancy sequence guarantees the coverage the upscaler assumes.
// ---------------------------------------------------------------------------
inline float radicalInverse(uint32_t base, uint32_t i) {
    float f = 1.0f, r = 0.0f;
    const float invBase = 1.0f / float(base);
    while (i > 0u) {
        f *= invBase;
        r += f * float(i % base);
        i /= base;
    }
    return r;
}

// The sequence lives in [0,1); a jitter wants [-0.5, 0.5) about the pixel centre.
inline Vec2 haltonJitter(uint32_t frame) {
    return {radicalInverse(2u, frame + 1u) - 0.5f, radicalInverse(3u, frame + 1u) - 0.5f};
}

}  // namespace v4
