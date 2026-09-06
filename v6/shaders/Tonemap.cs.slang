// ---------------------------------------------------------------------------
// Tonemap.cs.slang -- linear HDR to the swapchain.
//
// The CPU engine in this lineage did this across a worker pool and measured
// 50 ms a frame at 1280x720 -- more than the path tracing it was displaying,
// because pow() per channel per pixel is a genuinely expensive thing to do a
// million times. It ended up behind an 8192-entry lookup table to make the
// viewer usable. On the GPU the honest version is free.
//
// The OUTPUT IS AN SRGB TARGET, so this writes LINEAR values and the hardware
// does the transfer function on the way out. v2 encoded sRGB by hand because it
// was writing bytes into a buffer it then blitted through OpenGL; here the
// swapchain format is BGRA8UnormSrgb and doing it in the shader as well would
// apply the curve twice and wash the frame out.
// ---------------------------------------------------------------------------

Texture2D<float4> gSrc;

// PHASE C, auxiliary: the ambient-occlusion buffer, shown instead of the
// picture when asked.
//
// WORTH A DEBUG VIEW OF ITS OWN, because AO is the one signal in this engine
// whose correctness cannot be judged from the finished frame. Multiplied into
// the shading it is a subtle darkening, and a subtly WRONG darkening looks
// exactly like a subtly right one. On its own it is obvious in a second: the
// ground under a trunk should be black, open sky white, and the boundary should
// follow the GEOMETRY rather than the lighting.
Texture2D<float> gAoSrc;
RWTexture2D<float4> gDst;

cbuffer gTonemapCB
{
    // 0 the picture, 1 the raw AO hit distance, 2 the denoised one.
    int gShowAo;
    // The AO search radius, so metres can be shown as 0..1. Kept in metres in
    // the buffer itself because that is what NRD wants.
    float gAoRadius;
    uint2 gDim;
    float gExposure;
    // The toe of the curve below -- how much of a shadow survives it. 0.03 is
    // Narkowicz's published constant; this engine opens at 0.10. See the note
    // on acesFilmic.
    float gToe;
};

// The ACES filmic approximation (Narkowicz). Reinhard desaturates highlights
// toward white, which on a sunlit canopy turns every specular hit into a grey
// blob; this curve keeps the hue into the shoulder, so a bright needle stays
// green and a sun glint on water stays warm.
//
// THE TOE IS LIFTED, and that is the one departure from the published fit.
//
// Narkowicz's curve behaves like `c * b / e` near black, and with the original
// b = 0.03 that is a slope of 0.21: everything in shadow arrives at a FIFTH of
// its value before the display transfer function has even run. On a sunlit
// meadow nobody notices, because there is barely anything down there. In a
// conifer wood most of the frame is down there -- a grass blade's sides face
// sideways into its neighbours and see almost no sky -- and the whole floor
// came out as harsh near-black with the colour crushed out of it.
//
// b = 0.10 puts the slope at 0.71 and leaves the rest of the curve alone: the
// shoulder is set by the ratio of the QUADRATIC terms, which b is not one of,
// so highlights roll off exactly as before and a bright needle is still green.
// Measured over a frame of forest floor: the darkest fifth rises 43%, the
// brightest fifth 5%, and the part of the frame below 24/255 falls from 6.1%
// to 1.3%.
//
// THIS IS A TONE DECISION AND NOT A LIGHTING ONE, which is worth stating
// because the obvious suspect was the light transport. It is not: rendering
// the same frame with sixteen bounces instead of six gives bit-identical
// shadow radiance. The photons were always there; the curve was throwing them
// away on the way to the screen.
//
// Left on a slider because a toe is a matter of taste and this is the one
// setting whose effect you cannot judge from a number.
float acesFilmic(float c, float b)
{
    const float a = 2.51f, cc = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((c * (a * c + b)) / (c * (cc * c + d) + e));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    // The AO views bypass the tone curve entirely: this is a visibility term,
    // not radiance, and pushing it through an exposure and a filmic shoulder
    // would make a perfectly correct buffer look wrong.
    if (gShowAo != 0)
    {
        // THE TWO AO BUFFERS ARE IN DIFFERENT UNITS, which is easy to miss
        // because both are "the AO buffer". What the tracer writes is a hit
        // distance in METRES, bounded by the search radius. What NRD returns is
        // its own NORMALISED hit distance, already 0..1. Dividing the second by
        // the radius as well pins the whole frame near 0.5 -- a flat grey that
        // looks exactly like a denoiser that produced nothing.
        const float raw = gAoSrc[tid.xy];
        const float a = saturate(gShowAo >= 2 ? raw : raw / max(1e-3f, gAoRadius));
        gDst[tid.xy] = float4(a, a, a, 1.0f);
        return;
    }

    const float3 c = gSrc[tid.xy].rgb * gExposure;
    gDst[tid.xy] =
        float4(acesFilmic(c.r, gToe), acesFilmic(c.g, gToe), acesFilmic(c.b, gToe), 1.0f);
}
