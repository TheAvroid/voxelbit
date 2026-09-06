// ---------------------------------------------------------------------------
// AtmoSkyView.cs.slang -- the dome the tracer actually reads.
//
// 192x108 of scattered radiance, in (azimuth relative to the sun, zenith). The
// only one of the three tables that depends on where the sun is, so the only
// one rebuilt when it moves -- see atmosphere.h, which rebuilds it when the sun
// has turned far enough to matter and skips it otherwise.
//
// ---------------------------------------------------------------------------
// WHY A TABLE AND NOT THE MARCH ITSELF.
//
// The march below is 32 steps with a transmittance fetch and a multiscatter
// fetch each. Running it on every escaping ray in a path tracer -- and in this
// engine most rays escape -- is somewhere north of a hundred times the cost of
// what Sky.slang does today. Run once per texel it is 20k invocations for the
// whole frame, and the tracer's sky lookup stays a single bilinear fetch, which
// is roughly what evaluating the Perez fit cost it anyway.
//
// The resolution is affordable because of the parameterisation, not despite it:
// atmoUvToSkyViewParams spends half its rows in the few degrees either side of
// the horizon, where every interesting feature of a real sky lives.
//
// ---------------------------------------------------------------------------
// THE UNITS ARE THE ENGINE'S, and that is not a fudge.
//
// gSolarIrradiance is handed in as the same number sky.h uses for the sun -- 22
// working units at full elevation. The integral below is linear in it, so the
// radiance that comes out is in the same units the rest of the renderer already
// speaks, with no calibration constant anywhere.
//
// It is worth recording WHY that works, because it looks like luck. Preetham's
// zenith at 45 degrees and turbidity 2.6 is 6193 cd/m^2, which sky.h scales by
// 0.00018 to 1.11 working units, against a sun irradiance of 22 -- a ratio of
// 0.051 per steradian. The physical ratio for a real clear sky is about 0.055.
// The two halves of the existing sky were already very nearly photometrically
// consistent with each other, so an integrator fed the same irradiance lands in
// the same place. Had they not been, this line would need a fudge factor and it
// would deserve to be called one.
// ---------------------------------------------------------------------------
#include "Atmosphere.slang"

Texture2D<float4> gTransmittance;
Texture2D<float4> gMultiScatter;
SamplerState gLutSamp;
RWTexture2D<float4> gSkyView;

cbuffer AtmoSkyCB
{
    float3 gSunDir;           // world space, y up
    float gViewHeight;        // km from the planet centre, so ground radius + camera height
    float3 gSolarIrradiance;  // the engine's own sun, unramped
    float gPad0;
    // WHAT THE PHYSICS CANNOT GIVE. This model knows about sunlight and nothing
    // else, so 18 degrees after sunset it correctly returns zero -- and zero is
    // a black screen. Airglow, starlight, zodiacal light and scattered
    // moonlight are all real and none of them is in the integral. The host
    // fades this in as the sun goes down; it is the one term here that is
    // asserted rather than derived, and it is deliberately the same brightness
    // the Preetham path's 2% floor already produced.
    float3 gNightSky;
    float gPad1;
};

// 32 steps. The horizon rows are the expensive ones -- a grazing ray crosses
// hundreds of kilometres of air -- but they are also where the analytic step
// integral in atmoIntegrate does its work, so the error stays bounded rather
// than growing with the path length.
static const int kSteps = 32;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kAtmoSkyW) || tid.y >= uint(kAtmoSkyH)) return;

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(float(kAtmoSkyW), float(kAtmoSkyH));

    float viewZenithCos, lightViewCos;
    atmoUvToSkyViewParams(uv, gViewHeight, viewZenithCos, lightViewCos);

    // THE TABLE IS BUILT IN THE SUN'S OWN FRAME, which is what makes two angles
    // enough for a three-dimensional problem. The sun is placed in the x-y
    // plane and the view direction is reconstructed around it, so azimuth is
    // stored RELATIVE to the sun and the table stays valid as the sun goes
    // round. Sky.slang undoes the same rotation when it reads.
    const float sunZenithCos = clamp(gSunDir.y, -1.0f, 1.0f);
    const float3 sunLocal =
        normalize(float3(sqrt(saturate(1.0f - sunZenithCos * sunZenithCos)), sunZenithCos, 0.0f));

    const float3 o = float3(0.0f, gViewHeight, 0.0f);
    const float vzSin = sqrt(saturate(1.0f - viewZenithCos * viewZenithCos));
    const float3 d = float3(vzSin * lightViewCos, viewZenithCos,
                            vzSin * sqrt(saturate(1.0f - lightViewCos * lightViewCos)));

    // Real phases, and the multiply-scattered term added back in.
    const AtmoScatterResult res = atmoIntegrate(o, d, sunLocal, gSolarIrradiance, gTransmittance,
                                                gMultiScatter, gLutSamp, kSteps, false, true);

    gSkyView[tid.xy] = float4(res.L + gNightSky, 1.0f);
}
