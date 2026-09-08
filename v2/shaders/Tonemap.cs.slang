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

// The guide depth, for the flare occlusion test only. 1.0 where the primary ray
// escaped, so it answers "is something solid in front of the sun" and nothing
// else. Sampled rather than loaded because it is at RENDER resolution while
// this pass runs at DISPLAY resolution.
Texture2D<float> gFlareDepth;
SamplerState gFlareSamp;

// -- what post.h computed, a moment before this dispatch ---------------------
//
// The bloom chain's level 0, at half this pass's resolution, already exposure
// scaled and already thresholded. Sampled with the flare's own sampler: it is
// linear and clamped, which is exactly what a half-resolution lookup wants, and
// a second identical sampler would only be another thing to bind.
//
// BOUND EVEN WHEN THERE IS NO BLOOM -- at a 1x1 placeholder -- because a
// declared resource must be bound whether or not the branch that reads it runs.
Texture2D<float4> gBloom;

// [1] is the auto-exposure multiplier. Read only when gUseAuto says the
// measuring pass actually ran this frame; stale otherwise, by design.
StructuredBuffer<float> gExposureState;

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

    // -- the sun flare. gFlare is the master strength; 0 disables the block --
    //
    // The camera basis comes in rather than a matrix because the flare needs
    // exactly three dot products to place the sun on screen, and gTanH with
    // gAspect is what turns those into NDC. Same tangent projection the tracer
    // builds its rays with, so the flare lands where the disc is drawn.
    float3 gSunDir;
    float gFlare;
    float3 gCamRight;
    float gTanH;
    float3 gCamUp;
    float gAspect;
    float3 gCamFwd;
    float gSunCosR;

    // -- AND THE SAME FOR THE MOON, at its own strength ----------------------
    //
    // "a slight glare like the sun. only slightly though." The flare below is
    // not really about the sun -- it is about a very bright small disc seen
    // through a lens -- so the moon wants the same function and a much smaller
    // number, not a second effect. gMoonFlare is that number and 0 disables it.
    float3 gMoonDir;
    float gMoonFlare;

    // The vignette. 0 is off, which is the default -- it is a look, not a fix.
    float gVignette;

    // The bloom's master strength; 0 skips the lookup entirely. Took two of the
    // three padding words this buffer was already carrying, so its size is
    // unchanged.
    float gBloomStrength;
    uint gUseAuto;
    // Took the last padding word this buffer was carrying, so its size is
    // unchanged -- the moon's own angular radius, which is not the sun's.
    float gMoonCosR;
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


// ---------------------------------------------------------------------------
// THE SUN'S GLARE AND ITS LENS FLARE, ported from the WebGPU game's blit.js.
//
// A LENS FLARE IS MADE AT THE LENS, NOT AT THE SUBJECT, which is why it is
// added to the tone-mapped image here rather than traced. Scattering it back
// through the tone curve would read as a smudge on the scene instead of an
// artefact of the glass.
//
// ---------------------------------------------------------------------------
// THE GLARE AND THE FLARE ARE THE SAME FOUR DISCS.
//
// That is the whole trick, and it is not obvious. The ghosts sit along the axis
// from the sun to the screen centre, at sunPix + axis * (1 + K). When the sun
// IS at the centre, axis collapses to zero and all four pile onto the sun --
// and that pile is what a glare looks like. So the same four discs, the same
// radii and the same tints are ALSO drawn centred on the sun at every angle,
// which makes the look you get staring straight at it available from any angle
// rather than an imitation of it.
//
// They are additive, deliberately: the axis train is a real lens artefact and
// is worth keeping when the sun is off to one side.
//
// EACH GHOST FADES AS IT LANDS ON THE SUN. Without that, a centred sun gets the
// four ghosts on top of the four sun-centred discs -- eight against four --
// which reads as the glare dying as you turn away rather than doubling as you
// turn toward.
// ---------------------------------------------------------------------------

// smoothstep with the edges reversed: 1 inside `lo`, 0 outside `hi`. HLSL does
// not define smoothstep for e0 > e1, which the source relied on.
float sstepDown(float hi, float lo, float x) { return 1.0f - smoothstep(lo, hi, x); }

// A BODY, NOT THE SUN. Everything below was written against gSunDir and gFlare
// directly, and none of it is actually about the sun: it is about a small very
// bright disc seen through a lens. Passing the body in lets the moon have the
// same glare at its own strength instead of growing a second copy of this that
// would drift out of step with it.
float3 bodyFlare(float2 fc, float2 dim, float3 bodyDir, float bodyCosR, float strength)
{
    if (strength <= 0.0f) return float3(0.0f);

    // Behind the camera, or below the horizon: nothing.
    const float sf = dot(bodyDir, gCamFwd);
    const float dayK = smoothstep(-0.03f, 0.05f, bodyDir.y);
    if (sf <= 0.05f || dayK <= 0.0f) return float3(0.0f);

    const float2 sunNDC = float2(dot(bodyDir, gCamRight) / (sf * gTanH * gAspect),
                                 dot(bodyDir, gCamUp) / (sf * gTanH));
    const float2 sunPix = float2((sunNDC.x * 0.5f + 0.5f) * dim.x,
                                 (0.5f - sunNDC.y * 0.5f) * dim.y);

    // THE FLARE IS SIZED OFF THE DISC, and it GROWS OFF-AXIS because the disc
    // does. A tangent projection magnifies away from centre -- radially by
    // 1/sf^2 and tangentially by 1/sf -- and the sky draws the disc by ANGLE so
    // it grows automatically. Pinning the flare to the centre-of-screen size
    // instead lets the disc outgrow its own glare past about 15 degrees off
    // axis, leaving a bare hard circle. 1.5 is the exponent between the two
    // magnifications. The max() guards 1/sf^1.5 from blowing up at the gate.
    const float sunPx = tan(acos(bodyCosR)) / gTanH * 0.5f * dim.y / pow(max(sf, 0.2f), 1.5f);
    const float2 ctr = dim * 0.5f;
    const float2 axis = ctr - sunPix;

    const float K[4] = {0.35f, 0.65f, 1.15f, 1.7f};
    const float3 TINT[4] = {float3(1.00f, 0.85f, 0.60f), float3(0.60f, 1.00f, 0.75f),
                            float3(0.65f, 0.75f, 1.00f), float3(1.00f, 0.70f, 0.85f)};

    float ghw[4];
    float sgw[4];
    float gAny = 0.0f;
    const float dSun = length(fc - sunPix);
    for (int i = 0; i < 4; ++i)
    {
        const float2 gp = sunPix + axis * (1.0f + K[i]);
        const float rr = sunPx * (0.65f + 1.15f * K[i]);
        const float sep = length(gp - sunPix) / max(rr, 1.0f);
        ghw[i] = sstepDown(rr, rr * 0.22f, length(fc - gp)) * smoothstep(0.6f, 1.6f, sep);
        sgw[i] = sstepDown(rr, rr * 0.22f, dSun);
        gAny += ghw[i] + sgw[i];
    }
    if (gAny <= 0.0f) return float3(0.0f);

    // ── IS SOMETHING SOLID IN FRONT OF THE SUN, and nothing else ────────────
    //
    // The guide depth is 1.0 where the primary ray escaped, so this asks the
    // one question an occlusion test should. It deliberately does NOT ask
    // whether the sun is still BRIGHT there: cloud is sky, so the sun keeps
    // behind a cloud the same halo it has in the open, while a trunk or a
    // ridge hides it because those clear the sky mask.
    //
    // ── THE PROBE IS THE DISC, NOT A FIXED LITTLE BOX ───────────────────────
    //
    // This used to be nine taps on a 6 px grid: an 18 px probe of a disc that
    // is 44 px across at the centre of the screen and half as much again off
    // axis. So it asked about a patch near the MIDDLE of the sun and called the
    // answer the whole sun -- and through a pine canopy that patch lands in the
    // gaps between needles often enough to report the sun mostly clear. The
    // flare then drew a bright soft blob straight over the tree, which is what
    // "I can see the sun through the tree" was.
    //
    // Rings scaled to sunPx instead, so the probe is the sun whatever the field
    // of view is and wherever on screen it has got to. Taps that fall outside
    // the frame are not counted rather than clamped to the edge: an edge texel
    // is not evidence about a part of the sky nobody rendered, and clamping let
    // one bright border pixel vote nine times.
    float sv = 0.0f;
    float wsum = 0.0f;
    {
        // The middle of the disc counts for more than its rim, which is what
        // keeps a branch clipping the edge from behaving like a trunk.
        const float ringR[3] = {0.0f, 0.55f, 0.95f};
        const float ringW[3] = {2.0f, 1.0f, 0.55f};
        for (int ring = 0; ring < 3; ++ring)
        {
            const int taps = (ring == 0) ? 1 : 8;
            for (int t = 0; t < taps; ++t)
            {
                float2 o = float2(0.0f, 0.0f);
                if (ring > 0)
                {
                    // Half a step of rotation per ring so the two rings do not
                    // line up into eight spokes that can all sit in one gap.
                    const float ang = (float(t) + 0.5f * float(ring)) * (6.2831853f / 8.0f);
                    o = float2(cos(ang), sin(ang)) * (ringR[ring] * sunPx);
                }
                const float2 uv = (sunPix + o) / dim;
                if (ring > 0 && (any(uv < 0.0f) || any(uv > 1.0f))) continue;
                const float d = gFlareDepth.SampleLevel(gFlareSamp, saturate(uv), 0.0f);
                sv += ringW[ring] * (d > 0.999f ? 1.0f : 0.0f);
                wsum += ringW[ring];
            }
        }
    }
    sv = wsum > 0.0f ? sv / wsum : 0.0f;

    // ── AND A DISC BROKEN UP BY FOLIAGE IS NOT A DIMMER SUN ─────────────────
    //
    // Scaling the flare linearly by the visible fraction is the wrong curve for
    // what a lens does. The ghosts come from the disc's light bouncing between
    // glass elements, and a disc chopped into slivers by needles throws far
    // less of that around than the fraction of it that survives -- the light is
    // there, but it no longer forms the coherent bright source the artefact
    // needs. Linearly, a sun 40 % hidden still drew 60 % of a full flare, and
    // over a canopy 60 % of a full flare is precisely the blob.
    //
    // Full strength in clear sky, gone by the time the sun is nominally half
    // visible. A thin bare branch across the sun killing the flare is not an
    // overshoot: that is what it does through a real lens too.
    sv = smoothstep(0.55f, 0.98f, sv);
    sv *= sv;
    if (sv <= 0.002f) return float3(0.0f);

    // TWO WEIGHTS, not one: with the doubling gone the sun-centred stack has to
    // carry alone what eight discs used to, so it takes the larger share.
    float3 add = float3(0.0f);
    for (int i = 0; i < 4; ++i)
        add += TINT[i] * (ghw[i] * 0.16f + sgw[i] * 0.24f);
    return add * sv * dayK * strength;
}

// The two of them. Exactly one can ever contribute -- they sit at opposite
// points of the sky, so whichever is up the other is below the horizon and its
// own dayK gate returns black.
float3 sunFlare(float2 fc, float2 dim)
{
    return bodyFlare(fc, dim, gSunDir, gSunCosR, gFlare) +
           bodyFlare(fc, dim, gMoonDir, gMoonCosR, gMoonFlare);
}
[numthreads(8, 8, 1)]
void main(uint3 tid: SV_DispatchThreadID)
{
    if (tid.x >= gDim.x || tid.y >= gDim.y)
        return;
    // -- THE STOP, and where the auto one multiplies rather than replaces ----
    //
    // gExposure stays the setting it always was and the measured value scales
    // it. Replacing it would make the control inert the moment adaptation came
    // on, and the thing people reach for first is precisely "the same, but a
    // bit brighter".
    const float exposure = gExposure * (gUseAuto != 0u ? gExposureState[1] : 1.0f);

    float3 c = gSrc[tid.xy].rgb * exposure;

    // -- BLOOM, BEFORE THE CURVE AND NOT AFTER ------------------------------
    //
    // It is light that reached the sensor, so it has to be tone mapped with the
    // rest of the light. Adding it to the LDR image afterwards -- which is what
    // the lens flare below does, correctly, because a flare is an artefact of
    // the glass rather than a measurement -- would let it blow past white and
    // clip flat instead of rolling into the shoulder.
    if (gBloomStrength > 0.0f)
    {
        const float2 buv = (float2(tid.xy) + 0.5f) / float2(gDim);
        c += gBloom.SampleLevel(gFlareSamp, buv, 0.0f).rgb * gBloomStrength;
    }

    const float3 ldr =
        float3(acesFilmic(c.r, gToe), acesFilmic(c.g, gToe), acesFilmic(c.b, gToe));
    float3 outc = deepShadowLift(ldr, gDeepLift, gDeepRange);
    // LAST, onto the finished image -- see the note above sunFlare.
    outc += sunFlare(float2(tid.xy) + 0.5f, float2(gDim));

    // ── CINEMATIC VIGNETTE, LAST ───────────────────────────────────────────
    //
    // After the flare on purpose, so it darkens the finished image -- ghosts
    // included -- rather than having them scatter back out over the top of it.
    //
    // ASPECT-CORRECTED. Measuring the falloff on raw UV makes an ellipse
    // stretched to the window, which reads as letterboxing rather than as a
    // vignette; correcting the shorter axis keeps it a circle at any shape of
    // window.
    if (gVignette > 0.001f)
    {
        const float2 uv = (float2(tid.xy) + 0.5f) / float2(gDim);
        const float2 q = (uv - 0.5f) * float2(max(1.0f, gAspect), max(1.0f, 1.0f / gAspect));
        outc *= max(1.0f - gVignette * smoothstep(0.18f, 0.78f, length(q)), 0.0f);
    }
    gDst[tid.xy] = float4(outc, 1.0f);
}
