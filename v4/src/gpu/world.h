// ---------------------------------------------------------------------------
// world.h -- the endless wood, as DXR acceleration structures.
//
// THE DESIGN QUESTION IS STILL INSTANCING. Nine pine models meshed to their
// exposed faces come to millions of triangles between them; twenty-six rocks
// and six flowers more. A stand flattened would be unbuildable, and every tree
// would still be one of the nine shapes actually authored. One bottom-level
// structure per model, referred to through a top-level instance, keeps the
// memory at one copy of each while the scene draws thousands.
//
// The terrain is instanced the same way: not one structure built at startup but
// a ring of chunk structures that follows the camera. See scene/chunks.h for
// why that is affordable; this file owns the GPU half.
//
// WHAT THE MOVE OFF OPTIX ACTUALLY CHANGED, in one place:
//
//   THE SHADER BINDING TABLE IS GONE, AND WITH IT THE SLOT ALLOCATOR. Under
//   OptiX every resident chunk had to own a hit-group record so its closest-hit
//   program could find that chunk's triangle attributes. Records are addressed
//   by index, so residency became a slot-allocation problem: a fixed pool of
//   1024 records, a free list, a hand-written publish step whenever a chunk took
//   or released a slot, and a rule that the pool must be at least as large as
//   the ring or chunks silently stop drawing.
//
//   Inline ray tracing has no table to bind, so the question "which triangle
//   array does this instance read" is answered by a NUMBER THE INSTANCE
//   CARRIES. Every instance already needed a record in a structured buffer for
//   its tint; that record now also holds an offset into one shared pool of
//   packed triangle attributes. The slot pool, the free list, the publish
//   callback and the fixed ceiling all deleted, and the thing that replaced
//   them is an integer that is written when the instance array is built and
//   never touched again.
//
//   THE INSTANCE ARRAY IS ALSO REBUILT WHOLE rather than appended to. v2 pushed
//   one entry per tree onto a vector that was never trimmed, so a long session
//   walking through the wood grew it without bound -- a slow leak that nothing
//   would ever surface, because it is only a few bytes per tree. Here the array
//   is assembled from the resident chunks each time the top-level structure is
//   rebuilt, which is exactly when it can change, so an evicted chunk's entries
//   go with it.
//
// ROTATION IS IN QUARTER TURNS ONLY, for everything. An arbitrary yaw would put
// a voxel model off the lattice its own faces are aligned to, and the crisp
// axis-aligned silhouette that makes a voxel tree look like a voxel tree would
// turn into stair-stepped mush. It also keeps every transform orthonormal,
// which is what lets the shader transform a normal with the plain 3x3 instead
// of an inverse transpose.
//
// EVERY BOTTOM-LEVEL STRUCTURE IS COMPACTED. The driver builds into a
// conservatively sized buffer and reports afterwards how much it actually
// needed; for millions of small coplanar quads the compacted structure is
// routinely half the size. With hundreds of chunks resident that is the
// difference between fitting in 12 GB and not.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/RtAccelerationStructure.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../shaders/Shared.slang"
#include "../core/noise.h"
#include "../scene/chunks.h"
#include "../scene/collide.h"
#include "../scene/sky.h"
#include "../scene/vox.h"
#include "../scene/voxelworld.h"

// The shader carries its own copies of these two, because Shared.slang has to
// compile as Slang and cannot include a C++ header. Neither has ever changed,
// but a silent disagreement about the voxel size would move every model
// relative to the ground it stands on, so it is checked rather than trusted.
static_assert(v4::VOXEL_M == v4::kVoxelM, "voxel size disagrees with the shader");
static_assert(v4::SUN_COS_THETA_MAX == v4::kSunCosThetaMax, "sun size disagrees with the shader");

namespace v4 {

using Falcor::Buffer;
using Falcor::Device;
using Falcor::RenderContext;
using Falcor::ref;
using Falcor::ResourceBindFlags;
using Falcor::RtAccelerationStructure;
using Falcor::RtAccelerationStructureBuildFlags;
using Falcor::RtAccelerationStructureBuildInputs;
using Falcor::RtAccelerationStructureKind;
using Falcor::RtAccelerationStructurePostBuildInfoDesc;
using Falcor::RtAccelerationStructurePostBuildInfoPool;
using Falcor::RtAccelerationStructurePostBuildInfoQueryType;
using Falcor::RtGeometryDesc;
using Falcor::RtGeometryFlags;
using Falcor::RtGeometryInstanceFlags;
using Falcor::RtGeometryType;
using Falcor::RtInstanceDesc;

// ---------------------------------------------------------------------------
// The packed-triangle pool.
//
// Every instance in the scene reads its per-triangle (material, face) uint16
// out of ONE buffer, at an offset the instance carries. That is the whole of
// what replaced the shader binding table, and it needs an allocator because
// chunks come and go at different sizes.
//
// First fit with coalescing, over a std::map keyed by offset. Deliberately the
// dumbest thing that works: allocations happen a couple of times a frame at
// most, the map never holds more than a few hundred entries, and a fancier
// allocator would be untestable code sitting under a renderer.
//
// UNITS ARE UINT16, NOT BYTES, throughout -- an offset here is added directly
// to a primitive index in the shader.
// ---------------------------------------------------------------------------
class TriPool {
  public:
    static constexpr uint32_t kInvalid = 0xFFFFFFFFu;

    void init(const ref<Device> &device, size_t units) {
        device_ = device;
        units_ = units;
        buffer_ = device_->createBuffer(units_ * sizeof(uint16_t), ResourceBindFlags::ShaderResource,
                                        Falcor::MemoryType::DeviceLocal);
        buffer_->setName("v4::TriPool");
        free_.clear();
        free_[0] = units_;
    }

    const ref<Buffer> &buffer() const { return buffer_; }
    size_t capacityUnits() const { return units_; }
    size_t usedUnits() const {
        size_t f = 0;
        for (const auto &kv : free_) f += kv.second;
        return units_ - f;
    }

    // Reserve `count` uint16s and copy `data` into them. Grows the pool if it
    // has to, which costs a device-side copy of everything already in it -- so
    // the initial size is chosen from the ring radius to make that rare rather
    // than to make it impossible.
    uint32_t upload(RenderContext *ctx, const std::vector<uint16_t> &data) {
        if (data.empty()) return kInvalid;
        uint32_t off = take(data.size());
        if (off == kInvalid) {
            grow(ctx, data.size());
            off = take(data.size());
            if (off == kInvalid) return kInvalid;  // asked for more than the pool can ever hold
        }
        ctx->updateBuffer(buffer_.get(), data.data(), size_t(off) * sizeof(uint16_t),
                          data.size() * sizeof(uint16_t));
        return off;
    }

    void release(uint32_t offset, size_t count) {
        if (offset == kInvalid || count == 0) return;
        auto it = free_.emplace(offset, count).first;

        // Coalesce with the block after, then the one before. Without this a
        // few hours of walking fragments the pool into unusable slivers even
        // though the total free space never falls.
        auto next = std::next(it);
        if (next != free_.end() && it->first + it->second == next->first) {
            it->second += next->second;
            free_.erase(next);
        }
        if (it != free_.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second == it->first) {
                prev->second += it->second;
                free_.erase(it);
            }
        }
    }

  private:
    ref<Device> device_;
    ref<Buffer> buffer_;
    size_t units_ = 0;
    std::map<uint32_t, size_t> free_;  // offset -> length, in uint16 units

    uint32_t take(size_t count) {
        for (auto it = free_.begin(); it != free_.end(); ++it) {
            if (it->second < count) continue;
            const uint32_t off = it->first;
            const size_t rest = it->second - count;
            free_.erase(it);
            if (rest > 0) free_[uint32_t(off + count)] = rest;
            return off;
        }
        return kInvalid;
    }

    void grow(RenderContext *ctx, size_t need) {
        const size_t want = std::max(units_ * 2, units_ + need * 2);
        ref<Buffer> nb = device_->createBuffer(want * sizeof(uint16_t),
                                              ResourceBindFlags::ShaderResource,
                                              Falcor::MemoryType::DeviceLocal);
        nb->setName("v4::TriPool");
        ctx->copyBufferRegion(nb.get(), 0, buffer_.get(), 0, units_ * sizeof(uint16_t));
        ctx->submit(true);  // the old buffer dies with this scope; it must be done being read
        release(uint32_t(units_), want - units_);
        buffer_ = nb;
        units_ = want;
        std::printf("v4: triangle pool grown to %.0f MB\n",
                    double(units_ * sizeof(uint16_t)) / (1024.0 * 1024.0));
    }
};

// A bottom-level structure and the buffer under it.
struct Blas {
    ref<Buffer> buffer;
    ref<RtAccelerationStructure> as;
    bool valid() const { return as != nullptr; }
};

// One instanced model: a pine, a rock, a flower. They differ only in what
// scatters them and how deep it sinks them.
struct ModelTemplate {
    Blas blas;
    uint32_t triOffset = TriPool::kInvalid;
    size_t tris = 0;
    int sx = 0, sy = 0, sz = 0;
    // What a body runs into, measured off this model's own voxels at load time.
    // See scene/collide.h for why it is measured rather than chosen.
    ModelCollider col;
};

// A chunk that is resident on the GPU. Its mesh is already in world space, so
// its own instance is an identity transform that exists only to carry the
// triangle offset; the decor it holds is placed.
struct Chunk {
    int cx = 0, cz = 0;
    Blas blas;
    uint32_t triOffset = TriPool::kInvalid;
    size_t tris = 0;
    std::vector<RtInstanceDesc> decorDesc;
    std::vector<V4Instance> decorInfo;
    // The trees and rocks of this chunk as things to walk into. Held per chunk
    // rather than in one world-wide list so eviction is free: the colliders go
    // when the chunk does, and nothing has to be searched to remove them.
    std::vector<Solid> solids;
};

inline long long chunkKey(int cx, int cz) {
    // Packed so a std::map can order them; 21 bits each side is +/- a million
    // chunks, which at 25.6 m is a world about 50 000 km across.
    return (static_cast<long long>(cx) << 21) ^ static_cast<long long>(cz);
}

// ---------------------------------------------------------------------------
class World {
  public:
    VoxelTerrain terrain;
    Sky sky;
    Palette palette;

    std::string pineDir = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decorDir = "C:/voxelbit/game/assets/decoration";
    int viewChunks = 12;  // ring radius, in chunks
    float treeDensity = 0.31f;
    float rockDensity = 0.010f;
    float flowerDensity = 0.45f;
    uint32_t seed = 20260904u;
    int meshThreads = 0;

    int loadedPines = 0, loadedRocks = 0, loadedFlowers = 0;
    size_t uniqueTris = 0;

    // A ref rather than a raw pointer: ShaderVar::setAccelerationStructure
    // takes one, and it is the only thing the tracer needs from here.
    const ref<RtAccelerationStructure> &tlasRef() const { return tlas_; }
    const ref<Buffer> &triPool() const { return pool_.buffer(); }
    const ref<Buffer> &instanceBuffer() const { return instanceInfo_; }
    const ref<Buffer> &materialBuffer() const { return materials_; }

    size_t chunkCount() const { return chunks_.size(); }
    size_t instanceCount() const { return instanceDescs_.size(); }
    size_t residentTris() const { return residentTris_; }
    size_t pendingChunks() { return mesher_.inFlight(); }
    double buildMs() const { return buildMs_; }
    size_t poolBytes() const { return pool_.capacityUnits() * sizeof(uint16_t); }

    // -----------------------------------------------------------------------
    bool build(const ref<Device> &device, RenderContext *ctx) {
        device_ = device;
        ctx_ = ctx;

        // The pool has to hold the resident ring plus the models. A chunk over
        // this terrain meshes to roughly 200k triangles; the budget below is
        // that with a margin, and the pool grows if the ground turns out to be
        // rougher than that. Models are a rounding error beside it.
        const size_t ring = size_t(2 * std::max(1, viewChunks) + 1);
        pool_.init(device_, ring * ring * 260000 + (8u << 20));

        // Templates first: the terrain's grass and soil colours are sampled
        // from the palette the models bring with them.
        if (!loadPines()) return false;
        loadRocks();
        loadFlowers();
        palette.deriveGroundFromTrees();
        buildWater();

        // The palette is a fixed 255 entries whose fields line up one for one
        // with the shader's material record, but they are separate structs on
        // purpose: MaterialLook belongs to the world builder and V4Material to
        // the pipeline, and the day one of them gains a field the other does
        // not need, this loop is the only thing that has to know.
        std::vector<V4Material> mats(palette.table().size());
        for (size_t i = 0; i < mats.size(); ++i) {
            const MaterialLook &m = palette.table()[i];
            mats[i].albedo = float3(m.albedo.x, m.albedo.y, m.albedo.z);
            mats[i].roughness = m.roughness;
            mats[i].specular = m.specular;
            mats[i].translucency = m.translucency;
            mats[i].pad0 = mats[i].pad1 = 0.0f;
        }
        materials_ = device_->createStructuredBuffer(sizeof(V4Material), uint32_t(mats.size()),
                                                     ResourceBindFlags::ShaderResource,
                                                     Falcor::MemoryType::DeviceLocal, mats.data());
        materials_->setName("v4::materials");

        mesher_.seed = seed;
        mesher_.treeDensity = treeDensity;
        mesher_.rockDensity = rockDensity;
        mesher_.flowerDensity = flowerDensity;
        for (const ModelTemplate &t : pines_) mesher_.pineFoot.push_back({t.sx, t.sz, t.sy});
        for (const ModelTemplate &t : rocks_) mesher_.rockFoot.push_back({t.sx, t.sz, t.sy});
        for (const ModelTemplate &t : flowers_) mesher_.flowerFoot.push_back({t.sx, t.sz, t.sy});

        const int hw = int(std::thread::hardware_concurrency());
        mesher_.start(terrain, meshThreads > 0 ? meshThreads : maxi(2, hw - 2));
        return true;
    }

    // -----------------------------------------------------------------------
    // Bring the resident ring in line with the camera, then take delivery of
    // however many finished chunks the budget allows.
    //
    // The budget is on STRUCTURE BUILDS, not on meshes taken: the build is the
    // part recorded on this thread and therefore the part that can stall a
    // frame. Returns true if the set changed and the launch needs a new
    // top-level handle.
    // -----------------------------------------------------------------------
    bool update(Vec3 camPos, int buildBudget = 2, bool rebuildTop = true) {
        const int cx = floorDiv(int(floorf(camPos.x / VOXEL_M)), CHUNK_VOX);
        const int cz = floorDiv(int(floorf(camPos.z / VOXEL_M)), CHUNK_VOX);

        bool changed = false;
        if (!primed_ || cx != lastCx_ || cz != lastCz_) {
            lastCx_ = cx;
            lastCz_ = cz;
            primed_ = true;
            changed |= rering(cx, cz);
        }

        // Collect a batch first, then build them together. See adoptMany.
        std::vector<ChunkBuild> batch;
        ChunkBuild b;
        while (int(batch.size()) < buildBudget && mesher_.take(&b)) {
            requested_.erase(chunkKey(b.cx, b.cz));
            if (wanted_.count(chunkKey(b.cx, b.cz)) == 0) continue;  // evicted while queued
            batch.push_back(std::move(b));
        }
        if (!batch.empty()) {
            adoptMany(std::move(batch));
            changed = true;
        }

        // Deferred while priming: the top-level structure is rebuilt WHOLE
        // every time it changes, so doing it once per batch of adopted chunks
        // meant hundreds of full rebuilds to reach the same state one rebuild
        // at the end would have produced.
        if (changed && rebuildTop) rebuildTlas();
        return changed;
    }

    // -----------------------------------------------------------------------
    // Everything solid within `reach` metres of a point.
    //
    // Gathered into a list once a tick rather than answered per test: the
    // player asks six times a tick, a chunk holds about forty solids, and at
    // that size any structure worth building costs more than the scan it saves.
    //
    // The CHUNKS looked at reach further than `reach` does, because a solid
    // belongs to the chunk holding its centre and the big rocks are four metres
    // across -- one centred just over the border can still be underfoot.
    // -----------------------------------------------------------------------
    void collidersNear(Vec3 p, float reach, std::vector<Solid> *out) const {
        out->clear();
        const float span = reach + 8.0f;
        const int x0 = floorDiv(int(floorf((p.x - span) / VOXEL_M)), CHUNK_VOX);
        const int x1 = floorDiv(int(floorf((p.x + span) / VOXEL_M)), CHUNK_VOX);
        const int z0 = floorDiv(int(floorf((p.z - span) / VOXEL_M)), CHUNK_VOX);
        const int z1 = floorDiv(int(floorf((p.z + span) / VOXEL_M)), CHUNK_VOX);
        for (int cz = z0; cz <= z1; ++cz)
            for (int cx = x0; cx <= x1; ++cx) {
                auto it = chunks_.find(chunkKey(cx, cz));
                if (it == chunks_.end()) continue;
                for (const Solid &s : it->second.solids)
                    if (fabsf(s.cx - p.x) < reach + s.hx && fabsf(s.cz - p.z) < reach + s.hz)
                        out->push_back(s);
            }
    }

    // Block until the ring around the camera is fully resident. Used once at
    // startup so the first frame is not a hole in the ground.
    void primeBlocking(Vec3 camPos) {
        update(camPos, 0, false);
        while (mesher_.inFlight() > 0 || !requested_.empty()) {
            // No cap worth the name here and no top-level rebuild: nothing is
            // being displayed yet, so the only thing that matters is draining
            // the queue as fast as the workers can fill it. 48 at a time keeps
            // the uncompacted structures held simultaneously well inside video
            // memory.
            if (!update(camPos, 48, false))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rebuildTlas();
    }

  private:
    ref<Device> device_;
    RenderContext *ctx_ = nullptr;
    ChunkMesher mesher_;
    TriPool pool_;

    std::vector<ModelTemplate> pines_, rocks_, flowers_;
    Blas waterBlas_;
    uint32_t waterTriOffset_ = TriPool::kInvalid;

    std::map<long long, Chunk> chunks_;
    std::map<long long, int> wanted_;
    std::set<long long> requested_;

    ref<Buffer> materials_, instanceInfo_, instanceDescBuf_, tlasBuffer_, tlasScratch_;
    ref<RtAccelerationStructure> tlas_;
    std::vector<RtInstanceDesc> instanceDescs_;
    std::vector<V4Instance> instanceInfos_;

    size_t residentTris_ = 0;
    double buildMs_ = 0.0;
    int lastCx_ = 0, lastCz_ = 0;
    bool primed_ = false;

    // ------------------------------------------------------------ structures
    // Build one bottom-level structure from a mesh, compacted.
    //
    // Used for the models and the water at load time, where a synchronous build
    // apiece costs nothing. Chunks go through adoptMany instead, which does the
    // same work for a whole batch behind two flushes rather than two per chunk.
    Blas buildBlas(const VoxMesh &m) {
        std::vector<VoxMesh> one;
        one.push_back(m);  // one-element batch, so there is one build path only
        std::vector<Blas> out = buildBlasBatch(one);
        return std::move(out[0]);
    }

    // -----------------------------------------------------------------------
    // Build a batch of structures with TWO device flushes, not two per mesh.
    //
    // Building one structure is: record the build, wait, read back how much the
    // compacted form needs, record the compact, wait. Done per chunk that is
    // two round trips each, and at a radius-12 ring -- six hundred and
    // twenty-five chunks -- it is twelve hundred and fifty of them. The waits
    // are not free even when the GPU has nothing left to do: each one is a
    // submit plus a driver round trip, and they serialise the CPU against the
    // device for the whole prime.
    //
    // Batched, every build is recorded back to back, ONE wait covers all of
    // them, all the sizes are read at once, every compact is recorded, and one
    // more wait finishes. The cost is holding every uncompacted structure in
    // the batch at once, which is why the batch is bounded rather than
    // unlimited.
    // -----------------------------------------------------------------------
    std::vector<Blas> buildBlasBatch(const std::vector<VoxMesh> &meshes) {
        const auto t0 = std::chrono::steady_clock::now();
        const size_t n = meshes.size();
        std::vector<Blas> result(n);
        if (n == 0) return result;

        struct Staged {
            ref<Buffer> verts, idx, scratch, uncompacted;
            ref<RtAccelerationStructure> as;
            RtGeometryDesc geom{};
            uint64_t uncompactedSize = 0;
        };
        std::vector<Staged> staged(n);

        RtAccelerationStructurePostBuildInfoPool::Desc poolDesc;
        poolDesc.queryType = RtAccelerationStructurePostBuildInfoQueryType::CompactedSize;
        poolDesc.elementCount = uint32_t(n);
        ref<RtAccelerationStructurePostBuildInfoPool> sizePool =
            RtAccelerationStructurePostBuildInfoPool::create(device_.get(), poolDesc);

        // -- pass 1: record every build ---------------------------------------
        for (size_t k = 0; k < n; ++k) {
            const VoxMesh &m = meshes[k];
            Staged &g = staged[k];

            // These two exist only to be read by the build. A structure is
            // self-contained once it is finished, so they are dropped at the
            // end of this function -- after the flush that guarantees the build
            // has read them.
            g.verts = device_->createBuffer(m.position.size() * sizeof(Vec3),
                                            ResourceBindFlags::ShaderResource,
                                            Falcor::MemoryType::DeviceLocal, m.position.data());
            g.idx = device_->createBuffer(m.index.size() * sizeof(uint32_t),
                                          ResourceBindFlags::ShaderResource,
                                          Falcor::MemoryType::DeviceLocal, m.index.data());

            g.geom.type = RtGeometryType::Triangles;
            // OPAQUE is what lets the shader's Proceed() return without ever
            // re-entering traversal: there is no any-hit to run. Foliage is cut
            // out by its voxels, not by an alpha test, so nothing is lost.
            g.geom.flags = RtGeometryFlags::Opaque;
            g.geom.content.triangles.transform3x4 = 0;
            g.geom.content.triangles.vertexFormat = Falcor::ResourceFormat::RGB32Float;
            g.geom.content.triangles.indexFormat = Falcor::ResourceFormat::R32Uint;
            g.geom.content.triangles.vertexCount = uint32_t(m.position.size());
            g.geom.content.triangles.indexCount = uint32_t(m.index.size());
            g.geom.content.triangles.vertexData = g.verts->getGpuAddress();
            g.geom.content.triangles.indexData = g.idx->getGpuAddress();
            g.geom.content.triangles.vertexStride = sizeof(Vec3);

            RtAccelerationStructureBuildInputs inputs = {};
            inputs.kind = RtAccelerationStructureKind::BottomLevel;
            inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace |
                           RtAccelerationStructureBuildFlags::AllowCompaction;
            inputs.descCount = 1;
            inputs.geometryDescs = &g.geom;

            const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
            g.uncompactedSize = pre.resultDataMaxSize;
            g.scratch = device_->createBuffer(pre.scratchDataSize,
                                              ResourceBindFlags::UnorderedAccess,
                                              Falcor::MemoryType::DeviceLocal);
            g.uncompacted = device_->createBuffer(pre.resultDataMaxSize,
                                                  ResourceBindFlags::AccelerationStructure,
                                                  Falcor::MemoryType::DeviceLocal);

            RtAccelerationStructure::Desc cd;
            cd.setKind(RtAccelerationStructureKind::BottomLevel);
            cd.setBuffer(g.uncompacted, 0, pre.resultDataMaxSize);
            g.as = RtAccelerationStructure::create(device_, cd);

            RtAccelerationStructure::BuildDesc bd = {};
            bd.inputs = inputs;
            bd.source = nullptr;
            bd.dest = g.as.get();
            bd.scratchData = g.scratch->getGpuAddress();

            RtAccelerationStructurePostBuildInfoDesc pbi = {};
            pbi.type = RtAccelerationStructurePostBuildInfoQueryType::CompactedSize;
            pbi.index = uint32_t(k);
            pbi.pool = sizePool.get();
            ctx_->buildAccelerationStructure(bd, 1, &pbi);
        }

        // -- pass 2: read the sizes and record every compact -------------------
        // The first getElement flushes and waits for the whole batch; the rest
        // are reads out of the same readback.
        for (size_t k = 0; k < n; ++k) {
            const uint64_t compacted = sizePool->getElement(ctx_, uint32_t(k));
            Staged &g = staged[k];
            Blas &b = result[k];

            const uint64_t finalSize = (compacted > 0 && compacted < g.uncompactedSize)
                                           ? compacted
                                           : g.uncompactedSize;
            b.buffer = device_->createBuffer(finalSize, ResourceBindFlags::AccelerationStructure,
                                             Falcor::MemoryType::DeviceLocal);
            RtAccelerationStructure::Desc bdc;
            bdc.setKind(RtAccelerationStructureKind::BottomLevel);
            bdc.setBuffer(b.buffer, 0, finalSize);
            b.as = RtAccelerationStructure::create(device_, bdc);

            ctx_->copyAccelerationStructure(
                b.as.get(), g.as.get(),
                (finalSize == compacted) ? RenderContext::RtAccelerationStructureCopyMode::Compact
                                         : RenderContext::RtAccelerationStructureCopyMode::Clone);
        }

        // Everything in `staged` is released when this returns, so the copies
        // above have to have run first.
        ctx_->submit(true);
        for (const Blas &b : result) ctx_->uavBarrier(b.buffer.get());

        buildMs_ +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        return result;
    }

    // ------------------------------------------------------------ templates
    void loadModelSet(const std::vector<std::string> &paths, std::vector<ModelTemplate> *out,
                      bool multiModel, bool quiet) {
        for (const std::string &path : paths) {
            std::vector<VoxModel> models;
            std::string err;
            if (multiModel) {
                if (!voxLoadAll(path, &models, &err)) {
                    if (!quiet) std::fprintf(stderr, "v4: %s\n", err.c_str());
                    continue;
                }
            } else {
                VoxModel mo;
                if (!voxLoad(path, &mo, &err)) {
                    if (!quiet) std::fprintf(stderr, "v4: %s\n", err.c_str());
                    continue;
                }
                models.push_back(std::move(mo));
            }

            for (const VoxModel &mo : models) {
                VoxAsset a = toWorld(mo, 0, mo.sx);
                if (a.sx <= 0) continue;

                // ONLY THE ENTRIES THE MODEL USES. Registering all 255 of a
                // file's palette floods the shared table, and past 255
                // forModelColor returns AIR and real voxels stop being drawn.
                std::vector<uint8_t> idOfEntry(256, mat::AIR);
                std::vector<bool> used(256, false);
                for (uint8_t v : a.a) used[v] = true;
                for (int e = 1; e <= 255; ++e)
                    if (used[e]) idOfEntry[e] = palette.forModelColor(mo.pal[e - 1]);

                VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
                if (mesh.triCount() == 0) continue;

                ModelTemplate t;
                t.sx = a.sx;
                t.sy = a.sy;
                t.sz = a.sz;
                // Here and not later: `a` is the only place the model's voxels
                // exist on the host, and it goes out of scope with this loop.
                t.col = measureCollider(a, VOXEL_M, kBodyHeightM);
                t.tris = mesh.triCount();
                t.triOffset = pool_.upload(ctx_, mesh.tri);
                t.blas = buildBlas(mesh);
                uniqueTris += mesh.triCount();
                out->push_back(std::move(t));
            }
        }
    }

    bool loadPines() {
        std::vector<std::string> paths;
        for (int i = 1; i <= 9; ++i)
            paths.push_back(pineDir + "/pine_" + std::to_string(i) + ".vox");
        loadModelSet(paths, &pines_, false, false);
        if (pines_.empty()) {
            std::fprintf(stderr, "v4: no pine models loaded from %s -- pass --pines\n",
                         pineDir.c_str());
            return false;
        }
        loadedPines = int(pines_.size());
        return true;
    }

    // The 26 rocks, each its own file. The names are listed rather than
    // globbed: a fixed list means a missing file is a warning about that file
    // rather than a scene that silently has fewer rocks in it than it should.
    void loadRocks() {
        static const char *kNames[] = {
            "BIG_1_BiG_0",     "Big_2_BiG_0",     "Big_3_BiG_0",     "Big_4_BiG_0",
            "Big_5_BiG_0",     "Mid_1_MID_0",     "Mid_2_MID_0",     "Mid_3_MID_0",
            "Mid_4_MID_0",     "Mid_4_MID_0_001", "Mid_5_MID_0",     "Runic_1_Runic_0",
            "Runic_2_Runic_0", "Runic_3_Runic_0", "Runic_4_Runic_0", "Runic_5_Runic_0",
            "Runic_6_Runic_0", "Runic_7_Runic_0", "Small_1_SMall_0", "Small_2_SMall_0",
            "Small_3_SMall_0", "Small_4_SMall_0", "Small_5_SMall_0", "Small_6_SMall_0",
            "Small_7_SMall_0", "Small_8_SMall_0"};
        std::vector<std::string> paths;
        for (const char *n : kNames) paths.push_back(decorDir + "/rocks/" + n + ".vox");
        loadModelSet(paths, &rocks_, false, false);
        loadedRocks = int(rocks_.size());
    }

    void loadFlowers() {
        loadModelSet({decorDir + "/flowers.vox"}, &flowers_, true, false);
        loadedFlowers = int(flowers_.size());
    }

    void buildWater() {
        // One flat quad, larger than any ring will ever reach. Flat because a
        // tarn in a wood is sheltered; the ripple lives in the shading normal.
        VoxMesh wm;
        const float s = 8000.0f;
        const float y = terrain.waterLevel;
        wm.addQuad({-s, y, -s}, {-s, y, s}, {s, y, s}, {s, y, -s}, mat::AIR, face::POS_Y);
        waterTriOffset_ = pool_.upload(ctx_, wm.tri);
        waterBlas_ = buildBlas(wm);
    }

    // -------------------------------------------------------------- the ring
    bool rering(int cx, int cz) {
        const int R = maxi(1, viewChunks);
        wanted_.clear();
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) wanted_[chunkKey(cx + i, cz + j)] = 1;

        bool changed = false;
        for (auto it = chunks_.begin(); it != chunks_.end();) {
            if (wanted_.count(it->first) == 0) {
                pool_.release(it->second.triOffset, it->second.tris);
                residentTris_ -= it->second.tris;
                it = chunks_.erase(it);
                changed = true;
            } else {
                ++it;
            }
        }

        // Nearest first: the chunk you are standing on matters more than the
        // one at the edge of the ring, and at speed you may never reach that.
        std::vector<std::pair<int, std::pair<int, int>>> order;
        for (int j = -R; j <= R; ++j)
            for (int i = -R; i <= R; ++i) {
                const long long k = chunkKey(cx + i, cz + j);
                if (chunks_.count(k) || requested_.count(k)) continue;
                order.push_back({i * i + j * j, {cx + i, cz + j}});
            }
        std::sort(order.begin(), order.end(),
                  [](const std::pair<int, std::pair<int, int>> &a,
                     const std::pair<int, std::pair<int, int>> &b) { return a.first < b.first; });
        for (const auto &o : order) {
            requested_.insert(chunkKey(o.second.first, o.second.second));
            mesher_.request(o.second.first, o.second.second);
        }
        return changed;
    }

    void adoptMany(std::vector<ChunkBuild> &&batch) {
        std::vector<VoxMesh> meshes;
        meshes.reserve(batch.size());
        for (const ChunkBuild &b : batch) meshes.push_back(b.mesh);
        std::vector<Blas> built = buildBlasBatch(meshes);

        for (size_t k = 0; k < batch.size(); ++k) {
            ChunkBuild &b = batch[k];
            Chunk c;
            c.cx = b.cx;
            c.cz = b.cz;
            c.blas = std::move(built[k]);
            c.tris = b.mesh.triCount();
            c.triOffset = pool_.upload(ctx_, b.mesh.tri);

            for (const Placement &p : b.decor) {
                // Flowers are knee-high and are walked through, so they are
                // drawn and not collided with. Trees and rocks are both.
                Solid s;
                V4Instance info{};
                c.decorDesc.push_back(makeInstance(p, &info, p.kind == 2 ? nullptr : &s));
                c.decorInfo.push_back(info);
                if (p.kind != 2 && s.hx > 0.0f) c.solids.push_back(s);
            }

            residentTris_ += c.tris;
            chunks_.emplace(chunkKey(b.cx, b.cz), std::move(c));
        }
    }

    const ModelTemplate &templateFor(int kind, int index) const {
        const std::vector<ModelTemplate> &v =
            (kind == 0) ? pines_ : (kind == 1) ? rocks_ : flowers_;
        return v[size_t(index) % v.size()];
    }

    // The instance the renderer draws, the record the shader reads, and --
    // through `solidOut` -- the collider the player walks into. ONE function on
    // purpose: the collider is read back out of the transform this just wrote,
    // so there is no second copy of the placement arithmetic that could drift
    // away from the first.
    RtInstanceDesc makeInstance(const Placement &p, V4Instance *info, Solid *solidOut) {
        static const float kRot[4][9] = {
            { 1, 0, 0,  0, 1, 0,  0, 0, 1},
            { 0, 0, 1,  0, 1, 0, -1, 0, 0},
            {-1, 0, 0,  0, 1, 0,  0, 0,-1},
            { 0, 0,-1,  0, 1, 0,  1, 0, 0},
        };
        const ModelTemplate &t = templateFor(p.kind, p.index);
        const float *m = kRot[p.yaw & 3];
        const int fx = (p.yaw & 1) ? t.sz : t.sx;
        const int fz = (p.yaw & 1) ? t.sx : t.sz;

        // A tree standing exactly on the surface looks like it is on tiptoe, so
        // it is sunk a voxel or two. A rock is sunk in proportion to its own
        // height, which is what makes a boulder read as embedded in the ground
        // rather than set down on it.
        int sink = 1;
        if (p.kind == 0) sink = 1 + int(hashUnit(seed + 17u, p.cell) * 2.0f);
        else if (p.kind == 1) sink = 1 + int(float(t.sy) * 0.18f);

        const float tx = float(p.ci) * VOXEL_M - halfOf(fx);
        const float tz = float(p.cj) * VOXEL_M - halfOf(fz);
        const float ty = float(p.h + 1 - sink) * VOXEL_M;

        const float cx = float(t.sx) * VOXEL_M * 0.5f, cz = float(t.sz) * VOXEL_M * 0.5f;
        RtInstanceDesc inst = {};
        writeTransform(inst, m, tx + halfOf(fx) - (m[0] * cx + m[2] * cz), ty,
                       tz + halfOf(fz) - (m[6] * cx + m[8] * cz));

        if (solidOut && t.col.solid()) {
            // The collider's centre in the model's own frame, put through the
            // transform above -- rows 0 and 2 of it, since a quarter turn about
            // Y leaves the height alone.
            const float qx = cx + t.col.cx, qz = cz + t.col.cz;
            solidOut->cx = inst.transform[0][0] * qx + inst.transform[0][2] * qz + inst.transform[0][3];
            solidOut->cz = inst.transform[2][0] * qx + inst.transform[2][2] * qz + inst.transform[2][3];
            // A quarter turn swaps the extents rather than rotating them, which
            // is the whole reason the placement is restricted to quarter turns.
            solidOut->hx = (p.yaw & 1) ? t.col.hz : t.col.hx;
            solidOut->hz = (p.yaw & 1) ? t.col.hx : t.col.hz;
            solidOut->top = inst.transform[1][3] + t.col.top;
            solidOut->standable = (p.kind == 1);
        }

        info->triOffset = t.triOffset;
        info->kind = (p.kind == 0) ? KIND_TREE : KIND_TERRAIN;
        info->tint = (p.kind == 0) ? tintFor(p.cell) : float3(1.0f, 1.0f, 1.0f);
        info->pad0 = info->pad1 = info->pad2 = 0u;

        inst.instanceMask = 0xFF;
        inst.instanceContributionToHitGroupIndex = 0;
        inst.flags = RtGeometryInstanceFlags::None;
        inst.accelerationStructure = t.blas.as->getGpuAddress();
        // instanceID is filled in by rebuildTlas, because it is the position in
        // the instance array and that is only known once the array is laid out.
        inst.instanceID = 0;
        return inst;
    }

    // A few percent of per-tree hue. Nine models over thousands of trees would
    // otherwise show their repeat.
    float3 tintFor(uint32_t cell) const {
        const float t = hashUnit(seed + 19u, cell);
        const float v = 0.90f + 0.20f * hashUnit(seed + 20u, cell);
        return float3(lerpf(0.94f, 1.06f, t) * v, 1.0f * v, lerpf(1.05f, 0.92f, t) * v);
    }

    static void writeTransform(RtInstanceDesc &inst, const float *m, float tx, float ty, float tz) {
        inst.transform[0][0] = m[0]; inst.transform[0][1] = m[1]; inst.transform[0][2] = m[2];
        inst.transform[0][3] = tx;
        inst.transform[1][0] = m[3]; inst.transform[1][1] = m[4]; inst.transform[1][2] = m[5];
        inst.transform[1][3] = ty;
        inst.transform[2][0] = m[6]; inst.transform[2][1] = m[7]; inst.transform[2][2] = m[8];
        inst.transform[2][3] = tz;
    }

    static float halfOf(int voxels) { return float(voxels) * VOXEL_M * 0.5f; }

    // ------------------------------------------------------------------ TLAS
    void rebuildTlas() {
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        instanceDescs_.clear();
        instanceInfos_.clear();

        auto push = [&](RtInstanceDesc d, V4Instance info) {
            d.instanceID = uint32_t(instanceInfos_.size());
            instanceDescs_.push_back(d);
            instanceInfos_.push_back(info);
        };

        {
            RtInstanceDesc water = {};
            writeTransform(water, kI, 0.0f, 0.0f, 0.0f);
            water.instanceMask = 0xFF;
            water.accelerationStructure = waterBlas_.as->getGpuAddress();
            V4Instance info{};
            info.triOffset = waterTriOffset_;
            info.kind = KIND_WATER;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(water, info);
        }

        for (const auto &kv : chunks_) {
            const Chunk &c = kv.second;
            RtInstanceDesc ci = {};
            // Chunk meshes are already in world space, so the transform is
            // identity -- the instance exists to carry the triangle offset, not
            // to place anything.
            writeTransform(ci, kI, 0.0f, 0.0f, 0.0f);
            ci.instanceMask = 0xFF;
            ci.accelerationStructure = c.blas.as->getGpuAddress();
            V4Instance info{};
            info.triOffset = c.triOffset;
            info.kind = KIND_TERRAIN;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(ci, info);

            for (size_t i = 0; i < c.decorDesc.size(); ++i) push(c.decorDesc[i], c.decorInfo[i]);
        }

        const uint32_t n = uint32_t(instanceDescs_.size());
        ensureBuffer(instanceDescBuf_, n * sizeof(RtInstanceDesc), ResourceBindFlags::ShaderResource,
                     "v4::instanceDescs");
        ctx_->updateBuffer(instanceDescBuf_.get(), instanceDescs_.data(), 0,
                           n * sizeof(RtInstanceDesc));

        if (!instanceInfo_ || instanceInfo_->getElementCount() < n) {
            instanceInfo_ = device_->createStructuredBuffer(
                sizeof(V4Instance), std::max(n, 1u), ResourceBindFlags::ShaderResource,
                Falcor::MemoryType::DeviceLocal);
            instanceInfo_->setName("v4::instances");
        }
        if (n > 0)
            ctx_->updateBuffer(instanceInfo_.get(), instanceInfos_.data(), 0,
                               n * sizeof(V4Instance));

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        // No compaction here, unlike the bottom-level structures: this is
        // rebuilt every time the ring moves, and the compaction pass costs a
        // second flush for a structure that is a few megabytes at most.
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        inputs.descCount = n;
        inputs.instanceDescs = instanceDescBuf_->getGpuAddress();

        const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
        ensureBuffer(tlasScratch_, pre.scratchDataSize, ResourceBindFlags::UnorderedAccess,
                     "v4::tlasScratch");

        // The structure object wraps a buffer and does not own it, so a grown
        // buffer means a new object as well.
        if (!tlasBuffer_ || tlasBuffer_->getSize() < pre.resultDataMaxSize) {
            tlasBuffer_ = device_->createBuffer(pre.resultDataMaxSize,
                                                ResourceBindFlags::AccelerationStructure,
                                                Falcor::MemoryType::DeviceLocal);
            tlasBuffer_->setName("v4::tlas");
            tlas_ = nullptr;
        }
        if (!tlas_) {
            RtAccelerationStructure::Desc d;
            d.setKind(RtAccelerationStructureKind::TopLevel);
            d.setBuffer(tlasBuffer_, 0, tlasBuffer_->getSize());
            tlas_ = RtAccelerationStructure::create(device_, d);
        } else {
            ctx_->uavBarrier(tlasBuffer_.get());
            ctx_->uavBarrier(tlasScratch_.get());
        }

        RtAccelerationStructure::BuildDesc bd = {};
        bd.inputs = inputs;
        bd.source = nullptr;
        bd.dest = tlas_.get();
        bd.scratchData = tlasScratch_->getGpuAddress();
        ctx_->buildAccelerationStructure(bd, 0, nullptr);
        ctx_->uavBarrier(tlasBuffer_.get());
    }

    void ensureBuffer(ref<Buffer> &b, size_t bytes, ResourceBindFlags flags, const char *name) {
        bytes = std::max<size_t>(bytes, 256);
        if (b && b->getSize() >= bytes) return;
        b = device_->createBuffer(bytes, flags, Falcor::MemoryType::DeviceLocal);
        b->setName(name);
    }
};

}  // namespace v4
