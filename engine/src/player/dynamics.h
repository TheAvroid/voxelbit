// ---------------------------------------------------------------------------
// dynamics.h -- pull the loud parts of a bed down, and leave the quiet alone.
//
// WHAT THIS IS FOR, measured on the file it was written for. bird_ambience.mp3
// is a five-minute wood, and its level distribution over 50 ms windows is:
//
//     median  -35.3 dBFS      p90  -18.1        loudness range 12.4 LU
//     p75     -25.5           p95  -15.7        true peak      -3.0 dBFS
//     p85     -20.4           max  -10.9
//
// The bed is the median and the birds are the top decile, about twenty dB
// above it. Turning the whole thing down -- which is all the Ambience volume
// can do -- takes the wood away with the birds. What is wanted is the top of
// that distribution moved and the bottom left exactly where it is, which is
// downward compression AND NO MAKEUP GAIN. Makeup is what a mastering
// compressor adds to win back the level it just removed; adding it here would
// push the quiet bed up and undo the entire point.
//
// So: above the threshold the samples are pulled toward it, and below it they
// are left alone. BUT "LEFT ALONE" IS A CLAIM ABOUT LEVEL, NOT ABOUT BITS, and
// the difference is worth stating because it is easy to write the stronger
// version by accident. The gain is a smoothed envelope, so a quiet moment
// sitting in the decay of a loud one still carries a little of its reduction.
//
// Measured over the whole file, of the 3021 blocks below -35 dBFS:
//
//     24 (0.79%) move by more than 0.5 dB
//     22 of those 24 are within half a second of a block above -20 dBFS
//      2 move with no loud neighbour at all, the worst of them by 0.6 dB
//
// That is what releaseQuietMs below is for, and it is the difference between
// this and a compressor that quietly ducks the bed behind every bird.
//
// Nothing is ever made louder, which is also why this cannot clip -- the gain
// is <= 1 by construction.
//
// ---------------------------------------------------------------------------
// TWO PASSES, BECAUSE THE BED LOOPS
//
// The ambience is submitted to XAudio2 once with XAUDIO2_LOOP_INFINITE, so the
// last sample is followed by the first, forever. A compressor run straight
// through from sample zero starts with an empty detector: the opening seconds
// get no reduction they should have had, and worse, the gain the envelope has
// arrived at by the end does not match the gain it starts with -- a step at
// the seam, in a loop that render/audio.h says was baked precisely so there
// would be nothing to hear there.
//
// The fix is to run the whole buffer once and THROW THE OUTPUT AWAY, keeping
// only the detector and gain state, then run it again for real starting from
// that state. Pass two therefore begins with exactly the state pass one ended
// with, which is the state the seam actually has. It costs one extra read of
// a buffer that is already in RAM.
//
// ---------------------------------------------------------------------------
// WHY THE DETECTOR IS POWER AND NOT PEAK
//
// A peak detector keys off the sharpest edge in a chirp, so it reduces on
// transients whose energy the ear barely registers and the bed audibly ducks
// under a single click. Mean power across both channels is what the numbers
// above were measured with (it is ffmpeg's RMS convention), so the thresholds
// here mean what that table says they mean.
//
// The two channels share one gain. Compressing them independently would move
// the stereo image every time a bird lands in one of them.
// ---------------------------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

namespace vb {

// Everything about the curve, in the units the numbers above are quoted in.
struct Tame {
    float thresholdDb = -24.0f;  // level above which anything happens at all
    float ratio = 3.0f;          // 3:1 -- 12 dB over the threshold comes out 4
    float kneeDb = 8.0f;         // width of the soft knee, centred on threshold
    float attackMs = 15.0f;      // onto the gain reduction, not the detector
    // RELEASE IS THE KNOB THAT PROTECTS THE QUIET, and it is short for that
    // reason rather than for any classic-compressor reason. A bird call is a
    // transient over a bed twenty dB below it, so a long release goes on
    // holding the gain down after the call has finished and ducks the bed that
    // follows it -- which is the one thing this is not allowed to do. Measured
    // on bird_ambience.mp3, at 250 ms the median block came down 1.4 dB; at
    // 40 ms it comes down 0.3, while the peaks move just as far.
    float releaseMs = 40.0f;
    // AND THE RELEASE ONCE THE SIGNAL IS ACTUALLY QUIET AGAIN, which is a
    // separate number because it is doing a separate job. Above, release is
    // shaping how a loud passage recovers and wants to be smooth. Here the
    // curve is already asking for no reduction at all and the only thing still
    // holding gain down is the filter -- so the fastest recovery that is not
    // an audible step is the right one, and it is what keeps the bed after a
    // call as loud as the bed before it.
    float releaseQuietMs = 8.0f;
    float detectorMs = 15.0f;    // the power-averaging window
};

// Interleaved stereo int16 at `rate`, processed in place. A buffer that is not
// a whole number of frames long has its odd trailing sample left alone.
inline void tameLoudPeaks(std::vector<int16_t> *pcm, const Tame &t, int rate = 48000) {
    if (!pcm || pcm->size() < 2 || rate <= 0) return;

    const size_t frames = pcm->size() / 2;
    const float sr = float(rate);
    // A one-pole per millisecond figure. exp(-1/(tau*sr)) is the pole; the
    // coefficient below is 1 - pole, i.e. how far toward the input one sample
    // travels. A zero or negative time is allowed and means "instant".
    auto pole = [sr](float ms) {
        if (!(ms > 0.0f)) return 1.0f;
        return 1.0f - std::exp(-1.0f / ((ms * 0.001f) * sr));
    };
    const float aDet = pole(t.detectorMs);
    const float aAtk = pole(t.attackMs);
    const float aRel = pole(t.releaseMs);
    const float aRelQuiet = pole(t.releaseQuietMs);

    // 1 - 1/ratio is the fraction of every dB over the threshold that gets
    // removed. Ratio <= 1 would be an expander and is not what this is for.
    const float slope = (t.ratio > 1.0f) ? (1.0f - 1.0f / t.ratio) : 0.0f;
    const float knee = (t.kneeDb > 0.0f) ? t.kneeDb : 0.0f;
    if (slope <= 0.0f) return;

    float power = 0.0f;  // smoothed mean square, 0..1
    float gr = 0.0f;     // gain reduction currently applied, in dB, >= 0

    // Pass 0 primes the state at the loop seam; pass 1 writes. See the header.
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < frames; ++i) {
            const float l = float((*pcm)[i * 2]) * (1.0f / 32768.0f);
            const float r = float((*pcm)[i * 2 + 1]) * (1.0f / 32768.0f);
            power += (0.5f * (l * l + r * r) - power) * aDet;

            // 10*log10 of power is the same dB scale as 20*log10 of amplitude.
            const float lvl = 10.0f * std::log10(power > 1e-12f ? power : 1e-12f);
            const float over = lvl - t.thresholdDb;

            float target;  // dB of reduction the static curve asks for
            if (over <= -0.5f * knee) {
                target = 0.0f;
            } else if (over >= 0.5f * knee) {
                target = slope * over;
            } else {
                // Quadratic through the knee: continuous in value AND slope at
                // both ends, so there is no audible corner where it engages.
                const float k = over + 0.5f * knee;
                target = slope * k * k / (2.0f * knee);
            }

            // Three rates, not two: attacking, releasing while still over the
            // threshold, and releasing once the curve wants nothing at all.
            const float coeff = (target > gr) ? aAtk : (target > 0.0f ? aRel : aRelQuiet);
            gr += (target - gr) * coeff;

            if (pass == 0) continue;

            const float g = std::pow(10.0f, -gr * (1.0f / 20.0f));
            for (int c = 0; c < 2; ++c) {
                const float v = float((*pcm)[i * 2 + size_t(c)]) * g;
                const float rounded = v < 0.0f ? v - 0.5f : v + 0.5f;
                const int q = int(rounded);
                (*pcm)[i * 2 + size_t(c)] =
                    int16_t(q < -32768 ? -32768 : (q > 32767 ? 32767 : q));
            }
        }
    }
}

}  // namespace vb
