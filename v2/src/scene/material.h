// ---------------------------------------------------------------------------
// material.h -- the BSDFs a conifer wood actually needs, on both processors.
//
// Four surfaces carry this whole scene: ground, bark, needle, water. Rather
// than a general uber-shader, each lobe here exists because one of those four
// could not be drawn without it.
//
// THE ONE THAT MATTERS IS TRANSLUCENCY. A needle is about a third of a
// millimetre thick and light goes through it; a canopy lit from behind glows.
// Model needles as opaque diffuse and a backlit crown turns into a black
// cut-out, which is the single most common way a rendered conifer looks wrong.
// The diffuse-transmission lobe below is what fixes that, and it is cheap: a
// cosine hemisphere on the FAR side of the surface.
//
// Every function is V2_FN, so the closest-hit program and any host-side probe
// evaluate literally the same instructions. On a GPU port that is not tidiness;
// it is the only way to be sure the picture did not change when the renderer
// moved, because there is no second implementation to disagree with.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"

namespace v2 {

enum class MatType : uint8_t {
    Opaque,      // ground, bark, needles -- diffuse + GGX + optional translucency
    Dielectric,  // water -- smooth, refracting, Beer-Lambert inside
};

struct Material {
    MatType type = MatType::Opaque;
    Vec3 albedo{0.5f, 0.5f, 0.5f};
    float roughness = 0.9f;
    float specular = 0.04f;     // F0 at normal incidence
    float translucency = 0.0f;  // fraction of the diffuse lobe that passes through
    float ior = 1.33f;
    Vec3 attenuation{1.0f, 1.0f, 1.0f};  // per-metre transmittance inside a dielectric
};

struct BsdfSample {
    Vec3 f{0.0f, 0.0f, 0.0f};
    Vec3 wi{0.0f, 0.0f, 0.0f};
    float pdf = 0.0f;
    bool specular = false;  // skip MIS and light sampling through this vertex
    bool transmitted = false;
};

// ---------------------------------------------------------------------------
// GGX / Trowbridge-Reitz
// ---------------------------------------------------------------------------
V2_FN float ggxD(float cosTh, float a2) {
    const float d = cosTh * cosTh * (a2 - 1.0f) + 1.0f;
    return a2 / maxf(1e-9f, PI * d * d);
}

// Smith height-correlated masking-shadowing, the Heitz form.
V2_FN float ggxG2(float nv, float nl, float a2) {
    const float lv = nl * sqrtf(nv * nv * (1.0f - a2) + a2);
    const float ll = nv * sqrtf(nl * nl * (1.0f - a2) + a2);
    return 2.0f * nl * nv / maxf(1e-9f, lv + ll);
}

V2_FN float schlick(float f0, float c) {
    const float m = clampf(1.0f - c, 0.0f, 1.0f);
    const float m2 = m * m;
    return f0 + (1.0f - f0) * m2 * m2 * m;
}

// Heitz 2018: sample the distribution of VISIBLE normals. Sampling D alone
// generates backfacing microfacets at grazing angles, which have to be thrown
// away -- and grazing is exactly where bark and a wet rock get their sheen, so
// that is the worst place to lose samples.
V2_FN Vec3 sampleGgxVndf(Vec3 wo, float a, Vec2 u) {
    const Vec3 vh = normalize(Vec3(a * wo.x, a * wo.y, wo.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const Vec3 t1 = lensq > 0.0f ? Vec3(-vh.y, vh.x, 0.0f) / sqrtf(lensq) : Vec3(1, 0, 0);
    const Vec3 t2 = cross(vh, t1);

    const float r = sqrtf(u.x);
    const float phi = TWO_PI * u.y;
    const float p1 = r * cosf(phi);
    float p2 = r * sinf(phi);
    const float s = 0.5f * (1.0f + vh.z);
    p2 = (1.0f - s) * sqrtf(maxf(0.0f, 1.0f - p1 * p1)) + s * p2;

    const Vec3 nh = t1 * p1 + t2 * p2 + vh * sqrtf(maxf(0.0f, 1.0f - p1 * p1 - p2 * p2));
    return normalize(Vec3(a * nh.x, a * nh.y, maxf(1e-6f, nh.z)));
}

// ---------------------------------------------------------------------------
// Opaque BSDF, evaluated in the local frame where the shading normal is +z.
//
// wo and wi both point AWAY from the surface. wi.z < 0 means the ray went
// through, which only the translucency lobe can produce.
// ---------------------------------------------------------------------------
V2_FN Vec3 evalOpaque(const Material &m, Vec3 wo, Vec3 wi, float *pdf) {
    *pdf = 0.0f;
    if (wo.z <= 0.0f) return Vec3(0.0f);

    const float kt = m.translucency;
    // Lobe-selection probabilities. The specular share is tied to Fresnel at
    // normal incidence so a rough dark surface does not spend half its samples
    // on a lobe worth four percent of its response.
    const float pSpec = clampf(m.specular * 4.0f, 0.06f, 0.5f);
    const float pDiff = (1.0f - pSpec) * (1.0f - kt);
    const float pTrans = (1.0f - pSpec) * kt;

    Vec3 f(0.0f);

    if (wi.z > 0.0f) {
        // Diffuse reflection.
        f += m.albedo * (INV_PI * (1.0f - kt));
        *pdf += pDiff * wi.z * INV_PI;

        // Specular reflection.
        const Vec3 h = normalize(wo + wi);
        const float a = maxf(1e-3f, m.roughness * m.roughness);
        const float a2 = a * a;
        const float D = ggxD(h.z, a2);
        const float G = ggxG2(wo.z, wi.z, a2);
        const float F = schlick(m.specular, dot(wo, h));
        f += Vec3(D * G * F / maxf(1e-9f, 4.0f * wo.z * wi.z));

        // VNDF pdf: D_visible(h) / (4 |wo.h|).
        const float dv = D * ggxG2(wo.z, wo.z, a2) * absDot(wo, h) / maxf(1e-9f, wo.z);
        *pdf += pSpec * dv / maxf(1e-9f, 4.0f * absDot(wo, h));
    } else if (kt > 0.0f) {
        // Diffuse transmission: the needle glowing from behind.
        f += m.albedo * (INV_PI * kt);
        *pdf += pTrans * (-wi.z) * INV_PI;
    }
    return f;
}

V2_FN BsdfSample sampleOpaque(const Material &m, Vec3 wo, Rng &rng) {
    BsdfSample s;
    if (wo.z <= 0.0f) return s;

    const float kt = m.translucency;
    const float pSpec = clampf(m.specular * 4.0f, 0.06f, 0.5f);
    const float pTrans = (1.0f - pSpec) * kt;

    const float u = rng.nextFloat();
    if (u < pSpec) {
        const float a = maxf(1e-3f, m.roughness * m.roughness);
        const Vec3 h = sampleGgxVndf(wo, a, rng.next2D());
        s.wi = reflect(wo, h);
        if (s.wi.z <= 0.0f) return BsdfSample{};
    } else if (u < pSpec + pTrans) {
        const Vec3 d = sampleCosineHemisphere(rng.next2D());
        s.wi = Vec3(d.x, d.y, -d.z);
        s.transmitted = true;
    } else {
        s.wi = sampleCosineHemisphere(rng.next2D());
    }

    s.f = evalOpaque(m, wo, s.wi, &s.pdf);
    if (s.pdf <= 0.0f) return BsdfSample{};
    return s;
}

// ---------------------------------------------------------------------------
// Smooth dielectric -- the tarn.
//
// Specular in both directions, so it is flagged and skips light sampling: a
// delta lobe has zero probability of hitting the sun by chance, and asking it
// to would only add a zero to every shadow ray from the water.
// ---------------------------------------------------------------------------
V2_FN BsdfSample sampleDielectric(const Material &m, Vec3 wo, bool entering, Rng &rng) {
    BsdfSample s;
    s.specular = true;

    const float eta = entering ? (1.0f / m.ior) : m.ior;
    const float cosI = fabsf(wo.z);
    const float sin2T = eta * eta * (1.0f - cosI * cosI);

    float F;
    if (sin2T >= 1.0f) {
        F = 1.0f;  // total internal reflection
    } else {
        const float cosT = sqrtf(1.0f - sin2T);
        // Full Fresnel, not Schlick: at a water surface the grazing response is
        // most of what a lake looks like, and Schlick is visibly wrong there.
        const float rs = (cosI - m.ior * cosT) / (cosI + m.ior * cosT);
        const float rp = (m.ior * cosI - cosT) / (m.ior * cosI + cosT);
        F = 0.5f * (rs * rs + rp * rp);
    }

    if (rng.nextFloat() < F) {
        s.wi = Vec3(-wo.x, -wo.y, wo.z);
        s.f = Vec3(F / fabsf(s.wi.z));
        s.pdf = F;
    } else {
        const float cosT = sqrtf(maxf(0.0f, 1.0f - sin2T));
        s.wi = normalize(Vec3(-wo.x * eta, -wo.y * eta, -copysignf(cosT, wo.z)));
        s.f = Vec3((1.0f - F) / fabsf(s.wi.z));
        s.pdf = 1.0f - F;
        s.transmitted = true;
    }
    return s;
}

}  // namespace v2
