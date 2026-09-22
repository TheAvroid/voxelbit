// ---------------------------------------------------------------------------
// AtmoTransmittance.cs.slang -- how much of a beam survives the trip out.
//
// The first and cheapest of the three tables: for a point at radius r and a
// direction whose cosine to straight up is mu, what fraction of each wavelength
// reaches the top of the atmosphere. Everything else in the model asks this
// question constantly -- every march step needs to know how much sunlight is
// still arriving where it stands -- so it is answered once into a 256x64 table
// and sampled thereafter.
//
// BAKED ONCE, NOT PER FRAME. It depends only on the medium, and the medium does
// not change: not with the sun, not with the camera, not with the hour. See
// atmosphere.h, which runs this exactly once and never again.
// ---------------------------------------------------------------------------
#include "Atmosphere.slang"

RWTexture2D<float4> gTransmittance;

// 40 steps, and the error is not where you would guess. The integrand is smooth
// everywhere except the last few hundred metres of a horizon-grazing ray, and
// the non-linear uv mapping in atmoUvToTransParams has already spent most of
// the table's width there -- so the steps land where the function moves without
// anyone having to distribute them by hand.
static const int kSteps = 40;

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= uint(kAtmoTransW) || tid.y >= uint(kAtmoTransH)) return;

    const float2 uv = (float2(tid.xy) + 0.5f) / float2(float(kAtmoTransW), float(kAtmoTransH));
    float r, mu;
    atmoUvToTransParams(uv, r, mu);

    // The ray, in a plane. Nothing here depends on azimuth -- the medium is
    // spherically symmetric -- so the whole problem is two dimensional and the
    // third component is a constant zero.
    const float3 o = float3(0.0f, r, 0.0f);
    const float3 d = float3(sqrt(max(1.0f - mu * mu, 0.0f)), mu, 0.0f);

    // ALWAYS TO THE TOP, NEVER TO THE GROUND. A ray that would strike the
    // planet still gets its optical depth to the boundary, because the callers
    // that could hit the ground test for that themselves and the ones that
    // cannot must not pay for a branch. Bruneton's parameterisation is built
    // on the same assumption; breaking it here puts a hard edge along the
    // horizon in every table downstream.
    const float tTop = atmoRaySphere(o, d, kAtmoTopR);
    if (tTop <= 0.0f)
    {
        gTransmittance[tid.xy] = float4(1.0f, 1.0f, 1.0f, 1.0f);
        return;
    }

    const float dt = tTop / float(kSteps);
    float3 opticalDepth = float3(0.0f, 0.0f, 0.0f);

    for (int i = 0; i < kSteps; ++i)
    {
        // Midpoint rather than either end. The extinction falls exponentially
        // with altitude, so sampling at a step boundary biases every step the
        // same way and the whole integral inherits it.
        const float t = (float(i) + 0.5f) * dt;
        const float3 p = o + d * t;
        const float h = length(p) - kAtmoGroundR;
        opticalDepth += atmoMedium(h).extinction * dt;
    }

    gTransmittance[tid.xy] = float4(exp(-opticalDepth), 1.0f);
}
