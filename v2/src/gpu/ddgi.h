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
// that offer would be a mistake here. v2's own Slang shaders touch those
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
#include "../../shaders/Shared.slang"

#if V2_HAS_DDGI
#include <d3d12.h>
// RTXGI 1.3's headers open `namespace rtxgi { using namespace rtxgi; }` in two
// places, which MSVC reports as C4515 -- and v2 builds with /WX, so a warning
// in somebody else's header is a failed build. Suppressed only across these
// includes rather than switched off for the target: v2's own code should still
// have to answer for C4515 if it ever earns one.
#pragma warning(push)
#pragma warning(disable : 4515)  // 'rtxgi': namespace uses itself
#include <rtxgi/ddgi/DDGIVolume.h>
#include <rtxgi/ddgi/gfx/DDGIVolume_D3D12.h>
#pragma warning(pop)
#endif

namespace v2 {

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
// values by a comment nobody can honour -- which is why v2 goes further and
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

#if !V2_HAS_DDGI
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
    V6DdgiConsts consts() const { return V6DdgiConsts{}; }
    void shutdown() {}

  private:
    std::string status_ = "built without the RTXGI SDK (set V2_RTXGI_DIR)";
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
        if (!createVariabilityReadback()) return false;
        if (!createRtvHeap()) return false;
        if (!createConstantsBuffers()) return false;
        if (!createDescriptorHeap()) return false;
        if (!loadShaders(shaderDir)) return false;
        if (!createRootSignature()) return false;
        if (!createPipelines()) return false;
        if (!createViews()) return false;
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

        // FOUR ARGUMENTS, NOT FIVE. The D3D12 backend takes the command list
        // and not the device -- the constants ring it writes into belongs to
        // the volume, which already holds the device it was created with.
        rtxgi::d3d12::DDGIVolume *vols[] = {volume_};
        const rtxgi::ERTXGIStatus s = rtxgi::d3d12::UploadDDGIVolumeConstants(
            cmd, UINT(frameIndex_ % kUploadRing), 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "constants upload failed (" + std::to_string(int(s)) + ")";
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // The same volume, described for v2's OWN two shaders -- the probe trace
    // that fills the ray data, and the irradiance lookup in Ddgi.slang.
    //
    // READ BACK OUT OF THE SDK rather than rebuilt from the constants at the
    // top of this file. Rebuilding would be shorter and it would drift the
    // first time describeVolume() changed without this being changed with it.
    // The failure mode of that drift is not a crash: it is a probe atlas that
    // decodes ALMOST right -- a ray rotation one frame stale, or a scroll
    // offset the lookup does not know about -- which shows up as indirect light
    // that swims slightly as you walk and nothing that ever points at this
    // function. Asking the volume what it actually did cannot drift.
    // -----------------------------------------------------------------------
    V6DdgiConsts consts() const {
        V6DdgiConsts c{};
        if (!ready_ || !volume_) return c;

        const rtxgi::DDGIVolumeDescGPU g = volume_->GetDescGPU();
        const rtxgi::int3 scroll = volume_->GetScrollOffsets();

        c.origin = Falcor::float3(g.origin.x, g.origin.y, g.origin.z);
        c.maxRayDistance = g.probeMaxRayDistance;
        c.probeCounts = Falcor::int3(g.probeCounts.x, g.probeCounts.y, g.probeCounts.z);
        c.numRays = g.probeNumRays;
        c.probeSpacing = Falcor::float3(g.probeSpacing.x, g.probeSpacing.y, g.probeSpacing.z);
        c.normalBias = g.probeNormalBias;
        c.rayRotation = Falcor::float4(g.probeRayRotation.x, g.probeRayRotation.y,
                                       g.probeRayRotation.z, g.probeRayRotation.w);
        c.probeScroll = Falcor::int3(scroll.x, scroll.y, scroll.z);
        c.useClassification = g.probeClassificationEnabled ? 1 : 0;

        // Both counts, because the lookup needs the border-inclusive stride to
        // find a probe in the atlas and the interior count to map an octahedral
        // uv inside it. Deriving one from the other here would put the +2 in
        // two places.
        c.irradianceTexels =
            Falcor::float2(float(kProbeIrrTexels), float(kProbeIrrInterior));
        c.distanceTexels =
            Falcor::float2(float(kProbeDistTexels), float(kProbeDistInterior));
        c.irradianceGamma = g.probeIrradianceEncodingGamma;
        c.viewBias = g.probeViewBias;
        c.ddgiPad0 = 0.0f;
        c.ddgiPad1 = 0.0f;
        return c;
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
        if (variabilityReadback_) { variabilityReadback_->Release(); variabilityReadback_ = nullptr; }
        if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
        irradianceRtv_ = D3D12_CPU_DESCRIPTOR_HANDLE{};
        distanceRtv_ = D3D12_CPU_DESCRIPTOR_HANDLE{};
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
    ID3D12Resource *variabilityReadback_ = nullptr;
    ID3D12DescriptorHeap *rtvHeap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE irradianceRtv_{};
    D3D12_CPU_DESCRIPTOR_HANDLE distanceRtv_{};

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
        desc_.name = "v2 forest";
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
        // word with RTXGI own encoding, which would mean v2 probe trace had to
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

    // Falcor's format for the DXGI format the SDK actually asked for. This is
    // the only mapping that may be used to CREATE a probe texture: see the note
    // in createTextures(). An unrecognised format returns Unknown rather than
    // guessing, so a future SDK format fails loudly here instead of quietly
    // producing a resource nothing can view.
    static Falcor::ResourceFormat fromDxgi(DXGI_FORMAT f) {
        switch (f) {
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return Falcor::ResourceFormat::RGBA32Float;
            case DXGI_FORMAT_R32G32_FLOAT: return Falcor::ResourceFormat::RG32Float;
            case DXGI_FORMAT_R32_FLOAT: return Falcor::ResourceFormat::R32Float;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return Falcor::ResourceFormat::RGBA16Float;
            case DXGI_FORMAT_R16G16_FLOAT: return Falcor::ResourceFormat::RG16Float;
            case DXGI_FORMAT_R16_FLOAT: return Falcor::ResourceFormat::R16Float;
            case DXGI_FORMAT_R10G10B10A2_UNORM: return Falcor::ResourceFormat::RGB10A2Unorm;
            default: return Falcor::ResourceFormat::Unknown;
        }
    }

    // The old enum-only mapping, kept for reference and no longer used to make
    // anything -- see fromDxgi above for why it cannot be.
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
        // THE IRRADIANCE AND DISTANCE ATLASES ALSO NEED TO BE RENDER TARGETS.
        // The SDK clears their borders with ClearRenderTargetView, so Create()
        // demands an RTV for each -- and D3D12 will not make an RTV for a
        // resource that was not created with ALLOW_RENDER_TARGET. Without this
        // flag the RTVs below are silently wrong rather than absent.
        const Falcor::ResourceBindFlags rwRt = rw | Falcor::ResourceBindFlags::RenderTarget;
        auto make = [&](rtxgi::EDDGIVolumeTextureType type, rtxgi::EDDGIVolumeTextureFormat fmt,
                        const char *name,
                        Falcor::ResourceBindFlags flags) -> Falcor::ref<Falcor::Texture> {
            uint32_t w = 0, h = 0, n = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, type, w, h, n);
            if (w == 0 || h == 0 || n == 0) return nullptr;
            // THE FORMAT IS ASKED OF THE SDK, NOT DERIVED FROM THE ENUM, and
            // this is the single most expensive mistake available in this file.
            //
            // GetDDGIVolumeTextureFormat() is not a lookup table on `fmt` -- it
            // is a function of the TYPE as well, and for VariabilityAverage it
            // ignores `fmt` completely and always answers R32G32_FLOAT.
            // Deriving the Falcor format from the enum instead gives R16_FLOAT
            // there, so the texture and every view of it disagree. That is an
            // INVALID_CALL, which removes the device -- and it surfaces inside
            // Falcor's texture allocator, hundreds of lines from here, on some
            // later unrelated allocation.
            const Falcor::ResourceFormat ff =
                fromDxgi(rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, fmt));
            auto t = device_->createTexture2D(w, h, ff, n, 1, nullptr, flags);
            t->setName(name);
            return t;
        };

        using T = rtxgi::EDDGIVolumeTextureType;
        texRayData_ = make(T::RayData, desc_.probeRayDataFormat, "v2::ddgiRayData", rw);
        texIrradiance_ =
            make(T::Irradiance, desc_.probeIrradianceFormat, "v2::ddgiIrradiance", rwRt);
        texDistance_ = make(T::Distance, desc_.probeDistanceFormat, "v2::ddgiDistance", rwRt);
        texProbeData_ = make(T::Data, desc_.probeDataFormat, "v2::ddgiProbeData", rw);
        texVariability_ =
            make(T::Variability, desc_.probeVariabilityFormat, "v2::ddgiVariability", rw);
        texVariabilityAvg_ = make(T::VariabilityAverage, desc_.probeVariabilityFormat,
                                  "v2::ddgiVariabilityAvg", rw);

        if (!texRayData_ || !texIrradiance_ || !texDistance_ || !texProbeData_ ||
            !texVariability_ || !texVariabilityAvg_) {
            status_ = "could not create the probe textures";
            return false;
        }
        return true;
    }

    // The eight bytes the validator insists on. Created straight on the device
    // rather than through Falcor: it is a READBACK-heap resource that no shader
    // and no Falcor pass ever touches, so putting it in Falcor's resource-state
    // model would be describing a relationship that does not exist.
    //
    // Shape copied from the SDK's own managed-mode allocation: a row-major
    // buffer of two floats, created in COPY_DEST.
    // The two render target views Create() insists on, in a heap of v2's own.
    //
    // NOT SHADER VISIBLE: an RTV heap never is. These exist only so the SDK can
    // call ClearRenderTargetView on the two atlases when a scrolling plane
    // comes into range and its probes have to be wiped rather than blended.
    //
    // The descriptions are copied from the SDK's managed path rather than
    // guessed -- a TEXTURE2DARRAY view over the whole array, with the format
    // asked of GetDDGIVolumeTextureFormat rather than re-derived. A view whose
    // format disagrees with its resource is an INVALID_CALL that surfaces as a
    // removed device somewhere else entirely.
    bool createRtvHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = 2;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        if (FAILED(d3d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) {
            status_ = "could not create the DDGI RTV heap";
            return false;
        }

        uint32_t w = 0, h = 0, arraySize = 0;
        rtxgi::GetDDGIVolumeTextureDimensions(desc_, rtxgi::EDDGIVolumeTextureType::Irradiance, w,
                                              h, arraySize);

        const UINT stride = d3d_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        rtv.Texture2DArray.ArraySize = arraySize;

        using T = rtxgi::EDDGIVolumeTextureType;
        rtv.Format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(T::Irradiance,
                                                              desc_.probeIrradianceFormat);
        irradianceRtv_ = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
        d3d_->CreateRenderTargetView(native(texIrradiance_), &rtv, irradianceRtv_);

        rtv.Format =
            rtxgi::d3d12::GetDDGIVolumeTextureFormat(T::Distance, desc_.probeDistanceFormat);
        distanceRtv_.ptr = irradianceRtv_.ptr + stride;
        d3d_->CreateRenderTargetView(native(texDistance_), &rtv, distanceRtv_);
        return true;
    }

    bool createVariabilityReadback() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_READBACK;

        D3D12_RESOURCE_DESC rd = {};
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.Width = sizeof(float) * 2;
        rd.Height = 1;
        rd.MipLevels = 1;
        rd.DepthOrArraySize = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Flags = D3D12_RESOURCE_FLAG_NONE;

        const HRESULT hr = d3d_->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&variabilityReadback_));
        if (FAILED(hr)) {
            status_ = "variability readback buffer allocation failed";
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
        // SIZED FROM THE LAYOUT, NOT FROM THE SDK'S COUNT.
        //
        // GetDDGIVolumeNumResourceDescriptors() returns 12 -- the six textures
        // times a UAV and an SRV each. It says nothing about the constants slot
        // or the resource-indices slot, and heapDesc() below places those at 0
        // and 1 and then puts the textures at 2..13. A heap sized 12 therefore
        // has its last two descriptors written past the end, which is not a
        // validation error, just another heap's memory.
        hd.NumDescriptors = UINT(rtxgi::GetDDGIVolumeNumResourceDescriptors()) + 2u;
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

    // -----------------------------------------------------------------------
    // The descriptors the SDK's probe shaders read, written into v2's heap.
    //
    // THIS HAS TO BE HERE BECAUSE UNMANAGED MODE DOES NOT DO IT. The SDK's own
    // CreateDescriptors() is inside `#if RTXGI_DDGI_RESOURCE_MANAGEMENT`, so in
    // unmanaged mode Create() succeeds against a completely empty heap and the
    // first dispatch reads whatever was in that memory. It does not fail, it
    // removes the device -- which is exactly what it did here until this
    // existed.
    //
    // The layout is heapDesc()'s, the view descriptions are copied from the
    // SDK's managed path, and every format is ASKED FOR rather than derived:
    // a view whose format disagrees with its resource is an INVALID_CALL that
    // surfaces hundreds of lines away from the mistake.
    // -----------------------------------------------------------------------
    bool createViews() {
        const rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc hd = heapDesc();
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            heap_->GetCPUDescriptorHandleForHeapStart();
        auto at = [&](uint32_t index) {
            D3D12_CPU_DESCRIPTOR_HANDLE h;
            h.ptr = start.ptr + SIZE_T(index) * SIZE_T(hd.entrySize);
            return h;
        };

        // The volume constants, as a one-element structured buffer SRV. This is
        // the t0 the blend, relocation and classification shaders all read
        // their volume out of.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Buffer.NumElements = 1;  // desc_.index + 1, and index is 0
            sd.Buffer.StructureByteStride = sizeof(rtxgi::DDGIVolumeDescGPUPacked);
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d3d_->CreateShaderResourceView(constantsBuf_, &sd, at(hd.constantsIndex));
        }

        using T = rtxgi::EDDGIVolumeTextureType;
        struct Item {
            T type;
            rtxgi::EDDGIVolumeTextureFormat fmt;
            ID3D12Resource *res;
            uint32_t uav, srv;
        };
        const rtxgi::DDGIVolumeResourceIndices &ix = hd.resourceIndices;
        const Item items[] = {
            {T::RayData, desc_.probeRayDataFormat, native(texRayData_), ix.rayDataUAVIndex,
             ix.rayDataSRVIndex},
            {T::Irradiance, desc_.probeIrradianceFormat, native(texIrradiance_),
             ix.probeIrradianceUAVIndex, ix.probeIrradianceSRVIndex},
            {T::Distance, desc_.probeDistanceFormat, native(texDistance_),
             ix.probeDistanceUAVIndex, ix.probeDistanceSRVIndex},
            {T::Data, desc_.probeDataFormat, native(texProbeData_), ix.probeDataUAVIndex,
             ix.probeDataSRVIndex},
            {T::Variability, desc_.probeVariabilityFormat, native(texVariability_),
             ix.probeVariabilityUAVIndex, ix.probeVariabilitySRVIndex},
            {T::VariabilityAverage, desc_.probeVariabilityFormat, native(texVariabilityAvg_),
             ix.probeVariabilityAverageUAVIndex, ix.probeVariabilityAverageSRVIndex},
        };

        for (const Item &it : items) {
            uint32_t w = 0, h = 0, arraySize = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, it.type, w, h, arraySize);
            if (arraySize == 0) {
                status_ = "a probe texture reported a zero array size";
                return false;
            }
            const DXGI_FORMAT f = rtxgi::d3d12::GetDDGIVolumeTextureFormat(it.type, it.fmt);

            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.ArraySize = arraySize;
            ud.Format = f;
            d3d_->CreateUnorderedAccessView(it.res, nullptr, &ud, at(it.uav));

            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            sd.Texture2DArray.ArraySize = arraySize;
            sd.Texture2DArray.MipLevels = 1;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = f;
            d3d_->CreateShaderResourceView(it.res, &sd, at(it.srv));
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
        // ONE SLOT, NOT THE WHOLE RING. This is the STRIDE BETWEEN ring slots,
        // not the size of the allocation: UploadDDGIVolumeConstants computes
        // its copy offset as `constantsBufferSizeInBytes * bufferingIndex`. Set
        // to the ring size, slot 1 writes three strides into a three-stride
        // buffer and slot 2 writes six -- so the first frame looks perfect and
        // the device is gone on the second.
        res.constantsBufferSizeInBytes = sizeof(rtxgi::DDGIVolumeDescGPUPacked);

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
        // NOT NULL, EVEN THOUGH VARIABILITY IS OFF.
        //
        // ValidateUnmanagedResourcesDesc() checks every resource pointer
        // unconditionally -- it never consults probeVariabilityEnabled -- so a
        // null here fails Create() with status 43,
        // ERROR_DDGI_INVALID_TEXTURE_PROBE_VARIABILITY_READBACK. Nothing ever
        // reads the buffer with variability disabled; it exists purely to get
        // past the validator, which is why it is eight bytes.
        res.unmanaged.probeVariabilityReadback = variabilityReadback_;

        res.unmanaged.probeIrradianceRTV = irradianceRtv_;
        res.unmanaged.probeDistanceRTV = distanceRtv_;

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
#endif  // V2_HAS_DDGI

}  // namespace v2
