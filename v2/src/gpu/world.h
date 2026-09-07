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
static_assert(v2::VOXEL_M == v2::kVoxelM, "voxel size disagrees with the shader");
static_assert(v2::mat::GRASS_0 == v2::kGrass0, "grass ramp disagrees with the shader");
static_assert(v2::mat::GRASS_COUNT == v2::kGrassCount, "grass ramp disagrees with the shader");
static_assert(v2::mat::SOIL_0 == v2::kSoil0, "soil ramp disagrees with the shader");
static_assert(v2::mat::SOIL_COUNT == v2::kSoilCount, "soil ramp disagrees with the shader");
static_assert(v2::mat::LITTER_0 == v2::kLitter0, "litter ramp disagrees with the shader");
static_assert(v2::mat::LITTER_COUNT == v2::kLitterCount, "litter ramp disagrees with the shader");
static_assert(v2::SUN_COS_THETA_MAX == v2::kSunCosThetaMax, "sun size disagrees with the shader");

namespace v2 {

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
        buffer_->setName("v2::TriPool");
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
        nb->setName("v2::TriPool");
        ctx->copyBufferRegion(nb.get(), 0, buffer_.get(), 0, units_ * sizeof(uint16_t));
        ctx->submit(true);  // the old buffer dies with this scope; it must be done being read
        release(uint32_t(units_), want - units_);
        buffer_ = nb;
        units_ = want;
        std::printf("v2: triangle pool grown to %.0f MB\n",
                    double(units_ * sizeof(uint16_t)) / (1024.0 * 1024.0));
    }
};

// THE TWO RAY MASKS, host side. Shared.slang carries the shader's copy and the
// note on what they are for; these are the same two bits, and the assert below
// is what stops them drifting.
constexpr uint8_t kMaskWorld = 0x01;
constexpr uint8_t kMaskHeld = 0x02;

// HOW MANY SHAFTS CAN BE IN THE AIR, as instances the structure reserves. It
// is here rather than in render/arrows.h because the STRUCTURE is what the
// number is really about: an update may not change how many instances there
// are, so the slots have to exist from the first build whether or not anything
// has been shot. See the note over the arrow block in rebuildTlas.
constexpr int kArrowInstances = 12;

// ...AND HOW MANY BUTTERFLIES, for exactly the same reason and at exactly the
// same cost. The flock is a fixed pool of slots that are filled and emptied as
// their homes come into range and leave it (render/butterflies.h), so the
// number that matters to the STRUCTURE is the ceiling, not how many are flying:
// every slot exists from the first build, masked off until something is in it.
//
// SIXTY-FOUR, against a scene of about forty-two thousand instances. The whole
// band is a rounding error in the top-level structure and its refit, and it is
// three times the flock the viewer opens with -- so `--butterflies` can be
// turned up on the command line without the structure having to be told.
constexpr int kFlyerInstances = 64;
static_assert(kMaskWorld == uint8_t(MASK_WORLD), "ray masks disagree with the shader");
static_assert(kMaskHeld == uint8_t(MASK_HELD), "ray masks disagree with the shader");

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
    // Only filled for pines: every voxel in the crown a cone could rest on.
    std::vector<Perch> perches;
    // Anchors wide enough for a beehive -- see the clearW note on
    // collectPerches. Only filled for the birch wood; a pine has no use for
    // them and computing them is a 5x5 sweep per candidate voxel.
    std::vector<Perch> hivePerches;
    // The top of every column, for the collider. See columnTops.
    std::vector<int16_t> colTop;
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
    const char *name_ = "v2::transient";
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
    int decorKind[6] = {0, 0, 0, 0, 0, 0};  // pine, rock, flower, mushroom, cone, hive
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
    // The birch wood's sixteen trees. Same 10 cm grid as the pines.
    std::string birchDir = "C:/voxelbit/game/assets/foilage/birch_trees";
    int viewChunks = 12;  // ring radius, in chunks
    float treeDensity = 0.2325f;
    float rockDensity = 0.010f;
    float flowerDensity = 0.45f;
    float mushroomDensity = 0.015f;
    int pineconesPerTree = 14;
    uint32_t seed = 20260904u;
    int meshThreads = 0;

    int loadedPines = 0, loadedRocks = 0, loadedFlowers = 0, loadedMushrooms = 0;
    int loadedPinecones = 0;
    int mushroomBig0 = 0;
    size_t uniqueTris = 0;

    // A ref rather than a raw pointer: ShaderVar::setAccelerationStructure
    // takes one, and it is the only thing the tracer needs from here.
    const ref<RtAccelerationStructure> &tlasRef() const { return tlas_; }
    const ref<Buffer> &triPool() const { return pool_.buffer(); }
    const ref<Buffer> &instanceBuffer() const { return instanceInfo_; }
    const ref<Buffer> &materialBuffer() const { return materials_; }

    // -----------------------------------------------------------------------
    // THE TOOL IN THE PLAYER'S HAND, as geometry.
    //
    // Loaded exactly the way a rock is -- the same .vox reader, the same
    // Palette::forModelColor for every colour it uses, the same mesher, the
    // same triangle pool, the same bottom-level structure. It is a decor model
    // that happens to be placed by the camera instead of by the scatter, and
    // everything downstream of here treats it as one.
    //
    // MESHED AT ONE UNIT PER VOXEL, not at VOXEL_M. A held tool's voxels are a
    // few millimetres, the size depends on the pose and on the field of view,
    // and both move on a slider -- so the size lives in the instance transform
    // as a uniform scale rather than baked into vertices that would have to be
    // re-meshed every time the slider twitched. It is the only instance in this
    // scene carrying a scale, which is why KIND_HELD exists: the shader has to
    // normalise the normal it transforms. See Trace.cs.slang.
    //
    // Returns the grid's extents so the caller can build a pose around the
    // model's own box; false if the file will not load, which is not fatal --
    // an engine that refuses to start over a missing viewmodel would be a worse
    // bug than the missing viewmodel.
    // -----------------------------------------------------------------------
    int addHeldModel(const std::string &path, int *sx, int *sy, int *sz) {
        VoxModel mo;
        std::string err;
        if (!voxLoad(path, &mo, &err)) {
            std::fprintf(stderr, "v2: held item %s: %s -- skipped\n", path.c_str(), err.c_str());
            return -1;
        }
        return addHeldVox(mo, path, sx, sy, sz);
    }

    // The same, for a model that was COMPOSED rather than read. The bow's draw
    // frames are cut out of one file by render/bow.h and never exist on disk as
    // models of their own, so there is nothing for addHeldModel to open; `what`
    // is only ever printed.
    int addHeldVox(const VoxModel &mo, const std::string &path, int *sx, int *sy, int *sz) {
        // WHOLE, NOT TRIMMED. A held model's pose is measured from the middle
        // of its grid, and a strip's frames have to share one -- see
        // toWorldWhole for what trimming does to a bow.
        VoxAsset a = toWorldWhole(mo);
        if (a.sx <= 0) {
            std::fprintf(stderr, "v2: held item %s is empty -- skipped\n", path.c_str());
            return -1;
        }

        // ONLY THE ENTRIES THE MODEL USES, exactly as loadDecor does:
        // registering all 255 of a file's palette floods a table that is 255
        // entries for the whole world, and past the end forModelColor returns
        // AIR and real voxels stop being drawn.
        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : a.a) used[v] = true;
        int minted = 0;
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)]) {
                idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1]);
                if (idOfEntry[size_t(e)] != mat::AIR) ++minted;
            }
        // The entries this just minted are not on the GPU yet: init() uploaded
        // the table before this was called, because the world has to exist
        // before anything can be held in front of it.
        uploadMaterials();

        const VoxMesh mesh = meshAsset(a, idOfEntry, 1.0f);
        if (mesh.triCount() == 0) {
            std::fprintf(stderr, "v2: held item %s meshed to nothing -- skipped\n", path.c_str());
            return -1;
        }

        HeldModel hm;
        hm.tri = pool_.upload(ctx_, mesh.tri);
        hm.blas = buildBlas(mesh);
        if (!hm.blas.valid()) return -1;
        hm.sx = a.sx;
        hm.sy = a.sy;
        hm.sz = a.sz;
        held_.push_back(std::move(hm));

        if (sx) *sx = a.sx;
        if (sy) *sy = a.sy;
        if (sz) *sz = a.sz;
        std::printf("v2: held item %s  %dx%dx%d, %zu tris, %d materials\n", path.c_str(), a.sx,
                    a.sy, a.sz, mesh.triCount(), minted);
        std::fflush(stdout);
        // The slot has to exist in the structure from the next rebuild on --
        // see setHeldInstance for why it is present even while nothing is held.
        // Rebuilt on the FIRST model only: the slot is one instance whatever is
        // in it, so a second tool changes nothing about the structure's shape.
        if (held_.size() == 1) rebuildTlas();
        return int(held_.size()) - 1;
    }

    bool heldLoaded() const { return !held_.empty(); }
    int heldModelCount() const { return int(held_.size()); }

    // -----------------------------------------------------------------------
    // A FLYER'S FRAME, as geometry, and it is deliberately not the held path.
    //
    // Everything a tool loads through -- the reader, the palette, the mesher,
    // the pool, the structure -- is the same. ONE number differs and it is the
    // one that decides which of two worlds the model lives in:
    //
    //   A HELD MODEL IS MESHED AT ONE UNIT PER VOXEL and shrunk by its instance
    //   transform, because a viewmodel's size is a slider and re-meshing on
    //   every twitch of it is absurd.
    //
    //   A FLYER IS MESHED AT VOXEL_M, so its vertices are already in metres and
    //   the instance transform is a TURN AND A TRANSLATION -- the same shape
    //   every tree and rock in this world carries. That is what keeps it on the
    //   terrain's own path: the tracer's note over the normal transform says
    //   every 3x3 in this scene is orthonormal, and a butterfly's is, where a
    //   voxel-scaled one would not have been.
    //
    // (The fade a flyer materialises through does put a scale back into that
    // 3x3 -- see KIND_FLYER. It is a uniform one, so the transform stays a
    // rotation times a number, and one normalise in the tracer covers it.)
    //
    // WHOLE, NOT TRIMMED, for the same reason a bow's draw frames are: the
    // eight flap frames are one animation and have to share an origin, and
    // trimming each to its own occupied bounds would shift the body sideways
    // every time a wing came up.
    // -----------------------------------------------------------------------
    int addFlyerModel(const VoxModel &mo, const std::string &what, int *sx, int *sy, int *sz) {
        VoxAsset a = toWorldWhole(mo);
        if (a.sx <= 0) {
            std::fprintf(stderr, "v2: flyer %s is empty -- skipped\n", what.c_str());
            return -1;
        }
        std::vector<uint8_t> idOfEntry(256, mat::AIR);
        std::vector<bool> used(256, false);
        for (uint8_t v : a.a) used[v] = true;
        for (int e = 1; e <= 255; ++e)
            if (used[size_t(e)])
                // NOT A CONIFER: see the note over forModelColor for what the
                // green rule does to a lime butterfly's wing.
                idOfEntry[size_t(e)] = palette.forModelColor(mo.pal[size_t(e) - 1], false);
        uploadMaterials();

        const VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
        if (mesh.triCount() == 0) {
            std::fprintf(stderr, "v2: flyer %s meshed to nothing -- skipped\n", what.c_str());
            return -1;
        }
        HeldModel fm;
        fm.tri = pool_.upload(ctx_, mesh.tri);
        fm.blas = buildBlas(mesh);
        if (!fm.blas.valid()) return -1;
        fm.sx = a.sx;
        fm.sy = a.sy;
        fm.sz = a.sz;
        flyers_.push_back(std::move(fm));

        if (sx) *sx = a.sx;
        if (sy) *sy = a.sy;
        if (sz) *sz = a.sz;
        // The band has to exist in the structure from the next rebuild on --
        // see the note over kFlyerInstances. On the FIRST model only: the band
        // is the same size whatever is loaded into it.
        if (flyers_.size() == 1) rebuildTlas();
        return int(flyers_.size()) - 1;
    }

    bool flyersLoaded() const { return !flyers_.empty(); }
    int flyerModelCount() const { return int(flyers_.size()); }

    // -----------------------------------------------------------------------
    // Where one butterfly is this frame.
    //
    // `m` is the 3x3 row-major, the fade scale folded in; `tx/ty/tz` the
    // translation; `px/py/pz` how far it moved since the last frame, which the
    // motion vector needs and nothing else reads (see KIND_FLYER).
    //
    // NOTHING GOES UP THE BUS HERE. Unlike the tool and the shafts, which are
    // one instance each and upload themselves, a flock is up to sixty-four
    // instances that ALL move on every frame -- and sixty-four ten-byte-ish
    // writes into a mapped buffer is a hundred and twenty-eight driver calls a
    // frame to move six kilobytes. So these write the host's own copy and
    // flushFlyerInstances sends the whole band in one go, twice.
    // -----------------------------------------------------------------------
    void setFlyerInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                          float px, float py, float pz, bool show) {
        if (flyers_.empty() || flyerBase_ < 0 || slot < 0 || slot >= kFlyerInstances) return;
        const size_t idx = size_t(flyerBase_ + slot);
        if (idx >= instanceDescs_.size()) return;
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const bool ok = show && m && model >= 0 && model < int(flyers_.size());

        RtInstanceDesc &inst = instanceDescs_[idx];
        writeTransform(inst, ok ? m : kI, tx, ty, tz);
        inst.instanceMask = ok ? kMaskWorld : 0;
        inst.instanceID = uint32_t(idx);
        const size_t mi = size_t(ok ? model : 0);
        inst.accelerationStructure = flyers_[mi].blas.as->getGpuAddress();
        instanceInfos_[idx].triOffset = flyers_[mi].tri;
        instanceInfos_[idx].prevOffset = float3(px, py, pz);
        flyersDirty_ = true;
    }

    // The whole band, in two writes. Called once a frame, before refitTlas.
    void flushFlyerInstances() {
        if (!flyersDirty_ || flyerBase_ < 0 || !instanceDescBuf_ || !instanceInfo_) return;
        flyersDirty_ = false;
        const size_t base = size_t(flyerBase_);
        const size_t n = std::min(size_t(kFlyerInstances), instanceDescs_.size() - base);
        if (n == 0) return;
        ctx_->updateBuffer(instanceDescBuf_.get(), &instanceDescs_[base],
                           base * sizeof(RtInstanceDesc), n * sizeof(RtInstanceDesc));
        ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[base], base * sizeof(V6Instance),
                           n * sizeof(V6Instance));
    }

    // -----------------------------------------------------------------------
    // Where the tool is this frame, and a REFIT rather than a rebuild.
    //
    // `m` is the 3x3 in row-major order, scale included; `tx/ty/tz` the
    // translation. `show` false hides it by zeroing the instance mask, which is
    // how a hidden tool costs nothing: the traversal rejects it before it looks
    // at the geometry, and the slot stays in the structure so the refit below
    // still has the same instance count it was built with.
    //
    // A REFIT, AND THAT IS THE WHOLE REASON THIS IS AFFORDABLE. Rebuilding the
    // top-level structure costs 1.15 ms on this scene -- measured, over 31
    // rebuilds at 42 000 instances -- against a 3.28 ms median frame, so doing
    // it every frame to move one 19-voxel model would have been a third of the
    // frame rate. An update reuses the tree and only moves the box that
    // changed, which is exactly the case it is designed for: one instance
    // moving a little, every other one standing still.
    //
    // AND ONLY 64 BYTES GO UP THE BUS. The held item is instance ZERO, always,
    // so its descriptor sits at offset 0 and the other forty-two thousand are
    // left alone. Uploading the whole array every frame would have cost more
    // than the refit.
    // -----------------------------------------------------------------------
    void setHeldInstance(int model, const float *m, float tx, float ty, float tz, bool show) {
        if (held_.empty() || !tlas_ || instanceDescs_.empty()) return;
        if (model < 0 || model >= int(held_.size())) show = false;

        RtInstanceDesc &inst = instanceDescs_[0];
        writeTransform(inst, m, tx, ty, tz);
        inst.instanceMask = show ? kMaskHeld : 0;
        inst.instanceID = 0;
        // WHICH MODEL IS IN THE SLOT, AND WHY AN UPDATE MAY CHANGE IT. A
        // top-level update is allowed to rewrite the instance descriptors; what
        // it may not change is how MANY there are. So swapping tools -- and the
        // bow stepping through its draw frames -- is the same 64-byte write as
        // moving the tool, and costs the same refit. The instance's triOffset
        // lives in a separate buffer that the structure never reads, so it is
        // updated alongside rather than through the build.
        if (show && model != heldModel_) {
            heldModel_ = model;
            instanceInfos_[0].triOffset = held_[size_t(model)].tri;
            ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[0], 0, sizeof(V6Instance));
        }
        inst.accelerationStructure =
            held_[size_t(model >= 0 && model < int(held_.size()) ? model : 0)]
                .blas.as->getGpuAddress();
        ctx_->updateBuffer(instanceDescBuf_.get(), &inst, 0, sizeof(RtInstanceDesc));
    }

    // -----------------------------------------------------------------------
    // One shaft, in the slot it was given. `m` null hides it.
    //
    // The arrow slots follow the held one, so slot i is instance 1 + i and its
    // descriptor is at a fixed offset like the tool's. Same 64-byte write, same
    // refit -- see setHeldInstance for why that is what makes any of this
    // affordable.
    // -----------------------------------------------------------------------
    void setArrowInstance(int slot, int model, const float *m, float tx, float ty, float tz,
                          bool show) {
        if (held_.empty() || !tlas_ || slot < 0 || slot >= kArrowInstances) return;
        const size_t idx = size_t(1 + slot);
        if (idx >= instanceDescs_.size()) return;
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};

        RtInstanceDesc &inst = instanceDescs_[idx];
        writeTransform(inst, m ? m : kI, tx, ty, tz);
        inst.instanceMask = (show && m) ? kMaskWorld : 0;
        inst.instanceID = uint32_t(idx);
        if (model >= 0 && model < int(held_.size())) {
            inst.accelerationStructure = held_[size_t(model)].blas.as->getGpuAddress();
            if (instanceInfos_[idx].triOffset != held_[size_t(model)].tri) {
                instanceInfos_[idx].triOffset = held_[size_t(model)].tri;
                ctx_->updateBuffer(instanceInfo_.get(), &instanceInfos_[idx],
                                   idx * sizeof(V6Instance), sizeof(V6Instance));
            }
        }
        ctx_->updateBuffer(instanceDescBuf_.get(), &inst, idx * sizeof(RtInstanceDesc),
                           sizeof(RtInstanceDesc));
    }

    // -----------------------------------------------------------------------
    // Fold this frame's writes into the structure.
    //
    // ONE UPDATE FOR ALL OF THEM. The tool and every shaft are written first
    // and the tree is walked once afterwards; refitting per instance would pay
    // the traversal thirteen times for the same frame.
    // -----------------------------------------------------------------------
    void refitTlas() {
        // EITHER MOVER IS ENOUGH. This used to ask only whether a tool was
        // loaded, which was the whole truth while the tool was the only thing
        // in this world that moved. `--noaxe` with a flock in the air is now a
        // world with nothing in the hand and sixty-four instances that move
        // every frame, and refusing the refit there would have left every
        // butterfly frozen at the origin.
        if ((held_.empty() && flyers_.empty()) || !tlas_ || instanceDescs_.empty() ||
            !tlasUpdateScratch_)
            return;

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace |
                       RtAccelerationStructureBuildFlags::AllowUpdate |
                       RtAccelerationStructureBuildFlags::PerformUpdate;
        inputs.descCount = uint32_t(instanceDescs_.size());
        inputs.instanceDescs = instanceDescBuf_->getGpuAddress();

        RtAccelerationStructure::BuildDesc bd = {};
        bd.inputs = inputs;
        // IN PLACE: source and destination are the same structure, which the
        // API allows for an update and which is what makes this cost one pass
        // over the tree rather than a copy of it.
        bd.source = tlas_.get();
        bd.dest = tlas_.get();
        bd.scratchData = tlasUpdateScratch_->getGpuAddress();
        ctx_->buildAccelerationStructure(bd, 0, nullptr);
        ctx_->uavBarrier(tlasBuffer_.get());
    }

    // -----------------------------------------------------------------------
    // The palette, as the shader's material table.
    //
    // A SEPARATE FUNCTION BECAUSE THE TABLE CAN GAIN ENTRIES AFTER init(). Every
    // model registers its colours through Palette::forModelColor as it loads,
    // and until the axe there was nothing that loaded outside this class -- so
    // the upload sat inline at the end of init() and the table was final the
    // moment it ran. render/helditem.h registers the held model's colours the
    // same way and then asks for this, which is the whole of what it needs from
    // the world: the tool indexes the same table as every rock, so it is shaded
    // by the same materials rather than by a set of its own.
    //
    // The palette is a fixed 255 entries whose fields line up one for one with
    // the shader's material record, but they are separate structs on purpose:
    // MaterialLook belongs to the world builder and V6Material to the pipeline,
    // and the day one of them gains a field the other does not need, this loop
    // is the only thing that has to know.
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
        materials_->setName("v2::materials");
    }

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
        vertPool_.init(device_, ResourceBindFlags::ShaderResource, "v2::blasVerts");
        idxPool_.init(device_, ResourceBindFlags::ShaderResource, "v2::blasIndices");
        scratchPool_.init(device_, ResourceBindFlags::UnorderedAccess, "v2::blasScratch");
        rawPool_.init(device_, ResourceBindFlags::AccelerationStructure, "v2::blasRaw");

        // Templates first: the terrain's grass and soil colours are sampled
        // from the palette the models bring with them.
        if (!loadPines()) return false;
        // The ground's colours come from the PINES; everything after this line
        // is rock and flower, and neither is a soil.
        palette.markPinesLoaded();
        loadRocks();
        loadFlowers();
        loadMushrooms();
        loadPinecones();
        loadHives();
        palette.deriveGroundFromTrees();
        buildWater();

        uploadMaterials();

        mesher_.seed = seed;
        mesher_.treeDensity = treeDensity;
        mesher_.rockDensity = rockDensity;
        mesher_.flowerDensity = flowerDensity;
        mesher_.mushroomDensity = mushroomDensity;
        for (const ModelTemplate &t : pines_)
            mesher_.pineFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : rocks_)
            mesher_.rockFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : flowers_)
            mesher_.flowerFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : mushrooms_)
            mesher_.mushroomFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : pinecones_)
            mesher_.pineconeFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : hives_)
            mesher_.hiveFoot.push_back({t.sx, t.sz, t.sy, t.col.baseX, t.col.baseZ});
        for (const ModelTemplate &t : pines_) mesher_.pinePerch.push_back(t.perches);
        for (const ModelTemplate &t : pines_) mesher_.pineHivePerch.push_back(t.hivePerches);
        mesher_.pineconesPerTree = pineconesPerTree;
        mesher_.mushroomBig0 = mushroomBig0;

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

    std::vector<ModelTemplate> pines_, rocks_, flowers_, mushrooms_, pinecones_;
    // The beehives. One model, and only the birch wood plants it -- see
    // loadHives and the hive pass in scene/chunks.h.
    std::vector<ModelTemplate> hives_;
    Blas waterBlas_;
    uint32_t waterTriOffset_ = TriPool::kInvalid;

    std::map<long long, Chunk> chunks_;
    std::map<long long, int> wanted_;
    std::set<long long> requested_;

    ref<Buffer> materials_, instanceInfo_, instanceDescBuf_, tlasBuffer_, tlasScratch_;
    ref<RtAccelerationStructure> tlas_;
    std::vector<RtInstanceDesc> instanceDescs_;
    // EVERY tool that can be in the hand: one bottom-level structure each, its
    // slice of the triangle pool, and the grid it was meshed from. They share a
    // single top-level slot -- see setHeldInstance -- because only one of them
    // is ever held at a time, and an instance count that changed with the
    // selection would cost a full rebuild on every scroll of the wheel.
    struct HeldModel {
        Blas blas;
        uint32_t tri = TriPool::kInvalid;
        int sx = 0, sy = 0, sz = 0;
    };
    std::vector<HeldModel> held_;
    int heldModel_ = -1;  // which of them the slot currently points at
    // EVERY FRAME OF EVERY COLOUR the flock can wear: six colours of eight, so
    // forty-eight structures of ten voxels each. They are separate structures
    // rather than one model posed, because that is what a voxel animation IS --
    // see render/butterflies.h on why the flap is on the grid and the turn is
    // not. Which one a slot points at is a 64-byte write, exactly as swapping
    // the tool in the hand is.
    std::vector<HeldModel> flyers_;
    int flyerBase_ = -1;        // first instance of the band, -1 while unbuilt
    bool flyersDirty_ = false;  // anything written since the last flush
    ref<Buffer> tlasUpdateScratch_;
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
        b.buffer->setName("v2::blas");
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
                      bool multiModel, bool quiet, uint32_t mossSeed = 0,
                      bool perches = false, int upscale = 0) {
        for (const std::string &path : paths) {
            std::vector<VoxModel> models;
            std::string err;
            if (multiModel) {
                if (!voxLoadAll(path, &models, &err)) {
                    if (!quiet) std::fprintf(stderr, "v2: %s\n", err.c_str());
                    continue;
                }
            } else {
                VoxModel mo;
                if (!voxLoad(path, &mo, &err)) {
                    if (!quiet) std::fprintf(stderr, "v2: %s\n", err.c_str());
                    continue;
                }
                models.push_back(std::move(mo));
            }

            for (const VoxModel &mo : models) {
                VoxAsset a = toWorld(mo, 0, mo.sx);
                if (a.sx <= 0) continue;
                // A COUNT, NOT A FLAG. Each pass is 2x on a side, so 8x the
                // voxels and about 4x the surface a mesher has to emit. One
                // pass is the large mushrooms and the mid stones; two is the
                // big five, which are meant to read as landmarks rather than
                // as rocks you walk past.
                for (int u = 0; u < upscale; ++u) a = upscale2x(a);

                // ONLY THE ENTRIES THE MODEL USES. Registering all 255 of a
                // file's palette floods the shared table, and past 255
                // forModelColor returns AIR and real voxels stop being drawn.
                std::vector<uint8_t> idOfEntry(256, mat::AIR);
                std::vector<bool> used(256, false);
                for (uint8_t v : a.a) used[v] = true;
                for (int e = 1; e <= 255; ++e)
                    if (used[e]) idOfEntry[e] = palette.forModelColor(mo.pal[e - 1]);

                // The moss goes into the ASSET, before anything reads it, so
                // the mesh and the collider below cannot disagree about where
                // the rock's surface is.
                growMoss(&a, &idOfEntry, mossSeed);

                VoxMesh mesh = meshAsset(a, idOfEntry, VOXEL_M);
                if (mesh.triCount() == 0) continue;

                ModelTemplate t;
                if (perches) {
                    t.perches = collectPerches(a, idOfEntry);
                    // 5 for the 5 x 5 x 5 beehive. Only the birches carry them,
                    // and they are the models loaded at or past birchBase --
                    // out_ is being appended to, so its CURRENT size is the
                    // index this model is about to take.
                    if (int(out->size()) >= mesher_.birchBase && &pines_ == out)
                        t.hivePerches = collectPerches(a, idOfEntry, 5);
                }
                t.colTop = columnTops(a, idOfEntry);
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

    // ------------------------------------------------------------------------
    // THE TREES, WHICHEVER WOOD THIS IS.
    //
    // Birches load into pines_ and that is not a shortcut -- it is the point.
    // Everything downstream of this vector is model agnostic: the scatter picks
    // an index, the collider is measured off the asset, the perch finder walks
    // the voxels looking for somewhere a thing could hang. None of it knows
    // what species it is holding, so a second vector would need a second copy
    // of all of it to say the same thing twice.
    //
    // perches=true for both. In a pine those anchors carry cones; in a birch
    // they carry the beehives -- same question asked of the crown ("a solid
    // voxel with air under it"), different thing hung from the answer.
    // ------------------------------------------------------------------------
    bool loadPines() {
        // ONE VECTOR, TWO RANGES. Pines occupy [0, birchBase) and birches
        // [birchBase, size) -- so the scatter picks a SPECIES by choosing which
        // range to index, and everything downstream stays exactly as it was.
        // templateFor, the footprints, the colliders and the perch lists are
        // all positional into this one array and none of them needs to know a
        // species exists.
        //
        // Both are loaded whenever the bands are live, because a chunk near a
        // seam contains both. --birch and --pine pin the world to one, and then
        // only that one is worth the load time and the triangles.
        std::vector<std::string> paths;
        const bool wantPine = !terrain.forced || terrain.biome == Biome::Pine;
        const bool wantBirch = !terrain.forced || terrain.biome == Biome::Birch;
        if (wantPine)
            for (int i = 1; i <= 9; ++i)
                paths.push_back(pineDir + "/pine_" + std::to_string(i) + ".vox");
        mesher_.birchBase = wantPine ? 9 : 0;
        if (wantBirch)
            // 1.vox .. 16.vox, authored at 10 cm like everything else -- 18.2 m
            // to 30.5 m, the tallest a 100 ft tree exactly. The set was scaled by
            // ONE factor so the saplings stayed saplings in proportion, which makes
            // the spread wide: the shortest birch is barely half the tallest and
            // stands well under the pines, every one of which is 30.5 m.
            //
            // Anything past 25.5 m is TWO STACKED MODELS in one file, that being
            // the tallest a .vox coordinate byte can address -- see the scene-graph
            // note at the top of scene/vox.h, and tools/revoxel_trees_tall.py,
            // which writes them.
            for (int i = 1; i <= 16; ++i)
                paths.push_back(birchDir + "/" + std::to_string(i) + ".vox");
        loadModelSet(paths, &pines_, false, false, 0u, /*perches=*/true);
        if (pines_.empty()) {
            std::fprintf(stderr, "v2: no pine models loaded from %s -- pass --pines\n",
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
        // THE ORDER OF THESE TWO LISTS IS LOAD-BEARING. decorSink in
        // scene/collide.h decides how deep a rock sits from its INDEX -- big
        // below kRockBigEnd, mid below kRockBigMidEnd -- so the big five have
        // to land at 0..4 and the mid six at 5..10. loadModelSet appends, so
        // two calls in this order give exactly that.
        static const char *kBigNames[] = {
            "BIG_1_BiG_0", "Big_2_BiG_0", "Big_3_BiG_0", "Big_4_BiG_0", "Big_5_BiG_0"};
        // THE MID SIX ARE THEIR OWN LIST NOW, purely so they can be loaded at a
        // different scale from the ones after them. The ORDER and the POSITION
        // are unchanged and must stay that way: decorSink reads the index, so
        // mid still has to occupy 5..10 with Runic_1 landing at 11.
        static const char *kMidNames[] = {"Mid_1_MID_0",     "Mid_2_MID_0", "Mid_3_MID_0",
                                          "Mid_4_MID_0",     "Mid_4_MID_0_001", "Mid_5_MID_0"};
        static const char *kRestNames[] = {
            "Runic_1_Runic_0", "Runic_2_Runic_0",
            "Runic_3_Runic_0", "Runic_4_Runic_0", "Runic_5_Runic_0", "Runic_6_Runic_0",
            "Runic_7_Runic_0", "Small_1_SMall_0", "Small_2_SMall_0", "Small_3_SMall_0",
            "Small_4_SMall_0", "Small_5_SMall_0", "Small_6_SMall_0", "Small_7_SMall_0",
            "Small_8_SMall_0"};

        std::vector<std::string> big, mid, rest;
        for (const char *n : kBigNames) big.push_back(decorDir + "/rocks/" + n + ".vox");
        for (const char *n : kMidNames) mid.push_back(decorDir + "/rocks/" + n + ".vox");
        for (const char *n : kRestNames) rest.push_back(decorDir + "/rocks/" + n + ".vox");

        // NOTHING IS UPSCALED HERE ANY MORE, and that is the point.
        //
        // Big and mid used to be grown with upscale2x -- 4x and 2x on a side --
        // which is not the same thing as making them bigger, however much it
        // looks like it. That call replaces each voxel with a block of copies
        // of itself: the mesh stays at 10 cm, but the SHAPE still only has the
        // detail the source had, so at 4x every surface feature was 40 cm and
        // the boulders were visibly built from blocks four times the size of
        // the terrain they stood on.
        //
        // They are now revoxelised from the sculpt instead, by
        // tools/revoxel_rocks_scaled.py, which samples the SAME mesh on a
        // finer grid and ships the result. The detail was always in the .glb;
        // it was the .vox that was too coarse to carry it.
        //
        //     big  122-228 voxels tall  (12.2-22.8 m)  from a 4x sampling
        //     mid   57- 77              ( 5.7- 7.7 m)  from a 2x sampling
        //     rest   7- 23              ( 0.7- 2.3 m)  untouched
        //
        // Two of the big five are sampled slightly under 4x -- Big_2 at 3.42
        // and Big_5 at 3.78 -- because a .vox XYZI record packs each coordinate
        // in a single byte and no axis may exceed 255. That is a limit of the
        // file format, and the tool clamps to it rather than silently wrapping.
        //
        // The only model set that grows moss -- see mossFace in voxelworld.h.
        loadModelSet(big, &rocks_, false, false, 0x4D055EEDu);
        loadModelSet(mid, &rocks_, false, false, 0x4D055EEDu);
        loadModelSet(rest, &rocks_, false, false, 0x4D055EEDu);
        loadedRocks = int(rocks_.size());
    }

    void loadFlowers() {
        loadModelSet({decorDir + "/flowers.vox"}, &flowers_, true, false);
        loadedFlowers = int(flowers_.size());
    }

    // TWO SETS FROM ONE FILE: the models as authored, then the same models
    // revoxelised at 2x. The small ones come first and mushroomBig0 is the
    // boundary, so the scatter can weight the draw between them.
    void loadMushrooms() {
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false);
        mushroomBig0 = int(mushrooms_.size());
        loadModelSet({decorDir + "/mushroom.vox"}, &mushrooms_, true, false, 0u, false,
                     /*upscale=*/1);
        loadedMushrooms = int(mushrooms_.size());
    }

    // The beehive. Birch only -- see the hive pass in scene/chunks.h, which
    // hangs one in a hundredth of the trees.
    void loadHives() {
        if (!terrain.birch()) return;
        loadModelSet({decorDir + "/beehive.vox"}, &hives_, false, true);
    }

    void loadPinecones() {
        loadModelSet({decorDir + "/pinecone.vox"}, &pinecones_, true, false);
        loadedPinecones = int(pinecones_.size());
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
                if (p.kind >= 0 && p.kind < 6) ++c.decorKind[p.kind];
                // A HIVE IS NOT WALKED INTO EITHER. It hangs several metres up
                // in a crown, so a ground collider for it would be an invisible
                // wall under the tree.
                const bool walkThrough = (p.kind == 2 || p.kind == 4 || p.kind == 5);
                c.decorDesc.push_back(makeInstance(p, &info, walkThrough ? nullptr : &s));
                c.decorInfo.push_back(info);
                if (!walkThrough && s.hx > 0.0f) c.solids.push_back(s);
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
            (kind == 0)   ? pines_
            : (kind == 1) ? rocks_
            : (kind == 2) ? flowers_
            : (kind == 3) ? mushrooms_
            : (kind == 5) ? hives_
                          : pinecones_;
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
        // it is sunk half a metre. Rocks are sunk by category rather than by
        // height -- the big and mid boulders a metre and a half, everything
        // else half a metre -- so that a boulder reads as embedded in the
        // ground rather than set down on it. See decorSink in scene/collide.h,
        // which the scatter shares so the two cannot disagree.
        const int sink = decorSink(p.kind, p.index, t.sy, seed, p.cell);

        const float tx = float(p.ci) * VOXEL_M - halfOf(fx);
        const float tz = float(p.cj) * VOXEL_M - halfOf(fz);
        // yOff is ZERO for everything that stands on the ground and is the
        // whole story for a pinecone, which does not: it is the height of the
        // branch the cone was perched on, measured from the tree's own base, so
        // the cone rides the tree rather than the terrain under it.
        // extraSink is what the SCATTER measured for this particular site --
        // how much further down this model has to go before its underside stops
        // showing daylight over the ground it spans. Zero on flat ground, and
        // zero for a pinecone, which is hung on a branch and never touches the
        // terrain at all. See Placement in scene/chunks.h for why it is carried
        // here rather than recomputed: this function has no height field, and
        // the collider below is derived from this same transform, so measuring
        // it once is also what stops the two disagreeing.
        const float ty = float(p.h + 1 - sink - p.extraSink + p.yOff) * VOXEL_M;

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
            // Rocks and mushrooms are floors; a trunk is a wall whose top is a
            // canopy twenty metres up and must never be stood on.
            solidOut->standable = (p.kind == 1 || p.kind == 3);
            solidOut->bouncy = (p.kind == 3);

            // The voxel-accurate surface. `col` is borrowed from the template,
            // which lives for the run; the three numbers beside it are the
            // instance transform this function just wrote, kept so the collider
            // can invert it rather than reconstructing the placement from
            // scratch and drifting away from it.
            if (!t.colTop.empty()) {
                solidOut->col = t.colTop.data();
                solidOut->msx = int16_t(t.sx);
                solidOut->msz = int16_t(t.sz);
                solidOut->yaw = uint8_t(p.yaw & 3);
                solidOut->tx = inst.transform[0][3];
                solidOut->tz = inst.transform[2][3];
                solidOut->baseY = inst.transform[1][3];
            }
        }

        info->triOffset = t.triOffset;
        info->kind = (p.kind == 0) ? KIND_TREE : KIND_TERRAIN;
        info->tint = (p.kind == 0) ? tintFor(p.cell) : float3(1.0f, 1.0f, 1.0f);
        // A tree does not move, so the motion vector's static-world formula is
        // exactly right for it and there is nothing to subtract.
        info->prevOffset = float3(0.0f, 0.0f, 0.0f);

        inst.instanceMask = kMaskWorld;
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

        // -- THE HELD ITEM IS INSTANCE ZERO, ALWAYS ------------------------
        //
        // First, before the water, and present on every rebuild from the moment
        // the model loads -- whether or not anything is in the hand. Two things
        // depend on that and both are in setHeldInstance: its descriptor is at
        // a FIXED offset, so moving it is a 64-byte write rather than a 2.7 MB
        // one; and the instance COUNT never changes, which is what an update is
        // allowed to assume. A slot that came and went would force a full
        // rebuild on every draw and put away.
        //
        // The transform is identity here and is overwritten before the first
        // trace that could see it. The mask is 0 so it cannot be hit in the
        // meantime.
        if (!held_.empty()) {
            const size_t m = size_t(heldModel_ >= 0 && heldModel_ < int(held_.size()) ? heldModel_
                                                                                      : 0);
            RtInstanceDesc held = {};
            writeTransform(held, kI, 0.0f, 0.0f, 0.0f);
            held.instanceMask = 0;
            held.accelerationStructure = held_[m].blas.as->getGpuAddress();
            V6Instance info{};
            info.triOffset = held_[m].tri;
            info.kind = KIND_HELD;
            info.tint = float3(1.0f, 1.0f, 1.0f);
            push(held, info);

            // -- AND THE SHAFTS, RESERVED ----------------------------------
            //
            // Every arrow slot, present from this build on and masked off until
            // something is actually in it. The count is what an update is not
            // allowed to change, so a pool that grew with the shooting would
            // force a full rebuild on every loose and every landing -- the two
            // moments in the frame least able to afford one.
            //
            // KIND_TERRAIN, not KIND_HELD: a shaft in the air is an ordinary
            // object in the world. It is lit like one, it casts like one, and
            // -- unlike the tool -- the motion vector formula is exactly right
            // for it, because it really does move through a world the camera is
            // looking at from outside.
            for (int i = 0; i < kArrowInstances; ++i) {
                RtInstanceDesc arrow = {};
                writeTransform(arrow, kI, 0.0f, 0.0f, 0.0f);
                arrow.instanceMask = 0;
                arrow.accelerationStructure = held_[m].blas.as->getGpuAddress();
                V6Instance ai{};
                ai.triOffset = held_[m].tri;
                ai.kind = KIND_TERRAIN;
                ai.tint = float3(1.0f, 1.0f, 1.0f);
                push(arrow, ai);
            }
        }

        // -- AND THE FLOCK, RESERVED THE SAME WAY -------------------------
        //
        // AFTER the hand and its shafts and BEFORE the water, so the band is
        // contiguous and its base is the only thing setFlyerInstance has to
        // know. The base is recorded rather than computed because it depends on
        // whether anything is in the hand at all -- a `--noaxe` run puts the
        // first butterfly at instance zero -- and the two loads can happen in
        // either order.
        flyerBase_ = -1;
        if (!flyers_.empty()) {
            flyerBase_ = int(instanceDescs_.size());
            for (int i = 0; i < kFlyerInstances; ++i) {
                RtInstanceDesc fly = {};
                writeTransform(fly, kI, 0.0f, 0.0f, 0.0f);
                fly.instanceMask = 0;
                fly.accelerationStructure = flyers_[0].blas.as->getGpuAddress();
                V6Instance fi{};
                fi.triOffset = flyers_[0].tri;
                fi.kind = KIND_FLYER;
                fi.tint = float3(1.0f, 1.0f, 1.0f);
                push(fly, fi);
            }
            // Whatever the flock had written into the old band is gone with it,
            // so the next frame's publish has to go up whether or not a
            // butterfly moved. Without this a rebuild -- which is to say every
            // time the ring steps -- would leave the whole flock parked at the
            // origin, masked off, until something happened to touch it.
            flyersDirty_ = true;
        }

        {
            RtInstanceDesc water = {};
            writeTransform(water, kI, 0.0f, 0.0f, 0.0f);
            water.instanceMask = kMaskWorld;
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
            ci.instanceMask = kMaskWorld;
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
                     "v2::instanceDescs");
        ctx_->updateBuffer(instanceDescBuf_.get(), instanceDescs_.data(), 0,
                           n * sizeof(RtInstanceDesc));

        if (!instanceInfo_ || instanceInfo_->getElementCount() < n) {
            instanceInfo_ = device_->createStructuredBuffer(
                sizeof(V6Instance), std::max(n, 1u), ResourceBindFlags::ShaderResource,
                Falcor::MemoryType::DeviceLocal);
            instanceInfo_->setName("v2::instances");
        }
        if (n > 0)
            ctx_->updateBuffer(instanceInfo_.get(), instanceInfos_.data(), 0,
                               n * sizeof(V6Instance));

        RtAccelerationStructureBuildInputs inputs = {};
        inputs.kind = RtAccelerationStructureKind::TopLevel;
        // No compaction here, unlike the bottom-level structures: this is
        // rebuilt every time the ring moves, and the compaction pass costs a
        // second flush for a structure that is a few megabytes at most.
        //
        // ALLOW_UPDATE, so the held item can be moved with a refit instead of a
        // rebuild -- see setHeldInstance for the measurement that makes that
        // the difference between affordable and not. It is asked for only when
        // there is something to move: the flag costs a little build time and a
        // little traversal speed on a structure that would otherwise never be
        // updated, and a world with nothing in the hand should not pay it.
        inputs.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        if (!held_.empty() || !flyers_.empty())
            inputs.flags = inputs.flags | RtAccelerationStructureBuildFlags::AllowUpdate;
        inputs.descCount = n;
        inputs.instanceDescs = instanceDescBuf_->getGpuAddress();

        const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), inputs);
        ensureBuffer(tlasScratch_, pre.scratchDataSize, ResourceBindFlags::UnorderedAccess,
                     "v2::tlasScratch");
        // Its own buffer rather than sharing the build's: an update is issued
        // in the middle of a frame that may also have rebuilt, and the driver
        // is entitled to still be reading the build scratch.
        // ...and EITHER MOVER, matching the flag above it. This asked only
        // about the hand, which was the whole truth until something else in
        // this world moved: an --out render loads no viewmodel at all, so a
        // flock published into a structure with no update scratch was a refit
        // that returned at its first line and sixty-four butterflies that were
        // never in the picture.
        if (!held_.empty() || !flyers_.empty())
            ensureBuffer(tlasUpdateScratch_, pre.updateScratchDataSize,
                         ResourceBindFlags::UnorderedAccess, "v2::tlasUpdateScratch");

        // The structure object wraps a buffer and does not own it, so a grown
        // buffer means a new object as well.
        if (!tlasBuffer_ || tlasBuffer_->getSize() < pre.resultDataMaxSize) {
            tlasBuffer_ = device_->createBuffer(pre.resultDataMaxSize,
                                                ResourceBindFlags::AccelerationStructure,
                                                Falcor::MemoryType::DeviceLocal);
            tlasBuffer_->setName("v2::tlas");
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

}  // namespace v2
