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
// The same BSDF, with the two lobes kept apart.
//
// THIS EXISTS FOR DEMODULATION, and demodulation is the reason the denoisers
// downstream can tell a needle from the grain between two needles. Radiance
// that came out of the DIFFUSE lobe carries the surface's albedo as a factor;
// radiance from the SPECULAR lobe carries its Fresnel term instead. Divide each
// by the factor it carries and what is left is pure lighting -- smooth, low
// frequency, and safe to denoise. Multiply the texture back afterwards and not
// one voxel of detail has been through the filter.
//
// evalOpaque() below is now a thin wrapper over this, so the composited BSDF
// and the split one cannot drift apart: there is one implementation and the
// sum of its parts IS the whole.
//
// The lobe split is exactly the one the guide buffers are written against --
// see the primary-hit block in Trace.cs.slang. fDiff carries m.albedo (both
// the reflection and the transmission lobe do, with the (1-kt) / kt weights
// left INSIDE the radiance where they belong); fSpec carries the Fresnel term.
// ---------------------------------------------------------------------------
void evalOpaqueSplit(Surface m, float3 wo, float3 wi, out float3 fDiff, out float3 fSpec,
                     out float pdf)
{
    fDiff = float3(0.0f);
    fSpec = float3(0.0f);
    pdf = 0.0f;
    if (wo.z <= 0.0f)
        return;

    const float kt = m.translucency;
    // Lobe-selection probabilities. The specular share is tied to Fresnel at
    // normal incidence so a rough dark surface does not spend half its samples
    // on a lobe worth four percent of its response.
    const float pSpec = clamp(m.specular * 4.0f, 0.06f, 0.5f);
    const float pDiff = (1.0f - pSpec) * (1.0f - kt);
    const float pTrans = (1.0f - pSpec) * kt;

    if (wi.z > 0.0f)
    {
        // Diffuse reflection.
        fDiff += m.albedo * (INV_PI * (1.0f - kt));
        pdf += pDiff * wi.z * INV_PI;

        // Specular reflection.
        const float3 h = normalize(wo + wi);
        const float a = max(1e-3f, m.roughness * m.roughness);
        const float a2 = a * a;
        const float D = ggxD(h.z, a2);
        const float G = ggxG2(wo.z, wi.z, a2);
        const float F = schlick(m.specular, dot(wo, h));
        fSpec += float3(D * G * F / max(1e-9f, 4.0f * wo.z * wi.z));

        // VNDF pdf: D_visible(h) / (4 |wo.h|).
        const float dv = D * ggxG2(wo.z, wo.z, a2) * abs(dot(wo, h)) / max(1e-9f, wo.z);
        pdf += pSpec * dv / max(1e-9f, 4.0f * abs(dot(wo, h)));
    }
    else if (kt > 0.0f)
    {
        // Diffuse transmission: the needle glowing from behind. Modulated by
        // the albedo like the reflection lobe, so it belongs in the same
        // channel -- the kt weight rides in the radiance, not in the divisor.
        fDiff += m.albedo * (INV_PI * kt);
        pdf += pTrans * (-wi.z) * INV_PI;
    }
}

// ---------------------------------------------------------------------------
// Opaque BSDF, evaluated in the local frame where the shading normal is +z.
//
// wo and wi both point AWAY from the surface. wi.z < 0 means the ray went
// through, which only the translucency lobe can produce.
// ---------------------------------------------------------------------------
float3 evalOpaque(Surface m, float3 wo, float3 wi, out float pdf)
{
    float3 fDiff, fSpec;
    evalOpaqueSplit(m, wo, wi, fDiff, fSpec, pdf);
    return fDiff + fSpec;
}

// THE SAMPLE NUMBERS ARE ARGUMENTS, not draws, and that split is what lets the
// primary bounce take its direction from the blue-noise mask while every deeper
// one keeps the pixel's PCG stream. `u` picks the lobe and `u2` picks the
// direction within it; all three branches consume exactly one 2D sample, so the
// caller's draw count is the same whichever lobe wins.
BsdfSample sampleOpaqueU(Surface m, float3 wo, float u, float2 u2)
{
    BsdfSample s = emptySample();
    if (wo.z <= 0.0f)
        return s;

    const float kt = m.translucency;
    const float pSpec = clamp(m.specular * 4.0f, 0.06f, 0.5f);
    const float pTrans = (1.0f - pSpec) * kt;

    if (u < pSpec)
    {
        const float a = max(1e-3f, m.roughness * m.roughness);
        const float3 h = sampleGgxVndf(wo, a, u2);
        s.wi = reflect3(wo, h);
        if (s.wi.z <= 0.0f)
            return emptySample();
    }
    else if (u < pSpec + pTrans)
    {
        const float3 d = sampleCosineHemisphere(u2);
        s.wi = float3(d.x, d.y, -d.z);
        s.transmitted = true;
    }
    else
    {
        s.wi = sampleCosineHemisphere(u2);
    }

    s.f = evalOpaque(m, wo, s.wi, s.pdf);
    if (s.pdf <= 0.0f)
        return emptySample();
    return s;
}

// The rng-drawing form, which is what every vertex past the first still uses.
// The order of the two draws is the order the old single-function version used,
// so a path that goes through here consumes the stream exactly as before.
BsdfSample sampleOpaque(Surface m, float3 wo, inout Rng rng)
{
    const float u = rng.nextFloat();
    const float2 u2 = rng.next2D();
    return sampleOpaqueU(m, wo, u, u2);
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

// ---------------------------------------------------------------------------
// GROUND SHADE -- HERE, ONCE, BECAUSE TWO FILES NEED IT.
//
// This used to be defined separately in Trace.cs.slang and in
// ProbeTrace.cs.slang: two copies of one rule, which is two answers to one
// question the moment either is edited. The probes would have shaded the
// ground differently from the camera and nothing would have said so -- the
// indirect light would simply have been the wrong colour.
// ---------------------------------------------------------------------------
uint hashVoxel(int3 v)
{
    uint h = uint(v.x) * 374761393u + uint(v.y) * 1103515245u + uint(v.z) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

// ---------------------------------------------------------------------------
// WHICH SHADE OF GROUND THIS VOXEL IS.
//
// A pine's bark is half a dozen browns scattered voxel by voxel, and that
// scatter is drawn HERE rather than stored: the host writes one material id
// per family and the device spreads it over the family's ramp using a hash of
// the voxel's own coordinate. One id in the store, many shades on screen.
//
// TAKES A MATERIAL ID, NOT A PACKED TRIANGLE WORD. It used to take the word so
// it could pull the material byte and the grass-strand bits out of it; there
// are no triangle words now, and a voxel hands its material over directly.
//
// The `strand` argument is what remains of that: it was three bits the mesher
// stored on a grass blade so the ramp could be read UP the blade rather than
// scattered along it. Nothing writes it any more, every caller passes 0, and
// the test below already reads 0 as "not a strand". It stays in the signature
// because the gradient is worth rebuilding when grass comes back, and deleting
// it would lose the one place that records how it worked.
// ---------------------------------------------------------------------------
uint groundShadeId(uint mtl, uint strand, float3 p, float3 ng)
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
    else if (mtl == kRock)
    {
        // ROCK IS ONE ID AND SIX SHADES. The mesher writes kRock for a whole
        // hillside so its faces merge into long runs; the variation is spent
        // here instead, per voxel, exactly as grass and soil already do it.
        base = kStone0;
        count = kStoneCount;
    }
    else if (mtl == kBedrock)
    {
        // THE FLOOR OF THE WORLD, and one id again for the same reason kRock
        // is: the host writes mat::BEDROCK for ten voxels under every column
        // in the world, and spending the variation here costs nothing and
        // keeps those runs merged.
        base = kBedrock0;
        count = kBedrockCount;
    }
    else
    {
        return mtl; // sand, water, and everything a model brought with it
    }

    // HALF A VOXEL ALONG THE NORMAL, so the sample lands INSIDE the voxel the
    // face belongs to rather than exactly on its boundary -- where the floor of
    // a coordinate is a coin toss and every edge in the frame would shimmer
    // between two shades as the camera moved.
    const int3 v = int3(floor((p - ng * (0.5f * kVoxelM)) / kVoxelM));

    // -----------------------------------------------------------------------
    // A BLADE IS A GRADIENT, NOT A SCATTER.
    //
    // The hash above is right for the ground, which is a material seen from
    // above: what varies over a lawn is the plant, and the plant changes from
    // one tuft to the next. A standing blade is the opposite case. Its base is
    // buried in its own sward and sees almost no sky; its tip sees all of it.
    // Scattering shades up a blade paints that away and leaves a green stick.
    //
    // So a strand reads the ramp ALONG ITS HEIGHT -- kGrass0 at the soil, the
    // lightest green at the tip -- and the ramp is already sorted that way,
    // being the pines' own foliage taken in order of luminance.
    //
    // MEASURED FROM THE GROUND, NOT NORMALISED PER BLADE, which is a choice
    // and not a shortcut. Normalising would make every strand span the whole
    // ramp, so a three-voxel blade and a six-voxel one standing side by side
    // would disagree about the colour of the same height -- per-blade paint
    // rather than a light field. Absolute, they agree everywhere, short blades
    // stay dark because a short blade IS in shadow, and only the ones tall
    // enough to clear the sward carry a lit tip.
    //
    // The row comes back from the three bits the mesher stored: modular
    // arithmetic, so it is right below sea level too, where v.y is negative.
    // -----------------------------------------------------------------------
    if (strand != 0u && base == kGrass0)
    {
        const uint row = (uint(v.y) - (strand - 1u)) & kStrandRowMask;
        return kGrass0 + min(row, kGrassCount - 1u);
    }

    return base + hashVoxel(v) % count;
}
