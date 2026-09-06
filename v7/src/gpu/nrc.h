// ---------------------------------------------------------------------------
// nrc.h -- the neural radiance cache, host side: weights, batches, and the two
//          dispatches that teach it.
//
// The network itself is in shaders/Nrc.slang and the passes are NrcTrain and
// NrcUpdate; this file owns the buffers they share and the schedule they run
// on. See Nrc.slang for what the network is and why it is that shape.
//
// ---------------------------------------------------------------------------
// THE CACHE IS WORTHLESS UNTIL IT IS TRAINED, AND IT SAYS SO.
//
// A freshly initialised network predicts noise. If the tracer started asking it
// for radiance on frame one the wood would be full of coloured fog, so the
// cache reports a WARMUP: until enough batches have gone through, inference is
// refused and the tracer runs full-length paths exactly as it did before. The
// menu shows the count climbing, which is also the only honest way to tell
// whether training is actually happening.
//
// ---------------------------------------------------------------------------
// WHY THE WEIGHTS ARE INITIALISED ON THE CPU AND UPLOADED ONCE.
//
// It is 14 KB. A GPU init pass would need its own shader, its own RNG and its
// own correctness argument, to save an upload that happens once per run. The
// distribution matters more than where it is computed: He initialisation
// (variance 2/fan_in) is what keeps a ReLU network's activations from either
// vanishing or saturating through the layers, and getting it wrong is the
// difference between a cache that converges in seconds and one that never does.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/Pass/ComputePass.h"
#include "Core/Program/Program.h"

// For nrcAddCapability -- the two files ask the compiler for the same thing.
#include "neural.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <string>
#include <vector>

namespace v7 {

// MUST MATCH Nrc.slang. A disagreement here does not crash -- it reads the
// weights at the wrong offsets and produces a plausible-looking wrong picture,
// which is far worse, so both sides derive everything from the same three
// widths and neither hardcodes a byte offset.
struct NrcLayout {
    static constexpr int kIn = 32;
    static constexpr int kHidden = 64;
    static constexpr int kOut = 16;

    static constexpr uint32_t kW0 = 0;
    static constexpr uint32_t kW1 = kW0 + kHidden * kIn * 2;
    static constexpr uint32_t kW2 = kW1 + kHidden * kHidden * 2;
    static constexpr uint32_t kB0 = kW2 + kOut * kHidden * 2;
    static constexpr uint32_t kB1 = kB0 + kHidden * 2;
    static constexpr uint32_t kB2 = kB1 + kHidden * 2;
    static constexpr uint32_t kBytes = kB2 + kOut * 2;
    static constexpr uint32_t kElems = kBytes / 2;
};

// One training record, written by the tracer. MUST MATCH NrcSample in
// NrcTrain.cs.slang -- 17 floats, no padding games.
struct NrcSampleCpu {
    float pos[3];
    float dir[3];
    float normal[3];
    float albedo[3];
    float roughness;
    float target[3];
    uint32_t valid;
};

// How many training records one frame may produce. At 1280x720 with one pixel
// in 64 selected that is about 14k, so 64k leaves room for a 4K window without
// the tracer ever having to check whether it has run out -- the buffer is 4 MB,
// which is nothing beside the 326 MB triangle pool.
static constexpr uint32_t kNrcMaxSamples = 1u << 16;

class Nrc {
  public:
    // ---- knobs, all live from the settings menu -------------------------
    bool enabled = false;         // query the cache from the tracer
    bool training = true;         // keep learning while it is on
    float learningRate = 0.01f;
    float momentum = 0.9f;
    // Where along the path the cache takes over. Two means the camera ray and
    // one real bounce are always traced -- everything you can directly see is
    // still the path tracer, and only the tail is predicted.
    int queryDepth = 2;
    // How much of the frame is spent generating training data. One pixel in
    // this many runs to full depth and writes a record.
    int trainEvery = 64;

    // The cache is only asked for radiance after this many batches. Roughly a
    // second of walking at 60 fps.
    static constexpr uint32_t kWarmupBatches = 64;

    bool init(const Falcor::ref<Falcor::Device> &device, uint32_t maxSamples) {
        device_ = device;
        maxSamples_ = maxSamples;

        try {
            Falcor::ProgramDesc dt;
            dt.addShaderLibrary("v7/shaders/NrcTrain.cs.slang").csEntry("main");
            nrcAddCapability(device_, dt);
            train_ = Falcor::ComputePass::create(device_, dt);

            Falcor::ProgramDesc du;
            du.addShaderLibrary("v7/shaders/NrcUpdate.cs.slang").csEntry("main");
            nrcAddCapability(device_, du);
            update_ = Falcor::ComputePass::create(device_, du);
        } catch (const std::exception &e) {
            status_ = std::string("training shaders did not compile: ") + e.what();
            return false;
        }

        using Falcor::ResourceBindFlags;
        const auto kUav = ResourceBindFlags::UnorderedAccess | ResourceBindFlags::ShaderResource;

        weights_ = device_->createBuffer(NrcLayout::kBytes, kUav, Falcor::MemoryType::DeviceLocal);
        grad_ = device_->createBuffer(NrcLayout::kBytes, kUav, Falcor::MemoryType::DeviceLocal);
        velocity_ = device_->createBuffer(NrcLayout::kBytes, kUav, Falcor::MemoryType::DeviceLocal);
        samples_ = device_->createStructuredBuffer(sizeof(NrcSampleCpu), maxSamples_, kUav);

        reset();
        ready_ = true;
        status_ = "ready";
        return true;
    }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }
    uint32_t batches() const { return batches_; }
    bool warm() const { return batches_ >= kWarmupBatches; }

    // What the tracer should actually do this frame. Both halves can be false:
    // a cache that is on but cold still collects training data while the
    // renderer behaves exactly as it did without it.
    bool shouldQuery() const { return ready_ && enabled && warm(); }
    bool shouldTrain() const { return ready_ && enabled && training; }

    const Falcor::ref<Falcor::Buffer> &weightBuffer() const { return weights_; }
    const Falcor::ref<Falcor::Buffer> &sampleBuffer() const { return samples_; }

    // ---------------------------------------------------------------------
    // Start again from noise. Called on init, and from the menu when the cache
    // has been poisoned -- which does happen: fp16 training can diverge, and
    // when it does every query is NaN and the only cure is a fresh network.
    // ---------------------------------------------------------------------
    void reset() {
        if (!weights_) return;
        std::vector<uint16_t> w(NrcLayout::kElems, 0);

        // He initialisation: normal with variance 2/fan_in, which is the
        // variance that leaves a ReLU layer's output with the same scale as its
        // input. Biases start at zero -- a nonzero bias here just costs the
        // network its first few hundred batches unlearning it.
        std::mt19937 rng(0x7C0FFEEu);
        auto fill = [&](uint32_t byteOffset, int rows, int cols) {
            std::normal_distribution<float> d(0.0f, std::sqrt(2.0f / float(cols)));
            for (int k = 0; k < rows * cols; ++k)
                w[byteOffset / 2 + k] = f32ToF16(d(rng));
        };
        fill(NrcLayout::kW0, NrcLayout::kHidden, NrcLayout::kIn);
        fill(NrcLayout::kW1, NrcLayout::kHidden, NrcLayout::kHidden);
        fill(NrcLayout::kW2, NrcLayout::kOut, NrcLayout::kHidden);

        weights_->setBlob(w.data(), 0, NrcLayout::kBytes);
        std::vector<uint16_t> zero(NrcLayout::kElems, 0);
        grad_->setBlob(zero.data(), 0, NrcLayout::kBytes);
        velocity_->setBlob(zero.data(), 0, NrcLayout::kBytes);
        batches_ = 0;
    }

    // ---------------------------------------------------------------------
    // One batch: fit to the samples the tracer just wrote, then step.
    //
    // `count` is how many records the tracer produced. Passed in rather than
    // read back from the GPU deliberately -- a readback here would stall the
    // frame on the device to learn a number the CPU could have counted itself,
    // and the tracer knows it exactly: one per pixel that was selected.
    // ---------------------------------------------------------------------
    void trainBatch(Falcor::RenderContext *ctx, uint32_t count) {
        if (!ready_ || count == 0) return;
        count = std::min(count, maxSamples_);

        {
            auto var = train_->getRootVar();
            var["gSamples"] = samples_;
            var["gWeights"] = weights_;
            var["gGrad"] = grad_;
            var["NrcTrainCB"]["gSampleCount"] = count;
            // ONE GROUP PER SAMPLE, and the group is one subgroup wide -- see
            // the note at the top of NrcTrain.cs.slang. execute() takes a
            // thread count, so this is groups x 32.
            train_->execute(ctx, count * 32, 1, 1);
        }
        {
            auto var = update_->getRootVar();
            var["gWeights"] = weights_;
            var["gGrad"] = grad_;
            var["gVelocity"] = velocity_;
            var["NrcUpdateCB"]["gLearningRate"] = learningRate;
            var["NrcUpdateCB"]["gMomentum"] = momentum;
            var["NrcUpdateCB"]["gBatchSize"] = count;
            var["NrcUpdateCB"]["gWeightCount"] = NrcLayout::kElems;
            update_->execute(ctx, NrcLayout::kElems, 1, 1);
        }
        ++batches_;
    }

  private:
    // A plain IEEE binary32 -> binary16. Falcor has a half type, but pulling
    // its math headers in for fourteen kilobytes of one-off conversion is more
    // coupling than the twenty lines cost.
    static uint16_t f32ToF16(float f) {
        uint32_t x;
        std::memcpy(&x, &f, 4);
        const uint32_t sign = (x >> 16) & 0x8000u;
        int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
        uint32_t man = x & 0x7FFFFFu;
        if (exp <= 0) return uint16_t(sign);              // underflow to zero
        if (exp >= 31) return uint16_t(sign | 0x7BFFu);   // clamp, never inf
        return uint16_t(sign | (uint32_t(exp) << 10) | (man >> 13));
    }

    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> train_, update_;
    Falcor::ref<Falcor::Buffer> weights_, grad_, velocity_, samples_;
    uint32_t maxSamples_ = 0;
    uint32_t batches_ = 0;
    bool ready_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
