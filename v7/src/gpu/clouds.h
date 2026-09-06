// ---------------------------------------------------------------------------
// clouds.h -- the cloud density cache: one 3D texture, filled once, then left
//             alone.
//
// The graph is in shaders/Clouds.slang and the fill is CloudFill. This file
// owns the memory and the fill schedule, and the schedule is the interesting
// part because it is almost nothing: the cache is a TILE of an endless deck, so
// it never has to be refilled for movement, and the deck has no evolution clock
// so it never has to be refilled for time either. Wind is a lookup offset in
// the march. Once every slice has been written this class stops dispatching.
//
// ---------------------------------------------------------------------------
// WHY A BAND OF SLICES A FRAME.
//
// The whole volume is 256 x 32 x 256 = two million texels, and each one runs a
// fractal Voronoi graph that visits 27 cells an octave. In one dispatch that is
// a visible hitch on the frame the world finishes streaming -- which is exactly
// the frame that can least afford one. Spread over a few frames it is invisible,
// and because the fill is idempotent there is no cost to it taking its time.
//
// At 4 MB in R16Float the cache is a sixth of what the fog volume costs, and it
// buys a sky that a formula cannot produce at all: Sky.slang is Preetham, a
// closed-form clear-sky model with no volume in it anywhere.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Sampler.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"

#include <cstdint>
#include <string>

namespace v7 {

struct CloudVol {
    // MUST MATCH nothing in the shader -- the shader is told the dimensions.
    // The proportions are not arbitrary: x and z span one 4096 m tile, so 256
    // texels is 16 m each, and y spans the 320 m deck, so 32 texels is 10 m
    // each. Roughly cubic texels, which is what keeps a cloud the same shape
    // horizontally and vertically.
    static constexpr uint32_t kX = 256;
    static constexpr uint32_t kY = 32;
    static constexpr uint32_t kZ = 256;
    // How many y slices per frame. Eight frames to fill at this rate.
    static constexpr uint32_t kSlicesPerFrame = 4;
};

class Clouds {
  public:
    bool enabled = true;

    // How fast the deck drifts. The march turns this into a lookup offset, so
    // it costs nothing and the cache never notices.
    float windSpeed = 1.0f;

    // How bright the deck is against this renderer's illuminant.
    //
    // NOT the source's 17. That figure is against a white sun of 1.0 and a
    // Reinhard tonemap; here the illuminant is the Preetham fit through ACES,
    // so carrying it over would clip the whole deck to white.
    float sunStrength = 2.2f;
    float ambStrength = 0.55f;

    // WHAT THE MOON IS WORTH TO THE DECK. Separate from sky.h's moonKey, which
    // is 300 because a surface needs that much to read through an ACES curve
    // built for daylight. The deck is lit directly rather than through NEE, and
    // at 300 it came out about 550x the sky behind it -- a sunlit cloud over a
    // midnight sky. 16 puts a moonlit cloud a few times its own sky, which is
    // roughly what the eye expects and still leaves the deck legible at night.
    float moonStrength = 16.0f;

    // HOW MUCH OF THE SKY IS CLOUD. 0.45 is the measured median of the field the
    // JS version probed -- about half the deck. Because the cut renormalises,
    // moving it changes how MUCH sky is cloud without changing how solid that
    // cloud is. Raising it thins toward scattered; lowering it merges cells
    // into overcast.
    float cut = 0.45f;

    // HOW FAR COVERAGE VARIES FROM REGION TO REGION, which is what puts weather
    // in the sky instead of an even scatter everywhere. It ADDS to the
    // per-cloud spread rather than replacing it, so a sparse region still has
    // big and small clouds in it -- the two spreads are about different things.
    float regVar = 0.26f;

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

        // THE TEXTURE IS CREATED BEFORE THE SHADER, and even if the shader
        // fails. The tracer DECLARES this volume, and an unbound declared
        // resource fails the dispatch rather than the branch that reads it --
        // so a missing cloud pass has to leave a bindable, empty volume behind
        // rather than nothing at all. `filled()` is what stops the march
        // reading it.
        using Falcor::ResourceBindFlags;
        vol_ = device_->createTexture3D(
            CloudVol::kX, CloudVol::kY, CloudVol::kZ, Falcor::ResourceFormat::R16Float, 1, nullptr,
            ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess);
        vol_->setName("v7::cloudVolume");

        // LINEAR AND WRAPPED, and the wrap is not a detail. The march samples
        // at frac(world / tile), so the texture IS the tile: clamping would
        // pin the edge texel across the whole sky instead of repeating the
        // deck, and point sampling would show 16 m texels as blocks.
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear, Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Linear);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Wrap,
                             Falcor::TextureAddressingMode::Wrap,
                             Falcor::TextureAddressingMode::Wrap);
        sampler_ = device_->createSampler(sd);

        try {
            fill_ = Falcor::ComputePass::create(device_, "v7/shaders/CloudFill.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("cloud shader did not compile: ") + e.what();
            return false;
        }

        ready_ = true;
        status_ = "256x32x256 density cache";
        return true;
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled; }
    const std::string &status() const { return status_; }
    const Falcor::ref<Falcor::Texture> &volume() const { return vol_; }
    const Falcor::ref<Falcor::Sampler> &sampler() const { return sampler_; }

    // Whether the cache has anything in it yet. The march must not run against
    // a volume that is still half zeroes -- it would show as the deck growing
    // in from one end over the first few frames.
    bool filled() const { return nextSlice_ >= CloudVol::kY; }

    // Either knob above changes what the graph produces, so the cache no longer
    // describes the sky it was filled for and every slice has to be written
    // again. Cheap -- the fill is a few frames.
    void rebuild() { nextSlice_ = 0; }

    // The wind offset the march applies, in the same clock the day cycle uses.
    float windT() const { return windT_; }
    void advance(float dt) { windT_ += dt * windSpeed; }

    // ---------------------------------------------------------------------
    // Fill the next band. Does nothing once the volume is complete, which is
    // the normal state after the first handful of frames.
    // ---------------------------------------------------------------------
    void update(Falcor::RenderContext *ctx) {
        if (!ready_ || filled()) return;

        const uint32_t begin = nextSlice_;
        const uint32_t count = std::min(CloudVol::kSlicesPerFrame, CloudVol::kY - begin);

        auto var = fill_->getRootVar();
        var["gCloudVol"].setTexture(vol_);
        var["CloudFillCB"]["gDim"] = Falcor::uint3(CloudVol::kX, CloudVol::kY, CloudVol::kZ);
        var["CloudFillCB"]["gSliceBegin"] = begin;
        var["CloudFillCB"]["gSliceCount"] = count;
        var["CloudFillCB"]["gCut"] = cut;
        var["CloudFillCB"]["gRegVar"] = regVar;
        fill_->execute(ctx, CloudVol::kX, CloudVol::kZ, count);
        ctx->uavBarrier(vol_.get());

        nextSlice_ = begin + count;
    }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> fill_;
    Falcor::ref<Falcor::Texture> vol_;
    Falcor::ref<Falcor::Sampler> sampler_;
    uint32_t nextSlice_ = 0;
    float windT_ = 0.0f;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
