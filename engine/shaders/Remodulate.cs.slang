// ---------------------------------------------------------------------------
// Remodulate.cs.slang -- PHASE D. Put the texture back over clean lighting.
//
// The exact inverse of Demodulate.cs.slang, and it is written as its own file
// beside it rather than folded into the tone map so that the inverse is
// OBVIOUS: the same two divisors, multiplied instead of divided, plus the
// emission that was held out of both.
//
//     out = emission + albedo * diffuse + specAlbedo * specular
//
// which is the identity the split was built to satisfy. With the denoisers
// switched off it reproduces the composited image the tracer wrote to the last
// bit on every pixel whose albedo cleared the epsilon floor -- that is what
// --check-demod measures, and it is the test that the whole phase is sound.
//
// WHAT THE ORDER BUYS. The albedo multiplied in here has never been through a
// filter. It came out of the material table at the primary hit and went into a
// texture; the denoiser between the two passes only ever saw irradiance. So a
// needle one voxel across is exactly as sharp in the final frame as it was in
// the G-buffer, no matter how hard the lighting underneath it was filtered.
// That is the entire reason the demodulate/remodulate pair is worth two extra
// full-screen passes.
// ---------------------------------------------------------------------------
#include "Shared.slang"

Texture2D<float4> gInDiff;     // DENOISED irradiance, diffuse
Texture2D<float4> gInSpec;     // DENOISED irradiance, specular
Texture2D<float4> gAlbedo;     // the same divisor Demodulate used
Texture2D<float4> gSpecAlbedo;
Texture2D<float4> gEmissionTex; // modulated by nothing: sky, and in-scatter

RWTexture2D<float4> gOut;

cbuffer gRemodCB
{
    uint2 gDim;
    float gPad0, gPad1;
};

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    const uint2 pix = tid.xy;

    const float3 L = gEmissionTex[pix].xyz + gAlbedo[pix].xyz * gInDiff[pix].xyz +
                     gSpecAlbedo[pix].xyz * gInSpec[pix].xyz;

    // A denoiser can return a small negative on a ringing edge, and a negative
    // radiance is not a colour. Clamped here rather than in the tone map so the
    // buffer this pass produces is always a valid image on its own terms.
    gOut[pix] = float4(max(L, float3(0.0f)), 1.0f);
}
