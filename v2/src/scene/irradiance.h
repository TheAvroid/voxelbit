// ---------------------------------------------------------------------------
// irradiance.h -- a probe grid for the indirect bounces.
//
// WHY. Sky importance sampling made the FIRST bounce cheaper to estimate. The
// bounces after it are still traced one ray at a time, and they are where the
// remaining noise lives: by the third bounce a path has picked up three random
// directions, and the estimate of what it brings back is nearly all variance.
// It is also where the time goes -- six bounces of tree geometry per sample.
//
// So stop tracing them. Indirect light in a wood is low-frequency: it varies
// over metres, not pixels. Sample it once on a grid, and let the shading read
// the grid instead of continuing the path.
//
// THIS IS AN APPROXIMATION AND IT WILL CHANGE THE IMAGE. Unlike the sky
// sampler, which converges to exactly the same picture, this converges to a
// different one -- indirect light gets the resolution of the probe spacing
// rather than of the pixel. Contact darkening under a trunk softens. That is
// the trade, and it is why it is a toggle.
//
// WHAT IS STORED. Four spherical-harmonic coefficients per colour channel: the
// constant term and the three linear ones. That is enough to know roughly which
// direction the light is coming from, which is the difference between a grid
// that looks lit and one that looks like flat ambient. Reconstructing irradiance
// from L1 is Ramamoorthi and Hanrahan's result; the convolution weights below
// are theirs.
//
// THE GRID FOLLOWS THE CAMERA. There is no world bound to cover -- the terrain
// streams in chunks around wherever the player is -- so the volume is centred
// on the camera and snapped to its own spacing, and each frame's probes take
// their history from the PREVIOUS frame's grid by world position. That way
// walking forward does not throw the grid away; only the strip of probes that
// just entered the volume starts cold.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"

namespace v2 {

// 48 x 24 x 48 at 2 m gives a 96 x 48 x 96 m box around the camera. The spacing
// was measured, not guessed: against a converged reference the grid's own error
// is RMSE 20.1 at 4 m, 11.6 at 2 m and 9.1 at 1 m, and 4 m leaks enough light
// out of the clearings to run the whole frame 25% bright. Beyond the box the
// path tracer takes over, so the size costs accuracy nowhere -- only speed.
constexpr int kProbeX = 48;
constexpr int kProbeY = 24;
constexpr int kProbeZ = 48;
constexpr int kProbeCount = kProbeX * kProbeY * kProbeZ;

// 64 bytes, so a probe is one cache line and the pad is free.
//
// Spelled with a plain float array rather than float4: this header is included
// by the host build too, where float4 is a stand-in struct declared for layout
// only, and there is no reason to make a probe depend on it.
struct alignas(16) ProbeSH {
    // [channel][coefficient], coefficients being (L00, L1-1, L10, L11).
    // Radiance, not irradiance -- the cosine convolution happens at lookup,
    // where the normal is known.
    float c[3][4];
    // How much history this probe has, 0 to 1. A probe that has just scrolled
    // into the volume, or that baked inside solid geometry, is not trusted and
    // is dropped from the interpolation rather than averaged in dark.
    float valid;
    float pad[3];
};

struct IrradianceGridGPU {
    ProbeSH *data;
    Vec3 origin;  // world position of probe (0, 0, 0)
    float spacing;

    V2_FN Vec3 probePos(int ix, int iy, int iz) const {
        return origin + Vec3(float(ix), float(iy), float(iz)) * spacing;
    }
    static V2_FN int index(int ix, int iy, int iz) {
        return (iz * kProbeY + iy) * kProbeX + ix;
    }

    // Irradiance over pi, for a surface at p facing n. Multiply by the diffuse
    // albedo and you have the outgoing radiance -- the same thing the path
    // would have returned, without the path.
    //
    // Returns false when the point is outside the volume or every probe around
    // it is untrusted, and the caller then does whatever it did before.
    V2_FN bool lookup(Vec3 p, Vec3 n, Vec3 *out) const {
        if (!data || spacing <= 0.0f) return false;

        // Step off the surface along the normal before interpolating. Without
        // it a floor samples the probes that sit just below the floor, which
        // are dark, and every flat surface reads too dim near its own plane.
        const Vec3 q = p + n * (0.45f * spacing);
        const Vec3 g = (q - origin) * (1.0f / spacing);
        if (g.x < 0.0f || g.y < 0.0f || g.z < 0.0f) return false;
        const int ix = int(g.x), iy = int(g.y), iz = int(g.z);
        if (ix >= kProbeX - 1 || iy >= kProbeY - 1 || iz >= kProbeZ - 1) return false;

        const float fx = g.x - float(ix), fy = g.y - float(iy), fz = g.z - float(iz);

        Vec3 sum(0.0f);
        float wsum = 0.0f;
        for (int k = 0; k < 8; ++k) {
            const int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
            const ProbeSH &pr = data[index(ix + dx, iy + dy, iz + dz)];
            if (pr.valid <= 0.01f) continue;

            float w = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy) * (dz ? fz : 1.0f - fz);
            if (w <= 0.0f) continue;

            // A probe behind the surface has no business lighting it. Squaring
            // the half-angle term makes the falloff smooth rather than a hard
            // cut, which would show up as a seam along the grid planes.
            const Vec3 toProbe = normalize(probePos(ix + dx, iy + dy, iz + dz) - p);
            const float facing = 0.5f * dot(toProbe, n) + 0.5f;
            w *= facing * facing + 0.02f;
            w *= pr.valid;
            if (w <= 0.0f) continue;

            sum = sum + evalIrradiance(pr, n) * w;
            wsum += w;
        }
        if (wsum <= 1e-6f) return false;
        *out = sum * (1.0f / wsum);
        return true;
    }

    // E(n) / pi from the four coefficients. The two weights are the cosine
    // lobe's own SH expansion: A0/pi = 1 and A1/pi = 2/3, folded into the
    // basis constants Y00 = 0.2820948 and Y1 = 0.4886025.
    static V2_FN Vec3 evalIrradiance(const ProbeSH &pr, Vec3 n) {
        const float k0 = 0.2820948f;
        const float k1 = 0.3257350f;  // (2/3) * 0.4886025
        // Clamped, because an L1 fit to a sharp sky can ring negative, and a
        // negative irradiance shows up as a black bruise on a lit slope.
        float e[3];
        for (int ch = 0; ch < 3; ++ch) {
            const float *c = pr.c[ch];
            e[ch] = maxf(0.0f, k0 * c[0] + k1 * (c[1] * n.y + c[2] * n.z + c[3] * n.x));
        }
        return Vec3(e[0], e[1], e[2]);
    }
};

// Project one radiance sample arriving from direction d into the four
// coefficients, uniform over the sphere so the estimator is (4*pi/N) * sum.
V2_FN void shAccumulate(Vec3 L, Vec3 d, float weight, float c[3][4]) {
    const float y0 = 0.2820948f;
    const float y1 = 0.4886025f;
    const float b[4] = {y0, y1 * d.y, y1 * d.z, y1 * d.x};
    const float l[3] = {L.x, L.y, L.z};
    for (int ch = 0; ch < 3; ++ch)
        for (int k = 0; k < 4; ++k) c[ch][k] += l[ch] * b[k] * weight;
}

}  // namespace v2
