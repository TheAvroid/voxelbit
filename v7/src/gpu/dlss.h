// ---------------------------------------------------------------------------
// dlss.h -- DLSS Ray Reconstruction, as the denoiser and the upscaler at once.
//
// WHY RAY RECONSTRUCTION AND NOT A DENOISER. Every spatial denoiser this
// lineage tried was removed, and always for the same reason: a voxel canopy is
// thousands of needle-sized faces, and a filter that decides how much to blur
// from the noise it can see cannot tell a needle from the grain between two
// needles. The canopy came out as felt.
//
// Ray Reconstruction is trained on the other thing. It is handed the one-sample
// path traced radiance AND the albedo, normal, roughness and depth that
// produced it, so it can separate what is noise in the LIGHTING from what is
// detail in the SURFACE -- and the surface detail is precisely what a voxel
// wood is made of. It also replaces the temporal accumulation rather than
// sitting on top of it: with RR running there is no film, the tracer draws one
// sample a frame and RR carries the history on the motion vectors.
//
// WHY NOT FALCOR'S DLSSPass. Falcor ships one, and it is DLSS Super Resolution
// -- the packman DLSS package it builds against has no Ray Reconstruction
// headers at all (no nvsdk_ngx_defs_dlssd.h). Super Resolution is an upscaler
// that assumes a clean input; fed a one-sample path trace it upscales the
// noise. So this talks to NGX directly, against the full SDK.
//
// WHAT IT IS NOT: this file does not own a single pixel. It hands NGX the
// textures the tracer already wrote and gets one back. Everything about what
// goes IN those textures is in shaders/Trace.cs.slang, which is where it
// belongs.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"

#include <cstdio>
#include <filesystem>
#include <string>

#if V7_HAS_DLSS
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <nvsdk_ngx_helpers_dlssd.h>
#endif

namespace v7 {

// The quality ladder, which for Ray Reconstruction is really a RESOLUTION
// ladder: every rung denoises, and what changes is how many pixels are traced
// before it does. DLAA traces every one.
enum class DlssQuality { UltraPerformance, Performance, Balanced, Quality, Dlaa };

inline const char *dlssQualityName(DlssQuality q) {
    switch (q) {
        case DlssQuality::UltraPerformance: return "ultra performance";
        case DlssQuality::Performance: return "performance";
        case DlssQuality::Balanced: return "balanced";
        case DlssQuality::Quality: return "quality";
        default: return "DLAA";
    }
}

#if !V7_HAS_DLSS
// ---------------------------------------------------------------------------
// Built without the SDK: the same class, permanently unavailable.
//
// A stub rather than an #ifdef at every call site. The engine already has to
// handle "the driver cannot do this" at runtime, so having it also handle "this
// build has no DLSS" costs nothing and keeps the frame loop readable.
// ---------------------------------------------------------------------------
class Dlss {
  public:
    bool available() const { return false; }
    const std::string &status() const { return status_; }
    bool init(const Falcor::ref<Falcor::Device> &, const std::filesystem::path &) { return false; }
    bool optimalRenderSize(uint2, DlssQuality, uint2 *) { return false; }
    bool resize(Falcor::RenderContext *, uint2, uint2, DlssQuality) { return false; }
    struct Inputs {
        Falcor::Texture *color, *output, *depth, *motion;
        Falcor::Texture *diffuseAlbedo, *specularAlbedo, *normalRoughness;
        float jitterX, jitterY;
        bool reset;
    };
    bool evaluate(Falcor::RenderContext *, const Inputs &) { return false; }
    void shutdown() {}

  private:
    std::string status_ = "built without the DLSS SDK (set V7_DLSS_DIR)";
};
#else

inline NVSDK_NGX_PerfQuality_Value toNgxQuality(DlssQuality q) {
    switch (q) {
        case DlssQuality::UltraPerformance: return NVSDK_NGX_PerfQuality_Value_UltraPerformance;
        case DlssQuality::Performance: return NVSDK_NGX_PerfQuality_Value_MaxPerf;
        case DlssQuality::Balanced: return NVSDK_NGX_PerfQuality_Value_Balanced;
        case DlssQuality::Quality: return NVSDK_NGX_PerfQuality_Value_MaxQuality;
        default: return NVSDK_NGX_PerfQuality_Value_DLAA;
    }
}

class Dlss {
  public:
    // AN APPLICATION ID, NOT A PROJECT UUID.
    //
    // NGX offers both: Init(appId, ...) and Init_with_ProjectID(uuid, ...). The
    // project form validates the string as a real UUID and returns
    // InvalidParameter for anything else, which is how this first went wrong --
    // a readable id with the engine's name in it is not a UUID. An
    // unregistered integration is expected to pass an arbitrary application id
    // here, which is what Falcor's own DLSS pass does.
    static constexpr unsigned long long kAppId = 0x763478626974ull;  // 'voxbit'

    ~Dlss() { shutdown(); }

    bool available() const { return ngxReady_ && rrSupported_; }
    const std::string &status() const { return status_; }
    uint2 renderSize() const { return renderSize_; }
    uint2 outputSize() const { return outputSize_; }

    // -----------------------------------------------------------------------
    // Bring NGX up. Failure here is NOT fatal: the engine falls back to
    // accumulating, which is what it did before this file existed, and says so.
    // A denoiser that takes the whole program down when a driver is old is a
    // worse denoiser than none.
    // -----------------------------------------------------------------------
    bool init(const Falcor::ref<Falcor::Device> &device, const std::filesystem::path &searchPath) {
        device_ = device;
        // THE DEVICE IS ASKED WHAT IT IS, not what its handle looks like.
        //
        // This used to test the pointer, and the test did not work. Falcor's
        // NativeHandle::as<T> is a REINTERPRETATION, not a checked cast -- on a
        // Vulkan device it hands back the VkDevice under an ID3D12Device*, which
        // is not null, so the guard passed and the first NGX call walked a
        // Vulkan handle through a D3D12 vtable. The result was a segfault two
        // lines into a function whose whole job was to fail politely.
        //
        // getType() is the question that was actually meant.
        if (device_->getType() != Falcor::Device::Type::D3D12) {
            status_ = "not a D3D12 device (Ray Reconstruction here is D3D12 only)";
            return false;
        }
        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "no D3D12 device handle";
            return false;
        }

        // Where nvngx_dlssd.dll is looked for -- 40 MB of model that NGX loads
        // by name. The build copies it next to the exe, so this is the exe's
        // own folder, and it doubles as the writable path NGX logs into.
        const std::wstring dir = searchPath.wstring();
        const wchar_t *pathList[] = {dir.c_str()};
        NVSDK_NGX_FeatureCommonInfo info = {};
        info.PathListInfo.Path = const_cast<wchar_t **>(&pathList[0]);
        info.PathListInfo.Length = 1;

        NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(kAppId, dir.c_str(), d3d, &info);
        if (NVSDK_NGX_FAILED(r)) {
            status_ = "NGX init failed (" + resultString(r) + ")";
            return false;
        }
        ngxReady_ = true;

        r = NVSDK_NGX_D3D12_GetCapabilityParameters(&params_);
        if (NVSDK_NGX_FAILED(r) || !params_) {
            status_ = "NGX capability query failed (" + resultString(r) + ")";
            return false;
        }

        int available = 0;
        r = params_->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_Available, &available);
        if (NVSDK_NGX_FAILED(r) || !available) {
            // The driver says why, and the reason is usually worth printing:
            // an old driver and an unsupported GPU are very different problems.
            int reason = 0;
            params_->Get(NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult, &reason);
            status_ = "Ray Reconstruction unavailable on this driver/GPU (result " +
                      std::to_string(reason) + ")";
            return false;
        }
        rrSupported_ = true;
        status_ = "ready";
        return true;
    }

    // -----------------------------------------------------------------------
    // What resolution to trace at for a given output size and quality.
    //
    // Asked of NGX rather than hardcoded as a ratio: the optimal input size for
    // a mode is the model's business and it has changed between DLSS versions.
    // -----------------------------------------------------------------------
    bool optimalRenderSize(uint2 output, DlssQuality q, uint2 *out) {
        if (!available()) return false;
        unsigned int ow = 0, oh = 0, maxW = 0, maxH = 0, minW = 0, minH = 0;
        float sharpness = 0.0f;
        const NVSDK_NGX_Result r =
            NGX_DLSSD_GET_OPTIMAL_SETTINGS(params_, output.x, output.y, toNgxQuality(q), &ow, &oh,
                                           &maxW, &maxH, &minW, &minH, &sharpness);
        if (NVSDK_NGX_FAILED(r) || ow == 0 || oh == 0) return false;
        *out = uint2(ow, oh);
        return true;
    }

    // -----------------------------------------------------------------------
    // Create (or recreate) the feature for a render/output size pair.
    // -----------------------------------------------------------------------
    bool resize(Falcor::RenderContext *ctx, uint2 render, uint2 output, DlssQuality q) {
        if (!available()) return false;
        // all(), because comparing two uint2s gives a bool2 and a size that
        // matched in one dimension only would silently keep the old feature.
        if (handle_ && all(render == renderSize_) && all(output == outputSize_) &&
            q == quality_)
            return true;

        releaseFeature(ctx);
        renderSize_ = render;
        outputSize_ = output;
        quality_ = q;

        NVSDK_NGX_DLSSD_Create_Params cp = {};
        cp.InWidth = render.x;
        cp.InHeight = render.y;
        cp.InTargetWidth = output.x;
        cp.InTargetHeight = output.y;
        cp.InPerfQualityValue = toNgxQuality(q);
        // The colour handed over is linear radiance with a sun in it, so the
        // range runs to five figures -- IsHDR is not optional here. Auto
        // exposure lets NGX work that range out for itself rather than being
        // handed an exposure texture the tracer does not otherwise need.
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_IsHDR |
                                  NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                                  NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
        // Roughness travels in the w of the normal texture -- one fetch, one
        // less surface to allocate and keep in step.
        cp.InRoughnessMode = NVSDK_NGX_DLSS_Roughness_Mode_Packed;
        // The depth the tracer writes is the standard DirectX perspective
        // mapping, not linear metres. See ndcDepth() in Trace.cs.slang.
        cp.InUseHWDepth = NVSDK_NGX_DLSS_Depth_Type_HW;

        ID3D12GraphicsCommandList *cmd =
            ctx->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList *>();
        const NVSDK_NGX_Result r =
            NGX_D3D12_CREATE_DLSSD_EXT(cmd, 1, 1, &handle_, params_, &cp);
        if (NVSDK_NGX_FAILED(r)) {
            handle_ = nullptr;
            status_ = "feature creation failed (" + resultString(r) + ")";
            return false;
        }
        // The creation was RECORDED, not executed. NGX allocates and uploads
        // weights on that list, and evaluating before it has run reads them
        // half-written -- which shows up as a driver-side crash rather than
        // anything this code could catch.
        ctx->submit(true);
        return true;
    }

    struct Inputs {
        Falcor::Texture *color = nullptr;
        Falcor::Texture *output = nullptr;
        Falcor::Texture *depth = nullptr;
        Falcor::Texture *motion = nullptr;
        Falcor::Texture *diffuseAlbedo = nullptr;
        Falcor::Texture *specularAlbedo = nullptr;
        Falcor::Texture *normalRoughness = nullptr;
        float jitterX = 0.0f, jitterY = 0.0f;
        bool reset = false;
    };

    bool evaluate(Falcor::RenderContext *ctx, const Inputs &in) {
        if (!handle_ || !available()) return false;

        // Falcor tracks resource states itself, so every texture NGX is about
        // to touch has to be put in the state NGX expects before the raw
        // command list is handed over -- and Falcor has to be told, or its own
        // tracking and the actual state diverge on the next pass.
        ctx->resourceBarrier(in.color, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.depth, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.motion, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.diffuseAlbedo, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.specularAlbedo, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.normalRoughness, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(in.output, Falcor::Resource::State::UnorderedAccess);
        ctx->submit(false);

        NVSDK_NGX_D3D12_DLSSD_Eval_Params ep = {};
        ep.pInColor = native(in.color);
        ep.pInOutput = native(in.output);
        ep.pInDepth = native(in.depth);
        ep.pInMotionVectors = native(in.motion);
        ep.pInDiffuseAlbedo = native(in.diffuseAlbedo);
        ep.pInSpecularAlbedo = native(in.specularAlbedo);
        ep.pInNormals = native(in.normalRoughness);
        ep.pInRoughness = nullptr;  // packed in normals.w; see InRoughnessMode
        ep.InJitterOffsetX = in.jitterX;
        ep.InJitterOffsetY = in.jitterY;
        ep.InRenderSubrectDimensions.Width = renderSize_.x;
        ep.InRenderSubrectDimensions.Height = renderSize_.y;
        // The vectors are already in render pixels, so there is nothing to
        // scale. Writing them that way rather than in normalised screen space
        // is what makes this a 1.0 and not a number to get wrong.
        ep.InMVScaleX = 1.0f;
        ep.InMVScaleY = 1.0f;
        ep.InReset = in.reset ? 1 : 0;

        ID3D12GraphicsCommandList *cmd =
            ctx->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList *>();
        const NVSDK_NGX_Result r = NGX_D3D12_EVALUATE_DLSSD_EXT(cmd, handle_, params_, &ep);
        if (NVSDK_NGX_FAILED(r)) {
            status_ = "evaluate failed (" + resultString(r) + ")";
            return false;
        }
        // NGX bound its own descriptor heaps and pipeline state on that list.
        // Falcor's next pass assumes the state it left behind, so the two have
        // to be separated by a submit or the following dispatch runs with
        // NGX's bindings.
        ctx->submit(false);
        return true;
    }

    void shutdown() {
        if (!ngxReady_) return;
        if (handle_) {
            NVSDK_NGX_D3D12_ReleaseFeature(handle_);
            handle_ = nullptr;
        }
        if (params_) {
            NVSDK_NGX_D3D12_DestroyParameters(params_);
            params_ = nullptr;
        }
        if (device_) {
            ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
            if (d3d) NVSDK_NGX_D3D12_Shutdown1(d3d);
        }
        ngxReady_ = false;
        rrSupported_ = false;
    }

  private:
    Falcor::ref<Falcor::Device> device_;
    NVSDK_NGX_Parameter *params_ = nullptr;
    NVSDK_NGX_Handle *handle_ = nullptr;
    bool ngxReady_ = false, rrSupported_ = false;
    uint2 renderSize_{0, 0}, outputSize_{0, 0};
    DlssQuality quality_ = DlssQuality::Quality;
    std::string status_ = "not initialised";

    static ID3D12Resource *native(Falcor::Texture *t) {
        return t ? t->getNativeHandle().as<ID3D12Resource *>() : nullptr;
    }

    void releaseFeature(Falcor::RenderContext *ctx) {
        if (!handle_) return;
        // The feature owns GPU allocations the last frames may still be
        // reading, so nothing is released until the device is idle.
        ctx->submit(true);
        NVSDK_NGX_D3D12_ReleaseFeature(handle_);
        handle_ = nullptr;
    }

    static std::string resultString(NVSDK_NGX_Result r) {
        char b[32];
        std::snprintf(b, sizeof(b), "0x%08x", unsigned(r));
        return b;
    }
};
#endif  // V7_HAS_DLSS

}  // namespace v7
