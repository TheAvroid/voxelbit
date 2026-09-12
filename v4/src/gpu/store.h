// ---------------------------------------------------------------------------
// store.h -- the host half of the seam. See shaders/Store.slang for the other.
//
// A STORE OWNS EVERYTHING ABOUT GEOMETRY AND NOTHING ELSE. It holds the voxels
// however it likes, uploads them however it likes, and answers rays through the
// two functions in its .slang file. The renderer knows five verbs:
//
//     build / shutdown     lifecycle
//     update(camPos)       a chance to stream, page or rebuild
//     bind(var)            put EVERYTHING the shader needs on this root var
//     addDefines(defs)     select the implementation, and any of its options
//
// bind() TAKES THE CONSTANTS TOO, AND THAT IS THE WHOLE DESIGN DECISION.
//
// The engine this was cut from had the renderer building the store's constant
// buffer by hand at three separate call sites -- a struct in the shared header,
// a setBlob per pass, and a field layout the tracer had to know. Every one of
// those is a place a new backend would have to edit code that has nothing to do
// with it. Here a store binds its own resources and its own constants in one
// call, so a backend with a wildly different shape -- a BLAS of brick AABBs, a
// dense bitmask, an SVO -- is a new pair of files and no edits at all.
//
// ---------------------------------------------------------------------------
// build() IS HANDED THE PALETTE, AND THAT IS THE ONE ADDITION TO THE CONTRACT.
//
// A store AUTHORS ITS OWN CONTENT -- there is no world generator on the other
// side of the seam to do it -- and content that has colours has to mint them
// somewhere. The palette stays on the renderer's side, as it always did, and
// what crosses back is still nothing but a material id; the store simply gets
// to say which ids exist before they are uploaded. See the note on the palette
// in gpu/world.h, and buildPalette in scene/generate.h.
//
// IT IS ALSO WHERE THE .vox ASSETS LOAD, because they are what the palette is
// built FROM: the ground's greens and browns are derived from the pines' own
// needles and bark, and the stone ramp from the boulders' own greys.
//
// ---------------------------------------------------------------------------
// ready() DOES NOT GATE RENDERING, AND MUST NOT BE MADE TO.
//
// It reports whether there is anything to hit, for the banner and the overlay.
// The trace runs regardless: a store with nothing in it simply misses, the sky
// is what comes back, and every pass downstream keeps running. Gating the
// dispatch on it -- which is what the old vdbOk() checks did -- means an engine
// with no backend draws black, and black is indistinguishable from a broken
// atmosphere, a broken exposure or a broken swapchain. This engine's whole
// premise is that you can look at the lighting with no world behind it.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <string>
#include <vector>

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/RenderContext.h"
#include "Core/API/RtAccelerationStructure.h"
#include "Core/Program/Program.h"
#include "Core/Program/ShaderVar.h"

#include "../core/vecmath.h"
#include "../scene/bricks.h"
#include "../scene/generate.h"
#include "../scene/stream.h"
#include "../scene/materials.h"

namespace v4 {

using Falcor::Buffer;
using Falcor::DefineList;
using Falcor::Device;
using Falcor::ref;
using Falcor::RenderContext;
using Falcor::ResourceBindFlags;
using Falcor::RtAccelerationStructure;
using Falcor::RtAccelerationStructureBuildFlags;
using Falcor::RtAccelerationStructureBuildInputs;
using Falcor::RtAccelerationStructureKind;
using Falcor::RtGeometryDesc;
using Falcor::RtGeometryFlags;
using Falcor::RtGeometryInstanceFlags;
using Falcor::RtGeometryType;
using Falcor::RtInstanceDesc;
using Falcor::ShaderVar;

// ---------------------------------------------------------------------------
// THE NULL STORE. Holds nothing, binds nothing, misses everything.
//
// It is the reference implementation of the interface, not a placeholder: it
// stays once real backends exist, because it is the zero every measurement of
// them is taken against. Profile this and you have the frame cost with the
// geometry query removed entirely -- ray setup, the lighting passes, the
// denoiser. A backend's true cost is its profile minus this one.
//
// COPY THIS CLASS TO MAKE A BACKEND. Keep the six verbs, change nothing about
// their signatures, and put the new .slang beside stores/Null.slang.
// ---------------------------------------------------------------------------
class NullStore {
  public:
    bool build(const ref<Device> &device, RenderContext *ctx, Palette &pal) {
        (void)device;
        (void)ctx;
        (void)pal;
        return true;
    }

    void shutdown() {}

    // The camera moved. A paged backend re-centres its window here; this one
    // has no window and no pages.
    void update(Vec3 camPos) { (void)camPos; }

    // EVERYTHING the shader side needs, on one root var: buffers, textures,
    // acceleration structures, constants. The null store needs none of it, and
    // an empty body here is the proof that the renderer is not secretly
    // binding something on a backend's behalf.
    void bind(const ShaderVar &var) const { (void)var; }

    // Which stores/*.slang the trace programs compile against. The quotes are
    // part of the value -- the shader does `#include V4_STORE_IMPL`, so the
    // define has to expand to a quoted path, and an unquoted one fails inside
    // the preprocessor with an error naming neither this file nor that one.
    void addDefines(DefineList &defs) const { defs.add("V4_STORE_IMPL", "\"stores/Null.slang\""); }

    // Informational ONLY. See the note at the top of this file: nothing in the
    // render path may branch on this.
    bool ready() const { return false; }

    const char *status() const { return "null -- no geometry, every ray misses"; }
    const char *models() const { return "none"; }

    // The host query half. Nothing to stand on, so there is no ground.
    float groundM(float x, float z) const {
        (void)x;
        (void)z;
        return 0.0f;
    }
};

// ---------------------------------------------------------------------------
// THE AABB STORE -- voxels through the ray tracing cores.
//
// One AABB per 8^3 brick, one bottom-level acceleration structure per 25.6 m
// chunk, one top-level structure over the lot with identity transforms. The
// hardware finds which bricks a ray crosses; stores/Aabb.slang walks the mask
// inside the one it was handed. See the long note at the top of that file for
// why the work is split there, and scene/bricks.h for the format.
//
// ---------------------------------------------------------------------------
// EVERYTHING IS BUILT ONCE, AT STARTUP, AND update() DOES NOTHING.
//
// That is a real limitation and it is stated rather than hidden. There is no
// streaming here: the world is a bounded square, generated whole, packed whole
// and handed to the device whole. What the design DOES leave ready is the thing
// streaming needs -- the chunk is already the acceleration-structure unit, so a
// changed 25.6 m cube is one bottom-level rebuild over a few thousand AABBs and
// a top-level refit, and nothing else in the world is touched. rebuildChunk()
// is the function that does not exist yet; the shape it would need is here.
// ---------------------------------------------------------------------------
class AabbStore {
  public:
    // What to build, and how to pack it. Filled from the command line before
    // build() -- see the store options in src/app.h.
    GenOptions gen;
    PackOptions pack;

    // -- ENDLESS, OR A BOX ---------------------------------------------------
    //
    // Streaming keeps a window of chunks around the player and slides it as
    // they move; see scene/stream.h. The fixed world is kept and is not a dead
    // option: it is what every measurement in this engine was taken against.
    // bench.bat builds one world once and photographs it from three pinned
    // cameras, and a world that rebuilds itself underneath that is not a
    // benchmark. --no-stream.
    bool stream = true;
    int streamRadius = 4;  // chunks each way: 9x9, 230 m across
    float spawnX = 0.0f, spawnZ = 0.0f;

    uint32_t debugMode = 0;
    // Cap the in-brick walk at this many steps; 0 means kBrickMaxSteps, the
    // real bound. Below it the picture is wrong on purpose -- see the note in
    // stores/Aabb.slang. --max-steps.
    uint32_t maxSteps = 0;
    // Reject a candidate brick on its octant byte before reading a mask word.
    //
    // OFF, AND MEASURED OFF. It sounds obviously good and it is not: 3.20 ms
    // against the flat walk's 2.71, twice, on a quiet machine. Candidates
    // mostly HIT -- the tight AABB already made the box small enough that a ray
    // entering one usually finds a voxel within a step or two -- so the
    // rejection almost never fires and all it adds is its own setup. Nesting
    // the cell walk inside an octant walk instead was worse still, at 3.51.
    //
    // Kept, and kept switchable, because the octant byte costs nothing (it was
    // pad) and because a denser or differently-shaped world could flip the
    // answer. --octant-skip turns it on.
    bool octantSkip = false;

    bool build(const ref<Device> &device, RenderContext *ctx, Palette &pal) {
        device_ = device;
        ctx_ = ctx;

        const auto t0 = std::chrono::steady_clock::now();
        // THE PALETTE IS BUILT FROM THE MODELS AND THE MODELS ARE LOADED HERE,
        // before a single voxel of terrain is written -- the ground's greens
        // and browns are derived from the PINES' own colours, so the trees have
        // to exist first. See buildPalette in scene/generate.h.
        content_ = buildPalette(pal, gen);
        if (stream) {
            // THE WINDOW IS STARTED WHERE THE PLAYER WILL BE, not at the
            // origin: the spawn is a command-line position, and a window built
            // around (0,0) would be thrown away on the first frame.
            stream_.configure(streamRadius);
            stream_.start(gen, content_, spawnX, spawnZ);
        } else {
            generate(world_, content_, gen);
        }
        const Content &content = content_;
        const double genMs = msSince(t0);

        const auto t1 = std::chrono::steady_clock::now();
        packStoreInto(packed_, live(), pack, &packCache_, nullptr);
        const PackedStore &p = packed_;
        const double packMs = msSince(t1);

        const auto t2 = std::chrono::steady_clock::now();
        upload(p);
        buildStructures(p);
        const double asMs = msSince(t2);

        bricks_ = uint32_t(p.brick.size());
        chunks_ = uint32_t(p.chunk.size());
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "dxr aabb -- %.2f M voxels, %.2f M reachable (%.0f%%), %u bricks "
                      "(%llu uniform boxes over %llu merged bricks, %llu detail, %llu dropped), "
                      "aabb fill %.0f%%, %u blas, "
                      "%.1f MB data + %.1f MB structure, gen %.0f / pack %.0f / build %.0f ms",
                      double(p.voxels) * 1e-6, double(p.kept) * 1e-6,
                      100.0 * double(p.kept) / double(p.voxels ? p.voxels : 1), bricks_,
                      (unsigned long long)p.uniform, (unsigned long long)p.merged,
                      (unsigned long long)p.detail, (unsigned long long)p.hidden, 100.0 * p.fill,
                      chunks_,
                      double(p.bytes()) / (1024.0 * 1024.0),
                      double(asBytes_) / (1024.0 * 1024.0), genMs, packMs, asMs);
        status_ = buf;
        // THE ASSETS GET THEIR OWN LINE. "9 pines, 1 rocks, 1 mushrooms, 6
        // flowers" is the difference between the authored wood and the
        // procedural fallback, and it is the first thing to check when the
        // trees look wrong.
        models_ = content.status;
        return true;
    }

    // A std::thread that is still JOINABLE when it is destroyed calls
    // std::terminate -- and a finished thread stays joinable until someone
    // joins it, so "the work is done" is not enough. This is the backstop for
    // every exit that does not go through shutdown().
    ~AabbStore() {
        if (worker_.joinable()) worker_.join();
    }

    void shutdown() {
        // A worker mid-pack holds a reference to the world it is reading.
        if (worker_.joinable()) worker_.join();
        blas_.clear();
        tlas_ = nullptr;
        blasBuf_ = nullptr;
        tlasBuf_ = nullptr;
        scratch_ = nullptr;
        aabbBuf_ = nullptr;
        brickBuf_ = nullptr;
        maskBuf_ = nullptr;
        instBuf_ = nullptr;
        world_.clear();
    }

    // -----------------------------------------------------------------------
    // FOLLOW THE PLAYER, ON A WORKER THREAD, IN TWO HANDOVERS.
    //
    // Called once a frame. Crossing a chunk boundary used to cost 2.4 seconds
    // on the render thread; the work is the same work, but almost none of it
    // has to happen where the frame can see it:
    //
    //   PREPARE   evaluate the noise for the strip of ground that came into
    //             view and build the chunks that entered, into worlds of their
    //             own. Touches nothing the walker or the packer is reading, so
    //             it runs on the worker.
    //   COMMIT    erase what left, take in what arrived, republish the columns.
    //             This is the only step that modifies the world anyone else is
    //             reading, so it happens HERE, between frames. It is the short
    //             one -- a walk of hash-map nodes.
    //   PACK      lay out the device buffers. Reads the world and writes only
    //             its own output, so it goes back to the worker.
    //   UPLOAD    the buffers and the acceleration structures. D3D12, so it is
    //             the render thread's by definition.
    //
    // WHAT IS ON SCREEN STAYS WHOLE THROUGHOUT. The old window is still
    // uploaded and still complete until the new buffers land, so a crossing
    // shows up as ground appearing a few frames later rather than as anything
    // missing.
    //
    // ONE CROSSING AT A TIME. Move far enough while a pack is in flight and the
    // window simply lags; the next update picks it up. A second prepare on top
    // of an unread one would be building from columns that are about to move.
    // -----------------------------------------------------------------------
    void update(Vec3 camPos) {
        if (!stream || !stream_.started() || !device_) return;

        if (phase_ == Phase::Idle) {
            if (!stream_.wouldMove(camPos.x, camPos.z)) return;
            const float cx = camPos.x, cz = camPos.z;
            done_.store(false, std::memory_order_release);
            worker_ = std::thread([this, cx, cz]() {
                stream_.prepare(cx, cz);
                done_.store(true, std::memory_order_release);
            });
            phase_ = Phase::Preparing;
            return;
        }

        if (!done_.load(std::memory_order_acquire)) return;
        worker_.join();

        if (phase_ == Phase::Preparing) {
            const auto t0 = std::chrono::steady_clock::now();
            change_ = StreamWorld::Change();  // it reports THIS crossing, not every one so far
            stream_.commit(&change_);
            commitMs_ = msSince(t0);
            done_.store(false, std::memory_order_release);
            // -- ONLY THE CHUNKS NEAR WHAT MOVED ---------------------------
            //
            // A chunk's device bytes depend on its own bricks and, through the
            // peel, on the bricks immediately around it -- so the ring ONE
            // chunk wide beside anything that arrived or left has to be redone
            // and everything else is kept. Drawn any tighter this leaves a
            // chunk holding a peel taken against a neighbour that has since
            // gone, which is a hole at a seam in a build that reports nothing.
            dirty_.clear();
            auto touch = [this](const ColGrid &g) {
                const int ci = floorDiv(g.i0, StreamWorld::kChunkCols);
                const int cj = floorDiv(g.j0, StreamWorld::kChunkCols);
                for (const auto &kv : packCache_.chunk) {
                    const int kx = keyX(kv.first), kz = keyZ(kv.first);
                    if (std::abs(kx - ci) <= 1 && std::abs(kz - cj) <= 1) dirty_.insert(kv.first);
                }
            };
            for (const ColGrid &g : change_.added) touch(g);
            for (const ColGrid &g : change_.removed) touch(g);

            worker_ = std::thread([this]() {
                packStoreInto(packed_, live(), pack, &packCache_, &dirty_);
                done_.store(true, std::memory_order_release);
            });
            phase_ = Phase::Packing;
            return;
        }

        // Phase::Packing -- the buffers are ready.
        const auto t0 = std::chrono::steady_clock::now();
        // The cheap paths, and a full rebuild whenever anything outgrew what is
        // allocated -- which is rare and has to stay correct rather than fast.
        bool inc = uploadDirty(packed_);
        if (inc) inc = buildStructuresIncremental(packed_);
        const double upMs = msSince(t0);
        const auto tAs = std::chrono::steady_clock::now();
        if (!inc) {
            upload(packed_);
            buildStructures(packed_);
        }
        const double asMs = msSince(tAs);
        // FROM THE CHUNKS, not from the buffer: the buffer is padded, and a
        // report of how big the world is should not include the headroom.
        bricks_ = 0;
        for (const PackedStore::Chunk &c : packed_.chunk) bricks_ += c.count;
        chunks_ = uint32_t(packed_.chunk.size());
        const double gpuMs = msSince(t0);
        phase_ = Phase::Idle;
        ++rebuilds_;

        std::printf("v4: window -> chunk (%d %d)  +%zu -%zu  | on the frame: commit %.0f + gpu "
                    "%.0f (up+blas %.0f / full %.0f, %u blas rebuilt) ms  | off it: prepare %.0f, pack %.0f (%zu of %zu chunks)\n",
                    stream_.centreX(), stream_.centreZ(), change_.added.size(),
                    change_.removed.size(), commitMs_, gpuMs, upMs, asMs, blasRebuilt_,
                    change_.columnsMs + change_.chunksMs, packed_.peelMs + packed_.assembleMs,
                    dirty_.size(), packCache_.chunk.size());
        std::fflush(stdout);
        // packed_ is KEPT: its three buffers are the shadow of the device's,
        // and the next pack writes only the chunks that changed into them.
    }

    uint64_t rebuilds() const { return rebuilds_; }

    void bind(const ShaderVar &var) const {
        if (tlas_)
            var["gStoreTlas"].setAccelerationStructure(ref<RtAccelerationStructure>(tlas_.get()));
        var["gStoreBricks"] = brickBuf_;
        var["gStoreMasks"] = maskBuf_;

        // AND THE CONSTANTS, on the same call. This is the whole reason the
        // seam is shaped the way it is -- a backend that needed the tracer to
        // assemble its constant buffer would be a backend the tracer has to
        // know the fields of. See the note at the top of this file.
        //
        // setBlob OF A NAMED LOCAL, not of a temporary: a temporary's address
        // is not reliably still live by the time the copy is made, and the
        // failure is a constant buffer holding whatever was on the stack.
        V4StoreConsts c{};
        c.brickCount = bricks_;
        c.debugMode = debugMode;
        c.maxSteps = maxSteps > 0u ? maxSteps : uint32_t(kBrickMaxSteps);
        c.pad0 = 0;
        var["V4StoreCB"]["gStore"].setBlob(&c, sizeof(c));
    }

    void addDefines(DefineList &defs) const {
        defs.add("V4_STORE_IMPL", "\"stores/Aabb.slang\"");
        // THE BRICK SIZE THE HOST WAS COMPILED WITH, handed to the shader so
        // the two cannot disagree about it. They share AabbShared.slang, but
        // the header's default only applies to whichever half did not get the
        // define -- and a host packing 8^3 bricks read by a shader indexing
        // 16^3 ones does not fail, it reads the wrong bits.
        defs.add("V4_BRICK_SHIFT", std::to_string(kBrickShift));
        defs.add("V4_OCTANT_SKIP", octantSkip ? "1" : "0");
    }

    bool ready() const { return tlas_ != nullptr && bricks_ > 0; }
    const char *status() const { return status_.c_str(); }
    const char *models() const { return models_.c_str(); }

    // -- THE SECOND INTERFACE, on the host ---------------------------------
    //
    // Placing a spawn, settling the camera onto the ground, and any pick or
    // collision test are questions the shader seam does not answer. See the
    // note in gpu/world.h about why they are the half that gets forgotten.
    const BrickWorld &world() const { return live(); }
    float groundM(float x, float z) const { return live().groundM(x, z); }
    bool solidAt(int i, int y, int j) const { return live().solidAt(i, y, j); }

  private:
    static double msSince(std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t)
            .count();
    }

    ref<Buffer> makeBuffer(size_t bytes, ResourceBindFlags flags, const void *data,
                           const char *name) {
        // A ZERO-BYTE BUFFER IS NOT ALLOWED, and an empty world would ask for
        // one of each of these. 256 bytes of nothing keeps every binding valid
        // and every ray missing, which is exactly the null store's behaviour
        // and exactly what the render path has to be able to survive.
        ref<Buffer> b = device_->createBuffer(std::max<size_t>(bytes, 256), flags,
                                             Falcor::MemoryType::DeviceLocal, data);
        b->setName(name);
        return b;
    }

    // -- ONLY THE RANGES THAT CHANGED ---------------------------------------
    //
    // The slotted pack leaves every untouched chunk's bytes exactly where they
    // were, so a crossing rewrites about a tenth of the buffers rather than all
    // 350 MB of them.
    //
    // THE BARRIER IS NOT OPTIONAL, and the note on the full upload below says
    // why: updateBuffer leaves the resource in CopyDest, and a structure build
    // is handed a raw device address with no idea what resource it belongs to.
    // Reading a CopyDest resource as a build input is undefined and on this
    // driver it removes the device -- so the AABB buffer is put back into a
    // readable state here, before buildStructures touches it.
    bool uploadDirty(const PackedStore &p) {
        if (!p.incremental || !aabbBuf_ || !brickBuf_ || !maskBuf_) return false;
        // A buffer that outgrew its allocation has to be made again.
        if (p.aabb.size() * sizeof(float) != aabbBuf_->getSize() ||
            p.brick.size() != brickBuf_->getElementCount() ||
            p.mask.size() != maskBuf_->getElementCount())
            return false;  // the pack grew its buffers: they have to be remade

        for (const PackedStore::Span &sp : p.dirtyBricks) {
            ctx_->updateBuffer(brickBuf_.get(), p.brick.data() + sp.first,
                               size_t(sp.first) * sizeof(V4Brick),
                               size_t(sp.count) * sizeof(V4Brick));
            ctx_->updateBuffer(aabbBuf_.get(), p.aabb.data() + size_t(sp.first) * 6,
                               size_t(sp.first) * 6 * sizeof(float),
                               size_t(sp.count) * 6 * sizeof(float));
        }
        for (const PackedStore::Span &sp : p.dirtyMask)
            ctx_->updateBuffer(maskBuf_.get(), p.mask.data() + sp.first,
                               size_t(sp.first) * sizeof(uint32_t),
                               size_t(sp.count) * sizeof(uint32_t));

        // NonPixelShader, NOT ShaderResource: the AABB buffer is read by a
        // structure BUILD, which D3D12 requires to be in
        // NON_PIXEL_SHADER_RESOURCE, and the other two are read by a compute
        // shader which wants the same. Getting this wrong is not a validation
        // warning on this driver, it is a removed device.
        ctx_->resourceBarrier(aabbBuf_.get(), Falcor::Resource::State::NonPixelShader);
        ctx_->resourceBarrier(brickBuf_.get(), Falcor::Resource::State::NonPixelShader);
        ctx_->resourceBarrier(maskBuf_.get(), Falcor::Resource::State::NonPixelShader);
        // ...and the copies must be DONE before the builds below read them.
        ctx_->submit(false);
        return true;
    }

    void upload(const PackedStore &p) {
        // CREATED WITH THEIR DATA, NOT FILLED AFTERWARDS. updateBuffer leaves a
        // buffer in CopyDest, and buildAccelerationStructure adds no barriers
        // of its own -- it is handed a raw device address and cannot know what
        // resource it belongs to. Reading a CopyDest resource as a build input
        // is undefined, and on this driver it removes the device. Creating it
        // with the data leaves it in Common, which is legal to read.
        // CREATED WITH THEIR DATA, AND THAT IS NOT NEGOTIABLE -- see above. The
        // headroom a streamed world needs comes from the PACK sizing these
        // vectors to a stable capacity (packStoreInto's slotted branch), not
        // from filling a bigger buffer afterwards: updateBuffer leaves the
        // resource in CopyDest and a structure build reading that removes the
        // device. It was tried the other way round and it did exactly that.
        aabbBuf_ = makeBuffer(p.aabb.size() * sizeof(float), ResourceBindFlags::ShaderResource,
                              p.aabb.empty() ? nullptr : p.aabb.data(), "v4::storeAabbs");

        brickBuf_ = device_->createStructuredBuffer(
            sizeof(V4Brick), uint32_t(std::max<size_t>(p.brick.size(), 1)),
            ResourceBindFlags::ShaderResource, Falcor::MemoryType::DeviceLocal,
            p.brick.empty() ? nullptr : p.brick.data());
        brickBuf_->setName("v4::storeBricks");

        // ONE BUFFER FOR THE MASKS AND THE MATERIALS BOTH, interleaved per
        // brick -- see PackedStore::mask. They were two, and the split cost a
        // cache miss on every material resolve.
        maskBuf_ = device_->createStructuredBuffer(
            sizeof(uint32_t), uint32_t(std::max<size_t>(p.mask.size(), 1)),
            ResourceBindFlags::ShaderResource, Falcor::MemoryType::DeviceLocal,
            p.mask.empty() ? nullptr : p.mask.data());
        maskBuf_->setName("v4::storeBrickData");

    }

    // -----------------------------------------------------------------------
    // ONE BOTTOM-LEVEL STRUCTURE PER CHUNK, ALL IN ONE BUFFER.
    //
    // A structure object in Falcor wraps a buffer range and does not own it, so
    // several can share one allocation as long as each starts on a 256-byte
    // boundary -- which is what kAccelerationStructureByteAlignment is. Three
    // hundred separate committed resources would be three hundred 64 KB heap
    // allocations for structures that are frequently smaller than that.
    //
    // NO COMPACTION. It would roughly halve the result buffer, and it costs a
    // second pass plus a readback of every chunk's compacted size, which means
    // a device drain at startup -- and this engine has a compaction bug in its
    // history that replaced whole chunks of terrain with a flat sheet. The
    // memory is not worth re-opening that.
    // -----------------------------------------------------------------------
    // One chunk's geometry: a contiguous slice of the AABB buffer. Shared by
    // both structure paths so they cannot drift apart.
    RtGeometryDesc geomDesc(const PackedStore &p, uint32_t c, uint64_t aabbBase) const {
        RtGeometryDesc g{};
        g.type = RtGeometryType::ProcedurePrimitives;
        g.flags = RtGeometryFlags::Opaque;
        g.content.proceduralAABBs.count = p.chunk[c].count;
        g.content.proceduralAABBs.data = aabbBase + uint64_t(p.chunk[c].first) * 24ull;
        g.content.proceduralAABBs.stride = 24ull;
        return g;
    }
    static RtAccelerationStructureBuildInputs geomInputs(const RtGeometryDesc *g) {
        RtAccelerationStructureBuildInputs in = {};
        in.kind = RtAccelerationStructureKind::BottomLevel;
        in.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        in.descCount = 1;
        in.geometryDescs = g;
        return in;
    }

    void buildTlas(const std::vector<RtInstanceDesc> &inst) {
        instBuf_ =
            makeBuffer(std::max<size_t>(inst.size(), 1) * sizeof(RtInstanceDesc),
                       ResourceBindFlags::ShaderResource,
                       inst.empty() ? nullptr : inst.data(), "v4::storeInstances");

        RtAccelerationStructureBuildInputs tin = {};
        tin.kind = RtAccelerationStructureKind::TopLevel;
        tin.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        tin.descCount = uint32_t(inst.size());
        tin.instanceDescs = instBuf_->getGpuAddress();

        const auto tpre = RtAccelerationStructure::getPrebuildInfo(device_.get(), tin);
        tlasBuf_ = makeBuffer(size_t(tpre.resultDataMaxSize),
                              ResourceBindFlags::AccelerationStructure, nullptr, "v4::storeTlas");
        ref<Buffer> tscratch =
            makeBuffer(size_t(tpre.scratchDataSize), ResourceBindFlags::UnorderedAccess, nullptr,
                       "v4::storeTlasScratch");

        RtAccelerationStructure::Desc td;
        td.setKind(RtAccelerationStructureKind::TopLevel);
        td.setBuffer(tlasBuf_, 0, tlasBuf_->getSize());
        tlas_ = RtAccelerationStructure::create(device_, td);

        RtAccelerationStructure::BuildDesc tbd = {};
        tbd.inputs = tin;
        tbd.source = nullptr;
        tbd.dest = tlas_.get();
        tbd.scratchData = tscratch->getGpuAddress();
        ctx_->buildAccelerationStructure(tbd, 0, nullptr);
        ctx_->uavBarrier(tlasBuf_.get());
        // Flushed, so the local scratch cannot be released under a build that
        // has not run. See the note in the full path.
        ctx_->submit(true);
    }

    // -----------------------------------------------------------------------
    // REBUILD ONLY THE CHUNKS THAT MOVED.
    //
    // A bottom-level structure is built over a RANGE OF THE AABB BUFFER, so it
    // stays valid for exactly as long as that range does. The slotted pack is
    // what makes that true: an untouched chunk keeps its address, its bytes are
    // never rewritten, and its structure is still the structure of what is
    // there. Rebuilding all 2,105 of them because 250 moved was 281 ms of a
    // 348 ms crossing.
    //
    // THE TOP LEVEL IS STILL REBUILT WHOLE, and that is fine -- it is 2,105
    // instance descriptors, 135 KB, and a build over boxes rather than over
    // geometry. It is milliseconds.
    //
    // Returns false if anything about the world outgrew what is allocated, in
    // which case the caller falls back to building everything.
    bool buildStructuresIncremental(const PackedStore &p) {
        if (!p.incremental || !blasBuf_ || blasOf_.empty()) return false;

        const uint32_t n = uint32_t(p.chunk.size());
        std::unordered_set<uint64_t> dirty;
        dirty.reserve(p.dirtyChunks.size() * 2);
        for (uint32_t ci : p.dirtyChunks)
            if (ci < n)
                dirty.insert(brickKey(p.chunk[ci].cx, p.chunk[ci].cy, p.chunk[ci].cz));

        const uint64_t aabbBase = aabbBuf_->getGpuAddress();
        std::vector<RtInstanceDesc> inst(n);
        uint64_t maxScratch = 0;
        std::vector<uint32_t> rebuild;
        rebuild.reserve(dirty.size() + 16);

        // First pass: work out what has to be rebuilt and whether it all fits.
        for (uint32_t c = 0; c < n; ++c) {
            const uint64_t ck = brickKey(p.chunk[c].cx, p.chunk[c].cy, p.chunk[c].cz);
            auto it = blasOf_.find(ck);
            const bool moved = it == blasOf_.end() || it->second.first != p.chunk[c].first ||
                               it->second.prims != p.chunk[c].count || dirty.count(ck) != 0;
            if (!moved) continue;
            RtGeometryDesc g = geomDesc(p, c, aabbBase);
            const auto in = geomInputs(&g);
            const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), in);
            if (it == blasOf_.end() || it->second.bytes < pre.resultDataMaxSize) {
                // Needs a region, or a bigger one. Give back what it had.
                if (it != blasOf_.end()) {
                    blasAlloc_.give({it->second.off, it->second.bytes});
                    blasOf_.erase(it);
                }
                const SlotAlloc::Range r = blasAlloc_.take(uint32_t(pre.resultDataMaxSize));
                if (uint64_t(r.first) + r.cap > blasBuf_->getSize()) return false;  // full
                BlasSlot sl;
                sl.off = r.first;
                sl.bytes = r.cap;
                blasOf_[ck] = sl;
            }
            maxScratch = std::max(maxScratch, pre.scratchDataSize);
            rebuild.push_back(c);
        }
        if (maxScratch > 0 && (!scratch_ || scratch_->getSize() < maxScratch))
            scratch_ = makeBuffer(size_t(maxScratch), ResourceBindFlags::UnorderedAccess, nullptr,
                                  "v4::storeScratch");

        for (uint32_t c : rebuild) {
            const uint64_t ck = brickKey(p.chunk[c].cx, p.chunk[c].cy, p.chunk[c].cz);
            BlasSlot &sl = blasOf_[ck];
            RtAccelerationStructure::Desc d;
            d.setKind(RtAccelerationStructureKind::BottomLevel);
            d.setBuffer(blasBuf_, sl.off, sl.bytes);
            sl.as = RtAccelerationStructure::create(device_, d);
            sl.first = p.chunk[c].first;
            sl.prims = p.chunk[c].count;

            RtGeometryDesc g = geomDesc(p, c, aabbBase);
            RtAccelerationStructure::BuildDesc bd = {};
            bd.inputs = geomInputs(&g);
            bd.source = nullptr;
            bd.dest = sl.as.get();
            bd.scratchData = scratch_->getGpuAddress();
            ctx_->buildAccelerationStructure(bd, 0, nullptr);
            ctx_->uavBarrier(scratch_.get());  // one scratch: serialise by hand
        }

        // Anything the world no longer holds gives its region back.
        std::unordered_set<uint64_t> live;
        live.reserve(n * 2);
        for (uint32_t c = 0; c < n; ++c)
            live.insert(brickKey(p.chunk[c].cx, p.chunk[c].cy, p.chunk[c].cz));
        for (auto it = blasOf_.begin(); it != blasOf_.end();) {
            if (live.count(it->first)) { ++it; continue; }
            blasAlloc_.give({it->second.off, it->second.bytes});
            it = blasOf_.erase(it);
        }

        for (uint32_t c = 0; c < n; ++c) {
            RtInstanceDesc &i = inst[c];
            std::memset(i.transform, 0, sizeof(i.transform));
            i.transform[0][0] = 1.0f;
            i.transform[1][1] = 1.0f;
            i.transform[2][2] = 1.0f;
            i.instanceID = p.chunk[c].first & 0xFFFFFFu;
            i.instanceMask = 0xFF;
            i.instanceContributionToHitGroupIndex = 0;
            i.flags = RtGeometryInstanceFlags::ForceOpaque;
            i.accelerationStructure =
                blasOf_.at(brickKey(p.chunk[c].cx, p.chunk[c].cy, p.chunk[c].cz)).as->getGpuAddress();
        }
        ctx_->uavBarrier(blasBuf_.get());
        buildTlas(inst);
        blasRebuilt_ = uint32_t(rebuild.size());
        return true;
    }

    uint32_t blasRebuilt() const { return blasRebuilt_; }

    void buildStructures(const PackedStore &p) {
        const uint32_t n = uint32_t(p.chunk.size());
        std::vector<uint64_t> off(n, 0), size(n, 0);
        uint64_t total = 0, maxScratch = 0;

        const uint64_t aabbBase = aabbBuf_->getGpuAddress();
        auto geomFor = [&](uint32_t c) {
            RtGeometryDesc g{};
            g.type = RtGeometryType::ProcedurePrimitives;
            // OPAQUE, and it means something slightly different here than on a
            // triangle. There is no any-hit shader either way; what this buys
            // is that a candidate arrives ready to commit instead of carrying a
            // non-opaque flag the query has to test. The INTERSECTION still
            // runs in the shader -- that is what a procedural primitive is --
            // and no flag can remove it.
            g.flags = RtGeometryFlags::Opaque;
            g.content.proceduralAABBs.count = p.chunk[c].count;
            // 24 bytes per AABB, and the run is contiguous, so a chunk's slice
            // is an offset into the one buffer. 24 is 8-byte aligned, which is
            // what D3D12 asks of an AABB array address.
            g.content.proceduralAABBs.data = aabbBase + uint64_t(p.chunk[c].first) * 24ull;
            g.content.proceduralAABBs.stride = 24ull;
            return g;
        };
        auto inputsFor = [&](const RtGeometryDesc *g) {
            RtAccelerationStructureBuildInputs in = {};
            in.kind = RtAccelerationStructureKind::BottomLevel;
            in.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
            in.descCount = 1;
            in.geometryDescs = g;
            return in;
        };

        for (uint32_t c = 0; c < n; ++c) {
            const RtGeometryDesc g = geomFor(c);
            const auto in = inputsFor(&g);
            const auto pre = RtAccelerationStructure::getPrebuildInfo(device_.get(), in);
            off[c] = (total + 255ull) & ~255ull;
            size[c] = pre.resultDataMaxSize;
            total = off[c] + pre.resultDataMaxSize;
            maxScratch = std::max(maxScratch, pre.scratchDataSize);
        }

        // HEADROOM, BECAUSE THE NEXT CROSSING REBUILDS INTO THIS SAME BUFFER.
        // Chunks arriving on one edge take the regions chunks leaving the other
        // gave back, but they are never exactly the same size -- so the
        // allocator needs somewhere to put the difference or it would fall back
        // to a full rebuild on the first crossing and every one after it.
        asBytes_ = total;
        const uint64_t cap = total + total / 2 + (1ull << 20);
        blasBuf_ = makeBuffer(size_t(cap), ResourceBindFlags::AccelerationStructure, nullptr,
                              "v4::storeBlas");
        blasAlloc_.reset(256);
        blasOf_.clear();
        scratch_ = makeBuffer(size_t(maxScratch), ResourceBindFlags::UnorderedAccess, nullptr,
                              "v4::storeScratch");

        blas_.resize(n);
        std::vector<RtInstanceDesc> inst(n);
        for (uint32_t c = 0; c < n; ++c) {
            RtAccelerationStructure::Desc d;
            d.setKind(RtAccelerationStructureKind::BottomLevel);
            d.setBuffer(blasBuf_, off[c], size[c]);
            blas_[c] = RtAccelerationStructure::create(device_, d);
            // ...and remembered by chunk, so the next crossing can keep it.
            BlasSlot sl;
            sl.as = blas_[c];
            sl.off = uint32_t(off[c]);
            sl.bytes = uint32_t(size[c]);
            sl.first = p.chunk[c].first;
            sl.prims = p.chunk[c].count;
            blasOf_[brickKey(p.chunk[c].cx, p.chunk[c].cy, p.chunk[c].cz)] = sl;
            blasAlloc_.top = std::max(blasAlloc_.top, uint32_t(off[c] + size[c]));

            const RtGeometryDesc g = geomFor(c);
            RtAccelerationStructure::BuildDesc bd = {};
            bd.inputs = inputsFor(&g);
            bd.source = nullptr;
            bd.dest = blas_[c].get();
            bd.scratchData = scratch_->getGpuAddress();
            ctx_->buildAccelerationStructure(bd, 0, nullptr);
            // ONE SCRATCH BUFFER, SO THE BUILDS ARE SERIALISED BY HAND.
            // buildAccelerationStructure inserts no barriers, so without this
            // the next chunk's build starts scribbling on scratch the previous
            // one is still reading -- which does not fail, it produces a
            // structure with holes in it, and the holes move between runs.
            ctx_->uavBarrier(scratch_.get());

            RtInstanceDesc &i = inst[c];
            // IDENTITY. The AABBs are already in world metres -- see the note
            // at the top of stores/Aabb.slang.
            std::memset(i.transform, 0, sizeof(i.transform));
            i.transform[0][0] = 1.0f;
            i.transform[1][1] = 1.0f;
            i.transform[2][2] = 1.0f;
            // THE CHUNK'S FIRST BRICK INDEX, which is the whole indirection the
            // shader needs: brick is CandidateInstanceID() + CandidatePrimitiveIndex().
            // 24 bits, so this caps a world at 16.7 M bricks -- 8.6 billion
            // voxels of surface, well past what fits in memory.
            i.instanceID = p.chunk[c].first & 0xFFFFFFu;
            i.instanceMask = 0xFF;
            i.instanceContributionToHitGroupIndex = 0;
            i.flags = RtGeometryInstanceFlags::ForceOpaque;
            i.accelerationStructure = blas_[c]->getGpuAddress();
        }
        ctx_->uavBarrier(blasBuf_.get());

        instBuf_ =
            makeBuffer(inst.size() * sizeof(RtInstanceDesc), ResourceBindFlags::ShaderResource,
                       inst.empty() ? nullptr : inst.data(), "v4::storeInstances");

        RtAccelerationStructureBuildInputs tin = {};
        tin.kind = RtAccelerationStructureKind::TopLevel;
        tin.flags = RtAccelerationStructureBuildFlags::PreferFastTrace;
        tin.descCount = n;
        tin.instanceDescs = instBuf_->getGpuAddress();

        const auto tpre = RtAccelerationStructure::getPrebuildInfo(device_.get(), tin);
        tlasBuf_ = makeBuffer(size_t(tpre.resultDataMaxSize),
                              ResourceBindFlags::AccelerationStructure, nullptr, "v4::storeTlas");
        ref<Buffer> tscratch =
            makeBuffer(size_t(tpre.scratchDataSize), ResourceBindFlags::UnorderedAccess, nullptr,
                       "v4::storeTlasScratch");

        RtAccelerationStructure::Desc td;
        td.setKind(RtAccelerationStructureKind::TopLevel);
        td.setBuffer(tlasBuf_, 0, tlasBuf_->getSize());
        tlas_ = RtAccelerationStructure::create(device_, td);

        RtAccelerationStructure::BuildDesc tbd = {};
        tbd.inputs = tin;
        tbd.source = nullptr;
        tbd.dest = tlas_.get();
        tbd.scratchData = tscratch->getGpuAddress();
        ctx_->buildAccelerationStructure(tbd, 0, nullptr);
        ctx_->uavBarrier(tlasBuf_.get());

        // THE BUILDS ARE FLUSHED HERE, AT STARTUP, AND THAT IS DELIBERATE.
        // Everything above was recorded onto the render context; submitting and
        // waiting means the scratch buffers -- including the local one for the
        // top-level build, which goes out of scope on the next line -- cannot
        // be released underneath a build that has not run yet. It costs one
        // device drain, in a frame nobody is looking at.
        ctx_->submit(true);
    }

    ref<Device> device_;
    RenderContext *ctx_ = nullptr;

    // Where each chunk's bottom-level structure lives, keyed by the chunk and
    // not by an index -- chunks come and go, so an index is not a name.
    struct BlasSlot {
        ref<RtAccelerationStructure> as;
        uint32_t off = 0, bytes = 0;   // its region of blasBuf_
        uint32_t first = 0, prims = 0;  // the AABB run it was built over
    };
    std::unordered_map<uint64_t, BlasSlot> blasOf_;
    SlotAlloc blasAlloc_;
    uint32_t blasRebuilt_ = 0;

    // The streaming state machine -- see update().
    enum class Phase { Idle, Preparing, Packing };
    Phase phase_ = Phase::Idle;
    std::thread worker_;
    std::atomic<bool> done_{false};
    PackedStore packed_;
    PackCache packCache_;
    std::unordered_set<uint64_t> dirty_;
    StreamWorld::Change change_;
    double commitMs_ = 0.0;

    // The fixed world, used when streaming is off; `live()` picks between them.
    BrickWorld world_;
    Content content_;
    StreamWorld stream_;
    uint64_t rebuilds_ = 0;

    const BrickWorld &live() const { return stream ? stream_.world() : world_; }
    BrickWorld &live() { return stream ? stream_.world() : world_; }

    ref<Buffer> aabbBuf_, brickBuf_, maskBuf_, instBuf_;
    ref<Buffer> blasBuf_, tlasBuf_, scratch_;
    std::vector<ref<RtAccelerationStructure>> blas_;
    ref<RtAccelerationStructure> tlas_;

    uint32_t bricks_ = 0, chunks_ = 0;
    uint64_t asBytes_ = 0;  // what the bottom-level structures themselves cost
    std::string status_ = "dxr aabb -- not built";
    std::string models_ = "not loaded";
};

// The engine builds against exactly one store. Swapping backends is this
// alias plus the include above it, which is deliberate: selecting at runtime
// would mean the trace shader could not specialise on the backend, and a
// software march that cannot inline its own traversal is not worth measuring.
using Store = AabbStore;

}  // namespace v4
