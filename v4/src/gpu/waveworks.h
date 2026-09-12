// ---------------------------------------------------------------------------
// waveworks.h -- NVIDIA WaveWorks 2.0, for the lake surface.
//
// WHAT IS USED, AND WHAT CANNOT BE.
//
// WaveWorks is two halves. The first is an FFT wind-wave SIMULATION: it
// integrates an ocean spectrum on the GPU and publishes displacement, gradient
// and moment cascades as RGBA16F texture arrays. The second is a RENDERER: a
// quadtree of patches drawn through hardware tessellation, where a hull shader
// picks a tessellation factor and a domain shader displaces the vertices it
// generates (OceanSurfaceHS.hlsl / OceanSurfaceDS.hlsl in its sample).
//
// Only the first half can be used here, and that is not a shortcut taken for
// time. A path tracer has no tessellation stage: there is no vertex being
// generated at draw time to displace, because rays are fired at geometry that
// already exists in an acceleration structure. So the quadtree, the hull and
// domain shaders, and the whole LOD scheme go unused, and the displacement
// cascade is read by v2's own code instead -- see the water mesh in
// scene/voxelworld.h and the per-frame rebuild in gpu/world.h.
//
// IT IS NOT A STANDALONE D3D12 LIBRARY, WHICH THE LINKER FINDS FIRST.
// NVWaveWorks_static.d3d12.lib constructs an nvrhi::d3d12::Device around the
// ID3D12Device it is handed, so nvrhi_d3d12.lib has to be linked beside it.
// The one symbol that fails without it is that constructor, which is a
// confusing error to meet cold. Falcor brings its own abstraction over the same
// device; nvrhi is thin enough for the two to coexist, but there ARE two RHIs
// in this process now and only one of them owns the queue.
//
// THE LIBRARY IS PROPRIETARY: "Any use, reproduction, disclosure or
// distribution of this software ... without an express license agreement from
// NVIDIA CORPORATION is strictly prohibited", and the package ships no license
// file. external/ is gitignored so none of it is committed here.
//
// ABSENT, EVERYTHING STILL RUNS. Without V4_WAVEWORKS this class compiles to a
// few inline no-ops that report "not built", and the lake keeps the Gerstner
// surface it had. A missing SDK that turns into a wall of compiler errors is
// worse than one that is simply absent -- the same rule the RTXGI and Streamline
// blocks in CMakeLists.txt follow.
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>

#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"

#if V4_WAVEWORKS
#include <d3d12.h>

#include "GFSDK_WaveWorks.h"
#endif

namespace v4 {

// What the shader needs to turn a world position into a sample of the cascades.
// Mirrored into the trace shader's constants; kept as plain floats so it can be
// copied whether or not the SDK is present.
struct WaveWorksFrame {
    float uvScale[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float uvOffset[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float warpAmplitude = 0.0f;
    float warpFrequency = 0.0f;
    float cascadeToCascadeScale = 1.0f;
    float foamThreshold = 1.0f;
    uint32_t textureSize = 0;
    bool valid = false;
};

class WaveWorks {
  public:
    ~WaveWorks() { shutdown(); }

    // Whether the simulation exists and is producing cascades.
    bool ready() const { return ready_; }
    const std::string &status() const { return status_; }
    const WaveWorksFrame &frame() const { return frame_; }

#if V4_WAVEWORKS
    // THE WIND, AND WHY IT IS A LAKE'S WIND RATHER THAN AN OCEAN'S.
    //
    // WaveWorks models wind fetch over open sea; its Beaufort scale runs to
    // storm. These lakes are at most 134 m across and stand in a wood, which
    // shelters them. Driving the spectrum at ocean speeds on a tarn produces
    // swell with a wavelength longer than the lake is wide -- one slab of water
    // tilting, rather than a surface. Low wind and a short fetch is the shape
    // that fits; both are members so they can be swept without a rebuild.
    float windSpeed = 2.2f;      // m/s, not Beaufort -- see useBeaufort below
    float windDirX = 0.83f, windDirZ = 0.55f;
    float windFetchKm = 0.35f;   // a lake, not an ocean
    float amplitude = 0.6f;
    float timeScale = 1.0f;

    bool init(Falcor::ref<Falcor::Device> device) {
        device_ = device;
        if (!device_) {
            status_ = "no device";
            return false;
        }
        // ASK THE DEVICE WHAT IT IS, not what its handle looks like.
        // NativeHandle::as<T> reinterprets rather than checks, so on Vulkan a
        // VkDevice arrives here as a perfectly non-null ID3D12Device* and the
        // first call walks it through a D3D12 vtable. clusters.h carries the
        // same guard for the same reason.
        if (device_->getType() != Falcor::Device::Type::D3D12) {
            status_ = "not a D3D12 device (WaveWorks is linked against its d3d12 build)";
            return false;
        }
        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "no D3D12 device handle";
            return false;
        }
        ID3D12CommandQueue *queue = nativeQueue();
        if (!queue) {
            status_ = "no D3D12 command queue handle";
            return false;
        }

        GFSDK_WaveWorks_API_GUID guid = GFSDK_WAVEWORKS_API_GUID;
        if (GFSDK_WaveWorks_Init(nullptr, guid) != gfsdk_wwresult_OK) {
            status_ = "GFSDK_WaveWorks_Init failed";
            return false;
        }
        inited_ = true;

        GFSDK_WaveWorks_Wind_Waves_Simulation_Settings settings = {};
        // Normal detail is a 256-texel cascade. The surface here is a lake seen
        // from its own shore, not a horizon of ocean, and every extra level
        // costs an FFT per frame for detail finer than the 10 cm voxel the water
        // is quantised onto anyway.
        settings.detail_level = GFSDK_WaveWorks_Simulation_DetailLevel_Normal;
        settings.simulation_api = GFSDK_WaveWorks_Simulation_API_Compute;
        settings.simulation_period = 200.0f;
        settings.use_Beaufort_scale = false;   // windSpeed is m/s here
        // NO CPU READBACK. The displacement is consumed on the GPU by the mesh
        // rebuild; asking for the readback path as well would cost a copy back
        // over the bus every frame for data nothing on this side reads.
        // BOTH PATHS, and each is used for a different thing.
        //
        // The GPU cascade is what the trace shader samples for the surface
        // normal. The CPU path is what the MESHER needs: the water surface is
        // quantised to whole voxel rows before it becomes triangles, and that
        // decision happens on a worker thread building a mesh, nowhere near a
        // command list. Asking the GPU for it would mean a readback and a sync
        // inside chunk meshing, which is the one place that must not stall.
        settings.enable_CPU_driven_displacement_calculation = true;
        settings.enable_GPU_driven_displacement_calculation = true;
        settings.num_readback_FIFO_entries = 0;
        settings.CPU_simulation_threading_model =
            GFSDK_WaveWorks_Simulation_CPU_Threading_Model_Automatic;
        settings.num_GPUs = 1;
        settings.enable_GPU_timers = false;
        settings.enable_CPU_timers = false;
        settings_ = settings;

        if (GFSDK_WaveWorks_Wind_Waves_Simulation_CreateDirectX12(
                d3d, queue, /*enable_graphics*/ true, settings, params(), &sim_) !=
            gfsdk_wwresult_OK) {
            status_ = "Wind_Waves_Simulation_CreateDirectX12 failed";
            GFSDK_WaveWorks_Release();
            inited_ = false;
            return false;
        }
        ready_ = true;
        status_ = std::string("WaveWorks ") + GFSDK_WaveWorks_GetBuildString();
        return true;
    }

    // Advance the simulation and start its work for this frame. The cascades it
    // publishes are the PREVIOUS kick's until this one lands, which is exactly
    // what a pipelined simulation is for -- nothing here waits on it.
    void tick(double seconds) {
        if (!ready_) return;
        GFSDK_WaveWorks_Wind_Waves_Simulation_SetTime(sim_, seconds);
        uint64_t kick = 0;
        GFSDK_WaveWorks_Wind_Waves_Simulation_Kick(sim_, &kick);

        GFSDK_WaveWorks_Wind_Waves_Rendering_Data d = {};
        if (GFSDK_WaveWorks_Wind_Waves_GetDataForRendering(sim_, d) != gfsdk_wwresult_OK) return;
        frame_.uvScale[0] = d.cascade0_UV_scale;
        frame_.uvScale[1] = d.cascade1_UV_scale;
        frame_.uvScale[2] = d.cascade2_UV_scale;
        frame_.uvScale[3] = d.cascade3_UV_scale;
        frame_.uvOffset[0] = d.cascade0_UV_offset;
        frame_.uvOffset[1] = d.cascade1_UV_offset;
        frame_.uvOffset[2] = d.cascade2_UV_offset;
        frame_.uvOffset[3] = d.cascade3_UV_offset;
        frame_.warpAmplitude = d.uv_warping_amplitude;
        frame_.warpFrequency = d.uv_warping_frequency;
        frame_.cascadeToCascadeScale = d.cascade_to_cascade_scale;
        frame_.foamThreshold = d.foam_whitecaps_threshold;
        frame_.textureSize = d.size_of_texture_arrays;
        frame_.valid = d.displacements_texture_array != nullptr;
        displacements_ = static_cast<ID3D12Resource *>(d.displacements_texture_array);
        gradients_ = static_cast<ID3D12Resource *>(d.gradients_texture_array);
    }

    // -----------------------------------------------------------------------
    // THE SURFACE HEIGHT AT A SET OF POINTS, on the CPU.
    //
    // This is the whole input to the water mesh. WaveWorks returns a full 3D
    // displacement per point -- waves move water sideways as well as up, which
    // is what makes a Gerstner crest sharp -- but the mesh here is a heightfield
    // on a voxel lattice, so only y survives quantisation. The lateral part is
    // not wasted: it is still in the GPU cascade the shading normal samples, so
    // the surface LOOKS like it leans even though its voxels do not.
    //
    // calculateOnGPU is false deliberately. True routes the query through the
    // GPU and back, which is a sync; the CPU spectrum is already being
    // maintained for exactly this (see the settings) and costs no stall on a
    // worker thread.
    bool sampleHeights(const float *xz, float *outY, uint32_t count) const {
        if (!ready_ || count == 0) return false;
        auto *pts = reinterpret_cast<const gfsdk_float2 *>(xz);
        scratch_.resize(count);
        if (GFSDK_WaveWorks_Wind_Waves_Simulation_GetDisplacements(
                sim_, pts, scratch_.data(), count, /*calculateOnGPU*/ false) !=
            gfsdk_wwresult_OK)
            return false;
        for (uint32_t i = 0; i < count; ++i) outY[i] = scratch_[i].y;
        return true;
    }

    // THE CASCADES THEMSELVES, as the resources WaveWorks owns. Borrowed, never
    // released here -- they are recreated inside the library whenever the
    // settings change, so nothing may hold one across an UpdateProperties.
    ID3D12Resource *displacements() const { return displacements_; }
    ID3D12Resource *gradients() const { return gradients_; }

    // Push changed wind values without tearing the simulation down: WaveWorks
    // reinitialises only what actually changed, which a Destroy/Create pair
    // cannot know.
    void refreshParams() {
        if (!ready_) return;
        GFSDK_WaveWorks_Wind_Waves_Simulation_UpdateProperties(sim_, settings_, params());
    }

    // The largest crest the current spectrum can throw, in metres. The mesh
    // rebuild needs it to size the band of voxels a wave may occupy, and asking
    // the library beats guessing at the sum of the cascades.
    float maxDisplacement() const {
        if (!ready_) return 0.0f;
        return GFSDK_WaveWorks_Wind_Waves_Simulation_GetConservativeMaxDisplacementEstimate(sim_);
    }

    void shutdown() {
        if (ready_) {
            GFSDK_WaveWorks_Wind_Waves_Simulation_Destroy(sim_);
            ready_ = false;
        }
        if (inited_) {
            GFSDK_WaveWorks_Release();
            inited_ = false;
        }
        displacements_ = nullptr;
        gradients_ = nullptr;
        frame_ = {};
    }

  private:
    GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters params() const {
        GFSDK_WaveWorks_Wind_Waves_Simulation_Parameters p = {};
        p.base_wind_direction = {windDirX, windDirZ};
        p.base_wind_speed = windSpeed;
        p.base_wind_distance = windFetchKm;
        p.base_wind_dependency = 0.9f;
        p.base_spectrum_peaking = 1.0f;
        p.base_small_waves_cutoff_length = 0.0f;
        p.base_small_waves_cutoff_power = 0.0f;
        p.base_amplitude_multiplier = amplitude;
        // NO SWELL. Swell is wave energy that travelled in from somewhere else,
        // which is a thing an ocean has and a lake in a wood does not.
        p.swell_wind_direction = {windDirX, windDirZ};
        p.swell_wind_speed = 0.0f;
        p.swell_wind_distance = 0.0f;
        p.swell_wind_dependency = 0.0f;
        p.swell_spectrum_peaking = 1.0f;
        p.swell_small_waves_cutoff_length = 0.0f;
        p.swell_small_waves_cutoff_power = 0.0f;
        p.swell_amplitude_multiplier = 0.0f;
        p.lateral_multiplier = 1.0f;
        p.time_scale = timeScale;
        p.uv_warping_amplitude = 0.05f;
        p.uv_warping_frequency = 2.0f;
        p.foam_whitecaps_threshold = 1.0f;
        p.foam_generation_threshold = 1.0f;
        p.foam_generation_amount = 0.0f;
        p.foam_dissipation_speed = 1.0f;
        p.foam_falloff_speed = 1.0f;
        return p;
    }

    ID3D12CommandQueue *nativeQueue() const {
        // Falcor has exactly one, and it is the queue every list it records is
        // submitted to -- WaveWorks must share it or its compute lands out of
        // order with the frame that reads the cascades. nrcsdk.h reaches for it
        // the same way.
        gfx::ICommandQueue *q = device_->getGfxCommandQueue();
        if (!q) return nullptr;
        gfx::InteropHandle h = {};
        if (SLANG_FAILED(q->getNativeHandle(&h)) || h.handleValue == 0) return nullptr;
        return reinterpret_cast<ID3D12CommandQueue *>(h.handleValue);
    }

    Falcor::ref<Falcor::Device> device_;
    GFSDK_WaveWorks_Wind_Waves_SimulationHandle sim_ = nullptr;
    GFSDK_WaveWorks_Wind_Waves_Simulation_Settings settings_ = {};
    mutable std::vector<gfsdk_float4> scratch_;   // reused by sampleHeights
    ID3D12Resource *displacements_ = nullptr;
    ID3D12Resource *gradients_ = nullptr;
    bool inited_ = false;
#else
    // Not built with the SDK. Every entry point still exists so no call site
    // needs an #if around it.
    float windSpeed = 0.0f, windDirX = 0.0f, windDirZ = 0.0f;
    float windFetchKm = 0.0f, amplitude = 0.0f, timeScale = 0.0f;
    bool init(Falcor::ref<Falcor::Device>) {
        status_ = "not built with WaveWorks";
        return false;
    }
    void tick(double) {}
    bool sampleHeights(const float *, float *, uint32_t) const { return false; }
    void refreshParams() {}
    float maxDisplacement() const { return 0.0f; }
    void shutdown() {}

  private:
#endif
    WaveWorksFrame frame_;
    std::string status_ = "not initialised";
    bool ready_ = false;
};

}  // namespace v4
