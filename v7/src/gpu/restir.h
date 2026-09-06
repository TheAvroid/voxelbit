// ---------------------------------------------------------------------------
// restir.h -- the reservoirs, the two resampling passes, and the buffers they
//             ping-pong between.
//
// The algorithm is in shaders/Restir.slang and the passes are RestirTemporal
// and RestirSpatial; this file owns the memory and the order.
//
// ---------------------------------------------------------------------------
// FOUR RESERVOIR BUFFERS, AND EACH ONE EARNS ITS PLACE.
//
//   candidate    what the tracer found THIS frame, one bounce per pixel
//   temporal[2]  candidate plus this pixel's own history -- ping-ponged,
//                because the pass reads last frame's while writing this one's
//   spatialOut   temporal plus its neighbours: what SHADING reads, and nothing
//                else ever reads it again
//
// ---------------------------------------------------------------------------
// THE SPATIAL RESULT IS NEVER FED BACK, AND THAT IS THE WHOLE REASON THIS
// WORKS. It was fed back at first, and the engine gained 2.4x the light it
// should have.
//
// The measurement that found it: temporal alone reproduced the converged
// reference (76.3 against 78.0), spatial alone reproduced it (75.7), and the
// two together came out at 184.9. Each half unbiased, the combination wildly
// not -- which is the signature of double counting rather than of a wrong
// weight somewhere.
//
// The cause is correlation. restirMerge weights a neighbour by its M, and that
// is only unbiased if the two reservoirs are INDEPENDENT. Feeding the spatial
// output back as history makes pixel P's history contain its neighbours'
// samples, so next frame P merges those same neighbours again -- and they now
// contain P. Every sample is counted through a widening web of paths, and the
// energy compounds every frame.
//
// Taking history from the TEMPORAL output instead breaks the loop: history is
// then only ever this pixel's own past, spatial reuse is applied for shading
// and immediately discarded, and neither pass can see the other's output.
// Properly fixing the correlated case needs MIS weights over the reservoirs
// (Talbot's estimator); not feeding it back is the cheap correct alternative,
// and it costs one frame of spatial reuse that would have been double counted
// anyway.
//
// ---------------------------------------------------------------------------
// THE PREVIOUS FRAME'S NORMAL AND DEPTH ARE KEPT HERE, not in the tracer.
//
// The temporal pass has to decide whether the pixel it reprojected to was the
// same surface, and that means comparing against what was there LAST frame. The
// tracer only keeps the current ones -- it writes its guides fresh every frame
// for DLSS, which has its own history and does not need the old ones.
//
// So they are copied at the end of each frame, after the passes have run and
// before the next trace overwrites them. Two copies of a normal and a depth
// buffer is cheap; getting the rejection wrong is what makes light smear
// across a silhouette and stay there.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"

#include <cstdint>
#include <string>

namespace v7 {

// MUST MATCH Reservoir in shaders/Restir.slang -- three exact 16-byte rows.
// See the note there on why the float3s are each paired with a scalar.
struct ReservoirCpu {
    float xs[3];
    float wSum;
    float ns[3];
    float W;
    float Ls[3];
    uint32_t M;
};
static_assert(sizeof(ReservoirCpu) == 48, "Reservoir must be three 16-byte rows");

class Restir {
  public:
    // ---- knobs, live from the settings menu ------------------------------
    bool enabled = false;
    bool temporal = true;
    bool spatial = true;

    // How much history one pixel may accumulate. The cap is what bounds
    // staleness -- see restirCapM in Restir.slang. Twenty is about a third of a
    // second at 60 fps, which is long enough to be worth having and short
    // enough that walking past a trunk does not leave its shadow behind.
    int maxM = 20;

    int taps = 4;          // spatial neighbours tried per pixel
    float radius = 12.0f;  // in pixels
    // Relative, not absolute: this wood runs from a trunk at arm's length to a
    // ridge hundreds of metres out, and one absolute tolerance cannot serve
    // both.
    float depthTol = 0.05f;
    float normalTol = 0.9f;  // cosine

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        try {
            temporalPass_ =
                Falcor::ComputePass::create(device_, "v7/shaders/RestirTemporal.cs.slang", "main");
            spatialPass_ =
                Falcor::ComputePass::create(device_, "v7/shaders/RestirSpatial.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("resampling shaders did not compile: ") + e.what();
            return false;
        }
        ready_ = true;
        status_ = "ready";
        return true;
    }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }
    bool active() const { return ready_ && enabled; }

    // Reservoir buffers are per PIXEL, so they follow the traced size rather
    // than the presented one -- resampling happens before any upscaling.
    void resize(uint32_t w, uint32_t h) {
        if (!ready_ || (w == w_ && h == h_)) return;
        w_ = w;
        h_ = h;
        const uint32_t n = w_ * h_;
        using Falcor::ResourceBindFlags;
        const auto kRw = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

        candidate_ = device_->createStructuredBuffer(sizeof(ReservoirCpu), n, kRw);
        for (int i = 0; i < 2; ++i)
            temporal_[i] = device_->createStructuredBuffer(sizeof(ReservoirCpu), n, kRw);
        spatialOut_ = device_->createStructuredBuffer(sizeof(ReservoirCpu), n, kRw);

        primaryPos_ = device_->createTexture2D(w_, h_, Falcor::ResourceFormat::RGBA32Float, 1, 1,
                                               nullptr, kRw);
        prevNormRough_ = device_->createTexture2D(w_, h_, Falcor::ResourceFormat::RGBA16Float, 1, 1,
                                                  nullptr, kRw);
        prevDepth_ =
            device_->createTexture2D(w_, h_, Falcor::ResourceFormat::R32Float, 1, 1, nullptr, kRw);
        cur_ = 0;
        // A fresh set of buffers holds whatever the allocator left. The first
        // temporal pass would read it as history, so the history is declared
        // empty for one frame instead.
        historyValid_ = false;
    }

    const Falcor::ref<Falcor::Buffer> &candidateBuffer() const { return candidate_; }
    const Falcor::ref<Falcor::Buffer> &finalBuffer() const { return spatialOut_; }
    const Falcor::ref<Falcor::Texture> &primaryPos() const { return primaryPos_; }

    // ---------------------------------------------------------------------
    // Temporal, then spatial. Run after the trace has filled `candidate`.
    // ---------------------------------------------------------------------
    void run(Falcor::RenderContext *ctx, const Falcor::ref<Falcor::Texture> &normRough,
             const Falcor::ref<Falcor::Texture> &depth,
             const Falcor::ref<Falcor::Texture> &motion, uint32_t frame) {
        if (!ready_ || !candidate_) return;
        const uint32_t prev = cur_ ^ 1u;

        {
            auto var = temporalPass_->getRootVar();
            var["gCandidate"] = candidate_;
            // History is the previous TEMPORAL result, never the spatial one.
            var["gHistory"] = temporal_[prev];
            var["gOut"] = temporal_[cur_];
            var["gNormRough"] = normRough;
            var["gDepth"] = depth;
            var["gMotion"] = motion;
            var["gPrevNormRough"] = prevNormRough_;
            var["gPrevDepth"] = prevDepth_;
            var["RestirCB"]["gDim"] = Falcor::uint2(w_, h_);
            var["RestirCB"]["gFrame"] = frame;
            var["RestirCB"]["gMaxM"] = uint32_t(maxM);
            var["RestirCB"]["gDepthTol"] = depthTol;
            var["RestirCB"]["gNormalTol"] = normalTol;
            var["RestirCB"]["gEnabled"] = (enabled && temporal && historyValid_) ? 1u : 0u;
            temporalPass_->execute(ctx, w_, h_);
        }
        {
            auto var = spatialPass_->getRootVar();
            var["gIn"] = temporal_[cur_];
            // Written for shading and read by nothing else, ever.
            var["gOut"] = spatialOut_;
            var["gNormRough"] = normRough;
            var["gDepth"] = depth;
            var["gPrimaryPos"] = primaryPos_;
            var["RestirSpatialCB"]["gDim"] = Falcor::uint2(w_, h_);
            var["RestirSpatialCB"]["gFrame"] = frame;
            var["RestirSpatialCB"]["gTaps"] = uint32_t(taps);
            var["RestirSpatialCB"]["gRadius"] = radius;
            var["RestirSpatialCB"]["gDepthTol"] = depthTol;
            var["RestirSpatialCB"]["gNormalTol"] = normalTol;
            var["RestirSpatialCB"]["gEnabled"] = (enabled && spatial) ? 1u : 0u;
            spatialPass_->execute(ctx, w_, h_);
        }

        // Keep this frame's guides for the next frame's rejection tests, and
        // swap which `final` is the live one.
        ctx->copyResource(prevNormRough_.get(), normRough.get());
        ctx->copyResource(prevDepth_.get(), depth.get());
        cur_ ^= 1u;
        historyValid_ = true;
    }

    // Throw the history away. The camera teleporting, the window resizing or the
    // world reloading all make every reprojection meaningless, and a reservoir
    // that survives one of those is a smear that takes seconds to wash out.
    void invalidate() { historyValid_ = false; }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> temporalPass_, spatialPass_;
    Falcor::ref<Falcor::Buffer> candidate_, temporal_[2], spatialOut_;
    Falcor::ref<Falcor::Texture> primaryPos_, prevNormRough_, prevDepth_;
    uint32_t w_ = 0, h_ = 0, cur_ = 0;
    bool ready_ = false, historyValid_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
