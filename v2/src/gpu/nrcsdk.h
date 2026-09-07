// ---------------------------------------------------------------------------
// nrcsdk.h -- NVIDIA's Neural Radiance Cache (RTXGI 2.x), the capability gate
//             and the context.
//
// NOT THE SAME THING AS gpu/nrc.h, and both are in the tree on purpose.
//
//   nrc.h      v2's own network: 32 inputs, two hidden layers, cooperative
//              vectors, and an encoding built out of voxel identity rather than
//              position -- which is what lets its weights transfer between
//              worlds. It is the interesting one and it is also the one that
//              still walks into its weight clamp past a few thousand batches.
//
//   nrcsdk.h   NVIDIA's, as a binary library. Production code, someone else's
//              optimiser, no encoding hook of any kind.
//
// ---------------------------------------------------------------------------
// WHY IT IS WORTH HAVING THE SECOND ONE, and the reason is not image quality.
//
// v2's cache needs cooperative vectors, which need Shader Model 6.10, which
// this machine's D3D12 does not report without the preview Agility runtime --
// so the neural path only runs under `--vulkan`, and the Vulkan backend has no
// DLSS, no Ray Reconstruction, no clusters and no probes. The engine has been
// made to choose between a neural cache and a denoiser.
//
// This library does its training on TENSOR CORES through its own DLL and CUDA,
// not through cooperative vectors in a shader. It has no shader model floor to
// clear, so it runs on the D3D12 device the rest of the stack already wants.
// That is the whole argument: not a faster frame -- measured at 6-8% on v2's
// own cache and expected to be similar here -- but a neural cache on the SAME
// backend as everything else for the first time.
//
// ---------------------------------------------------------------------------
// THE SCENE BOUNDS ARE THE AWKWARD PART, AND IT IS WORTH KNOWING UP FRONT.
//
// ContextSettings wants sceneBoundsMin/Max and says the whole scene -- anything
// a ray can hit -- must be inside them. This world does not have that shape: it
// is endless, streamed, and the resident part is a ring of chunks that MOVES
// WITH THE CAMERA. There is no static box to hand over.
//
// So the bounds follow the camera, and Configure() -- which reallocates and
// generally resets the cache -- would be called every time the ring shifts if
// it were sized tightly. It is deliberately sized LOOSE instead: see
// kBoundsMargin below for what that costs and why it is the cheaper mistake.
// ---------------------------------------------------------------------------
#pragma once

#if V2_HAS_NRCSDK

#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"
#include "Core/API/Buffer.h"

#include <d3d12.h>

#include "NrcD3d12.h"
#include "NrcSecurity.h"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>

#include "../core/vecmath.h"

namespace v2 {

// HOW FAR PAST THE RESIDENT RING THE BOUNDS REACH, in metres.
//
// The cache's spatial resolution is derived from the bounds together with
// smallestResolvableFeatureSize, so a bigger box is a coarser cache -- this is
// not free. It is still the right trade here. A tight box would have to be
// re-issued through Configure() every time the chunk ring scrolled, and
// Configure() resets the network: the cache would be permanently re-learning
// the wood instead of occasionally being slightly coarse.
//
// A 12-chunk radius is 307 m, so 1 km of margin is about three ring widths --
// enough that walking in a straight line goes a long way before the box has to
// move at all.
static constexpr float kNrcSdkBoundsMargin = 1000.0f;

// THE VERTICAL EXTENT IS FIXED AND SMALL, because the world is. Terrain plus
// canopy plus the sky the rays escape into is a few hundred metres; spending
// bounds on empty air above it would coarsen the cache for nothing.
static constexpr float kNrcSdkWorldFloor = -256.0f;
static constexpr float kNrcSdkWorldCeil = 768.0f;

class NrcSdk {
  public:
    bool enabled = false;

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }

    // -----------------------------------------------------------------------
    // Bring the library up. Returns false and leaves a reason in status().
    //
    // EVERY FAILURE HERE IS SURVIVABLE and none of them is allowed to take the
    // engine down, which is the same rule ddgi, dlss, clusters and neural
    // already follow: a machine without the driver, or with a driver too old,
    // renders the wood exactly as it did before and says so at startup.
    // -----------------------------------------------------------------------
    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

        // THE DLL IS SIGNATURE-CHECKED BEFORE IT IS TRUSTED. The SDK ships the
        // check for a reason: this is a binary blob that will be handed the
        // D3D12 device and a command list, and it loads CUDA behind us.
        const std::string dll =
            (Falcor::getRuntimeDirectory() / "NRC_D3D12.dll").string();
        const std::wstring wdll(dll.begin(), dll.end());
        if (!nrc::security::VerifySignature(wdll.c_str())) {
            status_ = "NRC_D3D12.dll failed its signature check";
            return false;
        }

        nrc::GlobalSettings gs;
        // LET THE SDK OWN ITS BUFFERS. The alternative is calling
        // GetBuffersAllocationInfo and creating eight resources by hand, and
        // the only thing it buys is placing them in our own heap -- which this
        // engine has no reason to want. GetBuffers() still hands back the
        // resources so the tracer can bind them.
        // THE APP OWNS THE BUFFERS, and this is not a preference.
        //
        // Left to itself the SDK allocates them and hands back bare
        // ID3D12Resource pointers -- which Falcor cannot bind, because a
        // ShaderVar takes a Falcor::Buffer and there is no way to wrap a
        // foreign resource in one. The tracer has to READ these buffers, so
        // they have to be Falcor's.
        //
        // So GetBuffersAllocationInfo tells us the shape, we create ordinary
        // Falcor structured buffers, and Configure is handed their native
        // pointers back. Falcor owns the lifetime and the binding; the SDK just
        // writes into them.
        gs.enableGPUMemoryAllocation = false;
        // ON, because some of the debug resolve modes are backed by them and
        // silently draw black without -- DirectCacheView, which is the SDK's
        // own recommended correctness check, is one of them.
        gs.enableDebugBuffers = true;
        // Falcor keeps a small number of frames in flight; the SDK only uses
        // this to size its own ring of staging allocations, so the conservative
        // default is left alone.
        gs.maxNumFramesInFlight = 4;
        gs.loggerFn = &NrcSdk::logThunk;

        logSink_ = this;
        nrc::Status st = nrc::d3d12::Initialize(gs);
        if (st != nrc::Status::OK) {
            status_ = "Initialize failed: " + statusName(st) + lastLog();
            return false;
        }
        initialised_ = true;

        // ID3D12Device5, which is what the SDK signs for, reached the same way
        // clusters.h reaches NVAPI: Falcor hands out the plain ID3D12Device and
        // QueryInterface is the version that can say no.
        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "no native ID3D12Device (is this the Vulkan backend?)";
            return false;
        }
        ID3D12Device5 *dev5 = nullptr;
        if (FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dev5))) || !dev5) {
            status_ = "device is not an ID3D12Device5";
            return false;
        }

        st = nrc::d3d12::Context::Create(dev5, ctx_);
        dev5->Release();
        if (st != nrc::Status::OK || !ctx_) {
            status_ = "Context::Create failed: " + statusName(st) + lastLog();
            return false;
        }

        ready_ = true;
        status_ = "RTXGI " + std::to_string(NRC_VERSION_MAJOR) + "." +
                  std::to_string(NRC_VERSION_MINOR) + " on tensor cores";
        return true;
    }

    // The network is NOT created by init() -- Configure does that, and it is
    // the call that allocates. Kept separate because it also has to run again
    // whenever the frame size or the scene bounds change.
    void shutdown() {
        if (ctx_) {
            nrc::d3d12::Context::Destroy(*ctx_);
            ctx_ = nullptr;
        }
        if (initialised_) {
            nrc::d3d12::Shutdown();
            initialised_ = false;
        }
        ready_ = false;
        logSink_ = nullptr;
    }

    ~NrcSdk() { shutdown(); }

    nrc::d3d12::Context *context() const { return ctx_; }

    // -----------------------------------------------------------------------
    // (Re)build the network. Expensive, resets the cache, and therefore called
    // only when the frame size or the scene box actually changes.
    //
    // THE BOX IS SNAPPED, and that is what stops this running every frame. The
    // camera moves continuously; snapping its centre to a grid one margin wide
    // means the box only changes after the camera has travelled a whole margin,
    // so an ordinary walk reconfigures every few minutes rather than never
    // settling. See kNrcSdkBoundsMargin for why the box is loose to begin with.
    // -----------------------------------------------------------------------
    bool configure(uint32_t w, uint32_t h, const Vec3 &cameraPos, uint32_t maxPathVertices) {
        if (!ready_) return false;

        const float snap = kNrcSdkBoundsMargin;
        const float cx = std::floor(cameraPos.x / snap) * snap;
        const float cz = std::floor(cameraPos.z / snap) * snap;

        nrc::ContextSettings cs;
        // FALSE, because v2 hands the cache radiance and not irradiance --
        // learnIrradiance wants albedo demodulation applied first and the
        // tracer's demodulate pass runs after this, not before.
        cs.learnIrradiance = false;
        // TRUE, because the point of the cache here is early path termination:
        // a path that stops at a cached vertex has to be given everything that
        // leaves it, direct light included, or the wood loses its sun.
        cs.includeDirectLighting = true;
        cs.sceneBoundsMin = {cx - snap, kNrcSdkWorldFloor, cz - snap};
        cs.sceneBoundsMax = {cx + snap, kNrcSdkWorldCeil, cz + snap};
        // A voxel is 10 cm and nothing in this world is smaller, so asking the
        // cache to resolve below that is spending its resolution on detail the
        // geometry does not have.
        cs.smallestResolvableFeatureSize = 0.1f;
        cs.frameDimensions = {w, h};
        cs.samplesPerPixel = 1;
        cs.maxPathVertices = maxPathVertices;
        cs.trainingDimensions =
            nrc::ComputeIdealTrainingDimensions({w, h}, kTrainingIterations, 0.0f);

        if (cs == applied_ && configured_) return true;

        std::printf("  nrc dims frame %ux%u  training %ux%u  spp %u  maxVerts %u\n",
                    cs.frameDimensions.x, cs.frameDimensions.y,
                    cs.trainingDimensions.x, cs.trainingDimensions.y,
                    cs.samplesPerPixel, cs.maxPathVertices);
        if (!allocateBuffers(cs)) return false;

        const nrc::Status st = ctx_->Configure(cs, &native_);
        if (st != nrc::Status::OK) {
            status_ = "Configure failed: " + statusName(st) + lastLog();
            ready_ = false;
            return false;
        }
        applied_ = cs;
        configured_ = true;
        trainW_ = cs.trainingDimensions.x;
        trainH_ = cs.trainingDimensions.y;
        return true;
    }

    uint32_t trainingWidth() const { return trainW_; }
    uint32_t trainingHeight() const { return trainH_; }
    bool configured() const { return ready_ && configured_; }

    // The buffers the tracer binds, as Falcor objects -- see the note on
    // enableGPUMemoryAllocation for why they are ours and not the SDK's.
    const Falcor::ref<Falcor::Buffer> &buffer(nrc::BufferIdx idx) const {
        return owned_[(int)idx];
    }

    // -----------------------------------------------------------------------
    // The four per-frame calls, in the order the guide fixes them:
    //   beginFrame -> [the two path tracer passes] -> queryAndTrain -> resolve
    // and endFrame once the command list has been submitted.
    // -----------------------------------------------------------------------
    bool beginFrame(Falcor::RenderContext *rc, float) {
        if (!configured()) return false;
        ID3D12GraphicsCommandList4 *cmd = cmdList(rc);
        if (!cmd) return false;

        nrc::FrameSettings fs;
        // THE SCALE THE NETWORK LEARNS IN. The library rescales radiance into a
        // friendly range by this before fitting, so a value far from what the
        // renderer actually produces trains the network on targets it cannot
        // represent. This wood's sun is a five-figure radiance, so 1.0 -- the
        // SDK's default -- is not obviously right and is worth sweeping.
        fs.maxExpectedAverageRadianceValue = maxRadiance;
        fs.usedTrainingDimensions = {trainW_, trainH_};
        fs.numTrainingIterations = kTrainingIterations;
        fs.trainTheCache = training;
        fs.learningRate = learningRate;
        // The tracer adds the cache's answer into its own accumulation, so the
        // SDK's resolve must ADD rather than replace -- the other modes are the
        // debug visualisations.
        // Global enum, not nrc::-scoped -- it lives in NrcStructures.h, which is
        // the header the SHADER side shares.
        // DEBUG MODES OVERWRITE THE OUTPUT rather than adding to it, which is
        // what makes them worth having: DirectCacheView is the SDK's own
        // recommended check that an integration is correct, and it shows what
        // the network has actually learnt with nothing of ours on top.
        fs.resolveMode = (debugMode > 0)
                             ? NrcResolveMode(debugMode)
                             : NrcResolveMode::AddQueryResultToOutput;

        const nrc::Status st = ctx_->BeginFrame(cmd, fs);
        cmd->Release();
        return st == nrc::Status::OK;
    }

    bool queryAndTrain(Falcor::RenderContext *rc) {
        if (!configured()) return false;
        ID3D12GraphicsCommandList4 *cmd = cmdList(rc);
        if (!cmd) return false;
        const nrc::Status st = ctx_->QueryAndTrain(cmd, &loss_);
        cmd->Release();
        return st == nrc::Status::OK;
    }

    bool resolve(Falcor::RenderContext *rc, Falcor::Texture *out) {
        if (!configured() || !out) return false;
        ID3D12GraphicsCommandList4 *cmd = cmdList(rc);
        if (!cmd) return false;
        ID3D12Resource *res = out->getNativeHandle().as<ID3D12Resource *>();
        const nrc::Status st = res ? ctx_->Resolve(cmd, res) : nrc::Status::WrongParameter;
        cmd->Release();
        return st == nrc::Status::OK;
    }

    // AFTER THE COMMAND LIST HAS BEEN SUBMITTED, and it must be the same queue
    // every previous list went to. Falcor has exactly one.
    bool endFrame() {
        if (!configured()) return false;
        gfx::ICommandQueue *q = device_->getGfxCommandQueue();
        if (!q) return false;
        gfx::InteropHandle h = {};
        if (SLANG_FAILED(q->getNativeHandle(&h)) || h.handleValue == 0) return false;
        auto *native = reinterpret_cast<ID3D12CommandQueue *>(h.handleValue);
        return ctx_->EndFrame(native) == nrc::Status::OK;
    }

    // The constant block the shader reads. Derived by the library from the
    // context and frame settings, so it is asked for rather than assembled.
    bool populateConstants(NrcConstants &out) const {
        return configured() && ctx_->PopulateShaderConstants(out) == nrc::Status::OK;
    }

    float trainingLoss() const { return loss_; }

    // WHAT THE PATH TRACER ACTUALLY WROTE, read straight out of the SDK's
    // counter buffer: [0] query records, [1] training records.
    //
    // Worth more than the training loss, which stays at zero in this
    // configuration whether or not anything is happening -- a number that
    // cannot distinguish "not training" from "not reported" is not a
    // diagnostic. These two can: zero here means the shader wrote nothing.
    void readCounters(uint32_t &queryRecs, uint32_t &trainRecs) const {
        queryRecs = trainRecs = 0;
        const auto &b = owned_[(int)nrc::BufferIdx::Counter];
        if (!b) return;
        uint32_t raw[2] = {0, 0};
        b->getBlob(raw, 0, sizeof(raw));
        queryRecs = raw[0];
        trainRecs = raw[1];
    }

    // ---- knobs -----------------------------------------------------------
    bool training = true;
    // 0 = normal. Otherwise an NrcResolveMode: 1 replaces the output with the
    // query result, 11 is DirectCacheView. See NrcStructures.h.
    int debugMode = 0;
    float maxRadiance = 1.0f;
    float learningRate = 1e-2f;

  private:
    // FOUR, the SDK's own default. It sizes the training buffers, so changing
    // it means reconfiguring; the frame-settings copy can be lowered later to
    // train less per frame without touching the allocation.
    static constexpr uint32_t kTrainingIterations = 4;

    // Falcor hands out an ID3D12GraphicsCommandList; the SDK signs for a 4.
    // QueryInterface rather than a cast, for the reason clusters.h gives: a
    // reinterpret_cast cannot say no.
    static ID3D12GraphicsCommandList4 *cmdList(Falcor::RenderContext *rc) {
        if (!rc) return nullptr;
        auto *base = rc->getLowLevelData()
                         ->getCommandBufferNativeHandle()
                         .as<ID3D12GraphicsCommandList *>();
        ID3D12GraphicsCommandList4 *cmd4 = nullptr;
        if (!base || FAILED(base->QueryInterface(IID_PPV_ARGS(&cmd4)))) return nullptr;
        return cmd4;
    }

    // -----------------------------------------------------------------------
    // Create one Falcor buffer per NRC buffer, to the SDK's own measurements.
    //
    // A COUNT OF ZERO IS LEGAL and means the buffer is not needed in this
    // configuration -- the debug training path info is exactly that when
    // enableDebugBuffers is off. Creating a zero-element structured buffer
    // fails, so those are left null and handed over as null, which is what the
    // SDK expects.
    // -----------------------------------------------------------------------
    bool allocateBuffers(const nrc::ContextSettings &cs) {
        nrc::BuffersAllocationInfo info{};
        if (nrc::d3d12::Context::GetBuffersAllocationInfo(cs, info) != nrc::Status::OK) {
            status_ = "GetBuffersAllocationInfo failed" + lastLog();
            return false;
        }

        using Falcor::ResourceBindFlags;
        for (int i = 0; i < (int)nrc::BufferIdx::Count; ++i) {
            const nrc::AllocationInfo &ai = info.allocationInfo[i];
            owned_[i] = nullptr;
            native_.buffers[i].resource = nullptr;
            native_.buffers[i].allocatedSize = 0;
            if (ai.elementCount == 0 || ai.elementSize == 0) continue;

            // ShaderResource as well as UnorderedAccess: the tracer writes
            // records, and the resolve reads them back.
            auto flags = ResourceBindFlags::ShaderResource;
            if (ai.allowUAV) flags |= ResourceBindFlags::UnorderedAccess;

            owned_[i] = device_->createStructuredBuffer(
                uint32_t(ai.elementSize), uint32_t(ai.elementCount), flags,
                Falcor::MemoryType::DeviceLocal, nullptr, false);
            if (!owned_[i]) {
                status_ = std::string("could not allocate ") +
                          (ai.debugName ? ai.debugName : "an NRC buffer");
                return false;
            }
            owned_[i]->setName(std::string("v2::nrcsdk::") +
                               (ai.debugName ? ai.debugName : std::to_string(i)));
            native_.buffers[i].resource =
                owned_[i]->getNativeHandle().as<ID3D12Resource *>();
            native_.buffers[i].allocatedSize = ai.elementCount * ai.elementSize;
            // Printed once per configure. The element sizes are what a custom
            // resolve pass has to declare its buffers as, and guessing them is
            // a silently wrong read rather than an error.
            std::printf("  nrc buf  %-24s %7zu x %3zu bytes\n",
                        ai.debugName ? ai.debugName : "?", ai.elementCount,
                        ai.elementSize);
        }
        std::fflush(stdout);
        return true;
    }

    Falcor::ref<Falcor::Buffer> owned_[(int)nrc::BufferIdx::Count];
    nrc::d3d12::Buffers native_{};
    nrc::ContextSettings applied_{};
    bool configured_ = false;
    uint32_t trainW_ = 0, trainH_ = 0;
    float loss_ = 0.0f;

    // The SDK's logger is a plain function pointer with no user data, so the
    // instance has to be reachable from a static. There is one NrcSdk in the
    // app and it outlives every call the SDK makes into this.
    static NrcSdk *logSink_;

    static void logThunk(const char *msg, nrc::LogLevel level) {
        if (!logSink_ || !msg) return;
        // Only the bad news is kept. The SDK is chatty at Info during
        // configuration and none of it belongs in a startup line.
        if (level == nrc::LogLevel::Warning || level == nrc::LogLevel::Error)
            logSink_->log_ = msg;
    }

    std::string lastLog() const { return log_.empty() ? "" : (" -- " + log_); }

    static std::string statusName(nrc::Status s) {
        switch (s) {
        case nrc::Status::OK: return "OK";
        case nrc::Status::SDKVersionMismatch: return "SDK version mismatch";
        case nrc::Status::AlreadyInitialized: return "already initialised";
        case nrc::Status::SDKNotInitialized: return "not initialised";
        case nrc::Status::InternalError: return "internal error";
        case nrc::Status::MemoryNotProvided: return "memory not provided";
        case nrc::Status::OutOfMemory: return "out of memory";
        case nrc::Status::AllocationFailed: return "allocation failed";
        case nrc::Status::ErrorParsingJSON: return "malformed JSON";
        case nrc::Status::WrongParameter: return "wrong parameter";
        case nrc::Status::UnsupportedDriver: return "driver too old";
        case nrc::Status::UnsupportedHardware: return "GPU not supported";
        default: return "unknown status";
        }
    }

    Falcor::ref<Falcor::Device> device_;
    nrc::d3d12::Context *ctx_ = nullptr;
    bool initialised_ = false;
    bool ready_ = false;
    std::string status_ = "not initialised";
    std::string log_;
};

inline NrcSdk *NrcSdk::logSink_ = nullptr;

} // namespace v2

#endif // V2_HAS_NRCSDK
