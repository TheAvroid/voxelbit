// ---------------------------------------------------------------------------
// ddgi.h -- PHASE A, the indirect half: NVIDIA RTXGI 1.3 irradiance probes.
//
// WHAT THIS BUYS, and why a path tracer that already does multi-bounce wants it
// at all. v4's integrator gets its indirect light the honest way: it keeps
// bouncing. That is correct and it is also where the noise lives -- a second
// bounce in a conifer canopy is a ray that has already lost 80 % of its energy
// to foliage albedo, so it contributes little and varies enormously, which is
// the worst possible ratio. Every frame pays full price for it and the result
// is still grainy.
//
// DDGI moves that integral off the camera path and into a grid of probes that
// remember. Each probe traces a few rays a frame in every direction, blends the
// result into an octahedral irradiance map with an exponential moving average,
// and from then on ANY shading point can ask "how much light arrives here from
// that direction" and get an answer that is the accumulated work of hundreds of
// past frames for the cost of one texture fetch. The camera path then only has
// to go ONE bounce before it can terminate into the probes.
//
// So the trade is: a fixed per-frame probe cost that does not scale with screen
// resolution, in exchange for shorter paths and far less variance in the part of
// the image that was noisiest. On an endless world the volume SCROLLS with the
// player rather than covering the scene, so the cost stays fixed as you walk.
//
// ---------------------------------------------------------------------------
// WHY UNMANAGED RESOURCE MODE.
//
// RTXGI offers to create its own probe textures ("managed mode"), and taking
// that offer would be a mistake here. v7's own Slang shaders touch those
// textures -- ProbeTrace fills the ray data, Trace samples the irradiance --
// and Falcor inserts barriers from a resource-state model it maintains itself.
// A texture Falcor did not create is absent from that model, so every barrier
// around every probe dispatch would silently be missing. That is the same class
// of bug the NGX interop in dlss.h has to hand-manage, and here it is avoidable
// outright.
//
// So Falcor creates the six probe textures and RTXGI is handed their native
// handles. The price is a root signature and eight pipeline states of ordinary
// D3D12 boilerplate, paid once in init(), which is cheap for a barrier model
// that stays correct.
//
// ---------------------------------------------------------------------------
// EVERY FAILURE PATH ENDS IN "NO DDGI", NEVER IN A CRASH.
//
// The probe shaders need Shader Model 6.6 and wave intrinsics. A build without
// the SDK, a driver too old, a GPU without wave ops, a missing .cso, a volume
// the SDK refuses to create -- each is caught, recorded in status(), and turns
// into available() == false. The engine then renders exactly as v4 did. An
// indirect-lighting cache that takes the program down when a driver is old is a
// worse cache than none.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "../core/vecmath.h"

#if V7_HAS_DDGI
#include <d3d12.h>
#include <rtxgi/ddgi/DDGIVolume.h>
#include <rtxgi/ddgi/gfx/DDGIVolume_D3D12.h>
#endif

namespace v7 {

// ---------------------------------------------------------------------------
// THE VOLUME'S SHAPE, AND IT IS A CONTRACT WITH THE SHADER COMPILER.
//
// The ray count and both texel counts are compiled INTO RTXGI's probe shaders
// as loop bounds and groupshared array sizes -- see compile_ddgi_shaders.bat,
// which passes these same numbers as -D defines. If the two ever disagree, the
// blend shader indexes a groupshared array past its end, which is not a crash
// and not a validation error: it is a wrong probe, somewhere, sometimes.
//
// So they are written down once here and asserted against the batch file's
// values by a comment nobody can honour -- which is why v7 goes further and
// prints them at startup under --pipeline, where a mismatch shows up as a wrong
// number rather than a wrong picture.
// ---------------------------------------------------------------------------
static constexpr int kProbeNumRays = 192;       // rays cast per probe per frame
static constexpr int kProbeIrrTexels = 8;       // irradiance, including border
static constexpr int kProbeIrrInterior = 6;     // ... excluding it
static constexpr int kProbeDistTexels = 16;     // distance, including border
static constexpr int kProbeDistInterior = 14;   // ... excluding it

// The grid. 20 x 8 x 20 probes at 6 m is a box 120 m wide and 48 m tall, which
// is about the distance a trunk still reads as a trunk through this much haze.
//
// THE COST IS THE PRODUCT, and it is the one number to watch: 3200 probes at
// 192 rays is 614k rays a frame, fixed, regardless of what resolution the
// camera is traced at. Doubling the grid doubles it. Beyond about a million the
// probes cost more than the bounces they replace and the whole exchange stops
// being worth making.
static constexpr int kProbeCountX = 20;
static constexpr int kProbeCountY = 8;
static constexpr int kProbeCountZ = 20;
static constexpr float kProbeSpacing = 6.0f;    // metres

#if !V7_HAS_DDGI
// ---------------------------------------------------------------------------
// Built without the SDK: the same class, permanently unavailable.
//
// A stub rather than #ifdefs at the call sites. The engine already has to
// handle "this driver cannot do it" at runtime, so having it also handle "this
// build does not have it" costs nothing and keeps the frame loop readable --
// the same argument dlss.h makes for the same shape.
// ---------------------------------------------------------------------------
class Ddgi {
  public:
    bool available() const { return false; }
    const std::string &status() const { return status_; }
    bool init(const Falcor::ref<Falcor::Device> &, const std::filesystem::path &) { return false; }
    void setOrigin(Vec3) {}
    int numProbes() const { return 0; }
    int raysPerProbe() const { return 0; }
    Falcor::Texture *rayData() const { return nullptr; }
    Falcor::Texture *irradiance() const { return nullptr; }
    Falcor::Texture *distance() const { return nullptr; }
    Falcor::Texture *probeData() const { return nullptr; }
    bool uploadConstants(Falcor::RenderContext *) { return false; }
    bool updateProbes(Falcor::RenderContext *) { return false; }
    void shutdown() {}

  private:
    std::string status_ = "built without the RTXGI SDK (set V7_RTXGI_DIR)";
};
#else

class Ddgi {
  public:
    ~Ddgi() { shutdown(); }

    bool available() const { return ready_; }
    const std::string &status() const { return status_; }
    int numProbes() const { return kProbeCountX * kProbeCountY * kProbeCountZ; }
    int raysPerProbe() const { return kProbeNumRays; }

    Falcor::Texture *rayData() const { return texRayData_.get(); }
    Falcor::Texture *irradiance() const { return texIrradiance_.get(); }
    Falcor::Texture *distance() const { return texDistance_.get(); }
    Falcor::Texture *probeData() const { return texProbeData_.get(); }

    // Where the probe grid is centred. Called every frame with the player's
    // position, snapped so the volume does not shimmer as they walk -- see
    // setOrigin().
    Vec3 origin() const { return origin_; }

    // -----------------------------------------------------------------------
    // Bring the volume up. Failure is never fatal.
    // -----------------------------------------------------------------------
    bool init(const Falcor::ref<Falcor::Device> &device, const std::filesystem::path &shaderDir) {
        device_ = device;

        // Asked of the DEVICE, not of the handle -- NativeHandle::as<T> only
        // reinterprets, so on Vulkan a VkDevice arrives here as a non-null
        // ID3D12Device* and every call below it is undefined. See dlss.h.
        if (device_->getType() != Falcor::Device::Type::D3D12) {
            status_ = "not a D3D12 device (RTXGI here is D3D12 only)";
            return false;
        }
        d3d_ = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d_) {
            status_ = "no D3D12 device handle";
            return false;
        }

        // -- capability: Shader Model 6.6 ------------------------------------
        // RTXGI's probe shaders are compiled cs_6_6. Asked BEFORE anything is
        // allocated, because the failure this prevents is a PSO creation error
        // deep in volume creation that says nothing about shader models.
        D3D12_FEATURE_DATA_SHADER_MODEL sm{};
        sm.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        if (FAILED(d3d_->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm))) ||
            sm.HighestShaderModel < D3D_SHADER_MODEL_6_6) {
            status_ = "driver does not support Shader Model 6.6";
            return false;
        }

        // -- capability: wave intrinsics -------------------------------------
        // The variability reduction is a wave-level tree and the blend shaders
        // use wave ops for the border copy.
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 o1{};
        if (FAILED(d3d_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &o1, sizeof(o1))) ||
            !o1.WaveOps) {
            status_ = "GPU does not report wave intrinsics";
            return false;
        }
        // The reduction shader was compiled for a 32-lane wave. Every NVIDIA
        // part is 32, but saying so out loud is what turns a silently wrong
        // average into a refusal on hardware that is not.
        if (o1.WaveLaneCountMin > 32 || o1.WaveLaneCountMax < 32) {
            status_ = "wave width " + std::to_string(o1.WaveLaneCountMin) + ".." +
                      std::to_string(o1.WaveLaneCountMax) + ", probe shaders were built for 32";
            return false;
        }

        describeVolume();

        if (!createTextures()) return false;      // Falcor owns these
        if (!createConstantsBuffers()) return false;
        if (!createDescriptorHeap()) return false;
        if (!loadShaders(shaderDir)) return false;
        if (!createRootSignature()) return false;
        if (!createPipelines()) return false;
        if (!createVolume()) return false;

        ready_ = true;
        status_ = "ready";
        return true;
    }

    // -----------------------------------------------------------------------
    // Move the grid with the player.
    //
    // SNAPPED TO THE PROBE SPACING, and that is the whole trick of a scrolling
    // volume. Probes are a cache keyed by world position; if the grid slid
    // continuously the key would change every frame and nothing would ever be
    // reused. Snapping means the grid only ever jumps by exactly one probe, so
    // all but one plane of probes keep their history and only the newly exposed
    // plane has to be relearned.
    // -----------------------------------------------------------------------
    void setOrigin(Vec3 p) {
        const float s = kProbeSpacing;
        origin_ = Vec3(floorf(p.x / s + 0.5f) * s,
                       floorf(p.y / s + 0.5f) * s,
                       floorf(p.z / s + 0.5f) * s);
        if (volume_) {
            volume_->SetScrollAnchor({origin_.x, origin_.y, origin_.z});
        }
    }

    // -----------------------------------------------------------------------
    // Per frame, before the probe rays are traced: hand the SDK this frame's
    // constants and let it pick a new random rotation for the probe rays.
    //
    // THE ROTATION IS NOT DECORATION. Every probe traces the same fixed
    // spherical Fibonacci pattern, and a fixed pattern samples the same 192
    // directions forever -- so anything that falls between them is never seen.
    // Rotating the whole set by a random quaternion each frame turns those 192
    // rays into a different 192 every frame, and the exponential blend below
    // integrates over all of them.
    // -----------------------------------------------------------------------
    bool uploadConstants(Falcor::RenderContext *ctx) {
        if (!ready_) return false;

        volume_->SetOrigin({origin_.x, origin_.y, origin_.z});
        volume_->Update();  // new random ray rotation, scroll bookkeeping

        ID3D12GraphicsCommandList *cmd = nativeCmdList(ctx);
        if (!cmd) return false;

        rtxgi::d3d12::DDGIVolume *vols[] = {volume_};
        const rtxgi::ERTXGIStatus s = rtxgi::d3d12::UploadDDGIVolumeConstants(
            d3d_, cmd, uint32_t(frameIndex_ % kUploadRing), 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "constants upload failed (" + std::to_string(int(s)) + ")";
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Per frame, AFTER the probe rays have been traced into rayData(): blend
    // them into the irradiance and distance atlases, then relocate and classify.
    //
    // ORDER MATTERS AND IS NOT NEGOTIABLE. Blending reads the ray data this
    // frame's trace just wrote; relocation nudges probes out of walls using the
    // distances blending just produced; classification then decides which
    // probes are inside geometry and can be skipped entirely next frame. Run
    // classification first and it would be deciding from last frame's
    // distances.
    // -----------------------------------------------------------------------
    bool updateProbes(Falcor::RenderContext *ctx) {
        if (!ready_) return false;

        // Falcor tracks resource states itself, so every texture the SDK is
        // about to touch has to be put in the state it expects BEFORE the raw
        // command list is handed over -- and Falcor has to be told, or its
        // model and the real state diverge on the next pass. Same contract as
        // the NGX call in dlss.h, for the same reason.
        ctx->resourceBarrier(texRayData_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(texIrradiance_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(texDistance_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(texProbeData_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(texVariability_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(texVariabilityAvg_.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->submit(false);

        ID3D12GraphicsCommandList *cmd = nativeCmdList(ctx);
        if (!cmd) return false;

        // The SDK binds its own root signature and descriptor heaps on this
        // list. Falcor's next pass assumes the state it left behind, so the two
        // have to be separated by a submit -- again exactly as dlss.h does.
        ID3D12DescriptorHeap *heaps[] = {heap_};
        cmd->SetDescriptorHeaps(1, heaps);

        rtxgi::d3d12::DDGIVolume *vols[] = {volume_};
        rtxgi::ERTXGIStatus s = rtxgi::d3d12::UpdateDDGIVolumeProbes(cmd, 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "probe blend failed (" + std::to_string(int(s)) + ")";
            ready_ = false;
            return false;
        }

        // Relocation and classification are both optional and both switched on
        // in describeVolume(). In a voxel wood they earn their place: a probe
        // that lands inside a trunk sees nothing but bark and would otherwise
        // poison every lookup that interpolates through it.
        s = rtxgi::d3d12::RelocateDDGIVolumeProbes(cmd, 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "probe relocation failed (" + std::to_string(int(s)) + ")";
            ready_ = false;
            return false;
        }
        s = rtxgi::d3d12::ClassifyDDGIVolumeProbes(cmd, 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "probe classification failed (" + std::to_string(int(s)) + ")";
            ready_ = false;
            return false;
        }

        ctx->submit(false);
        ++frameIndex_;
        return true;
    }

    void shutdown() {
        if (volume_) {
            volume_->Destroy();
            delete volume_;
            volume_ = nullptr;
        }
        releasePipelines();
        if (rootSig_) { rootSig_->Release(); rootSig_ = nullptr; }
        if (heap_) { heap_->Release(); heap_ = nullptr; }
        if (constantsUpload_) { constantsUpload_->Release(); constantsUpload_ = nullptr; }
        if (constantsBuf_) { constantsBuf_->Release(); constantsBuf_ = nullptr; }
        texRayData_.reset();
        texIrradiance_.reset();
        texDistance_.reset();
        texProbeData_.reset();
        texVariability_.reset();
        texVariabilityAvg_.reset();
        ready_ = false;
    }

  private:
    // How many frames of volume constants the upload ring holds. Three, so a
    // frame in flight is never writing over constants the GPU is still reading.
    static constexpr int kUploadRing = 3;

    Falcor::ref<Falcor::Device> device_;
    ID3D12Device *d3d_ = nullptr;

    rtxgi::DDGIVolumeDesc desc_{};
    rtxgi::d3d12::DDGIVolume *volume_ = nullptr;

    Falcor::ref<Falcor::Texture> texRayData_, texIrradiance_, texDistance_;
    Falcor::ref<Falcor::Texture> texProbeData_, texVariability_, texVariabilityAvg_;

    ID3D12DescriptorHeap *heap_ = nullptr;
    ID3D12RootSignature *rootSig_ = nullptr;
    ID3D12Resource *constantsBuf_ = nullptr, *constantsUpload_ = nullptr;

    ID3D12PipelineState *psoBlendIrr_ = nullptr, *psoBlendDist_ = nullptr;
    ID3D12PipelineState *psoReloc_ = nullptr, *psoRelocReset_ = nullptr;
    ID3D12PipelineState *psoClass_ = nullptr, *psoClassReset_ = nullptr;
    ID3D12PipelineState *psoReduce_ = nullptr, *psoReduceExtra_ = nullptr;

    std::vector<uint8_t> bcBlendIrr_, bcBlendDist_, bcReloc_, bcRelocReset_;
    std::vector<uint8_t> bcClass_, bcClassReset_, bcReduce_, bcReduceExtra_;

    Vec3 origin_{0.0f, 0.0f, 0.0f};
    uint64_t frameIndex_ = 0;
    bool ready_ = false;
    std::string status_ = "not initialised";

    // -----------------------------------------------------------------------
    static ID3D12GraphicsCommandList *nativeCmdList(Falcor::RenderContext *ctx) {
        return ctx->getLowLevelData()
            ->getCommandBufferNativeHandle()
            .as<ID3D12GraphicsCommandList *>();
    }

    void describeVolume() {
        desc_ = rtxgi::DDGIVolumeDesc{};
        desc_.index = 0;
        desc_.name = "v7 forest";
        desc_.rngSeed = 0;

        desc_.origin = {origin_.x, origin_.y, origin_.z};
        desc_.probeSpacing = {kProbeSpacing, kProbeSpacing, kProbeSpacing};
        desc_.probeCounts = {kProbeCountX, kProbeCountY, kProbeCountZ};

        desc_.probeNumRays = kProbeNumRays;
        desc_.probeNumIrradianceTexels = kProbeIrrTexels;
        desc_.probeNumIrradianceInteriorTexels = kProbeIrrInterior;
        desc_.probeNumDistanceTexels = kProbeDistTexels;
        desc_.probeNumDistanceInteriorTexels = kProbeDistInterior;

        // HOW FAST A PROBE FORGETS. 0.97 means a probe's irradiance is a moving
        // average with a time constant of about thirty frames -- half a second.
        // That is deliberately slow: the day/night clock moves the sun
        // continuously and a faster response would let probe noise through,
        // while a slower one would leave the indirect light visibly lagging the
        // shadows it belongs to.
        desc_.probeHysteresis = 0.97f;
        desc_.probeMaxRayDistance = 400.0f;
        // A probe blends only what arrives within this angle of its own normal
        // for a given texel. The default 0.9 leaks light through thin geometry,
        // and a voxel wood is nothing but thin geometry.
        desc_.probeNormalBias = 0.12f;
        desc_.probeViewBias = 0.6f;
        desc_.probeDistanceExponent = 50.0f;
        desc_.probeIrradianceEncodingGamma = 5.0f;

        // Change detection. A probe whose irradiance jumps by more than this in
        // one frame is treated as having seen a real lighting change rather
        // than noise, and its history is thrown away instead of blended.
        desc_.probeIrradianceThreshold = 0.2f;
        desc_.probeBrightnessThreshold = 0.10f;

        // RELOCATION AND CLASSIFICATION, both on. In a wood a good fraction of
        // the grid lands inside a trunk or under the terrain. Relocation nudges
        // those probes to somewhere they can see; classification marks the ones
        // that still cannot and skips tracing them at all, which is where most
        // of the ray budget is won back.
        desc_.probeRelocationEnabled = true;
        desc_.probeClassificationEnabled = true;
        desc_.probeVariabilityEnabled = false;

        // SCROLLING, because the world has no edges. A Default volume is
        // anchored in the world and the player walks out of it; a Scrolling one
        // moves with them and only relearns the plane of probes that just came
        // into range. That is what makes the cost fixed rather than growing
        // with how far you have walked.
        desc_.movementType = rtxgi::EDDGIVolumeMovementType::Scrolling;

        // F32x4 -- plain float4 of radiance and distance -- rather than the
        // packed F32x2 the SDK also accepts. F32x2 squeezes RGB into one 32-bit
        // word with RTXGI own encoding, which would mean v7 probe trace had to
        // reproduce that packing bit for bit to be read back correctly. Twice
        // the ray-data bandwidth is a cheap price for a format with no way to
        // get subtly wrong; the ray data is written once and read once, by the
        // blend pass, and never leaves the GPU.
        desc_.probeRayDataFormat = rtxgi::EDDGIVolumeTextureFormat::F32x4;
        desc_.probeIrradianceFormat = rtxgi::EDDGIVolumeTextureFormat::U32;
        desc_.probeDistanceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x2;
        desc_.probeDataFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
        desc_.probeVariabilityFormat = rtxgi::EDDGIVolumeTextureFormat::F16;
    }

    // Falcor's format for one of RTXGI's. Kept as an explicit switch rather
    // than a cast: the two enumerations agree on nothing, and a silent
    // mismatch here is a probe atlas that decodes as noise.
    static Falcor::ResourceFormat falcorFormat(rtxgi::EDDGIVolumeTextureFormat f) {
        using F = rtxgi::EDDGIVolumeTextureFormat;
        switch (f) {
            case F::U32: return Falcor::ResourceFormat::RGB10A2Unorm;
            case F::F16: return Falcor::ResourceFormat::R16Float;
            case F::F16x2: return Falcor::ResourceFormat::RG16Float;
            case F::F16x4: return Falcor::ResourceFormat::RGBA16Float;
            case F::F32: return Falcor::ResourceFormat::R32Float;
            case F::F32x2: return Falcor::ResourceFormat::RG32Float;
            default: return Falcor::ResourceFormat::RGBA32Float;
        }
    }

    bool createTextures() {
        const Falcor::ResourceBindFlags rw = Falcor::ResourceBindFlags::ShaderResource |
                                             Falcor::ResourceBindFlags::UnorderedAccess;
        auto make = [&](rtxgi::EDDGIVolumeTextureType type, rtxgi::EDDGIVolumeTextureFormat fmt,
                        const char *name) -> Falcor::ref<Falcor::Texture> {
            uint32_t w = 0, h = 0, n = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, type, w, h, n);
            if (w == 0 || h == 0 || n == 0) return nullptr;
            auto t = device_->createTexture2D(w, h, falcorFormat(fmt), n, 1, nullptr, rw);
            t->setName(name);
            return t;
        };

        using T = rtxgi::EDDGIVolumeTextureType;
        texRayData_ = make(T::RayData, desc_.probeRayDataFormat, "v7::ddgiRayData");
        texIrradiance_ = make(T::Irradiance, desc_.probeIrradianceFormat, "v7::ddgiIrradiance");
        texDistance_ = make(T::Distance, desc_.probeDistanceFormat, "v7::ddgiDistance");
        texProbeData_ = make(T::Data, desc_.probeDataFormat, "v7::ddgiProbeData");
        texVariability_ = make(T::Variability, desc_.probeVariabilityFormat, "v7::ddgiVariability");
        texVariabilityAvg_ =
            make(T::VariabilityAverage, desc_.probeVariabilityFormat, "v7::ddgiVariabilityAvg");

        if (!texRayData_ || !texIrradiance_ || !texDistance_ || !texProbeData_ ||
            !texVariability_ || !texVariabilityAvg_) {
            status_ = "could not create the probe textures";
            return false;
        }
        return true;
    }

    bool createConstantsBuffers() {
        const UINT64 stride = sizeof(rtxgi::DDGIVolumeDescGPUPacked);

        D3D12_HEAP_PROPERTIES hp{};
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        rd.Width = stride;
        if (FAILED(d3d_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&constantsBuf_)))) {
            status_ = "could not create the volume constants buffer";
            return false;
        }

        // The ring: one slot per frame in flight, so a frame still being read
        // is never written over. UploadDDGIVolumeConstants indexes it.
        hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        rd.Width = stride * kUploadRing;
        if (FAILED(d3d_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                 IID_PPV_ARGS(&constantsUpload_)))) {
            status_ = "could not create the volume constants upload buffer";
            return false;
        }
        return true;
    }

    bool createDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = UINT(rtxgi::GetDDGIVolumeNumResourceDescriptors());
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(d3d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
            status_ = "could not create the DDGI descriptor heap";
            return false;
        }
        return true;
    }

    // The eight .cso files compile_ddgi_shaders.bat produced. Read rather than
    // compiled, which is the whole reason that batch file exists -- see its
    // header for why a runtime DXC is worse.
    bool loadShaders(const std::filesystem::path &dir) {
        struct Item { const char *file; std::vector<uint8_t> *into; };
        const Item items[] = {
            {"blend_irradiance.cso", &bcBlendIrr_},
            {"blend_distance.cso", &bcBlendDist_},
            {"relocation_update.cso", &bcReloc_},
            {"relocation_reset.cso", &bcRelocReset_},
            {"classification_update.cso", &bcClass_},
            {"classification_reset.cso", &bcClassReset_},
            {"reduction.cso", &bcReduce_},
            {"reduction_extra.cso", &bcReduceExtra_},
        };
        for (const Item &it : items) {
            const std::filesystem::path p = dir / "ddgi" / it.file;
            std::ifstream f(p, std::ios::binary | std::ios::ate);
            if (!f) {
                status_ = std::string("missing probe shader ") + it.file +
                          " (run compile_ddgi_shaders.bat)";
                return false;
            }
            it.into->resize(size_t(f.tellg()));
            f.seekg(0);
            f.read(reinterpret_cast<char *>(it.into->data()), std::streamsize(it.into->size()));
            if (it.into->empty()) {
                status_ = std::string("empty probe shader ") + it.file;
                return false;
            }
        }
        return true;
    }

    bool createRootSignature() {
        rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc hd = heapDesc();
        ID3DBlob *sig = nullptr;
        if (!rtxgi::d3d12::GetDDGIVolumeRootSignatureDesc(hd, sig) || !sig) {
            status_ = "could not build the DDGI root signature description";
            return false;
        }
        const HRESULT hr = d3d_->CreateRootSignature(0, sig->GetBufferPointer(),
                                                     sig->GetBufferSize(),
                                                     IID_PPV_ARGS(&rootSig_));
        sig->Release();
        if (FAILED(hr)) {
            status_ = "could not create the DDGI root signature";
            return false;
        }
        return true;
    }

    bool makePso(const std::vector<uint8_t> &bc, ID3D12PipelineState **out, const char *what) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC d{};
        d.pRootSignature = rootSig_;
        d.CS.pShaderBytecode = bc.data();
        d.CS.BytecodeLength = bc.size();
        if (FAILED(d3d_->CreateComputePipelineState(&d, IID_PPV_ARGS(out)))) {
            status_ = std::string("could not create the ") + what + " pipeline state";
            return false;
        }
        return true;
    }

    bool createPipelines() {
        return makePso(bcBlendIrr_, &psoBlendIrr_, "probe irradiance blending") &&
               makePso(bcBlendDist_, &psoBlendDist_, "probe distance blending") &&
               makePso(bcReloc_, &psoReloc_, "probe relocation") &&
               makePso(bcRelocReset_, &psoRelocReset_, "probe relocation reset") &&
               makePso(bcClass_, &psoClass_, "probe classification") &&
               makePso(bcClassReset_, &psoClassReset_, "probe classification reset") &&
               makePso(bcReduce_, &psoReduce_, "probe variability reduction") &&
               makePso(bcReduceExtra_, &psoReduceExtra_, "probe variability extra reduction");
    }

    void releasePipelines() {
        ID3D12PipelineState *all[] = {psoBlendIrr_, psoBlendDist_, psoReloc_,  psoRelocReset_,
                                      psoClass_,    psoClassReset_, psoReduce_, psoReduceExtra_};
        for (ID3D12PipelineState *p : all)
            if (p) p->Release();
        psoBlendIrr_ = psoBlendDist_ = psoReloc_ = psoRelocReset_ = nullptr;
        psoClass_ = psoClassReset_ = psoReduce_ = psoReduceExtra_ = nullptr;
    }

    // WHERE EVERY DESCRIPTOR SITS ON THE HEAP. The SDK does not discover this;
    // it is told, and it writes its own views at exactly these indices. The
    // layout is arbitrary but has to be consistent between here, the root
    // signature built from it, and createViews().
    rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc heapDesc() const {
        rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc hd{};
        hd.resources = heap_;
        hd.samplers = nullptr;
        hd.entrySize = d3d_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        hd.constantsIndex = 0;
        hd.resourceIndicesIndex = 1;

        rtxgi::DDGIVolumeResourceIndices &ix = hd.resourceIndices;
        ix.rayDataUAVIndex = 2;
        ix.rayDataSRVIndex = 3;
        ix.probeIrradianceUAVIndex = 4;
        ix.probeIrradianceSRVIndex = 5;
        ix.probeDistanceUAVIndex = 6;
        ix.probeDistanceSRVIndex = 7;
        ix.probeDataUAVIndex = 8;
        ix.probeDataSRVIndex = 9;
        ix.probeVariabilityUAVIndex = 10;
        ix.probeVariabilitySRVIndex = 11;
        ix.probeVariabilityAverageUAVIndex = 12;
        ix.probeVariabilityAverageSRVIndex = 13;
        return hd;
    }

    bool createVolume() {
        rtxgi::d3d12::DDGIVolumeResources res{};
        res.descriptorHeap = heapDesc();

        res.constantsBuffer = constantsBuf_;
        res.constantsBufferUpload = constantsUpload_;
        res.constantsBufferSizeInBytes = sizeof(rtxgi::DDGIVolumeDescGPUPacked) * kUploadRing;

        res.unmanaged.enabled = true;
        res.unmanaged.rootSignature = rootSig_;
        // The slots GetDDGIVolumeRootSignatureDesc() lays out for the SDK's own
        // (non-bindless) signature: root constants first, then the table.
        res.unmanaged.rootParamSlotRootConstants = 0;
        res.unmanaged.rootParamSlotResourceDescriptorTable = 1;

        res.unmanaged.probeRayData = native(texRayData_);
        res.unmanaged.probeIrradiance = native(texIrradiance_);
        res.unmanaged.probeDistance = native(texDistance_);
        res.unmanaged.probeData = native(texProbeData_);
        res.unmanaged.probeVariability = native(texVariability_);
        res.unmanaged.probeVariabilityAverage = native(texVariabilityAvg_);
        res.unmanaged.probeVariabilityReadback = nullptr;  // variability is off

        res.unmanaged.probeBlendingIrradiancePSO = psoBlendIrr_;
        res.unmanaged.probeBlendingDistancePSO = psoBlendDist_;
        res.unmanaged.probeRelocation.updatePSO = psoReloc_;
        res.unmanaged.probeRelocation.resetPSO = psoRelocReset_;
        res.unmanaged.probeClassification.updatePSO = psoClass_;
        res.unmanaged.probeClassification.resetPSO = psoClassReset_;
        res.unmanaged.probeVariabilityPSOs.reductionPSO = psoReduce_;
        res.unmanaged.probeVariabilityPSOs.extraReductionPSO = psoReduceExtra_;

        volume_ = new rtxgi::d3d12::DDGIVolume();
        const rtxgi::ERTXGIStatus s = volume_->Create(desc_, res);
        if (s != rtxgi::ERTXGIStatus::OK) {
            delete volume_;
            volume_ = nullptr;
            status_ = "DDGIVolume::Create failed (" + std::to_string(int(s)) + ")";
            return false;
        }
        return true;
    }

    static ID3D12Resource *native(const Falcor::ref<Falcor::Texture> &t) {
        return t ? t->getNativeHandle().as<ID3D12Resource *>() : nullptr;
    }
};
#endif  // V7_HAS_DDGI

}  // namespace v7
