// ---------------------------------------------------------------------------
// Bloom.cs.slang -- the light that does not stay in its pixel.
//
// WHY A PATH TRACER STILL NEEDS THIS. Bloom is not a lighting effect and this
// pass is not making up transport the tracer failed to find. It is a LENS
// effect: scattering in the cornea, in the glass, off the sensor's own
// microlenses. A perfect renderer feeding a real camera would still produce it,
// and the reason a rendered frame without it reads as synthetic is that every
// photograph and every eye has it.
//
// It also does something specific for this scene. The engine already has a sun
// flare -- four discs, drawn analytically, keyed off the solar direction -- and
// that handles the sun ITSELF. What it cannot handle is the sun broken into a
// hundred fragments by a pine canopy, because there is no single direction to
// hang a disc on. Those fragments are exactly what bloom is good at, and they
// are most of what makes a wood look sunlit rather than merely lit.
//
// ---------------------------------------------------------------------------
// THE FILTER: Jimenez's progressive down/up chain (SIGGRAPH 2014), not a
// Gaussian blur.
//
// A single wide Gaussian at full resolution is both far more expensive and
// worse: its kernel is finite, so it has a visible edge, and the taps needed to
// cover a hundred pixels of radius are unaffordable. The chain instead
// downsamples six times with a 13-tap filter and then upsamples back with a
// 3x3 tent, adding at every level. The result is a sum of scales -- a tight
// core with a very wide, very smooth skirt -- which is what a real lens does,
// and it costs about a third of a full-resolution pass in total.
//
// KARIS AVERAGING ON THE FIRST DOWNSAMPLE ONLY. A path-traced frame at one
// sample per pixel has fireflies, and a firefly inside a bloom chain becomes a
// firefly the size of the screen that flickers. Weighting the first
// downsample's tap groups by 1/(1+luma) is Karis's fix: it makes that first
// reduction a partial median rather than a mean, so a single 10^4 pixel cannot
// dominate its neighbourhood. It is applied only at the first step because
// after that the fireflies are already averaged away and the weighting would
// just be a slight, pointless darkening of every highlight.
// ---------------------------------------------------------------------------

Texture2D<float4> gIn;
SamplerState gSamp;
RWTexture2D<float4> gOut;

// Read only, and only when gUseAuto says the auto-exposure pass ran this frame.
// See Exposure.cs.slang.
StructuredBuffer<float> gExposureState;

cbuffer gBloomCB
{
    // The DESTINATION extent, and the source's texel size. The two are not
    // simply related by a factor of two at every step: an odd-sized mip rounds
    // down, so deriving one from the other would drift by half a texel per
    // level and the chain would creep diagonally as it went.
    uint2 gOutDim;
    float2 gInTexel;

    // Applied at prefilter time so the threshold below means the same thing at
    // every hour of the day. Thresholding the raw HDR instead would make the
    // bloom appear and disappear as the sun moved, since the numbers coming out
    // of the tracer are absolute and the tone curve is what puts them on a
    // screen.
    float gExposure;
    uint gUseAuto;

    // The soft knee. Below gThreshold nothing blooms; above it everything does;
    // between them the response is quadratic rather than a step, which is what
    // keeps a surface drifting slowly past the threshold from popping.
    float gThreshold;
    float gKnee;

    // The tent's reach on the upsample, in source texels. Bigger is softer and
    // wider; past about 2 the levels stop overlapping cleanly and the skirt
    // starts to show rings.
    float gUpRadius;
    float gBloomPad0;
    float gBloomPad1;
    float gBloomPad2;
};

float bloomLuma(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

float3 tap(float2 uv) { return gIn.SampleLevel(gSamp, uv, 0.0f).rgb; }

// The 13-tap downsample. `karis` weights the five tap groups by 1/(1+luma) --
// see the note above.
float3 downsample13(float2 uv, float2 d, bool karis)
{
    const float3 a = tap(uv + d * float2(-2.0f, 2.0f));
    const float3 b = tap(uv + d * float2(0.0f, 2.0f));
    const float3 c = tap(uv + d * float2(2.0f, 2.0f));
    const float3 e = tap(uv + d * float2(-2.0f, 0.0f));
    const float3 f = tap(uv);
    const float3 g = tap(uv + d * float2(2.0f, 0.0f));
    const float3 h = tap(uv + d * float2(-2.0f, -2.0f));
    const float3 i = tap(uv + d * float2(0.0f, -2.0f));
    const float3 j = tap(uv + d * float2(2.0f, -2.0f));
    const float3 k = tap(uv + d * float2(-1.0f, 1.0f));
    const float3 l = tap(uv + d * float2(1.0f, 1.0f));
    const float3 m = tap(uv + d * float2(-1.0f, -1.0f));
    const float3 n = tap(uv + d * float2(1.0f, -1.0f));

    float3 g0 = (a + b + e + f) * 0.25f;
    float3 g1 = (b + c + f + g) * 0.25f;
    float3 g2 = (e + f + h + i) * 0.25f;
    float3 g3 = (f + g + i + j) * 0.25f;
    float3 g4 = (k + l + m + n) * 0.25f;

    if (karis)
    {
        const float w0 = 1.0f / (1.0f + bloomLuma(g0));
        const float w1 = 1.0f / (1.0f + bloomLuma(g1));
        const float w2 = 1.0f / (1.0f + bloomLuma(g2));
        const float w3 = 1.0f / (1.0f + bloomLuma(g3));
        const float w4 = 1.0f / (1.0f + bloomLuma(g4));
        // The centre group carries half the weight in the unweighted filter, so
        // it carries four times each corner's here too -- the Karis weights
        // modulate the filter, they do not replace it.
        const float wsum = 0.125f * (w0 + w1 + w2 + w3) + 0.5f * w4;
        return (g0 * (0.125f * w0) + g1 * (0.125f * w1) + g2 * (0.125f * w2) +
                g3 * (0.125f * w3) + g4 * (0.5f * w4)) / max(wsum, 1e-6f);
    }
    return (g0 + g1 + g2 + g3) * 0.125f + g4 * 0.5f;
}

// The soft-knee threshold, in the max-channel rather than in luminance: a
// saturated red highlight is as much a highlight as a white one, and measuring
// it by luminance would put it below the threshold on the strength of its hue.
float3 knee(float3 c)
{
    const float br = max(c.r, max(c.g, c.b));
    const float k = max(gKnee, 1e-4f);
    float soft = clamp(br - gThreshold + k, 0.0f, 2.0f * k);
    soft = soft * soft / (4.0f * k);
    return c * (max(soft, br - gThreshold) / max(br, 1e-4f));
}

// The exposure this frame is actually being shown at.
float currentExposure()
{
    return gUseAuto != 0u ? gExposure * gExposureState[1] : gExposure;
}

// ---------------------------------------------------------------------------
// Prefilter: full-resolution HDR to the first half-resolution bloom level, with
// the exposure applied and the threshold taken.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void prefilter(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gOutDim.x || tid.y >= gOutDim.y)
        return;
    const float2 uv = (float2(tid.xy) + 0.5f) / float2(gOutDim);
    float3 c = downsample13(uv, gInTexel, true) * currentExposure();
    // NaNs and infinities: a path tracer produces them occasionally and a bloom
    // chain is the one place they become a whole-screen artefact rather than a
    // single bad pixel. One max() at the entrance is the cheapest possible
    // containment and it costs nothing on a clean frame.
    c = min(max(c, 0.0f), 64.0f);
    gOut[tid.xy] = float4(knee(c), 1.0f);
}

[numthreads(8, 8, 1)]
void down(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gOutDim.x || tid.y >= gOutDim.y)
        return;
    const float2 uv = (float2(tid.xy) + 0.5f) / float2(gOutDim);
    gOut[tid.xy] = float4(downsample13(uv, gInTexel, false), 1.0f);
}

// ---------------------------------------------------------------------------
// Upsample: ADDS the smaller level into the larger one, which is what makes the
// finished chain a sum of scales rather than only its widest one.
// ---------------------------------------------------------------------------
[numthreads(8, 8, 1)]
void up(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gOutDim.x || tid.y >= gOutDim.y)
        return;
    const float2 uv = (float2(tid.xy) + 0.5f) / float2(gOutDim);
    const float2 d = gInTexel * gUpRadius;

    const float3 t =
        (tap(uv + d * float2(-1.0f, 1.0f)) + tap(uv + d * float2(0.0f, 1.0f)) * 2.0f +
         tap(uv + d * float2(1.0f, 1.0f)) + tap(uv + d * float2(-1.0f, 0.0f)) * 2.0f +
         tap(uv) * 4.0f + tap(uv + d * float2(1.0f, 0.0f)) * 2.0f +
         tap(uv + d * float2(-1.0f, -1.0f)) + tap(uv + d * float2(0.0f, -1.0f)) * 2.0f +
         tap(uv + d * float2(1.0f, -1.0f))) *
        (1.0f / 16.0f);

    gOut[tid.xy] = float4(gOut[tid.xy].rgb + t, 1.0f);
}
