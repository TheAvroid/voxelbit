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
RWTexture2D<float4> gDst;

cbuffer gTonemapCB
{
    uint2 gDim;
    float gExposure;
    // The toe of the curve below -- how much of a shadow survives it. 0.03 is
    // Narkowicz's published constant; this engine opens at 0.10. See the note
    // on acesFilmic.
    float gToe;
    // The deep-shadow lift: how much, and how far up the range it reaches.
    float gDeepLift;
    float gDeepRange;
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

// The deep-shadow lift -- brighten the BOTTOM of the range and nothing else.
//
// WHY THE TOE IS NOT THIS. gToe above is a parameter of the tone curve, so it
// acts everywhere the curve does: raising it to lift the underside of a pine
// also raises the forest floor, the far haze and the midtones of every trunk.
// Measured over one frame, taking the toe from 0.14 to 0.30 lifts the darkest
// quarter by 81% -- and the median by 39%, which is the whole frame going
// milky. That is a different picture, not a better-lit one.
//
// This acts on luminance and falls to EXACTLY ZERO by gDeepRange, so there is a
// value above which it provably does nothing. The falloff is cubic rather than
// linear: linear still has slope at the top of its range and smears a visible
// amount of lift into the midtones, whereas a cube is flat there and lands the
// weight where the pixels actually are.
//
// MULTIPLICATIVE, NOT ADDITIVE. The conventional lift/gamma/gain "lift" is an
// add, and an add is neutral grey -- it desaturates exactly the pixels being
// worked on. Shadows in a wood are blue, lit by the sky dome and nothing else,
// and greying them out is the one thing that would make them look wrong. A
// scale multiplies every channel alike and leaves the hue where it was; the
// floor on the divisor is what stops a near-black pixel from being multiplied
// by infinity.
float3 deepShadowLift(float3 ldr, float amount, float range)
{
    if (amount <= 0.0f)
        return ldr;
    const float l = dot(ldr, float3(0.2126f, 0.7152f, 0.0722f));
    const float t = saturate(l / max(range, 1e-3f));
    const float w = (1.0f - t) * (1.0f - t) * (1.0f - t);
    return saturate(ldr * (1.0f + amount * w / max(l, 0.03f)));
}

[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    const float3 c = gSrc[tid.xy].rgb * gExposure;
    const float3 ldr =
        float3(acesFilmic(c.r, gToe), acesFilmic(c.g, gToe), acesFilmic(c.b, gToe));
    gDst[tid.xy] = float4(deepShadowLift(ldr, gDeepLift, gDeepRange), 1.0f);
}
