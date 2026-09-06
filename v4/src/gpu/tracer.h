// ---------------------------------------------------------------------------
// tracer.h -- the film, and the dispatches that fill it.
//
// The whole renderer is two compute passes. One traces a sample per pixel; one
// tone maps what comes out. There is no pipeline to link, no binding table to
// publish, no miss shader, and no third artefact on disk that has to be kept in
// step with the exe -- which under OptiX was a real failure mode, because host
// and device shared a struct and a stale pair disagreed about where every field
// of it lived.
//
// BETWEEN THOSE TWO THERE ARE NOW TWO WAYS TO GET FROM ONE SAMPLE TO A PICTURE,
// and they are genuinely different renderers wearing the same tracer:
//
//   ACCUMULATE  the film sums samples for as long as the camera holds still.
//               Unbiased, converges to the right answer, and needs nothing but
//               a buffer -- but a MOVING camera throws it away every frame, so
//               walking is a one-sample image, and one sample of a path tracer
//               is mostly noise. Every engine in this lineage has looked like
//               that while you walked.
//
//   RECONSTRUCT hand DLSS Ray Reconstruction the one-sample image plus the
//               albedo, normal, roughness, depth and motion that produced it,
//               and let it carry the history instead. It is not filtering the
//               noise away -- it is reconstructing the lighting from a guide to
//               what the surface underneath actually is, which is the one thing
//               that lets a voxel canopy keep its needles.
//
// The accumulation stays, and not only as a fallback: an offline render is
// still the honest estimator, and --out uses it. Ray Reconstruction is temporal
// and there is nothing temporal about one frame.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"
#include "Utils/Image/Bitmap.h"

#include <string>
#include <vector>

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"
#include "dlss.h"
#include "world.h"

namespace v4 {

using Falcor::ComputePass;
using Falcor::ResourceFormat;
using Falcor::Texture;

struct RenderSettings {
    int width = 1280;
    int height = 720;
    int spp = 64;      // offline only; the viewer accumulates one at a time
    int maxDepth = 10;
    int rrStart = 1;   // see the note on Russian roulette in Trace.cs.slang
    float exposure = 1.0f;
    float clampIndirect = 24.0f;  // firefly ceiling on non-primary contributions
    float fogDensity = 0.0022f;   // extinction per metre at y = 0
    float fogHeight = 26.0f;      // e-folding height of the haze, metres
    uint32_t seed = 20260904u;
    // 0 = accumulate without limit. Set while the day/night clock is running,
    // so the film tracks the moving sun instead of being reset by it.
    uint32_t maxAccum = 0u;

    // Sun samples per vertex, and how many bounces get more than one. Costs a
    // shadow ray each rather than a whole extra sample, so it buys down the
    // direct-lighting noise far more cheaply than raising samples per frame.
    int shadowRays = 1;
    int shadowRayDepth = 1;
};

class Tracer {
  public:
    void init(const Falcor::ref<Falcor::Device> &device, World *world) {
        device_ = device;
        world_ = world;
        trace_ = ComputePass::create(device_, "v4/shaders/Trace.cs.slang", "main");
        tonemap_ = ComputePass::create(device_, "v4/shaders/Tonemap.cs.slang", "main");
    }

    int width() const { return w_; }      // traced
    int height() const { return h_; }
    int outWidth() const { return ow_; }  // presented
    int outHeight() const { return oh_; }
    uint32_t samples() const { return frame_; }
    const Falcor::ref<Texture> &display() const { return display_; }

    // Ray Reconstruction replaces the film, so nothing accumulates while it is
    // on -- see the note at the top of the file.
    void setDenoising(bool on) {
        if (on == denoise_) return;
        denoise_ = on;
        resetAccumulation();
        resetHistory_ = true;
    }
    bool denoising() const { return denoise_; }

    void setQuality(DlssQuality q) {
        if (q == quality_) return;
        quality_ = q;
        resetHistory_ = true;
    }
    DlssQuality quality() const { return quality_; }

    // -----------------------------------------------------------------------
    // `render` is what gets traced, `out` is what gets shown. They differ only
    // when Ray Reconstruction is upscaling.
    // -----------------------------------------------------------------------
    void resize(int renderW, int renderH, int outW, int outH) {
        renderW = maxi(8, renderW);
        renderH = maxi(8, renderH);
        outW = maxi(8, outW);
        outH = maxi(8, outH);
        if (renderW == w_ && renderH == h_ && outW == ow_ && outH == oh_) return;
        w_ = renderW;
        h_ = renderH;
        ow_ = outW;
        oh_ = outH;

        const Falcor::ResourceBindFlags rw =
            Falcor::ResourceBindFlags::ShaderResource | Falcor::ResourceBindFlags::UnorderedAccess;
        auto tex = [&](ResourceFormat f, int width, int height, const char *name) {
            auto t = device_->createTexture2D(width, height, f, 1, 1, nullptr, rw);
            t->setName(name);
            return t;
        };

        accum_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v4::accum");
        color_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v4::color");

        // The guides. Half precision throughout rather than full: these are
        // hints to a network, not radiance, and halving the bandwidth of five
        // full-screen surfaces is worth more than precision nobody reads. Depth
        // is the exception -- it is compared between frames to decide what is a
        // disocclusion, and half floats run out of mantissa across a kilometre.
        guideAlbedo_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v4::guideAlbedo");
        guideSpecular_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v4::guideSpecular");
        guideNormRough_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v4::guideNormRough");
        guideDepth_ = tex(ResourceFormat::R32Float, w_, h_, "v4::guideDepth");
        guideMotion_ = tex(ResourceFormat::RG16Float, w_, h_, "v4::guideMotion");

        // What Ray Reconstruction writes: output resolution, still linear.
        dlssOut_ = tex(ResourceFormat::RGBA16Float, ow_, oh_, "v4::dlssOut");
        // Linear, post-curve. The swapchain is an sRGB target and applies the
        // transfer function itself on the blit; encoding it here as well would
        // apply the curve twice and wash the frame out.
        display_ = tex(ResourceFormat::RGBA32Float, ow_, oh_, "v4::display");

        resetAccumulation();
        resetHistory_ = true;
    }

    // Throw the film away. The next dispatch overwrites rather than adds, so
    // this is a counter and not a clear.
    void resetAccumulation() { frame_ = 0; }

    // Tell the denoiser its history describes a world that no longer exists --
    // a resize, a quality change, a teleport. NOT for ordinary camera motion,
    // which is exactly what the motion vectors are for; resetting on that would
    // throw away the history on every frame you walked, which is the whole
    // thing Ray Reconstruction is here to keep.
    void resetHistory() { resetHistory_ = true; }

    // -----------------------------------------------------------------------
    // One sample per pixel.
    // -----------------------------------------------------------------------
    void renderSample(Falcor::RenderContext *ctx, const V4Camera &cam, const RenderSettings &cfg) {
        if (!accum_ || !world_->tlasRef()) return;

        V4Params p{};
        p.cam = cam;
        // On the very first frame there is no previous camera; using this one
        // makes every motion vector zero, which is exactly right -- there is no
        // history for them to point into yet.
        p.prevCam = havePrev_ ? prevCam_ : cam;
        p.sky = world_->sky.gpu();
        p.dim = uint2(uint32_t(w_), uint32_t(h_));
        p.frame = frame_;
        p.tick = tick_;
        p.seed = cfg.seed;
        p.maxAccum = cfg.maxAccum;
        p.maxDepth = cfg.maxDepth;
        p.rrStart = cfg.rrStart;
        p.clampIndirect = cfg.clampIndirect;
        p.fogDensity = cfg.fogDensity;
        p.fogHeight = cfg.fogHeight;
        p.paramsPad = 0.0f;
        // The jitter is a whole-frame Halton offset, driven by the monotonic
        // tick rather than the accumulation index -- see the note on the two
        // counters in Shared.slang. It was always here for the upscaler that
        // did not exist yet: a temporal reconstruction gains detail only if
        // successive frames land on DIFFERENT sub-pixel positions, and white
        // noise clumps.
        const Vec2 j = haltonJitter(tick_);
        p.jitter = float2(j.x, j.y);
        lastJitter_ = j;
        p.shadowRays = cfg.shadowRays;
        p.shadowRayDepth = cfg.shadowRayDepth;
        p.writeGuides = denoise_ ? 1 : 0;
        p.depthNear = kDepthNear;
        p.depthFar = kDepthFar;
        p.guidePad = 0.0f;

        auto var = trace_->getRootVar();
        var["gScene"].setAccelerationStructure(world_->tlasRef());
        var["gTriPool"] = world_->triPool();
        var["gInstances"] = world_->instanceBuffer();
        var["gMaterials"] = world_->materialBuffer();
        var["gAccum"] = accum_;
        var["gColor"] = color_;
        var["gGuideAlbedo"] = guideAlbedo_;
        var["gGuideSpecular"] = guideSpecular_;
        var["gGuideNormRough"] = guideNormRough_;
        var["gGuideDepth"] = guideDepth_;
        var["gGuideMotion"] = guideMotion_;
        var["gParamsCB"]["gParams"].setBlob(&p, sizeof(p));

        trace_->execute(ctx, uint32_t(w_), uint32_t(h_));
        ++frame_;
        ++tick_;
        prevCam_ = cam;
        havePrev_ = true;
    }

    // -----------------------------------------------------------------------
    // Ray Reconstruction: one noisy sample and its guides in, one clean frame
    // at output resolution out.
    // -----------------------------------------------------------------------
    bool reconstruct(Falcor::RenderContext *ctx, Dlss &dlss) {
        if (!denoise_ || !dlssOut_) return false;
        if (!dlss.resize(ctx, uint2(uint32_t(w_), uint32_t(h_)),
                         uint2(uint32_t(ow_), uint32_t(oh_)), quality_))
            return false;

        Dlss::Inputs in;
        in.color = color_.get();
        in.output = dlssOut_.get();
        in.depth = guideDepth_.get();
        in.motion = guideMotion_.get();
        in.diffuseAlbedo = guideAlbedo_.get();
        in.specularAlbedo = guideSpecular_.get();
        in.normalRoughness = guideNormRough_.get();
        // THE SIGN IS THE CLASSIC TRAP. DLSS defines the jitter the way a
        // rasteriser applies it -- an offset added to the projection, which
        // moves the image +jitter pixels. This tracer instead builds the ray
        // for pixel + jitter, which moves the image -jitter. The two
        // conventions are opposites, and getting it wrong does not look broken:
        // it looks like an image that is slightly soft and never quite
        // resolves, which is indistinguishable from the denoiser being bad.
        in.jitterX = -lastJitter_.x;
        in.jitterY = -lastJitter_.y;
        in.reset = resetHistory_;
        const bool ok = dlss.evaluate(ctx, in);
        resetHistory_ = false;
        return ok;
    }

    // -----------------------------------------------------------------------
    // The finished frame, tone mapped.
    // -----------------------------------------------------------------------
    void resolve(Falcor::RenderContext *ctx, const RenderSettings &cfg, bool fromDenoiser) {
        if (!display_) return;
        const Falcor::ref<Texture> &src = fromDenoiser ? dlssOut_ : color_;
        const uint2 dim = fromDenoiser ? uint2(uint32_t(ow_), uint32_t(oh_))
                                       : uint2(uint32_t(w_), uint32_t(h_));
        auto var = tonemap_->getRootVar();
        var["gSrc"] = src;
        var["gDst"] = display_;
        var["gTonemapCB"]["gDim"] = dim;
        var["gTonemapCB"]["gExposure"] = cfg.exposure;
        var["gTonemapCB"]["gPad"] = 0.0f;
        tonemap_->execute(ctx, dim.x, dim.y);
        displayW_ = int(dim.x);
        displayH_ = int(dim.y);
    }

    // -----------------------------------------------------------------------
    // The tone-mapped frame, on disk.
    //
    // Read back and encoded here rather than handed to Texture::captureToFile,
    // because the display texture is linear float and a PNG is 8-bit sRGB. The
    // transfer function is the one the swapchain would have applied, done once
    // on the host, so a screenshot and the window agree.
    // -----------------------------------------------------------------------
    bool writePng(Falcor::RenderContext *ctx, const std::string &path) {
        if (!display_ || displayW_ <= 0) return false;
        const std::vector<uint8_t> raw = ctx->readTextureSubresource(display_.get(), 0);
        const size_t n = size_t(displayW_) * size_t(displayH_);
        // The display texture is allocated at the OUTPUT size and the tone map
        // may have filled only the traced sub-rectangle of it, so the rows are
        // strided by the full width even when fewer were written.
        const size_t stride = size_t(ow_);
        if (raw.size() < size_t(ow_) * size_t(oh_) * 4 * sizeof(float)) return false;

        const float *src = reinterpret_cast<const float *>(raw.data());
        std::vector<uint8_t> rgba(n * 4);
        for (int y = 0; y < displayH_; ++y)
            for (int x = 0; x < displayW_; ++x) {
                const size_t s = (size_t(y) * stride + size_t(x)) * 4;
                const size_t d = (size_t(y) * size_t(displayW_) + size_t(x)) * 4;
                for (int c = 0; c < 3; ++c) rgba[d + c] = encodeSrgb(src[s + c]);
                rgba[d + 3] = 255;
            }
        Falcor::Bitmap::saveImage(path, uint32_t(displayW_), uint32_t(displayH_),
                                  Falcor::Bitmap::FileFormat::PngFile,
                                  Falcor::Bitmap::ExportFlags::None, ResourceFormat::RGBA8Unorm,
                                  true, rgba.data());
        return true;
    }

    // The linear radiance, before any curve -- the only way to see what the
    // tracer actually produced. PFM because it is four lines to write and every
    // image tool reads it.
    bool writePfm(Falcor::RenderContext *ctx, const std::string &path) {
        const std::vector<uint8_t> raw = ctx->readTextureSubresource(color_.get(), 0);
        const size_t n = size_t(w_) * size_t(h_);
        if (raw.size() < n * 4 * sizeof(float)) return false;
        const float *src = reinterpret_cast<const float *>(raw.data());

        FILE *f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "PF\n%d %d\n-1.0\n", w_, h_);  // -1.0 marks little-endian
        for (int y = h_ - 1; y >= 0; --y)              // PFM rows run bottom-up
            for (int x = 0; x < w_; ++x) {
                const size_t i = (size_t(y) * w_ + x) * 4;
                const float px[3] = {src[i], src[i + 1], src[i + 2]};
                std::fwrite(px, sizeof(float), 3, f);
            }
        std::fclose(f);
        return true;
    }

  private:
    // The range the guide depth is written against. Only the denoiser reads it,
    // and only to tell one surface from another between frames -- so what
    // matters is that the whole world fits, not that a metre is precise.
    static constexpr float kDepthNear = 0.05f;
    static constexpr float kDepthFar = 8000.0f;

    Falcor::ref<Falcor::Device> device_;
    World *world_ = nullptr;
    Falcor::ref<ComputePass> trace_, tonemap_;
    Falcor::ref<Texture> accum_, color_, display_, dlssOut_;
    Falcor::ref<Texture> guideAlbedo_, guideSpecular_, guideNormRough_, guideDepth_, guideMotion_;

    int w_ = 0, h_ = 0, ow_ = 0, oh_ = 0;
    int displayW_ = 0, displayH_ = 0;
    uint32_t frame_ = 0;  // samples in the film for THIS camera
    uint32_t tick_ = 0;   // monotonic; seeds the sampler and picks the jitter
    bool denoise_ = false;
    bool resetHistory_ = true;
    bool havePrev_ = false;
    V4Camera prevCam_{};
    Vec2 lastJitter_{0.0f, 0.0f};
    DlssQuality quality_ = DlssQuality::Quality;

    static uint8_t encodeSrgb(float c) {
        c = clampf(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
        return uint8_t(clampf(s * 255.0f + 0.5f, 0.0f, 255.0f));
    }
};

}  // namespace v4
