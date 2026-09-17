// ---------------------------------------------------------------------------
// Exposure.cs.slang -- what the eye is currently stopped down to.
//
// WHY A FIXED EXPOSURE COULD NOT SURVIVE THE DAY/NIGHT CLOCK. gExposure is one
// number baked into defaults.h, and the cycle moves the light in the scene over
// something like four orders of magnitude between noon and a moonlit night. One
// constant cannot serve both ends: tuned for noon the night is black, tuned for
// the night the day is a white sheet. Every screenshot this engine has taken
// was tuned at one hour and is wrong at the others, and the settings menu's
// exposure slider exists largely to paper over exactly that.
//
// So: measure the frame, and move a stop toward it. Measured over four hours
// (06/09/13/19) the median luminance spans 57x with a fixed stop and 26x with
// this one; dawn comes up 4x and dusk 4.5x.
//
// ---------------------------------------------------------------------------
// A HISTOGRAM, NOT AN AVERAGE, AND THE REASON IS THE SUN.
//
// The obvious implementation is a log-average of luminance over the frame. In a
// conifer wood that fails badly, because the solar disk is present in a handful
// of pixels at a radiance five orders of magnitude above the forest floor, and
// a mean -- even a log mean -- is not robust to a tail like that. Turning
// towards the sun would stop the whole wood down to nothing.
//
// A histogram lets the tail be DISCARDED rather than weighted. The brightest
// couple of percent of pixels are dropped outright (the disk, its glare, a
// specular glint off water) and so is the darkest chunk (the inside of a
// canopy, which is genuinely black and should stay that way). What is left is
// the part of the frame a viewer is actually looking at, and that is what sets
// the stop.
//
// ---------------------------------------------------------------------------
// THE ADAPTATION IS ON LUMINANCE AND IT IS ASYMMETRIC.
//
// Smoothing the exposure MULTIPLIER rather than the measured luminance gives a
// non-linear response to a linear change in light, which reads as the image
// lurching. The state below is the adapted luminance; the exposure is derived
// from it every frame.
//
// Up and down have separate rates because eyes do: walking out of a wood into
// a clearing is over in a fraction of a second, and walking back in takes long
// enough to notice. Equal rates make one of the two feel wrong, and it is
// always the bright one.
// ---------------------------------------------------------------------------

Texture2D<float4> gSrc;
RWStructuredBuffer<uint> gHistogram;

// [0] the adapted luminance, which persists between frames and IS the state.
// [1] the exposure multiplier the tone map and the bloom prefilter read.
RWStructuredBuffer<float> gExposureState;

cbuffer gExpCB
{
    uint2 gDim;

    // The window the histogram spans, in log2 luminance. Anything below the
    // floor lands in bin 0 and is excluded from the mean rather than clamped
    // into it -- a pixel darker than the range is not evidence about the
    // range, and letting it vote drags the whole measurement down whenever a
    // large black object crosses the frame.
    float gMinLog;
    float gLogRange;

    float gDt;        // seconds of wall clock since the last measurement
    float gSpeedUp;   // rate toward a BRIGHTER scene, per second
    float gSpeedDown; // and toward a darker one
    float gKey;       // the luminance the middle of the frame is aimed at

    float gMinExposure;
    float gMaxExposure;

    // Fractions of the frame thrown away at each end before the mean. See the
    // note on the sun above.
    float gLowCut;
    float gHighCut;

    // 1 on the first frame after this pass starts running, or after a jump the
    // history cannot describe: take the measurement as the answer instead of
    // easing toward it from whatever the state happens to hold. Without it the
    // first second of every run is a fade up from black, which also lands in
    // the first frame of every offline render -- where there IS no second
    // frame to recover in.
    uint gReset;
    float gExpPad0;
    float gExpPad1;
};

static const uint kBins = 256u;

float luminance(float3 c) { return dot(c, float3(0.2126f, 0.7152f, 0.0722f)); }

// ---------------------------------------------------------------------------
// Pass one: count the frame into 256 log-spaced bins.
// ---------------------------------------------------------------------------
groupshared uint gLocalBins[kBins];

[numthreads(16, 16, 1)]
void histogram(uint3 tid: SV_DispatchThreadID, uint gi: SV_GroupIndex)
{
    gLocalBins[gi] = 0u;
    GroupMemoryBarrierWithGroupSync();

    if (tid.x < gDim.x && tid.y < gDim.y)
    {
        const float l = luminance(gSrc[tid.xy].rgb);
        // Bin 0 is the underflow bucket, and everything about the resolve pass
        // treats it as "no information" rather than "very dark".
        uint bin = 0u;
        if (l > 1e-6f)
        {
            const float t = saturate((log2(l) - gMinLog) / gLogRange);
            bin = min(kBins - 1u, uint(t * float(kBins - 2u)) + 1u);
        }
        InterlockedAdd(gLocalBins[bin], 1u);
    }

    GroupMemoryBarrierWithGroupSync();
    // One global atomic per bin per group rather than one per pixel: at 4K that
    // is the difference between 8 million contended atomics and 122 thousand.
    if (gLocalBins[gi] > 0u)
        InterlockedAdd(gHistogram[gi], gLocalBins[gi]);
}

// ---------------------------------------------------------------------------
// Pass two: one group of 256 threads turns the histogram into a stop.
//
// It also ZEROES the histogram on the way out, which is why there is no
// separate clear dispatch: this pass is the only reader, it runs exactly once
// per frame, and a clear that lives anywhere else is a clear that can be
// forgotten on some path through the frame.
// ---------------------------------------------------------------------------
groupshared uint gCount[kBins];
groupshared float gWeighted[kBins];
groupshared float gPrefix[kBins];

[numthreads(256, 1, 1)]
void resolve(uint3 tid: SV_DispatchThreadID)
{
    const uint i = tid.x;
    const uint c = gHistogram[i];
    gCount[i] = c;
    gHistogram[i] = 0u;
    GroupMemoryBarrierWithGroupSync();

    // An inclusive scan over the counts, so each bin knows how much of the
    // frame is darker than it is and the percentile cuts become a clamp rather
    // than a search. Hillis-Steele: eight steps for 256 bins.
    gPrefix[i] = float(gCount[i]);
    GroupMemoryBarrierWithGroupSync();
    for (uint off = 1u; off < kBins; off <<= 1u)
    {
        const float add = (i >= off) ? gPrefix[i - off] : 0.0f;
        GroupMemoryBarrierWithGroupSync();
        gPrefix[i] += add;
        GroupMemoryBarrierWithGroupSync();
    }

    // Bin 0 is underflow: excluded from the total, so the cuts are percentiles
    // of the pixels that carry light rather than of the whole frame.
    const float total = gPrefix[kBins - 1u] - float(gCount[0]);
    const float base = float(gCount[0]);
    const float lo = base + total * gLowCut;
    const float hi = base + total * (1.0f - gHighCut);

    // How much of THIS bin falls inside the kept band. Overlap of two intervals
    // -- no branching, and a bin straddling a cut contributes its fraction
    // rather than all or nothing, which is what keeps the measurement from
    // stepping as the frame changes.
    const float upper = clamp(gPrefix[i], lo, hi);
    const float lower = clamp(gPrefix[i] - float(gCount[i]), lo, hi);
    const float w = (i == 0u) ? 0.0f : max(0.0f, upper - lower);

    // The bin's log luminance, taken at its centre.
    const float t = (float(i) - 1.0f + 0.5f) / float(kBins - 2u);
    gWeighted[i] = w * (gMinLog + t * gLogRange);
    gPrefix[i] = w;
    // NOTHING WRITES gCount FROM HERE ON. `base` above is a cross-thread read
    // of gCount[0], and there is no barrier between it and this point -- so a
    // write here, however convenient a scratch array it looks like, would be a
    // race against every thread that had not reached `base` yet.
    GroupMemoryBarrierWithGroupSync();

    // Sum both arrays down to thread 0.
    for (uint s = kBins >> 1u; s > 0u; s >>= 1u)
    {
        if (i < s)
        {
            gWeighted[i] += gWeighted[i + s];
            gPrefix[i] += gPrefix[i + s];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (i != 0u)
        return;

    const float wsum = gPrefix[0];
    // A frame with nothing in the band -- the very first frame, or a fade to
    // black -- must not move the state. Holding is the right answer: there is
    // no measurement, and inventing one would produce exactly the lurch this
    // whole pass exists to avoid.
    if (wsum <= 0.0f)
    {
        if (gReset != 0u)
        {
            gExposureState[0] = gKey;
            gExposureState[1] = clamp(1.0f, gMinExposure, gMaxExposure);
        }
        return;
    }

    const float measured = exp2(gWeighted[0] / wsum);

    const float prev = gExposureState[0];
    float adapted;
    if (gReset != 0u || prev <= 0.0f)
    {
        adapted = measured;
    }
    else
    {
        // Exponential approach with a per-second rate, so the speed is a
        // property of the setting and not of the frame rate. exp() rather than
        // a plain lerp for the same reason: a lerp by dt*speed overshoots and
        // then rings once dt*speed passes 1, which is precisely what happens on
        // a hitch.
        const float rate = (measured > prev) ? gSpeedUp : gSpeedDown;
        adapted = measured + (prev - measured) * exp(-max(gDt, 0.0f) * max(rate, 1e-3f));
    }

    gExposureState[0] = adapted;
    gExposureState[1] = clamp(gKey / max(adapted, 1e-5f), gMinExposure, gMaxExposure);
}
