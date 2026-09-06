// ---------------------------------------------------------------------------
// sky.h -- Preetham's analytic daylight model, host half.
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
// THE SPLIT IS THE SAME ONE v2 MADE. Fitting the Perez coefficients means
// tan(), pow() and a dozen polynomial evaluations, and it depends only on the
// sun angle and the turbidity -- not on the ray. Doing it per ray on the GPU
// would be several hundred wasted instructions on every escaping ray in the
// scene. So this class fits once a frame and hands over a V6Sky, which is a
// plain block of already-fitted coefficients; shaders/Sky.slang evaluates them.
// ---------------------------------------------------------------------------
#pragma once

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"

namespace v6 {

// The cosine of the sun's half-angle. Shared.slang carries the same number for
// the shader; they are asserted equal in world.h.
constexpr float SUN_COS_THETA_MAX = 0.99998932f;  // cos(0.2665 deg)

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
        sunDir_ = normalize(Vec3(cosf(el) * cosf(az), sinf(el), cosf(el) * sinf(az)));
        build();
    }

    float azimuthDeg() const { return azDeg_; }
    float elevationDeg() const { return elDeg_; }
    Vec3 sunDir() const { return sunDir_; }
    const V6Sky &gpu() const { return g_; }

  private:
    V6Sky g_{};
    Vec3 sunDir_{0.0f, 1.0f, 0.0f};
    float azDeg_ = 38.0f, elDeg_ = 24.0f;

    static float solidAngle() { return TWO_PI * (1.0f - SUN_COS_THETA_MAX); }

    // The same expression shaders/Sky.slang evaluates, needed here only to
    // normalise the fit at the zenith.
    static float perezF(float cosTheta, float gamma, float a, float b, float c, float d, float e) {
        const float ct = maxf(cosTheta, 0.01f);
        const float cg = cosf(gamma);
        return (1.0f + a * expf(b / ct)) * (1.0f + c * expf(d * gamma) + e * cg * cg);
    }

    void build() {
        const float T = turbidity;
        g_.sunDir = float3(sunDir_.x, sunDir_.y, sunDir_.z);
        g_.groundAlbedo = float3(0.16f, 0.13f, 0.10f);
        g_.sunPad = 0.0f;
        g_.groundPad = 0.0f;

        // NIGHT IS A DIMMED HORIZON FIT, NOT AN EXTRAPOLATION.
        //
        // Preetham is fitted for a sun above the horizon. Below it the zenith
        // luminance term runs tan(chi) straight through its pole and the model
        // returns negative radiance -- so the fit is pinned at the horizon and
        // the whole dome is scaled down instead. The 2% floor is what stops
        // night being pure black, which is useless to look at and worse to
        // navigate; it reads as moonlight without pretending to model one.
        const float elDeg = asinf(clampf(sunDir_.y, -1.0f, 1.0f)) * 180.0f / PI;
        const float daylight = sstep(saturate((elDeg + 6.0f) / 10.0f));
        g_.skyScale = 0.00018f * lerpf(0.02f, 1.0f, daylight);

        const float thetaS = acosf(clampf(maxf(sunDir_.y, 0.0f), -1.0f, 1.0f));

        // Distribution coefficients, per Preetham et al. 1999, table 1. Held as
        // the xyz of a float4 rather than a float[3]: an array in a constant
        // buffer has a 16-byte stride, so the two sides would disagree about
        // where every one of these lives. See the packing note in Shared.slang.
        g_.A = float4(0.1787f * T - 1.4630f, -0.0193f * T - 0.2592f, -0.0167f * T - 0.2608f, 0.0f);
        g_.B = float4(-0.3554f * T + 0.4275f, -0.0665f * T + 0.0008f, -0.0950f * T + 0.0092f, 0.0f);
        g_.C = float4(-0.0227f * T + 5.3251f, -0.0004f * T + 0.2125f, -0.0079f * T + 0.2102f, 0.0f);
        g_.D = float4(0.1206f * T - 2.5771f, -0.0641f * T - 0.8989f, -0.0441f * T - 1.6537f, 0.0f);
        g_.E = float4(-0.0670f * T + 0.3703f, -0.0033f * T + 0.0452f, -0.0109f * T + 0.0529f, 0.0f);

        const float ts = thetaS, ts2 = ts * ts, ts3 = ts2 * ts;

        const float chi = (4.0f / 9.0f - T / 120.0f) * (PI - 2.0f * ts);
        const float zenY =
            maxf(((4.0453f * T - 4.9710f) * tanf(chi) - 0.2155f * T + 2.4192f) * 1000.0f, 1.0f);
        const float zenx = (0.00166f * ts3 - 0.00375f * ts2 + 0.00209f * ts) * T * T +
                           (-0.02903f * ts3 + 0.06377f * ts2 - 0.03202f * ts + 0.00394f) * T +
                           (0.11693f * ts3 - 0.21196f * ts2 + 0.06052f * ts + 0.25886f);
        const float zeny = (0.00275f * ts3 - 0.00610f * ts2 + 0.00317f * ts) * T * T +
                           (-0.04214f * ts3 + 0.08970f * ts2 - 0.04153f * ts + 0.00516f) * T +
                           (0.15346f * ts3 - 0.26756f * ts2 + 0.06670f * ts + 0.26688f);
        g_.zenith = float4(zenY, zenx, zeny, 0.0f);

        const float A[3] = {g_.A.x, g_.A.y, g_.A.z};
        const float B[3] = {g_.B.x, g_.B.y, g_.B.z};
        const float C[3] = {g_.C.x, g_.C.y, g_.C.z};
        const float D[3] = {g_.D.x, g_.D.y, g_.D.z};
        const float E[3] = {g_.E.x, g_.E.y, g_.E.z};
        float nf[3];
        for (int i = 0; i < 3; ++i) nf[i] = perezF(1.0f, thetaS, A[i], B[i], C[i], D[i], E[i]);
        g_.normF = float4(nf[0], nf[1], nf[2], 0.0f);

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
        const float irradiance = 22.0f * sunScale * saturate(sunDir_.y * 4.0f);
        const Vec3 rad = trans * (irradiance / maxf(1e-8f, solidAngle()));
        g_.sunRadiance = float3(rad.x, rad.y, rad.z);
    }
};

}  // namespace v6
