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
#include "Core/API/Fence.h"
#include "Core/API/RtAccelerationStructure.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <deque>
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
static_assert(v6::VOXEL_M == v6::kVoxelM, "voxel size disagrees with the shader");
static_assert(v6::mat::GRASS_0 == v6::kGrass0, "grass ramp disagrees with the shader");
static_assert(v6::mat::GRASS_COUNT == v6::kGrassCount, "grass ramp disagrees with the shader");
static_assert(v6::mat::SOIL_0 == v6::kSoil0, "soil ramp disagrees with the shader");
static_assert(v6::mat::SOIL_COUNT == v6::kSoilCount, "soil ramp disagrees with the shader");
static_assert(v6::mat::LITTER_0 == v6::kLitter0, "litter ramp disagrees with the shader");
static_assert(v6::mat::LITTER_COUNT == v6::kLitterCount, "litter ramp disagrees with the shader");
static_assert(v6::SUN_COS_THETA_MAX == v6::kSunCosThetaMax, "sun size disagrees with the shader");

namespace v6 {

using Falcor::Buffer;
using Falcor::Fence;
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
        buffer_->setName("v6::TriPool");
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
        nb->setName("v6::TriPool");
        ctx->copyBufferRegion(nb.get(), 0, buffer_.get(), 0, units_ * sizeof(uint16_t));
        ctx->submit(true);  // the old buffer dies with this scope; it must be done being read
        release(uint32_t(units_), want - units_);
        buffer_ = nb;
        units_ = want;
        std::printf("v6: triangle pool grown to %.0f MB\n",
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

// ---------------------------------------------------------------------------
// A recycling pool for the buffers a structure build needs and then discards.
//
// WHY IT EXISTS. Four device buffers are created per chunk -- vertices,
// indices, build scratch and the uncompacted result -- and a profile of a
// flight put 2.5 seconds of a 4-second streaming cost in the RECORDING pass,
// not in either of the two device waits everyone assumes is the problem.
// Creating a buffer is a driver-side heap allocation, twelve hundred of them
// get made while crossing a wood, and every one is the same handful of sizes
// because every chunk is the same 65 536 columns of the same terrain.
//
// So they are kept and handed back out. SIZES ARE ROUNDED UP TO A GRANULE so
// the pool converges on a few dozen buffers instead of one per distinct byte
// count -- without that it is a leak with extra steps.
//
// NOTHING HERE EVER BLOCKS TO FIND OUT WHETHER A BUFFER IS FREE. A slot
// carries the fence value its last user was submitted under, and it is
// reusable once the device has passed that value -- which is a poll, not a
// wait. That is the whole reason this can sit on the frame's own thread.
// ---------------------------------------------------------------------------
class TransientPool {
  public:
    void init(const ref<Device> &d, ResourceBindFlags flags, const char *name) {
        device_ = d;
        flags_ = flags;
        name_ = name;
    }

    ref<Buffer> acquire(size_t bytes, uint64_t deviceDone) {
        bytes = granule(bytes);
        Slot *best = nullptr;
        for (Slot &s : slots_) {
            if (s.busy || s.freeAt > deviceDone || s.size < bytes) continue;
            // Smallest that fits, so a 12 MB request cannot consume the one
            // 40 MB buffer the next model needs and force a new allocation.
            if (!best || s.size < best->size) best = &s;
        }
        if (!best) {
            slots_.push_back({});
            best = &slots_.back();
            best->buf = device_->createBuffer(bytes, flags_, Falcor::MemoryType::DeviceLocal);
            best->buf->setName(name_);
            best->size = bytes;
            bytes_ += bytes;
        }
        best->busy = true;
        return best->buf;
    }

    // Hand a buffer back, unusable again until the device has passed `at`.
    // `frameNo` is only ever read by trim, which ages on frames rather than on
    // the fence -- see the note where it is called.
    void release(const ref<Buffer> &b, uint64_t at, uint64_t frameNo) {
        if (!b) return;
        for (Slot &s : slots_)
            if (s.buf.get() == b.get()) {
                s.busy = false;
                s.freeAt = at;
                s.freeFrame = frameNo;
                return;
            }
    }

    size_t bytes() const { return bytes_; }
    size_t count() const { return slots_.size(); }

  private:
    struct Slot {
        ref<Buffer> buf;
        size_t size = 0;
        uint64_t freeAt = 0;     // device fence value that makes it reusable
        uint64_t freeFrame = 0;  // frame it was handed back, for ageing out
        bool busy = false;
    };
    // THREE SIGNIFICANT BITS, in megabytes. A buffer is at most an eighth
    // bigger than it needed to be, and -- far more usefully -- there are only
    // a handful of distinct sizes for the pool to keep one of. At a flat
    // megabyte the classes were as fine-grained as the mesh sizes themselves,
    // so almost every request missed and the pool became a list of every size
    // a chunk had ever happened to be.
    static size_t granule(size_t b) {
        size_t mb = (b + (1u << 20) - 1) >> 20;
        if (mb == 0) mb = 1;
        size_t step = 1;
        while ((mb >> 3) >= step) step <<= 1;  // keep the top three bits
        mb = ((mb + step - 1) / step) * step;
        return mb << 20;
    }

  public:
    // Give back what nobody has asked for in a while. Called once a frame, so
    // the pool is sized by what the streamer is CURRENTLY doing rather than by
    // the worst thing it ever did -- the prime asks for far more at once than
    // flying ever does, and without this the whole prime's peak stayed resident
    // for the session.
    void trim(uint64_t beforeFrame) {
        for (size_t i = slots_.size(); i-- > 0;) {
            const Slot &s = slots_[i];
            if (s.busy || s.freeFrame == 0 || s.freeFrame > beforeFrame) continue;
            bytes_ -= s.size;
            slots_.erase(slots_.begin() + ptrdiff_t(i));
        }
    }

  private:

    ref<Device> device_;
    ResourceBindFlags flags_ = ResourceBindFlags::None;
    const char *name_ = "v6::transient";
    std::vector<Slot> slots_;
    size_t bytes_ = 0;
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
    std::vector<V6Instance> decorInfo;
    // The trees and rocks of this chunk as things to walk into. Held per chunk
    // rather than in one world-wide list so eviction is free: the colliders go
    // when the chunk does, and nothing has to be searched to remove them.
    std::vector<Solid> solids;
    // Placements by kind -- pine, rock, flower. Held per chunk so eviction
    // keeps the totals honest without anything having to be searched.
    int decorKind[3] = {0, 0, 0};
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
    float treeDensity = 0.2325f;
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
    size_t decorCount(int kind) const {
        size_t n = 0;
        for (const auto &kv : chunks_) n += size_t(kv.second.decorKind[kind]);
        return n;
    }
    size_t residentTris() const { return residentTris_; }
    size_t pendingChunks() { return mesher_.inFlight(); }
    double buildMs() const { return buildMs_; }

    // -- where the main thread goes while the world streams -------------------
    // Meshing is off-thread and cannot stall a frame; everything below is
    // recorded on the frame's own thread and therefore can.
    struct Profile {
        double blasMs = 0.0;   // structure builds, including any device wait
        double poolMs = 0.0;   // triangle-attribute uploads
        double tlasMs = 0.0;   // top-level rebuilds
        double meshMs = 0.0;   // worker time, for context only
        size_t blasCalls = 0, tlasCalls = 0, adopted = 0, meshed = 0;
        double uncompactedMb = 0.0, compactedMb = 0.0;
        // What the recycling pools ended up holding, which is the price of not
        // allocating per chunk and the number to watch if it ever looks wrong.
        double poolMb = 0.0;
        size_t poolBuffers = 0, pendingCompactions = 0;
        double drainMs = 0.0, takeMs = 0.0, reringMs = 0.0;
        size_t drains = 0, forcedDrains = 0;
    };
    Profile profile() const {
        Profile p = prof_;
        p.meshMs = const_cast<ChunkMesher &>(mesher_).meshMs.load(std::memory_order_relaxed);
        p.meshed = const_cast<ChunkMesher &>(mesher_).meshCount.load(std::memory_order_relaxed);
        p.poolMb = double(vertPool_.bytes() + idxPool_.bytes() + scratchPool_.bytes() +
                          rawPool_.bytes()) /
                   (1024.0 * 1024.0);
        p.poolBuffers =
            vertPool_.count() + idxPool_.count() + scratchPool_.count() + rawPool_.count();
        p.pendingCompactions = pendingItems();
        return p;
    }
    void resetProfile() {
        prof_ = Profile{};
        mesher_.meshMs.store(0.0, std::memory_order_relaxed);
        mesher_.meshCount.store(0, std::memory_order_relaxed);
    }
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

        fence_ = device_->createFence();
        vertPool_.init(device_, ResourceBindFlags::ShaderResource, "v6::blasVerts");
        idxPool_.init(device_, ResourceBindFlags::ShaderResource, "v6::blasIndices");
        scratchPool_.init(device_, ResourceBindFlags::UnorderedAccess, "v6::blasScratch");
        rawPool_.init(device_, ResourceBindFlags::AccelerationStructure, "v6::blasRaw");

        // Templates first: the terrain's grass and soil colours are sampled
        // from the palette the models bring with them.
        if (!loadPines()) return false;
        // The ground's colours come from the PINES; everything after this line
        // is rock and flower, and neither is a soil.
        palette.markPinesLoaded();
        loadRocks();
        loadFlowers();
        palette.deriveGroundFromTrees();
        buildWater();

        // The palette is a fixed 255 entries whose fields line up one for one
        // with the shader's material record, but they are separate structs on
        // purpose: MaterialLook belongs to the world builder and V6Material to
        // the pipeline, and the day one of them gains a field the other does
        // not need, this loop is the only thing that has to know.
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
        materials_->setName("v6::materials");

        mesher_.seed = seed;
        mesher_.treeDensity = treeDensity;
        mesher_.rockDensity = rockDensity;
        mesher_.flowerDensity = flowerDensity;
        for (const ModelTemplate &t : pines_)
            mesher_.pineFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : rocks_)
            mesher_.rockFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : flowers_)
            mesher_.flowerFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});

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

        // WHERE THE DEVICE HAS ACTUALLY GOT TO. Polled once, at the top, and
        // never waited on -- everything downstream that needs to know whether
        // some earlier frame's work has finished compares against this.
        if (fence_) {
            const uint64_t v = fence_->getCurrentValue();
            if (v > deviceDone_) deviceDone_ = v;
        }

        // IDLE MEANS: nothing arrived last frame and nothing is being meshed.
        // Measured before the drain, because the drain is what it gates -- and
        // taken from LAST frame's delivery rather than this one's, since this
        // frame's has not happened yet.
        streamIdle_ = (lastAdopted_ == 0 && mesher_.inFlight() == 0);

        bool changed = false;
        // Compacting a structure REPLACES it, so the top level has to be told:
        // the instance still describes the same geometry, but at a new address.
        if (drainCompactions(false)) changed = true;
        releaseBuildInputs();

        // Hand back pooled buffers nothing has wanted for a few hundred frames.
        // The prime asks for far more at once than flying ever does, and
        // without this its peak would stay resident for the whole session.
        // Aged on FRAMES, not on the fence: the fence only moves when a frame
        // records something, so a standing camera would never age anything out
        // -- which is precisely when the prime's peak has least reason to stay.
        ++frameNo_;
        if (frameNo_ > kPoolIdle) {
            const uint64_t cutoff = frameNo_ - kPoolIdle;
            vertPool_.trim(cutoff);
            idxPool_.trim(cutoff);
            scratchPool_.trim(cutoff);
            rawPool_.trim(cutoff);
        }
        if (!primed_ || cx != lastCx_ || cz != lastCz_) {
            lastCx_ = cx;
            lastCz_ = cz;
            primed_ = true;
            const auto tr = std::chrono::steady_clock::now();
            changed |= rering(cx, cz);
            prof_.reringMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tr)
                    .count();
        }

        // Collect a batch first, then build them together. See adoptMany.
        const auto tk = std::chrono::steady_clock::now();
        std::vector<ChunkBuild> batch;
        ChunkBuild b;
        while (int(batch.size()) < buildBudget && mesher_.take(&b)) {
            requested_.erase(chunkKey(b.cx, b.cz));
            if (wanted_.count(chunkKey(b.cx, b.cz)) == 0) continue;  // evicted while queued
            batch.push_back(std::move(b));
        }
        prof_.takeMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tk).count();
        lastAdopted_ = batch.size();
        if (!batch.empty()) {
            adoptMany(std::move(batch));
            changed = true;
        }

        // Deferred while priming: the top-level structure is rebuilt WHOLE
        // every time it changes, so doing it once per batch of adopted chunks
        // meant hundreds of full rebuilds to reach the same state one rebuild
        // at the end would have produced.
        if (changed && rebuildTop) rebuildTlas();

        // Close the frame's recording and put a fence value behind it. This is
        // a submit and NOT a wait: it costs a driver call, and it is what makes
        // "has the device finished with this buffer" answerable next frame
        // without anybody stopping to ask.
        if (recorded_) {
            ctx_->submit(false);
            ctx_->signal(fence_.get(), epoch_);
            ++epoch_;
            recorded_ = false;
        }
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
        // Nothing may still be waiting to be compacted when the first frame is
        // drawn: those entries are holding an uncompacted structure each, and
        // the whole ring's worth would not fit.
        drainCompactions(true);
        rebuildTlas();
    }

  private:
    // -- what the streaming pipeline is made of --------------------------------
    // Declared here rather than beside the code that uses them, because the
    // member block below names them and a member declaration cannot look ahead.

    // One recorded build and everything it borrowed to get recorded.
    struct Staged {
        ref<Buffer> verts, idx, scratch, uncompacted;
        ref<RtAccelerationStructure> as;
        uint64_t uncompactedSize = 0;
        uint32_t query = 0;
    };

    // A structure that is built and in use, but not yet compacted.
    struct PendingCompact {
        Staged staged;
        long long key = 0;       // the chunk to hand the compacted structure to
        Blas *direct = nullptr;  // ...or, at load time, this Blas
    };

    // One generation: the builds that share a query pool, and the fence value
    // by which all of them have run.
    struct CompactGroup {
        ref<RtAccelerationStructurePostBuildInfoPool> pool;
        std::vector<PendingCompact> items;
        uint64_t epoch = 0;    // highest epoch any item was recorded under
        uint64_t opened = 0;   // the epoch it started, so age can close it
        bool sealed = false;
        bool inputsFreed = false;  // vertices, indices and scratch handed back
    };

    // How many builds share a pool, and how many epochs an unfilled one may
    // stay open before it is sealed and becomes eligible to be read.
    static constexpr size_t kGroupSize = 16;
    static constexpr uint64_t kGroupAge = 6;

    // HOW MUCH UNCOMPACTED STRUCTURE IS WORTH HOLDING RATHER THAN STALLING FOR.
    //
    // Reading a compacted size costs a device drain -- Falcor's query pool
    // flushes on its first read and there is no way to ask it not to -- and a
    // drain waits for the frame currently on the GPU. Measured at about eleven
    // milliseconds a time, which is a whole frame, and doing it every eight
    // chunks was 3.7 seconds of a 5.6-second streaming cost on a long flight:
    // once the buffer allocations were fixed, THIS became the stutter.
    //
    // So compaction stopped being scheduled and became opportunistic. It
    // happens when the streamer has nothing else to do -- which in a game is
    // most of the time, because people stop and look at things -- and otherwise
    // only when the uncompacted structures have piled up this far. Sprinting in
    // a straight line for a minute is the case that pays, and it pays in memory
    // instead of in frames.
    static constexpr size_t kUncompactedBudget = size_t(320) << 20;

    // How many epochs a pooled buffer may sit unwanted before it is released.
    // An epoch is a frame that recorded something, so this is a few seconds of
    // walking -- long enough that crossing a chunk boundary never re-allocates,
    // short enough that standing still gives the prime's peak back.
    static constexpr uint64_t kPoolIdle = 240;

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
    std::vector<V6Instance> instanceInfos_;

    size_t residentTris_ = 0;
    double buildMs_ = 0.0;
    Profile prof_;
    int lastCx_ = 0, lastCz_ = 0;
    bool primed_ = false;

    // -- the streaming pipeline's clock ---------------------------------------
    //
    // ONE FENCE, TICKED ONCE PER FRAME THAT RECORDS ANYTHING. `epoch_` is the
    // value this frame's work will signal; `deviceDone_` is the value the queue
    // has actually reached, polled and never waited on. Everything else in the
    // streamer -- when a pooled buffer is reusable, when a post-build query may
    // be read -- is a comparison between those two numbers.
    ref<Fence> fence_;
    uint64_t epoch_ = 1;
    uint64_t deviceDone_ = 0;
    uint64_t frameNo_ = 0;
    size_t lastAdopted_ = 0;
    bool streamIdle_ = false;
    bool recorded_ = false;  // did this frame put anything on the queue

    std::deque<CompactGroup> groups_;
    std::vector<ref<RtAccelerationStructurePostBuildInfoPool>> freePools_;
    TransientPool vertPool_, idxPool_, scratchPool_, rawPool_;

    // ------------------------------------------------------------ structures
    //
    // THE STREAMING HITCH LIVED HERE, and the shape of the fix is the shape of
    // what was actually wrong rather than what it looked like.
    //
    // The old path built a chunk's structure the way the textbook describes:
    // record the build, WAIT, read back how small the compacted form would be,
    // record the compact, WAIT again. Two full device drains, on the frame's
    // own thread, every time a chunk arrived. A drain is not merely a submit --
    // it waits for everything already queued, which includes the path trace and
    // the reconstruction of the frame being drawn. So taking delivery of a
    // chunk cost a whole frame of rendering, paid twice, in whichever frame was
    // unlucky enough to be holding the parcel.
    //
    // Measured on a flight across the wood: 4.0 s of a 9 s run inside this
    // function, a QUARTER of all frames over twice the median, and a p99 of
    // 38 ms against a 7 ms median. That is the stutter, exactly.
    //
    // Three things changed, and only the last is the one anybody expects:
    //
    //   1. THE BUFFERS ARE RECYCLED. 2.5 of those 4 seconds were in neither
    //      wait -- they were in CREATING four device buffers per chunk, which
    //      is four driver-side heap allocations of the same few sizes, twelve
    //      hundred times over. See TransientPool.
    //
    //   2. COMPACTION IS DEFERRED, not abandoned. It is worth 60% of the
    //      structure memory here -- a radius-12 ring is 2.9 GB compacted
    //      against 7.3 GB raw, and the second number does not fit on the card
    //      -- so it has to happen. Nothing forces it to happen in the frame the
    //      chunk arrives, though: the chunk enters the world immediately in its
    //      uncompacted form and is compacted a few frames later, when the
    //      answer is already sitting in memory and reading it costs nothing.
    //
    //   3. NOTHING WAITS. Readiness is a FENCE VALUE THAT GETS POLLED. Work
    //      recorded during a frame is stamped with the value the queue will
    //      reach once it has run, and anything stamped at or below what the
    //      device has actually reached is finished. That is what makes it safe
    //      both to recycle a buffer and to read a post-build query without
    //      draining anything, and it is the difference between asking the
    //      device a question and stopping to hear the answer.
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // Record one build. Nothing is waited on and nothing is read back.
    //
    // The geometry description is a local because the build is recorded before
    // this returns -- the API reads it during the call and not afterwards.
    // -----------------------------------------------------------------------
    // The generation currently being written. A pool is written once and read
    // once, so a full or elderly one is sealed and a fresh one opened.
    CompactGroup &openGroup() {
        if (!groups_.empty() && !groups_.back().sealed &&
            groups_.back().items.size() < kGroupSize)
            return groups_.back();
        if (!groups_.empty() && !groups_.back().sealed) groups_.back().sealed = true;

        CompactGroup g;
        g.pool = acquireQueryPool();
        g.opened = epoch_;
        groups_.push_back(std::move(g));
        return groups_.back();
    }

    // Pools are reset and reused rather than recreated. `reset` is what puts a
    // pool back into the write-once state its results depend on.
    ref<RtAccelerationStructurePostBuildInfoPool> acquireQueryPool() {
        if (!freePools_.empty()) {
            ref<RtAccelerationStructurePostBuildInfoPool> q = freePools_.back();
            freePools_.pop_back();
            q->reset(ctx_);
            return q;
        }
        RtAccelerationStructurePostBuildInfoPool::Desc qd;
        qd.queryType = RtAccelerationStructurePostBuildInfoQueryType::CompactedSize;
        qd.elementCount = uint32_t(kGroupSize);
        return RtAccelerationStructurePostBuildInfoPool::create(device_.get(), qd);
    }

    Staged recordBuild(const VoxMesh &m, CompactGroup &group) {
        Staged g;
        g.query = uint32_t(group.items.size());

        const size_t vbytes = m.position.size() * sizeof(Vec3);
        const size_t ibytes = m.index.size() * sizeof(uint32_t);
        g.verts = vertPool_.acquire(vbytes, deviceDone_);
        g.idx = idxPool_.acquire(ibytes, deviceDone_);
        ctx_->updateBuffer(g.verts.get(), m.position.data(), 0, vbytes);
        ctx_->updateBuffer(g.idx.get(), m.index.data(), 0, ibytes);
        // AND THE BARRIER BACK. updateBuffer leaves a buffer in CopyDest, and
        // buildAccelerationStructure adds no barriers of its own -- it is
        // handed raw device addresses and cannot know which resources they
        // belong to. Reading a CopyDest resource as a build input is undefined,
        // and on this driver it removes the device. Creating the buffer WITH
        // its data hid this, because that path leaves it in Common; filling a
        // recycled one by hand does not.
        ctx_->resourceBarrier(g.verts.get(), Falcor::Resource::State::NonPixelShader);
        ctx_->resourceBarrier(g.idx.get(), Falcor::Resource::State::NonPixelShader);

        RtGeometryDesc geom{};
        geom.type = RtGeometryType::Triangles;
        // OPAQUE is what lets the shader's Proceed() return without ever
        // re-entering traversal: there is no any-hit to run. Foliage is cut
        // out by its voxels, not by an alpha test, so nothing is lost.
        geom.flags = RtGeometryFlags::Opaque;
        geom.content.triangles.transform3x4 = 0;
        geom.content.triangles.vertexFormat = Falcor::ResourceFormat::RGB32Float;
        geom.content.triangles.indexFormat = Falcor::ResourceFormat::R32Uint;
        geom.content.triangles.vertexCount = uint32_t(m.position.size());
        geom.content.triangles.indexCount = uint32_t(m.index.size());
        geom.content.triangles.vertexData = g.verts->getGpuAddress();
        geom.content.triangles.indexData = g.idx->getGpuAddress();
        geom.content.triangles.vertexStride = sizeof(Vec3);

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::BottomLevel;
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace |
                       RtAccelerationStructureBuildFlags::AllowCompaction;
        inputs.descCount = 1;
        inputs.geometryDescs = &geom;

        const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
        g.uncompactedSize = pre.resultDataMaxSize;
        g.scratch = scratchPool_.acquire(pre.scratchDataSize, deviceDone_);
        g.uncompacted = rawPool_.acquire(pre.resultDataMaxSize, deviceDone_);

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
        pbi.index = g.query;
        pbi.pool = group.pool.get();
        ctx_->buildAccelerationStructure(bd, 1, &pbi);
        recorded_ = true;
        return g;
    }

    // -----------------------------------------------------------------------
    // Turn a finished build into its compacted form and give it to its owner.
    //
    // The size is READ, not waited for: the caller has already established that
    // the device passed the build. The plausibility test is not paranoia about
    // that -- a driver is entitled to report a compacted size no smaller than
    // the original, and cloning is the right answer when it does.
    // -----------------------------------------------------------------------
    void finishCompact(PendingCompact &p, RtAccelerationStructurePostBuildInfoPool *pool) {
        Staged &g = p.staged;
        const uint64_t compacted = pool->getElement(ctx_, g.query);
        const uint64_t finalSize =
            (compacted > 0 && compacted < g.uncompactedSize) ? compacted : g.uncompactedSize;

        Blas b;
        b.buffer = device_->createBuffer(finalSize, ResourceBindFlags::AccelerationStructure,
                                         Falcor::MemoryType::DeviceLocal);
        b.buffer->setName("v6::blas");
        RtAccelerationStructure::Desc bdc;
        bdc.setKind(RtAccelerationStructureKind::BottomLevel);
        bdc.setBuffer(b.buffer, 0, finalSize);
        b.as = RtAccelerationStructure::create(device_, bdc);

        ctx_->copyAccelerationStructure(
            b.as.get(), g.as.get(),
            (finalSize == compacted) ? RenderContext::RtAccelerationStructureCopyMode::Compact
                                     : RenderContext::RtAccelerationStructureCopyMode::Clone);
        ctx_->uavBarrier(b.buffer.get());
        recorded_ = true;

        prof_.uncompactedMb += double(g.uncompactedSize) / (1024.0 * 1024.0);
        prof_.compactedMb += double(finalSize) / (1024.0 * 1024.0);

        if (p.direct) {
            *p.direct = std::move(b);
        } else {
            // The chunk may have been evicted while this was in flight, in
            // which case the compacted structure is simply dropped -- it is a
            // buffer and a descriptor, and nothing else ever knew about it.
            auto it = chunks_.find(p.key);
            if (it != chunks_.end()) it->second.blas = std::move(b);
        }
    }

    // THE BUILD INPUTS DIE BEFORE THE STRUCTURE DOES.
    //
    // Vertices, indices and scratch are read by the build and by nothing after
    // it, so they are free the moment the device has passed the build -- which
    // is several frames before the group is sealed and compacted. Holding them
    // until then was three quarters of what the pools were carrying, for no
    // reason beyond it being the tidier place to write the code.
    void releaseBuildInputs() {
        for (CompactGroup &g : groups_) {
            if (g.inputsFreed || g.epoch > deviceDone_) continue;
            for (PendingCompact &p : g.items) {
                vertPool_.release(p.staged.verts, epoch_, frameNo_);
                idxPool_.release(p.staged.idx, epoch_, frameNo_);
                scratchPool_.release(p.staged.scratch, epoch_, frameNo_);
                p.staged.verts = p.staged.idx = p.staged.scratch = nullptr;
            }
            g.inputsFreed = true;
        }
    }

    // What is left once a structure has been compacted: the uncompacted form it
    // was copied from, reusable once the device passes THIS frame -- the
    // compacting copy is still only recorded.
    void recyclePending(Staged &g) {
        vertPool_.release(g.verts, epoch_, frameNo_);
        idxPool_.release(g.idx, epoch_, frameNo_);
        scratchPool_.release(g.scratch, epoch_, frameNo_);
        rawPool_.release(g.uncompacted, epoch_, frameNo_);
        g.verts = g.idx = g.scratch = g.uncompacted = nullptr;
        g.as = nullptr;
    }

    // Draw a line under everything recorded so far and wait for it. The fence
    // is moved with it, so the pools and the pending list go on measuring
    // readiness the same way whether the line was drawn here or reached
    // naturally by frames going past.
    //
    // Load time and the memory valve only -- never while a frame is displayed.
    void syncPoint() {
        ctx_->submit(true);
        ctx_->signal(fence_.get(), epoch_);
        deviceDone_ = epoch_;
        ++epoch_;
        recorded_ = false;
    }

    // -----------------------------------------------------------------------
    // Compact whatever the device has finished building.
    //
    // `force` is the memory valve and the load-time path: it drains the device
    // first, which makes every outstanding build finished by definition. It is
    // never taken while a frame is being displayed.
    // -----------------------------------------------------------------------
    // The uncompacted structures currently being held, in bytes.
    size_t uncompactedBytes() const {
        size_t n = 0;
        for (const CompactGroup &g : groups_)
            for (const PendingCompact &p : g.items) n += size_t(p.staged.uncompactedSize);
        return n;
    }

    bool drainCompactions(bool force) {
        if (groups_.empty()) return false;
        const auto td = std::chrono::steady_clock::now();
        struct Timer {
            World *w;
            std::chrono::steady_clock::time_point t;
            ~Timer() {
                w->prof_.drainMs +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
                        .count();
            }
        } timer{this, td};
        ++prof_.drains;
        if (force) ++prof_.forcedDrains;
        // An open generation is sealed once it has aged, so a trickle of chunks
        // still gets compacted rather than sitting uncompacted indefinitely.
        if (!groups_.back().sealed && (force || epoch_ - groups_.back().opened >= kGroupAge))
            groups_.back().sealed = true;

        // The gate. Reading a size drains the device, so it waits for a moment
        // when that costs nothing -- the streamer being idle -- unless the
        // structures being held have grown past what the memory is worth.
        if (!force && !streamIdle_ && uncompactedBytes() < kUncompactedBudget) return false;
        if (force) syncPoint();  // every outstanding build has now certainly run

        bool any = false;
        while (!groups_.empty()) {
            CompactGroup &grp = groups_.front();
            if (!grp.sealed || grp.epoch > deviceDone_) break;
            for (PendingCompact &p : grp.items) {
                finishCompact(p, grp.pool.get());
                recyclePending(p.staged);
            }
            freePools_.push_back(grp.pool);
            groups_.pop_front();
            any = true;
        }
        return any;
    }

    size_t pendingItems() const {
        size_t n = 0;
        for (const CompactGroup &g : groups_) n += g.items.size();
        return n;
    }

    // -----------------------------------------------------------------------
    // Record a batch of chunk builds. Returns immediately with the UNCOMPACTED
    // structures, which are perfectly good structures -- they are simply bigger
    // than they need to be, and that is a problem for three frames from now.
    // -----------------------------------------------------------------------
    Blas recordChunkBuild(const VoxMesh &m, long long key) {
        const auto t0 = std::chrono::steady_clock::now();
        CompactGroup &grp = openGroup();
        Staged g = recordBuild(m, grp);

        Blas raw;
        raw.buffer = g.uncompacted;
        raw.as = g.as;

        PendingCompact p;
        p.staged = std::move(g);
        p.key = key;
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));

        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        buildMs_ += ms;
        prof_.blasMs += ms;
        ++prof_.blasCalls;
        return raw;
    }

    // -----------------------------------------------------------------------
    // One structure, synchronously. The models and the water only.
    //
    // Load time has no frame to protect, so this takes the simple road: record,
    // drain, compact. It goes through the same recording and compaction code as
    // a chunk so there is one build path and not two that can drift apart.
    // -----------------------------------------------------------------------
    Blas buildBlas(const VoxMesh &m) {
        Blas result;
        CompactGroup &grp = openGroup();
        PendingCompact p;
        p.staged = recordBuild(m, grp);
        p.direct = &result;
        grp.epoch = epoch_;
        grp.items.push_back(std::move(p));
        drainCompactions(true);
        // The compacting copy is only RECORDED by the drain, and the buffers it
        // reads have already gone back to the pools. A second sync point is
        // what makes their release honest now rather than at whatever frame
        // boundary happens to come next.
        syncPoint();
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
                    if (!quiet) std::fprintf(stderr, "v6: %s\n", err.c_str());
                    continue;
                }
            } else {
                VoxModel mo;
                if (!voxLoad(path, &mo, &err)) {
                    if (!quiet) std::fprintf(stderr, "v6: %s\n", err.c_str());
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
            std::fprintf(stderr, "v6: no pine models loaded from %s -- pass --pines\n",
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

    // NOTHING IS COPIED HERE ANY MORE. This used to build a vector of MESHES
    // to hand to the batch builder -- about seven megabytes a chunk of
    // positions and indices, memcpy'd on the frame's own thread purely to pass
    // them to something that reads them once and never keeps them.
    void adoptMany(std::vector<ChunkBuild> &&batch) {
        for (size_t k = 0; k < batch.size(); ++k) {
            ChunkBuild &b = batch[k];
            const long long key = chunkKey(b.cx, b.cz);
            Chunk c;
            c.cx = b.cx;
            c.cz = b.cz;
            c.blas = recordChunkBuild(b.mesh, key);
            c.tris = b.mesh.triCount();
            const auto tp = std::chrono::steady_clock::now();
            c.triOffset = pool_.upload(ctx_, b.mesh.tri);
            prof_.poolMs +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tp)
                    .count();
            ++prof_.adopted;

            for (const Placement &p : b.decor) {
                // Flowers are knee-high and are walked through, so they are
                // drawn and not collided with. Trees and rocks are both.
                Solid s;
                V6Instance info{};
                if (p.kind >= 0 && p.kind < 3) ++c.decorKind[p.kind];
                c.decorDesc.push_back(makeInstance(p, &info, p.kind == 2 ? nullptr : &s));
                c.decorInfo.push_back(info);
                if (p.kind != 2 && s.hx > 0.0f) c.solids.push_back(s);
            }

            residentTris_ += c.tris;
            chunks_.emplace(key, std::move(c));

            // THE VALVE, and it is checked HERE -- with the chunk already in
            // the world -- because a drain hands the compacted structure to
            // whichever chunk owns it, and a chunk that is not there yet gets
            // its structure thrown away and its buffer recycled underneath it.
            //
            // A MEMORY bound, not a scheduling one. It is reached by the
            // opening prime, where blocking is free, and by sustained flight in
            // one direction, where it is the price of not stalling sooner.
            if (uncompactedBytes() >= kUncompactedBudget) drainCompactions(true);
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
    RtInstanceDesc makeInstance(const Placement &p, V6Instance *info, Solid *solidOut) {
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
        const int sink = decorSink(p.kind, t.sy, seed, p.cell);

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
        const auto tt0 = std::chrono::steady_clock::now();
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        instanceDescs_.clear();
        instanceInfos_.clear();

        auto push = [&](RtInstanceDesc d, V6Instance info) {
            d.instanceID = uint32_t(instanceInfos_.size());
            instanceDescs_.push_back(d);
            instanceInfos_.push_back(info);
        };

        {
            RtInstanceDesc water = {};
            writeTransform(water, kI, 0.0f, 0.0f, 0.0f);
            water.instanceMask = 0xFF;
            water.accelerationStructure = waterBlas_.as->getGpuAddress();
            V6Instance info{};
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
            V6Instance info{};
            info.triOffset = c.triOffset;
            info.kind = KIND_TERRAIN;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(ci, info);

            for (size_t i = 0; i < c.decorDesc.size(); ++i) push(c.decorDesc[i], c.decorInfo[i]);
        }

        const uint32_t n = uint32_t(instanceDescs_.size());
        ensureBuffer(instanceDescBuf_, n * sizeof(RtInstanceDesc), ResourceBindFlags::ShaderResource,
                     "v6::instanceDescs");
        ctx_->updateBuffer(instanceDescBuf_.get(), instanceDescs_.data(), 0,
                           n * sizeof(RtInstanceDesc));

        if (!instanceInfo_ || instanceInfo_->getElementCount() < n) {
            instanceInfo_ = device_->createStructuredBuffer(
                sizeof(V6Instance), std::max(n, 1u), ResourceBindFlags::ShaderResource,
                Falcor::MemoryType::DeviceLocal);
            instanceInfo_->setName("v6::instances");
        }
        if (n > 0)
            ctx_->updateBuffer(instanceInfo_.get(), instanceInfos_.data(), 0,
                               n * sizeof(V6Instance));

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
                     "v6::tlasScratch");

        // The structure object wraps a buffer and does not own it, so a grown
        // buffer means a new object as well.
        if (!tlasBuffer_ || tlasBuffer_->getSize() < pre.resultDataMaxSize) {
            tlasBuffer_ = device_->createBuffer(pre.resultDataMaxSize,
                                                ResourceBindFlags::AccelerationStructure,
                                                Falcor::MemoryType::DeviceLocal);
            tlasBuffer_->setName("v6::tlas");
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
        prof_.tlasMs +=
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - tt0)
                .count();
        ++prof_.tlasCalls;
    }

    void ensureBuffer(ref<Buffer> &b, size_t bytes, ResourceBindFlags flags, const char *name) {
        bytes = std::max<size_t>(bytes, 256);
        if (b && b->getSize() >= bytes) return;
        b = device_->createBuffer(bytes, flags, Falcor::MemoryType::DeviceLocal);
        b->setName(name);
    }
};

}  // namespace v6
