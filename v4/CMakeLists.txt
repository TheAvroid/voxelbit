# ---------------------------------------------------------------------------
# v4 -- the lighting stack, with nothing in front of it.
#
# A path tracer with ReSTIR, SHaRC, a neural radiance cache, DDGI, volumetric
# fog, clouds, a physical atmosphere, post and DLSS Ray Reconstruction -- and no
# geometry at all. Rays are asked what is in front of them through two functions
# in shaders/Store.slang, and the store that answers them holds nothing.
#
# It is a base for trying voxel representations against an IDENTICAL renderer.
# Every backend is measured against this build, which is the zero: what a frame
# costs with the geometry query removed entirely.
#
# This is NOT a standalone CMake project. It is added by Falcor's own
# CMakeLists.txt, configured with
#
#     -DFALCOR_EXTERNAL_APP_DIR=C:/voxelbit/v4
#
# by build.bat next to this file. Falcor stays the top-level project because
# several of its rules copy data/ and scripts/ relative to CMAKE_SOURCE_DIR;
# what moves is the BINARY directory, which build.bat points here -- so
# v4.exe, Falcor.dll and every shader land under v4/build/bin and
# this tree is self-contained even though the framework lives elsewhere.
# ---------------------------------------------------------------------------
#
# 680 MB of Falcor, Slang, the Agility SDK, DXC, NVAPI and WaveWorks. Copying
# them per engine wastes the disk; JUNCTIONING them is worse, because a stray
# recursive delete of an engine directory then walks through the link and takes
# the originals with it. That is not hypothetical -- it is exactly how this tree
# lost its dependencies once already.
#
# So it is a PATH resolved at configure time, pointing at whichever tree owns
# them. Override with -DV4_EXTERNAL_DIR=... to build against a different one.
set(V4_EXTERNAL_DIR "C:/voxelbit/v3/external"
    CACHE PATH "Tree holding falcor/, slang/, agility-sdk/, dxcompiler/, nvapi/, waveworks/")
if(NOT EXISTS "${V4_EXTERNAL_DIR}/falcor/CMakeLists.txt")
    message(FATAL_ERROR "v4: no dependencies at ${V4_EXTERNAL_DIR} -- set V4_EXTERNAL_DIR")
endif()
message(STATUS "v4: dependencies from ${V4_EXTERNAL_DIR}")

add_falcor_executable(v4)

target_sources(v4 PRIVATE
    src/main.cpp
    src/app.h

    src/core/defaults.h
    src/core/bluenoise.h
    src/core/noise.h
    src/core/vecmath.h

    src/scene/daynight.h
    src/scene/sky.h
    src/scene/materials.h
    src/scene/bricks.h
    src/scene/generate.h
    src/scene/models.h
    src/scene/vox.h

    src/render/camera.h
    src/render/player.h
    src/render/audio.h
    src/render/dynamics.h
    src/render/governor.h
    src/render/recorder.h
    src/render/mfvideo.h

    src/gpu/tracer.h
    src/gpu/store.h
    src/gpu/world.h
    src/gpu/ddgi.h
    src/gpu/sharc.h
    src/gpu/streamline.h
    src/gpu/neural.h
    src/gpu/nrc.h
    src/gpu/restir.h
    src/gpu/atmosphere.h
    src/gpu/volfog.h
    src/gpu/cuda.h
    src/gpu/cuda/kernels.cu

    shaders/Shared.slang
    shaders/Store.slang
    shaders/stores/Null.slang
    # THE AABB STORE. AabbShared.slang is listed even though nothing compiles
    # it on its own: it is #included by BOTH halves of the backend -- the Slang
    # one beside it and gpu/store.h as C++ -- and a .slang that is not listed
    # here is never staged next to the exe, so the shader that includes it
    # fails at LOAD with a missing-file error naming a path nobody wrote.
    shaders/stores/Aabb.slang
    shaders/stores/AabbShared.slang
    shaders/VoxelKey.slang
    shaders/Vecmath.slang
    shaders/BlueNoise.slang
    shaders/Material.slang
    shaders/Sky.slang
    shaders/Atmosphere.slang
    shaders/AtmoTransmittance.cs.slang
    shaders/AtmoMultiScatter.cs.slang
    shaders/AtmoSkyView.cs.slang
    shaders/Trace.cs.slang
    shaders/ProbeTrace.cs.slang
    shaders/Ddgi.slang
    shaders/Sharc.slang
    shaders/SharcResolve.cs.slang
    shaders/Demodulate.cs.slang
    shaders/Remodulate.cs.slang
    shaders/NeuralProbe.cs.slang
    shaders/Nrc.slang
    shaders/NrcSdk.slang
    shaders/NrcSdkResolve.cs.slang
    shaders/VoxelKey.slang
    shaders/Restir.slang
    shaders/RestirWorld.slang
    shaders/Clouds.slang
    shaders/CloudFill.cs.slang
    shaders/VolFog.slang
    shaders/VolFogInject.cs.slang
    shaders/RestirTemporal.cs.slang
    shaders/RestirSpatial.cs.slang
    shaders/NrcTrain.cs.slang
    shaders/NrcUpdate.cs.slang
    shaders/Tonemap.cs.slang
    shaders/Bloom.cs.slang
    shaders/Exposure.cs.slang
    shaders/Crosshair.ps.slang
    shaders/CaptureNv12.cs.slang
)

# ---------------------------------------------------------------------------
# The screen recorder (R) and the editing panel behind it.
#
# Media Foundation, not NVENC -- the reasoning is at the top of
# src/render/mfvideo.h. The practical consequence is these four import
# libraries and no vendored SDK at all: mfplat and mfuuid are the platform and
# its GUIDs, mfreadwrite is the sink writer and the source reader, and mf is
# where MFTranscodeContainerType_MPEG4 lives. shell32 is the "show the take"
# button, which asks Explorer to select the file.
#
# All four ship with Windows, so unlike DLSS, RTXGI and PhysX below there is
# nothing to find and nothing to guard on -- the recorder is not an optional
# feature of this build.
# ---------------------------------------------------------------------------
target_link_libraries(v4 PRIVATE mfplat.lib mfreadwrite.lib mfuuid.lib mf.lib shell32.lib)

# ---------------------------------------------------------------------------
# The forest ambience (src/render/audio.h).
#
# One more in-box import library and still no vendored SDK: xaudio2.lib in the
# Windows SDK is the stub for XAudio2.9, which has shipped in Windows since 10
# and needs no redistributable. The DECODER is the Media Foundation above --
# the source reader that already exists for the recorder turns the mp3 into
# PCM, so audio costs this single line rather than a third-party mp3 library.
# ---------------------------------------------------------------------------
target_link_libraries(v4 PRIVATE xaudio2.lib)

target_include_directories(v4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---------------------------------------------------------------------------
# The CUDA half.
#
# Falcor already turned the CUDA language on for the whole project (it uses it
# for its own interop), so nvcc is configured and there is nothing to find. What
# is left is telling it which GPU to generate for and letting the .cu see
# Falcor's headers.
#
# 89 IS ADA, which is this machine's 4070. Real code rather than PTX, so there
# is no JIT pause the first time a kernel runs -- and 75 alongside it as a
# floor, because PTX for Turing will JIT forward onto anything newer. Compiling
# for every architecture NVIDIA ships would quadruple the build for hardware
# nobody here has.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# NVAPI, for RTX Mega Geometry.
#
# Falcor is configured with FALCOR_HAS_NVAPI=0 and that is left alone -- its
# NVAPI use is for shader intrinsics and ray tracing validation, neither of
# which v2 wants, and turning it on would change how Falcor compiles every
# shader. v2 links NVAPI for ITSELF instead and calls it with the ID3D12Device
# Falcor already made. See src/gpu/clusters.h for why this is the cheap door.
#
# Absent, v2 builds and runs exactly as before and says clusters are off.
# ---------------------------------------------------------------------------
set(V4_NVAPI_DIR ${V4_EXTERNAL_DIR}/nvapi CACHE PATH "NVAPI checkout")
if(EXISTS ${V4_NVAPI_DIR}/nvapi.h AND EXISTS ${V4_NVAPI_DIR}/amd64/nvapi64.lib)
    message(STATUS "v4: NVAPI from ${V4_NVAPI_DIR}")
    target_include_directories(v4 PRIVATE ${V4_NVAPI_DIR})
    target_link_libraries(v4 PRIVATE ${V4_NVAPI_DIR}/amd64/nvapi64.lib)
    target_compile_definitions(v4 PRIVATE V4_HAS_NVAPI=1)
    # THE SHADER HEADERS TOO, for Shader Execution Reordering. nvHLSLExtns.h is
    # HLSL and compiles as Slang unchanged -- Falcor's own Utils/NVAPI.slang
    # includes exactly these. Staged rather than vendored, like the SHaRC and
    # NRC headers, so the checkout stays the single copy.
    #
    # NOTE THE DOUBLED "shaders": target_copy_shaders preserves the shaders/
    # prefix, so v4's own land in shaders/v4/shaders/ and Trace.cs.slang
    # includes "nvapi/nvHLSLExtns.h" relative to itself.
    foreach(nvhdr nvHLSLExtns.h nvHLSLExtnsInternal.h nvShaderExtnEnums.h)
        add_custom_command(TARGET v4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${V4_NVAPI_DIR}/${nvhdr}"
                    "$<TARGET_FILE_DIR:v4>/shaders/v4/shaders/nvapi/${nvhdr}"
            COMMENT "v4: staging ${nvhdr} for reordering"
            VERBATIM)
    endforeach()
else()
    message(STATUS "v4: no NVAPI at ${V4_NVAPI_DIR} -- building without cluster operations")
endif()

# ---------------------------------------------------------------------------
# NO PHYSX. It simulated debris against a height field sampled out of the voxel
# columns, and both halves of that need a world. A backend that wants rigid
# bodies brings it back -- build_physx.bat in the v3 tree is still the recipe,
# and it needs its own script because PhysX's presets stop at Visual Studio 2022
# while this machine has 2026.
# ---------------------------------------------------------------------------

set_target_properties(v4 PROPERTIES CUDA_ARCHITECTURES "75;89")
target_include_directories(v4 PRIVATE ${CUDAToolkit_INCLUDE_DIRS})

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
# GUARDED TO C++, because nvcc is now compiling in this target too and it does
# not speak MSVC's flags. "/fp:fast" reaching nvcc is not a warning -- it is
# parsed as an input FILE, and the error says "a single input file is required
# for a non-link phase", which mentions neither the flag nor the language.
#
# The world generator still gets exactly the flags it did: the .cu files hold no
# terrain code, and if they ever do, their float behaviour has to be matched
# deliberately rather than inherited.
target_compile_options(v4 PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/fp:fast /arch:AVX2>)

# Where "Bake as default" writes defaults.h back to. An absolute path, because
# the launcher runs the exe from C:\voxelbit and anything relative would land in
# the wrong tree and report success having written nothing anyone compiles.
target_compile_definitions(v4 PRIVATE V4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Shared.slang is included by BOTH the shaders and the C++, which is what keeps
# the two sides of every struct in one place. MSVC will not preprocess a .slang
# file as a header on its own, so nothing here needs a flag -- but the include
# path has to reach the shaders folder from src/.
target_include_directories(v4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Copies every .slang above into <runtime>/shaders/v4/, preserving the
# shaders/ subfolder -- which is why the tracer loads
# "v4/shaders/Trace.cs.slang".
target_copy_shaders(v4 v4)

# ---------------------------------------------------------------------------
# NRC -- NVIDIA's Neural Radiance Cache, also RTXGI 2.x, and NOT header-only.
#
# THE OPPOSITE SHAPE TO SHaRC BELOW. SHaRC is three HLSL headers and no library;
# this is a binary distribution -- a .lib to link, a DLL that does the training,
# and CUDA beside it. That is also the reason to want it: the training runs on
# TENSOR CORES through that DLL rather than through cooperative vectors in a
# shader, so unlike v2's own neural cache it has no Shader Model 6.10 floor and
# runs on the D3D12 device that has DLSS and the clusters on it.
#
# FIVE DLLs, and all of them are needed. NRC_D3D12.dll is the library; the other
# four are the CUDA runtime and NVRTC it loads at init. Copied beside the exe
# rather than referenced in place, matching the rule the shaders, the DLSS model
# and the preview DXC already follow -- the build output stays self-contained.
#
# It is a SUBMODULE of the RTXGI checkout and is not fetched by default:
#   git -C <rtxgi> submodule update --init Libraries/Nrc
# Without it this block finds nothing and the engine builds without the cache,
# which is the same way every other optional SDK here behaves.
# ---------------------------------------------------------------------------
set(V4_NRCSDK_DIR "C:/Users/mrwbh/RTXGI/Libraries/Nrc" CACHE PATH "NRC (RTXGI 2.x) checkout")
if(EXISTS "${V4_NRCSDK_DIR}/Include/NrcD3d12.h" AND EXISTS "${V4_NRCSDK_DIR}/Lib/NRC_D3D12.lib")
    message(STATUS "v4: NRC from ${V4_NRCSDK_DIR}")
    target_compile_definitions(v4 PRIVATE V4_HAS_NRCSDK=1)
    target_include_directories(v4 PRIVATE "${V4_NRCSDK_DIR}/Include")
    target_link_libraries(v4 PRIVATE "${V4_NRCSDK_DIR}/Lib/NRC_D3D12.lib")
    # The SHADER headers, staged beside v2's own for the same reason SHaRC's
    # are: NrcSdk.slang includes them relative to itself, so they have to land
    # in the doubled shaders/v4/shaders/ directory rather than one level up.
    add_custom_command(TARGET v4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${V4_NRCSDK_DIR}/Include" "$<TARGET_FILE_DIR:v4>/shaders/v4/shaders/nrcsdk"
        COMMENT "v4: staging the NRC shader headers"
        VERBATIM)
    foreach(nrcdll NRC_D3D12.dll cudart64_12.dll nvrtc64_120_0.dll nvrtc-builtins64_128.dll)
        add_custom_command(TARGET v4 POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${V4_NRCSDK_DIR}/Bin/${nrcdll}" $<TARGET_FILE_DIR:v4>
            COMMENT "v4: staging ${nrcdll}"
            VERBATIM)
    endforeach()
else()
    message(STATUS "v4: no NRC at ${V4_NRCSDK_DIR} -- building without the neural cache SDK")
endif()

# ---------------------------------------------------------------------------
# SHaRC -- the spatial hash radiance cache, from RTXGI 2.x.
#
# SHADER HEADERS ONLY. Unlike DDGI there is no library to build and nothing to
# link: SHaRC is three HLSL headers and the algorithm lives entirely in v2's own
# shaders. That is also why it works on BOTH backends, where DDGI is D3D12 only.
#
# They are STAGED next to v2's shaders rather than vendored into this tree, so
# the checkout stays the single copy and `#include "sharc/SharcCommon.h"`
# resolves through the shader search path that target_copy_shaders already set
# up. Copied on every build for the same reason the probe shaders are: a stale
# header that disagrees with the host's struct sizes is a silent wrong answer.
# ---------------------------------------------------------------------------
set(V4_SHARC_DIR "C:/Users/mrwbh/RTXGI/Libraries/Sharc" CACHE PATH "SHaRC (RTXGI 2.x) checkout")
if(EXISTS "${V4_SHARC_DIR}/include/SharcCommon.h")
    message(STATUS "v4: SHaRC headers from ${V4_SHARC_DIR}")
    target_compile_definitions(v4 PRIVATE V4_HAS_SHARC=1)
    add_custom_command(TARGET v4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                # NOTE THE DOUBLED "shaders": target_copy_shaders preserves the
                # shaders/ prefix each file is listed with, so v2's own shaders
                # deploy to shaders/v4/shaders/. The SDK headers are included
                # relative to Sharc.slang, so they have to land beside it rather
                # than one level up -- which is where the obvious path puts them.
                "${V4_SHARC_DIR}/include" "$<TARGET_FILE_DIR:v4>/shaders/v4/shaders/sharc"
        COMMENT "v4: staging the SHaRC shader headers"
        VERBATIM)
else()
    message(STATUS "v4: no SHaRC at ${V4_SHARC_DIR} -- building without the hash cache")
endif()

# ---------------------------------------------------------------------------
# dx/linalg.h, next to the exe.
#
# The DXIL path for cooperative vectors does not include this header from
# anything in this repository -- Slang EMITS an HLSL file that includes it, and
# the preview DXC then has to resolve it. So it is not a compile-time include
# path, it is a RUNTIME one: gpu/neural.h passes it to the downstream compiler
# through -Xdxc, and it has to exist beside the shipped exe for that to mean
# anything.
#
# Copied rather than referenced back into the DXC package so the build output
# stays self-contained, which is the same rule the shaders and the DLSS model
# already follow.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# THE PREVIEW D3D12 STACK, COPIED OVER FALCOR'S AFTER IT HAS DEPLOYED ITS OWN.
#
# WHY IT IS DONE THIS WAY ROUND, which looks crude and is not.
#
# The obvious approach is to repoint external/packman/agility-sdk and
# .../dxcompiler at the preview packages, the way v2 does for Slang. It does not
# survive: those junctions are OWNED BY PACKMAN, which recreates them from
# dependencies.xml on every configure. Repointing them by hand works exactly
# until the next cmake run silently puts 1.4.10 and DXC 1.7.2207 back, and the
# only symptom is that cooperative vectors stop compiling for no visible reason.
#
# Editing dependencies.xml instead would make packman try to DOWNLOAD packages
# that exist only as local files.
#
# So Falcor deploys whatever packman gave it, and then v2 -- which links
# afterwards, so this ordering is guaranteed -- copies the preview runtime over
# the top. The override lives in v2's own build rules, where nothing else can
# undo it, and the shared C:\packman-repo is never written to at all, so v6 goes
# on getting 1.4.10 and DXC 1.7 exactly as before.
#
# WHAT EACH PIECE IS FOR:
#   D3D12Core.dll     the preview runtime, SDK version 620. The stock one will
#                     not load a Shader Model 6.10 shader at all. Refuses to
#                     load without Windows Developer Mode.
#   dxcompiler/dxil   DXC 1.10, the only compiler that knows dx/linalg.h.
#   dxc-inc           that header, resolved at RUNTIME -- see gpu/neural.h.
# ---------------------------------------------------------------------------
# OFF by default. See Falcor's Core/Macros.h for the full reasoning -- briefly:
# the preview runtime will not load without Windows Developer Mode, that state
# is latched at BOOT, and when it refuses the only symptom is that no D3D12
# device can be created at all. That is a much worse day than no neural shading,
# so it is opted into rather than out of.
#
#   build.bat            stock runtime, SM 6.8, DLSS -- everything but coop-vec
#   build.bat preview    preview runtime, SM 6.10, cooperative vectors
option(V4_D3D12_PREVIEW "Ask for the preview Agility SDK (needs Developer Mode + a reboot)" OFF)
target_compile_definitions(v4 PRIVATE V4_D3D12_PREVIEW=$<BOOL:${V4_D3D12_PREVIEW}>)

set(V4_AGILITY_DIR ${V4_EXTERNAL_DIR}/agility-sdk)
set(V4_DXC_DIR ${V4_EXTERNAL_DIR}/dxcompiler)

if(V4_D3D12_PREVIEW AND EXISTS ${V4_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll)
    message(STATUS "v4: preview Agility SDK from ${V4_AGILITY_DIR}")
    add_custom_command(TARGET v4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${V4_AGILITY_DIR}/build/native/bin/x64/D3D12Core.dll
                $<TARGET_FILE_DIR:v4>/D3D12/D3D12Core.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${V4_AGILITY_DIR}/build/native/bin/x64/d3d12SDKLayers.dll
                $<TARGET_FILE_DIR:v4>/D3D12/d3d12SDKLayers.dll
        COMMENT "v4: overriding the Agility runtime with 1.721.2-preview"
        VERBATIM)
else()
    message(STATUS "v4: stock Agility runtime (SM 6.8) -- D3D12 cooperative vectors off; configure with -DV4_D3D12_PREVIEW=ON for them, or run --vulkan")
endif()

if(V4_D3D12_PREVIEW AND EXISTS ${V4_DXC_DIR}/bin/x64/dxcompiler.dll)
    message(STATUS "v4: preview DXC from ${V4_DXC_DIR}")
    add_custom_command(TARGET v4 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${V4_DXC_DIR}/bin/x64/dxcompiler.dll $<TARGET_FILE_DIR:v4>/dxcompiler.dll
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${V4_DXC_DIR}/bin/x64/dxil.dll $<TARGET_FILE_DIR:v4>/dxil.dll
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                ${V4_DXC_DIR}/inc $<TARGET_FILE_DIR:v4>/dxc-inc
        COMMENT "v4: overriding DXC with the 1.10 preview, and staging dx/linalg.h"
        VERBATIM)
endif()

target_source_group(v4 "v4")

# TWO THINGS SAMPLEAPP INSISTS ON that v2 has no use for, and both are cheaper
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
# FalcorPython already depends on all of them, so this one line covers both. v2
# imports no scenes and uses no render graph, so those thirty-odd DLLs are dead
# weight -- but they cost about nine milliseconds at startup and emptying the
# list does not stop them being built, so it is not worth the moving part.
add_dependencies(v4 FalcorPython)

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
# Set V4_DLSS_DIR to a DLSS SDK checkout. Absent, v2 still builds and runs --
# it falls back to accumulating and says so at startup -- because a denoiser
# that turns a missing folder into a wall of compiler errors is worse than one
# that is simply not there.
# ---------------------------------------------------------------------------
set(V4_DLSS_DIR "C:/Users/mrwbh/DLSS" CACHE PATH "DLSS SDK with Ray Reconstruction headers")

set(V4_DLSS_LIB "")
foreach(archdir x86_64 x64)
    if(EXISTS ${V4_DLSS_DIR}/lib/Windows_x86_64/${archdir}/nvsdk_ngx_d.lib)
        set(V4_DLSS_LIB ${V4_DLSS_DIR}/lib/Windows_x86_64/${archdir}/nvsdk_ngx_d.lib)
    endif()
endforeach()

if(EXISTS ${V4_DLSS_DIR}/include/nvsdk_ngx_helpers_dlssd.h AND V4_DLSS_LIB)
    message(STATUS "v4: DLSS Ray Reconstruction from ${V4_DLSS_DIR}")
    target_include_directories(v4 PRIVATE ${V4_DLSS_DIR}/include)
    target_link_libraries(v4 PRIVATE ${V4_DLSS_LIB} d3d12.lib)
    target_compile_definitions(v4 PRIVATE V4_HAS_DLSS=1 NV_WINDOWS)

    # nvngx_dlssd.dll is the model itself, 40 MB of it, and NGX loads it by name
    # from the search path the engine hands over -- which is the exe's own
    # folder. It is not a link-time dependency; it just has to be there.
    foreach(reldir rel dev)
        if(EXISTS ${V4_DLSS_DIR}/lib/Windows_x86_64/${reldir}/nvngx_dlssd.dll)
            add_custom_command(TARGET v4 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${V4_DLSS_DIR}/lib/Windows_x86_64/${reldir}/nvngx_dlssd.dll
                        $<TARGET_FILE_DIR:v4>/nvngx_dlssd.dll
                VERBATIM)
            break()
        endif()
    endforeach()
else()
    message(STATUS "v4: no DLSS SDK at ${V4_DLSS_DIR} -- building without Ray Reconstruction")
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
# Absent either, v2 still builds and still runs -- it says at startup that the
# probes are off and falls back to v4's path-traced indirect. A missing SDK that
# turns into a wall of compiler errors is worse than one that is simply absent.
# ---------------------------------------------------------------------------
set(V4_RTXGI_DIR "C:/Users/mrwbh/RTXGI-DDGI" CACHE PATH "RTXGI 1.3 (DDGI) checkout")
set(V4_RTXGI_LIB "C:/voxelbit/v3/build-rtxgi/lib/Release/rtxgi-d3d12.lib"
    CACHE FILEPATH "The RTXGI DDGI static library built by build_rtxgi.bat")

if(EXISTS "${V4_RTXGI_DIR}/rtxgi-sdk/include/rtxgi/ddgi/DDGIVolume.h" AND EXISTS "${V4_RTXGI_LIB}")
    message(STATUS "v4: RTXGI DDGI from ${V4_RTXGI_DIR}")
    target_include_directories(v4 PRIVATE ${V4_RTXGI_DIR}/rtxgi-sdk/include)
    target_link_libraries(v4 PRIVATE ${V4_RTXGI_LIB} d3d12.lib dxgi.lib)
    # RTXGI_COORDINATE_SYSTEM is #error if undefined -- deliberately, because
    # getting it wrong transposes the probe grid silently. 2 is Right Hand,
    # Y-Up, and it MUST match both the value build_rtxgi.bat compiled the
    # library with and the -D the probe shaders were compiled with.
    # RTXGI_DDGI_RESOURCE_MANAGEMENT is #error if undefined, exactly as the
    # coordinate system is, and for the same reason: it changes the SHAPE of
    # DDGIVolume -- managed mode adds the texture pointers the SDK would own --
    # so a consumer compiled without it would disagree with the library about
    # the layout of the object it is passing around.
    #
    # 0 is UNMANAGED and it MUST match the -DRTXGI_DDGI_RESOURCE_MANAGEMENT=OFF
    # that build_rtxgi.bat compiled the library with. This was missing until the
    # first build that actually had the library to link against, because until
    # then no translation unit ever reached the SDK header.
    target_compile_definitions(v4 PRIVATE V4_HAS_DDGI=1 RTXGI_COORDINATE_SYSTEM=2
                                          RTXGI_DDGI_RESOURCE_MANAGEMENT=0)

    # The probe shaders, rebuilt into the runtime folder on every build. Cheap
    # (eight small compute shaders) and it means a change to the volume's shape
    # cannot leave a stale .cso behind that disagrees with ddgi.h.
    add_custom_command(TARGET v4 POST_BUILD
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/compile_ddgi_shaders.bat
                $<TARGET_FILE_DIR:v4>/shaders/v4/ddgi
        COMMENT "v4: compiling the RTXGI probe shaders"
        VERBATIM)
else()
    message(STATUS "v4: no RTXGI DDGI at ${V4_RTXGI_DIR} (or no lib -- run build_rtxgi.bat) "
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
# So v2 keeps both: streamline.h drives SR/RR/FG through Streamline when it is
# present, and dlss.h remains as the direct-NGX fallback for RR when it is not.
# slIsFeatureSupported decides per feature at RUNTIME, which is what makes one
# binary correct on a 20-series card (SR and RR, no FG) and on a 40-series card
# (all three).
# ---------------------------------------------------------------------------
set(V4_STREAMLINE_DIR "C:/Users/mrwbh/Streamline" CACHE PATH "Streamline SDK")

if(EXISTS "${V4_STREAMLINE_DIR}/include/sl.h" AND
   EXISTS "${V4_STREAMLINE_DIR}/lib/x64/sl.interposer.lib")
    message(STATUS "v4: Streamline from ${V4_STREAMLINE_DIR}")
    target_include_directories(v4 PRIVATE ${V4_STREAMLINE_DIR}/include)
    # NOT linked against sl.interposer.lib, and this is the whole reason frame
    # generation did nothing for so long.
    #
    # Linking it makes v2.exe IMPORT sl.interposer.dll -- a second module
    # instance with its own state, separate from the one slang-gfx loads to
    # create the device with. Windows keys loaded modules by path, so slInit on
    # one says nothing to the other: v2 was initialising Streamline in a copy of
    # itself that owned no swapchain, and every feature query answered happily
    # about a pipeline that was never going to run.
    #
    # gpu/streamline.h opens the interposer BY NAME instead and resolves every
    # entry point from that one handle. Only the headers are needed here.
    target_compile_definitions(v4 PRIVATE V4_HAS_STREAMLINE=1)

    # The interposer and its plugins are loaded BY NAME at runtime from beside
    # the exe -- they are not link-time dependencies, they just have to be
    # there. sl.common is the plugin host; sl.dlss / sl.dlss_d / sl.dlss_g are
    # Super Resolution, Ray Reconstruction and Frame Generation; sl.reflex and
    # sl.pcl are not optional for Frame Generation -- DLSS-G refuses to run
    # without Reflex active, and says so through eFailReflexNotDetectedAtRuntime.
    # The nvngx_* files are the models themselves.
    foreach(sldll sl.interposer sl.common sl.dlss sl.dlss_d sl.dlss_g sl.reflex sl.pcl
                  nvngx_dlss nvngx_dlssd nvngx_dlssg)
        if(EXISTS "${V4_STREAMLINE_DIR}/bin/x64/${sldll}.dll")
            add_custom_command(TARGET v4 POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${V4_STREAMLINE_DIR}/bin/x64/${sldll}.dll
                        $<TARGET_FILE_DIR:v4>/${sldll}.dll
                VERBATIM)
        endif()
    endforeach()
else()
    message(STATUS "v4: no Streamline at ${V4_STREAMLINE_DIR} "
                   "-- no Frame Generation; Ray Reconstruction still runs through NGX directly")
endif()

# ---------------------------------------------------------------------------
# NVIDIA WaveWorks 2.0 -- the FFT wind-wave simulation behind the lake surface.
#
# ONLY THE SIMULATION HALF IS USED, and that is not a shortcut. WaveWorks
# renders its ocean through a quadtree of patches and HARDWARE TESSELLATION
# (OceanSurfaceHS/DS.hlsl in its sample): a hull shader picks a tessellation
# factor and a domain shader displaces the generated vertices. A path tracer
# has no tessellation stage at all -- geometry has to exist in an acceleration
# structure before a ray can be fired at it -- so none of that transfers. What
# does transfer is the simulation: FFT displacement, gradient and moment
# cascades, which it exposes as native D3D12 texture arrays.
#
# IT IS NOT A STANDALONE D3D12 LIBRARY. NVWaveWorks_static.d3d12.lib pulls in
# nvrhi -- it builds an nvrhi::d3d12::Device around whatever ID3D12Device it is
# handed -- so nvrhi_d3d12.lib has to be linked beside it or the only symbol
# that fails is nvrhi::d3d12::Device::Device. Falcor brings its own abstraction
# and the two now wrap the same device; nvrhi is thin enough for that to be
# fine, but it IS a second RHI in the process.
#
# THE LIBRARY IS PROPRIETARY. Its header carries "Any use, reproduction,
# disclosure or distribution of this software ... without an express license
# agreement from NVIDIA CORPORATION is strictly prohibited", and the package
# ships no license file. external/ is gitignored, so nothing here is committed
# -- but shipping a build that links it is a question for whoever holds that
# agreement, not for this file.
#
# Absent, v2 builds and runs exactly as before: the lake keeps the Gerstner
# surface, and startup says so.
# ---------------------------------------------------------------------------
set(V4_WAVEWORKS_DIR ${V4_EXTERNAL_DIR}/waveworks
    CACHE PATH "NVIDIA WaveWorks 2.0 (headers + d3d12 static libs)")

if(EXISTS "${V4_WAVEWORKS_DIR}/include/GFSDK_WaveWorks.h"
   AND EXISTS "${V4_WAVEWORKS_DIR}/lib/NVWaveWorks_static.d3d12.lib"
   AND EXISTS "${V4_WAVEWORKS_DIR}/lib/nvrhi_d3d12.lib")
    message(STATUS "v4: WaveWorks 2.0 from ${V4_WAVEWORKS_DIR}")
    target_include_directories(v4 PRIVATE ${V4_WAVEWORKS_DIR}/include)
    target_link_libraries(v4 PRIVATE
        ${V4_WAVEWORKS_DIR}/lib/NVWaveWorks_static.d3d12.lib
        ${V4_WAVEWORKS_DIR}/lib/nvrhi_d3d12.lib
        d3d12.lib dxgi.lib d3dcompiler.lib)
    # The header switches linkage on this: 0 selects the static library, which
    # is what is vendored. Getting it wrong asks for __declspec(dllimport)
    # symbols that the .lib does not carry.
    target_compile_definitions(v4 PRIVATE V4_WAVEWORKS=1 WAVEWORKS_DYNAMIC_BUILD=0)
else()
    message(STATUS "v4: no WaveWorks at ${V4_WAVEWORKS_DIR} "
                   "-- the lake keeps its Gerstner surface")
endif()

# ---------------------------------------------------------------------------
# NO OPENVDB, NO NANOVDB, AND NOTHING ELSE THAT STORES A VOXEL.
#
# What stood here was 110 lines of ONE backend's storage: a static OpenVDB out
# of vcpkg, a glob of blosc/zlib/zstd/lz4/snappy/tbb/boost archives to link it,
# three warning suppressions, /bigobj because it is a deep v4 library,
# NANOVDB_USE_OPENVDB to select the right createNanoGrid overload,
# OPENVDB_STATICLIB to drop the dllimport decoration, and a POST_BUILD staging
# PNanoVDB.h beside the deployed shaders because Falcor's shader search path
# finds its own older copy first.
#
# Every one of those is the cost of a storage CHOICE, and this engine has not
# made one. A backend brings its own block here -- and that it runs to a hundred
# lines for a single representation is worth knowing before picking the next.
#
# THE TRAPS ARE KEPT, because they cost a cycle each and are in no README:
#
#   * Falcor ships its OWN openvdb.dll (OpenVDB 9, openvdb::v9_0) and Falcor.dll
#     imports it. Link OpenVDB 12 dynamically and one of the two dies at load --
#     "entry point not found" naming a v12_0 symbol. Static is forced, not
#     chosen.
#   * Include vcpkg BEFORE Falcor and NOT as SYSTEM. -external:I is searched
#     after every -I, so Falcor's bundled NanoVDB wins: <nanovdb/NanoVDB.h>
#     resolves old while <nanovdb/tools/CreateNanoGrid.h> resolves new -- a
#     hundred errors inside HostBuffer.h, none naming an include path.
#   * Wrap warning and /bigobj flags in $<COMPILE_LANGUAGE:CXX>. nvcc reports an
#     unrecognised /wd as an EXTRA INPUT FILE -- "a single input file is
#     required for a non-link phase" -- which says nothing about warning flags.
#   * vcpkg must not pick MSYS2's cmake off PATH: it builds an MSVC triplet with
#     the MinGW toolchain and zlib dies first, blaming zlib.
# ---------------------------------------------------------------------------

# NO DLLS ARE STAGED, DELIBERATELY. Everything is inside v2.exe now, and copying
# vcpkg's bin/ here is exactly what broke it the first time: openvdb.dll
# collides with Falcor's.
