// ---------------------------------------------------------------------------
// NrdPack.cs.slang -- v6's G-buffer, in the formats NRD insists on.
//
// WHY A TRANSLATION PASS AT ALL. NRD does not take a normal, a depth and a
// distance; it takes ITS normal, ITS depth and ITS distance, and each of the
// three differs from what this engine already has in a way that is silent when
// wrong:
//
//   NORMAL+ROUGHNESS  NRD packs both into one texture with an octahedral
//                     encoding whose exact form depends on the defines the
//                     denoiser's own shaders were compiled with. Writing a
//                     plain world normal there does not fail -- it decodes to a
//                     different direction, and the denoiser blurs across edges
//                     it should have respected.
//
//   VIEW Z            NRD wants LINEAR view-space depth in metres. v6's depth
//                     guide is the DirectX perspective mapping into 0..1,
//                     written for DLSS, which is a completely different curve.
//                     Handing it over directly makes every surface appear to be
//                     at almost the same distance, and REBLUR then happily
//                     blends the whole frame together.
//
//   HIT DISTANCE      NRD wants it NORMALISED against its own hit-distance
//                     parameters and the surface's view depth, not in metres.
//                     REBLUR_FrontEnd_GetNormHitDist is that normalisation and
//                     it has to be given the same hitDistParams the host puts
//                     in ReblurSettings, or the denoiser's idea of "near" and
//                     the engine's disagree.
//
// So each is converted here, once, into a texture NRD owns the format of. The
// alternative -- teaching Trace.cs.slang to write NRD's formats directly --
// would put a denoiser's encoding inside the path tracer and make the depth
// guide unusable by DLSS, which needs the other curve.
// ---------------------------------------------------------------------------
#include "Shared.slang"

// NRD's own front-end helpers, from the SDK's shader tree that Falcor deploys
// beside the exe. Including them is what guarantees the packing matches the
// denoiser rather than merely resembling it.
#define NRD_COMPILER_DXC 1
#define NRD_USE_OCT_NORMAL_ENCODING 1
#define NRD_USE_MATERIAL_ID 0
#include "nrd/Shaders/Include/NRD.hlsli"

Texture2D<float4> gInNormRough;  // world normal xyz, linear roughness w
Texture2D<float> gInDepth;       // NDC depth, the curve DLSS was given
Texture2D<float> gInAoHitDist;   // metres

RWTexture2D<float4> gOutNormRough;  // NRD packed
RWTexture2D<float> gOutViewZ;       // linear metres
RWTexture2D<float> gOutHitDist;     // NRD normalised

cbuffer gNrdPackCB
{
    uint2 gDim;
    // The projection v6's depth guide was written with -- see ndcDepth() in
    // Trace.cs.slang. Inverting it here rather than passing viewZ down from the
    // tracer keeps the tracer's output single-purpose.
    float gNear, gFar;
    // Must equal ReblurSettings::hitDistanceParameters on the host. Passed in
    // rather than hardcoded so there is one definition of it.
    float4 gHitDistParams;
    float gAoRadius;
    float gPad0, gPad1, gPad2;
};

// The exact inverse of ndcDepth(): d = (f/(f-n)) * (1 - n/z), so
// z = n / (1 - d*(f-n)/f).
float linearViewZ(float d)
{
    const float a = gFar / (gFar - gNear);
    const float t = 1.0f - d / max(1e-6f, a);
    return gNear / max(1e-6f, t);
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    const uint2 pix = tid.xy;

    const float4 nr = gInNormRough[pix];
    const float depth = gInDepth[pix];

    // A pixel where the primary ray escaped has depth 1. NRD's convention for
    // "no surface" is a very large view Z, which is what makes it leave the sky
    // alone instead of trying to denoise it against the geometry behind it.
    const bool isSky = depth >= 0.999999f;
    const float viewZ = isSky ? 1e6f : linearViewZ(depth);

    gOutViewZ[pix] = viewZ;
    gOutNormRough[pix] = NRD_FrontEnd_PackNormalAndRoughness(normalize(nr.xyz), nr.w, 0);

    // Metres -> NRD's normalised form. The sky writes 1, which reads as
    // "nothing occluding for as far as this can see".
    const float hit = gInAoHitDist[pix];
    gOutHitDist[pix] =
        isSky ? 1.0f : REBLUR_FrontEnd_GetNormHitDist(hit, viewZ, gHitDistParams, 1.0f);
}
