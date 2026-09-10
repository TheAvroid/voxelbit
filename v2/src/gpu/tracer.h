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

#include <functional>
#include <cstdio>
#include <string>
#include <vector>

#include "../../shaders/Shared.slang"
#include "../core/bluenoise.h"
#include "../core/vecmath.h"
#include "ddgi.h"
#include "sharc.h"
#include "dlss.h"
#include "nrc.h"
#include "restir.h"
#include "nrcsdk.h"
#include "atmosphere.h"
#include "post.h"
#include "volfog.h"
#include "clouds.h"
#include "world.h"

namespace v2 {

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
    // 0.00164 per metre at y = 0. Set by eye against the slider rather than
    // derived from anything: it is the haze the wood is meant to have, and the
    // number only means what it does because the slider now resolves it -- at
    // the old 0..0.10 range this and the previous 0.0022 were the same pixel.
    float fogDensity = 0.00100f;  // user 2026-09-07; was 0.00164
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
    // 0 none, 1 DDGI probes, 2 SHaRC. app.h drops 1 to SHaRC where RTXGI
    // cannot go -- Vulkan, or a build without the SDK -- and to none if
    // neither came up.
    //
    // OFF BY DEFAULT, AND THAT IS A MEASUREMENT RATHER THAN AN OPINION.
    //
    // This defaulted to 1 for a long time, which sounds like DDGI but has not
    // been DDGI in any build without the RTXGI SDK: the fall-through in app.h
    // turns 1 into 2, so what actually ran was SHaRC. A/B on a pinned spawn,
    // and the renderer is deterministic so the control really is zero:
    //
    //   gi 0 against gi 0     0.0000/255 mean,    0 px differ, trace 3.36 ms
    //   gi 0 against SHaRC    0.0450/255 mean, 1073 px differ, trace 3.61 ms
    //
    // A quarter of a millisecond, 7.4% of the trace, to move a fifth of one
    // percent of the pixels by a level of 255. Nobody was ever going to see
    // that, and everybody was paying for it every frame.
    //
    // THE DDGI CASE BELOW IS NOT WHAT WAS MEASURED and still stands: where
    // RTXGI does build, probes at giDepth 2 land within 2% of a 32-bounce
    // reference and are worth having. --gi 1 asks for them.
    int giMode = 0;
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
    // decides what source the tracer is made of -- see the note on V2_NRC in
    // Trace.cs.slang.
    void init(const Falcor::ref<Falcor::Device> &device, World *world, bool coopVec = false,
              bool voxelKey = true, bool nrcVoxelFeatures = true) {
        device_ = device;
        world_ = world;
        coopVec_ = coopVec;
        voxelKey_ = voxelKey;
        nrcVoxelFeatures_ = nrcVoxelFeatures;

        Falcor::DefineList defs;
        if (coopVec_) defs.add("V2_NRC", "1");
        // THE THIRD PLACE THE ENCODING IS SET, and the one that matters most:
        // this is the program that QUERIES the cache. gpu/nrc.h sets the same
        // define on the two training passes, and if these three ever disagree
        // the network is asked a different question from the one it was
        // taught, which looks like a cache that simply does not work.
        if (coopVec_ && nrcVoxelFeatures_) defs.add("V2_NRC_VOXEL_FEATURES", "1");
#if V2_HAS_SHARC
        // The camera pass READS the cache; the update pass WRITES it. They are
        // the same source file compiled twice, because the update has to shade
        // exactly the way the camera shades or the cache records a different
        // renderer -- see the note on sharcUpdateMain in Trace.cs.slang. The
        // SDK's own headers insist the two modes are separate compilations, so
        // this is also the only shape it allows.
        defs.add("V2_SHARC_QUERY", "1");
        // WHICH KEY THE CACHE IS FILED UNDER, and it has to be a define because
        // both halves of the cache -- the query in the camera pass and the
        // insert in the update pass -- must agree. Two programs, one answer.
        if (voxelKey_) defs.add("V2_SHARC_VOXEL_KEY", "1");
#endif
#if V2_HAS_NRCSDK
        // NVIDIA's cache compiles into the SAME tracer, as a third variant of
        // it. Only when it is actually on: the include pulls in the SDK's
        // headers and its buffer bindings, and a program carrying five
        // resources nothing fills is a dispatch-time validation failure.
        if (nrcSdkOn_) defs.add("V2_NRCSDK", "1");
#endif
        Falcor::ProgramDesc dt;
        dt.addShaderLibrary("v2/shaders/Trace.cs.slang").csEntry("main");
        if (coopVec_ && device_->getType() == Falcor::Device::Type::Vulkan)
            dt.addCompilerArguments({"-capability", "spvCooperativeVectorNV"});
        trace_ = ComputePass::create(device_, dt, defs);

#if V2_HAS_SHARC
        Falcor::DefineList updDefs;
        if (coopVec_) updDefs.add("V2_NRC", "1");
        updDefs.add("V2_SHARC_UPDATE", "1");
        if (voxelKey_) updDefs.add("V2_SHARC_VOXEL_KEY", "1");
        Falcor::ProgramDesc du;
        du.addShaderLibrary("v2/shaders/Trace.cs.slang").csEntry("sharcUpdateMain");
        if (coopVec_ && device_->getType() == Falcor::Device::Type::Vulkan)
            du.addCompilerArguments({"-capability", "spvCooperativeVectorNV"});
        sharcUpdate_ = ComputePass::create(device_, du, updDefs);
#endif
#if V2_HAS_NRCSDK
        // THE UPDATE PASS, and it is a separate program for the reason the
        // SHaRC pair is: NRC_UPDATE and NRC_QUERY are compile-time on the
        // SDK's side, so the mode cannot be a uniform. It runs at the training
        // resolution rather than the frame's -- see nrcsdk.h.
        if (nrcSdkOn_) {
            Falcor::DefineList un = defs;
            un.add("V2_NRCSDK_UPDATE", "1");
            Falcor::ProgramDesc du2;
            du2.addShaderLibrary("v2/shaders/Trace.cs.slang").csEntry("main");
            nrcSdkUpdate_ = ComputePass::create(device_, du2, un);
            // The custom resolve, which replaces nrc::Context::Resolve --
            // see the long note at the top of NrcSdkResolve.cs.slang for why
            // the built-in one cannot be used against an accumulating film.
            nrcSdkResolve_ = ComputePass::create(
                device_, "v2/shaders/NrcSdkResolve.cs.slang", "main", defs);
        }
#endif
        tonemap_ = ComputePass::create(device_, "v2/shaders/Tonemap.cs.slang", "main");
        // PHASE B, and the back half of PHASE D. Created unconditionally: they
        // are two small compute passes, and having them always present means the
        // demodulated route can be switched on from the settings menu without a
        // rebuild. Neither is DISPATCHED unless the route that needs it is on.
        demodulate_ = ComputePass::create(device_, "v2/shaders/Demodulate.cs.slang", "main");
        remodulate_ = ComputePass::create(device_, "v2/shaders/Remodulate.cs.slang", "main");

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
        ddgiPlaceholder_->setName("v2::ddgiPlaceholder");


        // -- the blue-noise mask ------------------------------------------
        //
        // GENERATED UNCONDITIONALLY, even with the sampler switched off. It is
        // 32 KB and 22 ms of void-and-cluster, and the alternative -- building
        // it the first time the toggle is used -- puts an allocation and a CPU
        // loop inside a frame, which is a hitch at the exact moment somebody is
        // looking for a difference in the picture.
        {
            const std::vector<float> mask = bluenoise::generateRG();
            blueNoiseTex_ = device_->createTexture2D(
                uint32_t(bluenoise::kDim), uint32_t(bluenoise::kDim), ResourceFormat::RG32Float, 1,
                1, mask.data(), Falcor::ResourceBindFlags::ShaderResource);
            blueNoiseTex_->setName("v2::blueNoise");
        }

        // Auto-exposure and bloom. Failing to come up is not fatal: post.h
        // reports it and both features stay off, which is exactly what they are
        // by default anyway.
        if (!post_.init(device_))
            std::fprintf(stderr, "v2: post: %s\n", post_.status().c_str());
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
            moonTex_->setName("v2::moonFallback");
        }

        // The probe rays. Plain Slang on both backends -- it is only the SDK's
        // blend that is D3D12 -- so it is compiled unconditionally and simply
        // never dispatched where there is no volume to fill.
        probeTrace_ = ComputePass::create(device_, "v2/shaders/ProbeTrace.cs.slang", "main");
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
#if V2_HAS_NRCSDK
    // Told BEFORE init(), because whether the SDK is on decides which
    // programs get compiled.
    void setNrcSdk(NrcSdk *n) { nrcSdk_ = n; nrcSdkOn_ = (n != nullptr); }
    bool nrcSdkResolveDebug = false;
#endif
    void setVolFog(VolFog *f) { volfog_ = f; }
    void setClouds(Clouds *c) { clouds_ = c; }
    void setAtmosphere(Atmosphere *a) { atmo_ = a; }


    // THE SAMPLER'S PATTERN, not its rate. Non-zero routes the shallow sample
    // dimensions -- the shadow ray, the dome ray, the primary bounce, the lens
    // -- through the void-and-cluster mask instead of the pixel's PCG stream.
    // Costs nothing measurable and changes no estimator; see BlueNoise.slang.
    bool blueNoise = false;

    // THE WORLD'S OWN CLOCK, in seconds, and not the wall clock. The meteors
    // are scheduled against it (see meteors() in Sky.slang), so tying it to the
    // day-night cycle rather than to how long the process has been running is
    // what makes a given night reproducible: the same time of day gives the
    // same sky, which is the difference between a reference shot that can be
    // taken twice and one that cannot. It also means scrubbing time scrubs the
    // meteors, which is the behaviour anybody scrubbing would expect.
    float skyTime = 0.0f;
    // THE WATER, for the wave field and for knowing what is under it. waterY is
    // metres and sits far below the world where the band has no water; waterTime
    // is a WALL clock, deliberately NOT the day clock -- scrubbing the sun with
    // the arrow keys must not run the sea at forty times speed, or backwards.
    // Beside skyTime because it is the same kind of thing: a per-frame value the
    // app pushes in, not a setting anybody tunes.
    float waterY = -1.0e4f;
    float waterTime = 0.0f;

    // Auto-exposure and bloom live in their own module because they share a
    // resolution and a number; this is how the command line reaches their
    // knobs. See post.h.
    Post &post() { return post_; }
    const Post &post() const { return post_; }

    // The sun glare and lens flare, drawn in the tone-map pass. 0 disables it.
    float flare = 2.0f;

    // Where the moon photograph lives. Beside the decoration set rather than in
    // it: it is not decor, it is the sky. Same hardcoded-default style the pine
    // and decor paths use.
    std::string moonPath = "C:/voxelbit/game/assets/moon.png";

    // The vignette, 0..1.
    //
    // ON, at a quarter, because the person whose renderer it is asked for it.
    // The note this replaces said a vignette is "a look, not a correction, and
    // turning one on for someone who did not ask is how a renderer acquires a
    // signature nobody chose" -- which is right, and is exactly why the value
    // is stated here rather than buried in the tone map: it is a choice, it is
    // one line, and the slider in the menu undoes it.
    float vignette = 0.25f;

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
    // The moon's, and the fraction of the sun's glare it gets.
    //
    // A TENTH, because "only slightly" is the whole of the request and a moon
    // with the sun's glare stops reading as a moon -- the ghosts down the axis
    // are what a bright source does to a lens, and a night sky with a chain of
    // coloured discs across it is a lens flare with a moon in it. At 0.1 the
    // halo is visible against a dark sky, which is where a tenth of something
    // is still plenty, and the ghosts stay under the noise.
    float moonFlare = 0.10f;
    float flareMoonCosR = 0.999756245f;

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

    // -----------------------------------------------------------------------
    // WHERE THE TOOL IS THIS FRAME, so that next frame can be told where it
    // was. See V6Params::heldPrev0 for what it is for.
    //
    // HELD HERE AND NOT IN THE APP, for the same reason prevCam_ is: what a
    // motion vector measures against is the PREVIOUS FRAME, and this is the one
    // object in the scene whose previous frame has to be stepped in lockstep
    // with the camera's. They are stepped by the same line below, so they
    // cannot come from two different frames -- which, at more than one sample a
    // frame, is exactly the bug the note over that line describes.
    // -----------------------------------------------------------------------
    // `which` identifies WHAT is in the hand -- see heldPrevValid below for why
    // a transform on its own is not enough.
    void setHeldXform(const float *m, float tx, float ty, float tz, bool show, int which) {
        for (int i = 0; i < 9; ++i) held_[i] = m ? m[i] : ((i % 4) == 0 ? 1.0f : 0.0f);
        held_[9] = tx;
        held_[10] = ty;
        held_[11] = tz;
        heldShown_ = show;
        heldWhich_ = which;
    }

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

        accum_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v2::accum");
        color_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v2::color");

        // The guides. Half precision throughout rather than full: these are
        // hints to a network, not radiance, and halving the bandwidth of five
        // full-screen surfaces is worth more than precision nobody reads. Depth
        // is the exception -- it is compared between frames to decide what is a
        // disocclusion, and half floats run out of mantissa across a kilometre.
        guideAlbedo_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::guideAlbedo");
        guideSpecular_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::guideSpecular");
        guideNormRough_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::guideNormRough");
        guideDepth_ = tex(ResourceFormat::R32Float, w_, h_, "v2::guideDepth");
        guideMotion_ = tex(ResourceFormat::RG16Float, w_, h_, "v2::guideMotion");

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
        diffRadiance_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::diffRadiance");
        specRadiance_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::specRadiance");
        emission_ = tex(ResourceFormat::RGBA32Float, w_, h_, "v2::emission");
        demodAlbedo_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::demodAlbedo");
        demodDiff_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::demodDiff");
        demodSpec_ = tex(ResourceFormat::RGBA16Float, w_, h_, "v2::demodSpec");
        // Where Remodulate writes. OUTPUT resolution, because on the Super
        // Resolution route the remultiply happens AFTER the upscale -- which is
        // the whole point of doing it last.
        remodOut_ = tex(ResourceFormat::RGBA32Float, ow_, oh_, "v2::remodOut");

        // What Ray Reconstruction writes: output resolution, still linear.
        dlssOut_ = tex(ResourceFormat::RGBA16Float, ow_, oh_, "v2::dlssOut");
        // Linear, post-curve. The swapchain is an sRGB target and applies the
        // transfer function itself on the blit; encoding it here as well would
        // apply the curve twice and wash the frame out.
        display_ = tex(ResourceFormat::RGBA32Float, ow_, oh_, "v2::display");

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
    void resetHistory() {
        resetHistory_ = true;
        // The eye does not ease into a new place either. A teleport or a resize
        // makes the previous measurement a description of somewhere else, and
        // adapting away from it would be a second of the wrong exposure over
        // the exact frames a jump makes most noticeable.
        post_.reset();
    }

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
        p.waterY = waterY;
        p.waterTime = waterTime;
        // On the very first frame there is no previous camera; using this one
        // makes every motion vector zero, which is exactly right -- there is no
        // history for them to point into yet.
        p.prevCam = havePrev_ ? prevCam_ : cam;
        // ...and where the tool was, in the same breath and under the same
        // condition: no previous frame, no previous pose, and the w of 0 sends
        // the tracer back to the zero it used to write.
        // -- AND IT HAS TO BE THE SAME TOOL, NOT JUST A VISIBLE ONE --------
        //
        // This asked three questions -- is there a previous frame, was
        // something held then, is something held now -- and none of them is
        // "was it the SAME THING". So on the frame the hand changed tools the
        // tracer reprojected the bow's surface through the AXE's object-to-
        // world of the previous frame: a wrong motion vector on every pixel of
        // the item, which under Ray Reconstruction is a smear across the middle
        // of the screen for a frame or two. That is the swap glitch.
        //
        // The BOW showed it worst of the three and that is not a coincidence:
        // its pose is the furthest from the others (z 6.99 against 8.73, and a
        // scale of its own), so the bad transform was wrongest for it.
        //
        // THE TOOL, NOT THE MODEL. The bow steps through fourteen models as it
        // draws, and those share an origin and a pose by construction -- the
        // strip is meshed whole for exactly that reason -- so the transform
        // stays valid across them and invalidating per model would throw away a
        // good motion vector on every frame of a draw. What is not valid is
        // carrying one across a change of OBJECT.
        p.heldPrevValid =
            (havePrev_ && heldPrevShown_ && heldShown_ && heldWhich_ == heldPrevWhich_) ? 1 : 0;
        p.heldPrev0 = float4(heldPrev_[0], heldPrev_[1], heldPrev_[2], heldPrev_[9]);
        p.heldPrev1 = float4(heldPrev_[3], heldPrev_[4], heldPrev_[5], heldPrev_[10]);
        p.heldPrev2 = float4(heldPrev_[6], heldPrev_[7], heldPrev_[8], heldPrev_[11]);
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
        keepFilm_ = (p.keepFilm != 0);
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
        p.blueNoise = blueNoise ? 1 : 0;
        p.skyTime = skyTime;
        p.skyPad0 = p.skyPad1 = p.skyPad2 = 0.0f;
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

        // -- EVERY BINDING THE TRACER NEEDS, as a lambda rather than in line --
        //
        // Written straight against trace_'s root var until NVIDIA's cache
        // arrived, at which point a SECOND program -- the NRC update pass --
        // needed the identical set. Duplicating a hundred and fifty bindings is
        // how two passes quietly drift into shading differently, which for a
        // cache is not a cosmetic problem: it would be trained on a renderer
        // that is not the one it serves.
        //
        // Captures by reference, so `p` and `cam` are the same objects the
        // caller assembled; only the dispatch DIMENSIONS differ between the two
        // passes, and they are the argument.
        auto bindTrace = [&](const Falcor::ShaderVar &var, uint2 dim) {
        p.dim = dim;
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
        var["gBlueNoise"] = blueNoiseTex_;
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

#if V2_HAS_NRCSDK
        // NVIDIA'S CACHE: five record buffers and the constants it fills in.
        //
        // Bound only when the programs were compiled with V2_NRCSDK -- unlike the
        // probes below, these resources do not exist in the other variant at all,
        // so there is nothing to bind a placeholder to.
        if (nrcSdkOn_ && nrcSdk_ && nrcSdk_->configured()) {
            bindNrcSdk(var);
        }
#endif

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

#if V2_HAS_SHARC
        // Bound whenever the cache exists, for the same reason as the probes:
        // the shader declares the buffers unconditionally.
        if (sharc_ && sharc_->available())
            sharc_->bind(var, Vec3(cam.pos.x, cam.pos.y, cam.pos.z));
#endif

        var["gParamsCB"]["gParams"].setBlob(&p, sizeof(p));
        };

        auto var = trace_->getRootVar();
        bindTrace(var, uint2(uint32_t(w_), uint32_t(h_)));

#if V2_HAS_SHARC
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
            // Before the update pass, which is the one that records a dropped
            // insert. See clearStats in sharc.h for what clearing it later did.
            sharc_->clearStats(ctx);
            const Vec3 camPos(cam.pos.x, cam.pos.y, cam.pos.z);
            if (sharc_->needsClear()) sharc_->clear(ctx);

            auto uv = sharcUpdate_->getRootVar();
            uv["gScene"].setAccelerationStructure(world_->tlasRef());
            uv["gTriPool"] = world_->triPool();
            uv["gInstances"] = world_->instanceBuffer();
            uv["gMaterials"] = world_->materialBuffer();
            uv["gBlueNoise"] = blueNoiseTex_;
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

#if V2_HAS_NRCSDK
        // -- THE CACHE'S OWN PATH TRACE, before the camera's --------------------
        //
        // A SECOND, SMALLER TRACE whose only purpose is to be learnt from. It
        // runs at trainingDimensions rather than the frame's, which
        // ComputeIdealTrainingDimensions sizes so the batch lands near the 64K
        // records the network wants -- a few percent of the pixels, not a
        // second full frame.
        //
        // BEFORE the camera pass, so the records exist by the time QueryAndTrain
        // runs at the end of the frame. It shades through exactly the same code
        // because it IS the same shader, compiled with NRC_UPDATE instead of
        // NRC_QUERY.
        if (nrcSdkOn_ && nrcSdk_ && nrcSdk_->configured() && nrcSdkUpdate_) {
            const uint32_t tw = nrcSdk_->trainingWidth();
            const uint32_t th = nrcSdk_->trainingHeight();
            if (tw > 0 && th > 0) {
                auto uv = nrcSdkUpdate_->getRootVar();
                bindTrace(uv, uint2(tw, th));
                bindNrcSdk(uv);
                nrcSdkUpdate_->execute(ctx, tw, th);
            }
        }
        // ...and the camera pass is restored to the frame's own size, because
        // bindTrace above left p.dim holding the training grid.
        if (nrcSdkOn_ && nrcSdk_ && nrcSdk_->configured())
            bindTrace(var, uint2(uint32_t(w_), uint32_t(h_)));
#endif

        trace_->execute(ctx, uint32_t(w_), uint32_t(h_));

        // RESAMPLE WHAT THAT SAMPLE FOUND, per SAMPLE and not per frame. The
        // trace just filled the candidate buffer with one bounce per pixel;
        // the two resampling passes turn it into what the NEXT trace shades
        // from. Run it once a frame at --spf 4 and three quarters of the
        // candidates would be overwritten before anything resampled them.
        //
        // AFTER the trace, never before: the reservoir the tracer reads is
        // last frame's finished one -- see the note at the shading site in
        // Trace.cs.slang -- and running this first would hand it a result built
        // from a candidate buffer this frame has not written yet.
        runRestir(ctx, uint32_t(tick_));

        ++frame_;
        ++tick_;

        // -- advance the history, ONCE A FRAME AND NOT ONCE A SAMPLE --------
        //
        // frame_ and tick_ are per SAMPLE and belong here: frame_ counts what
        // is in the film, and tick_ has to move or the samples within a frame
        // would all redraw the same one. prevCam_ is a different kind of
        // quantity. It is the camera the PREVIOUS FRAME was drawn from, and
        // that is the only thing a motion vector can be measured against.
        //
        // Setting it every sample was silent at --spf 1 and catastrophic above
        // it. The second sample of a frame found prevCam_ already holding THIS
        // frame's camera, so it wrote a screen of zero motion -- and the guides
        // are overwritten by each sample, so the zeroes are what DLSS got. A
        // reconstruction handed zero motion while the camera walks does not
        // reproject its history at all: it lays the last frame straight over
        // this one, which is ghosting in its most literal form, and it is
        // worst where the two frames disagree most -- a canopy of needles.
        //
        // The Y menu has a Samples / frame slider that runs to 64, so this was
        // one drag away from anyone.
        //
        // The counter comes from cfg, which already carries the count for the
        // film, so the tracer needs no new call from the app to know where a
        // frame ends. >= rather than == so that lowering the slider mid-run
        // cannot strand the counter above the new total.
        if (++sampleInFrame_ >= (cfg.samplesPerFrame > 0 ? cfg.samplesPerFrame : 1)) {
            sampleInFrame_ = 0;
            prevCam_ = cam;
            for (int i = 0; i < 12; ++i) heldPrev_[i] = held_[i];
            heldPrevShown_ = heldShown_;
            heldPrevWhich_ = heldWhich_;
            havePrev_ = true;
        }
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

#if V2_HAS_NRCSDK
    // The five buffers and the constant block, in one place so the query pass
    // and the update pass cannot disagree about them.
    void bindNrcSdk(const Falcor::ShaderVar &var) {
        var["gNrcQueryPathInfo"] = nrcSdk_->buffer(nrc::BufferIdx::QueryPathInfo);
        var["gNrcTrainingPathInfo"] = nrcSdk_->buffer(nrc::BufferIdx::TrainingPathInfo);
        var["gNrcTrainingPathVertices"] = nrcSdk_->buffer(nrc::BufferIdx::TrainingPathVertices);
        var["gNrcQueryRadianceParams"] = nrcSdk_->buffer(nrc::BufferIdx::QueryRadianceParams);
        var["gNrcCounters"] = nrcSdk_->buffer(nrc::BufferIdx::Counter);

        // POPULATED BY THE LIBRARY, not assembled here. It carries the scene
        // bounds, both resolutions, the termination thresholds and the mode, and
        // every one of them is derived from settings the SDK already holds.
        NrcConstants c{};
        nrcSdk_->populateConstants(c);
        var["gNrcCB"]["gNrcConstants"].setBlob(&c, sizeof(c));
    }

    // -----------------------------------------------------------------------
    // THE UPDATE PASS IS NOT WIRED YET, and this is where it goes.
    //
    // The program is compiled (nrcSdkUpdate_, V2_NRCSDK_UPDATE) and its shader
    // side is complete -- Trace.cs.slang writes training vertices in that
    // variant. What is missing is purely host-side plumbing: it needs the SAME
    // ~150 lines of scene, guide, probe and parameter bindings that
    // renderSample sets up inline for trace_, and those are currently written
    // straight into that function against one root var rather than into
    // something both programs can call.
    //
    // Sharing them is mechanical -- lift the block into a member taking a
    // ShaderVar -- but it is a refactor of the hottest function in the engine
    // and it is not worth doing carelessly. Until it is done the SDK cache has
    // no training data, so nrcsdk trains on nothing and must stay behind its
    // flag.
    //
    // The dispatch, once the bindings are shared, is:
    //     bindTrace(var, cam, cfg, trainingWidth(), trainingHeight());
    //     bindNrcSdk(var);
    //     nrcSdkUpdate_->execute(ctx, trainingWidth(), trainingHeight());
    // at the training resolution rather than the frame's -- it is a second,
    // smaller path trace whose only purpose is to be learnt from.
    // -----------------------------------------------------------------------
#endif

#if V2_HAS_NRCSDK
    // Called AFTER QueryAndTrain, which is what fills the radiance buffer this
    // reads. Everything about the ordering is in NrcSdkResolve.cs.slang.
    void runNrcSdkResolve(Falcor::RenderContext *ctx) {
        if (!nrcSdkOn_ || !nrcSdk_ || !nrcSdk_->configured() || !nrcSdkResolve_) return;
        if (!nrcSdkScratch_ || nrcSdkScratch_->getWidth() != uint32_t(w_) ||
            nrcSdkScratch_->getHeight() != uint32_t(h_)) {
            // Same flags the film uses -- see the tex() lambda in resize(),
            // which is local to it and so not reachable from here.
            const auto rw = Falcor::ResourceBindFlags::ShaderResource |
                            Falcor::ResourceBindFlags::UnorderedAccess;
            nrcSdkScratch_ = device_->createTexture2D(
                uint32_t(w_), uint32_t(h_), ResourceFormat::RGBA32Float, 1, 1, nullptr, rw);
            nrcSdkScratch_->setName("v2::nrcSdkScratch");
        }

        // CLEARED FIRST, because the SDK's resolve mode is ADD. Left dirty it
        // would accumulate its own output frame on frame into a runaway.
        ctx->clearUAV(nrcSdkScratch_->getUAV().get(), Falcor::float4(0.0f));

        // The SDK writes the prediction here rather than into the film. Its own
        // resolve is known good -- its debug views draw the wood -- and this is
        // the only thing that was ever wrong with using it.
        if (!nrcSdk_->resolve(ctx, nrcSdkScratch_.get())) return;
        ctx->uavBarrier(nrcSdkScratch_.get());
        auto var = nrcSdkResolve_->getRootVar();
        var["gNrcScratch"] = nrcSdkScratch_;
        var["gAccum"] = accum_;
        var["gColorOut"] = color_;
        var["NrcResolveCB"]["gResolveDim"] = uint2(uint32_t(w_), uint32_t(h_));
        var["NrcResolveCB"]["gResolveKeepFilm"] = keepFilm_ ? 1u : 0u;
        nrcSdkResolve_->execute(ctx, uint32_t(w_), uint32_t(h_));
    }
#endif

    void runRestir(Falcor::RenderContext *ctx, uint32_t frame) {
        if (!restir_ || !restir_->active()) return;
        restir_->resize(uint32_t(w_), uint32_t(h_));
        restir_->run(ctx, guideNormRough_, guideDepth_, guideMotion_, frame,
                     Falcor::float2(lastJitter_.x, lastJitter_.y));
        // From the next frame there is a resampled result worth shading from.
        restirWarm_ = true;
    }

    // `dt` is wall-clock seconds since the last resolve, and it is only ever
    // read by the exposure adaptation. It defaults to zero so the offline path
    // -- which has no previous frame and wants the measurement taken as the
    // answer rather than eased toward -- needs no argument at all.
    void resolve(Falcor::RenderContext *ctx, const RenderSettings &cfg, bool fromDenoiser,
                 float dt = 0.0f) {
        if (!display_) return;
        const Falcor::ref<Texture> &src = fromDenoiser ? dlssOut_ : color_;
        const uint2 dim = fromDenoiser ? uint2(uint32_t(ow_), uint32_t(oh_))
                                       : uint2(uint32_t(w_), uint32_t(h_));

        // -- measure, then bloom, then map --------------------------------
        //
        // SIZED FROM `dim` RATHER THAN FROM ow_/oh_, because those two are not
        // the same thing on the route that does not upscale: without Ray
        // Reconstruction the tone map reads the RENDER-resolution image out of
        // a display-resolution texture, and a bloom chain built for the larger
        // of the two would sample a rectangle of it that was never written.
        // resize() no-ops when nothing changed, so this costs a comparison.
        post_.resize(int(dim.x), int(dim.y));
        post_.run(ctx, src, dim.x, dim.y, cfg.exposure, dt);

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
        // ...AND THE MOON'S, at a fraction of it. The moon is the same
        // geometry to this shader -- a small bright disc -- so it takes the
        // same glare rather than a second effect, at moonFlare's own strength.
        // Its angular radius is its own: the two discs subtend nearly the same
        // angle in the sky but this engine draws them at different sizes, so
        // borrowing the sun's would put the moon's halo around the wrong circle.
        var["gTonemapCB"]["gMoonDir"] = sky.moonDir;
        var["gTonemapCB"]["gMoonFlare"] = flare * moonFlare;
        var["gTonemapCB"]["gMoonCosR"] = flareMoonCosR;
        var["gTonemapCB"]["gVignette"] = vignette;

        // The bloom, and the stop. Both resources are bound every frame whether
        // or not they are read -- the placeholder stands in for the chain when
        // there is no bloom -- for the reason spelled out over the demodulation
        // bindings above: reflection comes from the shader, not from the host's
        // intentions.
        const bool bloomOn = post_.bloomActive();
        var["gBloom"] = bloomOn ? post_.bloomTexture() : ddgiPlaceholder_;
        var["gExposureState"] = post_.exposureState();
        var["gTonemapCB"]["gBloomStrength"] = bloomOn ? post_.bloom : 0.0f;
        var["gTonemapCB"]["gUseAuto"] = post_.exposureActive() ? 1u : 0u;

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
    Falcor::ref<Texture> blueNoiseTex_;
    Post post_;
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
#if V2_HAS_NRCSDK
    NrcSdk *nrcSdk_ = nullptr;
    bool nrcSdkOn_ = false;
    bool keepFilm_ = true;
    Falcor::ref<ComputePass> nrcSdkUpdate_;
    Falcor::ref<ComputePass> nrcSdkResolve_;
    Falcor::ref<Texture> nrcSdkScratch_;
#endif
    V6Camera flareCam_ = {};
    VolFog *volfog_ = nullptr;
    Clouds *clouds_ = nullptr;
    Atmosphere *atmo_ = nullptr;
    bool restirWarm_ = false;
    bool coopVec_ = false;
    bool voxelKey_ = true;
    bool nrcVoxelFeatures_ = true;
    bool denoise_ = false;
    bool demod_ = false;
    bool resetHistory_ = true;
    bool havePrev_ = false;
    V6Camera prevCam_{};
    // The tool's object-to-world, this frame and last: nine of rotation and
    // three of translation. Stepped with prevCam_ above.
    float held_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    float heldPrev_[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
    bool heldShown_ = false, heldPrevShown_ = false;
    // Which tool the transform above describes, and which one the previous
    // frame's described. -1 for an empty hand.
    int heldWhich_ = -1, heldPrevWhich_ = -1;
    // How many samples of the current frame have gone in. See the note where
    // it is stepped: it is what keeps prevCam_ a per-FRAME quantity.
    int sampleInFrame_ = 0;
    Vec2 lastJitter_{0.0f, 0.0f};
    DlssQuality quality_ = DlssQuality::Quality;

    static uint8_t encodeSrgb(float c) {
        c = clampf(c, 0.0f, 1.0f);
        const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
        return uint8_t(clampf(s * 255.0f + 0.5f, 0.0f, 255.0f));
    }
};

}  // namespace v2
