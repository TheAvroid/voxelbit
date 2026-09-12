// ---------------------------------------------------------------------------
// ser.h -- Shader Execution Reordering, turned on for this device.
//
// WHAT SER IS, AND WHY IT IS THE LAST LEVER LEFT.
//
// A GPU runs threads in warps of thirty-two, in lockstep. When those threads
// finish a ray query and then branch -- hit against miss, one material against
// another -- the warp executes EVERY branch in turn with most of its lanes
// switched off. That is divergence, and in a path tracer it is usually the
// largest cost after memory.
//
// SER lets the shader say, at one point: reorder these threads into new warps,
// grouped by this key. The hardware shuffles lanes so that threads about to do
// the same work end up together, and the divergent part then runs with full
// warps. Ada does it in silicon.
//
// IT IS THE LAST LEVER BECAUSE NO REPRESENTATION CAN REACH IT. Every storage
// change measured in this engine moved memory and not trace -- word-sparse
// masks, per-brick palettes, uniform merging, dedup -- and the one that did move
// trace, 4^3 bricks, cost three times the memory. Divergence is not something a
// store can be shaped to avoid; it is a property of what the shader does AFTER
// the store has answered.
//
// ---------------------------------------------------------------------------
// AND IT DOES NOT WORK HERE. READ THIS BEFORE SPENDING A DAY ON IT.
//
// **NvReorderThread is only available in shaders of type RAYGENERATION.** It
// cannot be used from a compute shader, and inline ray tracing is not a
// raygeneration context. This engine traces from Trace.cs.slang -- a COMPUTE
// shader using RayQuery -- so the intrinsic compiles, the extension is live,
// and executing it is an access violation at the first dispatch.
//
// EVERYTHING ELSE IN THIS FILE IS CORRECT AND WAS PROVED SO. The caps query
// says the 4070 can reorder. Falcor builds the pipeline through
// NvAPI_D3D12_CreateComputePipelineState -- `g_NvidiaExt in reflection: YES`
// from the probe in tracer.h. The four shader defines are right. None of it
// matters: the restriction is on the SHADER STAGE, and no amount of plumbing
// changes what stage a compute shader is.
//
// WHAT IT WOULD TAKE: rewriting the tracer as a DXR raygeneration shader with a
// shader table -- which the note at the top of Trace.cs.slang says this engine
// deliberately does NOT do, because avoiding that dispatch is why everything
// runs from compute. SER is not a thing v4 can adopt; it is a thing a
// differently-shaped engine gets.
//
// So init() refuses, with that reason, and --ser cannot crash the process. The
// code is kept because the finding is worth more written down than deleted.
//
// ---------------------------------------------------------------------------
// THIS FILE DOES NOT REGISTER THE EXTENSION SLOT, AND THAT IS THE WHOLE POINT
// OF THE NOTE.
//
// It did, with NvAPI_D3D12_SetNvShaderExtnSlotSpace, on the belief that Falcor
// had no hook for COMPUTE pipelines -- its slot registration sits in
// beforeCreateRayTracingState, and this engine creates no ray tracing state
// objects because it traces from a compute shader with inline ray tracing.
//
// THAT WAS WRONG. Falcor also has `createComputePipelineState`, which calls
// NvAPI_D3D12_CreateComputePipelineState with an extension descriptor built
// from the reflected binding of `g_NvidiaExt`. That is the per-PSO mechanism,
// and it is the correct one.
//
// THE TWO MECHANISMS ARE ALTERNATIVES, NOT LAYERS. Setting the global slot
// space AND letting Falcor pass a per-PSO extension descriptor is not belt and
// braces; it crashed the process at pipeline creation. Falcor owns the
// registration; this file only asks the hardware whether it can reorder and
// gates the shader define.
//
// ---------------------------------------------------------------------------
// WHAT IS LEFT FOR THIS FILE, WHEN FALCOR ALREADY INTEGRATES NVAPI.
//
// Falcor's integration registers the extension slot in beforeCreateRayTracingState
// -- a hook on RAY TRACING STATE OBJECTS. This engine has none: it traces from a
// COMPUTE shader with inline ray tracing, so that hook never fires and Falcor's
// support, even switched on, would do nothing here.
//
// So v4 registers the slot ITSELF, with the ID3D12Device Falcor already made --
// the same cheap door src/gpu/clusters.h uses to reach NVAPI for cluster
// operations, and the reason FALCOR_HAS_NVAPI is left at 0 in CMakeLists.
//
// ---------------------------------------------------------------------------
// THE SLOT IS SET ONCE AND LEFT SET, AND THAT IS DELIBERATE.
//
// NvAPI_D3D12_SetNvShaderExtnSlotSpace is device state the driver reads when a
// PIPELINE IS CREATED, not when one is dispatched. Falcor creates its compute
// pipelines lazily, inside ComputePass, with no hook to wrap -- so the slot is
// set before any pass is built and never cleared. Anything compiled afterwards
// picks it up; nothing that does not use the intrinsics is affected by it.
//
// SET IT TOO LATE AND NOTHING FAILS. The pipeline is simply created without the
// extension, the intrinsic compiles to nothing useful, and the reordering
// silently does not happen -- which measures as "SER bought us zero". That is
// why init() runs before the tracer builds its programs and says so out loud.
// ---------------------------------------------------------------------------
#pragma once

#include <string>

#include "Core/API/Device.h"

#if V4_HAS_NVAPI
// DXGI BEFORE NVAPI, AND THAT IS NOT STYLE. nvapi.h declares functions taking
// IDXGISwapChain without declaring the type itself, so including it first gives
// a wall of "syntax error: identifier 'IDXGISwapChain'" pointing inside NVAPI
// rather than at whoever included it.
#include <d3d12.h>
#include <dxgi1_4.h>
#include <nvapi.h>
#endif

namespace v4 {

using Falcor::ref;
using Falcor::Device;

// The register and space the shader declares its extension UAV at. u999 is what
// Falcor's own integration uses and what every NVAPI sample uses; it is chosen
// to be far above anything a real binding would occupy.
constexpr uint32_t kSerExtnSlot = 999;
constexpr uint32_t kSerExtnSpace = 0;

class Ser {
  public:
    // Called BEFORE the tracer builds any program. See the note above about
    // what happens if it is called after: nothing, silently.
    bool init(const ref<Device> &device) {
        // THE STAGE, BEFORE ANYTHING ELSE. See the note at the top: the
        // intrinsic is raygeneration-only and this engine traces from compute,
        // so every check below would pass and the first dispatch would still
        // fault. Refusing here is what keeps --ser from crashing the process.
        status_ = "unavailable: NvReorderThread is raygeneration-only and this "
                  "engine traces from a compute shader";
        return false;
#if !V4_HAS_NVAPI
        status_ = "no NVAPI at build time -- reordering off";
        return false;
#else
        if (device->getType() != Device::Type::D3D12) {
            // The intrinsics are a D3D12 extension. On Vulkan the equivalent is
            // VK_NV_ray_tracing_invocation_reorder, which is a different API
            // and not wired here -- and this engine's Streamline integration is
            // D3D12-only anyway, so a Vulkan run has already given up more than
            // this.
            status_ = "Vulkan backend -- reordering is a D3D12 extension here";
            return false;
        }
        if (NvAPI_Initialize() != NVAPI_OK) {
            status_ = "NvAPI_Initialize failed -- reordering off";
            return false;
        }
        ID3D12Device *d3d = device->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "no native ID3D12Device -- reordering off";
            return false;
        }
        // ASK WHETHER THE HARDWARE CAN DO IT BEFORE CLAIMING IT DOES. Ada and
        // later report true; Ampere and earlier report false and the intrinsic
        // would compile and then reorder nothing, which is the worst outcome
        // because it looks like a measurement.
        bool supported = false;
        NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS caps =
            NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_NONE;
        if (NvAPI_D3D12_GetRaytracingCaps(d3d, NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING,
                                          &caps, sizeof(caps)) == NVAPI_OK) {
            supported = (caps & NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD) != 0;
        }
        if (!supported) {
            status_ = "this GPU reports no thread reordering -- off";
            return false;
        }
        // AND THAT IS ALL THIS FILE DOES. It does NOT register the extension
        // slot -- see the note at the top about why that was a mistake.
        ready_ = true;
        status_ = "on (Falcor creates the PSO through NvAPI)";
        return true;
#endif
    }

    // Whether the SHADER should compile the intrinsic in. Nothing in the render
    // path may branch on this at runtime -- it is a compile-time define, so a
    // program built without it has no reorder in it at all and costs nothing.
    bool ready() const { return ready_ && enabled_; }
    void setEnabled(bool on) { enabled_ = on; }
    const char *status() const { return ready_ && enabled_ ? status_.c_str()
                                 : ready_                 ? "available, switched off"
                                                          : status_.c_str(); }

  private:
    bool ready_ = false;
    bool enabled_ = true;
    std::string status_ = "not initialised";
};

}  // namespace v4
