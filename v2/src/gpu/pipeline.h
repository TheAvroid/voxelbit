// ---------------------------------------------------------------------------
// pipeline.h -- the OptiX context, module, programs and shader binding table.
//
// Mostly boilerplate, with three decisions in it worth reading:
//
// MAX TRACE DEPTH IS ONE. Not the path length -- the number of optixTrace calls
// that can be nested. Because the path loop lives in raygen and closest-hit
// never traces anything itself, no trace is ever inside another, however many
// bounces a path takes. OptiX sizes the continuation stack from this number, so
// getting it right is the difference between a few hundred bytes per thread and
// a few thousand; at two million threads in flight that is the difference
// between fitting in cache-friendly local memory and not.
//
// THE SBT HAS ONE HITGROUP RECORD PER GEOMETRY AND A STRIDE OF ONE. The usual
// OptiX layout gives each geometry one record per ray type, so a shadow ray can
// find a different program. v2's shadow rays disable closest-hit entirely and
// resolve in the miss program, so there is no second program to find and no
// second record to store: the stride stays 1 and the instance's sbtOffset is
// just the geometry index. Eleven records for the whole wood.
//
// THE MODULE IS LOADED FROM DISK AS OPTIX-IR, not embedded as a string. Keeping
// v2.optixir a real file beside the exe means a shader change is one nvcc call
// and a relaunch, with no host relink -- which during the week this integrator
// was being tuned was worth more than the tidiness of a single binary.
// ---------------------------------------------------------------------------
#pragma once

#include <optix.h>
#include <optix_stack_size.h>
#include <optix_stubs.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "../optix/params.h"
#include "cuda_util.h"

namespace v2 {

template <typename T>
struct alignas(OPTIX_SBT_RECORD_ALIGNMENT) SbtRecord {
    char header[OPTIX_SBT_RECORD_HEADER_SIZE];
    T data;
};

struct EmptyRecord {};
using RaygenRecord = SbtRecord<EmptyRecord>;
using MissRecord = SbtRecord<EmptyRecord>;
using HitRecord = SbtRecord<HitGroupData>;

inline void optixLogCallback(unsigned int level, const char *tag, const char *message, void *) {
    // Level 1 is fatal, 2 error, 3 warning, 4 print. Warnings from the OptiX
    // compiler are worth seeing: they are usually the reason a pipeline that
    // linked cleanly then renders black.
    if (level <= 3) std::fprintf(stderr, "v2: optix [%s] %s\n", tag ? tag : "?", message);
}

class Pipeline {
  public:
    ~Pipeline() { release(); }

    OptixDeviceContext context() const { return ctx_; }
    OptixPipeline pipeline() const { return pipeline_; }
    const OptixShaderBindingTable &sbt() const { return sbt_; }

    // -----------------------------------------------------------------------
    // A CUDA context on device 0, and an OptiX context on top of it.
    // -----------------------------------------------------------------------
    void initContext(bool validation) {
        CU_CHECK(cuInit(0));
        CU_CHECK(cuDeviceGet(&device_, 0));
        // The PRIMARY context, not a new one: it is the context the driver
        // hands to anything else that touches this device, which keeps the
        // door open for a future graphics interop without a context switch
        // on every frame.
        CU_CHECK(cuDevicePrimaryCtxRetain(&cuCtx_, device_));
        CU_CHECK(cuCtxSetCurrent(cuCtx_));
        CU_CHECK(cuStreamCreate(&stream_, CU_STREAM_DEFAULT));

        OPTIX_CHECK(optixInit());

        OptixDeviceContextOptions opts = {};
        opts.logCallbackFunction = &optixLogCallback;
        opts.logCallbackLevel = 4;
        if (validation) opts.validationMode = OPTIX_DEVICE_CONTEXT_VALIDATION_MODE_ALL;
        OPTIX_CHECK(optixDeviceContextCreate(cuCtx_, &opts, &ctx_));
    }

    CUstream stream() const { return stream_; }

    void deviceName(char *out, int bytes) const { cuDeviceGetName(out, bytes, device_); }

    size_t freeVideoMemory() const {
        size_t freeB = 0, totalB = 0;
        cuMemGetInfo(&freeB, &totalB);
        return freeB;
    }

    // -----------------------------------------------------------------------
    void buildPipeline(const std::string &optixIrPath, int maxDepthHint) {
        std::vector<char> ir = readFile(optixIrPath);

        OptixModuleCompileOptions mco = {};
        mco.maxRegisterCount = OPTIX_COMPILE_DEFAULT_MAX_REGISTER_COUNT;
        mco.optLevel = OPTIX_COMPILE_OPTIMIZATION_LEVEL_3;
        mco.debugLevel = OPTIX_COMPILE_DEBUG_LEVEL_NONE;

        pco_ = {};
        pco_.usesMotionBlur = 0;
        // The scene is exactly one IAS over many GASes, and saying so lets
        // OptiX generate traversal for that shape alone instead of the general
        // any-graph walker.
        pco_.traversableGraphFlags = OPTIX_TRAVERSABLE_GRAPH_FLAG_ALLOW_SINGLE_LEVEL_INSTANCING;
        pco_.numPayloadValues = 2;    // a packed pointer, or the occlusion flag
        pco_.numAttributeValues = 2;  // built-in triangle barycentrics
        pco_.exceptionFlags = OPTIX_EXCEPTION_FLAG_NONE;
        pco_.pipelineLaunchParamsVariableName = "params";
        pco_.usesPrimitiveTypeFlags = OPTIX_PRIMITIVE_TYPE_FLAGS_TRIANGLE;

        char log[4096];
        size_t logSize = sizeof(log);
        OptixResult r = optixModuleCreate(ctx_, &mco, &pco_, ir.data(), ir.size(), log, &logSize,
                                          &module_);
        if (r != OPTIX_SUCCESS) {
            std::fprintf(stderr, "v2: module log: %s\n", log);
            OPTIX_CHECK(r);
        }

        // -- program groups --------------------------------------------------
        OptixProgramGroupOptions pgo = {};

        OptixProgramGroupDesc rgDesc = {};
        rgDesc.kind = OPTIX_PROGRAM_GROUP_KIND_RAYGEN;
        rgDesc.raygen.module = module_;
        rgDesc.raygen.entryFunctionName = "__raygen__pinhole";
        makeGroup(rgDesc, pgo, &raygenPg_);

        OptixProgramGroupDesc msDesc = {};
        msDesc.kind = OPTIX_PROGRAM_GROUP_KIND_MISS;
        msDesc.miss.module = module_;
        msDesc.miss.entryFunctionName = "__miss__radiance";
        makeGroup(msDesc, pgo, &missPg_);

        msDesc.miss.entryFunctionName = "__miss__shadow";
        makeGroup(msDesc, pgo, &missShadowPg_);

        OptixProgramGroupDesc hgDesc = {};
        hgDesc.kind = OPTIX_PROGRAM_GROUP_KIND_HITGROUP;
        hgDesc.hitgroup.moduleCH = module_;
        hgDesc.hitgroup.entryFunctionNameCH = "__closesthit__radiance";
        makeGroup(hgDesc, pgo, &hitPg_);

        // -- link -------------------------------------------------------------
        OptixProgramGroup groups[] = {raygenPg_, missPg_, missShadowPg_, hitPg_};

        OptixPipelineLinkOptions plo = {};
        plo.maxTraceDepth = 1;  // see the header note: the path loop is in raygen

        logSize = sizeof(log);
        r = optixPipelineCreate(ctx_, &pco_, &plo, groups, 4, log, &logSize, &pipeline_);
        if (r != OPTIX_SUCCESS) {
            std::fprintf(stderr, "v2: link log: %s\n", log);
            OPTIX_CHECK(r);
        }

        setStackSizes(groups, 4);
        (void)maxDepthHint;
    }

    // -----------------------------------------------------------------------
    // The shader binding table.
    //
    // Rebuilt only when the geometry changes, which for a static wood means
    // once. The hitgroup payloads come straight from the scene, in the order
    // the instances' sbtOffsets expect.
    // -----------------------------------------------------------------------
    void buildSbt(const std::vector<HitGroupData> &hitGroups) {
        RaygenRecord rg = {};
        OPTIX_CHECK(optixSbtRecordPackHeader(raygenPg_, &rg));
        raygenBuf_.upload(&rg, 1);

        MissRecord miss[2] = {};
        OPTIX_CHECK(optixSbtRecordPackHeader(missPg_, &miss[RAY_RADIANCE]));
        OPTIX_CHECK(optixSbtRecordPackHeader(missShadowPg_, &miss[RAY_SHADOW]));
        missBuf_.upload(miss, 2);

        std::vector<HitRecord> hits(hitGroups.size());
        for (size_t i = 0; i < hitGroups.size(); ++i) {
            OPTIX_CHECK(optixSbtRecordPackHeader(hitPg_, &hits[i]));
            hits[i].data = hitGroups[i];
        }
        hitBuf_.upload(hits);

        sbt_ = {};
        sbt_.raygenRecord = raygenBuf_.ptr();
        sbt_.missRecordBase = missBuf_.ptr();
        sbt_.missRecordStrideInBytes = sizeof(MissRecord);
        sbt_.missRecordCount = 2;
        sbt_.hitgroupRecordBase = hitBuf_.ptr();
        sbt_.hitgroupRecordStrideInBytes = sizeof(HitRecord);
        sbt_.hitgroupRecordCount = unsigned(hits.size());
    }

    // -----------------------------------------------------------------------
    // Rewrite ONE hitgroup record in place.
    //
    // A chunk taking an SBT slot changes 32 bytes of a table with a few hundred
    // entries in it. Rebuilding and re-uploading the whole table for that would
    // be a needless round trip every time the ring moves, and the ring moves
    // whenever the camera crosses 25 metres.
    // -----------------------------------------------------------------------
    void writeHitRecord(unsigned index, const HitGroupData &d) {
        if (index >= sbt_.hitgroupRecordCount) return;
        HitRecord rec = {};
        OPTIX_CHECK(optixSbtRecordPackHeader(hitPg_, &rec));
        rec.data = d;
        CU_CHECK(cuMemcpyHtoD(hitBuf_.ptr() + size_t(index) * sizeof(HitRecord), &rec,
                              sizeof(HitRecord)));
    }

    void release() {
        if (pipeline_) { optixPipelineDestroy(pipeline_); pipeline_ = nullptr; }
        if (hitPg_) { optixProgramGroupDestroy(hitPg_); hitPg_ = nullptr; }
        if (missShadowPg_) { optixProgramGroupDestroy(missShadowPg_); missShadowPg_ = nullptr; }
        if (missPg_) { optixProgramGroupDestroy(missPg_); missPg_ = nullptr; }
        if (raygenPg_) { optixProgramGroupDestroy(raygenPg_); raygenPg_ = nullptr; }
        if (module_) { optixModuleDestroy(module_); module_ = nullptr; }
        if (ctx_) { optixDeviceContextDestroy(ctx_); ctx_ = nullptr; }
        if (stream_) { cuStreamDestroy(stream_); stream_ = nullptr; }
        if (cuCtx_) { cuDevicePrimaryCtxRelease(device_); cuCtx_ = nullptr; }
    }

  private:
    CUdevice device_ = 0;
    CUcontext cuCtx_ = nullptr;
    CUstream stream_ = nullptr;
    OptixDeviceContext ctx_ = nullptr;
    OptixModule module_ = nullptr;
    OptixPipelineCompileOptions pco_ = {};
    OptixProgramGroup raygenPg_ = nullptr, missPg_ = nullptr, missShadowPg_ = nullptr,
                      hitPg_ = nullptr;
    OptixPipeline pipeline_ = nullptr;
    OptixShaderBindingTable sbt_ = {};
    DeviceBuffer raygenBuf_, missBuf_, hitBuf_;

    void makeGroup(const OptixProgramGroupDesc &desc, const OptixProgramGroupOptions &opts,
                   OptixProgramGroup *out) {
        char log[2048];
        size_t logSize = sizeof(log);
        OptixResult r = optixProgramGroupCreate(ctx_, &desc, 1, &opts, log, &logSize, out);
        if (r != OPTIX_SUCCESS) {
            std::fprintf(stderr, "v2: program group log: %s\n", log);
            OPTIX_CHECK(r);
        }
    }

    void setStackSizes(OptixProgramGroup *groups, int count) {
        OptixStackSizes ss = {};
        for (int i = 0; i < count; ++i)
            OPTIX_CHECK(optixUtilAccumulateStackSizes(groups[i], &ss, pipeline_));

        unsigned int fromTraversal = 0, fromState = 0, continuation = 0;
        OPTIX_CHECK(optixUtilComputeStackSizes(&ss, /*maxTraceDepth=*/1, /*maxCCDepth=*/0,
                                               /*maxDCDepth=*/0, &fromTraversal, &fromState,
                                               &continuation));
        // Graph depth 2: the IAS, then the GAS under it. Saying 2 rather than
        // the default 3 saves a traversal stack entry on every ray.
        OPTIX_CHECK(optixPipelineSetStackSize(pipeline_, fromTraversal, fromState, continuation,
                                              /*maxTraversableGraphDepth=*/2));
    }

    static std::vector<char> readFile(const std::string &path) {
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f) throw std::runtime_error("v2: cannot open " + path);
        const std::streamsize n = f.tellg();
        f.seekg(0, std::ios::beg);
        std::vector<char> buf(static_cast<size_t>(n));
        if (n > 0 && !f.read(buf.data(), n)) throw std::runtime_error("v2: cannot read " + path);
        return buf;
    }
};

}  // namespace v2
