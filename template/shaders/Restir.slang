// ---------------------------------------------------------------------------
// Restir.slang -- reservoirs, and the resampling that makes one sample per
//                 pixel behave like hundreds.
//
// WHY GI AND NOT DI. Textbook ReSTIR resamples DIRECT lighting, and it is
// spectacular when there are thousands of lights to choose between. This wood
// has ONE: the sun, plus a sky dome. There is nothing to choose. Direct
// lighting here is already a single cone sample against a disk and resampling
// it would buy nothing at all.
//
// The noise is all in the INDIRECT. Every pixel fires one bounce into a canopy
// and whatever it hits decides that pixel's indirect light -- one sample of an
// integral over the hemisphere, which is why a one-sample path trace of a wood
// looks like television static. But the pixel NEXT DOOR fired its own bounce,
// from almost the same place, into almost the same hemisphere. Its sample is
// very nearly a valid sample for this pixel too.
//
// That is the whole idea: a reservoir keeps one sample and a weight, neighbours
// merge their reservoirs, and after temporal and spatial reuse each pixel is
// effectively integrating over hundreds of bounces it never traced. The ray
// count does not change. The noise falls off a cliff.
//
// ---------------------------------------------------------------------------
// WHAT A SAMPLE IS HERE.
//
// A point in the world that light came FROM, and how much. Not a direction --
// a POINT, and that distinction is the reason the technique works across
// pixels. Two neighbouring pixels see the same lit patch of canopy from
// slightly different places, so the same direction means different things to
// them, while the same POINT means the same thing and the difference in
// geometry between them is a Jacobian that can be computed exactly.
//
// ---------------------------------------------------------------------------
// RESERVOIR SAMPLING, in one paragraph, because the code below is short enough
// to look like it is doing nothing.
//
// A reservoir holds one candidate and the SUM of the weights of everything it
// has considered. Offer it a new candidate with weight w: it replaces its held
// sample with probability w / (wSum + w). After N offers it holds one sample,
// drawn in proportion to weight, having stored only one. Merging two reservoirs
// is offering the other's held sample weighted by its own total -- which is why
// spatial reuse costs one merge per neighbour and not one per sample they ever
// saw.
// ---------------------------------------------------------------------------
#pragma once

#include "Vecmath.slang"

// One resampled indirect sample.
//
// PACKED TO 48 BYTES and not one more. There is one of these per pixel per
// buffer and there are four buffers; at 4K that is already 100 MB, so a lazy
// float4-per-field layout would cost more bandwidth than the resampling saves.
// PAIRED float3+scalar, DELIBERATELY, and not for tidiness. A bare float3 in a
// structured buffer is 12 bytes of data with an alignment the language does not
// pin down, so three of them in a row is a layout the shader and the C++ can
// legitimately disagree about -- and the failure is not a compile error, it is
// a reservoir that reads its neighbour's fields and produces plausible garbage.
// Each float3 is followed by the scalar that fills its row, so the struct is
// three exact 16-byte rows and there is nothing left to disagree about.
struct Reservoir {
    float3 xs;    // the point light came from
    float wSum;   // sum of candidate weights seen
    float3 ns;    // its normal, for the Jacobian and for rejecting backfaces
    float W;      // the resampled importance weight -- what shading multiplies by
    float3 Ls;    // radiance leaving xs toward the pixel that found it
    uint M;       // how many candidates are behind this sample
};

// The target function: what the resampling is trying to be proportional to.
//
// LUMINANCE OF THE INCOMING RADIANCE, and deliberately not the full shading
// integrand. The correct target would include the BSDF and the geometry term,
// which would make it different for every pixel and therefore recomputed for
// every neighbour in the spatial loop. Luminance alone is a good proxy -- it is
// what decides whether a sample matters -- and it is the same number for
// everyone, so a neighbour's weight can be reused directly.
float restirTarget(Reservoir r)
{
    return luminance(r.Ls);
}

Reservoir restirEmpty()
{
    Reservoir r;
    r.xs = float3(0.0f);
    r.ns = float3(0.0f, 1.0f, 0.0f);
    r.Ls = float3(0.0f);
    r.wSum = 0.0f;
    r.W = 0.0f;
    r.M = 0u;
    return r;
}

// Offer a candidate. Returns true if the reservoir took it.
bool restirUpdate(inout Reservoir r, float3 xs, float3 ns, float3 Ls, float w, inout Rng rng)
{
    r.wSum += w;
    r.M += 1u;
    // > rather than >= so a zero-weight candidate can never displace a real
    // one, which matters because most candidates in a canopy ARE zero: the
    // bounce hit something in shadow.
    if (w > 0.0f && rng.next2D().x * r.wSum < w) {
        r.xs = xs;
        r.ns = ns;
        r.Ls = Ls;
        return true;
    }
    return false;
}

// Merge another reservoir in, its held sample weighted by that reservoir's own
// total. `jacobian` corrects for the neighbour having seen xs from somewhere
// else; pass 1 for temporal reuse of the same pixel.
void restirMerge(inout Reservoir r, Reservoir other, float jacobian, inout Rng rng)
{
    if (other.M == 0u || other.W <= 0.0f)
        return;
    const float w = restirTarget(other) * other.W * float(other.M) * jacobian;
    r.wSum += w;
    if (w > 0.0f && rng.next2D().x * r.wSum < w) {
        r.xs = other.xs;
        r.ns = other.ns;
        r.Ls = other.Ls;
    }
    r.M += other.M;
}

// Turn the accumulated weight into the factor shading multiplies by.
//
// W = wSum / (M * pHat). The division by pHat is what un-biases the fact that
// the sample was chosen in proportion to pHat in the first place; without it
// the bright samples that resampling deliberately favours would be counted
// twice and the whole image would gain energy.
void restirFinalise(inout Reservoir r)
{
    const float pHat = restirTarget(r);
    r.W = (pHat > 0.0f && r.M > 0u) ? (r.wSum / (float(r.M) * pHat)) : 0.0f;
}

// CAP THE HISTORY, or temporal reuse never lets go.
//
// M grows without bound otherwise, and a reservoir with M in the thousands is
// so confident that a new candidate essentially never displaces its sample --
// so the moment the sun moves or you walk behind a trunk, the pixel keeps
// showing light that is no longer there. Capping M is what bounds how stale the
// answer can get, and 20 is roughly a third of a second at 60 fps.
void restirCapM(inout Reservoir r, uint cap)
{
    if (r.M > cap) {
        r.wSum *= float(cap) / float(r.M);
        r.M = cap;
    }
}

// The change-of-variables factor for reusing a neighbour's sample.
//
// The neighbour saw xs from xq; this pixel sees it from xr. The solid angle xs
// subtends is different from the two points, and the cosine at xs is different
// too. Without this correction, reused samples from grazing angles are
// over-counted and surfaces near geometry gain a bright rim.
float restirJacobian(float3 xr, float3 xq, float3 xs, float3 ns)
{
    const float3 dr = xr - xs;
    const float3 dq = xq - xs;
    const float lr2 = max(dot(dr, dr), 1e-8f);
    const float lq2 = max(dot(dq, dq), 1e-8f);
    const float cr = abs(dot(normalize(dr), ns));
    const float cq = abs(dot(normalize(dq), ns));
    if (cq < 1e-6f)
        return 0.0f;
    // Clamped. A near-tangential reuse produces an enormous ratio, and one
    // pixel with a Jacobian of 10^4 is a permanent white speck that temporal
    // reuse will then happily spread to its neighbours.
    return clamp((cr * lq2) / (cq * lr2), 0.0f, 8.0f);
}
