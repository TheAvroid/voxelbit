#pragma once
// ---------------------------------------------------------------------------
// recorder.h -- the built-in screen recorder. R starts a take, R ends it.
//
// ═══════════════════════════════════════════════════════════════════════════
// WHY THE PLAYBACK USED TO STUTTER, AND WHAT IS DIFFERENT HERE
// ═══════════════════════════════════════════════════════════════════════════
//
// The WebGPU game in this lineage shipped a recorder and it juddered, and the
// diagnosis it eventually wrote down (ui/mp4-mux.js, "WHY THE EXPORT STOPPED
// USING MediaRecorder") is worth having in front of you, because it is the
// reason this file is shaped the way it is:
//
//     measured on four shipped exports: EVERY frame interval in them is an
//     integer multiple of 8.33 ms, the user's 120 Hz refresh period. [...]
//     Even the best export had only 48.6% of frames on a steady cadence --
//     25.7% were held for three refreshes instead of two. That is the stutter,
//     it is baked in at capture time, and no encoder setting can undo it.
//
// So the stutter was never an encoder setting. It was that each frame got the
// WALL-CLOCK TIME OF WHATEVER PAINT IT LANDED ON as its presentation time, so
// the file described an unevenly spaced sequence of pictures, and a player
// asked to honour those times reproduces exactly the unevenness it was given.
// A second failure in the same family: intervals written as raw microseconds
// against a 1,000,000 tick timescale made the file declare one million frames
// a second, and players that believe r_frame_rate then try to build a million
// frames a second (that one showed as GREEN frames, not as judder).
//
// This recorder cannot produce either, because it never asks the wall clock
// what time a frame is. Four rules, and all four are load-bearing:
//
//  1. CONSTANT FRAME RATE BY CONSTRUCTION. Frame n's presentation time is
//     n / fps, computed with integer arithmetic that cannot accumulate error
//     (see slotPts). Not "roughly n / fps"; exactly, to within one 100 ns
//     tick, which at 60 fps is one part in 1.67 million. There is no path in
//     this file by which a frame can be stamped with a measured time.
//
//  2. THE WALL CLOCK CHOOSES *WHICH* SLOT, NEVER *WHAT TIME*. Every rendered
//     frame asks which capture slot the clock is in. If the slot is one the
//     recorder has already filled, the paint is skipped and costs nothing --
//     that is how a 300 fps game records at 60 without recording in fast
//     motion. (The WebGPU recorder got this wrong the other way round: it
//     paced by counting paints, so a window painting 143 fps against a 60 fps
//     label wrote an 8 s take that spanned 19 s. Slow motion.)
//
//  3. A SLOT IS NEVER LEFT EMPTY. If the game hitches and three slots go by
//     between paints, the encoder emits the PREVIOUS picture for the two it
//     missed and the new picture for the one it is in. The result is that a
//     hitch reads back as a hitch -- a frame held a little longer -- instead
//     of as a timeline that has silently lost 33 ms and now runs early against
//     everything after it. Holding the PREVIOUS frame rather than repeating
//     the new one backwards matters: the new content did not exist yet during
//     those slots, and showing it early is a visible 50 ms jump on every
//     hitch.
//
//  4. THE RENDER THREAD NEVER WAITS. Colour conversion is a compute shader,
//     the read-back is a fenced ring polled without blocking, and the encoder
//     lives on its own thread behind a bounded queue. When the encoder cannot
//     keep up the recorder DROPS a slot (which rule 3 then fills with a held
//     frame) rather than stalling the game -- because a recorder that costs
//     frame rate makes the thing it is recording worse, and that is its own
//     kind of stutter.
//
// ═══════════════════════════════════════════════════════════════════════════
//
// Media Foundation does the encoding and the muxing. That is a deliberate
// choice over binding NVENC directly: MF ships with Windows so there is no SDK
// to vendor, it picks up the NVIDIA hardware encoder MFT on its own, it writes
// the mp4 container -- and, the part that actually decides it, an
// IMFSample carries an explicit presentation time that the sink writer honours
// verbatim. Rule 1 above needs precisely that and nothing more.
//
// There is no audio track: v2 has no audio. A recorder for an engine that made
// sound would need the A/V alignment work the WebGPU game did, and none of it
// is here.
// ---------------------------------------------------------------------------

#include <Falcor.h>
#include <Core/Pass/ComputePass.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "render/mfvideo.h"

namespace vb {

using Falcor::ComputePass;
using Falcor::ResourceFormat;
using Falcor::Texture;
using Falcor::uint2;

// A finished take: everything a caller needs to describe the file that was
// just written without opening it again.
struct Take {
    std::string path;
    int width = 0;
    int height = 0;
    int fpsNum = 60;
    int fpsDen = 1;
    int64_t frames = 0;
    double fps() const { return fpsDen > 0 ? double(fpsNum) / double(fpsDen) : 60.0; }
    double seconds() const { return fps() > 0.0 ? double(frames) / fps() : 0.0; }
    bool valid() const { return width > 0 && height > 0 && frames > 0; }
};

// ---------------------------------------------------------------------------
// The GPU half: linear RGBA -> NV12, into a buffer, without a stall.
//
// Shared by the live recorder and the export, which need exactly the same
// conversion from two different sources (the display texture is linear float;
// a decoded video frame is 8-bit and already curved). gDecodeSrgb in the
// shader is the only difference between them.
// ---------------------------------------------------------------------------
class Nv12Convert {
  public:
    void init(Falcor::ref<Falcor::Device> device) {
        if (pass_) return;
        device_ = device;
        pass_ = ComputePass::create(device_, "v2/shaders/CaptureNv12.cs.slang", "main");
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear, Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Point);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp);
        sampler_ = device_->createSampler(sd);
    }

    bool ready() const { return pass_ != nullptr; }

    // srcW/srcH is the sub-rectangle of `src` that holds a picture, which is
    // not necessarily the texture's size -- see the note on gSrcDim.
    void run(Falcor::RenderContext *ctx, const Falcor::ref<Texture> &src, int srcW, int srcH,
             const Falcor::ref<Falcor::Buffer> &dst, int outW, int outH, bool srcIsSrgb) {
        if (!pass_ || !src || !dst || outW <= 0 || outH <= 0) return;
        const float stepX = float(srcW) / float(outW);
        const float stepY = float(srcH) / float(outH);
        auto var = pass_->getRootVar();
        var["gSrc"] = src;
        var["gSamp"] = sampler_;
        var["gDst"] = dst;
        var["gCaptureCB"]["gOutDim"] = uint2(uint32_t(outW), uint32_t(outH));
        var["gCaptureCB"]["gSrcDim"] = Falcor::float2(float(srcW), float(srcH));
        var["gCaptureCB"]["gInvSrcTex"] =
            Falcor::float2(1.0f / float(src->getWidth()), 1.0f / float(src->getHeight()));
        var["gCaptureCB"]["gStep"] = Falcor::float2(stepX, stepY);
        // One tap when there is no downscale, otherwise enough taps to cover
        // the source rect. Capped at 8: past that the picture is so much
        // smaller than the frame that a few more taps change nothing anyone
        // can see, and the loop is per output pixel.
        var["gCaptureCB"]["gTaps"] =
            uint2(uint32_t(std::clamp(int(std::ceil(stepX)), 1, 8)),
                  uint32_t(std::clamp(int(std::ceil(stepY)), 1, 8)));
        var["gCaptureCB"]["gDecodeSrgb"] = uint32_t(srcIsSrgb ? 1 : 0);
        pass_->execute(ctx, uint32_t(outW / 4), uint32_t(outH / 2), 1);
    }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<ComputePass> pass_;
    Falcor::ref<Falcor::Sampler> sampler_;
};

// NV12 is 1.5 bytes a pixel: a full luma plane then a half-height interleaved
// chroma plane.
inline size_t nv12Bytes(int w, int h) { return size_t(w) * size_t(h) * 3 / 2; }

// The encoder wants even dimensions for 4:2:0; the conversion shader wants a
// width that is a multiple of 4 so its stores land aligned (see the block-shape
// note in CaptureNv12.cs.slang). Rounded DOWN, so a capture is never asked to
// invent pixels that were not rendered.
inline void alignCaptureDims(int &w, int &h) {
    w = std::max(16, w & ~3);
    h = std::max(16, h & ~1);
}

// ---------------------------------------------------------------------------
// The recorder.
// ---------------------------------------------------------------------------
class Recorder {
  public:
    // How many frames may be in flight between the GPU and the encoder. Six is
    // ~19 MB at 1080p and covers about 100 ms of encoder jitter at 60 fps,
    // which is more than a hardware H.264 encoder has ever needed here. Making
    // it larger would not stop a drop -- an encoder that is genuinely slower
    // than the capture rate fills any ring -- it would only delay noticing.
    static constexpr int kRing = 6;

    // The longest hitch that is reproduced as a hitch. Past this the take
    // would be showing one still picture for over a second, which is worse
    // than a cut: the clock is slid forward instead and the gap simply is not
    // in the video. Reported at stop, so it is never silent.
    static constexpr double kMaxHoldSeconds = 1.0;

    enum class State { Idle, Recording, Finalizing };

    void init(Falcor::ref<Falcor::Device> device) {
        device_ = device;
        conv_.init(device);
    }

    State state() const { return state_; }
    bool recording() const { return state_ == State::Recording; }
    bool busy() const { return state_ != State::Idle; }

    // Seconds of video written so far -- the slot clock, not the wall clock,
    // so a take that has dropped or held frames still reports the length the
    // file will have.
    double elapsed() const {
        return fpsNum_ > 0 ? double(nextSlot_) * double(fpsDen_) / double(fpsNum_) : 0.0;
    }
    int64_t droppedFrames() const { return dropped_; }
    int64_t heldFrames() const { return held_; }
    int captureWidth() const { return outW_; }
    int captureHeight() const { return outH_; }
    double captureFps() const { return fpsDen_ > 0 ? double(fpsNum_) / double(fpsDen_) : 0.0; }

    // -----------------------------------------------------------------------
    // Start a take.
    //
    // `srcW`/`srcH` is what the tracer is presenting; `maxWidth` clamps the
    // recording (hardware H.264 tops out at 4096 wide, and there is no point
    // recording larger than the person will ever watch). The capture size is
    // derived, not asked for, so it always matches the aspect of what is on
    // screen.
    // -----------------------------------------------------------------------
    bool start(const std::string &path, int srcW, int srcH, int maxWidth, int fpsNum, int fpsDen,
               double now) {
        if (state_ != State::Idle || srcW <= 0 || srcH <= 0) return false;
        if (!conv_.ready()) return false;

        int w = srcW, h = srcH;
        if (maxWidth > 0 && w > maxWidth) {
            h = int(std::lround(double(h) * double(maxWidth) / double(w)));
            w = maxWidth;
        }
        alignCaptureDims(w, h);

        outW_ = w;
        outH_ = h;
        fpsNum_ = std::max(1, fpsNum);
        fpsDen_ = std::max(1, fpsDen);
        path_ = path;

        const size_t bytes = nv12Bytes(outW_, outH_);
        // DeviceLocal, UAV: the shader's destination. Copied to a read-back
        // buffer rather than written into one directly, because a UAV in
        // read-back memory is either refused or is uncached writes from the
        // GPU, and both are worse than the copy.
        nv12_ = device_->createBuffer(bytes,
                                      Falcor::ResourceBindFlags::UnorderedAccess |
                                          Falcor::ResourceBindFlags::ShaderResource,
                                      Falcor::MemoryType::DeviceLocal);
        nv12_->setName("v2::recorder::nv12");

        for (int i = 0; i < kRing; ++i) {
            Slot &s = ring_[i];
            s.buf = device_->createBuffer(bytes, Falcor::ResourceBindFlags::None,
                                          Falcor::MemoryType::ReadBack);
            s.buf->setName("v2::recorder::readback");
            // MAPPED ONCE AND LEFT MAPPED. Map/unmap per frame is a driver
            // call per frame for a pointer that never moves, and on the render
            // thread. The encoder reads straight out of this pointer; the slot
            // is not reused until it says it is done with it.
            s.mapped = reinterpret_cast<const uint8_t *>(s.buf->map());
            s.inFlight = false;
            s.withEncoder = false;
            if (!s.mapped) {
                stopHard();
                return false;
            }
        }
        fence_ = device_->createFence();
        head_ = tail_ = 0;

        t0_ = now;
        nextSlot_ = 0;
        dropped_ = 0;
        held_ = 0;
        slid_ = 0;
        firstFrame_ = true;

        // The bitrate follows the picture, at ~0.2 bits per pixel -- the same
        // rule and the same constant the WebGPU game settled on for its master
        // recording, which is generous on purpose: this file is the thing the
        // file is the deliverable, so it wants to be near-transparent rather
        // than small. ~25 Mbps at 1080p60, ~44 at 1440p60.
        const double bpp = 0.2;
        const uint32_t rate = uint32_t(std::clamp(
            double(outW_) * double(outH_) * captureFps() * bpp, 8.0e6, 200.0e6));

        // A KEYFRAME EVERY SECOND, not every two. Seek granularity in any
        // player is the GOP, and a take that is about to be scrubbed through
        // in an editor feels sticky at 2 s. A second costs a few percent of
        // bitrate and is worth it for footage meant to be cut.
        VideoWriter::Config cfg;
        cfg.width = outW_;
        cfg.height = outH_;
        cfg.fpsNum = fpsNum_;
        cfg.fpsDen = fpsDen_;
        cfg.bitrate = rate;
        cfg.gopFrames = int(std::lround(captureFps()));
        cfg.path = path_;

        stopRequested_ = false;
        writerFailed_ = false;
        state_ = State::Recording;
        worker_ = std::thread([this, cfg] { encodeLoop(cfg); });
        return true;
    }

    // -----------------------------------------------------------------------
    // One rendered frame. Called every frame while a take is running; does
    // nothing at all on the paints that are not due.
    // -----------------------------------------------------------------------
    void tick(Falcor::RenderContext *ctx, const Falcor::ref<Texture> &src, int srcW, int srcH,
              double now) {
        if (state_ != State::Recording || !src || srcW <= 0 || srcH <= 0) return;

        harvest();

        // ── WHICH SLOT IS THE CLOCK IN ────────────────────────────────────
        // Rule 2. This is the only place the wall clock is consulted, and all
        // it ever produces is an integer index.
        int64_t slot = int64_t((now - t0_) * captureFps());
        if (slot < nextSlot_) return;  // not this paint's turn, and free

        int64_t hold = slot - nextSlot_;

        // Rule 3's escape hatch: a hitch longer than kMaxHoldSeconds is cut
        // rather than held. Sliding t0_ keeps every later slot consistent with
        // this decision -- the alternative, clamping `hold` alone, would leave
        // the clock permanently ahead of the file and re-drop the difference
        // on every frame after it.
        const int64_t maxHold = int64_t(kMaxHoldSeconds * captureFps());
        if (hold > maxHold) {
            const int64_t cut = hold - maxHold;
            t0_ += double(cut) / captureFps();
            slot -= cut;
            hold = maxHold;
            slid_ += cut;
        }

        Slot *s = acquire();
        if (!s) {
            // Rule 4: the encoder is behind. Do not wait -- leave nextSlot_
            // where it is so the NEXT successful capture reports these slots
            // as a hold and fills them with the last picture.
            ++dropped_;
            return;
        }

        conv_.run(ctx, src, srcW, srcH, nv12_, outW_, outH_, /*srcIsSrgb*/ false);
        ctx->copyResource(s->buf.get(), nv12_.get());
        // A submit is needed for the fence to mean anything -- Falcor's own
        // submit happens after this function returns, and polling a fence
        // signalled on commands that have not been handed to the queue yet
        // would never complete. Non-blocking: the point is to get the copy
        // moving, not to see it finish.
        ctx->submit(false);
        s->fenceValue = ctx->signal(fence_.get());
        s->slot = slot;
        s->hold = firstFrame_ ? 0 : hold;  // nothing to hold before the first picture
        s->inFlight = true;

        held_ += s->hold;
        firstFrame_ = false;
        nextSlot_ = slot + 1;
    }

    // -----------------------------------------------------------------------
    // End the take. Returns immediately; the encoder finishes on its thread.
    // poll() reports when the file is closed and hands back the Take.
    // -----------------------------------------------------------------------
    void stop() {
        if (state_ != State::Recording) return;
        state_ = State::Finalizing;
        // Everything already read back still belongs in the file, so drain the
        // ring before telling the encoder to close. This blocks on the GPU,
        // which is fine: the take is over and the panel is about to open.
        drain();
        {
            std::lock_guard<std::mutex> lk(m_);
            stopRequested_ = true;
        }
        cv_.notify_all();
    }

    // Call every frame. Returns true exactly once, when the file is finished,
    // and fills `out`.
    bool poll(Take &out) {
        if (state_ == State::Recording) {
            harvest();
            return false;
        }
        if (state_ != State::Finalizing) return false;
        if (!done_.load(std::memory_order_acquire)) return false;

        if (worker_.joinable()) worker_.join();

        out.path = path_;
        out.width = outW_;
        out.height = outH_;
        out.fpsNum = fpsNum_;
        out.fpsDen = fpsDen_;
        out.frames = written_.load(std::memory_order_relaxed);

        const bool ok = !writerFailed_ && out.frames > 0;
        std::printf("v2: recorded %s -- %dx%d @ %.3g fps, %lld frames (%.2f s)",
                    path_.c_str(), outW_, outH_, captureFps(),
                    (long long)out.frames, out.seconds());
        if (held_) std::printf(", %lld held", (long long)held_);
        if (dropped_) std::printf(", %lld dropped", (long long)dropped_);
        if (slid_) std::printf(", %lld slot(s) cut past the %.1fs hold limit",
                               (long long)slid_, kMaxHoldSeconds);
        std::printf("\n");
        if (!ok) std::fprintf(stderr, "v2: the recording did not produce a usable file\n");
        std::fflush(stdout);

        release();
        state_ = State::Idle;
        return ok;
    }

    // Tear a take down without finishing the file -- used on shutdown.
    void abandon() {
        if (state_ == State::Idle) return;
        {
            std::lock_guard<std::mutex> lk(m_);
            stopRequested_ = true;
            abandon_.store(true, std::memory_order_relaxed);
        }
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        release();
        state_ = State::Idle;
    }

    Nv12Convert &converter() { return conv_; }

  private:
    struct Slot {
        Falcor::ref<Falcor::Buffer> buf;
        const uint8_t *mapped = nullptr;
        uint64_t fenceValue = 0;
        int64_t slot = 0;
        int64_t hold = 0;
        bool inFlight = false;    // GPU copy issued, not yet known complete
        std::atomic<bool> withEncoder{false};  // handed over, not yet released
    };

    struct Job {
        int ring = -1;
        int64_t slot = 0;
        int64_t hold = 0;
    };

    // Next free ring slot, in order. In-order use is what lets harvest() stop
    // at the first incomplete fence instead of scanning.
    Slot *acquire() {
        Slot &s = ring_[head_ % kRing];
        if (s.inFlight || s.withEncoder.load(std::memory_order_acquire)) return nullptr;
        ++head_;
        return &s;
    }

    // Hand every finished read-back to the encoder. Never blocks: a fence that
    // has not reached its value simply is not ready this frame.
    void harvest() {
        if (!fence_) return;
        const uint64_t reached = fence_->getCurrentValue();
        while (tail_ < head_) {
            Slot &s = ring_[tail_ % kRing];
            if (!s.inFlight) break;
            if (s.fenceValue > reached) break;
            s.inFlight = false;
            s.withEncoder.store(true, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(m_);
                queue_.push_back(Job{int(tail_ % kRing), s.slot, s.hold});
            }
            cv_.notify_one();
            ++tail_;
        }
    }

    // Wait for everything in flight, then hand it over. Only on stop.
    void drain() {
        if (!fence_) return;
        while (tail_ < head_) {
            Slot &s = ring_[tail_ % kRing];
            if (s.inFlight) fence_->wait(s.fenceValue);
            harvest();
        }
    }

    void release() {
        for (int i = 0; i < kRing; ++i) {
            Slot &s = ring_[i];
            if (s.buf && s.mapped) s.buf->unmap();
            s.mapped = nullptr;
            s.buf = nullptr;
            s.inFlight = false;
            s.withEncoder.store(false, std::memory_order_relaxed);
        }
        nv12_ = nullptr;
        fence_ = nullptr;
        queue_.clear();
        head_ = tail_ = 0;
        done_.store(false, std::memory_order_relaxed);
        written_.store(0, std::memory_order_relaxed);
        abandon_.store(false, std::memory_order_relaxed);
    }

    void stopHard() {
        release();
        state_ = State::Idle;
    }

    // -----------------------------------------------------------------------
    // The encoder thread. All Media Foundation work happens here and nowhere
    // else, so there is exactly one apartment to think about.
    // -----------------------------------------------------------------------
    void encodeLoop(VideoWriter::Config cfg) {
        VideoWriter writer;
        if (!writer.open(cfg)) {
            writerFailed_ = true;
            // Still drain the queue, or tick() blocks forever on a full ring.
            for (;;) {
                Job job;
                if (!nextJob(job)) break;
                ring_[job.ring].withEncoder.store(false, std::memory_order_release);
            }
            done_.store(true, std::memory_order_release);
            return;
        }

        const size_t bytes = nv12Bytes(cfg.width, cfg.height);
        int64_t written = 0;

        for (;;) {
            Job job;
            if (!nextJob(job)) break;
            if (abandon_) {
                ring_[job.ring].withEncoder.store(false, std::memory_order_release);
                continue;
            }

            // Rule 3, and this is the whole of it: the slots this frame skipped
            // are filled with the PREVIOUS picture, at their own exact times,
            // before the new picture is written at its own.
            if (job.hold > 0 && writer.hasLastFrame()) {
                writer.repeatLast(job.slot - job.hold, job.hold);
                written += job.hold;
            }
            writer.writeFrame(ring_[job.ring].mapped, bytes, job.slot);
            ++written;
            written_.store(written, std::memory_order_relaxed);

            // The encoder copied the pixels into its own media buffer, so the
            // ring slot is free the moment writeFrame returns.
            ring_[job.ring].withEncoder.store(false, std::memory_order_release);
        }

        if (!writer.close() && !abandon_) writerFailed_ = true;
        written_.store(written, std::memory_order_relaxed);
        done_.store(true, std::memory_order_release);
    }

    bool nextJob(Job &out) {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return !queue_.empty() || stopRequested_; });
        if (queue_.empty()) return false;  // stop requested and drained
        out = queue_.front();
        queue_.pop_front();
        return true;
    }

    Falcor::ref<Falcor::Device> device_;
    Nv12Convert conv_;

    Falcor::ref<Falcor::Buffer> nv12_;
    Falcor::ref<Falcor::Fence> fence_;
    Slot ring_[kRing];
    uint64_t head_ = 0, tail_ = 0;

    State state_ = State::Idle;
    std::string path_;
    int outW_ = 0, outH_ = 0;
    int fpsNum_ = 60, fpsDen_ = 1;

    double t0_ = 0.0;
    int64_t nextSlot_ = 0;
    int64_t dropped_ = 0, held_ = 0, slid_ = 0;
    bool firstFrame_ = true;

    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<Job> queue_;
    bool stopRequested_ = false;
    // Read by the encoder thread outside the lock, on its way out.
    std::atomic<bool> abandon_{false};
    std::atomic<bool> done_{false};
    std::atomic<int64_t> written_{0};
    bool writerFailed_ = false;
};

}  // namespace vb
