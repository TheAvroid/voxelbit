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

namespace v7 {

// The cosine of the sun's half-angle. Shared.slang carries the same number for
// the shader; they are asserted equal in world.h.
constexpr float SUN_COS_THETA_MAX = 0.99998932f;  // cos(0.2665 deg)

class Sky {
  public:
    float turbidity = 2.6f;  // 2 is a clear alpine day; 6 is summer haze
    float sunScale = 1.0f;
    // Multipliers for tuning without a rebuild: the disc.s brightness, and the
    // light it casts. Separate because they trade against different things --
    // one against the phase being visible, the other against the wood being.
    float moonScale = 1.0f;
    float moonKeyScale = 1.0f;

    // THE SUN'S COLOUR, WHEN THE ATMOSPHERE PASS IS SUPPLYING IT.
    //
    // Negative means "not supplied" and the Kasten-Young fit below is used, so
    // the Preetham path is bit-for-bit what it was. When the atmosphere is on
    // the app fills this from Atmosphere::sunTransmittance() -- the same medium
    // the dome is integrated through -- because a sun reddened by one model
    // over a sky reddened by another disagrees most at sunset, which is exactly
    // when the key light is the warmest thing in the frame.
    Vec3 sunTransOverride{-1.0f, -1.0f, -1.0f};

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

    // ---- the moon ---------------------------------------------------------
    //
    // HUNG AT THE ANTI-SOLAR POINT. It is up exactly when the sun is down, sets
    // as the sun rises, and the two are never in the sky together. That is the
    // JS engine's arrangement and it is worth keeping: it makes "night" a
    // single well-defined thing rather than two bodies to reconcile.
    //
    // WHICH IS ALSO WHY THE PHASE CANNOT COME FROM ITS POSITION. Anti-solar
    // means the observer is always between sun and moon, so the geometry says
    // FULL, every night, forever. The phase is therefore its own clock and the
    // disc is shaded from a synthetic light swung by it -- see moonDisk in
    // Sky.slang. Synthetic is not a cheat here: it is the only way to get a
    // phase at all without moving the moon off the anti-solar point.
    //
    // EIGHT DAYS, not 29.5. A twenty-minute day makes a real synodic month just
    // under ten hours of play, so nobody would ever see the moon change. Eight
    // game-days is a phase every 2.7 hours -- slow enough to feel like weather,
    // fast enough that a long session sees more than one.
    static constexpr float MOON_PERIOD_DAYS = 8.0f;

    // How bright the full moon is against the sun. The real ratio is about
    // 1/400000; this is nothing like that, and deliberately. The night has to be
    // navigable and the tone map has already spent its range on daylight, so
    // this is a lit-surface intensity chosen to read, not a photometric one.
    // THIS IS A RADIANCE, NOT AN IRRADIANCE, and confusing the two is what made
    // the first attempt invisible. The moon.s SURFACE is bright -- sunlit rock
    // -- and it is the tiny cone it subtends that makes its total light small.
    // The NEE already handles the cone, so this has to be a surface brightness
    // comparable to a lit object, not the faint number moonlight adds up to.
    // THE DISC.S OWN RADIANCE, sized for the TONE CURVE rather than for physics:
    // a full moon lands just above the white point, so the phase shading still
    // has somewhere to go. At 400 the whole disc -- earthshine included -- sat
    // so far past white that a quarter moon rendered as a flat white circle,
    // which is the shading working perfectly and being clipped away.
    //
    // The KEY light is boosted off this separately in Sky.slang. They have to
    // differ: a daylight-calibrated ACES curve cannot hold a body bright enough
    // to light a wood AND show a gradient across its own face.
    static constexpr float MOON_FULL = 1.6f;

    // THE SUN'S IRRADIANCE IN WORKING UNITS, named because two things need it.
    // build() ramps it to zero across the horizon for the disc; the atmosphere
    // pass needs it UNRAMPED, because the extinction it computes for itself is
    // the physical version of that same fade and applying both would fade the
    // sun twice.
    static constexpr float SUN_IRRADIANCE = 22.0f;

    // What is left of the key at new moon. Near nothing, but NOT zero: a new
    // moon that switched the light off entirely would make the shadow rays
    // pointless and the wood unnavigable for a quarter of the cycle.
    static constexpr float MOON_LMIN = 0.06f;

    // 0 is full, 0.5 is new -- the same convention moonDisk shades with.
    //
    // Does NOT rebuild: the caller sets the phase and then the sun, and setSun
    // rebuilds once with both. Two rebuilds a frame for one changed number is
    // a Perez fit nobody asked for.
    void setMoonPhase(float days) {
        const float p = days / MOON_PERIOD_DAYS;
        phase_ = p - floorf(p);
    }

    float moonPhase() const { return phase_; }
    Vec3 moonDir() const { return Vec3(-sunDir_.x, -sunDir_.y, -sunDir_.z); }

  private:
    V6Sky g_{};
    Vec3 sunDir_{0.0f, 1.0f, 0.0f};
    float azDeg_ = 38.0f, elDeg_ = 24.0f;
    float phase_ = 0.0f;

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
        // Defaults for the two words the atmosphere borrowed. The tracer
        // patches both per frame -- neither is a function of the sun, so
        // neither should have to wait for a sun rebuild.
        g_.atmoViewHeight = kAtmoGroundR;
        g_.atmoOn = 0.0f;

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

        // ── AND BELOW THE HORIZON THE FIT IS WALKED OUT, NOT JUST DIMMED ─────
        //
        // Pinning thetaS above freezes the SHAPE of the sunset; skyScale then
        // changes only its BRIGHTNESS. Chromaticity is scale invariant, so what
        // survived the dimming was the sunset itself -- x 0.434, y 0.445, a
        // horizon fifteen times the zenith -- held at that exact hue at every
        // hour of the night. Midnight was a turned-down sunset, and the orange
        // band along the treeline was that, not a scattering artefact.
        //
        // So the coefficients are walked out over twilight rather than scaled:
        //
        //   A, B    the horizon gradient, toward a gentle 23% lift. Preetham's
        //           is worth about 15x, which is a daytime haze band.
        //   C, D, E the solar aureole, to nothing. It is centred on the sun and
        //           the sun is under the ground, so it was putting a glow around
        //           a point nobody can see.
        //   zenx,y  the chromaticity, toward a cool blue-grey.
        //
        // ENDS AT -12 DEGREES, the bottom of nautical twilight, which is about
        // where the last colour goes out of a real sky. The brightness ramp
        // above deliberately ends sooner, at -6: the last of the hue then
        // crossfades at an already-dim level instead of taking the visible part
        // of the transition with it.
        //
        // WHAT THIS IS NOT: a twilight WEDGE. Killing the aureole leaves a dome
        // with no azimuthal variation at all, so the glow cannot sit over the
        // point where the sun actually set, and there is no earth shadow
        // opposite it. Preetham has no term that could carry either -- its only
        // directional structure is the aureole, and that is anchored to a sun
        // below the horizon. A real wedge needs the scattering integral, which
        // is the argument for a sky-view LUT and not something a fit can be
        // talked into.
        const float night = sstep(saturate(-elDeg / 12.0f));
        if (night > 0.0f) {
            // CHOSEN, NOT DERIVED. There is no night in Preetham to fit
            // against, so this is a plausible cool blue-grey and nothing more
            // honest than that. A scattering LUT would compute it.
            constexpr float NIGHT_X = 0.26f, NIGHT_Y = 0.27f;
            // A mild horizon lift kept on purpose, so the dome does not read as
            // a painted ceiling. Real night skies do brighten a little toward
            // the horizon; they do not do it fifteenfold and they do not do it
            // in orange.
            constexpr float NIGHT_A_Y = -0.25f, NIGHT_B_Y = -0.30f;

            const float keep = 1.0f - night;
            g_.A.x = lerpf(g_.A.x, NIGHT_A_Y, night);
            g_.B.x = lerpf(g_.B.x, NIGHT_B_Y, night);
            g_.A.y *= keep;
            g_.A.z *= keep;
            g_.C.x *= keep; g_.C.y *= keep; g_.C.z *= keep;
            g_.D.x *= keep; g_.D.y *= keep; g_.D.z *= keep;
            g_.E.x *= keep; g_.E.y *= keep; g_.E.z *= keep;
            g_.zenith.y = lerpf(zenx, NIGHT_X, night);
            g_.zenith.z = lerpf(zeny, NIGHT_Y, night);
        }

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
        const Vec3 fitTrans = vexp(-(tau + mie * (turbidity - 1.0f)) * am);
        // The atmosphere pass wins when it has an answer. Everything above this
        // line still runs: turbidity, the air mass and the fit stay the sun's
        // colour whenever that pass is off, and the two paths differ in exactly
        // one expression rather than in two copies of the whole calculation.
        const Vec3 trans = sunTransOverride.x >= 0.0f ? sunTransOverride : fitTrans;

        // Irradiance in working units, converted to radiance over the disk.
        const float irradiance = SUN_IRRADIANCE * sunScale * saturate(sunDir_.y * 4.0f);
        const Vec3 rad = trans * (irradiance / maxf(1e-8f, solidAngle()));
        g_.sunRadiance = float3(rad.x, rad.y, rad.z);

        // -- the moon ------------------------------------------------------
        //
        // THE ILLUMINATED FRACTION IS (1 + cos alpha) / 2, the same quantity the
        // disc is shaded with, so the KEY light and the FACE always agree. A
        // crescent that lit the wood like a full moon was the thing this had to
        // avoid; the light corresponds to the phase because it is computed from
        // the phase.
        const float alpha = phase_ * TWO_PI;
        const float lit = 0.5f + 0.5f * cosf(alpha);
        const float key = MOON_LMIN + (1.0f - MOON_LMIN) * lit;
        // FADED ACROSS THE HORIZON CROSSING. The shader flips the key light
        // direction hard at sunDir.y = 0, which is correct -- the two bodies are
        // opposite and there is nothing meaningful between them -- and this is
        // what makes that flip invisible: the moon arrives as the sun leaves,
        // while neither is bright.
        float mf = (sunDir_.y - 0.02f) / -0.08f;
        mf = mf < 0.0f ? 0.0f : (mf > 1.0f ? 1.0f : mf);
        const float nightFade = mf * mf * (3.0f - 2.0f * mf);
        g_.moonDir = float3(-sunDir_.x, -sunDir_.y, -sunDir_.z);
        g_.moonPhase = phase_;
        // Cool white: moonlight is sunlight off a grey rock, and it reads cold
        // because the eye is dark-adapted, not because the spectrum is blue.
        const float mr = MOON_FULL * moonScale * key * nightFade;
        g_.moonRadiance = float3(0.94f * mr, 0.95f * mr, 1.00f * mr);
        // THE KEY, AND IT HAS TO BE THIS LARGE. At a boost of 40 the moon put
        // 0.098 of irradiance on the ground against the noon sun.s 22 -- which
        // is already 1800x real moonlight, and still landed the forest floor at
        // about one part in 255 after ACES. Computing light nobody can see is
        // the same as computing none. 300 puts it where a dark-adapted eye
        // would put it rather than where a photometer would.
        g_.moonKey = 300.0f * moonKeyScale;
    }
};

}  // namespace v7
