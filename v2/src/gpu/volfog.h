// ---------------------------------------------------------------------------
// volfog.h -- the world-anchored fog volume: its cascades, its snapping, and
//             the one pass that fills them.
//
// The algorithm is in shaders/VolFog.slang and the pass is VolFogInject. This
// file owns the memory and the arithmetic that decides WHERE each cascade sits
// in the world, which is the part that makes the volume anchored rather than
// merely large.
//
// ---------------------------------------------------------------------------
// FOUR TEXTURES, TWO PER CASCADE.
//
// Each cascade is double-buffered because the pass reads last frame's copy as
// history while writing this frame's, and a single volume would have it reading
// values it was in the middle of overwriting. There is no third "marched"
// volume any more: a world grid has no axis to pre-integrate along, so the
// integration moved into the tracer. See the march note in Trace.cs.slang.
//
// At 128 x 48 x 128 in RGBA16F each volume is 6.3 MB, so the whole system is
// about 25 MB -- against 22 MB for the froxel grid it replaces, which is close
// enough to call a wash.
//
// ---------------------------------------------------------------------------
// SNAPPING, WHICH IS THE WHOLE TRICK.
//
// A cascade follows the player, or the far half of the wood would fall out of
// it. What it must NOT do is follow continuously: if the origin slid by a
// fraction of a cell, every cell would cover a slightly different parcel of air
// each frame, the temporal history would be averaging different questions, and
// the fog would swim exactly the way the froxel grid did.
//
// So the origin is quantised to a whole number of the cascade's own cells --
// one floor() per axis. Two consequences follow, and both are the point:
//
//   1. A cell is a FIXED region of the world for as long as it is in the
//      volume. The shadow ray is re-asked about the same air every frame, so
//      the blend is a real average instead of a smear.
//   2. Last frame's volume differs from this one by a WHOLE NUMBER OF CELLS,
//      so reprojection is an integer offset. No matrix, no perspective divide,
//      no landing between texels. A cell either has history or has walked off
//      the edge, and that is the only case there is.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/RtAccelerationStructure.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Sampler.h"
#include "Core/API/Texture.h"
#include "Core/Pass/ComputePass.h"

#include <cmath>
#include <cstdint>
#include <string>

// V6Camera and V6Sky, the same structs the shaders see -- one definition, so
// the two sides cannot disagree about the layout.
#include "../../shaders/Shared.slang"

namespace v2 {

// MUST MATCH the constants in shaders/VolFog.slang.
struct VolFogGrid {
    static constexpr uint32_t kX = 128;
    static constexpr uint32_t kY = 48;
    static constexpr uint32_t kZ = 128;
    static constexpr int kCascades = 2;
};

class VolFog {
  public:
    // ---- knobs, live from the settings menu ------------------------------
    bool enabled = true;

    // How far the tracer marches, in metres. Not a property of the volume any
    // more -- the far cascade reaches 512 m whatever this says -- but of how
    // much of it is worth integrating per pixel.
    float farD = 400.0f;

    // CELL SIZES, AND THE REASON THERE ARE TWO.
    //
    // A single uniform volume big enough for the view distance would be far too
    // coarse near the camera, which is precisely the property the froxel grid
    // had for free and this had to find another way to keep. Two cascades: one
    // fine and close, one coarse and far.
    //
    //   L0  0.75 m cells -> 96 x 36 x 96 m around the player
    //   L1  4.00 m cells -> 512 x 192 x 512 m
    float cell0 = 0.75f;
    float cell1 = 4.00f;

    // Forward scattering. Water droplets and dust really are strongly forward,
    // and that is what puts a glow in the air near the sun instead of lifting
    // the whole volume uniformly.
    //
    // WALKED DOWN THREE TIMES, 0.7 -> 0.5 -> 0.30 -> 0, always in the same
    // direction. The tight lobe this opened with put a hard bright core around
    // the sun and left the rest of the wood flat; every step away from it
    // spread the same light further into the trees, and every step was an
    // improvement. It is now off.
    //
    // ZERO IS ISOTROPIC, not "no fog". The volume still scatters and still
    // occludes -- fogDensity is the knob that turns it off -- it just scatters
    // the same in every direction, so the haze reads as air rather than as a
    // lamp pointed at the camera. The old note here warned that zero would
    // look like milk; three walks down the slider say otherwise, and the
    // slider is still there for anyone who wants the lobe back.
    float anisotropy = 0.0f;

    // How much of the sky dome reaches a cell that can see it. Applied at march
    // time now, against the visibility the volume stores.
    //
    // FULL, which is the top of the slider. A quarter was a conservative
    // opening value chosen before the visibility term existed to hold it back;
    // now that a cell only gets this where it can actually see the dome, the
    // honest coefficient is the whole of it, and anything less is the sky being
    // dimmed twice.
    float ambient = 1.00f;

    // How much sky light still reaches a cell the up ray found covered.
    //
    // NOT ZERO, AND NOT A QUARTER EITHER. One ray straight up is a proxy for a
    // hemisphere and the paths it misses are real. Measured on the wood: at
    // 0.25 the sky came out DARKER than the ground under it, which is fog
    // absorbing sky light without scattering any back. 0.60 keeps the sky above
    // the ground where it belongs and still takes the glare off. 1.0 is the old
    // unshadowed term, sun blur and all.
    float skyShadow = 0.60f;

    // How much of each new frame survives the temporal blend, standing still
    // and moving.
    //
    // THE MOVING NUMBER CAN BE FAR LOWER THAN THE FROXEL VERSION'S 0.40, and
    // that is a direct dividend of anchoring. That number was high because a
    // moving camera invalidated the history: a froxel that was shadowed and was
    // now in the open held a value that was simply WRONG rather than misplaced,
    // and no reprojection makes a stale value fresh. Here the camera does not
    // enter into it. A cell is the same air whether you walk or stand, so the
    // history only goes stale when the WORLD changes.
    float settleStill = 0.05f;
    float settleMoving = 0.10f;

    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        try {
            inject_ =
                Falcor::ComputePass::create(device_, "v2/shaders/VolFogInject.cs.slang", "main");
        } catch (const std::exception &e) {
            status_ = std::string("fog shaders did not compile: ") + e.what();
            return false;
        }

        using Falcor::ResourceBindFlags;
        const auto kRw = ResourceBindFlags::ShaderResource | ResourceBindFlags::UnorderedAccess;
        for (int c = 0; c < VolFogGrid::kCascades; ++c) {
            for (int i = 0; i < 2; ++i) {
                vol_[c][i] = device_->createTexture3D(VolFogGrid::kX, VolFogGrid::kY,
                                                      VolFogGrid::kZ,
                                                      Falcor::ResourceFormat::RGBA16Float, 1,
                                                      nullptr, kRw);
                vol_[c][i]->setName("v2::fogVolume");
            }
        }

        // LINEAR AND CLAMPED. The tracer samples this at arbitrary world
        // positions between cell centres, so point sampling would show the
        // lattice directly. Clamping matters at the edges: the march tests the
        // bounds itself, and a wrapped tap would fetch the far side of the wood.
        Falcor::Sampler::Desc sd;
        sd.setFilterMode(Falcor::TextureFilteringMode::Linear, Falcor::TextureFilteringMode::Linear,
                         Falcor::TextureFilteringMode::Linear);
        sd.setAddressingMode(Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp,
                             Falcor::TextureAddressingMode::Clamp);
        sampler_ = device_->createSampler(sd);

        ready_ = true;
        status_ = "world volume 128x48x128, 2 cascades";
        return true;
    }

    bool available() const { return ready_; }
    bool active() const { return ready_ && enabled; }
    const std::string &status() const { return status_; }
    const Falcor::ref<Falcor::Sampler> &sampler() const { return sampler_; }

    // What the tracer binds and reads. cur_ has already flipped by the time the
    // trace runs, so the volume written this frame is the one at cur_ ^ 1.
    const Falcor::ref<Falcor::Texture> &volume(int c) const { return vol_[c][cur_ ^ 1u]; }
    float3 originWs(int c) const { return originWs_[c]; }
    float cellSize(int c) const { return c == 0 ? cell0 : cell1; }

    // A teleport, or anything else that makes the history describe somewhere
    // else. One frame of noise beats several seconds of bleed-through.
    void invalidate() { warm_ = false; }

    // ---------------------------------------------------------------------
    // Fill both cascades. Run before the trace that marches them.
    // ---------------------------------------------------------------------
    void render(Falcor::RenderContext *ctx, const V6Camera &cam, const V6Sky &sky, float density,
                float height, uint32_t frame, Falcor::RtAccelerationStructure *tlas, bool moving) {
        if (!ready_ || !tlas) return;
        const uint32_t cur = cur_, prev = cur_ ^ 1u;

        for (int c = 0; c < VolFogGrid::kCascades; ++c) {
            const float cell = cellSize(c);

            // The snap. Centre the volume on the camera, then quantise the min
            // corner to a whole number of cells.
            const int32_t ox = snapCell(cam.pos.x, cell) - int32_t(VolFogGrid::kX) / 2;
            const int32_t oy = snapCell(cam.pos.y, cell) - int32_t(VolFogGrid::kY) / 2;
            const int32_t oz = snapCell(cam.pos.z, cell) - int32_t(VolFogGrid::kZ) / 2;

            // This frame's cell c holds world cell (origin + c); last frame it
            // sat at (origin + c) - prevOrigin. One integer add in the shader.
            const int32_t shiftX = ox - prevCell_[c][0];
            const int32_t shiftY = oy - prevCell_[c][1];
            const int32_t shiftZ = oz - prevCell_[c][2];

            originWs_[c] = float3(float(ox) * cell, float(oy) * cell, float(oz) * cell);

            auto var = inject_->getRootVar();
            var["gScene"].setAccelerationStructure(
                Falcor::ref<Falcor::RtAccelerationStructure>(tlas));
            var["gFogVol"].setTexture(vol_[c][cur]);
            var["gFogHistory"].setTexture(vol_[c][prev]);
            var["VolFogCB"]["gOriginWs"] = originWs_[c];
            var["VolFogCB"]["gDensity"] = density;
            var["VolFogCB"]["gCellSize"] = float3(cell, cell, cell);
            var["VolFogCB"]["gHeight"] = height;
            var["VolFogCB"]["gSunDir"] = sky.sunDir;
            var["VolFogCB"]["gSkyShadow"] = skyShadow;
            var["VolFogCB"]["gHistShift"] = int3(shiftX, shiftY, shiftZ);
            var["VolFogCB"]["gOriginCell"] = int3(ox, oy, oz);
            var["VolFogCB"]["gFrame"] = frame;
            var["VolFogCB"]["gAccumulate"] = warm_ ? 1u : 0u;
            var["VolFogCB"]["gAlpha"] = moving ? settleMoving : settleStill;
            inject_->execute(ctx, VolFogGrid::kX, VolFogGrid::kY, VolFogGrid::kZ);

            ctx->uavBarrier(vol_[c][cur].get());

            prevCell_[c][0] = ox;
            prevCell_[c][1] = oy;
            prevCell_[c][2] = oz;
        }

        cur_ ^= 1u;
        warm_ = true;
    }

  private:
    // floor(v / cell) as an integer, which is what keeps the lattice stable
    // across the origin. Truncation would fold -0.4 and +0.4 into the same cell
    // and put a seam through the middle of the world.
    static int32_t snapCell(float v, float cell) { return int32_t(std::floor(v / cell)); }

    Falcor::ref<Falcor::Device> device_;
    Falcor::ref<Falcor::ComputePass> inject_;
    Falcor::ref<Falcor::Texture> vol_[VolFogGrid::kCascades][2];
    Falcor::ref<Falcor::Sampler> sampler_;
    float3 originWs_[VolFogGrid::kCascades] = {};
    int32_t prevCell_[VolFogGrid::kCascades][3] = {};
    uint32_t cur_ = 0;
    bool ready_ = false, warm_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v2
