// ---------------------------------------------------------------------------
// skycdf.h -- importance sampling the sky dome.
//
// WHY THIS EXISTS. The integrator light-samples the SUN and nothing else. The
// dome is only ever reached by a BSDF bounce that happens to escape the canopy,
// which means the ambient light on a shadowed surface -- most of the light
// there is -- is estimated by bouncing at random and hoping. The darkest parts
// of the frame are the noisiest for exactly that reason.
//
// A SECOND STRATEGY ONLY HELPS IF IT DIFFERS FROM THE FIRST. Cosine-sampling
// the hemisphere and tracing a shadow ray would be the same distribution a
// diffuse BSDF already draws from, and multiple importance sampling between two
// identical strategies buys nothing. So this samples the dome in proportion to
// its RADIANCE: the Perez sky is far from uniform -- bright near the sun, bright
// at the horizon, dim at the zenith opposite -- and aiming at the bright parts
// is information the BSDF does not have.
//
// A 2D piecewise-constant distribution over an equirectangular grid: pick a row
// from the marginal, a column from that row's conditional. Rebuilt on the host
// whenever the sun moves, which is cheap -- 64x32 evaluations of an analytic
// function -- and uploaded as a few kilobytes.
//
// The rows are weighted by sin(theta) so the poles, where the grid cells shrink
// to nothing, are not oversampled. Forgetting that is the classic way to get a
// sampler that is subtly wrong everywhere rather than obviously wrong somewhere.
// ---------------------------------------------------------------------------
#pragma once

#include "sky.h"

namespace v2 {

constexpr int kSkyCdfW = 64;  // azimuth
constexpr int kSkyCdfH = 32;  // polar angle, 0 = straight up

// Uploaded whole. Small enough that the layout can stay obvious.
struct SkyCdfGPU {
    // marginal[i] is the CDF up to row i; marginal[kSkyCdfH] == 1.
    float marginal[kSkyCdfH + 1];
    // conditional[i][j] is the CDF along row i up to column j.
    float conditional[kSkyCdfH][kSkyCdfW + 1];
    // The unnormalised function value per cell, for the pdf.
    float func[kSkyCdfH][kSkyCdfW];
    // Integral of func over the sphere, so a pdf can be normalised.
    float funcInt;
    int valid;

    // ---------------------------------------------------------------- device
    static V2_FN int upperBound(const float *cdf, int n, float u) {
        int lo = 0, hi = n;
        while (lo < hi) {
            const int mid = (lo + hi) >> 1;
            if (cdf[mid] <= u) lo = mid + 1; else hi = mid;
        }
        return maxi(0, mini(n - 1, lo - 1));
    }

    // theta from the row, phi from the column, both at the sampled offset
    // WITHIN the cell so the result is continuous rather than a grid of spikes.
    V2_FN Vec3 sample(float u1, float u2, float *pdf) const {
        if (!valid || funcInt <= 0.0f) { *pdf = 0.0f; return Vec3(0.0f, 1.0f, 0.0f); }

        const int row = upperBound(marginal, kSkyCdfH + 1, u1);
        const float rowLo = marginal[row], rowHi = marginal[row + 1];
        const float dv = (rowHi > rowLo) ? (u1 - rowLo) / (rowHi - rowLo) : 0.5f;

        const float *cond = conditional[row];
        const int col = upperBound(cond, kSkyCdfW + 1, u2);
        const float colLo = cond[col], colHi = cond[col + 1];
        const float du = (colHi > colLo) ? (u2 - colLo) / (colHi - colLo) : 0.5f;

        const float theta = (float(row) + dv) * (PI / float(kSkyCdfH));
        const float phi = (float(col) + du) * (TWO_PI / float(kSkyCdfW));
        const float st = sinf(theta);

        *pdf = pdfFor(row, col, st);
        // y is up in this engine, and theta is measured from it.
        return Vec3(st * cosf(phi), cosf(theta), st * sinf(phi));
    }

    // The same pdf, for a direction that came from somewhere else -- which is
    // what MIS needs in order to weight a BSDF sample against this strategy.
    V2_FN float pdf(Vec3 d) const {
        if (!valid || funcInt <= 0.0f) return 0.0f;
        const Vec3 n = normalize(d);
        const float theta = acosf(clampf(n.y, -1.0f, 1.0f));
        float phi = atan2f(n.z, n.x);
        if (phi < 0.0f) phi += TWO_PI;
        const int row = mini(kSkyCdfH - 1, maxi(0, int(theta * float(kSkyCdfH) / PI)));
        const int col = mini(kSkyCdfW - 1, maxi(0, int(phi * float(kSkyCdfW) / TWO_PI)));
        return pdfFor(row, col, sinf(theta));
    }

  private:
    // Cell value over the integral gives a density in (u,v); dividing by the
    // solid angle a cell covers turns it into a density per steradian.
    V2_FN float pdfFor(int row, int col, float sinTheta) const {
        if (sinTheta <= 1e-6f) return 0.0f;
        const float cells = float(kSkyCdfW) * float(kSkyCdfH);
        return func[row][col] * cells / (funcInt * TWO_PI * PI * sinTheta);
    }
};

// ---------------------------------------------------------------------------
// The host half: fill one in from the sky as it currently stands.
//
// Called whenever the sun moves, so it has to stay cheap: 2048 evaluations of
// an analytic function and two prefix sums.
// ---------------------------------------------------------------------------
inline void buildSkyCdf(const SkyGPU &sky, SkyCdfGPU *out) {
    double total = 0.0;
    for (int i = 0; i < kSkyCdfH; ++i) {
        const float theta = (float(i) + 0.5f) * (PI / float(kSkyCdfH));
        // sin(theta) is the cell's shrinking width toward the poles. Without it
        // the sampler piles samples into slivers that cover almost no sky.
        const float st = sinf(theta);
        double rowSum = 0.0;
        for (int j = 0; j < kSkyCdfW; ++j) {
            const float phi = (float(j) + 0.5f) * (TWO_PI / float(kSkyCdfW));
            const Vec3 d(st * cosf(phi), cosf(theta), st * sinf(phi));
            const Vec3 r = sky.domeRadiance(d);
            // Luminance, not a channel: the sampler is choosing WHERE to look,
            // and brightness is what matters for that, not hue.
            const float lum = 0.2126f * r.x + 0.7152f * r.y + 0.0722f * r.z;
            const float f = maxf(0.0f, lum) * st;
            out->func[i][j] = f;
            rowSum += double(f);
        }
        // Conditional CDF along the row.
        float acc = 0.0f;
        out->conditional[i][0] = 0.0f;
        for (int j = 0; j < kSkyCdfW; ++j) {
            acc += out->func[i][j];
            out->conditional[i][j + 1] = acc;
        }
        if (acc > 0.0f)
            for (int j = 1; j <= kSkyCdfW; ++j) out->conditional[i][j] /= acc;
        else
            for (int j = 1; j <= kSkyCdfW; ++j)
                out->conditional[i][j] = float(j) / float(kSkyCdfW);
        out->marginal[i] = float(rowSum);
        total += rowSum;
    }

    // The row sums were parked in marginal[]; turn them into a CDF in place.
    float acc = 0.0f;
    float rowVals[kSkyCdfH];
    for (int i = 0; i < kSkyCdfH; ++i) rowVals[i] = out->marginal[i];
    out->marginal[0] = 0.0f;
    for (int i = 0; i < kSkyCdfH; ++i) {
        acc += rowVals[i];
        out->marginal[i + 1] = acc;
    }
    if (acc > 0.0f)
        for (int i = 1; i <= kSkyCdfH; ++i) out->marginal[i] /= acc;
    else
        for (int i = 1; i <= kSkyCdfH; ++i) out->marginal[i] = float(i) / float(kSkyCdfH);

    out->funcInt = float(total);
    out->valid = (total > 0.0) ? 1 : 0;
}

}  // namespace v2
