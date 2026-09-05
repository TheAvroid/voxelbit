// ---------------------------------------------------------------------------
// sky.h -- Preetham's analytic daylight model, split across the PCI bus.
//
// WHY AN ANALYTIC SKY AND NOT A CONSTANT: almost all of the light reaching the
// floor of a wood is sky light, not sun -- the canopy blocks the sun over most
// of the ground. Sky light is strongly directional (bright near the horizon in
// haze, deep blue at zenith) and strongly coloured, and a constant-colour dome
// loses the blue fill on the shaded side of every trunk, which is a large part
// of why an outdoor render looks outdoor.
//
// The sun is sampled as a CONE, not a direction. Its half-angle is about a
// quarter of a degree, which sounds negligible until you look at the shadow of
// a branch twenty metres up: the penumbra is then some nine centimetres wide,
// and that softness is visible on the ground everywhere in the frame.
//
// WHAT IS NEW IN v2 IS THE SPLIT. Fitting the Perez coefficients means tan(),
// pow() and a dozen polynomial evaluations, and it depends only on the sun
// angle and the turbidity -- not on the ray. Doing it per ray on the GPU would
// be several hundred wasted instructions in the miss program, which every
// escaping ray in the scene runs. So SkyGPU is a plain POD of already-fitted
// coefficients that rides along in the launch parameters, and the fitting lives
// in the host-side Sky that produces it. The device half is then twenty lines
// of arithmetic with no branches worth the name.
// ---------------------------------------------------------------------------
#pragma once

#include "../core/vecmath.h"

namespace v2 {

// The sun subtends about 0.53 degrees; this is the cosine of its half-angle.
constexpr float SUN_COS_THETA_MAX = 0.99998932f;  // cos(0.2665 deg)

// ---------------------------------------------------------------------------
// The device half: coefficients in, radiance out.
// ---------------------------------------------------------------------------
// A plain aggregate with no initialisers: it is a member of LaunchParams, which
// lives in __constant__ memory and may only be constant-initialised. Sky::build
// below fills every field, so nothing here is ever read before it is written.
struct SkyGPU {
    Vec3 sunDir;
    Vec3 sunRadiance;   // constant across the disk
    Vec3 groundAlbedo;
    float A[3], B[3], C[3], D[3], E[3];
    float zenith[3];  // Y in cd/m^2, then x and y
    float normF[3];
    float skyScale;

    V2_FN float solidAngle() const { return TWO_PI * (1.0f - SUN_COS_THETA_MAX); }

    static V2_FN float perezF(float cosTheta, float gamma, float a, float b, float c, float d,
                              float e) {
        const float ct = maxf(cosTheta, 0.01f);
        const float cg = cosf(gamma);
        return (1.0f + a * expf(b / ct)) * (1.0f + c * expf(d * gamma) + e * cg * cg);
    }

    // Radiance of the dome in the renderer's working units.
    V2_FN Vec3 perez(Vec3 dir) const {
        const Vec3 dn = normalize(dir);
        const float cosTheta = maxf(dn.y, 0.01f);
        const float gamma = acosf(clampf(dot(dn, sunDir), -1.0f, 1.0f));

        float v[3];
        for (int i = 0; i < 3; ++i)
            v[i] = zenith[i] * perezF(cosTheta, gamma, A[i], B[i], C[i], D[i], E[i]) /
                   maxf(1e-6f, normF[i]);

        // xyY -> XYZ -> linear sRGB.
        const float Y = maxf(0.0f, v[0]), x = v[1], y = maxf(1e-4f, v[2]);
        const float X = (x / y) * Y;
        const float Z = ((1.0f - x - y) / y) * Y;
        Vec3 rgb(3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z,
                 -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z,
                 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z);
        rgb = vmax(rgb, Vec3(0.0f));
        return rgb * skyScale;
    }

    // The dome WITHOUT the sun disk. Kept separate from the disk so multiple
    // importance sampling can weight the two contributions differently: the
    // disk is found by the cone sampler, the dome by whatever the BSDF did.
    V2_FN Vec3 domeRadiance(Vec3 d) const {
        // Below the horizon there is no sky, but a hard cut at y = 0 draws a
        // visible seam right where the eye is looking. Fade into a dim ground
        // bounce instead -- this stands in for the world beyond the terrain
        // patch, and it keeps the far treeline sitting in something.
        if (d.y <= 0.0f) {
            const float t = saturate(-d.y / 0.10f);
            const Vec3 horizon = perez(Vec3(d.x, 1e-4f, d.z));
            const float avg = (horizon.x + horizon.y + horizon.z) * (1.0f / 3.0f);
            return lerp(horizon, groundAlbedo * avg, sstep(t));
        }
        return perez(d);
    }

    // The solar disk itself. Zero outside the cone, so this can simply be added.
    V2_FN Vec3 diskRadiance(Vec3 d) const {
        return dot(d, sunDir) >= SUN_COS_THETA_MAX ? sunRadiance : Vec3(0.0f);
    }

    V2_FN Vec3 radiance(Vec3 d) const { return domeRadiance(d) + diskRadiance(d); }
};

// ---------------------------------------------------------------------------
// The host half: fit the coefficients once, hand over a SkyGPU.
// ---------------------------------------------------------------------------
class Sky {
  public:
    float turbidity = 2.6f;  // 2 is a clear alpine day; 6 is summer haze
    float sunScale = 1.0f;

    Sky() { setSun(38.0f, 24.0f); }

    void setSun(float azimuthDeg, float elevationDeg) {
        azDeg_ = azimuthDeg;
        elDeg_ = elevationDeg;
        const float az = azimuthDeg * PI / 180.0f;
        const float el = elevationDeg * PI / 180.0f;
        g_.sunDir = normalize(Vec3(cosf(el) * cosf(az), sinf(el), cosf(el) * sinf(az)));
        build();
    }

    float azimuthDeg() const { return azDeg_; }
    float elevationDeg() const { return elDeg_; }
    const SkyGPU &gpu() const { return g_; }
    Vec3 sunDir() const { return g_.sunDir; }

  private:
    SkyGPU g_;
    float azDeg_ = 38.0f, elDeg_ = 24.0f;

    void build() {
        const float T = turbidity;
        g_.groundAlbedo = Vec3(0.16f, 0.13f, 0.10f);

        // NIGHT IS A DIMMED HORIZON FIT, NOT AN EXTRAPOLATION.
        //
        // Preetham is fitted for a sun above the horizon. Below it the zenith
        // luminance term runs tan(chi) straight through its pole and the model
        // returns negative radiance -- so the fit is pinned at the horizon and
        // the whole dome is scaled down instead. The 2% floor is what stops
        // night being pure black, which is useless to look at and worse to
        // navigate; it reads as moonlight without pretending to model one.
        const float elDeg = asinf(clampf(g_.sunDir.y, -1.0f, 1.0f)) * 180.0f / PI;
        const float daylight = sstep(saturate((elDeg + 6.0f) / 10.0f));
        g_.skyScale = 0.00018f * lerpf(0.02f, 1.0f, daylight);

        const float thetaS = acosf(clampf(maxf(g_.sunDir.y, 0.0f), -1.0f, 1.0f));

        // Distribution coefficients, per Preetham et al. 1999, table 1.
        g_.A[0] =  0.1787f * T - 1.4630f;  g_.B[0] = -0.3554f * T + 0.4275f;
        g_.C[0] = -0.0227f * T + 5.3251f;  g_.D[0] =  0.1206f * T - 2.5771f;
        g_.E[0] = -0.0670f * T + 0.3703f;

        g_.A[1] = -0.0193f * T - 0.2592f;  g_.B[1] = -0.0665f * T + 0.0008f;
        g_.C[1] = -0.0004f * T + 0.2125f;  g_.D[1] = -0.0641f * T - 0.8989f;
        g_.E[1] = -0.0033f * T + 0.0452f;

        g_.A[2] = -0.0167f * T - 0.2608f;  g_.B[2] = -0.0950f * T + 0.0092f;
        g_.C[2] = -0.0079f * T + 0.2102f;  g_.D[2] = -0.0441f * T - 1.6537f;
        g_.E[2] = -0.0109f * T + 0.0529f;

        const float ts = thetaS, ts2 = ts * ts, ts3 = ts2 * ts;

        const float chi = (4.0f / 9.0f - T / 120.0f) * (PI - 2.0f * ts);
        g_.zenith[0] = ((4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f) * 1000.0f;
        g_.zenith[0] = maxf(g_.zenith[0], 1.0f);

        g_.zenith[1] = ( 0.00166f * ts3 - 0.00375f * ts2 + 0.00209f * ts) * T * T +
                       (-0.02903f * ts3 + 0.06377f * ts2 - 0.03202f * ts + 0.00394f) * T +
                       ( 0.11693f * ts3 - 0.21196f * ts2 + 0.06052f * ts + 0.25886f);
        g_.zenith[2] = ( 0.00275f * ts3 - 0.00610f * ts2 + 0.00317f * ts) * T * T +
                       (-0.04214f * ts3 + 0.08970f * ts2 - 0.04153f * ts + 0.00516f) * T +
                       ( 0.15346f * ts3 - 0.26756f * ts2 + 0.06670f * ts + 0.26688f);

        for (int i = 0; i < 3; ++i)
            g_.normF[i] = SkyGPU::perezF(1.0f, thetaS, g_.A[i], g_.B[i], g_.C[i], g_.D[i], g_.E[i]);

        // Extinction along the slant path, as a function of air mass. Kasten
        // and Young's formula rather than 1/cos, which diverges at the horizon
        // and would make a setting sun infinitely red.
        const float elev = maxf(0.0f, 90.0f - thetaS * 180.0f / PI);
        const float am =
            1.0f / (sinf(elev * PI / 180.0f) + 0.50572f * powf(elev + 6.07995f, -1.6364f));

        // Rayleigh optical depth is strongly wavelength dependent, which is the
        // whole reason a low sun is orange. Coefficients are at roughly 615,
        // 535 and 465 nm -- the sRGB primaries.
        const Vec3 tau(0.1170f, 0.1900f, 0.4200f);
        const Vec3 mie(0.0295f, 0.0330f, 0.0380f);
        const Vec3 trans = vexp(-(tau + mie * (turbidity - 1.0f)) * am);

        // Irradiance in working units, converted to radiance over the disk.
        const float irradiance = 22.0f * sunScale * saturate(g_.sunDir.y * 4.0f);
        g_.sunRadiance = trans * (irradiance / maxf(1e-8f, g_.solidAngle()));
    }
};

}  // namespace v2
