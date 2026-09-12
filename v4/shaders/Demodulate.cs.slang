// ---------------------------------------------------------------------------
// Demodulate.cs.slang -- PHASE B. Take the texture out of the lighting.
//
// WHY THIS PASS EXISTS AT ALL, stated plainly, because it is the one step in
// the pipeline whose value is invisible in a still frame and enormous in a
// moving one.
//
// A denoiser is handed a noisy image and has to decide, per pixel, how much of
// what it sees is variance it should smooth away and how much is signal it must
// keep. On this scene that decision is impossible in principle. A pine canopy
// is thousands of needle-sized faces, each one voxel across, each a slightly
// different green -- and a one-sample path trace of it is thousands of
// needle-sized faces, each a slightly different green, PLUS noise. The two look
// identical to any filter that only sees the composited pixel. Every spatial
// denoiser this lineage tried came to the same wrong conclusion and turned the
// canopy to felt.
//
// Demodulation removes the question. The albedo -- which is exactly the
// high-frequency part, and which the tracer knows EXACTLY because it looked the
// material up -- is divided out here. What is left is irradiance: how much
// light reached this point, which is a smooth, low-frequency function that a
// denoiser can filter hard without destroying anything, because there is no
// longer any detail in it to destroy. The needles are put back afterwards by
// Remodulate.cs.slang, having never been through a filter at all.
//
// THE DIVISORS ARE NOT GUESSES. gDemodAlbedo is the exact factor the diffuse
// channel carries -- see evalOpaqueSplit in Material.slang -- so the division
// inverts it to the last bit on any pixel where the albedo is not near zero.
// The specular divisor is Fresnel at the VIEW angle where the lobe carried
// Fresnel at the HALF-VECTOR angle; those differ, and the difference is left
// standing deliberately. It is the same approximation NRD makes internally, it
// is a smooth function of view direction, and correcting it would need a second
// full-resolution surface to carry a number worth a fraction of a percent.
// ---------------------------------------------------------------------------
#include "Shared.slang"

Texture2D<float4> gInDiff;     // diffuse-lobe radiance, still modulated
Texture2D<float4> gInSpec;     // specular-lobe radiance, still modulated
Texture2D<float4> gAlbedo;     // the exact divisor for the diffuse channel
Texture2D<float4> gSpecAlbedo; // Fresnel at the view angle

RWTexture2D<float4> gOutDiff; // irradiance: lighting with no texture in it
RWTexture2D<float4> gOutSpec;

cbuffer gDemodCB
{
    uint2 gDim;
    // THE FLOOR UNDER EVERY DIVISION, and it is the whole safety story of this
    // pass. A voxel wood has genuinely black surfaces in it -- wet rock, bark
    // in deep shade, the underside of a needle -- and on those the albedo is
    // zero or a rounding error away from it. Dividing by that produces an
    // infinity, the denoiser propagates the infinity across its whole filter
    // footprint, and a single black voxel takes out a patch of canopy.
    //
    // So the divisor is floored. Where the albedo is below the floor the
    // demodulated value is meaningless -- but it is also multiplied by that
    // same near-zero albedo on the way back out, so it lands where it started:
    // black. The floor buys numerical safety and costs nothing visible.
    float gEpsilon;
    float gPad;
};

// Per channel, not on the luminance: a surface can be black in red and bright
// in green -- moss on wet granite is exactly that -- and flooring on the
// luminance would leave the red channel dividing by its own near-zero.
float3 safeDemod(float3 radiance, float3 albedo, float eps)
{
    return radiance / max(albedo, float3(eps, eps, eps));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    const uint2 pix = tid.xy;

    const float3 dif = gInDiff[pix].xyz;
    const float3 spc = gInSpec[pix].xyz;

    gOutDiff[pix] = float4(safeDemod(dif, gAlbedo[pix].xyz, gEpsilon), 1.0f);
    gOutSpec[pix] = float4(safeDemod(spc, gSpecAlbedo[pix].xyz, gEpsilon), 1.0f);
}
