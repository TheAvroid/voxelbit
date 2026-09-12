// ---------------------------------------------------------------------------
// post.h -- the two things that happen between the traced image and the curve.
//
// AUTO-EXPOSURE and BLOOM, in that order, and the order is not arbitrary: the
// bloom threshold is taken in exposure-scaled units, so the stop has to be
// known before the highlights can be separated from everything else. Get it the
// other way round and the bloom appears and disappears as the sun moves, which
// is the failure a threshold in absolute radiance always has in a scene whose
// illumination spans four orders of magnitude.
//
// WHY THIS IS ONE FILE AND NOT TWO. They share a resolution, a place in the
// frame, and -- through that exposure value -- a number. Splitting them would
// mean two modules that must be resized together, dispatched in order, and
// wired to each other through the tracer, which is three chances to get the
// sequence wrong for no gain.
//
// NEITHER IS IN THE SETTINGS MENU. Both are command-line and bake settings
// (--auto-exposure, --exposure-key, --bloom, --bloom-threshold), off by
// default. That is a deliberate UI decision, not a statement about how finished
// they are -- see the note where the menu used to carry them.
//
// ---------------------------------------------------------------------------
// WHERE THIS SITS: inside Tracer::resolve, before the tone-map dispatch.
//
// That is deliberate and it is the only correct place, because resolve() is the
// ONE function both the interactive path and renderOffline call. app.h carries
// a long-standing note that renderOffline skips every per-frame system -- the
// fog grid rendered black that way once, and the atmosphere rendered the wrong
// model -- so anything hung off onFrameRender would be missing from every
// --out render and present in every screenshot, which is the worst of both.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/Pass/ComputePass.h"

#include <algorithm>
#include <string>
#include <vector>

namespace v4 {

// How many levels the bloom chain gets, at most. Six halvings take a 4K frame
// down to about 60x31, which is a skirt spanning roughly a third of the screen
// -- past that the level is a handful of texels and contributes a flat wash
// rather than a shape.
static constexpr int kBloomMaxMips = 6;

// The smallest level worth having. Below this the 13-tap downsample's outer
// ring is sampling outside the texture at every texel and the clamp addressing
// turns the level into a smeared edge.
static constexpr int kBloomMinExtent = 8;

class Post {
  public:
    // -- auto-exposure -------------------------------------------------------
    bool autoExposure = false;

    // The luminance the middle of the frame is aimed at, after trimming. 0.18
    // is the photographic middle grey and it is the right starting point, but
    // this engine's tone curve has a lifted toe (see Tonemap.cs.slang) which
    // already brightens the low end, so a slightly lower key holds the wood at
    // the density it was tuned at.
    float expKey = 0.14f;

    // Seconds-scale rates, not per-frame fractions. Brightening is roughly
    // three times faster than darkening, which is the direction real eyes work
    // in and the direction that feels right walking out of trees into a
    // clearing.
    float expSpeedUp = 3.0f;
    float expSpeedDown = 1.0f;

    // The band the stop may move within. This is the safety rail that keeps a
    // frame of near-black -- eyes shut in a canopy at night -- from being
    // amplified into a wall of noise.
    float expMin = 0.15f;
    float expMax = 6.0f;

    // What fraction of the frame is discarded at each end before the mean. Half
    // the frame off the bottom sounds drastic and is not: a conifer wood at
    // ground level really is more than half shadow, and including it drags the
    // measurement down until the sky blows out. The 2 % off the top is the sun.
    float expLowCut = 0.50f;
    float expHighCut = 0.02f;

    // The histogram's window in log2 luminance. -10 is about a thousandth of
    // middle grey and +12 is four thousand times it; the solar disk is far above
    // the top and lands in the last bin, where the high cut throws it away.
    float expMinLog = -10.0f;
    float expMaxLog = 12.0f;

    // -- bloom ---------------------------------------------------------------
    //
    // Strength 0 skips the entire chain -- no dispatches, no allocation cost
    // paid at run time -- so this doubles as the switch.
    float bloom = 0.0f;
    float bloomThreshold = 1.0f;
    float bloomKnee = 0.6f;
    float bloomRadius = 1.0f;

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }

    bool bloomActive() const { return ready_ && bloom > 0.0f && !chain_.empty(); }
    bool exposureActive() const { return ready_ && autoExposure; }

    const Falcor::ref<Falcor::Texture> &bloomTexture() const {
        return chain_.empty() ? null_ : chain_[0];
    }
    const Falcor::ref<Falcor::Buffer> &exposureState() const { return expState_; }

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        try {
            using Falcor::ResourceBindFlags;
            const ResourceBindFlags rw =
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

            // 256 bins, and two floats of state. The state buffer is READ by
            // the tone map and the bloom prefilter on every frame whether or
            // not auto-exposure is on, because an unbound resource is a
            // validation error even inside a branch that never executes -- so
            // it is allocated unconditionally and the shaders gate on a flag.
            histogram_ = device_->createStructuredBuffer(uint32_t(sizeof(uint32_t)), 256u, rw);
            histogram_->setName("v4::expHistogram");
            expState_ = device_->createStructuredBuffer(uint32_t(sizeof(float)), 2u, rw);
            expState_->setName("v4::expState");

            // Five passes out of two files. Every one is the same shader
            // library with a different entry point, which is what keeps the
            // filters and their constant buffer in one place per concern
            // rather than one place per dispatch.
            expHistogram_ =
                Falcor::ComputePass::create(device_, "v4/shaders/Exposure.cs.slang", "histogram");
            expResolve_ =
                Falcor::ComputePass::create(device_, "v4/shaders/Exposure.cs.slang", "resolve");
            bloomPre_ =
                Falcor::ComputePass::create(device_, "v4/shaders/Bloom.cs.slang", "prefilter");
            bloomDown_ = Falcor::ComputePass::create(device_, "v4/shaders/Bloom.cs.slang", "down");
            bloomUp_ = Falcor::ComputePass::create(device_, "v4/shaders/Bloom.cs.slang", "up");

            // Bilinear, clamped. Bilinear because the whole chain is built out
            // of taps at half-texel offsets and point sampling would turn the
            // 13-tap filter into a 13-tap box; clamped because a wrapping mode
            // makes the top of the sky bloom into the forest floor.
            Falcor::Sampler::Desc sd;
            sd.setFilterMode(Falcor::TextureFilteringMode::Linear,
                             Falcor::TextureFilteringMode::Linear,
                             Falcor::TextureFilteringMode::Point);
            sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                                 Falcor::TextureAddressingMode::Clamp,
                                 Falcor::TextureAddressingMode::Clamp);
            sampler_ = device_->createSampler(sd);
        } catch (const std::exception &e) {
            status_ = std::string("post passes failed: ") + e.what();
            return false;
        }
        ready_ = true;
        cleared_ = false;
        status_ = "auto-exposure + bloom";
        return true;
    }

    // The chain is allocated at DISPLAY resolution, because that is what the
    // tone map reads: on the Super Resolution route the frame has already been
    // upscaled by the time it gets here, and blooming the pre-upscale image
    // would put a half-resolution skirt on a full-resolution picture.
    void resize(int w, int h) {
        if (!ready_) return;
        w = std::max(8, w);
        h = std::max(8, h);
        if (w == w_ && h == h_) return;
        w_ = w;
        h_ = h;
        chain_.clear();

        const Falcor::ResourceBindFlags rw =
            Falcor::ResourceBindFlags::ShaderResource | Falcor::ResourceBindFlags::UnorderedAccess;
        int lw = w / 2, lh = h / 2;
        for (int i = 0; i < kBloomMaxMips; ++i) {
            if (lw < kBloomMinExtent || lh < kBloomMinExtent) break;
            auto t = device_->createTexture2D(uint32_t(lw), uint32_t(lh),
                                              Falcor::ResourceFormat::RGBA16Float, 1, 1, nullptr,
                                              rw);
            t->setName(("v4::bloom" + std::to_string(i)).c_str());
            chain_.push_back(t);
            lw /= 2;
            lh /= 2;
        }
        // A resize throws away everything the adaptation knew: the frame is a
        // different size, so the histogram it was measured from describes a
        // picture that no longer exists.
        reset_ = true;
    }

    // Throw away the adaptation state -- a teleport, a jump in the day clock,
    // the first frame. The next measurement is taken as the answer instead of
    // being eased toward.
    void reset() { reset_ = true; }

    // -----------------------------------------------------------------------
    // One frame's worth. `src` is the linear HDR image the tone map is about to
    // read, at display resolution; `manualExposure` is the slider's value, which
    // the auto stop multiplies rather than replaces so the control still means
    // something with adaptation on.
    // -----------------------------------------------------------------------
    void run(Falcor::RenderContext *ctx, const Falcor::ref<Falcor::Texture> &src, uint32_t dimX,
             uint32_t dimY, float manualExposure, float dt) {
        if (!ready_ || !src) return;
        if (!cleared_) {
            ctx->clearUAV(histogram_->getUAV().get(), Falcor::uint4(0));
            ctx->clearUAV(expState_->getUAV().get(), Falcor::uint4(0));
            cleared_ = true;
            reset_ = true;
        }

        if (autoExposure) {
            const float minLog = expMinLog;
            const float range = std::max(1e-3f, expMaxLog - expMinLog);

            {
                auto var = expHistogram_->getRootVar();
                var["gSrc"] = src;
                var["gHistogram"] = histogram_;
                var["gExposureState"] = expState_;
                setExpConstants(var, dimX, dimY, minLog, range, dt);
                expHistogram_->execute(ctx, dimX, dimY);
            }
            // EXPLICIT, because Falcor does not insert this one. Its state
            // tracking handles a transition -- UAV to shader-resource, say --
            // but two dispatches that both hold the same buffer as a UAV are
            // already in the state it wants, so nothing is emitted and the
            // resolve is free to read bins the histogram has not finished
            // writing. sharc.h barriers by hand for exactly this reason.
            ctx->uavBarrier(histogram_.get());
            {
                auto var = expResolve_->getRootVar();
                var["gSrc"] = src;
                var["gHistogram"] = histogram_;
                var["gExposureState"] = expState_;
                setExpConstants(var, dimX, dimY, minLog, range, dt);
                // ONE GROUP. The reduction is over 256 bins in shared memory,
                // so a second group would produce a second, wrong answer rather
                // than help with the first.
                expResolve_->execute(ctx, 256u, 1u);
            }
            // And again before anything reads the stop: the bloom prefilter
            // below and the tone map after it both sample this buffer, and both
            // would otherwise be racing the write above.
            ctx->uavBarrier(expState_.get());
            reset_ = false;
        } else {
            // With adaptation off the state is stale by definition, so the next
            // time it comes on it must not ease out of whatever was left there.
            reset_ = true;
        }

        if (!bloomActive()) return;

        const uint32_t useAuto = autoExposure ? 1u : 0u;
        const int levels = int(chain_.size());

        // Prefilter: display resolution down to half.
        {
            auto var = bloomPre_->getRootVar();
            var["gIn"] = src;
            var["gSamp"] = sampler_;
            var["gOut"] = chain_[0];
            var["gExposureState"] = expState_;
            setBloomConstants(var, chain_[0]->getWidth(), chain_[0]->getHeight(), dimX, dimY,
                              manualExposure, useAuto);
            bloomPre_->execute(ctx, chain_[0]->getWidth(), chain_[0]->getHeight());
        }

        for (int i = 1; i < levels; ++i) {
            auto var = bloomDown_->getRootVar();
            var["gIn"] = chain_[i - 1];
            var["gSamp"] = sampler_;
            var["gOut"] = chain_[i];
            var["gExposureState"] = expState_;
            setBloomConstants(var, chain_[i]->getWidth(), chain_[i]->getHeight(),
                              chain_[i - 1]->getWidth(), chain_[i - 1]->getHeight(),
                              manualExposure, useAuto);
            bloomDown_->execute(ctx, chain_[i]->getWidth(), chain_[i]->getHeight());
        }

        // And back up, ADDING into each larger level. Bottom-up, so by the time
        // level 0 is written every wider scale is already folded into level 1.
        for (int i = levels - 1; i > 0; --i) {
            auto var = bloomUp_->getRootVar();
            var["gIn"] = chain_[i];
            var["gSamp"] = sampler_;
            var["gOut"] = chain_[i - 1];
            var["gExposureState"] = expState_;
            setBloomConstants(var, chain_[i - 1]->getWidth(), chain_[i - 1]->getHeight(),
                              chain_[i]->getWidth(), chain_[i]->getHeight(), manualExposure,
                              useAuto);
            bloomUp_->execute(ctx, chain_[i - 1]->getWidth(), chain_[i - 1]->getHeight());
        }
    }

  private:
    template <typename Var>
    void setExpConstants(Var &var, uint32_t dimX, uint32_t dimY, float minLog, float range,
                         float dt) {
        var["gExpCB"]["gDim"] = Falcor::uint2(dimX, dimY);
        var["gExpCB"]["gMinLog"] = minLog;
        var["gExpCB"]["gLogRange"] = range;
        var["gExpCB"]["gDt"] = dt;
        var["gExpCB"]["gSpeedUp"] = expSpeedUp;
        var["gExpCB"]["gSpeedDown"] = expSpeedDown;
        var["gExpCB"]["gKey"] = expKey;
        var["gExpCB"]["gMinExposure"] = expMin;
        var["gExpCB"]["gMaxExposure"] = expMax;
        var["gExpCB"]["gLowCut"] = expLowCut;
        var["gExpCB"]["gHighCut"] = expHighCut;
        var["gExpCB"]["gReset"] = reset_ ? 1u : 0u;
        var["gExpCB"]["gExpPad0"] = 0.0f;
        var["gExpCB"]["gExpPad1"] = 0.0f;
    }

    template <typename Var>
    void setBloomConstants(Var &var, uint32_t outW, uint32_t outH, uint32_t inW, uint32_t inH,
                           float exposure, uint32_t useAuto) {
        var["gBloomCB"]["gOutDim"] = Falcor::uint2(outW, outH);
        var["gBloomCB"]["gInTexel"] =
            Falcor::float2(1.0f / float(std::max(1u, inW)), 1.0f / float(std::max(1u, inH)));
        var["gBloomCB"]["gExposure"] = exposure;
        var["gBloomCB"]["gUseAuto"] = useAuto;
        var["gBloomCB"]["gThreshold"] = bloomThreshold;
        var["gBloomCB"]["gKnee"] = bloomKnee;
        var["gBloomCB"]["gUpRadius"] = bloomRadius;
        var["gBloomCB"]["gBloomPad0"] = 0.0f;
        var["gBloomCB"]["gBloomPad1"] = 0.0f;
        var["gBloomCB"]["gBloomPad2"] = 0.0f;
    }

    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> expHistogram_, expResolve_;
    Falcor::ref<Falcor::ComputePass> bloomPre_, bloomDown_, bloomUp_;
    Falcor::ref<Falcor::Buffer> histogram_, expState_;
    Falcor::ref<Falcor::Sampler> sampler_;
    std::vector<Falcor::ref<Falcor::Texture>> chain_;
    Falcor::ref<Falcor::Texture> null_;

    int w_ = 0, h_ = 0;
    bool ready_ = false;
    bool cleared_ = false;
    bool reset_ = true;
    std::string status_ = "not initialised";
};

}  // namespace v4
