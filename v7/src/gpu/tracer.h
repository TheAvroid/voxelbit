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
#include "ddgi.h"
#include "sharc.h"
#include "dlss.h"
#include "nrc.h"
#include "restir.h"
#include "atmosphere.h"
#include "volfog.h"
#include "clouds.h"
#include "world.h"

namespace v7 {

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
    // The tone curve's toe. Narkowicz's published constant is 0.03, which cuts
    // a shadow to a fifth before the display transfer runs; see acesFilmic in
    // Tonemap.cs.slang for why a conifer wood cannot afford that.
    float shadowLift = 0.10f;
    // The deep-shadow lift, which is a different thing from the toe above: it
    // reaches zero at deepRange and leaves everything above it alone.
    //
    // OFF BY DEFAULT. It does exactly what it claims to the numbers -- the dark
    // quarter rises 81% and nothing above 96/255 moves -- and it still looked
    // wrong, because a canopy underside reading as near-black is not a fault in
    // the picture. It is a conifer wood at midday: foliage albedo 0.19, and an
    // underside sees almost no sky. Lifting it flattens the one thing that makes
    // the wood read as deep. The slider stays for anyone who wants it.
    float deepLift = 0.0f;
    float deepRange = 0.30f;
    float clampIndirect = 24.0f;  // firefly ceiling on non-primary contributions
    float fogDensity = 0.0022f;   // extinction per metre at y = 0
    float fogHeight = 30.0f;      // e-folding height of the haze, metres
    uint32_t seed = 20260904u;
    // 0 = accumulate without limit. Set while the day/night clock is running,
    // so the film tracks the moving sun instead of being reset by it.
    uint32_t maxAccum = 0u;

    // Sun samples per vertex, and how many bounces get more than one. Costs a
    // shadow ray each rather than a whole extra sample, so it buys down the
    // direct-lighting noise far more cheaply than raising samples per frame.
    int shadowRays = 1;
    int shadowRayDepth = 1;

    // Sky-dome samples per vertex, and how many bounces get more than one.
    //
    // ON BY DEFAULT, because under a canopy the dome is the only light there
    // is and leaving it to be found by chance is what made the shadows read as
    // black -- see the long note on sampleSkySplit in Trace.cs.slang. 0 turns
    // it off and restores the previous estimator exactly.
    int skyRays = 1;
    int skyRayDepth = 1;

    // -- the irradiance cache ------------------------------------------------
    //
    // 0 none, 1 DDGI probes, 2 SHaRC. Defaults to DDGI where the device can
    // run it; app.h drops it to SHaRC on Vulkan, where RTXGI cannot go, and to
    // none if neither came up.
    int giMode = 1;
    // TWO, AND THE NUMBER WAS MEASURED RATHER THAN CHOSEN.
    //
    // Against a 32-bounce, late-roulette reference of the same frame, the
    // darkest quarter of the image comes out:
    //
    //   path tracer alone (depth 6, rr 1)   -12.4 %   too dark, as reported
    //   + sky NEE                            -4.6 %
    //   + DDGI at giDepth 1                 +22.5 %   over-bright
    //   + DDGI at giDepth 2                  +2.2 %
    //
    // At 1 the cache replaces the FIRST bounce, and a six-metre probe grid is
    // too coarse to stand in for light that close to the camera -- it fills the
    // shadows past where they should stop. At 2 the path resolves two bounces
    // honestly and the cache only carries the tail, which is the part that was
    // being thrown away by roulette in the first place.
    int giDepth = 2;
    float giStrength = 1.0f;

    // How many samples the caller will put into the film for this frame. Only
    // the shader cares, and only to decide whether there IS a film -- see
    // keepFilm in Shared.slang.
    int samplesPerFrame = 1;
};

class Tracer {
  public:
    // `coopVec` says whether this device can compile cooperative vectors. It is
    // asked for BEFORE the trace program is built, not after, because the answer
    // decides what source the tracer is made of -- see the note on V7_NRC in
    // Trace.cs.slang.
    void init(const Falcor::ref<Falcor::Device> &device, World *world, bool coopVec = false) {
        device_ = device;
        world_ = world;
        coopVec_ = coopVec;

        Falcor::DefineList defs;
        if (coopVec_) defs.add("V7_NRC", "1");
#if V7_HAS_SHARC
        // The camera pass READS the cache; the update pass WRITES it. They are
        // the same source file compiled twice, because the update has to shade
        // exactly the way the camera shades or the cache records a different
        // renderer -- see the note on sharcUpdateMain in Trace.cs.slang. The
        // SDK's own headers insist the two modes are separate compilations, so
        // this is also the only shape it allows.
        defs.add("V7_SHARC_QUERY", "1");
#endif
        Falcor::ProgramDesc dt;
        dt.addShaderLibrary("v7/shaders/Trace.cs.slang").csEntry("main");
        if (coopVec_ && device_->getType() == Falcor::Device::Type::Vulkan)
            dt.addCompilerArguments({"-capability", "spvCooperativeVectorNV"});
        trace_ = ComputePass::create(device_, dt, defs);

#if V7_HAS_SHARC
        Falcor::DefineList updDefs;
        if (coopVec_) updDefs.add("V7_NRC", "1");
        updDefs.add("V7_SHARC_UPDATE", "1");
        Falcor::ProgramDesc du;
        du.addShaderLibrary("v7/shaders/Trace.cs.slang").csEntry("sharcUpdateMain");
        if (coopVec_ && device_->getType() == Falcor::Device::Type::Vulkan)
            du.addCompilerArguments({"-capability", "spvCooperativeVectorNV"});
        sharcUpdate_ = ComputePass::create(device_, du, updDefs);
#endif
        tonemap_ = ComputePass::create(device_, "v7/shaders/Tonemap.cs.slang", "main");
        // PHASE B, and the back half of PHASE D. Created unconditionally: they
        // are two small compute passes, and having them always present means the
        // demodulated route can be switched on from the settings menu without a
        // rebuild. Neither is DISPATCHED unless the route that needs it is on.
        demodulate_ = ComputePass::create(device_, "v7/shaders/Demodulate.cs.slang", "main");
        remodulate_ = ComputePass::create(device_, "v7/shaders/Remodulate.cs.slang", "main");

        // The probe placeholder and the sampler the irradiance lookup uses.
        // Made here rather than in ddgi.h because they are needed on the frames
        // -- and the backends -- where there is no Ddgi at all.
        //
        // BILINEAR AND CLAMPED, and both halves matter. The lookup leans on
        // hardware filtering to blend across the one-texel border each probe
        // carries, so point sampling would show the octahedral seam; and a
        // wrapping address mode would fetch the probe on the far side of the
        // atlas at every edge, which reads as light leaking out of nowhere.
        ddgiPlaceholder_ = device_->createTexture2D(1, 1, ResourceFormat::RGBA16Float, 1, 1,
                                                    nullptr,
                                                    Falcor::ResourceBindFlags::ShaderResource);
        ddgiPlaceholder_->setName("v7::ddgiPlaceholder");
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Linear);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp);
        ddgiSampler_ = device_->createSampler(sd);

        // ── THE MOON.S FACE ────────────────────────────────────────────────
        //
        // The one image file this renderer loads. Everything else in the wood
        // is generated, so there is no texture pipeline to hang this on -- and
        // it does not need one: a photograph of the near side is a fixed,
        // tidally locked thing that will never want mips, streaming or variants.
        //
        // NOT sRGB. It goes into a linear radiance term, and letting the loader
        // apply a transfer curve here would double one that the tone map is
        // already going to apply.
        //
        // A MISSING FILE IS NOT FATAL. The tracer DECLARES this texture, so
        // something has to be bound -- a 1x1 white pixel leaves a plain white
        // moon rather than a failed dispatch.
        try {
            moonTex_ = Falcor::Texture::createFromFile(device_, moonPath, false, false);
        } catch (const std::exception &) {
            moonTex_ = nullptr;
        }
        if (!moonTex_) {
            const uint32_t white = 0xffffffffu;
            moonTex_ = device_->createTexture2D(1, 1, ResourceFormat::RGBA8Unorm, 1, 1, &white,
                                                Falcor::ResourceBindFlags::ShaderResource);
            moonTex_->setName("v7::moonFallback");
        }

        // The probe rays. Plain Slang on both backends -- it is only the SDK's
        // blend that is D3D12 -- so it is compiled unconditionally and simply
        // never dispatched where there is no volume to fill.
        probeTrace_ = ComputePass::create(device_, "v7/shaders/ProbeTrace.cs.slang", "main");
    }

    // How far the cache reaches, in metres. The encoding normalises world
    // position by this, so it is also the scale the frequency encoding's
    // octaves are measured against: at 128 m the finest octave is about 16 m,
    // which is a stand of trees rather than a needle. Reaching further would
    // ask one small network to hold more wood than it can.
    static constexpr float kNrcExtent = 128.0f;

    void setNrc(Nrc *nrc) { nrc_ = nrc; }
    void setDdgi(Ddgi *d) { ddgi_ = d; }
    void setSharc(Sharc *s) { sharc_ = s; }
    void setRestir(Restir *r) { restir_ = r; }
    void setVolFog(VolFog *f) { volfog_ = f; }
    void setClouds(Clouds *c) { clouds_ = c; }
    void setAtmosphere(Atmosphere *a) { atmo_ = a; }

    // The sun glare and lens flare, drawn in the tone-map pass. 0 disables it.
    float flare = 1.0f;

    // Where the moon photograph lives. Beside the decoration set rather than in
    // it: it is not decor, it is the sky. Same hardcoded-default style the pine
    // and decor paths use.
    std::string moonPath = "C:/voxelbit/game/assets/moon.png";

    // The vignette, 0..1. OFF by default: it is a look, not a correction, and
    // turning one on for someone who did not ask is how a renderer acquires a
    // signature nobody chose.
    float vignette = 0.0f;

    // THE FLARE HAS ITS OWN SUN, AND IT MUST.
    //
    // kSunCosThetaMax is the real sun: a 0.53 degree disc, which at 1080p is
    // about three pixels. It is also what the sun sampling and the MIS weights
    // are built on, so widening it to make the sun VISIBLE would widen the
    // light source as well -- 23x the solid angle, and every shadow edge in
    // the wood goes soft with it.
    //
    // So the light keeps the physical cone and the LOOK gets its own. This is
    // the WebGPU game.s SUN_COS_R, a 1.27 degree half-angle -- a sun about
    // five times life size, which is what the flare radii in blit.js were
    // fitted against and what makes a sun read as a sun rather than a dot.
    float flareSunCosR = 0.999756245f;

    int width() const { return w_; }      // traced
    int height() const { return h_; }
    int outWidth() const { return ow_; }  // presented
    int outHeight() const { return oh_; }

    // What the tone map last filled, which is the presented size unless a
    // reconstruction was asked for and refused: the fallback tone maps the
    // smaller traced image into the same output-sized surface, so only this
    // much of the display texture is a picture.
    int displayWidth() const { return displayW_; }
    int displayHeight() const { return displayH_; }
    uint32_t samples() const { return frame_; }
    // The sub-pixel offset the last frame was traced with. Streamline needs it
    // to undo the jitter before interpolating; the sign convention is the
    // tracer's own -- see the note in reconstruct().
    Vec2 lastJitter() const { return lastJitter_; }

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

    // PHASE B. Whether the tracer also writes the split channels, and the
    // demodulate/remodulate pair runs around whatever denoises them.
    //
    // KEPT SEPARATE FROM denoising() ON PURPOSE -- the two answer different
    // questions. Ray Reconstruction wants the COMPOSITED colour plus its
    // guides and demodulates internally; handing it pre-demodulated radiance
    // would demodulate it twice. So the default route has this OFF while
    // denoising() is on, and it costs exactly nothing there: four stores per
    // pixel skipped in the tracer, and two full-screen passes not dispatched.
    void setDemodulate(bool on) {
        if (on == demod_) return;
        demod_ = on;
        resetAccumulation();
        resetHistory_ = true;
    }
    bool demodulating() const { return demod_; }

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

        accum_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v7::accum");
        color_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v7::color");

        // The guides. Half precision throughout rather than full: these are
        // hints to a network, not radiance, and halving the bandwidth of five
        // full-screen surfaces is worth more than precision nobody reads. Depth
        // is the exception -- it is compared between frames to decide what is a
        // disocclusion, and half floats run out of mantissa across a kilometre.
        guideAlbedo_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::guideAlbedo");
        guideSpecular_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::guideSpecular");
        guideNormRough_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::guideNormRough");
        guideDepth_ = tex(ResourceFormat::R32Float, w_, h_, "v7::guideDepth");
        guideMotion_ = tex(ResourceFormat::RG16Float, w_, h_, "v7::guideMotion");

        // -- PHASE B: the split, and what the division makes of it ----------
        //
        // RGBA16F rather than 32, and that is a BANDWIDTH decision rather than
        // a precision one. These carry radiance with an albedo divided out, so
        // what survives is irradiance -- bounded by how much light reached the
        // point, not by the 10^5 the solar disk itself carries. gEmission is
        // the one channel that CAN hold the disk, so it alone keeps full float.
        //
        // Six extra surfaces touched per frame is the real cost of this phase,
        // and it is exactly why demodulation is not simply always on.
        diffRadiance_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::diffRadiance");
        specRadiance_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::specRadiance");
        emission_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v7::emission");
        demodAlbedo_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::demodAlbedo");
        demodDiff_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::demodDiff");
        demodSpec_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v7::demodSpec");
        // Where Remodulate writes. OUTPUT resolution, because on the Super
        // Resolution route the remultiply happens AFTER the upscale -- which is
        // the whole point of doing it last.
        remodOut_ = tex(ResourceFormat::RGBA32Float, ow_, oh_, "v7::remodOut");

        // What Ray Reconstruction writes: output resolution, still linear.
        dlssOut_ = tex(ResourceFormat::RGBA16Float, ow_, oh_, "v7::dlssOut");
        // Linear, post-curve. The swapchain is an sRGB target and applies the
        // transfer function itself on the blit; encoding it here as well would
        // apply the curve twice and wash the frame out.
        display_ = tex(ResourceFormat::RGBA32Float, ow_, oh_, "v7::display");

        resetAccumulation();
        resetHistory_ = true;
    }

    // Throw the film away. The next dispatch overwrites rather than adds, so
    // this is a counter and not a clear.
    void restirInvalidate() {
        if (restir_) restir_->invalidate();
        restirWarm_ = false;
    }

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
    void renderSample(Falcor::RenderContext *ctx, const V6Camera &cam, const RenderSettings &cfg) {
        // Kept for the tone-map pass, which draws the lens flare and therefore
        // needs to know where the sun lands on screen. resolve() runs after
        // this and has no camera of its own.
        flareCam_ = cam;
        if (!accum_ || !world_->tlasRef()) return;

        V6Params p{};
        p.cam = cam;
        // On the very first frame there is no previous camera; using this one
        // makes every motion vector zero, which is exactly right -- there is no
        // history for them to point into yet.
        p.prevCam = havePrev_ ? prevCam_ : cam;
        p.sky = world_->sky.gpu();
        // The atmosphere's two borrowed words, patched in per frame rather than
        // baked by the Perez fit. Neither is a function of the sun, so making
        // them wait for a sun rebuild would leave the menu toggle a frame late
        // and the viewer's altitude permanently one rebuild stale.
        p.sky.atmoOn = (atmo_ && atmo_->active()) ? 1.0f : 0.0f;
        p.sky.atmoViewHeight = atmo_ ? atmo_->viewHeightKm() : kAtmoGroundR;
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
        // The film is real whenever more than one sample will land in it: the
        // accumulating renderer, a capped window, or several samples a frame.
        // Reconstruction at one sample a frame is the case that has no film.
        p.keepFilm = (!denoise_ || cfg.maxAccum > 0u || cfg.samplesPerFrame > 1) ? 1 : 0;
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
        p.skyRays = cfg.skyRays;
        p.skyRayDepth = cfg.skyRayDepth;
        p.giMode = cfg.giMode;
        p.giDepth = cfg.giDepth;
        p.giStrength = cfg.giStrength;
        // THE GUIDES ARE NOT ONLY FOR THE DENOISER any more. Demodulation
        // divides the specular channel by gGuideSpecular, and NRD reads the
        // normal, depth and motion guides directly -- so the guide block has to
        // run whenever EITHER route is live. Gating it on denoise_ alone left
        // gGuideSpecular unwritten under --demodulate: the divide then hit the
        // epsilon floor and the multiply back hit zero, and the entire specular
        // channel silently vanished from the frame.
        // ReSTIR joins the list of things that need the guides: the temporal
        // pass reprojects on the motion vector and rejects on normal and depth,
        // none of which exist unless these are written.
        // THE FLARE IS IN THIS LIST BECAUSE IT READS THE DEPTH GUIDE. Its
        // occlusion test asks "is something solid in front of the sun", and
        // the guide is where that answer lives -- so a pass that needs it has
        // to ask for it. Without this the flare silently vanished in any
        // configuration that had no other reason to write guides.
        p.writeGuides =
            (denoise_ || demod_ || (restir_ && restir_->active()) || flare > 0.0f) ? 1 : 0;
        p.depthNear = kDepthNear;
        p.depthFar = kDepthFar;
        p.demodulate = demod_ ? 1 : 0;

        // -- the radiance cache --------------------------------------------
        //
        // THE ORIGIN IS SNAPPED, and that is the whole reason it exists. The
        // world is endless, so an absolute position cannot be fed to a network;
        // it has to be relative to something. If that something followed the
        // camera continuously, every input would shift a little every frame and
        // the network would spend its whole life chasing a moving encoding
        // instead of learning the wood. Snapping to a coarse grid means the
        // origin holds still for many metres of walking and then jumps once --
        // one discontinuity to relearn, instead of a permanent one.
        if (nrc_ && nrc_->available()) {
            p.nrcMode = nrc_->enabled ? (nrc_->shouldQuery() ? 2 : 1) : 0;
            p.nrcQueryDepth = nrc_->queryDepth;
            p.nrcTrainEvery = nrc_->trainEvery;
            p.nrcInvExtent = 1.0f / kNrcExtent;
            const float snap = kNrcExtent * 0.5f;
            p.nrcOrigin = float3(std::floor(p.cam.pos.x / snap) * snap, 0.0f,
                                 std::floor(p.cam.pos.z / snap) * snap);
        } else {
            p.nrcMode = 0;
            p.nrcQueryDepth = 0;
            p.nrcTrainEvery = 1;
            p.nrcInvExtent = 1.0f;
            p.nrcOrigin = float3(0.0f);
        }
        // ReSTIR: collect from the first frame, but only SHADE from the
        // reservoirs once there is a resampled result to shade from.
        // The froxel grid, and the limits the tracer must map depth through.
        p.volFog = (volfog_ && volfog_->active()) ? 1 : 0;
        p.fogFar = volfog_ ? volfog_->farD : 400.0f;
        // The phase lobe and the dome moved into the tracer with the radiance:
        // the volume stores only what each cell can SEE. See VolFog.slang.
        p.fogAniso = volfog_ ? volfog_->anisotropy : 0.7f;
        p.fogAmbient = volfog_ ? volfog_->ambient : 0.25f;
        if (volfog_) {
            p.fogOrigin0 = volfog_->originWs(0);
            p.fogCell0 = volfog_->cellSize(0);
            p.fogOrigin1 = volfog_->originWs(1);
            p.fogCell1 = volfog_->cellSize(1);
        } else {
            // A cell size of 1 rather than 0: the march divides by it before it
            // ever checks volFog, and a NaN there would poison the whole frame.
            p.fogOrigin0 = float3(0.0f);
            p.fogCell0 = 1.0f;
            p.fogOrigin1 = float3(0.0f);
            p.fogCell1 = 1.0f;
        }

        // The deck marches only when the cache actually HOLDS something. It is
        // filled a band of slices at a time over the first few frames, and a
        // march against a half-written volume shows as the sky growing clouds
        // in from one end.
        p.clouds = (clouds_ && clouds_->active() && clouds_->filled()) ? 1 : 0;
        p.cloudWindT = clouds_ ? clouds_->windT() : 0.0f;
        p.cloudSun = clouds_ ? clouds_->sunStrength : 2.2f;
        p.cloudAmb = clouds_ ? clouds_->ambStrength : 0.55f;
        p.cloudMoonKey = clouds_ ? clouds_->moonStrength : 16.0f;

        p.restirMode = (restir_ && restir_->active()) ? (restirWarm_ ? 2 : 1) : 0;

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
        // Bound every frame even when demodulation is off. Slang reflects the
        // bindings from the SHADER, not from what the host happens to use, and
        // an unbound UAV on a declared resource is a validation error on the
        // dispatch -- not on the branch that would have written it. Binding a
        // descriptor is free; the STORES are what the flag actually skips.
        var["gDiffRadiance"] = diffRadiance_;
        var["gSpecRadiance"] = specRadiance_;
        var["gEmission"] = emission_;
        var["gDemodAlbedo"] = demodAlbedo_;
        // Bound whether or not the cache is on, for the reason given above: an
        // unbound declared resource fails the dispatch, not the branch.
        if (coopVec_ && nrc_ && nrc_->available()) {
            var["gNrcWeights"] = nrc_->weightBuffer();
            var["gNrcSamples"] = nrc_->sampleBuffer();
        }
        if (volfog_ && volfog_->available()) {
            var["gFogVol0"].setTexture(volfog_->volume(0));
            var["gFogVol1"].setTexture(volfog_->volume(1));
            var["gFogSampler"] = volfog_->sampler();
        }
        // ALWAYS BOUND when the object exists, because the tracer DECLARES the
        // volume and an unbound declared resource fails the dispatch rather
        // than the branch. gParams.clouds is what actually gates the march.
        if (clouds_ && clouds_->volume()) {
            var["gCloudVol"].setTexture(clouds_->volume());
            var["gCloudSamp"] = clouds_->sampler();
        }
        // ALWAYS BOUND when the textures exist, for the same reason the cloud
        // volume is: Trace.cs.slang DECLARES the table, and an unbound declared
        // resource fails the whole dispatch rather than the branch that reads
        // it. atmosphere.h therefore creates its textures even when its shaders
        // failed to compile, so this binding cannot be the thing that breaks.
        if (atmo_ && atmo_->skyView()) {
            var["gAtmoSkyView"].setTexture(atmo_->skyView());
            var["gAtmoSamp"] = atmo_->sampler();
        }
        if (moonTex_) {
            var["gMoonTex"].setTexture(moonTex_);
            var["gMoonSamp"] = ddgiSampler_;
        }
        if (restir_ && restir_->available()) {
            var["gRestirCandidate"] = restir_->candidateBuffer();
            var["gRestirFinal"] = restir_->finalBuffer();
            var["gPrimaryPos"] = restir_->primaryPos();
        }

        // -- the irradiance probes -------------------------------------------
        //
        // ALWAYS BOUND, even on Vulkan where the volume can never come up: the
        // shader declares the three arrays unconditionally and D3D12 validates
        // descriptors at dispatch, not at first use. A 1x1 placeholder costs
        // one texture for the life of the process and removes a whole class of
        // "works until the toggle is off" failure.
        //
        // The constants go up either way too. With the volume off they are
        // zeroed, and a zero probeCounts is what stops ddgiSampleIrradiance
        // from ever looking at the placeholder -- but giMode is 0 in that case
        // and the branch is not taken at all, so this is the second of two
        // independent reasons nothing reads it.
        // ref, not the raw pointer the accessors hand back: Falcor only ever
        // instantiates ShaderVar::operator= for ref<Texture>, so binding a
        // Texture* compiles happily against the template and then fails to
        // link, naming a mangled symbol that says nothing about which line.
        const bool ddgiLive = ddgi_ && ddgi_->available();
        var["gDdgiIrradiance"] =
            ddgiLive ? Falcor::ref<Texture>(ddgi_->irradiance()) : ddgiPlaceholder_;
        var["gDdgiDistance"] =
            ddgiLive ? Falcor::ref<Texture>(ddgi_->distance()) : ddgiPlaceholder_;
        var["gDdgiProbeData"] =
            ddgiLive ? Falcor::ref<Texture>(ddgi_->probeData()) : ddgiPlaceholder_;
        var["gDdgiSampler"] = ddgiSampler_;
        const V6DdgiConsts dc = ddgiLive ? ddgi_->consts() : V6DdgiConsts{};
        var["gDdgiCB"]["gDdgiConsts"].setBlob(&dc, sizeof(dc));

#if V7_HAS_SHARC
        // Bound whenever the cache exists, for the same reason as the probes:
        // the shader declares the buffers unconditionally.
        if (sharc_ && sharc_->available())
            sharc_->bind(var, Vec3(cam.pos.x, cam.pos.y, cam.pos.z));
#endif

        var["gParamsCB"]["gParams"].setBlob(&p, sizeof(p));

#if V7_HAS_SHARC
        // -- the hash cache: update, resolve, and only then render ------------
        //
        // ALL THREE IN THIS ORDER, INSIDE ONE SAMPLE. The update writes raw
        // sums, the resolve folds them into the running average, and the render
        // reads that average -- so resolving before updating would average last
        // frame's samples into this frame's, and rendering before resolving
        // would read half-written entries. Neither goes wrong loudly.
        //
        // Done here rather than from the app because the launch parameters the
        // update pass needs are `p`, which is assembled just above and nowhere
        // else. Handing it out through an accessor so somebody else could call
        // two dispatches in a fixed order would be a wider interface for
        // nothing -- the same argument runRestir makes.
        if (sharc_ && sharc_->available() && p.giMode == 2 && sharcUpdate_) {
            const Vec3 camPos(cam.pos.x, cam.pos.y, cam.pos.z);
            if (sharc_->needsClear()) sharc_->clear(ctx);

            auto uv = sharcUpdate_->getRootVar();
            uv["gScene"].setAccelerationStructure(world_->tlasRef());
            uv["gTriPool"] = world_->triPool();
            uv["gInstances"] = world_->instanceBuffer();
            uv["gMaterials"] = world_->materialBuffer();
            sharc_->bind(uv, camPos);
            uv["gParamsCB"]["gParams"].setBlob(&p, sizeof(p));

            // One path per 5x5 block: 4 % of the pixels, which the SDK's
            // integration guide recommends as the starting point and which is
            // enough to cover the screen over a second of frames.
            sharcUpdate_->execute(ctx, uint32_t((w_ + 4) / 5), uint32_t((h_ + 4) / 5));
            ctx->uavBarrier(sharc_->accumBuffer());
            ctx->uavBarrier(sharc_->hashBuffer());

            sharc_->runResolve(ctx, camPos);
        }
#endif

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
    // PHASE B -- divide the texture out.
    //
    // Runs at RENDER resolution, before anything upscales: the albedo it
    // divides by was written by the primary hit, and only exists at the
    // resolution that hit was traced at.
    // -----------------------------------------------------------------------
    void demodulate(Falcor::RenderContext *ctx) {
        if (!demod_ || !demodDiff_) return;
        auto var = demodulate_->getRootVar();
        var["gInDiff"] = diffRadiance_;
        var["gInSpec"] = specRadiance_;
        var["gAlbedo"] = demodAlbedo_;
        var["gSpecAlbedo"] = guideSpecular_;
        var["gOutDiff"] = demodDiff_;
        var["gOutSpec"] = demodSpec_;
        var["gDemodCB"]["gDim"] = uint2(uint32_t(w_), uint32_t(h_));
        var["gDemodCB"]["gEpsilon"] = kDemodEpsilon;
        var["gDemodCB"]["gPad"] = 0.0f;
        demodulate_->execute(ctx, uint32_t(w_), uint32_t(h_));
    }

    // -----------------------------------------------------------------------
    // PHASE D -- put the texture back.
    //
    // `dim` is whatever resolution the lighting arrived at: render resolution
    // if nothing upscaled it, output resolution if Super Resolution ran first.
    // The sources are passed in rather than assumed, for exactly that reason.
    // -----------------------------------------------------------------------
    void remodulate(Falcor::RenderContext *ctx, const Falcor::ref<Texture> &diff,
                    const Falcor::ref<Texture> &spec, const Falcor::ref<Texture> &albedo,
                    const Falcor::ref<Texture> &specAlbedo, uint2 dim) {
        if (!remodOut_) return;
        auto var = remodulate_->getRootVar();
        var["gInDiff"] = diff;
        var["gInSpec"] = spec;
        var["gAlbedo"] = albedo;
        var["gSpecAlbedo"] = specAlbedo;
        var["gEmissionTex"] = emission_;
        var["gOut"] = remodOut_;
        var["gRemodCB"]["gDim"] = dim;
        var["gRemodCB"]["gPad0"] = 0.0f;
        var["gRemodCB"]["gPad1"] = 0.0f;
        remodulate_->execute(ctx, dim.x, dim.y);
    }

    // The straight round trip, with nothing denoising in between. This is what
    // --check-demod measures against gColor, and it is the proof that the
    // split is lossless.
    void remodulatePassthrough(Falcor::RenderContext *ctx) {
        remodulate(ctx, demodDiff_, demodSpec_, demodAlbedo_, guideSpecular_,
                   uint2(uint32_t(w_), uint32_t(h_)));
    }

    const Falcor::ref<Texture> &color() const { return color_; }
    const Falcor::ref<Texture> &remodulated() const { return remodOut_; }
    const Falcor::ref<Texture> &demodDiff() const { return demodDiff_; }
    const Falcor::ref<Texture> &demodSpec() const { return demodSpec_; }
    const Falcor::ref<Texture> &demodAlbedo() const { return demodAlbedo_; }
    const Falcor::ref<Texture> &guideSpecular() const { return guideSpecular_; }
    const Falcor::ref<Texture> &guideNormRough() const { return guideNormRough_; }
    const Falcor::ref<Texture> &guideDepth() const { return guideDepth_; }
    const Falcor::ref<Texture> &guideMotion() const { return guideMotion_; }

    // -----------------------------------------------------------------------
    // The finished frame, tone mapped.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // The two resampling passes, run after a sample has been traced.
    //
    // DRIVEN FROM HERE rather than from the app because the textures they read
    // -- the normal, depth and motion guides -- are this class's, and handing
    // three of them out through accessors just to have somebody else call two
    // dispatches in a fixed order would be a wider interface for no gain.
    // -----------------------------------------------------------------------
    // -----------------------------------------------------------------------
    // Light and integrate the fog grid, before the trace that samples it.
    //
    // DRIVEN FROM HERE for the same reason runRestir is: the previous camera and
    // the sky both live in this class, and handing them out through accessors
    // just so somebody else could call two dispatches in a fixed order would be
    // a wider interface for nothing.
    // -----------------------------------------------------------------------
    void renderVolFog(Falcor::RenderContext *ctx, const V6Camera &cam, float density, float height,
                      bool moving) {
        if (!volfog_ || !volfog_->active() || !world_ || !world_->tlasRef()) return;
        // TICK, NOT THE SAMPLE COUNT, and the difference is a bug that showed as
        // the fog dragging while you ran.
        //
        // frame_ is "samples in the film for THIS camera" and resetAccumulation
        // zeroes it every time the camera moves. Feeding it to the injection
        // pass meant that while walking, the per-cell jitter was handed the same
        // number every frame -- so every cell re-sampled the SAME point inside
        // itself, the shadow ray returned the same hard 0 or 1 forever, and the
        // penumbra the jitter exists to produce never formed. The temporal blend
        // then held that frozen pattern and only refreshed it where cells
        // shifted, which is what the smearing was. Standing still let frame_
        // climb again and the fog visibly re-converged.
        //
        // tick_ is monotonic and never reset -- it is what the path tracer's own
        // sampler and Halton jitter already use, for exactly this reason.
        volfog_->render(ctx, cam, world_->sky.gpu(), density, height, tick_,
                        world_->tlasRef().get(), moving);
    }

    // -----------------------------------------------------------------------
    // PHASE A: cast this frame's probe rays, then let RTXGI blend them in.
    //
    // THE ORDER IS THE WHOLE OF IT, and getting it wrong is silent. The SDK
    // needs its constants uploaded BEFORE the rays are cast, because the ray
    // rotation those constants carry is the rotation this dispatch reads out
    // of consts() to aim with -- upload after and every probe traces one
    // frame's rotation while the blend pass unpacks them with the next one, so
    // radiance is folded into the wrong texels. It does not look like an error,
    // it looks like probes that never quite converge.
    //
    // Called before the camera sample rather than after, so the tracer reads an
    // atlas that already includes this frame's rays.
    // -----------------------------------------------------------------------
    void traceProbes(Falcor::RenderContext *ctx) {
        if (!ddgi_ || !ddgi_->available() || !probeTrace_) return;
        if (!world_ || !world_->tlasRef()) return;

        if (!ddgi_->uploadConstants(ctx)) return;

        const V6DdgiConsts dc = ddgi_->consts();

        auto var = probeTrace_->getRootVar();
        var["gScene"].setAccelerationStructure(world_->tlasRef());
        var["gTriPool"] = world_->triPool();
        var["gInstances"] = world_->instanceBuffer();
        var["gMaterials"] = world_->materialBuffer();
        var["gProbeRayData"] = Falcor::ref<Texture>(ddgi_->rayData());
        var["gProbeIrradiance"] = Falcor::ref<Texture>(ddgi_->irradiance());
        var["gProbeDistance"] = Falcor::ref<Texture>(ddgi_->distance());
        var["gProbeDataTex"] = Falcor::ref<Texture>(ddgi_->probeData());
        var["gProbeSampler"] = ddgiSampler_;
        var["gProbeCB"]["gDdgi"].setBlob(&dc, sizeof(dc));
        // THE PROBES GET THE SAME SKY THE PRIMARY RAYS DO. Leaving them on the
        // fit while the tracer used the tables would put the indirect fill a
        // different colour from the direct light at every hour where the two
        // models disagree -- which is precisely twilight, where almost all of
        // the light in a wood IS the fill.
        V6Sky sky = world_->sky.gpu();
        sky.atmoOn = (atmo_ && atmo_->active()) ? 1.0f : 0.0f;
        sky.atmoViewHeight = atmo_ ? atmo_->viewHeightKm() : kAtmoGroundR;
        var["gProbeCB"]["gProbeSky"].setBlob(&sky, sizeof(sky));
        if (atmo_ && atmo_->skyView()) {
            var["gProbeAtmoSky"].setTexture(atmo_->skyView());
            var["gProbeAtmoSamp"] = atmo_->sampler();
        }
        var["gProbeCB"]["gProbeSeed"] = uint32_t(0x9E3779B9u);
        var["gProbeCB"]["gProbeTick"] = tick_;
        // FEEDING LAST FRAME'S ATLAS BACK IN is what makes the cache
        // multi-bounce -- see the note at the top of ProbeTrace.cs.slang. It is
        // withheld for the first few frames only because an atlas that has
        // never been blended is not yet zero-meaningful, it is uninitialised.
        var["gProbeCB"]["gProbeUseCache"] = (probeFrames_ > 2u) ? 1 : 0;
        var["gProbeCB"]["gProbePad"] = 0.0f;

        probeTrace_->execute(ctx, uint32_t(ddgi_->raysPerProbe()),
                             uint32_t(ddgi_->numProbes()));

        ddgi_->updateProbes(ctx);
        ++probeFrames_;
    }

    void runRestir(Falcor::RenderContext *ctx, uint32_t frame) {
        if (!restir_ || !restir_->active()) return;
        restir_->resize(uint32_t(w_), uint32_t(h_));
        restir_->run(ctx, guideNormRough_, guideDepth_, guideMotion_, frame);
        // From the next frame there is a resampled result worth shading from.
        restirWarm_ = true;
    }

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
        var["gTonemapCB"]["gToe"] = cfg.shadowLift;
        var["gTonemapCB"]["gDeepLift"] = cfg.deepLift;
        var["gTonemapCB"]["gDeepRange"] = cfg.deepRange;

        // The flare. gFlare 0 disables the whole block in the shader.
        var["gFlareDepth"].setTexture(guideDepth_);
        var["gFlareSamp"] = ddgiSampler_;
        const V6Sky sky = world_ ? world_->sky.gpu() : V6Sky{};
        var["gTonemapCB"]["gSunDir"] = sky.sunDir;
        var["gTonemapCB"]["gFlare"] = flare;
        var["gTonemapCB"]["gCamRight"] = flareCam_.u;
        var["gTonemapCB"]["gTanH"] = flareCam_.halfH;
        var["gTonemapCB"]["gCamUp"] = flareCam_.v;
        var["gTonemapCB"]["gAspect"] = flareCam_.halfH > 0.0f ? flareCam_.halfW / flareCam_.halfH : 1.0f;
        var["gTonemapCB"]["gCamFwd"] = flareCam_.w;
        var["gTonemapCB"]["gSunCosR"] = flareSunCosR;
        var["gTonemapCB"]["gVignette"] = vignette;
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

    // The floor under the demodulation divide -- see the note in
    // Demodulate.cs.slang. Small enough that no material in the palette sits
    // near it, large enough that its reciprocal is nowhere near overflowing a
    // half float.
    static constexpr float kDemodEpsilon = 1e-3f;

    Falcor::ref<Falcor::Device> device_;
    World *world_ = nullptr;
    Falcor::ref<ComputePass> trace_, tonemap_, demodulate_, remodulate_;
    Falcor::ref<Texture> accum_, color_, display_, dlssOut_;
    Falcor::ref<Texture> guideAlbedo_, guideSpecular_, guideNormRough_, guideDepth_, guideMotion_;
    Falcor::ref<Texture> diffRadiance_, specRadiance_, emission_, demodAlbedo_;
    Falcor::ref<Texture> demodDiff_, demodSpec_, remodOut_;

    int w_ = 0, h_ = 0, ow_ = 0, oh_ = 0;
    int displayW_ = 0, displayH_ = 0;
    uint32_t frame_ = 0;  // samples in the film for THIS camera
    uint32_t tick_ = 0;   // monotonic; seeds the sampler and picks the jitter
    Nrc *nrc_ = nullptr;
    Ddgi *ddgi_ = nullptr;
    Sharc *sharc_ = nullptr;
    Falcor::ref<ComputePass> sharcUpdate_;

    // A 1x1 array texture standing in for the three probe atlases whenever the
    // volume is not running, and the sampler the lookup reads them all with.
    // Created once in init(); see the note where they are bound.
    Falcor::ref<Texture> ddgiPlaceholder_;
    Falcor::ref<Falcor::Sampler> ddgiSampler_;
    Falcor::ref<Falcor::Texture> moonTex_;
    Falcor::ref<ComputePass> probeTrace_;
    uint32_t probeFrames_ = 0;
    Restir *restir_ = nullptr;
    V6Camera flareCam_ = {};
    VolFog *volfog_ = nullptr;
    Clouds *clouds_ = nullptr;
    Atmosphere *atmo_ = nullptr;
    bool restirWarm_ = false;
    bool coopVec_ = false;
    bool denoise_ = false;
    bool demod_ = false;
    bool resetHistory_ = true;
    bool havePrev_ = false;
    V6Camera prevCam_{};
    Vec2 lastJitter_{0.0f, 0.0f};
    DlssQuality quality_ = DlssQuality::Quality;

    static uint8_t encodeSrgb(float c) {
        c = clampf(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
        return uint8_t(clampf(s * 255.0f + 0.5f, 0.0f, 255.0f));
    }
};

}  // namespace v7
