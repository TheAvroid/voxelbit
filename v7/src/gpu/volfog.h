// ---------------------------------------------------------------------------
// volfog.h -- the froxel grid, its two passes, and the textures they live in.
//
// The algorithm is in shaders/VolFog.slang and the passes are VolFogInject and
// VolFogMarch. This file owns the memory and the order, and explains the one
// non-obvious thing about the memory: why there are three grids and not one.
//
// ---------------------------------------------------------------------------
// THREE TEXTURES.
//
//   inject[2]   what the lighting pass produced -- RGB in-scatter, A density.
//               DOUBLED, because the pass reads last frame's result as history
//               while writing this frame's, and a single grid would have it
//               reading values it was in the middle of overwriting.
//   marched     the front-to-back integral -- RGB scattering, A transmittance.
//               What the tracer samples.
//
// The reference integrates in place, reading and writing one grid, which works
// because each slice only touches itself. v7 cannot: doing that would destroy
// the very values the NEXT frame wants as history. The extra grid is 7 MB and
// buys a temporal blend that is the difference between fog and fog that
// flickers.
//
// At 160 x 90 x 64 in RGBA16F each grid is about 7 MB, so the whole system is
// roughly 22 MB -- against a 326 MB triangle pool, which is the right sense of
// scale for something that replaces one exp().
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RtAccelerationStructure.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Sampler.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"

#include <cstdint>
#include <string>

// V6Camera and V6Sky, the same structs the shaders see -- one definition, so
// the two sides cannot disagree about the layout.
#include "../../shaders/Shared.slang"

namespace v7 {

// MUST MATCH the constants in shaders/VolFog.slang.
struct VolFogGrid {
    static constexpr uint32_t kX = 160;
    static constexpr uint32_t kY = 90;
    static constexpr uint32_t kZ = 64;
};

class VolFog {
  public:
    // ---- knobs, live from the settings menu ------------------------------
    bool enabled = true;

    // Where the grid begins and ends, in metres. The far limit is what the
    // exponential slice distribution is stretched across: too near and distant
    // haze simply stops, too far and every slice is wasted on air nobody can
    // resolve.
    float nearD = 0.5f;
    float farD = 400.0f;

    // Forward scattering. Water droplets and dust are strongly forward, which
    // is what makes the air near the sun glow instead of the whole volume
    // lifting uniformly. Zero would be isotropic and would look like milk.
    float anisotropy = 0.7f;

    // How much of the sky dome reaches a froxel. Not traced -- see the note in
    // the injection shader -- so this is the one honestly fudged number in the
    // system, and it is what stops shadowed air going black.
    //
    // BACK TO 0.25, AND THIS IS THE GLARE KNOB.
    //
    // It went to 0.6 to keep shadowed air from reading as a hole, and 0.6 turned
    // out to be what washes the frame out toward the sun. That reads as sun
    // glare and it is not: the term is isotropic and unshadowed, so it does not
    // know where the sun is at all. What it does is accumulate along the march,
    // so the further you can see the more of it piles up -- and the direction
    // you can see furthest in a wood is the one the light is coming from,
    // because that is where the canopy is thinnest. The glare follows the sun
    // without being caused by it.
    //
    // Measured over the frame, dropping it from 0.6 to 0.25 takes the median
    // from 46 to 30; the forward-scattering lobe, which was the obvious suspect,
    // moves it by nothing at all -- g = 0.7 to g = 0.0 changes the brightest
    // tenth from 176 to 187, the wrong way.
    float ambient = 0.25f;

    // How much of each new frame survives the temporal blend, standing still
    // and moving. The still number is the reference's and buys a very quiet
    // grid; the moving one is what stops the wood dragging its own shadows
    // behind it. See the long note in VolFogInject.
    float settleStill = 0.05f;
    float settleMoving = 0.40f;

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        try {
            inject_ = Falcor::ComputePass::create(device_, "v7/shaders/VolFogInject.cs.slang", "main");
            march_ = Falcor::ComputePass::create(device_, "v7/shaders/VolFogMarch.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("fog shaders did not compile: ") + e.what();
            return false;
        }

        using Falcor::ResourceBindFlags;
        const auto kRw = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;
        for (int i = 0; i < 2; ++i) {
            injectTex_[i] = device_->createTexture3D(VolFogGrid::kX, VolFogGrid::kY, VolFogGrid::kZ,
                                                     Falcor::ResourceFormat::RGBA16Float, 1,
                                                     nullptr, kRw);
            injectTex_[i]->setName("v7::fogInject");
        }
        marched_ = device_->createTexture3D(VolFogGrid::kX, VolFogGrid::kY, VolFogGrid::kZ,
                                            Falcor::ResourceFormat::RGBA16Float, 1, nullptr, kRw);
        marched_->setName("v7::fogMarched");

        // LINEAR AND CLAMPED. The tracer samples this grid at an arbitrary
        // depth between slices, so point sampling would put every slice
        // boundary on the screen as a visible step. Clamping matters at the
        // edges: a pixel just outside the grid should get the nearest air, not
        // wrap round to the far side of the frustum.
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear, Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Linear);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp);
        sampler_ = device_->createSampler(sd);

        ready_ = true;
        status_ = "froxel grid 160x90x64";
        return true;
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled; }
    const std::string &status() const { return status_; }
    const Falcor::ref<Falcor::Texture> &grid() const { return marched_; }
    const Falcor::ref<Falcor::Sampler> &sampler() const { return sampler_; }

    // A camera cut, a resize, a teleport: anything that makes last frame's grid
    // describe somewhere else. One frame of noise beats several seconds of the
    // previous location bleeding through.
    void invalidate() { warm_ = false; }

    // ---------------------------------------------------------------------
    // Light the grid, then integrate it. Run before the trace that samples it.
    // ---------------------------------------------------------------------
    void render(Falcor::RenderContext *ctx, const V6Camera &cam, const V6Camera &prevCam,
                const V6Sky &sky, float density, float height, uint32_t frame,
                Falcor::RtAccelerationStructure *tlas, bool moving) {
        if (!ready_ || !tlas) return;
        const uint32_t cur = cur_, prev = cur_ ^ 1u;

        {
            auto var = inject_->getRootVar();
            var["gScene"].setAccelerationStructure(
                Falcor::ref<Falcor::RtAccelerationStructure>(tlas));
            var["gFogGrid"] = injectTex_[cur];
            var["gFogHistory"] = injectTex_[prev];
            var["gFogSampler"] = sampler_;
            var["VolFogCB"]["gCam"].setBlob(&cam, sizeof(cam));
            var["VolFogCB"]["gPrevCam"].setBlob(&prevCam, sizeof(prevCam));
            var["VolFogCB"]["gSky"].setBlob(&sky, sizeof(sky));
            var["VolFogCB"]["gNear"] = nearD;
            var["VolFogCB"]["gFar"] = farD;
            var["VolFogCB"]["gDensity"] = density;
            var["VolFogCB"]["gHeight"] = height;
            var["VolFogCB"]["gAnisotropy"] = anisotropy;
            var["VolFogCB"]["gAmbient"] = ambient;
            var["VolFogCB"]["gFrame"] = frame;
            var["VolFogCB"]["gAccumulate"] = warm_ ? 1u : 0u;
            var["VolFogCB"]["gAlpha"] = moving ? settleMoving : settleStill;
            inject_->execute(ctx, VolFogGrid::kX, VolFogGrid::kY, VolFogGrid::kZ);
        }

        ctx->uavBarrier(injectTex_[cur].get());

        {
            auto var = march_->getRootVar();
            var["gFogIn"] = injectTex_[cur];
            var["gFogOut"] = marched_;
            var["VolFogMarchCB"]["gNear"] = nearD;
            var["VolFogMarchCB"]["gFar"] = farD;
            // One thread per COLUMN -- the walk down z is serial, see the pass.
            march_->execute(ctx, VolFogGrid::kX, VolFogGrid::kY, 1);
        }
        ctx->uavBarrier(marched_.get());

        cur_ ^= 1u;
        warm_ = true;
    }

  private:
    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> inject_, march_;
    Falcor::ref<Falcor::Texture> injectTex_[2], marched_;
    Falcor::ref<Falcor::Sampler> sampler_;
    uint32_t cur_ = 0;
    bool ready_ = false, warm_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
