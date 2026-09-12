// ---------------------------------------------------------------------------
// NrcSdkResolve.cs.slang -- fold the cache's predictions into the FILM, not
//                           onto the picture.
//
// WHY THIS EXISTS AT ALL. nrc::Context::Resolve adds the predicted radiance into
// whatever texture it is handed, which is right for a renderer whose output
// holds the samples of the frame being drawn. v2's does not:
//
//     gColor[pix] = acc.xyz / max(1, acc.w)
//
// color_ is the running MEAN of every sample the film has taken, rewritten from
// the accumulator each frame. Adding one sample's worth of radiance to an
// N-sample average is wrong twice: it lands N times too strong, and it lands
// after the division so it never accumulates. Measured, it made the picture
// five times worse than no cache at all.
//
// ---------------------------------------------------------------------------
// AND WHY IT NO LONGER READS THE SDK'S BUFFERS ITSELF.
//
// The first version of this pass walked QueryPathInfo and QueryRadiance
// directly, the way the integration guide's custom-resolve example does. The
// index arithmetic matched NrcCalculateQueryPathIndex exactly and it still
// contributed nothing -- the pass was not even dispatching, which a coverage
// probe eventually showed by painting 0.3% of the frame where it should have
// painted half.
//
// Chasing that further was the wrong move. The SDK's OWN resolve demonstrably
// works -- its debug views draw the wood -- and the only thing wrong with it is
// where it puts the answer. So it now writes into a scratch texture and this
// pass does the one thing it was actually needed for: add that into the
// accumulator at the right weight. No index arithmetic, no dependency on the
// SDK's buffer layout, and the piece that was broken is simply gone.
// ---------------------------------------------------------------------------
#include "Shared.slang"

// What the SDK's Resolve wrote this frame: predicted radiance, already
// modulated by path throughput, one texel per pixel.
Texture2D<float4> gNrcScratch;

RWTexture2D<float4> gAccum;
RWTexture2D<float4> gColorOut;

cbuffer NrcResolveCB {
    uint2 gResolveDim;
    // Whether there IS a film. v2 renders without one when the caller wants a
    // single unaccumulated sample -- see keepFilm in Shared.slang -- and then
    // gColorOut holds radiance directly and gAccum is untouched.
    uint gResolveKeepFilm;
    uint gResolvePad;
};

[shader("compute")]
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (any(tid.xy >= gResolveDim))
        return;
    const uint2 pix = tid.xy;

    const float3 radiance = gNrcScratch[pix].xyz;
    if (!all(isfinite(radiance)) || all(radiance <= 0.0f))
        return;

    if (gResolveKeepFilm != 0)
    {
        // INTO THE NUMERATOR, AND THE COUNT IS LEFT ALONE. The sample this
        // prediction belongs to was already counted by the trace that made the
        // query; incrementing w here would divide the film by one more sample
        // than it actually has.
        float4 acc = gAccum[pix];
        acc.xyz += radiance;
        gAccum[pix] = acc;
        gColorOut[pix] = float4(acc.xyz / max(1.0f, acc.w), 1.0f);
    }
    else
    {
        gColorOut[pix] += float4(radiance, 0.0f);
    }
}
