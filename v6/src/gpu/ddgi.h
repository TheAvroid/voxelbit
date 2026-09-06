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
// that offer would be a mistake here. v6's own Slang shaders touch those
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

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"

#if V6_HAS_DDGI
#include <d3d12.h>
#include <rtxgi/ddgi/DDGIVolume.h>
#include <rtxgi/ddgi/gfx/DDGIVolume_D3D12.h>
#endif

namespace v6 {

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
// values by a comment nobody can honour -- which is why v6 goes further and
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

#if !V6_HAS_DDGI
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
    void setHysteresis(float) {}
    int numProbes() const { return 0; }
    int raysPerProbe() const { return 0; }
    Falcor::Texture *rayData() const { return nullptr; }
    Falcor::Texture *irradiance() const { return nullptr; }
    Falcor::Texture *distance() const { return nullptr; }
    Falcor::Texture *probeData() const { return nullptr; }
    Falcor::ref<Falcor::Texture> rayDataRef() const { return nullptr; }
    Falcor::ref<Falcor::Texture> irradianceRef() const { return nullptr; }
    Falcor::ref<Falcor::Texture> distanceRef() const { return nullptr; }
    Falcor::ref<Falcor::Texture> probeDataRef() const { return nullptr; }
    bool uploadConstants(Falcor::RenderContext *) { return false; }
    bool updateProbes(Falcor::RenderContext *, int = 4) { return false; }
    V6DdgiConsts consts() const { return V6DdgiConsts{}; }
    void shutdown() {}

  private:
    std::string status_ = "built without the RTXGI SDK (set V6_RTXGI_DIR)";
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

    // The same four as ref<>, which is what a Falcor shader variable binds to.
    const Falcor::ref<Falcor::Texture> &rayDataRef() const { return texRayData_; }
    const Falcor::ref<Falcor::Texture> &irradianceRef() const { return texIrradiance_; }
    const Falcor::ref<Falcor::Texture> &distanceRef() const { return texDistance_; }
    const Falcor::ref<Falcor::Texture> &probeDataRef() const { return texProbeData_; }

    // Where the probe grid is centred. Called every frame with the player's
    // position, snapped so the volume does not shimmer as they walk -- see
    // setOrigin().
    Vec3 origin() const { return origin_; }

    // -----------------------------------------------------------------------
    // What v6's own probe shaders need to know about the volume.
    //
    // TAKEN FROM THE SDK RATHER THAN FROM desc_, and the difference matters
    // for exactly two fields. probeRayRotation is regenerated by Update()
    // every frame and is the whole reason 192 fixed directions are enough;
    // probeScrollOffsets is how a scrolling volume renumbers its probes as
    // the player walks. Both live only inside the volume. Reading the rest
    // from the same struct keeps every number in one place, so v6's shaders
    // and RTXGI's blend pass cannot end up describing different volumes.
    // -----------------------------------------------------------------------
    V6DdgiConsts consts() const {
        V6DdgiConsts c{};
        if (!ready_) return c;
        const rtxgi::DDGIVolumeDescGPU g = volume_->GetDescGPU();

        c.origin = float3(g.origin.x, g.origin.y, g.origin.z);
        c.maxRayDistance = g.probeMaxRayDistance;
        c.probeCounts = int3(g.probeCounts.x, g.probeCounts.y, g.probeCounts.z);
        c.numRays = g.probeNumRays;
        c.probeSpacing = float3(g.probeSpacing.x, g.probeSpacing.y, g.probeSpacing.z);
        c.normalBias = g.probeNormalBias;
        c.rayRotation = float4(g.probeRayRotation.x, g.probeRayRotation.y,
                               g.probeRayRotation.z, g.probeRayRotation.w);
        c.probeScroll = int3(g.probeScrollOffsets.x, g.probeScrollOffsets.y,
                             g.probeScrollOffsets.z);
        c.useFixedRays =
            (desc_.probeRelocationEnabled || desc_.probeClassificationEnabled) ? 1 : 0;
        // x is the texel count INCLUDING the one-texel border, y excluding it.
        // The GPU struct only carries the interior count, so the border is
        // added back here -- see ddgiProbeUV() for why both are needed.
        c.irradianceTexels = float2(float(g.probeNumIrradianceInteriorTexels + 2),
                                    float(g.probeNumIrradianceInteriorTexels));
        c.distanceTexels = float2(float(g.probeNumDistanceInteriorTexels + 2),
                                  float(g.probeNumDistanceInteriorTexels));
        c.irradianceGamma = g.probeIrradianceEncodingGamma;
        c.viewBias = g.probeViewBias;
        c.ddgiPad0 = c.ddgiPad1 = 0.0f;
        return c;
    }

    // -----------------------------------------------------------------------
    // Bring the volume up. Failure is never fatal.
    // -----------------------------------------------------------------------
    // Overridable so a diagnostic can set it to 0 -- no history at all, each
    // pass replacing the atlas outright. That separates "the probe rays are
    // wrong" from "the blend is accumulating when it should be averaging".
    void setHysteresis(float h) { hysteresis_ = h; }

    bool init(const Falcor::ref<Falcor::Device> &device, const std::filesystem::path &shaderDir) {
        device_ = device;

        d3d_ = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d_) {
            status_ = "not a D3D12 device (RTXGI here is D3D12 only)";
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

        // EVERY STEP IS FOLLOWED BY A DEVICE CHECK, and that is not paranoia --
        // it is the only way to attribute a removal to the call that caused it.
        // D3D12 reports a removed device on the NEXT api call, so an invalid
        // view created here surfaces as a failure inside Falcor's texture
        // allocator several hundred lines away, naming a call that did nothing
        // wrong. Asking after each step turns that into a sentence.
        if (!createTextures() || !alive("creating the probe textures")) return false;
        if (!createConstantsBuffers() || !alive("creating the constants buffers")) return false;
        if (!createDescriptorHeap() || !alive("creating the descriptor heap")) return false;
        if (!createRtvs() || !alive("creating the render target views")) return false;
        if (!loadShaders(shaderDir)) return false;
        if (!createRootSignature() || !alive("creating the root signature")) return false;
        if (!createPipelines() || !alive("creating the pipeline states")) return false;
        if (!createVolume() || !alive("DDGIVolume::Create")) return false;
        // After Create(), because in unmanaged mode nothing else fills the
        // heap the root signature already spans.
        if (!createViews() || !alive("creating the resource views")) return false;

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
            cmd, UINT(frameIndex_ % kUploadRing), 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "constants upload failed: " + statusName(s);
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
    // `stage` exists to bisect a GPU fault, and it stays because that is a
    // question worth being able to ask again. Each of these three calls binds
    // the SDK's own root signature and descriptor table and dispatches over the
    // whole probe grid; when one of them removes the device, the report says
    // only "device removed" and names no call. Running 2, then 3, then 4 says
    // which.
    //   2 = blend only, 3 = + relocation, 4 = + classification (the default)
    bool updateProbes(Falcor::RenderContext *ctx, int stage = 4) {
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
            status_ = "probe blend failed: " + statusName(s);
            ready_ = false;
            return false;
        }

        // Relocation and classification are both optional and both switched on
        // in describeVolume(). In a voxel wood they earn their place: a probe
        // that lands inside a trunk sees nothing but bark and would otherwise
        // poison every lookup that interpolates through it.
        if (stage < 3) { ctx->submit(false); ++frameIndex_; return true; }

        s = rtxgi::d3d12::RelocateDDGIVolumeProbes(cmd, 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "probe relocation failed: " + statusName(s);
            ready_ = false;
            return false;
        }
        if (stage < 4) { ctx->submit(false); ++frameIndex_; return true; }

        s = rtxgi::d3d12::ClassifyDDGIVolumeProbes(cmd, 1, vols);
        if (s != rtxgi::ERTXGIStatus::OK) {
            status_ = "probe classification failed: " + statusName(s);
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
        if (rtvHeap_) { rtvHeap_->Release(); rtvHeap_ = nullptr; }
        if (constantsUpload_) { constantsUpload_->Release(); constantsUpload_ = nullptr; }
        if (constantsBuf_) { constantsBuf_->Release(); constantsBuf_ = nullptr; }
        if (variabilityReadback_) { variabilityReadback_->Release(); variabilityReadback_ = nullptr; }
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
    ID3D12DescriptorHeap *rtvHeap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE irradianceRtv_{0}, distanceRtv_{0};
    ID3D12RootSignature *rootSig_ = nullptr;
    ID3D12Resource *constantsBuf_ = nullptr, *constantsUpload_ = nullptr;
    ID3D12Resource *variabilityReadback_ = nullptr;

    ID3D12PipelineState *psoBlendIrr_ = nullptr, *psoBlendDist_ = nullptr;
    ID3D12PipelineState *psoReloc_ = nullptr, *psoRelocReset_ = nullptr;
    ID3D12PipelineState *psoClass_ = nullptr, *psoClassReset_ = nullptr;
    ID3D12PipelineState *psoReduce_ = nullptr, *psoReduceExtra_ = nullptr;

    std::vector<uint8_t> bcBlendIrr_, bcBlendDist_, bcReloc_, bcRelocReset_;
    std::vector<uint8_t> bcClass_, bcClassReset_, bcReduce_, bcReduceExtra_;

    float hysteresis_ = 0.97f;
    Vec3 origin_{0.0f, 0.0f, 0.0f};
    uint64_t frameIndex_ = 0;
    bool ready_ = false;
    std::string status_ = "not initialised";

    // Has the device survived the last step?
    //
    // GetDeviceRemovedReason() is the only call that keeps working on a removed
    // device, and its answer is worth printing: DEVICE_HUNG means the GPU
    // faulted on work already submitted, while INVALID_CALL means the runtime
    // rejected something outright -- which for this integration usually means a
    // descriptor whose format or dimension disagrees with its resource.
    bool alive(const char *what) {
        const HRESULT r = d3d_->GetDeviceRemovedReason();
        if (r == S_OK) return true;
        const char *why = "unknown";
        switch (unsigned(r)) {
            case 0x887A0006u: why = "DEVICE_HUNG"; break;
            case 0x887A0005u: why = "DEVICE_REMOVED"; break;
            case 0x887A0007u: why = "DEVICE_RESET"; break;
            case 0x887A0020u: why = "DRIVER_INTERNAL_ERROR"; break;
            case 0x887A0001u: why = "INVALID_CALL"; break;
            default: break;
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), " (0x%08x)", unsigned(r));
        status_ = std::string("device lost while ") + what + ": " + why + buf;
        return false;
    }

    // -----------------------------------------------------------------------
    // The SDK's status codes, by name.
    //
    // WORTH THE TABLE. Create() has forty-odd distinct failure modes and they
    // are all reported as one integer; the first real one here was 43, which
    // turned out to mean "you passed a null variability readback buffer" and
    // was indistinguishable from "your root signature is wrong" without
    // counting enumerators by hand in a header. Only the codes this integration
    // can actually produce are listed -- the Vulkan and managed-mode ones
    // cannot happen from here.
    // -----------------------------------------------------------------------
    static std::string statusName(rtxgi::ERTXGIStatus s) {
        switch (s) {
            case rtxgi::OK: return "OK";
            case rtxgi::ERROR_DDGI_INVALID_PROBE_COUNTS: return "invalid probe counts";
            case rtxgi::ERROR_DDGI_INVALID_CONSTANTS_BUFFER: return "invalid constants buffer";
            case rtxgi::ERROR_DDGI_INVALID_CONSTANTS_UPLOAD_BUFFER:
                return "invalid constants upload buffer";
            case rtxgi::ERROR_DDGI_INVALID_RESOURCES_DESC: return "invalid resources description";
            case rtxgi::ERROR_DDGI_MAP_FAILURE_CONSTANTS_UPLOAD_BUFFER:
                return "could not map the constants upload buffer";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_RESOURCE_DESCRIPTOR_HEAP:
                return "invalid descriptor heap";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_RAY_DATA: return "no ray data texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_IRRADIANCE:
                return "no irradiance texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_DISTANCE: return "no distance texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_DATA: return "no probe data texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_VARIABILITY:
                return "no variability texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_VARIABILITY_AVERAGE:
                return "no variability average texture";
            case rtxgi::ERROR_DDGI_INVALID_TEXTURE_PROBE_VARIABILITY_READBACK:
                return "no variability readback buffer";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_ROOT_SIGNATURE: return "invalid root signature";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_DESCRIPTOR: return "invalid descriptor";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_PSO_PROBE_BLENDING_IRRADIANCE:
                return "invalid irradiance blending PSO";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_PSO_PROBE_BLENDING_DISTANCE:
                return "invalid distance blending PSO";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_PSO_PROBE_RELOCATION:
                return "invalid relocation PSO";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_PSO_PROBE_CLASSIFICATION:
                return "invalid classification PSO";
            case rtxgi::ERROR_DDGI_D3D12_INVALID_PSO_PROBE_REDUCTION:
                return "invalid reduction PSO";
            default: return "SDK status " + std::to_string(int(s));
        }
    }

    static ID3D12GraphicsCommandList *nativeCmdList(Falcor::RenderContext *ctx) {
        return ctx->getLowLevelData()
            ->getCommandBufferNativeHandle()
            .as<ID3D12GraphicsCommandList *>();
    }

    void describeVolume() {
        desc_ = rtxgi::DDGIVolumeDesc{};
        desc_.index = 0;
        desc_.name = "v6 forest";
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
        desc_.probeHysteresis = hysteresis_;
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
        // word with RTXGI own encoding, which would mean v6 probe trace had to
        // reproduce that packing bit for bit to be read back correctly. Twice
        // the ray-data bandwidth is a cheap price for a format with no way to
        // get subtly wrong; the ray data is written once and read once, by the
        // blend pass, and never leaves the GPU.
        desc_.probeRayDataFormat = rtxgi::EDDGIVolumeTextureFormat::F32x4;
        // F16x4, NOT the U32 the SDK defaults to. U32 is R10G10B10A2_UNORM,
        // packed to keep the atlas small -- but a typed UAV on that format is
        // conditional on the device advertising it among the additional typed
        // UAV formats, and creating one where it is not supported is an
        // INVALID_CALL that takes the device with it. RGBA16F is universally
        // UAV-capable, and at 3200 probes the atlas is a few megabytes either
        // way, so the packing was never buying much here.
        desc_.probeIrradianceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
        desc_.probeDistanceFormat = rtxgi::EDDGIVolumeTextureFormat::F16x2;
        desc_.probeDataFormat = rtxgi::EDDGIVolumeTextureFormat::F16x4;
        desc_.probeVariabilityFormat = rtxgi::EDDGIVolumeTextureFormat::F16;
    }

    // ---------------------------------------------------------------------
    // Falcor's format for a probe texture, derived from the SDK's OWN answer.
    //
    // THIS TAKES THE TEXTURE TYPE, NOT JUST THE FORMAT ENUM, AND THAT IS THE
    // WHOLE POINT. The obvious version of this function maps
    // EDDGIVolumeTextureFormat to a Falcor format and never mentions the type
    // -- and it is wrong, because the SDK does not honour the enum uniformly.
    // GetDDGIVolumeTextureFormat() returns R32G32_FLOAT for VariabilityAverage
    // whatever format was requested, since that texture stores a value and a
    // weight rather than one channel of anything.
    //
    // Creating that texture as R16_FLOAT from the enum, then asking for a UAV
    // in the format the SDK reports, is a view that disagrees with its
    // resource: INVALID_CALL, and the device goes with it -- surfacing several
    // hundred lines later inside Falcor's texture allocator.
    //
    // So the SDK is asked what the format is and the answer is translated.
    // There is one source of truth and it is not this file.
    // ---------------------------------------------------------------------
    static Falcor::ResourceFormat falcorFormat(rtxgi::EDDGIVolumeTextureType type,
                                               rtxgi::EDDGIVolumeTextureFormat f) {
        switch (rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, f)) {
            case DXGI_FORMAT_R10G10B10A2_UNORM: return Falcor::ResourceFormat::RGB10A2Unorm;
            case DXGI_FORMAT_R16_FLOAT: return Falcor::ResourceFormat::R16Float;
            case DXGI_FORMAT_R16G16_FLOAT: return Falcor::ResourceFormat::RG16Float;
            case DXGI_FORMAT_R16G16B16A16_FLOAT: return Falcor::ResourceFormat::RGBA16Float;
            case DXGI_FORMAT_R32_FLOAT: return Falcor::ResourceFormat::R32Float;
            case DXGI_FORMAT_R32G32_FLOAT: return Falcor::ResourceFormat::RG32Float;
            case DXGI_FORMAT_R32G32B32A32_FLOAT: return Falcor::ResourceFormat::RGBA32Float;
            default: return Falcor::ResourceFormat::Unknown;
        }
    }

    bool createTextures() {
        const Falcor::ResourceBindFlags rw = Falcor::ResourceBindFlags::ShaderResource |
                                             Falcor::ResourceBindFlags::UnorderedAccess;
        // THE TWO ATLASES ALSO NEED TO BE RENDER TARGETS, which is not obvious
        // from anything v6 does with them -- nothing here ever draws into them.
        // It is the SDK that insists: ValidateUnmanagedResourcesDesc() rejects a
        // volume whose probeIrradianceRTV or probeDistanceRTV handle is null,
        // because its optional gather-through-a-pixel-shader path needs them. A
        // texture created without ALLOW_RENDER_TARGET cannot have an RTV made
        // for it at all, so the flag has to be set here, at creation.
        const Falcor::ResourceBindFlags rwRt = rw | Falcor::ResourceBindFlags::RenderTarget;

        auto make = [&](rtxgi::EDDGIVolumeTextureType type, rtxgi::EDDGIVolumeTextureFormat fmt,
                        const char *name,
                        Falcor::ResourceBindFlags flags) -> Falcor::ref<Falcor::Texture> {
            uint32_t w = 0, h = 0, n = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, type, w, h, n);
            if (w == 0 || h == 0 || n == 0) return nullptr;
            const Falcor::ResourceFormat rf = falcorFormat(type, fmt);
            if (rf == Falcor::ResourceFormat::Unknown) return nullptr;
            auto t = device_->createTexture2D(w, h, rf, n, 1, nullptr, flags);
            t->setName(name);
            return t;
        };

        using T = rtxgi::EDDGIVolumeTextureType;
        texRayData_ = make(T::RayData, desc_.probeRayDataFormat, "v6::ddgiRayData", rw);
        texIrradiance_ =
            make(T::Irradiance, desc_.probeIrradianceFormat, "v6::ddgiIrradiance", rwRt);
        texDistance_ = make(T::Distance, desc_.probeDistanceFormat, "v6::ddgiDistance", rwRt);
        texProbeData_ = make(T::Data, desc_.probeDataFormat, "v6::ddgiProbeData", rw);
        texVariability_ =
            make(T::Variability, desc_.probeVariabilityFormat, "v6::ddgiVariability", rw);
        texVariabilityAvg_ = make(T::VariabilityAverage, desc_.probeVariabilityFormat,
                                  "v6::ddgiVariabilityAvg", rw);

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

        // THE VARIABILITY READBACK BUFFER IS NOT OPTIONAL, even though probe
        // variability itself is switched off in describeVolume(). Create()
        // validates the pointer unconditionally in unmanaged mode and returns
        // ERROR_DDGI_INVALID_TEXTURE_PROBE_VARIABILITY_READBACK for a null one --
        // which is how this first failed, as a bare "43".
        //
        // Nothing ever reads it. It exists so the validation passes, and it is
        // 256 bytes.
        hp.Type = D3D12_HEAP_TYPE_READBACK;
        rd.Width = 256;
        if (FAILED(d3d_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&variabilityReadback_)))) {
            status_ = "could not create the variability readback buffer";
            return false;
        }
        return true;
    }

    // THE HEAP HAS TO FIT THE LAYOUT, NOT THE SDK'S COUNT.
    //
    // GetDDGIVolumeNumResourceDescriptors() returns 12 -- a UAV and an SRV for
    // each of the six probe textures -- and says nothing about the two extra
    // slots heapDesc() puts in front of them for the volume constants and the
    // resource-indices buffer. Sizing the heap by that 12 while handing out
    // indices up to 13 overruns it, and an overrun descriptor heap is not a
    // validation error: it is a device removal several dispatches later, which
    // is exactly how this first failed.
    static constexpr UINT kHeapSlots = 2 + 12;  // constants, indices, then 6 UAV/SRV pairs

    bool createDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = kHeapSlots;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(d3d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)))) {
            status_ = "could not create the DDGI descriptor heap";
            return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------
    // Fill the heap.
    //
    // IN UNMANAGED MODE THIS IS THE APPLICATION'S JOB AND NOTHING SAYS SO.
    // DDGIVolume::Create() calls CreateDescriptors() only inside
    // `#if RTXGI_DDGI_RESOURCE_MANAGEMENT`; in unmanaged mode it stores the
    // resource pointers and returns OK with an entirely empty heap. The root
    // signature it builds still describes a table over those slots, so the
    // first probe dispatch reads uninitialised descriptors and the device is
    // removed. Create() reporting OK is what makes this one hard to find.
    //
    // The order below must match heapDesc() exactly -- that function is what
    // told the root signature where each range starts.
    // -----------------------------------------------------------------------
    bool createViews() {
        const UINT step =
            d3d_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_CPU_DESCRIPTOR_HANDLE start =
            heap_->GetCPUDescriptorHandleForHeapStart();
        auto at = [&](UINT i) {
            D3D12_CPU_DESCRIPTOR_HANDLE h = start;
            h.ptr += SIZE_T(i) * step;
            return h;
        };

        const rtxgi::d3d12::DDGIVolumeDescriptorHeapDesc hd = heapDesc();

        // -- the volume constants, as a structured buffer SRV (t0, space1) ---
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_UNKNOWN;         // structured, so no format
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = 0;
            d.Buffer.NumElements = 1;               // one volume
            d.Buffer.StructureByteStride = sizeof(rtxgi::DDGIVolumeDescGPUPacked);
            d.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
            d3d_->CreateShaderResourceView(constantsBuf_, &d, at(hd.constantsIndex));
            if (!alive("creating the constants SRV")) return false;
        }

        // -- the resource-indices slot --------------------------------------
        // Only bindless builds bind this, and v6 is not one. The slot still has
        // to hold a valid descriptor because it sits inside the heap the table
        // spans; pointing it at the constants buffer is the cheapest way to
        // make it valid and it is never read.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC d{};
            d.Format = DXGI_FORMAT_UNKNOWN;
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            d.Buffer.FirstElement = 0;
            d.Buffer.NumElements = 1;
            d.Buffer.StructureByteStride = sizeof(rtxgi::DDGIVolumeDescGPUPacked);
            d3d_->CreateShaderResourceView(constantsBuf_, &d, at(hd.resourceIndicesIndex));
            if (!alive("creating the resource-indices SRV")) return false;
        }

        using T = rtxgi::EDDGIVolumeTextureType;
        auto pair = [&](const Falcor::ref<Falcor::Texture> &tex, T type,
                        rtxgi::EDDGIVolumeTextureFormat fmt, UINT uavIndex, UINT srvIndex,
                        const char *name) -> bool {
            uint32_t w = 0, h = 0, n = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, type, w, h, n);
            const DXGI_FORMAT f = rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, fmt);

            // WHAT THE RESOURCE ACTUALLY IS, asked rather than assumed. The
            // dimensions come from the SDK and the format from the SDK, but the
            // resource was created by Falcor -- and a view whose array size or
            // format disagrees with the resource behind it is an INVALID_CALL
            // that removes the device rather than returning an error.
            const D3D12_RESOURCE_DESC rdesc = native(tex)->GetDesc();
            if (rdesc.DepthOrArraySize != n || rdesc.Width != w || rdesc.Height != h) {
                status_ = std::string("the ") + name + " texture is " +
                          std::to_string(rdesc.Width) + "x" + std::to_string(rdesc.Height) + "x" +
                          std::to_string(rdesc.DepthOrArraySize) + " but the SDK wants " +
                          std::to_string(w) + "x" + std::to_string(h) + "x" + std::to_string(n);
                return false;
            }

            D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
            u.Format = f;
            u.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            u.Texture2DArray.MipSlice = 0;
            u.Texture2DArray.FirstArraySlice = 0;
            u.Texture2DArray.ArraySize = n;
            d3d_->CreateUnorderedAccessView(native(tex), nullptr, &u, at(uavIndex));
            if (!alive((std::string("creating the ") + name + " UAV").c_str())) return false;

            D3D12_SHADER_RESOURCE_VIEW_DESC v{};
            v.Format = f;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2DArray.MostDetailedMip = 0;
            v.Texture2DArray.MipLevels = 1;
            v.Texture2DArray.FirstArraySlice = 0;
            v.Texture2DArray.ArraySize = n;
            d3d_->CreateShaderResourceView(native(tex), &v, at(srvIndex));
            if (!alive((std::string("creating the ") + name + " SRV").c_str())) return false;
            return true;
        };

        const rtxgi::DDGIVolumeResourceIndices &ix = hd.resourceIndices;
        return pair(texRayData_, T::RayData, desc_.probeRayDataFormat,
                    ix.rayDataUAVIndex, ix.rayDataSRVIndex, "ray data") &&
               pair(texIrradiance_, T::Irradiance, desc_.probeIrradianceFormat,
                    ix.probeIrradianceUAVIndex, ix.probeIrradianceSRVIndex, "irradiance") &&
               pair(texDistance_, T::Distance, desc_.probeDistanceFormat,
                    ix.probeDistanceUAVIndex, ix.probeDistanceSRVIndex, "distance") &&
               pair(texProbeData_, T::Data, desc_.probeDataFormat,
                    ix.probeDataUAVIndex, ix.probeDataSRVIndex, "probe data") &&
               pair(texVariability_, T::Variability, desc_.probeVariabilityFormat,
                    ix.probeVariabilityUAVIndex, ix.probeVariabilitySRVIndex, "variability") &&
               pair(texVariabilityAvg_, T::VariabilityAverage, desc_.probeVariabilityFormat,
                    ix.probeVariabilityAverageUAVIndex, ix.probeVariabilityAverageSRVIndex,
                    "variability average");
    }

    // The eight .cso files compile_ddgi_shaders.bat produced. Read rather than
    // compiled, which is the whole reason that batch file exists -- see its
    // header for why a runtime DXC is worse.
    // Two render target views the SDK demands and v6 never draws through. See
    // the note in createTextures(); this heap exists only so the handles are
    // non-null and point at the right resources.
    bool createRtvs() {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = UINT(rtxgi::GetDDGIVolumeNumRTVDescriptors());
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;  // CPU-only; never bound
        if (FAILED(d3d_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) {
            status_ = "could not create the DDGI RTV heap";
            return false;
        }
        const UINT step = d3d_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();

        auto rtv = [&](const Falcor::ref<Falcor::Texture> &t, rtxgi::EDDGIVolumeTextureFormat fmt,
                       rtxgi::EDDGIVolumeTextureType type, D3D12_CPU_DESCRIPTOR_HANDLE dst) {
            uint32_t w = 0, hh = 0, n = 0;
            rtxgi::GetDDGIVolumeTextureDimensions(desc_, type, w, hh, n);
            D3D12_RENDER_TARGET_VIEW_DESC d{};
            d.Format = rtxgi::d3d12::GetDDGIVolumeTextureFormat(type, fmt);
            d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MipSlice = 0;
            d.Texture2DArray.FirstArraySlice = 0;
            d.Texture2DArray.ArraySize = n;
            d3d_->CreateRenderTargetView(native(t), &d, dst);
        };

        irradianceRtv_ = h;
        rtv(texIrradiance_, desc_.probeIrradianceFormat,
            rtxgi::EDDGIVolumeTextureType::Irradiance, irradianceRtv_);
        h.ptr += step;
        distanceRtv_ = h;
        rtv(texDistance_, desc_.probeDistanceFormat,
            rtxgi::EDDGIVolumeTextureType::Distance, distanceRtv_);
        return true;
    }

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
        // ONE SLOT, NOT THE WHOLE RING, and the name says the opposite.
        //
        // UploadDDGIVolumeConstants computes its write offset as
        //     GetConstantsBufferSizeInBytes() * bufferingIndex
        // so this field is the STRIDE BETWEEN SLOTS, not the size of the
        // allocation. Setting it to the full ring made slot 1 land three
        // strides past the end of a three-stride buffer -- which is not a
        // validation error, it is a heap overrun, and it took the device out on
        // the SECOND frame while the first looked perfect.
        //
        // The allocation itself is still stride * kUploadRing; see
        // createConstantsBuffers().
        res.constantsBufferSizeInBytes = sizeof(rtxgi::DDGIVolumeDescGPUPacked);

        res.unmanaged.enabled = true;
        res.unmanaged.rootSignature = rootSig_;
        // The slots GetDDGIVolumeRootSignatureDesc() lays out for the SDK's own
        // (non-bindless) signature: root constants first, then the table.
        res.unmanaged.rootParamSlotRootConstants = 0;
        res.unmanaged.rootParamSlotResourceDescriptorTable = 1;
        res.unmanaged.probeIrradianceRTV = irradianceRtv_;
        res.unmanaged.probeDistanceRTV = distanceRtv_;

        res.unmanaged.probeRayData = native(texRayData_);
        res.unmanaged.probeIrradiance = native(texIrradiance_);
        res.unmanaged.probeDistance = native(texDistance_);
        res.unmanaged.probeData = native(texProbeData_);
        res.unmanaged.probeVariability = native(texVariability_);
        res.unmanaged.probeVariabilityAverage = native(texVariabilityAvg_);
        res.unmanaged.probeVariabilityReadback = variabilityReadback_;

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
            status_ = "DDGIVolume::Create failed: " + statusName(s);
            return false;
        }
        return true;
    }

    static ID3D12Resource *native(const Falcor::ref<Falcor::Texture> &t) {
        return t ? t->getNativeHandle().as<ID3D12Resource *>() : nullptr;
    }
};
#endif  // V6_HAS_DDGI

}  // namespace v6
