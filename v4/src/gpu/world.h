// ---------------------------------------------------------------------------
// world.h -- what the renderer is given instead of a world.
//
// Three things, and only one of them is geometry:
//
//     scene/sky.h        the sun, the moon and the atmosphere's inputs
//     scene/materials.h  the palette -- NOT geometry, see below
//     gpu/store.h        the voxels, or in this engine, the absence of them
//
// In the engine this was cut from, this file was a facade over a live OpenVDB
// tree and its NanoVDB image on the device, and it carried the query API that
// walking, digging, physics and debris all went through -- solidAt, topAt,
// groundM, collidersNear, dig, place. None of that is here. This engine has no
// world to query, by design: it is the lighting stack with a hole where the
// geometry goes, so that different ways of filling that hole can be tried
// against an identical renderer and compared honestly.
//
// ---------------------------------------------------------------------------
// WHY THE PALETTE STAYED WHEN THE VOXELS WENT.
//
// A material id is not geometry. Every backend that will ever plug in here
// resolves a hit to a byte and indexes the same table, and the shade that comes
// out has to be identical across all of them or an A/B between two stores is
// comparing two lighting setups. So the palette lives on this side of the seam
// and a store returns nothing but the id. This engine has no material textures
// at all -- surface variation is that palette lookup plus a hash of the voxel
// coordinate -- which is also why VoxelHit has to carry an integer ijk and not
// merely the point where the ray landed.
//
// ---------------------------------------------------------------------------
// THE QUERY API, WHICH IS THE HALF THAT GETS FORGOTTEN.
//
// A shader-side store answers rays. It does not answer the questions the HOST
// asks -- is this voxel filled, how high is the ground here, what can be walked
// into near this point -- and a plan that swaps only the tracer runs aground on
// that about halfway through. The template this engine was cut from wrote the
// list down while it cost nothing:
//
//     solidAt(i, y, j)      is this voxel filled
//     topAt(i, j)           the highest filled voxel in this column
//     groundM(i, j)         ...as a world height, for placing things
//     collidersNear(p, r)   what can be walked into near here
//
// The AABB store answers the first three, out of the same BrickWorld the
// acceleration structures are packed from -- so a host query and a ray can
// never disagree about where the ground is. They are forwarded below.
//
// collidersNear IS STILL MISSING, because nothing walks: the camera flies. It
// is the one this engine has not had to answer, not one it cannot.
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <vector>

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/Program/ShaderVar.h"

#include "../../shaders/Shared.slang"
#include "../core/vecmath.h"
#include "../scene/materials.h"
#include "../scene/sky.h"
#include "store.h"

namespace v4 {

using Falcor::ref;
using Falcor::Buffer;
using Falcor::Device;
using Falcor::RenderContext;
using Falcor::ResourceBindFlags;
using Falcor::ShaderVar;

// ---------------------------------------------------------------------------
// THE HOST AND THE DEVICE HAVE TO AGREE ABOUT THE PALETTE, and until now only a
// comment said so.
//
// Shared.slang carries kGrass0, kSoil0, kStone0 and the rest because it has to
// compile as Slang and cannot include a C++ header; scene/materials.h carries
// the same numbers for the generator. A silent disagreement paints the floor in
// tree bark -- or, once bedrock exists, paints the bottom of the world in
// whatever a model happened to mint at that id. These make it a build error.
// ---------------------------------------------------------------------------
static_assert(kGrass0 == mat::GRASS_0 && kGrassCount == mat::GRASS_COUNT, "grass ramp");
static_assert(kSoil0 == mat::SOIL_0 && kSoilCount == mat::SOIL_COUNT, "soil ramp");
static_assert(kLitter0 == mat::LITTER_0 && kLitterCount == mat::LITTER_COUNT, "litter ramp");
static_assert(kStone0 == mat::STONE_0 && kStoneCount == mat::STONE_COUNT, "stone ramp");
static_assert(kBedrock0 == mat::BEDROCK_0 && kBedrockCount == mat::BEDROCK_COUNT, "bedrock ramp");
static_assert(kRock == mat::ROCK && kBedrock == mat::BEDROCK && kWater == mat::WATER, "ids");
// And the ramps must not run into the ids the models are handed.
static_assert(mat::BEDROCK_0 + mat::BEDROCK_COUNT <= mat::TREE_BASE, "bedrock ramp overlaps models");

class World {
  public:
    Sky sky;
    Palette palette;
    Store store;

    // -----------------------------------------------------------------------
    // LIFECYCLE
    // -----------------------------------------------------------------------
    bool build(const ref<Device> &device, RenderContext *ctx) {
        device_ = device;
        ctx_ = ctx;
        // THE STORE MINTS ITS COLOURS FIRST AND THE TABLE IS UPLOADED SECOND,
        // and the order is load-bearing. A store authors its own content, so
        // the material ids a tree or a hillside wears do not exist until it has
        // run -- upload before that and the palette on the device is the
        // defaults, with every tree grey. See the note on build() in store.h.
        if (!store.build(device, ctx, palette)) return false;
        uploadMaterials();
        return true;
    }

    void shutdown() { store.shutdown(); }

    // Once a frame, before the trace. A paged backend re-centres here.
    void update(Vec3 camPos) { store.update(camPos); }

    // -----------------------------------------------------------------------
    // THE DEVICE SIDE -- two calls, and they are the whole seam
    // -----------------------------------------------------------------------

    // The palette. Bound by the renderer for every pass, because the shade of a
    // surface is the renderer's business and not a backend's.
    const ref<Buffer> &materialBuffer() const { return materials_; }

    // EVERYTHING a backend needs, on one root var -- buffers, textures,
    // acceleration structures, constants. See the note in gpu/store.h about why
    // the constants come through here rather than being assembled by the
    // caller: it is what keeps a new backend from having to edit the tracer.
    void bindStore(const ShaderVar &var) const { store.bind(var); }

    // Which stores/*.slang the trace programs compile against, plus whatever
    // options that backend wants. Folded into the tracer's define list at
    // program-creation time.
    void storeDefines(DefineList &defs) const { store.addDefines(defs); }

    // -----------------------------------------------------------------------
    // DIAGNOSTICS -- and nothing in the render path may branch on these
    // -----------------------------------------------------------------------
    bool storeReady() const { return store.ready(); }
    const char *storeStatus() const { return store.status(); }
    const char *storeModels() const { return store.models(); }

    // -----------------------------------------------------------------------
    // THE HOST QUERY, forwarded from the store. See the note at the top.
    //
    // The world height at a point, for settling a spawn or standing something
    // on the ground. Zero where the store holds nothing, which is the same
    // answer the null store gives and the same answer an empty world gives --
    // and, deliberately, not an error: a caller that has to branch on "is there
    // a world yet" is a caller that will be wrong once there is.
    // -----------------------------------------------------------------------
    float groundM(float x, float z) const { return store.groundM(x, z); }

  private:
    // The palette is a fixed table whose fields line up one for one with the
    // shader's material record, but they are separate structs on purpose:
    // MaterialLook belongs to the world and V6Material to the pipeline, and the
    // day one of them gains a field the other does not need, this loop is the
    // only thing that has to know.
    void uploadMaterials() {
        std::vector<V6Material> mats(palette.table().size());
        for (size_t i = 0; i < mats.size(); ++i) {
            const MaterialLook &m = palette.table()[i];
            mats[i].albedo = float3(m.albedo.x, m.albedo.y, m.albedo.z);
            mats[i].roughness = m.roughness;
            mats[i].specular = m.specular;
            mats[i].translucency = m.translucency;
            mats[i].pad0 = mats[i].pad1 = 0.0f;
        }
        materials_ = device_->createStructuredBuffer(sizeof(V6Material), uint32_t(mats.size()),
                                                     ResourceBindFlags::ShaderResource,
                                                     Falcor::MemoryType::DeviceLocal, mats.data());
        materials_->setName("v4::materials");
    }

    ref<Device> device_;
    RenderContext *ctx_ = nullptr;
    ref<Buffer> materials_;
};

}  // namespace v4
