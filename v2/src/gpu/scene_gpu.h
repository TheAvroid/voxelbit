// ---------------------------------------------------------------------------
// scene_gpu.h -- the endless wood, as OptiX acceleration structures.
//
// THE WHOLE DESIGN QUESTION IS STILL INSTANCING. Nine pine models meshed to
// their exposed faces come to millions of triangles between them; twenty-six
// rocks and six flowers more. A stand flattened would be unbuildable, and every
// tree would still be one of the nine shapes actually authored. One GAS per
// model, referred to through an IAS transform, keeps the memory at one copy of
// each while the scene draws thousands.
//
// WHAT IS NEW IS THAT THE TERRAIN IS INSTANCED THE SAME WAY. It is no longer
// one structure built at startup but a ring of chunk structures that follows
// the camera -- see chunks.h for why that is affordable. This file owns the GPU
// half: which chunks are resident, which SBT slot each holds, and when the IAS
// has to be rebuilt.
//
// ROTATION IS IN QUARTER TURNS ONLY, for everything. An arbitrary yaw would put
// a voxel model off the lattice its own faces are aligned to, and the crisp
// axis-aligned silhouette that makes a voxel tree look like a voxel tree would
// turn into stair-stepped mush.
//
// EVERY GAS IS COMPACTED. OptiX builds into a conservatively sized buffer and
// says afterwards how much it actually needed; for millions of small coplanar
// quads the compacted structure is routinely half the size. With dozens of
// chunks resident that is the difference between fitting in 12 GB and not.
// ---------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../core/noise.h"
#include "../optix/params.h"
#include "../scene/sky.h"
#include "../scene/vox.h"
#include "../scene/voxelworld.h"
#include "chunks.h"
#include "cuda_util.h"

namespace v2 {

struct Accel {
    DeviceBuffer buffer;
    OptixTraversableHandle handle = 0;
};

// One instanced model: a pine, a rock, a flower. They differ only in what
// scatters them and how deep it sinks them.
struct ModelTemplate {
    Accel gas;
    DeviceBuffer tri;
    int sx = 0, sy = 0, sz = 0;
    size_t tris = 0;
};

// How many chunk hitgroup records the SBT carries. A radius-12 ring is 625;
// this leaves room to raise --view further without rebuilding the table. The
// cost is one empty 32-byte record per unused slot, so being generous is free.
constexpr int kMaxChunkSlots = 1024;

class GpuScene {
  public:
    VoxelTerrain terrain;
    Sky sky;
    Palette palette;

    std::string pineDir = "C:/voxelbit/game/assets/foilage/pine9";
    std::string decorDir = "C:/voxelbit/game/assets/decoration";
    int viewChunks = 12;  // ring radius, in chunks
    float treeDensity = 0.31f;
    float rockDensity = 0.010f;
    float flowerDensity = 0.22f;
    uint32_t seed = 20260904u;
    int meshThreads = 0;

    int loadedPines = 0, loadedRocks = 0, loadedFlowers = 0;
    size_t uniqueTris = 0;

    OptixTraversableHandle handle() const { return iasHandle_; }
    size_t chunkCount() const { return chunks_.size(); }
    size_t instanceCount() const { return instances_.size(); }
    size_t residentTris() const { return residentTris_; }
    size_t pendingChunks() { return mesher_.inFlight(); }

    // Where the time actually went, so a slow world build can be attributed
    // rather than guessed at. gasMs is GPU structure building; the rest of the
    // wall clock is the worker threads meshing.
    double gasMs() const { return gasMs_; }
    double uploadMs() const { return uploadMs_; }

    const std::vector<HitGroupData> &hitGroups() const { return hitGroups_; }
    CUdeviceptr materialsPtr() const { return materials_.ptr(); }
    CUdeviceptr instancesPtr() const { return instanceData_.ptr(); }

    unsigned chunkSbtBase() const { return chunkSbtBase_; }

    // Set by main() so a chunk can publish its own SBT record when it takes a
    // slot. A callback rather than a back-reference, because this file has no
    // business knowing what a Pipeline is.
    void *sbtOwner = nullptr;
    void (*sbtWrite)(void *, unsigned, const HitGroupData &) = nullptr;

    // -----------------------------------------------------------------------
    bool build(OptixDeviceContext ctx) {
        ctx_ = ctx;

        // Templates first: the terrain's grass and soil colours are sampled
        // from the palette the models bring with them.
        if (!loadPines()) return false;
        loadRocks();
        loadFlowers();
        palette.deriveGroundFromTrees();

        buildWater();
        buildSbtLayout();

        materials_.upload(palette.table());
        looks_.push_back(InstanceData{});  // index 0: the identity tint

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
    // The budget is on GAS BUILDS, not on meshes taken: the build is the part
    // that runs on this thread and therefore the part that can stall a frame.
    // Returns true if the IAS changed and the launch needs the new handle.
    // -----------------------------------------------------------------------
    bool update(Vec3 camPos, int gasBudget = 2, bool buildIas = true) {
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
        while (int(batch.size()) < gasBudget && mesher_.take(&b)) {
            requested_.erase(chunkKey(b.cx, b.cz));
            if (wanted_.count(chunkKey(b.cx, b.cz)) == 0) continue;  // evicted while queued
            batch.push_back(std::move(b));
        }
        if (!batch.empty()) {
            adoptMany(std::move(batch));
            changed = true;
        }

        // Deferred while priming: the IAS is rebuilt WHOLE every time it
        // changes, so doing it once per batch of adopted chunks meant twenty-odd
        // full rebuilds and twenty-odd device synchronisations to reach the same
        // state one rebuild at the end would have produced.
        if (changed && buildIas) rebuildIas();
        return changed;
    }

    // Block until the ring around the camera is fully resident. Used once at
    // startup so the first frame is not a hole in the ground.
    void primeBlocking(Vec3 camPos) {
        update(camPos, 0, false);
        while (mesher_.inFlight() > 0 || !requested_.empty()) {
            // No cap worth the name here and no IAS rebuild: nothing is being
            // displayed yet, so the only thing that matters is draining the
            // queue as fast as the workers can fill it.
            // 48 at a time: large enough that the two synchronisations are
            // amortised to nothing, small enough that the uncompacted
            // structures held simultaneously stay well inside video memory.
            if (!update(camPos, 48, false))
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        rebuildIas();
    }

  private:
    OptixDeviceContext ctx_ = nullptr;
    ChunkMesher mesher_;

    std::vector<ModelTemplate> pines_, rocks_, flowers_;
    Accel waterGas_;
    DeviceBuffer waterTri_;

    std::map<long long, Chunk> chunks_;
    std::map<long long, int> wanted_;
    std::set<long long> requested_;
    std::vector<int> freeSlots_;
    std::vector<HitGroupData> hitGroups_;
    unsigned chunkSbtBase_ = 0;

    DeviceBuffer instanceBuf_, materials_, instanceData_, iasBuf_;
    std::vector<InstanceData> looks_;
    std::vector<OptixInstance> instances_;
    OptixTraversableHandle iasHandle_ = 0;
    size_t residentTris_ = 0;
    double gasMs_ = 0.0, uploadMs_ = 0.0;
    int lastCx_ = 0, lastCz_ = 0;
    bool primed_ = false;

    // ------------------------------------------------------------------ GAS
    Accel buildGas(const VoxMesh &m) {
        const auto t0 = std::chrono::steady_clock::now();
        DeviceBuffer verts, idx;
        verts.upload(m.position);
        idx.upload(m.index);

        CUdeviceptr vptr = verts.ptr();
        const unsigned int triFlags[1] = {OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT};

        OptixBuildInput in = {};
        in.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
        in.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
        in.triangleArray.vertexStrideInBytes = sizeof(Vec3);
        in.triangleArray.numVertices = unsigned(m.position.size());
        in.triangleArray.vertexBuffers = &vptr;
        in.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
        in.triangleArray.indexStrideInBytes = 3 * sizeof(unsigned int);
        in.triangleArray.numIndexTriplets = unsigned(m.index.size() / 3);
        in.triangleArray.indexBuffer = idx.ptr();
        in.triangleArray.flags = triFlags;
        in.triangleArray.numSbtRecords = 1;

        OptixAccelBuildOptions opts = {};
        opts.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
        opts.operation = OPTIX_BUILD_OPERATION_BUILD;

        OptixAccelBufferSizes sizes = {};
        OPTIX_CHECK(optixAccelComputeMemoryUsage(ctx_, &opts, &in, 1, &sizes));

        DeviceBuffer temp(sizes.tempSizeInBytes);
        DeviceBuffer uncompacted(sizes.outputSizeInBytes);
        DeviceBuffer compactedSize(sizeof(size_t));

        OptixAccelEmitDesc emit = {};
        emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
        emit.result = compactedSize.ptr();

        Accel out;
        OptixTraversableHandle h = 0;
        OPTIX_CHECK(optixAccelBuild(ctx_, 0, &opts, &in, 1, temp.ptr(), temp.bytes(),
                                    uncompacted.ptr(), uncompacted.bytes(), &h, &emit, 1));
        CU_CHECK(cuStreamSynchronize(0));

        size_t compacted = 0;
        compactedSize.download(&compacted, 1);
        if (compacted < sizes.outputSizeInBytes) {
            out.buffer.alloc(compacted);
            OPTIX_CHECK(optixAccelCompact(ctx_, 0, h, out.buffer.ptr(), compacted, &out.handle));
            CU_CHECK(cuStreamSynchronize(0));
        } else {
            out.buffer = std::move(uncompacted);
            out.handle = h;
        }
        gasMs_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - t0).count();
        return out;
    }

    // ------------------------------------------------------------ templates
    void loadModelSet(const std::vector<std::string> &paths, std::vector<ModelTemplate> *out,
                      bool multiModel, bool quiet) {
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

                // ONLY THE ENTRIES THE MODEL USES. Registering all 255 of a
                // file's palette floods the shared table, and past 255
                // forModelColor returns AIR and real voxels stop being drawn --
                // which is exactly what six flower files did in one pass.
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
                t.tris = mesh.triCount();
                t.tri.upload(mesh.tri);
                t.gas = buildGas(mesh);
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
            std::fprintf(stderr, "v2: no pine models loaded from %s -- pass --pines\n",
                         pineDir.c_str());
            return false;
        }
        loadedPines = int(pines_.size());
        return true;
    }

    // The 26 rocks, each its own file. The names are listed rather than
    // globbed: this build has no directory-walk helper, and a fixed list also
    // means a missing file is a warning about that file rather than a scene
    // that silently has fewer rocks in it than it should.
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
        waterTri_.upload(wm.tri);
        waterGas_ = buildGas(wm);
    }

    // ------------------------------------------------------------------ SBT
    void buildSbtLayout() {
        hitGroups_.clear();
        hitGroups_.push_back({waterTri_.as<const unsigned short>(), KIND_WATER, 0u});
        for (const ModelTemplate &t : pines_)
            hitGroups_.push_back({t.tri.as<const unsigned short>(), KIND_TREE, 0u});
        // Rocks shade as terrain: opaque, no per-instance tint, no translucency.
        for (const ModelTemplate &t : rocks_)
            hitGroups_.push_back({t.tri.as<const unsigned short>(), KIND_TERRAIN, 0u});
        for (const ModelTemplate &t : flowers_)
            hitGroups_.push_back({t.tri.as<const unsigned short>(), KIND_TERRAIN, 0u});

        chunkSbtBase_ = unsigned(hitGroups_.size());
        for (int i = 0; i < kMaxChunkSlots; ++i)
            hitGroups_.push_back({nullptr, KIND_TERRAIN, 0u});
        for (int i = kMaxChunkSlots - 1; i >= 0; --i) freeSlots_.push_back(i);
    }

    void publishSlot(int slot) {
        if (sbtWrite)
            sbtWrite(sbtOwner, chunkSbtBase_ + unsigned(slot), hitGroups_[chunkSbtBase_ + slot]);
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
                freeSlots_.push_back(it->second.slot);
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

    // -----------------------------------------------------------------------
    // Adopt a whole batch of chunks with TWO device synchronisations, not two
    // per chunk.
    //
    // Building one structure is: queue the build, wait, read back how much the
    // compacted form needs, queue the compact, wait. Done per chunk that is two
    // round trips each, and at a radius-12 ring -- six hundred and twenty-five
    // chunks -- it is twelve hundred and fifty of them. The waits are not free
    // even when the GPU has nothing left to do: each one is a launch plus a
    // driver round trip, and they serialise the CPU against the device for the
    // whole prime.
    //
    // Batched, every build in the batch is queued back to back, ONE wait covers
    // all of them, all the sizes are read at once, every compact is queued, and
    // one more wait finishes. Two synchronisations for sixty-four chunks
    // instead of a hundred and twenty-eight.
    //
    // The cost is holding every uncompacted structure in the batch at once,
    // which is why the batch is bounded rather than unlimited.
    // -----------------------------------------------------------------------
    void adoptMany(std::vector<ChunkBuild> &&batch) {
        const auto tGas = std::chrono::steady_clock::now();
        struct Staged {
            DeviceBuffer verts, idx, temp, uncompacted, sizeBuf;
            OptixTraversableHandle handle = 0;
            size_t uncompactedBytes = 0;
        };
        std::vector<Staged> staged(batch.size());

        const unsigned int triFlags[1] = {OPTIX_GEOMETRY_FLAG_DISABLE_ANYHIT};
        OptixAccelBuildOptions opts = {};
        opts.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE | OPTIX_BUILD_FLAG_ALLOW_COMPACTION;
        opts.operation = OPTIX_BUILD_OPERATION_BUILD;

        // -- pass 1: queue every build ---------------------------------------
        for (size_t k = 0; k < batch.size(); ++k) {
            const VoxMesh &m = batch[k].mesh;
            Staged &g = staged[k];
            g.verts.upload(m.position);
            g.idx.upload(m.index);

            CUdeviceptr vptr = g.verts.ptr();
            OptixBuildInput in = {};
            in.type = OPTIX_BUILD_INPUT_TYPE_TRIANGLES;
            in.triangleArray.vertexFormat = OPTIX_VERTEX_FORMAT_FLOAT3;
            in.triangleArray.vertexStrideInBytes = sizeof(Vec3);
            in.triangleArray.numVertices = unsigned(m.position.size());
            in.triangleArray.vertexBuffers = &vptr;
            in.triangleArray.indexFormat = OPTIX_INDICES_FORMAT_UNSIGNED_INT3;
            in.triangleArray.indexStrideInBytes = 3 * sizeof(unsigned int);
            in.triangleArray.numIndexTriplets = unsigned(m.index.size() / 3);
            in.triangleArray.indexBuffer = g.idx.ptr();
            in.triangleArray.flags = triFlags;
            in.triangleArray.numSbtRecords = 1;

            OptixAccelBufferSizes sizes = {};
            OPTIX_CHECK(optixAccelComputeMemoryUsage(ctx_, &opts, &in, 1, &sizes));
            g.temp.alloc(sizes.tempSizeInBytes);
            g.uncompacted.alloc(sizes.outputSizeInBytes);
            g.uncompactedBytes = sizes.outputSizeInBytes;
            g.sizeBuf.alloc(sizeof(size_t));

            OptixAccelEmitDesc emit = {};
            emit.type = OPTIX_PROPERTY_TYPE_COMPACTED_SIZE;
            emit.result = g.sizeBuf.ptr();

            OPTIX_CHECK(optixAccelBuild(ctx_, 0, &opts, &in, 1, g.temp.ptr(), g.temp.bytes(),
                                        g.uncompacted.ptr(), g.uncompacted.bytes(), &g.handle,
                                        &emit, 1));
        }
        CU_CHECK(cuStreamSynchronize(0));  // one wait for the whole batch

        // -- pass 2: queue every compact -------------------------------------
        std::vector<Chunk> made(batch.size());
        for (size_t k = 0; k < batch.size(); ++k) {
            Staged &g = staged[k];
            size_t compacted = 0;
            g.sizeBuf.download(&compacted, 1);

            Chunk &c = made[k];
            if (compacted < g.uncompactedBytes) {
                c.buffer.alloc(compacted);
                OPTIX_CHECK(
                    optixAccelCompact(ctx_, 0, g.handle, c.buffer.ptr(), compacted, &c.gas));
            } else {
                c.buffer = std::move(g.uncompacted);
                c.gas = g.handle;
            }
        }
        CU_CHECK(cuStreamSynchronize(0));  // and one for every compact

        // -- pass 3: publish -------------------------------------------------
        for (size_t k = 0; k < batch.size(); ++k) {
            if (freeSlots_.empty()) break;  // ring bigger than the slot table
            Chunk &c = made[k];
            ChunkBuild &b = batch[k];
            c.cx = b.cx;
            c.cz = b.cz;
            c.slot = freeSlots_.back();
            freeSlots_.pop_back();
            c.tris = b.mesh.triCount();
            c.tri.upload(b.mesh.tri);

            hitGroups_[chunkSbtBase_ + c.slot] = {c.tri.as<const unsigned short>(), KIND_TERRAIN,
                                                 0u};
            publishSlot(c.slot);
            for (const Placement &p : b.decor) c.decor.push_back(makeInstance(p));

            residentTris_ += c.tris;
            chunks_.emplace(chunkKey(b.cx, b.cz), std::move(c));
        }
        gasMs_ += std::chrono::duration<double, std::milli>(
                      std::chrono::steady_clock::now() - tGas).count();
    }

    const ModelTemplate &templateFor(int kind, int index) const {
        const std::vector<ModelTemplate> &v =
            (kind == 0) ? pines_ : (kind == 1) ? rocks_ : flowers_;
        return v[size_t(index) % v.size()];
    }

    OptixInstance makeInstance(const Placement &p) {
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
        OptixInstance inst = {};
        writeTransform(inst, m, tx + halfOf(fx) - (m[0] * cx + m[2] * cz), ty,
                       tz + halfOf(fz) - (m[6] * cx + m[8] * cz));

        unsigned sbt = 1u;
        if (p.kind == 0) sbt = 1u + unsigned(p.index % int(pines_.size()));
        else if (p.kind == 1)
            sbt = 1u + unsigned(pines_.size()) + unsigned(p.index % int(rocks_.size()));
        else
            sbt = 1u + unsigned(pines_.size() + rocks_.size()) +
                  unsigned(p.index % int(flowers_.size()));

        inst.instanceId = (p.kind == 0) ? tintFor(p.cell) : 0u;
        inst.sbtOffset = sbt;
        inst.visibilityMask = 255;
        inst.flags = OPTIX_INSTANCE_FLAG_NONE;
        inst.traversableHandle = t.gas.handle;
        return inst;
    }

    // A few percent of per-tree hue. Nine models over thousands of trees would
    // otherwise show their repeat.
    unsigned tintFor(uint32_t cell) {
        InstanceData look;
        const float t = hashUnit(seed + 19u, cell);
        const float v = 0.90f + 0.20f * hashUnit(seed + 20u, cell);
        look.tint = Vec3(lerpf(0.94f, 1.06f, t), 1.0f, lerpf(1.05f, 0.92f, t)) * v;
        looks_.push_back(look);
        return unsigned(looks_.size() - 1);
    }

    static void writeTransform(OptixInstance &inst, const float *m, float tx, float ty, float tz) {
        inst.transform[0] = m[0];  inst.transform[1] = m[1];  inst.transform[2] = m[2];
        inst.transform[3] = tx;
        inst.transform[4] = m[3];  inst.transform[5] = m[4];  inst.transform[6] = m[5];
        inst.transform[7] = ty;
        inst.transform[8] = m[6];  inst.transform[9] = m[7];  inst.transform[10] = m[8];
        inst.transform[11] = tz;
    }

    static float halfOf(int voxels) { return float(voxels) * VOXEL_M * 0.5f; }

    // ------------------------------------------------------------------ IAS
    void rebuildIas() {
        static const float kI[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        instances_.clear();

        OptixInstance water = {};
        writeTransform(water, kI, 0.0f, 0.0f, 0.0f);
        water.visibilityMask = 255;
        water.traversableHandle = waterGas_.handle;
        instances_.push_back(water);

        for (const auto &kv : chunks_) {
            OptixInstance ci = {};
            // Chunk meshes are already in world space, so the transform is
            // identity -- the instance exists to carry the sbtOffset, not to
            // place anything.
            writeTransform(ci, kI, 0.0f, 0.0f, 0.0f);
            ci.sbtOffset = chunkSbtBase_ + unsigned(kv.second.slot);
            ci.visibilityMask = 255;
            ci.traversableHandle = kv.second.gas;
            instances_.push_back(ci);
            instances_.insert(instances_.end(), kv.second.decor.begin(), kv.second.decor.end());
        }

        instanceData_.upload(looks_);
        instanceBuf_.upload(instances_);

        OptixBuildInput in = {};
        in.type = OPTIX_BUILD_INPUT_TYPE_INSTANCES;
        in.instanceArray.instances = instanceBuf_.ptr();
        in.instanceArray.numInstances = unsigned(instances_.size());

        OptixAccelBuildOptions opts = {};
        // No compaction here, unlike the GASes: this is rebuilt every time the
        // ring moves, and the compaction pass costs a second synchronise for a
        // structure that is a few megabytes at most.
        opts.buildFlags = OPTIX_BUILD_FLAG_PREFER_FAST_TRACE;
        opts.operation = OPTIX_BUILD_OPERATION_BUILD;

        OptixAccelBufferSizes sizes = {};
        OPTIX_CHECK(optixAccelComputeMemoryUsage(ctx_, &opts, &in, 1, &sizes));

        DeviceBuffer temp(sizes.tempSizeInBytes);
        iasBuf_.ensure(sizes.outputSizeInBytes);
        OPTIX_CHECK(optixAccelBuild(ctx_, 0, &opts, &in, 1, temp.ptr(), temp.bytes(),
                                    iasBuf_.ptr(), iasBuf_.bytes(), &iasHandle_, nullptr, 0));
        CU_CHECK(cuStreamSynchronize(0));
    }
};

}  // namespace v2
