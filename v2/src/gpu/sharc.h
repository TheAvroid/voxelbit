// ---------------------------------------------------------------------------
// sharc.h -- the SHaRC spatial hash radiance cache, host side.
//
// WHAT IS HERE AND WHAT IS NOT. This file owns four buffers and one dispatch
// and nothing else: there is no SDK object, no device interop, no library to
// link. SHaRC is shader code, so the algorithm lives in Sharc.slang,
// SharcResolve.cs.slang and the update entry point in Trace.cs.slang, and this
// is only the thing that allocates and sequences them.
//
// THAT IS ALSO WHY IT RUNS ON BOTH BACKENDS. DDGI is D3D12 only because
// RTXGI 1.3's probe blending is a D3D12 library; there is no equivalent here,
// so the Vulkan path -- which loses DLSS, Streamline and the probes all at once
// -- still gets an indirect cache. See ddgi.h for the other half.
//
// THE ORDER OF THE THREE PASSES IS FIXED AND EACH NEEDS A BARRIER AFTER IT:
//
//   update    sparse paths write raw sums into the accumulation buffer
//   resolve   one thread per entry folds those into the running average
//   render    the camera path reads the running average and terminates into it
//
// Run resolve before update and it averages last frame's samples into this
// frame's; skip the barriers and the render pass reads entries the resolve has
// not finished writing. Neither fails loudly.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/Pass/ComputePass.h"

#include <algorithm>
#include <string>

#include "../core/vecmath.h"

namespace v2 {

// HOW MANY CELLS THE MAP HOLDS, AND WHY THIS MANY.
//
// The SDK suggests 2^22 as a baseline. At 44 bytes an entry -- 8 for the hash,
// 16 accumulating, 16 resolved, 4 for the lock word -- that is 184 MB, and this
// engine is already the largest allocation on the device. 2^21 is 92 MB and, at
// the occupancy this scene produces, still sits well under the 10-20 % the SDK
// wants to see. Raise it if the wood ever looks like it is losing cache entries
// as you turn: that is what collisions look like.
static constexpr uint32_t kSharcCapacity = 1u << 21;

// ...AND THAT IS NOW A DEFAULT RATHER THAN THE ANSWER. Measured with
// --sharc-stats: at the shipped 32-frame eviction window the map sits at 56%
// occupancy, but the window is short precisely because the SDK's key goes
// stale. Let an exact voxel key remember for 512 frames instead and the same
// map is 100% full -- every slot taken, and inserts starting to be dropped.
// Remembering longer is only worth anything if there is somewhere to put it.
static constexpr uint32_t kSharcCapacityMax = 1u << 24;

// The temporal window, in frames, and how long an entry survives unwritten.
// Both are the SDK's own defaults; the second is clamped internally against
// SHARC_STALE_FRAME_NUM_MIN so a small value cannot thrash the map.
static constexpr uint32_t kSharcAccumFrames = 64u;
static constexpr uint32_t kSharcStaleFrames = 32u;

// HOW BIG A CACHE VOXEL IS AT THE CAMERA, near enough. The hash grid's level
// grows with distance, so this sets the finest level rather than a fixed size.
// The world is 10 cm voxels and a pine is about 9 m to its first branch, so a
// metre-ish cell near the camera keeps a trunk and the ground beside it in
// different cells, which is what stops light leaking around a trunk.
static constexpr float kSharcSceneScale = 12.0f;

class Sharc {
  public:
    bool available() const { return ready_; }
    const std::string &status() const { return status_; }
    // Set by app.h from the same option that picks the shader define, so the
    // status line cannot disagree with what the tracer was compiled to do.
    bool voxelKey = true;

    uint32_t capacity() const { return capacity_; }

    // -----------------------------------------------------------------------
    // Allocate, and clear.
    //
    // THE CLEAR IS PART OF THE CONTRACT, not hygiene. An unwritten hash entry
    // has to read as empty, and empty in this map is the value zero -- so a
    // buffer holding whatever the allocator last had in it is a map already
    // full of entries pointing at cells that were never sampled. The SDK's own
    // integration guide puts this in a warning box.
    // -----------------------------------------------------------------------
    bool init(const Falcor::ref<Falcor::Device> &device) {
#if !V2_HAS_SHARC
        status_ = "built without the SHaRC headers (set V2_SHARC_DIR)";
        return false;
#else
        device_ = device;
        try {
            using Falcor::ResourceBindFlags;
            const ResourceBindFlags uav =
                ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;

            hashEntries_ = device_->createBuffer(size_t(capacity_) * 8u, uav);
            lock_ = device_->createBuffer(size_t(capacity_) * 4u, uav);
            // uint4, and a float16x4 plus two uints: both 16 bytes. Written out
            // here rather than sizeof-ed because the shader-side structs are the
            // definition and this file cannot see them.
            accum_ = device_->createBuffer(size_t(capacity_) * 16u, uav);
            resolved_ = device_->createBuffer(size_t(capacity_) * 16u, uav);

            hashEntries_->setName("v2::sharcHashEntries");
            lock_->setName("v2::sharcLock");
            accum_->setName("v2::sharcAccum");
            resolved_->setName("v2::sharcResolved");
            // Four counters. Tiny, and bound whether or not anyone is counting,
            // because a declared resource with nothing behind it fails the
            // dispatch rather than the branch.
            stats_ = device_->createStructuredBuffer(sizeof(uint32_t), 4u, uav);
            stats_->setName("v2::sharcStats");

            resolve_ =
                Falcor::ComputePass::create(device_, "v2/shaders/SharcResolve.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("allocation failed: ") + e.what();
            return false;
        }

        cleared_ = false;
        ready_ = true;
        // WHICH KEY, because the two behave differently enough that a run
        // report which does not say is a report you cannot compare against.
        status_ = std::to_string(capacity_ >> 20) + "M entries, " +
                  (voxelKey ? "voxel face key" : "SDK hash grid");
        return true;
#endif
    }

    // Zero the map. On the first frame, and whenever the cache has to forget.
    void clear(Falcor::RenderContext *ctx) {
        if (!ready_) return;
        ctx->clearUAV(hashEntries_->getUAV().get(), Falcor::uint4(0));
        ctx->clearUAV(lock_->getUAV().get(), Falcor::uint4(0));
        ctx->clearUAV(accum_->getUAV().get(), Falcor::uint4(0));
        ctx->clearUAV(resolved_->getUAV().get(), Falcor::uint4(0));
        cleared_ = true;
        frame_ = 0u;
    }

    bool needsClear() const { return ready_ && !cleared_; }

    // ZEROED AT THE TOP OF THE FRAME, before the update pass runs.
    //
    // It lived in runResolve at first, which reads correctly for occupancy and
    // for queries and silently zeroes the one counter that matters most: the
    // UPDATE pass is what records a dropped insert, the resolve runs after it,
    // so dropped inserts were being cleared between being written and being
    // read. They reported zero at 100% occupancy, which is exactly the
    // condition under which they cannot be zero.
    void clearStats(Falcor::RenderContext *ctx) {
        if (ready_ && statsOn) ctx->clearUAV(stats_->getUAV().get(), Falcor::uint4(0));
    }

    // For the barriers the caller has to place between the three passes.
    Falcor::Buffer *accumBuffer() const { return accum_.get(); }
    Falcor::Buffer *hashBuffer() const { return hashEntries_.get(); }

    // Bind the four buffers and the constants. Shared by all three passes, so
    // none of them can describe the map differently from the others.
    void bind(const Falcor::ShaderVar &var, const Vec3 &cameraPos) {
        if (!ready_) return;
        var["gSharcHashEntries"] = hashEntries_;
        var["gSharcLock"] = lock_;
        var["gSharcAccum"] = accum_;
        var["gSharcResolved"] = resolved_;
        var["gSharcCB"]["gSharcCameraPos"] = Falcor::float3(cameraPos.x, cameraPos.y, cameraPos.z);
        var["gSharcCB"]["gSharcSceneScale"] = kSharcSceneScale;
        var["gSharcCB"]["gSharcCameraPosPrev"] =
            Falcor::float3(prevCam_.x, prevCam_.y, prevCam_.z);
        var["gSharcCB"]["gSharcCapacity"] = capacity_;
        var["gSharcCB"]["gSharcFrame"] = frame_;
        var["gSharcCB"]["gSharcAccumFrames"] = kSharcAccumFrames;
        var["gSharcCB"]["gSharcStaleFrames"] = staleFrames;
        var["gSharcStats"] = stats_;
        var["gSharcCB"]["gSharcStatsOn"] = statsOn ? 1u : 0u;
    }

    // ---- the counters -----------------------------------------------------
    //
    // Off by default: the query counter is an atomic on a path that runs per
    // pixel per bounce, which is exactly where an engine cannot afford one.
    bool statsOn = false;

    // HOW LONG AN ENTRY SURVIVES WITH NOTHING WRITTEN TO IT, in frames.
    //
    // 32 was right for the SDK's key and is probably not right for ours. That
    // key is a quantised position whose CELL SIZE depends on where the camera
    // is, so an old entry is not merely stale, it is filed under a grid that no
    // longer exists -- forgetting quickly was the correct answer. A voxel face
    // does not move and its radiance does not depend on where anyone stands, so
    // an old entry here is just an old measurement of something still true.
    // Half a second of memory is throwing away durable knowledge.
    //
    // The SDK caps it at SHARC_STALE_FRAME_NUM_MAX (1024) internally.
    uint32_t staleFrames = kSharcStaleFrames;

    // Set before init(); clamped to kSharcCapacityMax. At 60 bytes an entry
    // (8 key, 4 lock, 16 accumulating, 16 resolved) 2^21 is 92 MB and 2^23 is
    // 368 MB, which is why this is a decision and not simply raised.
    uint32_t capacity_ = kSharcCapacity;
    void setCapacity(uint32_t n) { capacity_ = (n > 0u) ? std::min(n, kSharcCapacityMax) : kSharcCapacity; }

    struct Stats {
        uint32_t live = 0, queries = 0, hits = 0, insertFails = 0;
    };

    // A blocking readback, so this is for --profile and the console, never for
    // a frame. Reports the LAST frame counted, which is what the per-frame
    // clear below makes it.
    Stats readStats() const {
        Stats st;
        if (!ready_ || !stats_) return st;
        uint32_t raw[4] = {0, 0, 0, 0};
        stats_->getBlob(raw, 0, sizeof(raw));
        st.live = raw[0];
        st.queries = raw[1];
        st.hits = raw[2];
        st.insertFails = raw[3];
        return st;
    }

    // One thread per entry, after the update pass and before the render.
    void runResolve(Falcor::RenderContext *ctx, const Vec3 &cameraPos) {
        if (!ready_ || !resolve_) return;
        auto var = resolve_->getRootVar();
        bind(var, cameraPos);
        resolve_->execute(ctx, capacity_, 1u);
        ctx->uavBarrier(resolved_.get());
        ctx->uavBarrier(accum_.get());
        prevCam_ = cameraPos;
        ++frame_;
    }

    void shutdown() {
        hashEntries_.reset();
        lock_.reset();
        accum_.reset();
        resolved_.reset();
        resolve_.reset();
        ready_ = false;
    }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::Buffer> hashEntries_, lock_, accum_, resolved_, stats_;
    Falcor::ref<Falcor::ComputePass> resolve_;
    Vec3 prevCam_{0.0f, 0.0f, 0.0f};
    uint32_t frame_ = 0u;
    bool ready_ = false;
    bool cleared_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
