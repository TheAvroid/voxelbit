// ---------------------------------------------------------------------------
// clusters.h -- RTX Mega Geometry: the capability gate.
//
// WHAT CLUSTERS ARE FOR. v7 builds one bottom-level acceleration structure per
// resident chunk, and at startup that is 625 of them and most of a second of
// pure structure building -- the number the profile calls out as the thing
// averages hide, because it is paid in whichever frame a chunk happens to
// arrive in rather than spread over the second it took to earn.
//
// A CLUSTER (a CLAS) is a small, pre-built lump of triangles that the driver can
// assemble into a bottom-level structure far more cheaply than building one from
// loose triangles. Templates make it cheaper still: a shape that recurs -- and a
// voxel wood is nothing but recurring shapes -- is built once and instantiated.
// That is the streaming hitch, addressed at its cause.
//
// ---------------------------------------------------------------------------
// WHY THIS IS D3D12 AND NOT VULKAN, WHICH IS THE OPPOSITE OF THE NEURAL PATH.
//
// The Vulkan route was the obvious one: the driver exposes
// VK_NV_cluster_acceleration_structure at revision 4, and v7 already runs on
// Vulkan for cooperative vectors. It is also the expensive one. slang-gfx
// decides the device extensions and that extension is NOT in its list -- the
// strings in gfx.dll say so -- and gfx.dll is prebuilt, so the only way to get
// it enabled is for v7 to CREATE THE VULKAN DEVICE ITSELF and hand it over
// through Device::Desc::existingVulkanHandles. That means reproducing the
// twenty-odd extensions and feature chains slang-gfx would have enabled, and
// the failure mode for getting one wrong is "Falcor does not come up".
//
// NVAPI reaches the same hardware on D3D12 through a device that ALREADY
// EXISTS. NvAPI_D3D12_RaytracingMultiIndirectClusterOperation takes the
// ID3D12Device Falcor made and a command list Falcor recorded; nothing about
// device creation changes. The whole custom-device problem evaporates.
//
// The Vulkan door is still there if it is ever wanted -- the Device::Desc hook
// is written and unused -- but this is the cheap way in, and it lands clusters
// on the SAME backend as DLSS rather than on the one without it.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Buffer.h"
#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if V7_HAS_NVAPI
// ORDER MATTERS. nvapi.h declares swapchain entry points in terms of
// IDXGISwapChain without declaring it, and gates its whole D3D12 section on
// __d3d12_h__ already being defined -- so both of these have to come first or
// the errors are a wall of "syntax error: identifier 'IDXGISwapChain'" from
// inside a header nobody here wrote.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvapi.h>
#endif

namespace v7 {

class Clusters {
  public:
    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;

#if !V7_HAS_NVAPI
        status_ = "built without NVAPI";
        return false;
#else
        // Ask the device what it is, not what its handle looks like -- the same
        // trap dlss.h fell into. NativeHandle::as<T> reinterprets rather than
        // checks, so on Vulkan a VkDevice arrives here as a perfectly non-null
        // ID3D12Device* and the first NVAPI call walks it through a D3D12
        // vtable.
        if (device_->getType() != Falcor::Device::Type::D3D12) {
            status_ = "not a D3D12 device (cluster operations here go through NVAPI)";
            return false;
        }
        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "no D3D12 device handle";
            return false;
        }

        // NVAPI is global and initialising it twice is harmless, but a FAILED
        // init is not: every entry point afterwards returns garbage rather than
        // an error, so this is checked rather than assumed.
        const NvAPI_Status ns = NvAPI_Initialize();
        if (ns != NVAPI_OK) {
            status_ = "NvAPI_Initialize failed (" + std::to_string(int(ns)) + ")";
            return false;
        }

        // THE DRIVER IS ASKED, not the GPU generation. Cluster operations are a
        // driver feature as much as a hardware one -- Ada can build clusters,
        // Blackwell accelerates the build in hardware -- and guessing from the
        // adapter name is how an engine ends up refusing to run on a card that
        // would have been fine.
        NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS clusterCaps =
            NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_NONE;
        if (NvAPI_D3D12_GetRaytracingCaps(d3d, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS,
                                          &clusterCaps, sizeof(clusterCaps)) != NVAPI_OK) {
            status_ = "driver does not answer the cluster-operations capability query";
            return false;
        }

        // Partitioned TLAS is the other half of Mega Geometry: it lets the top
        // level be rebuilt in pieces instead of whole, which is what makes a
        // moving ring of chunks affordable. Queried separately because a driver
        // can have one and not the other, and clusters are useful alone.
        NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS ptlasCaps =
            NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_NONE;
        NvAPI_D3D12_GetRaytracingCaps(d3d, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_PARTITIONED_TLAS,
                                      &ptlasCaps, sizeof(ptlasCaps));

        clusters_ = (clusterCaps & NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD) != 0;
        ptlas_ = (ptlasCaps & NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_STANDARD) != 0;

        if (!clusters_) {
            status_ = "driver reports no cluster-operations support";
            return false;
        }

        ready_ = true;
        status_ = std::string("cluster operations available") +
                  (ptlas_ ? ", partitioned TLAS too" : " (no partitioned TLAS)");
        return true;
#endif
    }

    bool available() const { return ready_; }
    bool hasPartitionedTlas() const { return ready_ && ptlas_; }
    const std::string &status() const { return status_; }

    // -------------------------------------------------------------------
    // HOW A CHUNK IS CUT INTO CLUSTERS, and why it costs nothing.
    //
    // A cluster is capped at 256 triangles and 256 vertices, which in general
    // makes partitioning a mesh a meshlet problem: group triangles so that the
    // vertices they share stay under the cap, which is a graph partition.
    //
    // v7's mesher makes that problem disappear. VoxMesh::addQuad pushes FOUR
    // FRESH VERTICES for every quad -- nothing is ever shared -- and emits
    // indices base+0,1,2 / base+0,2,3. So vertices and triangles run in
    // lockstep down the arrays, and any run of 64 consecutive quads is exactly
    // 256 vertices and 128 triangles: both caps hit exactly, no remapping, no
    // graph. Partitioning is a stride.
    //
    // It also makes the index buffer FREE. Cluster-local indices are the global
    // ones minus the cluster's vertex base, and since the base is always a
    // multiple of four the local pattern for the nth quad is 4n+0,1,2 /
    // 4n+0,2,3 regardless of which cluster it is in. Every cluster in the
    // engine therefore has BYTE-FOR-BYTE THE SAME index buffer -- 384 bytes,
    // built once, shared by all of them, and 8-bit because the largest index a
    // 64-quad cluster can name is 255.
    // -------------------------------------------------------------------
    static constexpr uint32_t kQuadsPerCluster = 64;
    static constexpr uint32_t kTrisPerCluster = kQuadsPerCluster * 2;   // 128
    static constexpr uint32_t kVertsPerCluster = kQuadsPerCluster * 4;  // 256

    // A built cluster BLAS. `address` goes straight into an instance desc --
    // RtInstanceDesc::accelerationStructure is a raw GPU virtual address, so
    // nothing here has to pretend to be a Falcor RtAccelerationStructure.
    struct ClusterBlas {
        Falcor::ref<Falcor::Buffer> clasStorage;  // the CLAS themselves
        Falcor::ref<Falcor::Buffer> blasStorage;  // the BLAS built from them
        uint64_t address = 0;
        uint32_t clusterCount = 0;
        bool valid() const { return address != 0; }
    };

    // -------------------------------------------------------------------
    // Triangles -> CLAS -> BLAS, recorded onto the context's command list.
    //
    // `verts` and `idx` are the chunk's already-uploaded buffers; `quadCount`
    // is its triangle count halved, which is what the mesher actually produced.
    // -------------------------------------------------------------------
    ClusterBlas build(Falcor::RenderContext *ctx, Falcor::Buffer *verts, uint32_t quadCount) {
        ClusterBlas out;
#if V7_HAS_NVAPI
        if (!ready_ || quadCount == 0 || !verts) return out;

        const uint32_t nClusters = (quadCount + kQuadsPerCluster - 1) / kQuadsPerCluster;
        out.clusterCount = nClusters;

        ID3D12GraphicsCommandList *cmd = nativeCmdList(ctx);
        if (!cmd) return out;
        if (!ensureSharedIndices()) return out;
        // THE SHARED INDEX BUFFER HAS TO BE TRANSITIONED TOO, every time.
        //
        // It is created once and reused by every cluster in the engine, which
        // made it easy to forget that it is still an input to this operation
        // like any other -- and the API states plainly that everything
        // indexBuffer points at must be in NON_PIXEL_SHADER_RESOURCE. Created
        // with initial data it starts in Common, and reading a Common resource
        // as a shader resource is undefined; on this driver it removes the
        // device. Falcor tracks the state, so this is a no-op after the first
        // call rather than a real barrier every build.
        ctx->resourceBarrier(sharedIdx_.get(), Falcor::Resource::State::NonPixelShader);

        // -- the per-cluster arguments -----------------------------------
        std::vector<NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_TRIANGLE_CLUSTER_ARGS>
            args(nClusters);
        const uint64_t vbase = verts->getGpuAddress();
        for (uint32_t c = 0; c < nClusters; ++c) {
            const uint32_t firstQuad = c * kQuadsPerCluster;
            const uint32_t quads = (quadCount - firstQuad < kQuadsPerCluster)
                                       ? (quadCount - firstQuad)
                                       : kQuadsPerCluster;
            auto &a = args[c];
            a = {};
            a.clusterId = c;
            a.clusterFlags = 0;
            a.triangleCount = quads * 2;
            a.vertexCount = quads * 4;
            a.positionTruncateBitCount = 0;
            a.indexFormat = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INDEX_FORMAT_8BIT;
            a.baseGeometryIndexAndFlags = 0;
            a.indexBufferStride = 0;  // zero means: use the index size
            a.vertexBufferStride = uint16_t(sizeof(float) * 3);
            // THE VERTEX BUFFER IS SLICED, NOT COPIED. Because the mesher never
            // shares a vertex, cluster c owns exactly the run starting at
            // firstQuad*4 -- so a cluster's vertex buffer is the chunk's own, at
            // an offset, and not one float is duplicated.
            a.vertexBuffer = vbase + uint64_t(firstQuad) * 4ull * (sizeof(float) * 3);
            a.indexBuffer = sharedIdx_->getGpuAddress();
        }
        auto argBuf = upload(args.data(), args.size() * sizeof(args[0]), "v7::clasArgs");

        // -- how much memory the driver wants for the CLAS ---------------
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS in = {};
        in.maxArgCount = nClusters;
        in.flags = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_NONE;
        in.type =
            NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        // EXPLICIT, NOT IMPLICIT, AND THE REASON IS EMPIRICAL.
        //
        // Implicit destinations -- driver packs the CLAS wherever it likes and
        // reports the addresses -- looked like the obvious choice: no CPU has to
        // know where anything went. The driver refuses it here with a bare
        // NvAPI_Status -1, while the SAME inputs and args in GET_SIZES mode
        // succeed, and every size and alignment checks out (17280 bytes of
        // result, 128 of scratch, everything 256-aligned). The documentation
        // also only ever describes GET_SIZES results as feeding EXPLICIT, which
        // suggests explicit is the path that is actually exercised.
        //
        // Explicit costs nothing here anyway. The requirements query returns a
        // PER-OBJECT maximum in this mode, so v7 can lay the CLAS out itself at
        // a fixed stride and hand over the addresses -- which it then already
        // knows, without the readback that implicit mode was supposed to avoid.
        in.mode = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_EXPLICIT_DESTINATIONS;
        in.trianglesDesc.vertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        in.trianglesDesc.maxGeometryIndexValue = 0;
        in.trianglesDesc.maxUniqueGeometryCountPerArg = 1;
        in.trianglesDesc.maxTriangleCountPerArg = kTrisPerCluster;
        in.trianglesDesc.maxVertexCountPerArg = kVertsPerCluster;
        in.trianglesDesc.maxTotalTriangleCount = quadCount * 2;
        in.trianglesDesc.maxTotalVertexCount = quadCount * 4;
        in.trianglesDesc.minPositionTruncateBitCount = 0;

        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO req = {};
        if (!requirements(in, req)) { buildNote_ = "CLAS requirements query refused"; return out; }

        // -- BISECTION: does the driver survive the SAFEST possible call? ---
        //
        // GET_SIZES is the one mode the documentation says may be handed null
        // vertex and index pointers -- it derives a conservative size from the
        // counts alone. So it exercises the inputs struct, the args stride, the
        // scratch and the result-size array, while touching NONE of the
        // per-cluster addresses.
        //
        // If this removes the device, the fault is in the operation inputs. If
        // it survives and the real build does not, the fault is in the
        // per-cluster args -- the vertex slices or the shared index buffer.
        // That is a two-way split of everything that could be wrong, for the
        // cost of one extra dispatch on a path that only runs in the self test.
        if (bisect_) {
            NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS gin = in;
            gin.mode = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_GET_SIZES;
            NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO greq = {};
            if (!requirements(gin, greq)) {
                bisectNote_ = "GET_SIZES requirements query refused";
                return out;
            }
            auto gScratch = device_->createBuffer(greq.scratchDataSizeInBytes ? greq.scratchDataSizeInBytes : 1,
                                                  Falcor::ResourceBindFlags::UnorderedAccess);
            auto gSizes = device_->createBuffer(uint64_t(nClusters) * 4,
                                                Falcor::ResourceBindFlags::UnorderedAccess);
            ctx->resourceBarrier(gScratch.get(), Falcor::Resource::State::UnorderedAccess);
            ctx->resourceBarrier(gSizes.get(), Falcor::Resource::State::UnorderedAccess);

            NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC gdesc = {};
            gdesc.inputs = gin;
            gdesc.addressResolutionFlags = 0;
            gdesc.batchResultData = 0;
            gdesc.batchScratchData = gScratch->getGpuAddress();
            gdesc.resultSizeArray = {gSizes->getGpuAddress(), 4};
            gdesc.indirectArgArray = {argBuf->getGpuAddress(), sizeof(args[0])};
            gdesc.indirectArgCount = 0;
            if (!execute(cmd, gdesc)) {
                bisectNote_ = "GET_SIZES execute refused";
                return out;
            }
            ctx->submit(true);
            bisectNote_ = "GET_SIZES survived";
        }

        using Falcor::ResourceBindFlags;
        // Per-object in explicit mode, rounded up to the CLAS alignment so that
        // laying them end to end keeps every one of them legally placed.
        const uint64_t clasStride =
            ((req.resultDataMaxSizeInBytes + NVAPI_D3D12_RAYTRACING_CLAS_BYTE_ALIGNMENT - 1) /
             NVAPI_D3D12_RAYTRACING_CLAS_BYTE_ALIGNMENT) *
            NVAPI_D3D12_RAYTRACING_CLAS_BYTE_ALIGNMENT;
        out.clasStorage = device_->createBuffer(clasStride * nClusters ? clasStride * nClusters : 1,
                                                ResourceBindFlags::AccelerationStructure);
        auto clasScratch = device_->createBuffer(req.scratchDataSizeInBytes ? req.scratchDataSizeInBytes : 1,
                                                 ResourceBindFlags::UnorderedAccess);
        auto clasSizes = device_->createBuffer(uint64_t(nClusters) * 4, ResourceBindFlags::UnorderedAccess);

        // v7 chooses where each CLAS goes, so the addresses are known here and
        // uploaded rather than reported back.
        std::vector<uint64_t> clasVAs(nClusters);
        for (uint32_t c = 0; c < nClusters; ++c)
            clasVAs[c] = out.clasStorage->getGpuAddress() + uint64_t(c) * clasStride;
        auto clasAddrs = upload(clasVAs.data(), clasVAs.size() * 8, "v7::clasAddrs");

        // The states the API demands, spelled out. It is handed raw addresses
        // and cannot know which resources they belong to, so nothing about
        // these transitions can be inferred -- exactly the trap recordBuild in
        // world.h documents for ordinary builds.
        // NO BARRIER ON THE ACCELERATION-STRUCTURE BUFFERS, deliberately.
        //
        // A D3D12 buffer used to hold an acceleration structure must be CREATED
        // in D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE and can
        // never be transitioned into or out of it. ResourceBindFlags::
        // AccelerationStructure is what arranges that, and world.h's own BLAS
        // pool relies on exactly the same thing -- it allocates from rawPool_
        // and never barriers the result either.
        //
        // Asking for the transition anyway is not a no-op: it is an illegal
        // barrier, and NVAPI refuses the whole operation with a bare
        // NvAPI_Status -1 rather than saying which field it disliked.
        ctx->resourceBarrier(argBuf.get(), Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(clasScratch.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(clasAddrs.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(clasSizes.get(), Falcor::Resource::State::UnorderedAccess);

        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC desc = {};
        desc.inputs = in;
        desc.addressResolutionFlags = 0;
        desc.batchResultData = out.clasStorage->getGpuAddress();
        desc.batchScratchData = clasScratch->getGpuAddress();
        desc.destinationAddressArray = {clasAddrs->getGpuAddress(), 8};
        desc.resultSizeArray = {clasSizes->getGpuAddress(), 4};
        desc.indirectArgArray = {argBuf->getGpuAddress(), sizeof(args[0])};
        desc.indirectArgCount = 0;  // zero means: use maxArgCount
        if (!execute(cmd, desc)) {
            // The numbers, because four plausible causes all present as -1.
            char b[256];
            std::snprintf(b, sizeof(b),
                          "CLAS build refused (%s) result=%llu scratch=%llu "
                          "resultAlign=%llu scratchAlign=%llu argsAlign=%llu",
                          lastNv_.c_str(), (unsigned long long)req.resultDataMaxSizeInBytes,
                          (unsigned long long)req.scratchDataSizeInBytes,
                          (unsigned long long)(out.clasStorage->getGpuAddress() & 255ull),
                          (unsigned long long)(clasScratch->getGpuAddress() & 255ull),
                          (unsigned long long)(argBuf->getGpuAddress() & 255ull));
            buildNote_ = b;
            return out;
        }

        // The CLAS addresses are written by the operation above and read by the
        // one below, both on the GPU, so the second has to wait for the first.
        ctx->uavBarrier(clasAddrs.get());
        ctx->uavBarrier(out.clasStorage.get());

        // AND IT CHANGES ROLE. For the CLAS build this array was a destination
        // -- UNORDERED_ACCESS, written by the driver. For the BLAS build it is
        // an INPUT: blasArg.clusterVAs points at it and the operation reads it,
        // which the API requires to be NON_PIXEL_SHADER_RESOURCE. A UAV barrier
        // orders the write against the read but does not change the state, and
        // leaving it wrong is the same undefined read as above.
        ctx->resourceBarrier(clasAddrs.get(), Falcor::Resource::State::NonPixelShader);

        // -- and now one BLAS out of all those CLAS ----------------------
        NVAPI_D3D12_RAYTRACING_ACCELERATION_STRUCTURE_MULTI_INDIRECT_CLUSTER_ARGS blasArg = {};
        blasArg.clusterCount = nClusters;
        blasArg.reserved = 0;
        blasArg.clusterVAs = clasAddrs->getGpuAddress();
        auto blasArgBuf = upload(&blasArg, sizeof(blasArg), "v7::blasArgs");

        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS bin = {};
        bin.maxArgCount = 1;
        bin.flags = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_NONE;
        bin.type = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        // EXPLICIT this time, and that is the whole reason the BLAS address is
        // knowable without a readback: v7 says where the result goes, so the
        // address that ends up in the instance desc is one it already had.
        // Implicit destinations would leave it on the GPU and force a stall to
        // learn a number we could simply have chosen.
        bin.mode = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_EXPLICIT_DESTINATIONS;
        bin.clasDesc.maxTotalClasCount = nClusters;
        bin.clasDesc.maxClasCountPerArg = nClusters;

        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO breq = {};
        if (!requirements(bin, breq)) { buildNote_ = "BLAS requirements query refused"; return out; }

        out.blasStorage = device_->createBuffer(breq.resultDataMaxSizeInBytes ? breq.resultDataMaxSizeInBytes : 1,
                                                ResourceBindFlags::AccelerationStructure);
        auto blasScratch = device_->createBuffer(breq.scratchDataSizeInBytes ? breq.scratchDataSizeInBytes : 1,
                                                 ResourceBindFlags::UnorderedAccess);
        const uint64_t blasVa = out.blasStorage->getGpuAddress();
        auto blasAddr = upload(&blasVa, sizeof(blasVa), "v7::blasAddr");
        auto blasSize = device_->createBuffer(4, ResourceBindFlags::UnorderedAccess);

        ctx->resourceBarrier(blasArgBuf.get(), Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(blasScratch.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(blasAddr.get(), Falcor::Resource::State::UnorderedAccess);
        ctx->resourceBarrier(blasSize.get(), Falcor::Resource::State::UnorderedAccess);

        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC bdesc = {};
        bdesc.inputs = bin;
        bdesc.addressResolutionFlags = 0;
        bdesc.batchResultData = 0;  // ignored for explicit destinations
        bdesc.batchScratchData = blasScratch->getGpuAddress();
        bdesc.destinationAddressArray = {blasAddr->getGpuAddress(), 8};
        bdesc.resultSizeArray = {blasSize->getGpuAddress(), 4};
        bdesc.indirectArgArray = {blasArgBuf->getGpuAddress(), sizeof(blasArg)};
        bdesc.indirectArgCount = 0;
        if (!execute(cmd, bdesc)) { buildNote_ = "BLAS-from-CLAS build refused (" + lastNv_ + ")"; return out; }

        ctx->uavBarrier(out.blasStorage.get());
        out.address = blasVa;
#else
        (void)ctx;
        (void)verts;
        (void)quadCount;
#endif
        return out;
    }

    // -------------------------------------------------------------------
    // Build one cluster BLAS out of a synthetic mesh and see whether an
    // address comes back.
    //
    // WHY THIS EXISTS. The capability query says the driver CAN do cluster
    // operations; it says nothing about whether the six structures this file
    // fills in are filled in correctly. Those are large, mostly-reserved, and
    // full of bitfields with documented maxima -- exactly the shape of API
    // where a wrong field is accepted silently and produces a BLAS that traces
    // nothing. So one is built, at startup, from a mesh whose shape is known,
    // before the renderer is allowed to depend on any of it.
    //
    // The mesh is laid out the way VoxMesh lays one out -- four unshared
    // vertices per quad, in order -- because that assumption is what the whole
    // partitioning rests on. Testing with anything else would prove the wrong
    // thing.
    // -------------------------------------------------------------------
    bool selfTest(Falcor::RenderContext *ctx) {
#if V7_HAS_NVAPI
        if (!ready_) return false;

        // Two and a bit clusters' worth, so the partial last cluster -- the
        // case with a triangle count that is not the maximum -- is exercised
        // rather than assumed.
        const uint32_t quads = kQuadsPerCluster * 2 + 7;
        std::vector<float> pos;
        pos.reserve(quads * 4 * 3);
        for (uint32_t q = 0; q < quads; ++q) {
            const float x = float(q % 16), z = float(q / 16);
            const float v[4][3] = {{x, 0.f, z}, {x + 1.f, 0.f, z},
                                   {x + 1.f, 0.f, z + 1.f}, {x, 0.f, z + 1.f}};
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 3; ++k) pos.push_back(v[i][k]);
        }

        auto vb = device_->createBuffer(pos.size() * sizeof(float),
                                        Falcor::ResourceBindFlags::ShaderResource,
                                        Falcor::MemoryType::DeviceLocal, pos.data());
        if (!vb) {
            status_ += " -- self test could not allocate";
            return false;
        }
        ctx->resourceBarrier(vb.get(), Falcor::Resource::State::NonPixelShader);

        const ClusterBlas b = build(ctx, vb.get(), quads);
        // Submitted and waited on: the operations were recorded onto the
        // context's list and nothing has executed yet. Only correct because
        // this runs once, at startup, where a flush costs nothing.
        ctx->submit(true);

        if (!b.valid()) {
            status_ += " -- but the self test failed to produce a BLAS";
            return false;
        }
        selfTestClusters_ = b.clusterCount;
        return true;
#else
        (void)ctx;
        return false;
#endif
    }

    uint32_t selfTestClusters() const { return selfTestClusters_; }
    const std::string &bisectNote() const { return bisectNote_; }
    const std::string &buildNote() const { return buildNote_; }
    void setBisect(bool b) { bisect_ = b; }

  private:
    uint32_t selfTestClusters_ = 0;
    bool bisect_ = false;
    std::string buildNote_ = "not run";
    std::string bisectNote_ = "not run";
#if V7_HAS_NVAPI
    static ID3D12GraphicsCommandList *nativeCmdList(Falcor::RenderContext *ctx) {
        return ctx->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList *>();
    }

    // ---------------------------------------------------------------------
    // The one index buffer every cluster in the engine shares.
    //
    // 64 quads, 8-bit, 384 bytes: {4n, 4n+1, 4n+2, 4n, 4n+2, 4n+3} for n in
    // 0..63. See the note on partitioning above for why this is the same bytes
    // for every cluster -- and why 255 is the largest value it can ever hold,
    // which is what makes 8-bit indices legal rather than merely tempting.
    // ---------------------------------------------------------------------
    bool ensureSharedIndices() {
        if (sharedIdx_) return true;
        std::vector<uint8_t> idx;
        idx.reserve(kQuadsPerCluster * 6);
        for (uint32_t n = 0; n < kQuadsPerCluster; ++n) {
            const uint8_t b = uint8_t(n * 4);
            idx.push_back(b);
            idx.push_back(uint8_t(b + 1));
            idx.push_back(uint8_t(b + 2));
            idx.push_back(b);
            idx.push_back(uint8_t(b + 2));
            idx.push_back(uint8_t(b + 3));
        }
        sharedIdx_ = device_->createBuffer(idx.size(), Falcor::ResourceBindFlags::ShaderResource,
                                           Falcor::MemoryType::DeviceLocal, idx.data());
        if (!sharedIdx_) return false;
        sharedIdx_->setName("v7::clusterIndices");
        return true;
    }

    Falcor::ref<Falcor::Buffer> upload(const void *data, size_t bytes, const char *name) {
        auto b = device_->createBuffer(bytes, Falcor::ResourceBindFlags::ShaderResource,
                                       Falcor::MemoryType::DeviceLocal, data);
        if (b) b->setName(name);
        return b;
    }

    bool requirements(const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS &in,
                      NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO &out) {
        ID3D12Device5 *dev5 = nullptr;
        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d || FAILED(d3d->QueryInterface(IID_PPV_ARGS(&dev5)))) return false;
        NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS p = {};
        p.version = NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER;
        p.pInput = &in;
        p.pInfo = &out;
        const NvAPI_Status st =
            NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo(dev5, &p);
        dev5->Release();
        return st == NVAPI_OK;
    }

    // NVAPI wants a CommandList4 -- the DXR-era interface -- and Falcor hands
    // out the base one. Queried rather than cast: the base pointer really is a
    // CommandList4 on any device that got this far, but a reinterpret_cast
    // would be the same mistake NativeHandle::as<T> makes elsewhere in this
    // engine, and QueryInterface is the version that can say no.
    bool execute(ID3D12GraphicsCommandList *cmd,
                        const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC &desc) {
        ID3D12GraphicsCommandList4 *cmd4 = nullptr;
        if (!cmd || FAILED(cmd->QueryInterface(IID_PPV_ARGS(&cmd4)))) return false;
        NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS p = {};
        p.version = NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS_VER;
        p.pDesc = &desc;
        const NvAPI_Status st = NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation(cmd4, &p);
        cmd4->Release();
        lastNv_ = "NvAPI_Status " + std::to_string(int(st));
        return st == NVAPI_OK;
    }

    Falcor::ref<Falcor::Buffer> sharedIdx_;
    std::string lastNv_ = "no call made";
#endif

    Falcor::ref<Falcor::Device> device_;
    bool ready_ = false, clusters_ = false, ptlas_ = false;
    std::string status_ = "not initialised";
};

}  // namespace v7
