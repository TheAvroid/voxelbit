// ---------------------------------------------------------------------------
// renderer.h -- the film, the launch, and everything that happens after it.
//
// One frame is two GPU stages and no synchronisation between them:
//
//   optixLaunch      one sample per pixel, added into the accumulator, and the
//                    running mean resolved out of it
//   tonemapKernel    linear HDR to 8-bit sRGB, in place on the device
//
// THERE IS NO DENOISER. Every mode of the OptiX one was removed: on this scene
// the spatial models turned a voxel canopy to felt and the temporal ones lagged
// behind a moving camera, and neither was worth what it cost. What replaces it
// is nothing clever -- brute-force accumulation on a card fast enough to trace
// hundreds of samples a second, plus a Halton jitter so those samples land
// somewhere useful.
//
// NOTHING CROSSES THE BUS UNTIL THE LAST STEP, and what crosses then is the
// 8-bit frame -- a quarter of the bytes of the float4 buffer it came from. v4
// resolved and tone mapped on the CPU and spent more time on the tone curve
// than on the path tracing.
//
// THE ACCUMULATION IS WHAT MAKES THIS USABLE, and it has two modes. Stationary,
// samples pile up without limit and the image converges under you. With the
// day/night clock running the lighting is never static, so the film instead
// keeps a rolling window of maxAccum samples -- an exponential moving average
// that tracks the sun rather than being thrown away every time it shifts.
// ---------------------------------------------------------------------------
#pragma once

#include <cuda.h>

#include <cstdio>
#include <string>
#include <vector>

#include "../optix/params.h"
#include "pipeline.h"
#include "scene_gpu.h"

namespace v2 {

struct RenderSettings {
    int width = 1280;
    int height = 720;
    int spp = 64;      // offline only; the viewer accumulates one at a time
    int maxDepth = 10;
    int rrStart = 1;   // see the note on Russian roulette in v2.cu
    float exposure = 1.0f;
    float clampIndirect = 24.0f;  // firefly ceiling on non-primary contributions
    float fogDensity = 0.0022f;   // extinction per metre at y = 0
    float fogHeight = 26.0f;      // e-folding height of the haze, metres
    unsigned int seed = 20260904u;
    // 0 = accumulate without limit. Set while the day/night clock is running,
    // so the film tracks the moving sun instead of being reset by it.
    unsigned int maxAccum = 0u;
};

class Renderer {
  public:
    // -----------------------------------------------------------------------
    void init(Pipeline *pipe, GpuScene *scene, const std::string &ptxPath) {
        pipe_ = pipe;
        scene_ = scene;

        // The display kernel is loaded as PTX and JITed by the driver rather
        // than shipped as a cubin for one architecture. It costs a millisecond
        // at startup and means the same build runs on any RTX card.
        CU_CHECK(cuModuleLoad(&module_, ptxPath.c_str()));
        CU_CHECK(cuModuleGetFunction(&tonemapFn_, module_, "tonemapKernel"));

        paramsBuf_.alloc(sizeof(LaunchParams));

        // GPU timing, via events rather than a host clock.
        //
        // This is not a nicety. Every call in a frame is asynchronous, so a
        // host timer around them measures how long the CPU took to SUBMIT the
        // work -- tens of microseconds -- unless something in the frame happens
        // to synchronise, in which case it measures the whole frame instead.
        // The stats line was reporting both, mode by mode, and the two differed
        // by three orders of magnitude. Events are recorded in the stream
        // itself and time what actually ran there.
        //
        // TWO INTERVALS, NOT ONE, because they answer different questions: the
        // trace scales with the pixels the tracer was asked for, the resolve
        // with the pixels resolved and read back. Keeping them apart is what
        // shows whether a slow frame is the tracer or the readback.
        for (CUevent *e : {&evTrace0_, &evTrace1_, &evResolve0_, &evResolve1_})
            CU_CHECK(cuEventCreate(e, CU_EVENT_DEFAULT));
    }

    int renderWidth() const { return rw_; }
    int renderHeight() const { return rh_; }
    int outWidth() const { return rw_ * int(scaleOut()); }
    int outHeight() const { return rh_ * int(scaleOut()); }
    unsigned samples() const { return frame_; }

    // -----------------------------------------------------------------------
    // Allocate for a render resolution. The display buffer is sized from here
    // too, so this is the one place the film's dimensions are decided.
    // -----------------------------------------------------------------------
    void resize(int width, int height) {
        if (width == rw_ && height == rh_) return;
        rw_ = maxi(8, width);
        rh_ = maxi(8, height);

        const size_t n = size_t(rw_) * rh_;
        accum_.alloc(n * sizeof(float4));
        color_.alloc(n * sizeof(float4));

        const size_t outN = size_t(outWidth()) * outHeight();
        display_.alloc(outN * sizeof(uchar4));
        // A resize reallocates everything the history lived in, so this is one
        // of the few places the history genuinely cannot be carried over.
        resetHistory();
    }

    // -----------------------------------------------------------------------
    // Two resets, because two things go stale on different events.
    //
    // THE ACCUMULATION goes stale whenever the camera moves or the scene
    // changes: those samples were drawn for a view that is gone.
    //
    // resetHistory() survives from when a temporal denoiser had state of its
    // own to invalidate. It now differs from resetAccumulation only in intent,
    // and is kept because the call sites still mean two different things: one
    // is "these samples are stale", the other is "everything is".
    // -----------------------------------------------------------------------
    void resetAccumulation() { frame_ = 0; }

    void resetHistory() { frame_ = 0; }

    // -----------------------------------------------------------------------
    // One sample per pixel, added to whatever is already there.
    // -----------------------------------------------------------------------
    void renderSample(const CameraGPU &cam, const RenderSettings &cfg) {
        // Read the previous frame's intervals BEFORE their start events are
        // overwritten, and only if the work has actually finished. Reading them
        // in the stats path instead was the bug that produced negative frame
        // times: by then this frame had already re-recorded the start event,
        // and the pair being subtracted came from two different frames.
        collectTimings();
        CU_CHECK(cuEventRecord(evTrace0_, pipe_->stream()));

        LaunchParams p = {};
        p.accum = accum_.as<float4>();
        p.color = color_.as<float4>();

        p.width = unsigned(rw_);
        p.height = unsigned(rh_);
        p.frame = frame_;
        p.tick = tick_;
        p.seed = cfg.seed;
        p.maxAccum = cfg.maxAccum;

        p.cam = cam;
        // A jitter of zero would be correct for a plain accumulator and wrong
        // for everything downstream of one: the upscaler reconstructs detail
        // from where the samples land, and pinned to pixel centres there is
        // nothing for it to reconstruct from.
        p.jitter = haltonJitter(tick_);

        p.maxDepth = cfg.maxDepth;
        p.rrStart = cfg.rrStart;
        p.clampIndirect = cfg.clampIndirect;
        p.fogDensity = cfg.fogDensity;
        p.fogHeight = cfg.fogHeight;

        p.sky = scene_->sky.gpu();
        p.materials = reinterpret_cast<const MaterialLook *>(scene_->materialsPtr());
        p.instances = reinterpret_cast<const InstanceData *>(scene_->instancesPtr());
        p.handle = scene_->handle();

        CU_CHECK(cuMemcpyHtoDAsync(paramsBuf_.ptr(), &p, sizeof(p), pipe_->stream()));
        OPTIX_CHECK(optixLaunch(pipe_->pipeline(), pipe_->stream(), paramsBuf_.ptr(),
                                sizeof(LaunchParams), &pipe_->sbt(), unsigned(rw_), unsigned(rh_),
                                1));

        CU_CHECK(cuEventRecord(evTrace1_, pipe_->stream()));
        tracePending_ = true;

        ++frame_;
        ++tick_;  // never reset: see the note on resetAccumulation
    }

    // -----------------------------------------------------------------------
    // Tone map the resolved mean into the 8-bit display buffer.
    //
    // Split from renderSample so the offline path can take many samples before
    // paying for it once, and the viewer can skip it on frames where nothing
    // has changed enough to be worth redrawing.
    // -----------------------------------------------------------------------
    void resolve(const RenderSettings &cfg) {
        CU_CHECK(cuEventRecord(evResolve0_, pipe_->stream()));
        tonemap(color_.ptr(), cfg.exposure);
        CU_CHECK(cuEventRecord(evResolve1_, pipe_->stream()));
        resolvePending_ = true;
    }

    // -----------------------------------------------------------------------
    // Read the finished frame back. Only the viewer's blit and the PNG writer
    // call these, and both are once per displayed frame at most.
    // -----------------------------------------------------------------------
    void downloadDisplay(std::vector<unsigned char> *rgba) {
        const size_t n = size_t(outWidth()) * outHeight();
        rgba->resize(n * 4);
        CU_CHECK(cuStreamSynchronize(pipe_->stream()));
        display_.download(rgba->data(), n * 4);
    }

    // The linear frame, for --hdr.
    void downloadLinear(std::vector<float> *rgba) {
        const size_t n = size_t(outWidth()) * outHeight();
        rgba->resize(n * 4);
        CU_CHECK(cuStreamSynchronize(pipe_->stream()));
        CU_CHECK(cuMemcpyDtoH(rgba->data(), color_.ptr(), n * 4 * sizeof(float)));
    }

    void sync() { CU_CHECK(cuStreamSynchronize(pipe_->stream())); }

    // The last completed measurements, in milliseconds. Never block, never go
    // backwards: if a frame is still in flight the previous figure stands.
    float traceMs() const { return traceMs_; }
    float resolveMs() const { return resolveMs_; }

  private:
    Pipeline *pipe_ = nullptr;
    GpuScene *scene_ = nullptr;
    CUmodule module_ = nullptr;
    CUfunction tonemapFn_ = nullptr;

    DeviceBuffer accum_;
    DeviceBuffer color_;
    DeviceBuffer display_, paramsBuf_;

    int rw_ = 0, rh_ = 0;
    unsigned frame_ = 0;  // samples in the film for this camera
    unsigned tick_ = 0;   // frames since startup; drives jitter and sampling
    bool tracePending_ = false, resolvePending_ = false;
    float traceMs_ = 0.0f, resolveMs_ = 0.0f;
    CUevent evTrace0_ = nullptr, evTrace1_ = nullptr;
    CUevent evResolve0_ = nullptr, evResolve1_ = nullptr;

    unsigned scaleOut() const { return 1u; }

    // cuEventQuery rather than cuEventSynchronize: this runs at the top of
    // every frame, and waiting here would serialise the CPU against a GPU that
    // is deliberately a frame ahead.
    static bool readInterval(CUevent a, CUevent b, float *out) {
        if (cuEventQuery(b) != CUDA_SUCCESS) return false;
        float ms = 0.0f;
        if (cuEventElapsedTime(&ms, a, b) != CUDA_SUCCESS || ms < 0.0f) return false;
        *out = ms;
        return true;
    }

    void collectTimings() {
        if (tracePending_ && readInterval(evTrace0_, evTrace1_, &traceMs_)) tracePending_ = false;
        if (resolvePending_ && readInterval(evResolve0_, evResolve1_, &resolveMs_))
            resolvePending_ = false;
    }

    void tonemap(CUdeviceptr src, float exposure) {
        int w = outWidth(), h = outHeight();
        CUdeviceptr dst = display_.ptr();
        void *args[] = {&src, &dst, &w, &h, &exposure};
        const unsigned bx = 16, by = 16;
        CU_CHECK(cuLaunchKernel(tonemapFn_, (w + bx - 1) / bx, (h + by - 1) / by, 1, bx, by, 1, 0,
                                pipe_->stream(), args, nullptr));
    }
};

}  // namespace v2
