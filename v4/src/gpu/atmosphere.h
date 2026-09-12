// ---------------------------------------------------------------------------
// atmosphere.h -- the host half of a Hillaire sky, and the schedule that makes
//                 it cost nothing.
//
// Three tables, three completely different lifetimes, and getting that split
// right is most of why this is affordable:
//
//   TRANSMITTANCE   256x64   baked ONCE, ever. Depends only on the medium.
//   MULTISCATTER    32x32    baked ONCE, ever. Same reason.
//   SKY-VIEW        192x108  rebuilt when the SUN HAS MOVED far enough to see.
//
// Only the last one has any per-frame cost at all, it is 20k threads, and it is
// skipped entirely on the great majority of frames -- see kSunEpsilon below.
// Standing still at noon this whole system runs zero dispatches.
//
// ---------------------------------------------------------------------------
// WHAT IT REPLACES, AND WHAT IT DOES NOT.
//
// It replaces skyDome(): the dome the tracer sees on every escaping ray, which
// is most of the light in a wood. It does NOT replace the moon, the cloud deck,
// or the fog volume -- those keep their own models and simply read the new dome
// where they used to read the old one.
//
// The sun's own colour DOES come from here when the atmosphere is on, via
// sunTransmittance(). Leaving it on Preetham's air-mass fit would have put a
// sun reddened by one model over a sky reddened by another, and at sunset the
// key light is the warmest thing in the frame -- a disagreement there is not
// subtle.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Sampler.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"

#include <cmath>
#include <string>

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"

namespace v4 {

class Atmosphere {
  public:
    // ON BY DEFAULT since 2026-09-06, by explicit decision rather than by
    // drift. The real default lives in defaults::kAtmosphere and app.h assigns
    // it here, so the menu, the command line and a "Bake as default" all agree
    // about one value.
    //
    // WHAT THAT COSTS, stated because it is not nothing: every screenshot and
    // every tuning decision made before this date was made against Preetham,
    // and the two models disagree most at exactly the hours people photograph.
    // The old sky has not been deleted -- it is --no-atmosphere, or the menu --
    // and a fallback is still what runs if these shaders fail to compile.
    bool enabled = true;

    // The one artistic number in the file, and it is here because the physics
    // genuinely stops. A Hillaire atmosphere models sunlight and nothing else,
    // so with the sun 18 degrees down it returns zero -- which is correct, and
    // which is a black screen. Real nights are not black: airglow, starlight,
    // zodiacal light and scattered moonlight all sit underneath, and none of
    // them is in this model.
    //
    // The default matches what the Preetham path's 2% floor already produced
    // (about 5.4e-3 at the zenith), so switching the atmosphere on does not
    // change how dark the wood gets at midnight -- only what colour the sky is
    // on the way there.
    float nightFloor = 5.4e-3f;

    // Cool and desaturated, the same blue-grey the fit is walked toward below
    // the horizon in sky.h, so the two paths agree at deep night and only
    // differ through twilight -- which is the part worth comparing.
    Vec3 nightTint{0.42f, 0.60f, 1.00f};

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

        // THE TEXTURES ARE CREATED BEFORE THE PASSES, and the order is load
        // bearing. Trace.cs.slang and ProbeTrace.cs.slang both DECLARE the
        // sky-view table, and an unbound declared resource fails the entire
        // dispatch rather than the branch that would have read it. So even a
        // total shader-compile failure here has to leave something bindable
        // behind: the feature then reports unavailable, atmoOn stays 0, and the
        // renderer carries on with the Perez fit instead of dying.
        using Falcor::ResourceBindFlags;
        const auto kRw = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

        // RGBA32Float for the two baked tables. They are tiny -- 256 KB and
        // 4 KB -- and they are read by everything downstream, so a half-float
        // quantisation here would be paid for at every sample. The sky-view
        // table is 16-bit because it IS the sample: 192x108 read bilinearly per
        // ray, where bandwidth is the whole cost.
        trans_ = device_->createTexture2D(kAtmoTransW, kAtmoTransH,
                                          Falcor::ResourceFormat::RGBA32Float, 1, 1, nullptr, kRw);
        trans_->setName("v4::atmoTransmittance");
        multi_ = device_->createTexture2D(kAtmoMultiW, kAtmoMultiH,
                                          Falcor::ResourceFormat::RGBA32Float, 1, 1, nullptr, kRw);
        multi_->setName("v4::atmoMultiScatter");
        sky_ = device_->createTexture2D(kAtmoSkyW, kAtmoSkyH, Falcor::ResourceFormat::RGBA16Float,
                                        1, 1, nullptr, kRw);
        sky_->setName("v4::atmoSkyView");

        // LINEAR AND CLAMPED, and the clamp is load bearing on the zenith axis:
        // the sky-view mapping runs from straight up to straight down with no
        // wrap, so a wrapped tap at the top row would fetch the ground. The
        // azimuth axis is symmetric about the sun rather than periodic -- it
        // stores the angle TO the sun, not around the compass -- so it does not
        // want wrapping either.
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear, Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Point);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp);
        sampler_ = device_->createSampler(sd);

        try {
            transPass_ = Falcor::ComputePass::create(
                device_, "v4/shaders/AtmoTransmittance.cs.slang", "main");
            multiPass_ = Falcor::ComputePass::create(
                device_, "v4/shaders/AtmoMultiScatter.cs.slang", "main");
            skyPass_ =
                Falcor::ComputePass::create(device_, "v4/shaders/AtmoSkyView.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("atmosphere shaders did not compile: ") + e.what();
            return false;
        }

        ready_ = true;
        status_ = "Hillaire LUTs 256x64 / 32x32 / 192x108";
        return true;
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled && built_; }
    const std::string &status() const { return status_; }

    const Falcor::ref<Falcor::Texture> &skyView() const { return sky_; }
    const Falcor::ref<Falcor::Texture> &transmittance() const { return trans_; }
    const Falcor::ref<Falcor::Sampler> &sampler() const { return sampler_; }

    // The viewer's radius, which Sky.slang needs to undo the same mapping this
    // built. In kilometres from the planet centre.
    float viewHeightKm() const { return viewHeight_; }

    // Force the sky-view table to rebuild next update, for the parameters that
    // are not the sun. Does NOT clear built_: dropping that would make active()
    // false for one frame and flash the Perez sky between two atmosphere frames.
    void invalidate() { lastSun_ = Vec3(0.0f, -2.0f, 0.0f); }

    // -----------------------------------------------------------------------
    // Called once a frame. Does nothing at all unless something moved.
    void update(Falcor::RenderContext *ctx, const V6Sky &sky, float camY, float solarIrradiance) {
        if (!ready_ || !enabled) return;

        // The two static tables, on the first frame the feature is switched on
        // and never again.
        if (!baked_) {
            auto tv = transPass_->getRootVar();
            tv["gTransmittance"].setTexture(trans_);
            transPass_->execute(ctx, kAtmoTransW, kAtmoTransH, 1);
            ctx->uavBarrier(trans_.get());

            auto mv = multiPass_->getRootVar();
            mv["gTransmittance"].setTexture(trans_);
            mv["gLutSamp"] = sampler_;
            mv["gMultiScatter"].setTexture(multi_);
            multiPass_->execute(ctx, kAtmoMultiW, kAtmoMultiH, 1);
            ctx->uavBarrier(multi_.get());

            baked_ = true;
        }

        const float vh = kAtmoGroundR + std::fmax(camY, 1.0f) * 0.001f;

        // HAS ANYTHING CHANGED ENOUGH TO SEE. The sky-view table is a function
        // of the sun direction and the viewer's altitude, and the altitude of
        // somebody walking in a wood does not matter at all -- a 30 m climb
        // moves the horizon by about a hundredth of a degree. So the test is
        // really about the sun, and kSunEpsilon is roughly a fifth of the
        // table's own azimuth resolution: below that a rebuild cannot change a
        // texel, so it would be a dispatch that provably does nothing.
        const Vec3 sd(sky.sunDir.x, sky.sunDir.y, sky.sunDir.z);
        const float moved = std::fabs(sd.x - lastSun_.x) + std::fabs(sd.y - lastSun_.y) +
                            std::fabs(sd.z - lastSun_.z);
        const bool irradianceMoved = std::fabs(solarIrradiance - lastIrradiance_) > 1e-4f;
        if (built_ && moved < kSunEpsilon && !irradianceMoved) return;

        // The night term, faded in as the physical model runs out. Full by -12
        // degrees, which is the bottom of nautical twilight and about where a
        // real sky stops changing.
        const float elDeg = std::asin(clampf(sd.y, -1.0f, 1.0f)) * 180.0f / PI;
        const float t = saturate(-elDeg / 12.0f);
        const float nf = nightFloor * (t * t * (3.0f - 2.0f * t));

        auto sv = skyPass_->getRootVar();
        sv["gTransmittance"].setTexture(trans_);
        sv["gMultiScatter"].setTexture(multi_);
        sv["gLutSamp"] = sampler_;
        sv["gSkyView"].setTexture(sky_);
        sv["AtmoSkyCB"]["gSunDir"] = sky.sunDir;
        sv["AtmoSkyCB"]["gViewHeight"] = vh;
        sv["AtmoSkyCB"]["gSolarIrradiance"] =
            float3(solarIrradiance, solarIrradiance, solarIrradiance);
        sv["AtmoSkyCB"]["gNightSky"] =
            float3(nightTint.x * nf, nightTint.y * nf, nightTint.z * nf);
        skyPass_->execute(ctx, kAtmoSkyW, kAtmoSkyH, 1);
        ctx->uavBarrier(sky_.get());

        lastSun_ = sd;
        lastIrradiance_ = solarIrradiance;
        viewHeight_ = vh;
        built_ = true;
    }

    // -----------------------------------------------------------------------
    // THE SUN'S OWN COLOUR, through the same air the dome is made of.
    //
    // On the CPU rather than read back from the transmittance table, because
    // the one number the host needs is one ray out of 16384 and a GPU readback
    // would cost a pipeline stall to fetch it. Sixty-four steps of the same
    // integrand the bake runs, which is well under a microsecond.
    //
    // KASTEN AND YOUNG DOES NOT APPEAR HERE, and its absence is the point. The
    // Preetham path needs an air-mass approximation because it has no geometry
    // to march; this has the geometry, so the horizon case that formula exists
    // to rescue is just a longer ray.
    Vec3 sunTransmittance(const Vec3 &sunDir) const {
        const float r = kAtmoGroundR + 0.001f;
        const Vec3 o(0.0f, r, 0.0f);
        const Vec3 d = normalize(sunDir);

        // Below the horizon there is no direct sun at all. The moon path in
        // sky.h takes over from here and does not come through this function.
        if (d.y <= 0.0f) return Vec3(0.0f, 0.0f, 0.0f);

        const float tTop = raySphere(o, d, kAtmoTopR);
        if (tTop <= 0.0f) return Vec3(1.0f, 1.0f, 1.0f);

        const int steps = 64;
        const float dt = tTop / float(steps);
        Vec3 od(0.0f, 0.0f, 0.0f);
        for (int i = 0; i < steps; ++i) {
            const float t = (float(i) + 0.5f) * dt;
            const Vec3 p = o + d * t;
            const float h = length(p) - kAtmoGroundR;
            od = od + extinctionAt(h) * dt;
        }
        return Vec3(std::exp(-od.x), std::exp(-od.y), std::exp(-od.z));
    }

  private:
    // Below this the sun has not turned far enough for the table to differ in
    // any texel. The sky-view azimuth axis is 192 wide over half a turn, so one
    // texel is a little under a degree; this is about a fifth of that.
    static constexpr float kSunEpsilon = 0.003f;

    // The same medium Atmosphere.slang declares. DUPLICATED, and it has to be:
    // that file compiles as Slang only, and this one as C++ only. The engine
    // makes this trade in several places -- kVoxelM, the material ids -- and
    // guards it with a static_assert where it can. Here it cannot, so the rule
    // is simply that these two blocks move together.
    static Vec3 extinctionAt(float h) {
        const float hh = h < 0.0f ? 0.0f : h;
        const float dR = std::exp(-hh / 8.0f);
        const float dM = std::exp(-hh / 1.2f);
        float dO = 1.0f - std::fabs(hh - 25.0f) / 15.0f;
        dO = dO < 0.0f ? 0.0f : (dO > 1.0f ? 1.0f : dO);
        const Vec3 rayleigh(5.802e-3f * dR, 13.558e-3f * dR, 33.100e-3f * dR);
        const float mie = (3.996e-3f + 4.400e-3f) * dM;
        const Vec3 ozone(0.650e-3f * dO, 1.881e-3f * dO, 0.085e-3f * dO);
        return Vec3(rayleigh.x + mie + ozone.x, rayleigh.y + mie + ozone.y,
                    rayleigh.z + mie + ozone.z);
    }

    static float raySphere(const Vec3 &o, const Vec3 &d, float R) {
        const float b = dot(o, d);
        const float c = dot(o, o) - R * R;
        if (c > 0.0f && b > 0.0f) return -1.0f;
        const float disc = b * b - c;
        if (disc < 0.0f) return -1.0f;
        const float s = std::sqrt(disc);
        const float t0 = -b - s, t1 = -b + s;
        if (t1 < 0.0f) return -1.0f;
        return t0 < 0.0f ? t1 : t0;
    }

    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> transPass_, multiPass_, skyPass_;
    Falcor::ref<Falcor::Texture> trans_, multi_, sky_;
    Falcor::ref<Falcor::Sampler> sampler_;

    bool ready_ = false;
    bool baked_ = false;  // the two static tables exist
    bool built_ = false;  // the sky-view table has been built at least once
    Vec3 lastSun_{0.0f, -2.0f, 0.0f};
    float lastIrradiance_ = -1.0f;
    float viewHeight_ = kAtmoGroundR;
    std::string status_ = "not initialised";
};

}  // namespace v4
