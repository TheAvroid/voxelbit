// ---------------------------------------------------------------------------
// Sky.slang -- the device half of Preetham's analytic daylight model.
//
// WHY AN ANALYTIC SKY AND NOT A CONSTANT: almost all of the light reaching the
// floor of a wood is sky light, not sun -- the canopy blocks the sun over most
// of the ground. Sky light is strongly directional (bright near the horizon in
// haze, deep blue at zenith) and strongly coloured, and a constant-colour dome
// loses the blue fill on the shaded side of every trunk, which is a large part
// of why an outdoor render looks outdoor.
//
// The host fits the coefficients (see scene/sky.h) and this file evaluates
// them. That split is why the miss path costs twenty lines of arithmetic with
// no branches worth the name, instead of the tan() and pow() the fit needs.
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"
#include "Vecmath.slang"

float skySolidAngle()
{
    return TWO_PI * (1.0f - kSunCosThetaMax);
}

float perezF(float cosTheta, float gamma, float a, float b, float c, float d, float e)
{
    const float ct = max(cosTheta, 0.01f);
    const float cg = cos(gamma);
    return (1.0f + a * exp(b / ct)) * (1.0f + c * exp(d * gamma) + e * cg * cg);
}

// Radiance of the dome in the renderer's working units.
float3 skyPerez(V4Sky sky, float3 dir)
{
    const float3 dn = normalize(dir);
    const float cosTheta = max(dn.y, 0.01f);
    const float gamma = acos(clamp(dot(dn, sky.sunDir), -1.0f, 1.0f));

    // xyY, one channel at a time. Written out rather than looped because the
    // coefficients live in the .xyz of five float4s -- see the packing note in
    // Shared.slang for why they are not an array.
    const float3 a = float3(sky.A.x, sky.A.y, sky.A.z);
    const float3 b = float3(sky.B.x, sky.B.y, sky.B.z);
    const float3 c = float3(sky.C.x, sky.C.y, sky.C.z);
    const float3 d = float3(sky.D.x, sky.D.y, sky.D.z);
    const float3 e = float3(sky.E.x, sky.E.y, sky.E.z);
    const float3 zen = float3(sky.zenith.x, sky.zenith.y, sky.zenith.z);
    const float3 nf = float3(sky.normF.x, sky.normF.y, sky.normF.z);

    float3 v;
    v.x = zen.x * perezF(cosTheta, gamma, a.x, b.x, c.x, d.x, e.x) / max(1e-6f, nf.x);
    v.y = zen.y * perezF(cosTheta, gamma, a.y, b.y, c.y, d.y, e.y) / max(1e-6f, nf.y);
    v.z = zen.z * perezF(cosTheta, gamma, a.z, b.z, c.z, d.z, e.z) / max(1e-6f, nf.z);

    // xyY -> XYZ -> linear sRGB.
    const float Y = max(0.0f, v.x), x = v.y, y = max(1e-4f, v.z);
    const float X = (x / y) * Y;
    const float Z = ((1.0f - x - y) / y) * Y;
    float3 rgb = float3(
        3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
        -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
        0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z
    );
    rgb = max(rgb, float3(0.0f));
    return rgb * sky.skyScale;
}

// The dome WITHOUT the sun disk. Kept separate from the disk so multiple
// importance sampling can weight the two contributions differently: the disk is
// found by the cone sampler, the dome by whatever the BSDF did.
float3 skyDome(V4Sky sky, float3 d)
{
    // Below the horizon there is no sky, but a hard cut at y = 0 draws a
    // visible seam right where the eye is looking. Fade into a dim ground
    // bounce instead -- this stands in for the world beyond what is resident,
    // and it keeps the far treeline sitting in something.
    if (d.y <= 0.0f)
    {
        const float t = saturate(-d.y / 0.10f);
        const float3 horizon = skyPerez(sky, float3(d.x, 1e-4f, d.z));
        const float avg = (horizon.x + horizon.y + horizon.z) * (1.0f / 3.0f);
        return lerp(horizon, sky.groundAlbedo * avg, sstep(t));
    }
    return skyPerez(sky, d);
}

// The solar disk itself. Zero outside the cone, so this can simply be added.
float3 skyDisk(V4Sky sky, float3 d)
{
    return dot(d, sky.sunDir) >= kSunCosThetaMax ? sky.sunRadiance : float3(0.0f);
}
