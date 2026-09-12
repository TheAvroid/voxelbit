// ---------------------------------------------------------------------------
// bluenoise.h -- a void-and-cluster mask, generated rather than shipped.
//
// WHY THIS EXISTS AT ALL. The tracer seeds one PCG stream per pixel and draws
// every sample from it, which makes the error between two neighbouring pixels
// independent -- white noise. White noise is the worst possible distribution
// for an image that is about to be looked at: the eye is most sensitive at low
// spatial frequencies and white noise puts as much energy there as anywhere
// else. Blue noise moves that same total error up into the high frequencies,
// where both the eye and every reconstruction filter downstream discard it.
// The estimator is unchanged and so is its variance -- what changes is where
// the variance sits on screen, and that is a perceptual win for free.
//
// GENERATED, NOT AN ASSET. Every other pattern in this engine is a function of
// its seed, and a 32 KB binary blob checked in beside procedural terrain would
// be the one thing in the build that nobody could regenerate. Ulichney's
// void-and-cluster is about ninety lines and runs in a few milliseconds at this
// size, so there is no reason to ship the output instead of the method.
//
// THE TILE IS 64x64 AND IT IS TOROIDAL. Every energy sum below wraps, so the
// tile has no edge and repeating it across a 4K frame introduces no seam --
// only a periodicity, at 64 pixels, which is far enough into the high
// frequencies to be exactly where the error was being sent anyway.
//
// TWO CHANNELS, INDEPENDENTLY GENERATED. A 2D sample needs two coordinates and
// taking them from one mask at two offsets correlates them along whatever
// offset was chosen. Two masks from two seeds have no such relationship.
//
// Measured, with the harness that verifies it: both channels come out exact
// permutations of 0..4095, low-frequency power (|f| < 6) sits four orders of
// magnitude below the high band where white noise has them within 15 % of each
// other, and the whole thing costs 22 ms at start-up.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace v4 {
namespace bluenoise {

// The tile edge. A power of two so the shader can wrap with a mask rather than
// a modulo, and 64 rather than 128 because the generator's cost is quadratic in
// the pixel count: 64 is ~22 ms for both channels, 128 would be the best part
// of a second of startup for a difference nobody can see through a moving
// image.
constexpr int kDim = 64;
constexpr int kCount = kDim * kDim;

namespace detail {

// The shifts below take kDim as a power of two, and the index arithmetic in
// Field takes it as 64 specifically. Say so, rather than leave a silent
// off-by-a-row waiting for anyone who edits the constant.
static_assert(kDim == 64, "Field's index arithmetic assumes a 64-wide tile");

// The energy kernel -- how strongly one point of the pattern repels another.
//
// SIGMA 1.9 IS ULICHNEY'S, and it is not a free parameter: it sets the radius
// at which the pattern stops caring, and therefore the frequency the resulting
// spectrum peaks at. Truncated at radius 6, where the weight is 0.0044, because
// carrying the tail out to the full 64 costs 100x the work to move the sums by
// a fraction of a percent.
constexpr int kRadius = 6;
constexpr float kSigma = 1.9f;

struct Kernel {
    float w[(2 * kRadius + 1) * (2 * kRadius + 1)];
    Kernel() {
        const float s2 = 2.0f * kSigma * kSigma;
        for (int dy = -kRadius; dy <= kRadius; ++dy)
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const float d2 = float(dx * dx + dy * dy);
                w[(dy + kRadius) * (2 * kRadius + 1) + (dx + kRadius)] = std::exp(-d2 / s2);
            }
    }
};

// A tiny xorshift, so the initial pattern is reproducible without dragging in
// <random> and its implementation-defined engines.
struct Rng32 {
    uint32_t s;
    explicit Rng32(uint32_t seed) : s(seed ? seed : 0x9E3779B9u) {}
    uint32_t next() {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        return s;
    }
};

// The incremental energy field. Toggling one point costs 169 adds instead of a
// full 4096-cell recomputation, which is the whole reason this terminates in
// milliseconds rather than minutes.
struct Field {
    std::vector<float> e;
    std::vector<uint8_t> b;
    const Kernel &k;

    explicit Field(const Kernel &kern) : e(kCount, 0.0f), b(kCount, 0), k(kern) {}

    void splat(int x, int y, float sign) {
        const int span = 2 * kRadius + 1;
        for (int dy = -kRadius; dy <= kRadius; ++dy) {
            const int yy = ((y + dy) & (kDim - 1)) * kDim;
            const int row = (dy + kRadius) * span;
            for (int dx = -kRadius; dx <= kRadius; ++dx) {
                const int xx = (x + dx) & (kDim - 1);
                e[yy + xx] += sign * k.w[row + (dx + kRadius)];
            }
        }
    }

    void set(int i, bool on) {
        if (bool(b[i]) == on) return;
        b[i] = on ? 1 : 0;
        splat(i & (kDim - 1), i >> 6, on ? 1.0f : -1.0f);
    }

    // The tightest cluster: the 1 with the most other 1s crowded around it.
    int tightestCluster() const {
        int best = -1;
        float bestE = -1e30f;
        for (int i = 0; i < kCount; ++i)
            if (b[i] && e[i] > bestE) {
                bestE = e[i];
                best = i;
            }
        return best;
    }

    // The largest void: the 0 furthest from any 1.
    //
    // AND ALSO THE TIGHTEST CLUSTER OF ZEROS, which is why the second half of
    // the ranking below uses the same call rather than a second energy field.
    // The kernel sums to the same constant everywhere on a toroidal domain, so
    // the energy of the 0s is exactly that constant minus the energy of the 1s
    // -- the argmax of one is the argmin of the other, identically.
    int largestVoid() const {
        int best = -1;
        float bestE = 1e30f;
        for (int i = 0; i < kCount; ++i)
            if (!b[i] && e[i] < bestE) {
                bestE = e[i];
                best = i;
            }
        return best;
    }
};

// One channel: a permutation of 0..kCount-1 in which every prefix is itself a
// well-distributed point set. That last property is the whole point -- it is
// what makes ANY threshold of the finished mask a blue-noise binary pattern,
// and therefore what makes the mask usable as a per-pixel sample offset.
inline void generateChannel(uint32_t seed, const Kernel &kern, std::vector<float> &out) {
    Field f(kern);

    // A tenth filled. Ulichney's own figure; the pattern only has to be dense
    // enough for "tightest cluster" and "largest void" to mean something.
    const int ones = kCount / 10;
    Rng32 rng(seed);
    for (int placed = 0; placed < ones;) {
        const int i = int(rng.next() % uint32_t(kCount));
        if (!f.b[i]) {
            f.set(i, true);
            ++placed;
        }
    }

    // PHASE 0 -- make the random pattern into a blue-noise one, by repeatedly
    // taking the most crowded point and moving it to the emptiest place. It has
    // converged when the emptiest place IS where the point just came from,
    // which is a fixed point rather than an iteration count.
    for (int guard = 0; guard < 64 * kCount; ++guard) {
        const int c = f.tightestCluster();
        if (c < 0) break;
        f.set(c, false);
        const int v = f.largestVoid();
        if (v < 0 || v == c) {
            f.set(c, true);
            break;
        }
        f.set(v, true);
    }

    const std::vector<uint8_t> initial = f.b;
    std::vector<int> rank(kCount, 0);

    // PHASE 1 -- rank the initial pattern downwards. Removing the tightest
    // cluster each time means the LAST point standing is the most isolated one,
    // and it gets rank 0: the first dot any threshold turns on.
    for (int r = ones - 1; r >= 0; --r) {
        const int c = f.tightestCluster();
        if (c < 0) break;
        rank[c] = r;
        f.set(c, false);
    }

    // PHASE 2 AND 3 -- rank upwards from the initial pattern, filling the
    // largest void each time. One loop rather than Ulichney's two, for the
    // reason given on largestVoid(): past the half-way point his "tightest
    // cluster of the minority" and this "largest void" are the same location.
    f.b = initial;
    std::fill(f.e.begin(), f.e.end(), 0.0f);
    for (int i = 0; i < kCount; ++i)
        if (f.b[i]) f.splat(i & (kDim - 1), i >> 6, 1.0f);
    for (int r = ones; r < kCount; ++r) {
        const int v = f.largestVoid();
        if (v < 0) break;
        rank[v] = r;
        f.set(v, true);
    }

    // Ranks to [0,1). The half-texel offset keeps the extremes off 0 and 1
    // exactly, so a sample drawn from this can never sit precisely on the edge
    // of a domain that a later frac() would wrap.
    out.resize(kCount);
    for (int i = 0; i < kCount; ++i) out[i] = (float(rank[i]) + 0.5f) / float(kCount);
}

}  // namespace detail

// Two interleaved channels, RG, kDim x kDim, ready to upload. Deterministic:
// the same two seeds give the same mask on any machine, which matters because
// a screenshot taken with blue noise on has to be reproducible.
inline std::vector<float> generateRG() {
    const detail::Kernel kern;
    std::vector<float> r, g;
    detail::generateChannel(0x1BADB002u, kern, r);
    detail::generateChannel(0x5EED1E55u, kern, g);

    std::vector<float> rg(size_t(kCount) * 2);
    for (int i = 0; i < kCount; ++i) {
        rg[size_t(i) * 2 + 0] = r[size_t(i)];
        rg[size_t(i) * 2 + 1] = g[size_t(i)];
    }
    return rg;
}

}  // namespace bluenoise
}  // namespace v4
