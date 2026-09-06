// ---------------------------------------------------------------------------
// Material.slang -- the BSDFs a conifer wood actually needs.
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
// Transcribed from v2's material.h without a single number changed. The port
// was a chance to redesign this and it was deliberately not taken -- these
// lobes were tuned against this scene, and a renderer swap is the worst
// possible time to also move the thing you would be comparing against.
// ---------------------------------------------------------------------------
#pragma once

#include "Vecmath.slang"

static const uint MAT_OPAQUE = 0;     // ground, bark, needles
static const uint MAT_DIELECTRIC = 1; // water

struct Surface
{
    uint type;
    float3 albedo;
    float roughness;
    float specular;
    float translucency;
    float ior;
};

struct BsdfSample
{
    float3 f;
    float3 wi;
    float pdf;
    bool specular;    // skip MIS and light sampling through this vertex
    bool transmitted;
};

BsdfSample emptySample()
{
    BsdfSample s;
    s.f = float3(0.0f);
    s.wi = float3(0.0f);
    s.pdf = 0.0f;
    s.specular = false;
    s.transmitted = false;
    return s;
}

// ---------------------------------------------------------------------------
// GGX / Trowbridge-Reitz
// ---------------------------------------------------------------------------
float ggxD(float cosTh, float a2)
{
    const float d = cosTh * cosTh * (a2 - 1.0f) + 1.0f;
    return a2 / max(1e-9f, PI * d * d);
}

// Smith height-correlated masking-shadowing, the Heitz form.
float ggxG2(float nv, float nl, float a2)
{
    const float lv = nl * sqrt(nv * nv * (1.0f - a2) + a2);
    const float ll = nv * sqrt(nl * nl * (1.0f - a2) + a2);
    return 2.0f * nl * nv / max(1e-9f, lv + ll);
}

float schlick(float f0, float c)
{
    const float m = saturate(1.0f - c);
    const float m2 = m * m;
    return f0 + (1.0f - f0) * m2 * m2 * m;
}

// Heitz 2018: sample the distribution of VISIBLE normals. Sampling D alone
// generates backfacing microfacets at grazing angles, which have to be thrown
// away -- and grazing is exactly where bark and a wet rock get their sheen, so
// that is the worst place to lose samples.
float3 sampleGgxVndf(float3 wo, float a, float2 u)
{
    const float3 vh = normalize(float3(a * wo.x, a * wo.y, wo.z));
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0.0f ? float3(-vh.y, vh.x, 0.0f) / sqrt(lensq) : float3(1, 0, 0);
    const float3 t2 = cross(vh, t1);

    const float r = sqrt(u.x);
    const float phi = TWO_PI * u.y;
    const float p1 = r * cos(phi);
    float p2 = r * sin(phi);
    const float s = 0.5f * (1.0f + vh.z);
    p2 = (1.0f - s) * sqrt(max(0.0f, 1.0f - p1 * p1)) + s * p2;

    const float3 nh = t1 * p1 + t2 * p2 + vh * sqrt(max(0.0f, 1.0f - p1 * p1 - p2 * p2));
    return normalize(float3(a * nh.x, a * nh.y, max(1e-6f, nh.z)));
}

// ---------------------------------------------------------------------------
// Opaque BSDF, evaluated in the local frame where the shading normal is +z.
//
// wo and wi both point AWAY from the surface. wi.z < 0 means the ray went
// through, which only the translucency lobe can produce.
// ---------------------------------------------------------------------------
float3 evalOpaque(Surface m, float3 wo, float3 wi, out float pdf)
{
    pdf = 0.0f;
    if (wo.z <= 0.0f)
        return float3(0.0f);

    const float kt = m.translucency;
    // Lobe-selection probabilities. The specular share is tied to Fresnel at
    // normal incidence so a rough dark surface does not spend half its samples
    // on a lobe worth four percent of its response.
    const float pSpec = clamp(m.specular * 4.0f, 0.06f, 0.5f);
    const float pDiff = (1.0f - pSpec) * (1.0f - kt);
    const float pTrans = (1.0f - pSpec) * kt;

    float3 f = float3(0.0f);

    if (wi.z > 0.0f)
    {
        // Diffuse reflection.
        f += m.albedo * (INV_PI * (1.0f - kt));
        pdf += pDiff * wi.z * INV_PI;

        // Specular reflection.
        const float3 h = normalize(wo + wi);
        const float a = max(1e-3f, m.roughness * m.roughness);
        const float a2 = a * a;
        const float D = ggxD(h.z, a2);
        const float G = ggxG2(wo.z, wi.z, a2);
        const float F = schlick(m.specular, dot(wo, h));
        f += float3(D * G * F / max(1e-9f, 4.0f * wo.z * wi.z));

        // VNDF pdf: D_visible(h) / (4 |wo.h|).
        const float dv = D * ggxG2(wo.z, wo.z, a2) * abs(dot(wo, h)) / max(1e-9f, wo.z);
        pdf += pSpec * dv / max(1e-9f, 4.0f * abs(dot(wo, h)));
    }
    else if (kt > 0.0f)
    {
        // Diffuse transmission: the needle glowing from behind.
        f += m.albedo * (INV_PI * kt);
        pdf += pTrans * (-wi.z) * INV_PI;
    }
    return f;
}

BsdfSample sampleOpaque(Surface m, float3 wo, inout Rng rng)
{
    BsdfSample s = emptySample();
    if (wo.z <= 0.0f)
        return s;

    const float kt = m.translucency;
    const float pSpec = clamp(m.specular * 4.0f, 0.06f, 0.5f);
    const float pTrans = (1.0f - pSpec) * kt;

    const float u = rng.nextFloat();
    if (u < pSpec)
    {
        const float a = max(1e-3f, m.roughness * m.roughness);
        const float3 h = sampleGgxVndf(wo, a, rng.next2D());
        s.wi = reflect3(wo, h);
        if (s.wi.z <= 0.0f)
            return emptySample();
    }
    else if (u < pSpec + pTrans)
    {
        const float3 d = sampleCosineHemisphere(rng.next2D());
        s.wi = float3(d.x, d.y, -d.z);
        s.transmitted = true;
    }
    else
    {
        s.wi = sampleCosineHemisphere(rng.next2D());
    }

    s.f = evalOpaque(m, wo, s.wi, s.pdf);
    if (s.pdf <= 0.0f)
        return emptySample();
    return s;
}

// ---------------------------------------------------------------------------
// Smooth dielectric -- the tarn.
//
// Specular in both directions, so it is flagged and skips light sampling: a
// delta lobe has zero probability of hitting the sun by chance, and asking it
// to would only add a zero to every shadow ray from the water.
// ---------------------------------------------------------------------------
BsdfSample sampleDielectric(Surface m, float3 wo, bool entering, inout Rng rng)
{
    BsdfSample s = emptySample();
    s.specular = true;

    const float eta = entering ? (1.0f / m.ior) : m.ior;
    const float cosI = abs(wo.z);
    const float sin2T = eta * eta * (1.0f - cosI * cosI);

    float F;
    if (sin2T >= 1.0f)
    {
        F = 1.0f; // total internal reflection
    }
    else
    {
        const float cosT = sqrt(1.0f - sin2T);
        // Full Fresnel, not Schlick: at a water surface the grazing response is
        // most of what a lake looks like, and Schlick is visibly wrong there.
        const float rs = (cosI - m.ior * cosT) / (cosI + m.ior * cosT);
        const float rp = (m.ior * cosI - cosT) / (m.ior * cosI + cosT);
        F = 0.5f * (rs * rs + rp * rp);
    }

    if (rng.nextFloat() < F)
    {
        s.wi = float3(-wo.x, -wo.y, wo.z);
        s.f = float3(F / abs(s.wi.z));
        s.pdf = F;
    }
    else
    {
        const float cosT = sqrt(max(0.0f, 1.0f - sin2T));
        s.wi = normalize(float3(-wo.x * eta, -wo.y * eta, -(wo.z >= 0.0f ? cosT : -cosT)));
        s.f = float3((1.0f - F) / abs(s.wi.z));
        s.pdf = 1.0f - F;
        s.transmitted = true;
    }
    return s;
}
