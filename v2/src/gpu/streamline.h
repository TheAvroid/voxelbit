// ---------------------------------------------------------------------------
// streamline.h -- NVIDIA Streamline: the capability gate for PHASES C and D,
//                 and the only road to Frame Generation.
//
// WHY THIS EXISTS BESIDE dlss.h RATHER THAN REPLACING IT.
//
// v4 talked to NGX directly and that was right at the time: NGX reaches Ray
// Reconstruction and Super Resolution, and Streamline was not on the machine.
// It does NOT reach FRAME GENERATION, and no amount of NGX gets you there.
//
// The reason is structural rather than a missing entry point. Generating a
// frame means having somewhere to PUT it: something has to sit between the
// engine and Present, hold the real frame back, insert the generated one, and
// pace the pair so the extra frame buys smoothness instead of just latency. It
// also has to keep Reflex in step, because a generated frame that is not paced
// against the CPU's submission is worse than no generated frame at all --
// DLSS-G refuses to run without Reflex and says so through
// eFailReflexNotDetectedAtRuntime. That machinery IS the Streamline interposer.
//
// ---------------------------------------------------------------------------
// HOW STREAMLINE GETS UNDERNEATH FALCOR, AND WHY EVERY CALL HERE IS A FUNCTION
// POINTER.
//
// The documented integration is to link sl.interposer.lib in place of d3d12.lib
// so device and factory creation resolve to Streamline's proxies. v2 cannot do
// that: it does not create the device. Falcor does, through a PREBUILT
// slang-gfx.dll -- which does not even IMPORT D3D12CreateDevice. It loads it at
// runtime from a base name held in its .rdata.
//
// So patch_gfx_interposer.py rewrites that base name: gfx.dll now asks for
// "slp12" instead of "d3d12" (and "slgi" instead of "dxgi"), and copies of
// sl.interposer.dll are shipped under those names. gfx.dll therefore creates the
// device through Streamline, while Streamline's own lookup of the real
// "d3d12.dll" still finds the system one -- which is the collision that made the
// obvious version of this trick crash.
//
// THAT IS WHY NOTHING HERE IS LINKED. If v2 imported sl.interposer.lib it would
// pull in sl.interposer.dll as a SECOND module instance, with its own state,
// and slInit on that instance would say nothing to the instance gfx.dll is
// actually creating the device through. Windows keys loaded modules by path, so
// the only way to reach the same one is to open it by the same name. preInit()
// therefore loads "slp12.dll" itself -- before Falcor exists, so the load order
// is ours -- and resolves every entry point out of that handle. gfx.dll's later
// request for the same name returns the same already-initialised module.
//
// ---------------------------------------------------------------------------
// EVERY FEATURE IS ASKED FOR SEPARATELY, AT RUNTIME.
//
// Streamline itself runs anywhere; the hardware floors are per feature:
//
//     Super Resolution     RTX 20-series and up
//     Ray Reconstruction   RTX 20-series and up
//     Reflex               GTX 900-series and up
//     Frame Generation     RTX 40-series and up (Ada's optical flow unit)
//
// so slIsFeatureSupported is asked once per feature against this adapter, and
// anything it refuses is simply not offered. A card that cannot generate frames
// still upscales; a card that can do neither still renders. And if the patch
// above was never applied, slp12.dll is absent, everything here reports
// unavailable, and v2 falls back to the direct-NGX path in dlss.h.
// ---------------------------------------------------------------------------
#pragma once

#include "Core/API/Device.h"
#include "Core/API/NativeHandleTraits.h"
#include "Core/API/RenderContext.h"
#include "Core/API/Texture.h"
#include "Utils/Math/Vector.h"

#include <cmath>
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#if V2_HAS_STREAMLINE
#include <d3d12.h>
#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#endif

namespace v2 {

// What the settings menu offers for frame generation. Mirrors sl::DLSSGMode,
// kept as its own enum so app.h and the bake in defaults.h do not have to
// include Streamline's headers to name a mode.
enum class FrameGen { Off, On2x, On3x, On4x };

inline const char *frameGenName(FrameGen f) {
    switch (f) {
        case FrameGen::On2x: return "2x";
        case FrameGen::On3x: return "3x";
        case FrameGen::On4x: return "4x";
        default: return "off";
    }
}
inline int frameGenExtraFrames(FrameGen f) {
    switch (f) {
        case FrameGen::On2x: return 1;
        case FrameGen::On3x: return 2;
        case FrameGen::On4x: return 3;
        default: return 0;
    }
}

#if !V2_HAS_STREAMLINE
// ---------------------------------------------------------------------------
// Built without the SDK: the same class, permanently unavailable. The engine
// then uses dlss.h's direct-NGX Ray Reconstruction and offers no frame
// generation, which is exactly what v4 did.
// ---------------------------------------------------------------------------
class Streamline {
  public:
    static bool preInit(const std::filesystem::path &) { return false; }
    static const std::string &preInitStatus() { return s_status; }
    bool init(const Falcor::ref<Falcor::Device> &) { return false; }
    bool available() const { return false; }
    bool hasSuperResolution() const { return false; }
    bool hasRayReconstruction() const { return false; }
    bool hasFrameGeneration() const { return false; }
    static bool swapchainIsProxied() { return false; }
    bool hasReflex() const { return false; }
    const std::string &status() const { return s_status; }
    const std::string &featureReport() const { return s_status; }
    bool setFrameGeneration(FrameGen, Falcor::uint2, Falcor::uint2) { return false; }
    FrameGen frameGeneration() const { return FrameGen::Off; }
    void newFrame() {}
    void markSimulationStart() {}
    void markSimulationEnd() {}
    void markRenderSubmitStart() {}
    void markRenderSubmitEnd() {}
    void markPresentStart() {}
    void markPresentEnd() {}
    void pollFrameGenState() {}
    std::string frameGenStatus() const { return "unavailable"; }
    static std::string moduleReport() { return "  (no Streamline)\n"; }
    bool setFrameConstants(const float *, const float *, const float *, const float *,
                           float, float, float, float, bool) { return false; }
    bool tagResources(Falcor::RenderContext *, Falcor::Texture *, Falcor::Texture *,
                      Falcor::Texture *, Falcor::uint2, Falcor::uint2) { return false; }
    int framesPresented() const { return 1; }
    int maxGeneratedFrames() const { return 0; }
    void shutdown() {}

  private:
    static inline std::string s_status = "built without the Streamline SDK";
};
#else

class Streamline {
  public:
    ~Streamline() { shutdown(); }

    // -----------------------------------------------------------------------
    // slInit, and it MUST run before anything creates a D3D12 device.
    //
    // Static and called from main() for that reason: by the time ForestApp's
    // constructor runs, Falcor has already built its device, and a Streamline
    // brought up after that would have proxied nothing.
    //
    // The module is opened BY THE NAME gfx.dll will ask for, so the two share
    // one instance -- see the note at the top of this file.
    // -----------------------------------------------------------------------
    static bool preInit(const std::filesystem::path &exeDir) {
        if (s_tried) return s_ok;
        s_tried = true;

        // Already loaded is fine and expected on a second call; LoadLibrary
        // refcounts by path, so this is the same module either way.
        s_module = LoadLibraryA(kInterposerName);
        if (!s_module) {
            s_status = std::string("no ") + kInterposerName +
                       " beside the exe -- run patch_gfx_interposer.py";
            return false;
        }

        // One resolve per entry point, all from the same handle.
        auto fn = [](const char *name) { return GetProcAddress(s_module, name); };
        s_slInit = reinterpret_cast<PFN_slInit>(fn("slInit"));
        s_slShutdown = reinterpret_cast<PFN_slShutdown>(fn("slShutdown"));
        s_slSetD3DDevice = reinterpret_cast<PFN_slSetD3DDevice>(fn("slSetD3DDevice"));
        s_slIsFeatureSupported =
            reinterpret_cast<PFN_slIsFeatureSupported>(fn("slIsFeatureSupported"));
        s_slGetNewFrameToken =
            reinterpret_cast<PFN_slGetNewFrameToken>(fn("slGetNewFrameToken"));
        s_slSetConstants = reinterpret_cast<PFN_slSetConstants>(fn("slSetConstants"));
        s_slSetTagForFrame =
            reinterpret_cast<PFN_slSetTagForFrame>(fn("slSetTagForFrame"));
        s_slEvaluateFeature =
            reinterpret_cast<PFN_slEvaluateFeature>(fn("slEvaluateFeature"));
        s_slGetFeatureFunction =
            reinterpret_cast<PFN_slGetFeatureFunction>(fn("slGetFeatureFunction"));

        if (!s_slInit || !s_slSetD3DDevice || !s_slIsFeatureSupported ||
            !s_slGetFeatureFunction) {
            s_status = std::string(kInterposerName) +
                       " is not a Streamline interposer (missing entry points)";
            return false;
        }

        // Only the features v2 actually drives. Loading a plugin that is never
        // evaluated still costs a DLL load and a driver query at startup.
        static const sl::Feature kFeatures[] = {
            sl::kFeatureDLSS,      // Super Resolution     (Phase D)
            sl::kFeatureDLSS_RR,   // Ray Reconstruction   (Phase C)
            sl::kFeatureDLSS_G,    // Frame Generation     (after Phase D)
            sl::kFeatureReflex,    // required by DLSS_G
            sl::kFeaturePCL,       // latency markers Reflex paces against
        };
        static std::wstring pluginDir = exeDir.wstring();
        static const wchar_t *paths[] = {pluginDir.c_str()};

        sl::Preferences pref{};
        pref.showConsole = false;
        // VERBOSE WHEN ASKED. Streamline's own log is the only place that says
        // why DLSS-G declined to generate a frame -- every API this integration
        // can call reports success while it quietly presents nothing. Off by
        // default because it is chatty; V2_SL_LOG=1 turns it on and points it at
        // a directory beside the build.
        const char *slLog = std::getenv("V2_SL_LOG");
        const bool verbose = slLog && slLog[0] == '1';
        pref.logLevel = verbose ? sl::LogLevel::eVerbose : sl::LogLevel::eOff;
        static std::wstring logDir = (exeDir / "sl-log").wstring();
        pref.pathToLogsAndData = verbose ? logDir.c_str() : nullptr;
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        pref.featuresToLoad = kFeatures;
        pref.numFeaturesToLoad = uint32_t(std::size(kFeatures));
        pref.engine = sl::EngineType::eCustom;
        pref.engineVersion = "v2";
        // An arbitrary application id, exactly as the NGX path in dlss.h uses.
        // An unregistered integration is expected to supply one.
        pref.applicationId = 0x76366269u;  // 'v6bi'
        pref.renderAPI = sl::RenderAPI::eD3D12;
        // NOT eUseManualHooking: the interposer is already underneath slang-gfx
        // by name, so device, factory and swapchain are proxied at creation and
        // there is nothing left to hook by hand.
        // THE TAGGING FLAG IS NOT OPTIONAL WITH slSetTagForFrame. The old
        // slSetTag is deprecated in favour of the frame-token form, but the new
        // one is only accepted when Streamline has been told to expect
        // frame-based tagging -- without it every call returns
        // eErrorInvalidIntegration (19). Nothing else fails: DLSS-G reports eOk
        // and presents 0 generated frames, because it was never handed a
        // resource to interpolate.
        //
        // eUseDXGIFactoryProxy makes the factory proxy explicit rather than
        // incidental. Frame generation needs the swapchain to be a Streamline
        // proxy, and the swapchain can only be one if the factory that made it
        // was -- which, given slang-gfx creates both behind our back, is worth
        // asking for rather than assuming.
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking |
                     sl::PreferenceFlags::eAllowOTA |
                     sl::PreferenceFlags::eLoadDownloadedPlugins |
                     sl::PreferenceFlags::eUseDXGIFactoryProxy |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;

        const sl::Result r = s_slInit(pref, sl::kSDKVersion);
        s_ok = (r == sl::Result::eOk);
        s_status = s_ok ? "ready" : ("slInit failed (" + std::to_string(int(r)) + ")");
        return s_ok;
    }

    static const std::string &preInitStatus() { return s_status; }

    // -----------------------------------------------------------------------
    // Once the device exists: hand it over, then ask about each feature.
    // -----------------------------------------------------------------------
    bool init(const Falcor::ref<Falcor::Device> &device) {
        device_ = device;
        if (!s_ok) {
            status_ = s_status;
            return false;
        }

        ID3D12Device *d3d = device_->getNativeHandle().as<ID3D12Device *>();
        if (!d3d) {
            status_ = "not a D3D12 device";
            return false;
        }

        // eErrorInvalidIntegration HERE MEANS SUCCESS, and reading it as a
        // failure disables the feature it proves is working.
        //
        // slSetD3DDevice does not merely record the device: it returns
        // PluginManager::initializePlugins() (sl.cpp:493). That returns
        // eErrorInvalidIntegration (19) when the plugins are ALREADY
        // initialised -- which is precisely what we want, because slgi_forwarder's
        // D3D12CreateDevice hands the device over at creation time, hundreds of
        // milliseconds before this runs. DLSS-G can only wrap the swap chain if
        // that happened first, so seeing 19 here is the confirmation that it did.
        //
        // The one other path returning 19 is "slInit was never called", and that
        // cannot reach this line: init() returns above if !s_ok.
        const sl::Result r = s_slSetD3DDevice(d3d);
        const bool alreadyInitialisedByShim =
            (r == sl::Result::eErrorInvalidIntegration);
        if (r != sl::Result::eOk && !alreadyInitialisedByShim) {
            // Anything else most likely means this device did NOT come through
            // the interposer -- i.e. gfx.dll was not patched, or was rebuilt
            // over. Worth saying, because the symptom is otherwise silent.
            status_ = "slSetD3DDevice failed (" + std::to_string(int(r)) +
                      ") -- was gfx.dll patched?";
            return false;
        }
        earlyDeviceHandover_ = alreadyInitialisedByShim;

        // Every feature question is asked against THIS adapter, identified by
        // its LUID. Asking in the abstract would answer for the machine rather
        // than for the GPU the engine is running on, which is the wrong answer
        // on any laptop with two of them.
        const LUID luid = d3d->GetAdapterLuid();
        sl::AdapterInfo adapter{};
        adapter.deviceLUID = reinterpret_cast<uint8_t *>(const_cast<LUID *>(&luid));
        adapter.deviceLUIDSizeInBytes = sizeof(LUID);

        auto ask = [&](sl::Feature f, const char *name, bool *out) {
            const sl::Result rr = s_slIsFeatureSupported(f, adapter);
            *out = (rr == sl::Result::eOk);
            // The refusal reason is worth keeping: "this GPU is too old" and
            // "the driver is too old" are very different problems and only one
            // of them is fixable by the person reading it.
            report_ += std::string("    ") + name + (*out ? ": yes\n"
                       : (": no (" + std::to_string(int(rr)) + ")\n"));
        };

        ask(sl::kFeatureDLSS, "super resolution", &hasSR_);
        ask(sl::kFeatureDLSS_RR, "ray reconstruction", &hasRR_);
        ask(sl::kFeatureReflex, "reflex", &hasReflex_);
        ask(sl::kFeaturePCL, "pcl markers", &hasPcl_);
        ask(sl::kFeatureDLSS_G, "frame generation", &hasFG_);

        // FRAME GENERATION WITHOUT REFLEX IS NOT AN OPTION, it is a failure
        // mode. DLSS-G checks for Reflex at runtime and refuses; offering it in
        // the menu anyway would produce a switch that silently does nothing.
        if (hasFG_ && !hasReflex_) {
            hasFG_ = false;
            report_ += "    (frame generation withdrawn: it requires Reflex)\n";
        }

        if (hasFG_) {
            s_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions",
                                   reinterpret_cast<void *&>(slDLSSGSetOptions_));
            s_slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState",
                                   reinterpret_cast<void *&>(slDLSSGGetState_));
            if (!slDLSSGSetOptions_) {
                hasFG_ = false;
                report_ += "    (frame generation withdrawn: no slDLSSGSetOptions)\n";
            }
        }
        if (hasReflex_) {
            s_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions",
                                   reinterpret_cast<void *&>(slReflexSetOptions_));
            // THE SLEEP CALL IS WHAT MAKES REFLEX ACTUALLY RUN. Setting the
            // options only configures it; slReflexSleep is the per-frame call
            // that paces the CPU, and without it Reflex is "on" in the sense
            // that nothing objects and off in the sense that nothing happens.
            // DLSS-G checks for Reflex AT RUNTIME and refuses when it does not
            // find it -- reporting eFailReflexNotDetectedAtRuntime, or, more
            // confusingly, reporting eOk while generating nothing.
            s_slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep",
                                   reinterpret_cast<void *&>(slReflexSleep_));
            enableReflex();
        }
        if (hasPcl_) {
            s_slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker",
                                   reinterpret_cast<void *&>(slPCLSetMarker_));
        }

        ready_ = true;
        status_ = "ready";
        return true;
    }

    bool available() const { return ready_; }
    bool hasSuperResolution() const { return ready_ && hasSR_; }
    bool hasRayReconstruction() const { return ready_ && hasRR_; }
    // FRAME GENERATION NEEDS MORE THAN THE GPU SAYING YES.
    //
    // slIsFeatureSupported answers about the HARDWARE, and on any RTX 40-series
    // card the answer is yes whether or not this process can actually use it.
    // What it cannot see is whether the SWAPCHAIN is a Streamline proxy -- and
    // without that, DLSS-G has nowhere to insert a generated frame. It does not
    // complain; it reports eOk and presents nothing.
    //
    // slgi.dll being loaded is that missing fact. slang-gfx only loads it if the
    // interposer patch is in place, and the patch is what routes factory and
    // swapchain creation through Streamline. So the two are asked together, and
    // a card that could generate frames in a differently-built engine is
    // honestly reported as unable to here rather than offered a switch that
    // silently does nothing.
    static bool swapchainIsProxied() { return GetModuleHandleA("slgi.dll") != nullptr; }

    // True when the device reached Streamline at creation time (via the shim)
    // rather than here. DLSS-G needs the former; frame generation is silently
    // inert without it, so it is worth being able to state plainly.
    bool earlyDeviceHandover() const { return earlyDeviceHandover_; }


    bool hasFrameGeneration() const { return ready_ && hasFG_ && swapchainIsProxied(); }

    // Whether the GPU could do it in principle, ignoring this build's plumbing.
    // Used only to explain WHY the feature is not on offer.
    bool gpuSupportsFrameGeneration() const { return ready_ && hasFG_; }
    bool hasReflex() const { return ready_ && hasReflex_; }
    const std::string &status() const { return status_; }
    const std::string &featureReport() const { return report_; }
    FrameGen frameGeneration() const { return fg_; }

    // -----------------------------------------------------------------------
    // Turn frame generation on, off, or up.
    // -----------------------------------------------------------------------
    // TWO SIZES, AND THEY ARE GENUINELY DIFFERENT IN THIS ENGINE.
    //
    // v2 traces at render resolution and presents at output resolution -- 853 x
    // 480 into 1280 x 720 under DLSS quality, say. The depth and motion vector
    // buffers are by-products of the primary hit, so they are at RENDER size;
    // the colour DLSS-G interpolates is the presented frame, at OUTPUT size.
    //
    // Declaring both as the output size (which is the easy mistake, because one
    // of them is right) tells DLSS-G to read the motion vectors over an extent
    // half again bigger than the texture actually is. It does not fail -- it
    // reads garbage off the edge and quietly declines to generate.
    bool setFrameGeneration(FrameGen f, Falcor::uint2 renderSize, Falcor::uint2 outputSize) {
        if (!ready_ || !hasFG_ || !slDLSSGSetOptions_) return false;

        sl::DLSSGOptions o{};
        o.mode = (f == FrameGen::Off) ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
        // AT LEAST 1, EVEN WHEN TURNING IT OFF. Streamline validates this
        // field independently of the mode and rejects zero outright --
        // "Input data numFramesToGenerate (0) must be greater than 0" -- so an
        // Off call built the obvious way fails, and the failure is reported
        // against the call that was only trying to shut the feature down.
        o.numFramesToGenerate = uint32_t(std::max(1, frameGenExtraFrames(f)));
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        o.onErrorCallback = &onApiError;
        // The swapchain Falcor built. DLSS-G needs to know how many back
        // buffers it has to work with; left at zero it has to guess, and a
        // wrong guess is one of the ways it declines to generate silently.
        o.numBackBuffers = 3;
        o.mvecDepthWidth = renderSize.x;
        o.mvecDepthHeight = renderSize.y;
        o.colorWidth = outputSize.x;
        o.colorHeight = outputSize.y;

        const sl::Result r = slDLSSGSetOptions_(viewport_, o);
        if (r != sl::Result::eOk) {
            status_ = "DLSSGSetOptions failed (" + std::to_string(int(r)) + ")";
            return false;
        }
        fg_ = f;
        return true;
    }

    // -----------------------------------------------------------------------
    // The per-frame handshake.
    //
    // THE MARKERS ARE NOT TELEMETRY. Reflex uses them to decide when to release
    // the CPU so the frame it submits lands where the pacer wants it, and frame
    // generation is paced off the same clock. Without them DLSS-G still runs
    // and the frame rate still doubles, but the extra frames are spaced wrongly
    // and the result reads as judder at a higher number -- the classic "frame
    // generation made it worse" report.
    // -----------------------------------------------------------------------
    void newFrame() {
        if (!ready_ || !s_slGetNewFrameToken) return;
        frame_ = nullptr;
        ++index_;
        s_slGetNewFrameToken(frame_, &index_);

        // FIRST THING IN THE FRAME, before any simulation. This is where Reflex
        // holds the CPU back so the frame it is about to let through lands
        // where the pacer wants it -- which is also the clock frame generation
        // spaces its inserted frames against. Called before the markers, since
        // the markers describe work that has not happened yet.
        if (slReflexSleep_ && frame_) slReflexSleep_(*frame_);
    }

    void markSimulationStart() { mark(sl::PCLMarker::eSimulationStart); }
    void markSimulationEnd() { mark(sl::PCLMarker::eSimulationEnd); }
    // The GPU submission window. Reflex uses the gap between these and the
    // present markers to work out how long the frame actually took.
    void markRenderSubmitStart() { mark(sl::PCLMarker::eRenderSubmitStart); }
    void markRenderSubmitEnd() { mark(sl::PCLMarker::eRenderSubmitEnd); }
    void markPresentStart() { mark(sl::PCLMarker::ePresentStart); }
    void markPresentEnd() { mark(sl::PCLMarker::ePresentEnd); }


    // -----------------------------------------------------------------------
    // The camera, as Streamline wants it.
    //
    // WHAT DLSS-G ACTUALLY NEEDS FROM THIS. It is interpolating between two
    // rendered frames, so it has to know how the camera moved between them --
    // that is clipToPrevClip, and it is the one matrix here that would ruin the
    // result if it were wrong. The rest are context: the projection so it can
    // linearise depth, the basis and near/far so it can reason about world
    // scale, the jitter so it can undo the sub-pixel offset.
    //
    // v2 has no matrices anywhere. Its camera is a basis and two half-extents,
    // built for constructing a ray rather than transforming a vertex, so the
    // matrices are assembled here and nowhere else. They are written to agree
    // EXACTLY with ndcDepth() in Trace.cs.slang -- the same A and B below --
    // because the depth buffer being tagged was written by that function, and a
    // projection that disagreed with it would place every pixel at the wrong
    // distance.
    //
    // Row-major, row-vector convention throughout: clip = world * V * P. That
    // is Streamline's convention (sl_consts.h says so explicitly) and it is the
    // opposite of the column-vector form most maths texts use, so every product
    // below reads left-to-right in the order the transforms apply.
    // -----------------------------------------------------------------------
    bool setFrameConstants(const float *pos, const float *right, const float *up,
                           const float *fwd, float halfW, float halfH,
                           float jitterX, float jitterY, bool reset) {
        if (!ready_ || !s_slSetConstants || !frame_) return false;

        const float n = kNear, f = kFar;
        const float A = f / (f - n);
        const float B = -n * f / (f - n);

        // world -> view. Rigid, so its inverse is the transpose of the basis
        // with the translation put back -- no general inversion needed.
        sl::float4x4 V{};
        V[0] = {right[0], up[0], fwd[0], 0.0f};
        V[1] = {right[1], up[1], fwd[1], 0.0f};
        V[2] = {right[2], up[2], fwd[2], 0.0f};
        V[3] = {-dot3(pos, right), -dot3(pos, up), -dot3(pos, fwd), 1.0f};

        sl::float4x4 invV{};
        invV[0] = {right[0], right[1], right[2], 0.0f};
        invV[1] = {up[0], up[1], up[2], 0.0f};
        invV[2] = {fwd[0], fwd[1], fwd[2], 0.0f};
        invV[3] = {pos[0], pos[1], pos[2], 1.0f};

        // view -> clip. halfW and halfH are tan(fov/2) in each axis, which is
        // exactly what the ray construction in Trace.cs.slang multiplies by.
        sl::float4x4 P{};
        P[0] = {1.0f / halfW, 0.0f, 0.0f, 0.0f};
        P[1] = {0.0f, 1.0f / halfH, 0.0f, 0.0f};
        P[2] = {0.0f, 0.0f, A, 1.0f};
        P[3] = {0.0f, 0.0f, B, 0.0f};

        // clip -> view, by hand rather than by a general inverse: this
        // projection has four non-zero terms and inverting it in closed form is
        // both exact and obvious to check against P above.
        sl::float4x4 invP{};
        invP[0] = {halfW, 0.0f, 0.0f, 0.0f};
        invP[1] = {0.0f, halfH, 0.0f, 0.0f};
        invP[2] = {0.0f, 0.0f, 0.0f, 1.0f / B};
        invP[3] = {0.0f, 0.0f, 1.0f, -A / B};

        sl::Constants c{};
        c.cameraViewToClip = P;
        c.clipToCameraView = invP;
        c.clipToLensClip = identity();  // no lens distortion in this engine

        if (havePrev_) {
            // clip -> world -> previous clip. The camera is the only thing that
            // moves in this world, so this single matrix describes ALL motion
            // between the two frames.
            const sl::float4x4 toWorld = mul(invP, invV);
            const sl::float4x4 toPrevClip = mul(prevV_, prevP_);
            c.clipToPrevClip = mul(toWorld, toPrevClip);
            c.prevClipToClip = mul(mul(invPrevP_, invPrevV_), mul(V, P));
        } else {
            c.clipToPrevClip = identity();
            c.prevClipToClip = identity();
        }

        c.jitterOffset = {jitterX, jitterY};
        // The motion vectors v2 writes are already in RENDER PIXELS, so there
        // is nothing to scale. Streamline wants them normalised, hence the
        // reciprocal of the render extent rather than 1.
        c.mvecScale = {mvecScale_[0], mvecScale_[1]};
        c.cameraPinholeOffset = {0.0f, 0.0f};
        c.cameraPos = {pos[0], pos[1], pos[2]};
        c.cameraUp = {up[0], up[1], up[2]};
        c.cameraRight = {right[0], right[1], right[2]};
        c.cameraFwd = {fwd[0], fwd[1], fwd[2]};
        c.cameraNear = n;
        c.cameraFar = f;
        c.cameraFOV = 2.0f * atanf(halfH);
        c.cameraAspectRatio = halfW / halfH;
        c.depthInverted = sl::Boolean::eFalse;      // ndcDepth is 0 near, 1 far
        // The vectors already describe the camera's own movement -- they are
        // this frame's hit point projected through last frame's camera -- so
        // Streamline must not add it again.
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.motionVectorsJittered = sl::Boolean::eFalse;
        c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;

        const sl::Result r = s_slSetConstants(c, *frame_, viewport_);
        lastConstants_ = int(r);

        prevV_ = V; prevP_ = P; invPrevV_ = invV; invPrevP_ = invP;
        havePrev_ = true;
        return r == sl::Result::eOk;
    }

    // -----------------------------------------------------------------------
    // Hand Streamline the three buffers it interpolates from.
    //
    // HUD-LESS COLOUR IS THE IMPORTANT ONE. What is tagged here is the tone
    // mapped frame BEFORE the crosshair and the settings menu are drawn over
    // it. Tag the finished window instead and the generated frames interpolate
    // the UI too -- which on a moving camera smears the crosshair across the
    // screen, the single most recognisable way a frame generator looks broken.
    // -----------------------------------------------------------------------
    bool tagResources(Falcor::RenderContext *ctx, Falcor::Texture *color,
                      Falcor::Texture *depth, Falcor::Texture *mvec,
                      Falcor::uint2 renderDim, Falcor::uint2 outDim) {
        if (!ready_ || !s_slSetTagForFrame || !frame_ || !color || !depth || !mvec)
            return false;

        mvecScale_[0] = 1.0f / float(renderDim.x);
        mvecScale_[1] = 1.0f / float(renderDim.y);

        // Streamline reads these on the GPU timeline, so they have to be in a
        // shader-readable state and Falcor has to know it -- the same contract
        // the NGX call in dlss.h observes, for the same reason.
        ctx->resourceBarrier(color, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(depth, Falcor::Resource::State::NonPixelShader);
        ctx->resourceBarrier(mvec, Falcor::Resource::State::NonPixelShader);
        ctx->submit(false);

        auto res = [](Falcor::Texture *t, Falcor::uint2 dim) {
            sl::Resource r{};
            r.type = sl::ResourceType::eTex2d;
            r.native = t->getNativeHandle().as<ID3D12Resource *>();
            r.state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            r.width = dim.x;
            r.height = dim.y;
            return r;
        };

        sl::Resource rc = res(color, outDim);
        sl::Resource rd = res(depth, renderDim);
        sl::Resource rm = res(mvec, renderDim);

        const sl::Extent outExt{0, 0, outDim.x, outDim.y};
        const sl::Extent renExt{0, 0, renderDim.x, renderDim.y};

        // DEPTH AND MOTION VECTORS ONLY -- NO HUD-LESS COLOUR.
        //
        // DLSS-G registers four required tags: Depth, MotionVectors,
        // HUDLessColor and UIColorAndAlpha. The last two are a PAIR. Handing it
        // a frame with the UI removed only makes sense if it is also given the
        // UI, because it has to composite that back over every frame it
        // generates. Supplying one without the other leaves it unable to build
        // a complete frame, so it declines -- silently, reporting eOk and
        // presenting nothing, which is exactly what happened.
        //
        // v2 has no separate UI texture: the crosshair and the settings menu are
        // drawn straight into the back buffer. Rather than invent one, the
        // HUD-less tag is dropped and DLSS-G interpolates the back buffer as it
        // finds it. The cost is that the crosshair is interpolated along with
        // the scene -- on a centred cross a few pixels wide that is not visible,
        // and it is a far better trade than the feature not running.
        sl::ResourceTag tags[] = {
            sl::ResourceTag{&rd, sl::kBufferTypeDepth,
                            sl::ResourceLifecycle::eValidUntilPresent, &renExt},
            sl::ResourceTag{&rm, sl::kBufferTypeMotionVectors,
                            sl::ResourceLifecycle::eValidUntilPresent, &renExt},
        };
        (void)rc;
        (void)outExt;

        ID3D12GraphicsCommandList *cmd =
            ctx->getLowLevelData()->getCommandBufferNativeHandle().as<ID3D12GraphicsCommandList *>();
        const sl::Result r =
            s_slSetTagForFrame(*frame_, viewport_, tags, uint32_t(std::size(tags)), cmd);
        lastTag_ = int(r);
        lastTagDims_[0] = renderDim.x;
        lastTagDims_[1] = renderDim.y;
        lastTagDims_[2] = outDim.x;
        lastTagDims_[3] = outDim.y;
        return r == sl::Result::eOk;
    }

    // How many frames the swapchain actually presented for the last one v2
    // drew. 1 with generation off; 2, 3 or 4 with it on. Read from the SDK
    // rather than assumed, because DLSS-G drops back to presenting real frames
    // on its own whenever it cannot generate a good one.
    // -----------------------------------------------------------------------
    // THE STATE IS POLLED AT MOST ONCE A SECOND, NOT ONCE A FRAME.
    //
    // slDLSSGGetState has to be synchronised with the present thread, and
    // Streamline says so out loud: "slDLSSGGetState must be synchronized with
    // the present thread". Calling it every frame from the render thread -- as
    // an fps counter naturally would -- produces a warning per frame and races
    // the very mechanism being measured.
    //
    // A frame counter does not need per-frame resolution anyway. This caches
    // the last answer and refreshes it on a timer, so the HUD stays live and
    // the present thread is left alone.
    // -----------------------------------------------------------------------
    // Call once at the top of the frame, right after the previous present.
    void pollFrameGenState() {
        if (ready_ && hasFG_ && fg_ != FrameGen::Off) refreshState();
    }

    int framesPresented() const {
        if (!ready_ || !hasFG_ || fg_ == FrameGen::Off) return 1;
        return cachedPresented_;   // read only; polled at the top of the frame
    }

    // -----------------------------------------------------------------------
    // WHICH STREAMLINE MODULES THE PROCESS ACTUALLY LOADED.
    //
    // THIS IS THE ONE FACT THE REST OF THE INTEGRATION ONLY INFERS. Frame
    // generation needs the swapchain to be a Streamline proxy, and it can only
    // be one if slang-gfx created it through SL's DXGI factory -- which happens
    // if and only if gfx.dll actually loaded the patched name. Everything else
    // reports success whether or not it did: slInit succeeds, the device is
    // proxied through slp12, features report supported, tags are accepted, and
    // DLSS-G reports eOk while generating nothing, because from its point of
    // view nothing is wrong -- it simply has no swapchain to insert into.
    //
    // GetModuleHandle answers it outright. slp12 loaded but slgi absent means
    // the D3D12 half of the patch took and the DXGI half did not: the device is
    // proxied, the swapchain is not, and frame generation cannot work no matter
    // what else is fixed.
    // -----------------------------------------------------------------------
    static std::string moduleReport() {
        struct Mod { const char *name, *why; };
        static const Mod mods[] = {
            {"sl.interposer.dll", "Streamline itself"},
            {"slp12.dll", "device creation (patched d3d12)"},
            {"slgi.dll", "factory + swapchain (patched dxgi) -- FRAME GENERATION NEEDS THIS"},
            {"sl.common.dll", "plugin host"},
            {"sl.dlss_g.dll", "frame generation"},
            {"sl.reflex.dll", "reflex"},
            {"sl.pcl.dll", "latency markers"},
        };
        std::string out;
        for (const Mod &m : mods) {
            const bool loaded = GetModuleHandleA(m.name) != nullptr;
            out += std::string("    ") + (loaded ? "loaded  " : "ABSENT  ") + m.name + "  (" +
                   m.why + ")\n";
        }
        return out;
    }

    // -----------------------------------------------------------------------
    // HOW MANY EXTRA FRAMES THIS GPU CAN GENERATE, which is not a constant.
    //
    // 2x (one generated frame) is what Ada does. 3x and 4x are DLSS 4
    // multi-frame generation and need Blackwell -- an RTX 50-series card. Ask
    // rather than assume: selecting 4x on a 40-series card is not an error,
    // DLSS-G simply clamps to what it can do and carries on, so the menu would
    // otherwise offer three settings that all behave identically and give no
    // hint why.
    // -----------------------------------------------------------------------
    int maxGeneratedFrames() const {
        if (!ready_ || !hasFG_ || !slDLSSGGetState_) return 0;
        refreshState();
        return cachedMaxGen_;
    }

    // -----------------------------------------------------------------------
    // WHY FRAME GENERATION IS NOT GENERATING, in words.
    //
    // DLSS-G does not refuse loudly. slDLSSGSetOptions accepts the mode, the
    // feature reports itself available, and then it simply presents only real
    // frames -- so "on" and "on and working" look identical from the outside
    // and the frame rate is the only clue. The status flags are the difference,
    // and each one names a specific thing the integration has not done.
    // -----------------------------------------------------------------------
    std::string frameGenStatus() const {
        if (!ready_) return status_;
        if (!hasFG_) return "not supported on this GPU";
        if (!slDLSSGGetState_) return "no slDLSSGGetState";

        refreshState();
        if (!cachedValid_) return "state not read yet";
        const sl::DLSSGState &st = cachedState_;

        // THE HANDSHAKE IS REPORTED SEPARATELY FROM DLSS-G'S OWN VERDICT,
        // because they can disagree in the one direction that matters: the
        // feature reports eOk (nothing it checks is wrong) while slSetTagForFrame
        // or slSetConstants has been quietly failing every frame, so it has
        // simply never been given anything to interpolate. Without this the two
        // cases look identical from the outside.
        std::string handshake;
        if (s_apiErrors) {
            char b[96];
            std::snprintf(b, sizeof(b), "; %ld DXGI errors, last 0x%08lx", s_apiErrors,
                          (unsigned long)s_lastApiError);
            handshake += b;
        }
        if (lastTag_ != 0)
            handshake += "; setTag failed (" + std::to_string(lastTag_) + ")";
        if (lastConstants_ != 0)
            handshake += "; setConstants failed (" + std::to_string(lastConstants_) + ")";
        if (lastTag_ == 0 && lastConstants_ == 0 && lastTagDims_[0] == 0)
            handshake = "; nothing tagged yet";
        if (lastTagDims_[0])
            handshake += "; tagged mvec/depth " + std::to_string(lastTagDims_[0]) + "x" +
                         std::to_string(lastTagDims_[1]) + ", colour " +
                         std::to_string(lastTagDims_[2]) + "x" +
                         std::to_string(lastTagDims_[3]);

        const uint32_t f = uint32_t(st.status);
        if (f == 0) {
            std::string cap;
            if (frameGenExtraFrames(fg_) > int(st.numFramesToGenerateMax))
                cap = "; " + std::string(frameGenName(fg_)) +
                      " asked for but this GPU caps at " +
                      std::to_string(st.numFramesToGenerateMax + 1) +
                      "x (3x/4x need an RTX 50-series)";
            return "ok, presenting " + std::to_string(st.numFramesActuallyPresented) +
                   " of a possible " + std::to_string(st.numFramesToGenerateMax + 1) +
                   cap + handshake;
        }
        std::string out;
        auto add = [&](uint32_t bit, const std::string &why) {
            if (f & bit) { if (!out.empty()) out += "; "; out += why; }
        };
        add(1u << 0, "output resolution too low (min " +
                     std::to_string(st.minWidthOrHeight) + " px)");
        add(1u << 1, "Reflex not active at runtime");
        add(1u << 2, "swapchain format not supported (HDR)");
        add(1u << 3, "common constants invalid -- camera matrices or tags are wrong");
        add(1u << 4, "the swapchain is not a Streamline proxy "
                     "(GetCurrentBackBufferIndex never called)");
        if (out.empty()) out = "unknown status bits 0x" + std::to_string(f);
        return out + handshake;
    }

    void shutdown() {
        if (ready_ && hasFG_ && fg_ != FrameGen::Off)
            setFrameGeneration(FrameGen::Off, Falcor::uint2(0, 0), Falcor::uint2(0, 0));
        ready_ = false;
    }

  private:
    // The name patch_gfx_interposer.py made slang-gfx ask for. Opening the same
    // name is what guarantees one shared module instance.
    // OPENED BY ITS REAL NAME, not by the shim's. The shims are forwarders,
    // so GetProcAddress on one would resolve the same code -- but Streamline
    // needs to be loaded under sl.interposer.dll for its own path lookup to
    // work, and loading it here by that name is what guarantees it is.
    static constexpr const char *kInterposerName = "sl.interposer.dll";

    // DLSS-G'S OWN VIEW OF WHAT THE DXGI LAYER SAID.
    //
    // Everything else in this integration reports its own opinion: the feature
    // says it is supported, the tags say they were accepted, DLSSGState says
    // eOk -- and none of them can see a Present that quietly failed underneath.
    // This callback is the one channel that carries the real API result out, so
    // "presenting 0 of a possible 2" stops being a dead end.
    static inline volatile long s_apiErrors = 0;
    static inline volatile long s_lastApiError = 0;

    static void onApiError(const sl::APIError &e) {
        s_lastApiError = long(e.hres);
        ++s_apiErrors;
    }

    using PFN_slInit = sl::Result (*)(const sl::Preferences &, uint64_t);
    using PFN_slShutdown = sl::Result (*)();
    using PFN_slSetD3DDevice = sl::Result (*)(void *);
    using PFN_slIsFeatureSupported = sl::Result (*)(sl::Feature, const sl::AdapterInfo &);
    using PFN_slGetNewFrameToken = sl::Result (*)(sl::FrameToken *&, const uint32_t *);
    using PFN_slSetConstants = sl::Result (*)(const sl::Constants &, const sl::FrameToken &,
                                              const sl::ViewportHandle &);
    using PFN_slSetTagForFrame = sl::Result (*)(const sl::FrameToken &,
                                                const sl::ViewportHandle &,
                                                const sl::ResourceTag *, uint32_t,
                                                sl::CommandBuffer *);
    using PFN_slEvaluateFeature = sl::Result (*)(sl::Feature, const sl::FrameToken &,
                                                 const sl::BaseStructure **, uint32_t,
                                                 sl::CommandBuffer *);
    using PFN_slGetFeatureFunction = sl::Result (*)(sl::Feature, const char *, void *&);

    static inline HMODULE s_module = nullptr;
    static inline bool s_tried = false, s_ok = false;
    static inline std::string s_status = "not initialised";
    static inline PFN_slInit s_slInit = nullptr;
    static inline PFN_slShutdown s_slShutdown = nullptr;
    static inline PFN_slSetD3DDevice s_slSetD3DDevice = nullptr;
    static inline PFN_slIsFeatureSupported s_slIsFeatureSupported = nullptr;
    static inline PFN_slGetNewFrameToken s_slGetNewFrameToken = nullptr;
    static inline PFN_slSetConstants s_slSetConstants = nullptr;
    static inline PFN_slSetTagForFrame s_slSetTagForFrame = nullptr;
    static inline PFN_slEvaluateFeature s_slEvaluateFeature = nullptr;
    static inline PFN_slGetFeatureFunction s_slGetFeatureFunction = nullptr;

    sl::Result (*slDLSSGSetOptions_)(const sl::ViewportHandle &, const sl::DLSSGOptions &) = nullptr;
    sl::Result (*slDLSSGGetState_)(const sl::ViewportHandle &, sl::DLSSGState &,
                                   const sl::DLSSGOptions *) = nullptr;
    sl::Result (*slReflexSetOptions_)(const sl::ReflexOptions &) = nullptr;
    sl::Result (*slReflexSleep_)(const sl::FrameToken &) = nullptr;
    sl::Result (*slPCLSetMarker_)(sl::PCLMarker, const sl::FrameToken &) = nullptr;

    Falcor::ref<Falcor::Device> device_;
    sl::ViewportHandle viewport_{0u};
    sl::FrameToken *frame_ = nullptr;
    uint32_t index_ = 0;

    bool ready_ = false;
    bool hasSR_ = false, hasRR_ = false, hasFG_ = false, hasReflex_ = false, hasPcl_ = false;
    FrameGen fg_ = FrameGen::Off;
    std::string status_ = "not initialised";
    std::string report_;

    sl::float4x4 prevV_{}, prevP_{}, invPrevV_{}, invPrevP_{};
    bool havePrev_ = false;
    float mvecScale_[2] = {1.0f, 1.0f};
    // The last result from each per-frame call, for frameGenStatus().
    mutable int lastTag_ = 0, lastConstants_ = 0;
    bool earlyDeviceHandover_ = false;
    mutable uint32_t lastTagDims_[4] = {0, 0, 0, 0};
    mutable sl::DLSSGState cachedState_{};
    mutable std::chrono::steady_clock::time_point lastPoll_{};
    mutable int cachedPresented_ = 1, cachedMaxGen_ = 1;
    mutable bool cachedValid_ = false;

    // The depth range ndcDepth() in Trace.cs.slang was written against. These
    // two numbers exist in three places -- there, in tracer.h, and here -- and
    // all three have to agree or the projection handed to Streamline describes
    // a different camera than the depth buffer beside it.
    static constexpr float kNear = 0.05f;
    static constexpr float kFar = 8000.0f;

    static float dot3(const float *a, const float *b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }

    static sl::float4x4 identity() {
        sl::float4x4 m{};
        m[0] = {1, 0, 0, 0};
        m[1] = {0, 1, 0, 0};
        m[2] = {0, 0, 1, 0};
        m[3] = {0, 0, 0, 1};
        return m;
    }

    // Row-vector convention: (a * b) applies a first, then b.
    static sl::float4x4 mul(const sl::float4x4 &a, const sl::float4x4 &b) {
        sl::float4x4 o{};
        for (int r = 0; r < 4; ++r) {
            const float ar[4] = {a.row[r].x, a.row[r].y, a.row[r].z, a.row[r].w};
            float out[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const float bk[4] = {b.row[k].x, b.row[k].y, b.row[k].z, b.row[k].w};
                for (int c = 0; c < 4; ++c) out[c] += ar[k] * bk[c];
            }
            o.row[r] = {out[0], out[1], out[2], out[3]};
        }
        return o;
    }

    // Refresh the cached DLSS-G state, at most once a second.
    //
    // WHERE THIS IS CALLED FROM MATTERS MORE THAN HOW OFTEN. Streamline warns
    // that slDLSSGGetState must be synchronised with the present thread. In
    // this engine Falcor presents on the main thread, which is also the render
    // thread -- so the requirement is satisfiable, and the mistake was the
    // MOMENT rather than the thread: querying mid-frame, while a present is in
    // flight, reads a value that is being written. Called from the top of the
    // frame instead, immediately after the previous present completed, it is
    // stable.
    void refreshState() const {
        if (!slDLSSGGetState_) return;
        const auto now = std::chrono::steady_clock::now();
        if (cachedValid_ &&
            std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPoll_).count() < 1000)
            return;
        lastPoll_ = now;

        sl::DLSSGState st{};
        sl::DLSSGOptions o{};
        o.mode = (fg_ == FrameGen::Off) ? sl::DLSSGMode::eOff : sl::DLSSGMode::eOn;
        o.numFramesToGenerate = uint32_t(std::max(1, frameGenExtraFrames(fg_)));
        if (slDLSSGGetState_(viewport_, st, &o) != sl::Result::eOk) return;
        cachedState_ = st;
        cachedPresented_ = int(st.numFramesActuallyPresented);
        cachedMaxGen_ = int(st.numFramesToGenerateMax);
        cachedValid_ = true;
    }

    void mark(sl::PCLMarker m) {
        if (!ready_ || !slPCLSetMarker_ || !frame_) return;
        slPCLSetMarker_(m, *frame_);
    }

    // Reflex in its low-latency mode. Switched on rather than left to the
    // driver because frame generation depends on it -- and because it is the
    // one setting in this engine that makes the mouse feel different.
    void enableReflex() {
        if (!slReflexSetOptions_) return;
        sl::ReflexOptions o{};
        o.mode = sl::ReflexMode::eLowLatency;
        o.useMarkersToOptimize = true;
        o.virtualKey = 0;
        o.frameLimitUs = 0;
        slReflexSetOptions_(o);
    }
};
#endif  // V2_HAS_STREAMLINE

}  // namespace v2
