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
// THE QUERY API A BACKEND WILL HAVE TO BRING BACK.
//
// Walking, collision and physics are not in this engine -- there is no ground
// to stand on, so the camera flies. When a real store arrives it will need to
// answer, on the HOST, questions the shader-side seam does not cover:
//
//     solidAt(i, y, j)      is this voxel filled
//     topAt(i, j)           the highest filled voxel in this column
//     groundM(i, j)         ...as a world height, for placing things
//     collidersNear(p, r)   what can be walked into near here
//
// That is the SECOND interface, and it is the one that gets forgotten: a plan
// that swaps only the tracer runs aground here, about halfway. It is written
// down now, while it costs nothing, rather than rediscovered later.
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

namespace tpl {

using Falcor::ref;
using Falcor::Buffer;
using Falcor::Device;
using Falcor::RenderContext;
using Falcor::ResourceBindFlags;
using Falcor::ShaderVar;

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
        if (!store.build(device, ctx)) return false;
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
        materials_->setName("tpl::materials");
    }

    ref<Device> device_;
    RenderContext *ctx_ = nullptr;
    ref<Buffer> materials_;
};

}  // namespace tpl
