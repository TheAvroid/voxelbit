// ---------------------------------------------------------------------------
// Atmosphere.slang -- the medium itself, and the three LUT parameterisations
//                     that make it affordable.
//
// After Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique", which is itself a restructuring of Bruneton-Neyret. The
// mappings below are his; the constants are the standard Earth fit everyone
// uses, and are cited where they are not obvious.
//
// ---------------------------------------------------------------------------
// WHY THIS EXISTS ALONGSIDE Sky.slang RATHER THAN REPLACING IT.
//
// Preetham is an ANALYTIC FIT to a sun ABOVE the horizon. Everything it knows
// about the sky is baked into coefficients that take the sun's zenith angle as
// input, and below the horizon that input has nowhere to go: sky.h has to pin
// thetaS at 90 degrees, and the result is a sunset that never finishes -- the
// chromaticity freezes at x 0.434, y 0.445 and the whole dome is dimmed by a
// scalar instead. Dimming does not change hue, so the horizon stays orange all
// night, at every hour, for ever.
//
// The deeper problem is structural rather than a bug. Preetham's only
// AZIMUTHAL structure is the solar aureole -- the c*exp(d*gamma) term -- which
// is anchored to the sun. With the sun underground that term is centred on a
// point nobody can see, so a fit cannot put a glow over the place the sun
// actually set. No amount of coefficient tuning produces a twilight WEDGE,
// because there is no term in the model whose shape a wedge could be.
//
// An integral has no such problem. March a view ray toward the horizon with the
// sun 4 degrees down and the march simply picks up air that is still in
// sunlight higher up, near the sun's azimuth and nowhere else. The wedge, the
// earth's shadow opposite it, and the way both fade as the sun descends all
// come out of the same sum -- not because anyone modelled them, but because
// that is what the geometry does.
//
// ---------------------------------------------------------------------------
// EVERYTHING HERE IS IN KILOMETRES, and the world is in metres.
//
// Not a preference: the scale heights are 8 km and 1.2 km against a 6360 km
// planet, so the ratio between the largest and smallest length that matters is
// about 5000:1. In metres the radii are ~6.4e6 and squaring them for the
// ray-sphere test loses most of a float32's mantissa exactly where the horizon
// is -- which is the one place in the whole model the precision is visible. The
// conversion happens at the boundary, in atmoViewHeight().
// ---------------------------------------------------------------------------
#pragma once

#include "Shared.slang"
#include "Vecmath.slang"

// The planet itself, and the three table sizes, live in Shared.slang -- the
// host has to agree with them. What follows is the MEDIUM, which it does not.

// -- Rayleigh: scattering, no absorption -------------------------------------
//
// THE WHOLE REASON THE SKY IS BLUE AND A LOW SUN IS RED IS THIS ONE VECTOR.
// Cross-section goes as 1/lambda^4, so blue is scattered 5.7x more strongly
// than red -- out of the beam on a long path (a red sun) and into the eye from
// everywhere else (a blue sky). The two facts are the same coefficient seen
// from two directions.
static const float3 kRayleighS = float3(5.802e-3f, 13.558e-3f, 33.100e-3f);  // km^-1
static const float kRayleighH = 8.0f;                                        // km, scale height

// -- Mie: aerosol, forward-scattering, nearly grey ---------------------------
//
// Grey is the point. Haze whitens the horizon rather than colouring it, which
// is why turbidity in Preetham washes the sky out instead of tinting it.
static const float kMieS = 3.996e-3f;  // km^-1, scattering
static const float kMieA = 4.400e-3f;  // km^-1, absorption
static const float kMieH = 1.2f;       // km, scale height -- aerosol sits low
static const float kMieG = 0.8f;       // strongly forward

// -- Ozone: absorption only, and it is why twilight is BLUE ------------------
//
// A tent centred at 25 km with no scattering at all. It matters far more than
// its size suggests: at twilight the sightline to the horizon passes through
// the ozone layer at a very shallow angle, so the path length through it is
// enormous. Ozone absorbs in the orange, which is what stops a deep twilight
// from going brown as the Rayleigh signal dies -- take this term out and the
// late sky turns exactly the muddy colour the Preetham pin produces.
static const float3 kOzoneA = float3(0.650e-3f, 1.881e-3f, 0.085e-3f);  // km^-1
static const float kOzoneCenter = 25.0f;                                // km
static const float kOzoneWidth = 15.0f;                                 // km, half-width of the tent

// ---------------------------------------------------------------------------
// What the air is doing at an altitude, in one struct.
// ---------------------------------------------------------------------------
struct AtmoMedium
{
    float3 scatterR;    // Rayleigh scattering coefficient here
    float scatterM;     // Mie scattering coefficient here
    float3 extinction;  // everything that removes light: both, plus ozone
};

AtmoMedium atmoMedium(float h)  // h: altitude above the ground, km
{
    AtmoMedium m;
    const float dR = exp(-max(h, 0.0f) / kRayleighH);
    const float dM = exp(-max(h, 0.0f) / kMieH);
    // The tent. Linear either side of 25 km, clamped to zero outside the layer.
    const float dO = saturate(1.0f - abs(h - kOzoneCenter) / kOzoneWidth);
    m.scatterR = kRayleighS * dR;
    m.scatterM = kMieS * dM;
    m.extinction = m.scatterR + (kMieS + kMieA) * dM + kOzoneA * dO;
    return m;
}

// ---------------------------------------------------------------------------
// Nearest positive hit against a sphere centred at the origin, or -1.
//
// WRITTEN TO BE STABLE RATHER THAN SHORT. b*b - c instead of the schoolbook
// discriminant, because at the horizon the two roots are nearly equal and the
// usual form subtracts two large nearly-equal numbers there.
// ---------------------------------------------------------------------------
float atmoRaySphere(float3 o, float3 d, float R)
{
    const float b = dot(o, d);
    const float c = dot(o, o) - R * R;
    // Pointing away from a sphere we are already outside of.
    if (c > 0.0f && b > 0.0f) return -1.0f;
    const float disc = b * b - c;
    if (disc < 0.0f) return -1.0f;
    const float s = sqrt(disc);
    const float t0 = -b - s;
    const float t1 = -b + s;
    if (t1 < 0.0f) return -1.0f;
    return t0 < 0.0f ? t1 : t0;
}

// ---------------------------------------------------------------------------
// Phase functions.
//
// THE ANGLE IS dot(sunDir, viewDir) AND THE SIGN IS EARNED. Light propagates
// from the sun along -sunDir and scatters toward the eye along -viewDir, so the
// scattering angle cosine is dot(-sunDir, -viewDir) = dot(sunDir, viewDir).
// Looking straight at the sun is therefore cos = 1, which must be the FORWARD
// peak -- hence the minus in the denominator below, where VolFog.slang has a
// plus. The two files disagree because VolFog's wo points back at the camera
// and this one's does not; both are correct in their own convention and getting
// it wrong is invisible at the zenith and completely wrong at sunset.
// ---------------------------------------------------------------------------
float atmoRayleighPhase(float cosTheta)
{
    return (3.0f / (16.0f * PI)) * (1.0f + cosTheta * cosTheta);
}

float atmoMiePhase(float cosTheta, float g)
{
    const float g2 = g * g;
    const float denom = 1.0f + g2 - 2.0f * g * cosTheta;
    return ((1.0f - g2) / (4.0f * PI)) * rcp(max(denom * sqrt(max(denom, 1e-6f)), 1e-6f));
}

// ---------------------------------------------------------------------------
// TRANSMITTANCE LUT MAPPING (Hillaire section 4, after Bruneton).
//
// Parameterised by (r, mu): the radius the ray starts at and the cosine of its
// angle to straight up. The mapping is not uniform in mu -- it is uniform in
// the DISTANCE to the top of the atmosphere -- which is what puts texels where
// the function actually changes. Near the horizon mu barely moves while the
// path length runs away, and a linear-in-mu table spends almost every texel on
// the part of the sky where nothing happens.
// ---------------------------------------------------------------------------
void atmoUvToTransParams(float2 uv, out float r, out float mu)
{
    const float H = sqrt(max(kAtmoTopR * kAtmoTopR - kAtmoGroundR * kAtmoGroundR, 0.0f));
    const float rho = H * uv.y;
    r = sqrt(max(rho * rho + kAtmoGroundR * kAtmoGroundR, 0.0f));

    const float dMin = kAtmoTopR - r;
    const float dMax = rho + H;
    const float d = dMin + uv.x * (dMax - dMin);
    mu = (d == 0.0f) ? 1.0f : (H * H - rho * rho - d * d) / (2.0f * r * d);
    mu = clamp(mu, -1.0f, 1.0f);
}

float2 atmoTransParamsToUv(float r, float mu)
{
    const float H = sqrt(max(kAtmoTopR * kAtmoTopR - kAtmoGroundR * kAtmoGroundR, 0.0f));
    const float rho = sqrt(max(r * r - kAtmoGroundR * kAtmoGroundR, 0.0f));

    const float disc = r * r * (mu * mu - 1.0f) + kAtmoTopR * kAtmoTopR;
    const float d = max(0.0f, -r * mu + sqrt(max(disc, 0.0f)));

    const float dMin = kAtmoTopR - r;
    const float dMax = rho + H;
    return float2(saturate((d - dMin) / max(dMax - dMin, 1e-6f)), saturate(rho / max(H, 1e-6f)));
}

// The table, once it exists. Sampled rather than integrated everywhere below.
float3 atmoTransmittance(Texture2D<float4> lut, SamplerState samp, float r, float mu)
{
    return lut.SampleLevel(samp, atmoTransParamsToUv(r, mu), 0.0f).rgb;
}

// ---------------------------------------------------------------------------
// MULTIPLE SCATTERING LUT MAPPING.
//
// Two axes only: the sun's zenith cosine and the altitude. That is the whole
// approximation and it is worth stating plainly -- multiply scattered light is
// assumed ISOTROPIC and to vary smoothly with position, so it needs no view
// direction and no azimuth. It is the term that keeps a clear sky from going
// black at the zenith and stops deep twilight collapsing to nothing, and at
// 32x32 it costs nothing to store.
// ---------------------------------------------------------------------------
float2 atmoMultiParamsToUv(float r, float muS)
{
    return float2(saturate(muS * 0.5f + 0.5f),
                  saturate((r - kAtmoGroundR) / max(kAtmoTopR - kAtmoGroundR, 1e-6f)));
}

void atmoUvToMultiParams(float2 uv, out float r, out float muS)
{
    muS = clamp(uv.x * 2.0f - 1.0f, -1.0f, 1.0f);
    r = lerp(kAtmoGroundR + 1e-3f, kAtmoTopR - 1e-3f, uv.y);
}

// ---------------------------------------------------------------------------
// SKY-VIEW LUT MAPPING, and this is the one that earns the whole technique.
//
// The dome is stored in (azimuth-relative-to-sun, zenith) with a SPLIT,
// SQUARED zenith mapping: half the texels above the horizon and half below,
// each squashed quadratically toward the horizon line itself. That is where all
// the structure is -- the wedge, the haze band, the earth's shadow -- and a
// linear mapping puts most of its rows on empty sky at the top.
//
// The azimuth axis is squared too, so the resolution crowds toward the sun,
// where the aureole is.
// ---------------------------------------------------------------------------
// Half-texel handling. A LUT sampled with linear filtering at the very edge
// reads a blend with nothing, so the mapping is defined over texel CENTRES and
// these two convert between that and the 0..1 the sampler wants.
float atmoFromUnitToSubUv(float u, float res) { return (u + 0.5f / res) * (res / (res + 1.0f)); }
float atmoFromSubUvToUnit(float u, float res) { return (u - 0.5f / res) * (res / (res - 1.0f)); }

void atmoUvToSkyViewParams(float2 uv, float viewHeight, out float viewZenithCos,
                           out float lightViewCos)
{
    uv = float2(atmoFromSubUvToUnit(uv.x, float(kAtmoSkyW)),
                atmoFromSubUvToUnit(uv.y, float(kAtmoSkyH)));

    // The angle from straight up down to the horizon, for this altitude.
    const float vHorizon =
        sqrt(max(viewHeight * viewHeight - kAtmoGroundR * kAtmoGroundR, 0.0f));
    const float cosBeta = clamp(vHorizon / max(viewHeight, 1e-6f), -1.0f, 1.0f);
    const float beta = acos(cosBeta);
    const float zenithHorizon = PI - beta;

    if (uv.y < 0.5f)
    {
        // Above the horizon. Squared so the rows bunch up at the bottom.
        float c = 1.0f - 2.0f * uv.y;
        c = 1.0f - c * c;
        viewZenithCos = cos(zenithHorizon * c);
    }
    else
    {
        // Below it, squared the other way so they bunch at the top.
        float c = uv.y * 2.0f - 1.0f;
        c *= c;
        viewZenithCos = cos(zenithHorizon + beta * c);
    }

    float c = uv.x;
    c *= c;
    lightViewCos = -(c * 2.0f - 1.0f);
}

float2 atmoSkyViewParamsToUv(bool intersectsGround, float viewZenithCos, float lightViewCos,
                             float viewHeight)
{
    const float vHorizon =
        sqrt(max(viewHeight * viewHeight - kAtmoGroundR * kAtmoGroundR, 0.0f));
    const float cosBeta = clamp(vHorizon / max(viewHeight, 1e-6f), -1.0f, 1.0f);
    const float beta = acos(cosBeta);
    const float zenithHorizon = PI - beta;

    const float viewZenith = acos(clamp(viewZenithCos, -1.0f, 1.0f));

    float2 uv;
    if (!intersectsGround)
    {
        float c = saturate(viewZenith / max(zenithHorizon, 1e-6f));
        c = 1.0f - c;
        c = sqrt(max(c, 0.0f));
        uv.y = (1.0f - c) * 0.5f;
    }
    else
    {
        float c = saturate((viewZenith - zenithHorizon) / max(beta, 1e-6f));
        uv.y = sqrt(max(c, 0.0f)) * 0.5f + 0.5f;
    }

    uv.x = sqrt(saturate(-lightViewCos * 0.5f + 0.5f));

    return float2(atmoFromUnitToSubUv(uv.x, float(kAtmoSkyW)),
                  atmoFromUnitToSubUv(uv.y, float(kAtmoSkyH)));
}

// ---------------------------------------------------------------------------
// The world's Y-up frame, turned into the two angles the sky-view LUT wants.
//
// The engine's viewer is on the floor of a wood, so "up" is (0,1,0) and the
// radius is the planet plus the player's height. That is also why there is no
// aerial-perspective LUT in here: it exists for a camera that can be at 30 km,
// and this one cannot leave the trees.
// ---------------------------------------------------------------------------
float atmoViewHeight(float camY)
{
    // The one place metres become kilometres. Clamped a metre off the ground so
    // the horizon angle is always defined.
    return kAtmoGroundR + max(camY, 1.0f) * 0.001f;
}

void atmoViewAngles(float3 dir, float3 sunDir, out float viewZenithCos, out float lightViewCos)
{
    const float3 up = float3(0.0f, 1.0f, 0.0f);
    viewZenithCos = clamp(dir.y, -1.0f, 1.0f);

    // The sun's bearing measured in the view ray's own horizontal frame. Built
    // from cross products rather than atan2 of world x/z so it stays defined
    // when the ray is near vertical -- the degenerate case is a ray straight up,
    // where the azimuth genuinely does not matter and any finite answer will do.
    float3 side = cross(up, dir);
    const float sideLen = length(side);
    if (sideLen < 1e-4f)
    {
        lightViewCos = 1.0f;
        return;
    }
    side /= sideLen;
    const float3 forward = normalize(cross(side, up));
    const float2 lightOnPlane = float2(dot(sunDir, forward), dot(sunDir, side));
    const float l = length(lightOnPlane);
    lightViewCos = (l < 1e-4f) ? 1.0f : (lightOnPlane.x / l);
}

// ---------------------------------------------------------------------------
// THE INTEGRATOR, and both baking passes are this function under two flags.
//
//   uniformPhase   the multiple-scattering table is built on the assumption
//                  that scattered light is isotropic, so it integrates with a
//                  1/4pi phase where the sky-view table uses the real ones.
//   useMs          whether to add the multiply-scattered term back in. Off
//                  while BUILDING that table -- it is the fixed point of this
//                  equation and cannot be an input to its own construction.
//
// Returns the scattered radiance, the transmittance along the whole ray, and
// msAs1: what one unit of isotropic light put into this column comes back as.
// That last one is the quantity the multiple-scattering table is made of.
// ---------------------------------------------------------------------------
struct AtmoScatterResult
{
    float3 L;
    float3 msAs1;
    float3 transmittance;
};

AtmoScatterResult atmoIntegrate(float3 o, float3 d, float3 sunDir, float3 solarIrradiance,
                                Texture2D<float4> transLut, Texture2D<float4> msLut,
                                SamplerState samp, int steps, bool uniformPhase, bool useMs)
{
    AtmoScatterResult res;
    res.L = float3(0.0f, 0.0f, 0.0f);
    res.msAs1 = float3(0.0f, 0.0f, 0.0f);
    res.transmittance = float3(1.0f, 1.0f, 1.0f);

    // How long the ray stays in air: stopped by the planet if it meets it, by
    // the top of the atmosphere otherwise.
    const float tGround = atmoRaySphere(o, d, kAtmoGroundR);
    const float tTop = atmoRaySphere(o, d, kAtmoTopR);
    float tMax;
    if (tGround > 0.0f)
        tMax = tGround;
    else if (tTop > 0.0f)
        tMax = tTop;
    else
        return res;

    if (tMax <= 0.0f) return res;

    const float cosTheta = dot(sunDir, d);
    const float isoPhase = 1.0f / (4.0f * PI);
    const float phaseR = uniformPhase ? isoPhase : atmoRayleighPhase(cosTheta);
    const float phaseM = uniformPhase ? isoPhase : atmoMiePhase(cosTheta, kMieG);

    const float dt = tMax / float(steps);
    float3 throughput = float3(1.0f, 1.0f, 1.0f);

    for (int i = 0; i < steps; ++i)
    {
        const float t = (float(i) + 0.5f) * dt;
        const float3 p = o + d * t;
        const float r = max(length(p), kAtmoGroundR);
        const AtmoMedium m = atmoMedium(r - kAtmoGroundR);

        const float3 stepT = exp(-m.extinction * dt);
        const float3 up = p / max(r, 1e-6f);
        const float muS = clamp(dot(up, sunDir), -1.0f, 1.0f);

        // IS THE SUN ACTUALLY VISIBLE FROM HERE, and this single test is the
        // whole twilight wedge. With the sun below the observer's horizon the
        // low air along the sightline sits in the planet's own shadow and
        // contributes nothing, while the high air a few hundred kilometres
        // toward the sun's bearing is still in full daylight. The glow ends up
        // over the point where the sun set because that is the only place the
        // lit air is -- nobody had to put it there.
        const float shadow = (atmoRaySphere(p, sunDir, kAtmoGroundR) > 0.0f) ? 0.0f : 1.0f;
        const float3 sunT = atmoTransmittance(transLut, samp, r, muS);

        float3 inScatter = (m.scatterR * phaseR + m.scatterM * phaseM) * sunT * shadow;
        if (useMs)
        {
            const float3 ms = msLut.SampleLevel(samp, atmoMultiParamsToUv(r, muS), 0.0f).rgb;
            inScatter += (m.scatterR + m.scatterM) * ms;
        }
        const float3 S = solarIrradiance * inScatter;

        // THE ANALYTIC STEP INTEGRAL, not S * dt. Holding the extinction
        // constant across a step, the exact answer is (S - S*exp(-sigma*dt))/sigma.
        // It matters at the horizon, where one step can be optically thick and
        // the rectangle rule credits the step with light it should have
        // absorbed on the way out -- which shows as a horizon that is too
        // bright, in exactly the place the whole model is being judged on.
        const float3 sigma = max(m.extinction, float3(1e-9f, 1e-9f, 1e-9f));
        res.L += throughput * ((S - S * stepT) / sigma);

        const float3 msS = m.scatterR + m.scatterM;
        res.msAs1 += throughput * ((msS - msS * stepT) / sigma);

        throughput *= stepT;
    }

    res.transmittance = throughput;
    return res;
}
