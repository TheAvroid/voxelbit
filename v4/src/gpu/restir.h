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

namespace v4 {

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

    // ---- world-space reservoirs ------------------------------------------
    //
    // Reservoirs filed against the voxel FACE they were found on, so a pixel
    // that loses its screen history to a disocclusion can pick up what other
    // pixels already learnt about that face instead of starting from noise.
    // See shaders/RestirWorld.slang for why this is consulted only on the
    // disocclusion path, and what goes wrong if it is not.
    bool world = true;
    // HARDER THAN maxM, DELIBERATELY. A face that has been accumulating for
    // hundreds of frames would otherwise arrive carrying enough confidence to
    // out-vote every candidate the newly-disoccluded pixel traces, and freeze
    // it -- the same runaway restirCapM exists to stop, arriving from a
    // different direction. Swept against a 64-sample reference on a walking
    // camera: 4 -> 0.09596 RMSE, 8 -> 0.09576, 16 -> 0.09546, 32 -> 0.09542,
    // against 0.09766 with no world map at all. The curve is flat past 16, so
    // the gain is bounded by how many pixels disocclude rather than by how much
    // confidence they are handed, and 16 takes the whole of it while staying
    // well under the screen history's own cap.
    int worldMaxM = 16;

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        try {
            temporalPass_ =
                Falcor::ComputePass::create(device_, "v4/shaders/RestirTemporal.cs.slang", "main");
            spatialPass_ =
                Falcor::ComputePass::create(device_, "v4/shaders/RestirSpatial.cs.slang", "main");
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

        // THE WORLD MAP IS NOT PER PIXEL AND IS NOT REALLOCATED HERE. It is
        // indexed by voxel face, so its size follows how much WORLD is
        // resident, not how many pixels are being traced -- resizing the window
        // must not throw away what the surfaces have learnt. Allocated once,
        // on the first resize that has a device.
        if (!worldKeys_) allocWorld();
    }

    const Falcor::ref<Falcor::Buffer> &candidateBuffer() const { return candidate_; }
    const Falcor::ref<Falcor::Buffer> &finalBuffer() const { return spatialOut_; }
    const Falcor::ref<Falcor::Texture> &primaryPos() const { return primaryPos_; }

    // HOW MANY FACES THE MAP REMEMBERS.
    //
    // One entry is a key, a lock word and a 48-byte reservoir -- 60 bytes, so
    // 2^20 entries is 63 MB. That is a quarter of what SHaRC holds and it is
    // enough: the map only ever needs the faces that are ON SCREEN, because a
    // face nobody is looking at is a face no pixel will disocclude onto, and at
    // 1920x1080 that is at most two million pixels sharing far fewer faces.
    // Entries for faces left behind are simply overwritten when their slot is
    // next claimed; there is no eviction pass and nothing needs one.
    static constexpr uint32_t kWorldCapacity = 1u << 20;

    void allocWorld() {
        using Falcor::ResourceBindFlags;
        const auto kRw = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;
        worldKeys_ = device_->createStructuredBuffer(sizeof(uint64_t), kWorldCapacity, kRw);
        worldLock_ = device_->createStructuredBuffer(sizeof(uint32_t), kWorldCapacity, kRw);
        worldRes_ = device_->createStructuredBuffer(sizeof(ReservoirCpu), kWorldCapacity, kRw);
        worldKeys_->setName("v4::restirWorldKeys");
        worldLock_->setName("v4::restirWorldLock");
        worldRes_->setName("v4::restirWorldRes");
        worldCleared_ = false;
    }

    // ---------------------------------------------------------------------
    // Temporal, then spatial. Run after the trace has filled `candidate`.
    // ---------------------------------------------------------------------
    // `jitter` is the sub-pixel offset the tracer built THIS frame's rays
    // with. The motion vectors it wrote have it divided out -- DLSS demands
    // jitter-free vectors -- so reprojection here has to add it back, or every
    // rejection test below is run against a pixel up to half a pixel away.
    void run(Falcor::RenderContext *ctx, const Falcor::ref<Falcor::Texture> &normRough,
             const Falcor::ref<Falcor::Texture> &depth,
             const Falcor::ref<Falcor::Texture> &motion, uint32_t frame,
             Falcor::float2 jitter) {
        if (!ready_ || !candidate_) return;
        const uint32_t prev = cur_ ^ 1u;

        // A KEY BUFFER OUT OF THE ALLOCATOR HOLDS WHATEVER WAS THERE, and the
        // map reads a slot as occupied whenever its key is not zero. Left
        // uncleared, the first frame finds a million random keys already in
        // residence, every claim walks its whole bucket and fails, and the
        // feature silently does nothing at all. Cleared once, not per frame:
        // the whole point is that it accumulates.
        if (worldKeys_ && !worldCleared_) {
            ctx->clearUAV(worldKeys_->getUAV().get(), Falcor::uint4(0));
            ctx->clearUAV(worldLock_->getUAV().get(), Falcor::uint4(0));
            ctx->clearUAV(worldRes_->getUAV().get(), Falcor::uint4(0));
            worldCleared_ = true;
        }

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
            var["RestirCB"]["gJitter"] = jitter;

            var["gPrimaryPos"] = primaryPos_;
            var["gWorldKeys"] = worldKeys_;
            var["gWorldLock"] = worldLock_;
            var["gWorldRes"] = worldRes_;
            var["RestirWorldCB"]["gWorldCapacity"] = kWorldCapacity;
            var["RestirWorldCB"]["gWorldMaxM"] = uint32_t(worldMaxM);
            var["RestirWorldCB"]["gWorldEnabled"] = (enabled && world) ? 1u : 0u;
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
    Falcor::ref<Falcor::Buffer> worldKeys_, worldLock_, worldRes_;
    Falcor::ref<Falcor::Texture> primaryPos_, prevNormRough_, prevDepth_;
    uint32_t w_ = 0, h_ = 0, cur_ = 0;
    bool ready_ = false, historyValid_ = false, worldCleared_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v4
