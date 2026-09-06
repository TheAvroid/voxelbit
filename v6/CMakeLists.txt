# ---------------------------------------------------------------------------
# v6 -- an endless voxel pine forest, path traced on NVIDIA Falcor.
#
# This is NOT a standalone CMake project. It is added by Falcor's own
# CMakeLists.txt, which is configured with
#
#     -DFALCOR_EXTERNAL_APP_DIR=C:/voxelbit/v6
#
# by build.bat next to this file. Falcor stays the top-level project because
# several of its rules copy data/ and scripts/ relative to CMAKE_SOURCE_DIR;
# what moves is the BINARY directory, which build.bat points here -- so
# v6.exe, Falcor.dll and every shader land under v6/build/bin and this tree
# is self-contained even though the framework lives elsewhere.
# ---------------------------------------------------------------------------

add_falcor_executable(v6)

target_sources(v6 PRIVATE
    src/main.cpp
    src/app.h

    src/core/defaults.h
    src/core/noise.h
    src/core/vecmath.h

    src/scene/chunks.h
    src/scene/collide.h
    src/scene/daynight.h
    src/scene/sky.h
    src/scene/vox.h
    src/scene/voxelworld.h

    src/render/camera.h
    src/render/player.h

    src/gpu/tracer.h
    src/gpu/world.h
    src/gpu/ddgi.h
    src/gpu/nrd.h
    src/gpu/streamline.h

    shaders/Shared.slang
    shaders/Vecmath.slang
    shaders/Material.slang
    shaders/Sky.slang
    shaders/Trace.cs.slang
    shaders/ProbeTrace.cs.slang
    shaders/NrdPack.cs.slang
    shaders/Ddgi.slang
    shaders/Demodulate.cs.slang
    shaders/Remodulate.cs.slang
    shaders/Tonemap.cs.slang
    shaders/VolFog.slang
    shaders/VolFogInject.cs.slang
    shaders/VolFogMarch.cs.slang
    shaders/Crosshair.ps.slang
)

target_include_directories(v6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# THE WORLD GENERATOR IS FLOATING POINT ALL THE WAY DOWN, and these two flags
# are part of the world's definition rather than an optimisation.
#
# The terrain is roughly twenty-three octaves of value noise per column and
# every decision taken from it -- is this steep enough to be rock, is this dense
# enough to plant a tree, does this cell clash with the one before it -- is a
# THRESHOLD on that float. So a last-bit difference does not blur anything; it
# flips a comparison, and a tree that stood in front of the camera is not there
# any more.
#
# v2 built its generator with exactly these flags. Configured without them, the
# same seed produced a wood five trees different -- including one in the
# immediate foreground of the reference shot. Matching them is what makes a seed
# mean the same thing in both engines.
target_compile_options(v6 PRIVATE /fp:fast /arch:AVX2)

# Where "Bake as default" writes defaults.h back to. An absolute path, because
# the launcher runs the exe from C:\voxelbit and anything relative would land in
# the wrong tree and report success having written nothing anyone compiles.
target_compile_definitions(v6 PRIVATE V6_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Shared.slang is included by BOTH the shaders and the C++, which is what keeps
# the two sides of every struct in one place. MSVC will not preprocess a .slang
# file as a header on its own, so nothing here needs a flag -- but the include
# path has to reach the shaders folder from src/.
target_include_directories(v6 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Copies every .slang above into <runtime>/shaders/v6/, preserving the shaders/
# subfolder -- which is why the tracer loads "v6/shaders/Trace.cs.slang".
target_copy_shaders(v6 v6)

target_source_group(v6 "v6")

# TWO THINGS SAMPLEAPP INSISTS ON that v6 has no use for, and both are cheaper
# to satisfy than to fight:
#
#   * run() starts an embedded Python interpreter before it does anything else,
#     and that interpreter imports the falcor module -- falcor_ext.pyd, from the
#     FalcorPython target. Without it the engine dies on ModuleNotFoundError
#     before a frame is drawn.
#
#   * the constructor calls PluginManager::loadAllPlugins(), which reads
#     plugins/plugins.json and throws if a name in it has no DLL beside it.
#     Falcor generates that list from every plugin in its tree.
#
# FalcorPython already depends on all of them, so this one line covers both. v6
# imports no scenes and uses no render graph, so those thirty-odd DLLs are dead
# weight -- but they cost about nine milliseconds at startup and emptying the
# list does not stop them being built, so it is not worth the moving part.
add_dependencies(v6 FalcorPython)

# ---------------------------------------------------------------------------
# DLSS Ray Reconstruction
#
# NOT the DLSS Falcor bundles. The packman package Falcor builds against ships
# Super Resolution only -- it has no nvsdk_ngx_defs_dlssd.h at all -- and Super
# Resolution is an upscaler that assumes a clean input. Fed a one-sample path
# trace it upscales the noise. Ray Reconstruction is the one trained to
# reconstruct lighting from a noisy trace plus its G-buffer, and it lives in the
# full SDK.
#
# Set V6_DLSS_DIR to a DLSS SDK checkout. Absent, v6 still builds and runs --
# it falls back to accumulating and says so at startup -- because a denoiser
# that turns a missing folder into a wall of compiler errors is worse than one
# that is simply not there.
# ---------------------------------------------------------------------------
set(V6_DLSS_DIR "C:/Users/mrwbh/DLSS" CACHE PATH "DLSS SDK with Ray Reconstruction headers")

set(V6_DLSS_LIB "")
foreach(archdir x86_64 x64)
    if(EXISTS ${V6_DLSS_DIR}/lib/Windows_x86_64/${archdir}/nvsdk_ngx_d.lib)
        set(V6_DLSS_LIB ${V6_DLSS_DIR}/lib/Windows_x86_64/${archdir}/nvsdk_ngx_d.lib)
    endif()
endforeach()

if(EXISTS ${V6_DLSS_DIR}/include/nvsdk_ngx_helpers_dlssd.h AND V6_DLSS_LIB)
    message(STATUS "v6: DLSS Ray Reconstruction from ${V6_DLSS_DIR}")
    target_include_directories(v6 PRIVATE ${V6_DLSS_DIR}/include)
    target_link_libraries(v6 PRIVATE ${V6_DLSS_LIB} d3d12.lib)
    target_compile_definitions(v6 PRIVATE V6_HAS_DLSS=1 NV_WINDOWS)

    # nvngx_dlssd.dll is the model itself, 40 MB of it, and NGX loads it by name
    # from the search path the engine hands over -- which is the exe's own
    # folder. It is not a link-time dependency; it just has to be there.
    foreach(reldir rel dev)
        if(EXISTS ${V6_DLSS_DIR}/lib/Windows_x86_64/${reldir}/nvngx_dlssd.dll)
            add_custom_command(TARGET v6 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${V6_DLSS_DIR}/lib/Windows_x86_64/${reldir}/nvngx_dlssd.dll
                        $<TARGET_FILE_DIR:v6>/nvngx_dlssd.dll
                VERBATIM)
            break()
        endif()
    endforeach()
else()
    message(STATUS "v6: no DLSS SDK at ${V6_DLSS_DIR} -- building without Ray Reconstruction")
endif()


# ---------------------------------------------------------------------------
# PHASE A -- NVIDIA RTXGI 1.3, the DDGI irradiance probes
#
# NOT the RTXGI on this machine at C:/Users/mrwbh/RTXGI. That one is 2.7, and
# 2.x does not contain DDGI at all -- NVIDIA retired the probe volumes after 1.3
# and 2.x ships NRC and SHaRC in their place. 1.3.7 is the last release with the
# irradiance probes, and it is a separate checkout.
#
# TWO THINGS HAVE TO BE BUILT BEFORE THIS FINDS ANYTHING, and both are one
# double-click each:
#
#   build_rtxgi.bat            the static library, once
#   compile_ddgi_shaders.bat   the eight probe shaders, whenever they change
#
# Absent either, v6 still builds and still runs -- it says at startup that the
# probes are off and falls back to v4's path-traced indirect. A missing SDK that
# turns into a wall of compiler errors is worse than one that is simply absent.
# ---------------------------------------------------------------------------
set(V6_RTXGI_DIR "C:/Users/mrwbh/RTXGI-DDGI" CACHE PATH "RTXGI 1.3 (DDGI) checkout")
set(V6_RTXGI_LIB "${CMAKE_CURRENT_SOURCE_DIR}/build-rtxgi/lib/Release/rtxgi-d3d12.lib"
    CACHE FILEPATH "The RTXGI DDGI static library built by build_rtxgi.bat")

if(EXISTS "${V6_RTXGI_DIR}/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h" AND EXISTS "${V6_RTXGI_LIB}")
    message(STATUS "v6: RTXGI DDGI from ${V6_RTXGI_DIR}")
    # SYSTEM, so Falcor's /WX does not fail the build on the SDK's own headers --
    # DDGIRootConstants.h puts `using namespace rtxgi;` at global scope, which is
    # the SDK's business and not something v6 can or should fix.
    target_include_directories(v6 SYSTEM PRIVATE ${V6_RTXGI_DIR}/rtxgi-sdk/include)
    target_link_libraries(v6 PRIVATE ${V6_RTXGI_LIB} d3d12.lib dxgi.lib)
    # RTXGI_COORDINATE_SYSTEM is #error if undefined -- deliberately, because
    # getting it wrong transposes the probe grid silently. 2 is Right Hand,
    # Y-Up, and it MUST match both the value build_rtxgi.bat compiled the
    # library with and the -D the probe shaders were compiled with.
    # BOTH of these are #error-if-undefined in the SDK headers, deliberately --
    # a wrong coordinate system transposes the probe grid and a wrong resource
    # mode changes which half of DDGIVolumeResources the SDK reads. They must
    # match what build_rtxgi.bat compiled the library with and what
    # compile_ddgi_shaders.bat passed the probe shaders.
    target_compile_definitions(v6 PRIVATE V6_HAS_DDGI=1
                                          RTXGI_COORDINATE_SYSTEM=2
                                          RTXGI_DDGI_RESOURCE_MANAGEMENT=0)

    # The probe shaders, rebuilt into the runtime folder on every build. Cheap
    # (eight small compute shaders) and it means a change to the volume's shape
    # cannot leave a stale .cso behind that disagrees with ddgi.h.
    add_custom_command(TARGET v6 POST_BUILD
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/compile_ddgi_shaders.bat
                $<TARGET_FILE_DIR:v6>/shaders/v6/ddgi
        COMMENT "v6: compiling the RTXGI probe shaders"
        VERBATIM)
else()
    message(STATUS "v6: no RTXGI DDGI at ${V6_RTXGI_DIR} (or no lib -- run build_rtxgi.bat) "
                   "-- building without irradiance probes")
endif()

# ---------------------------------------------------------------------------
# PHASES C and D -- NVIDIA Streamline: Super Resolution, Ray Reconstruction,
#                   Frame Generation and Reflex
#
# WHY STREAMLINE AND NOT NGX DIRECTLY, now that both are here. v4 talked to NGX
# directly and that was the right call at the time: it reaches Ray
# Reconstruction and Super Resolution, and Streamline was not installed. It does
# NOT reach FRAME GENERATION. Generating a frame means owning the swapchain --
# something has to sit between the engine and Present to pace the real and
# generated frames and to keep Reflex in step with them -- and that machinery is
# the Streamline interposer. There is no NGX-only path to it.
#
# So v6 keeps both: streamline.h drives SR/RR/FG through Streamline when it is
# present, and dlss.h remains as the direct-NGX fallback for RR when it is not.
# slIsFeatureSupported decides per feature at RUNTIME, which is what makes one
# binary correct on a 20-series card (SR and RR, no FG) and on a 40-series card
# (all three).
# ---------------------------------------------------------------------------
set(V6_STREAMLINE_DIR "C:/Users/mrwbh/Streamline" CACHE PATH "Streamline SDK")

if(EXISTS "${V6_STREAMLINE_DIR}/include/sl.h" AND
   EXISTS "${V6_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
    message(STATUS "v6: Streamline from ${V6_STREAMLINE_DIR}")
    target_include_directories(v6 SYSTEM PRIVATE ${V6_STREAMLINE_DIR}/include)
    target_compile_definitions(v6 PRIVATE V6_HAS_STREAMLINE=1)

    # NOT linked against sl.interposer.lib, deliberately.
    #
    # Linking it would make v6.exe import sl.interposer.dll -- a SECOND
    # module instance, with its own state, separate from the one slang-gfx
    # loads as slp12.dll to create the device with. Windows keys loaded
    # modules by path, so slInit on one would say nothing to the other and
    # Streamline would be initialised in the wrong copy of itself.
    #
    # streamline.h therefore opens "slp12.dll" by name and resolves every
    # entry point out of that handle. Only the headers are needed here.

    # The interposer and its plugins are loaded BY NAME at runtime from beside
    # the exe -- they are not link-time dependencies, they just have to be
    # there. sl.common is the plugin host; sl.dlss / sl.dlss_d / sl.dlss_g are
    # Super Resolution, Ray Reconstruction and Frame Generation; sl.reflex and
    # sl.pcl are not optional for Frame Generation -- DLSS-G refuses to run
    # without Reflex active, and says so through eFailReflexNotDetectedAtRuntime.
    # The nvngx_* files are the models themselves.
    foreach(sldll sl.interposer sl.common sl.dlss sl.dlss_d sl.dlss_g sl.reflex sl.pcl
                  nvngx_dlss nvngx_dlssd nvngx_dlssg)
        if(EXISTS "${V6_STREAMLINE_DIR}/bin/x64/${sldll}.dll")
            add_custom_command(TARGET v6 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${V6_STREAMLINE_DIR}/bin/x64/${sldll}.dll
                        $<TARGET_FILE_DIR:v6>/${sldll}.dll
                VERBATIM)
        endif()
    endforeach()
else()
    message(STATUS "v6: no Streamline at ${V6_STREAMLINE_DIR} "
                   "-- no Frame Generation; Ray Reconstruction still runs through NGX directly")
endif()


# ---------------------------------------------------------------------------
# PHASE C, auxiliary -- NVIDIA Real-Time Denoisers, for ambient occlusion only
#
# NRD is already on this machine: Falcor builds against it (FALCOR_HAS_NRD) and
# deploys NRD.dll and the whole NRD shader tree beside the exe, which is why
# nothing here has to copy either. v6 needs the headers and the import library.
#
# WHY ONLY AO. Ray Reconstruction is a radiance reconstructor -- it takes a
# noisy colour plus its G-buffer and returns a clean colour. It has no input for
# a scalar visibility term and no way to return one, so ambient occlusion cannot
# be given to it at all. That makes AO the one signal denoised outside RR, and
# is exactly why no buffer in this engine is ever denoised twice: the two
# denoisers have disjoint inputs.
#
# Absent, v6 still builds and runs; AO is simply not offered.
# ---------------------------------------------------------------------------
# CMAKE_SOURCE_DIR is Falcor's root -- Falcor stays the top-level project and
# v6 is added through its FALCOR_EXTERNAL_APP_DIR hook, so its packman tree is
# reachable from here without hardcoding a path.
set(V6_NRD_DIR "${CMAKE_SOURCE_DIR}/external/packman/nrd" CACHE PATH "NRD SDK")
if(NOT EXISTS "${V6_NRD_DIR}/Include/NRD.h")
    set(V6_NRD_DIR "C:/packman-repo/chk/nrd/3.1.0-falcor-windows-x86_64")
endif()

if(EXISTS "${V6_NRD_DIR}/Include/NRD.h" AND EXISTS "${V6_NRD_DIR}/Lib/Release/NRD.lib")
    message(STATUS "v6: NRD from ${V6_NRD_DIR}")
    # SYSTEM: NRD's headers are not written against Falcor's /WX warning set.
    target_include_directories(v6 SYSTEM PRIVATE ${V6_NRD_DIR}/Include)
    target_link_libraries(v6 PRIVATE ${V6_NRD_DIR}/Lib/Release/NRD.lib)
    target_compile_definitions(v6 PRIVATE V6_HAS_NRD=1)
else()
    message(STATUS "v6: no NRD at ${V6_NRD_DIR} -- building without ambient occlusion")
endif()
