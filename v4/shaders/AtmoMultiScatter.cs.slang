// ---------------------------------------------------------------------------
// AtmoMultiScatter.cs.slang -- the light that has bounced more than once.
//
// WHY THIS TABLE EXISTS AT ALL. Single scattering alone gives a sky that is too
// dark and too saturated: a clear zenith comes out nearly navy, the shadowed
// side of everything goes black, and deep twilight collapses to nothing several
// degrees too early. Real air is optically thick enough in the blue that a
// photon typically scatters several times before it reaches the eye, and the
// second and later bounces are most of the light in exactly the places a game
// looks at -- the zenith, the shaded side of a wood, the sky after sunset.
//
// Integrating them properly is a nested integral nobody can afford per pixel.
// Hillaire's trick, and it is the load-bearing idea of the whole paper:
//
//   1. Assume light that has scattered more than once is ISOTROPIC and varies
//      smoothly with altitude and sun angle -- so it needs no view direction
//      and no azimuth, and fits in 32x32.
//   2. Assume every scattering order after the first has the same spatial
//      distribution as the second.
//
// Under (2) the orders form a GEOMETRIC SERIES. If one unit of isotropic light
// put into a column comes back as f, the total over all orders is
// 1 + f + f^2 + ... = 1/(1-f), and the whole infinite sum collapses into one
// divide. That is what the last three lines of this shader are.
//
// BAKED ONCE. Like the transmittance table it depends only on the medium.
// ---------------------------------------------------------------------------
#include "Atmosphere.slang"

Texture2D<float4> gTransmittance;
SamplerState gLutSamp;
RWTexture2D<float4> gMultiScatter;

// 8x8 directions over the sphere, 20 march steps each. Small because the
// function being tabulated is smooth in both of its axes -- that smoothness is
// assumption (1) above, and it is the reason this can be 32x32 in the first
// place.
static const int kDirs = 8;
static const int kSteps = 20;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kAtmoMultiW) || tid.y >= uint(kAtmoMultiH)) return;

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(float(kAtmoMultiW), float(kAtmoMultiH));
    float r, muS;
    atmoUvToMultiParams(uv, r, muS);

    // The sun placed in the x-y plane at the requested zenith angle. Azimuth is
    // meaningless here -- the medium is spherically symmetric and the answer is
    // averaged over every direction anyway.
    const float3 o = float3(0.0f, r, 0.0f);
    const float3 sunDir = normalize(float3(sqrt(saturate(1.0f - muS * muS)), muS, 0.0f));

    float3 sumL = float3(0.0f, 0.0f, 0.0f);
    float3 sumMs = float3(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < kDirs; ++i)
    {
        for (int j = 0; j < kDirs; ++j)
        {
            // Uniform over the sphere: azimuth linear, and the COSINE of the
            // polar angle linear. Sampling the angle itself instead would crowd
            // the poles and quietly weight straight up far too heavily.
            const float a = (float(i) + 0.5f) / float(kDirs);
            const float b = (float(j) + 0.5f) / float(kDirs);
            const float theta = TWO_PI * a;
            const float cosPhi = 1.0f - 2.0f * b;
            const float sinPhi = sqrt(saturate(1.0f - cosPhi * cosPhi));
            const float3 d = float3(sinPhi * cos(theta), cosPhi, sinPhi * sin(theta));

            // UNIT IRRADIANCE, and deliberately. This table then describes the
            // MEDIUM rather than the engine's brightness, so it never needs
            // rebaking when the sun scale changes -- the sky-view pass
            // multiplies its own irradiance in when it samples.
            //
            // useMs is false: this is the quantity whose fixed point we are
            // computing, and it cannot be an input to its own construction.
            const AtmoScatterResult res =
                atmoIntegrate(o, d, sunDir, float3(1.0f, 1.0f, 1.0f), gTransmittance,
                              gTransmittance, gLutSamp, kSteps, true, false);
            sumL += res.L;
            sumMs += res.msAs1;
        }
    }

    const float inv = 1.0f / float(kDirs * kDirs);
    const float3 L = sumL * inv;
    const float3 f = sumMs * inv;

    // The series. Clamped away from 1 because f -> 1 would be an atmosphere
    // that returns everything put into it, which is not physical and is not
    // worth an infinity in a lookup table.
    const float3 sum = float3(1.0f, 1.0f, 1.0f) /
                       max(float3(1.0f, 1.0f, 1.0f) - f, float3(1e-3f, 1e-3f, 1e-3f));

    gMultiScatter[tid.xy] = float4(L * sum, 1.0f);
}
