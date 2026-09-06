# ---------------------------------------------------------------------------
# v4 -- an endless voxel pine forest, path traced on NVIDIA Falcor.
#
# This is NOT a standalone CMake project. It is added by Falcor's own
# CMakeLists.txt, which is configured with
#
#     -DFALCOR_EXTERNAL_APP_DIR=C:/voxelbit/v4
#
# by build.bat next to this file. Falcor stays the top-level project because
# several of its rules copy data/ and scripts/ relative to CMAKE_SOURCE_DIR;
# what moves is the BINARY directory, which build.bat points here -- so
# v4.exe, Falcor.dll and every shader land under v4/build/bin and this tree
# is self-contained even though the framework lives elsewhere.
# ---------------------------------------------------------------------------

add_falcor_executable(v4)

target_sources(v4 PRIVATE
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

    shaders/Shared.slang
    shaders/Vecmath.slang
    shaders/Material.slang
    shaders/Sky.slang
    shaders/Trace.cs.slang
    shaders/Tonemap.cs.slang
)

target_include_directories(v4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

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
target_compile_options(v4 PRIVATE /fp:fast /arch:AVX2)

# Where "Bake as default" writes defaults.h back to. An absolute path, because
# the launcher runs the exe from C:\voxelbit and anything relative would land in
# the wrong tree and report success having written nothing anyone compiles.
target_compile_definitions(v4 PRIVATE V4_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/src")

# Shared.slang is included by BOTH the shaders and the C++, which is what keeps
# the two sides of every struct in one place. MSVC will not preprocess a .slang
# file as a header on its own, so nothing here needs a flag -- but the include
# path has to reach the shaders folder from src/.
target_include_directories(v4 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Copies every .slang above into <runtime>/shaders/v4/, preserving the shaders/
# subfolder -- which is why the tracer loads "v4/shaders/Trace.cs.slang".
target_copy_shaders(v4 v4)

target_source_group(v4 "v4")

# TWO THINGS SAMPLEAPP INSISTS ON that v4 has no use for, and both are cheaper
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
# FalcorPython already depends on all of them, so this one line covers both. v4
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
# Set V4_DLSS_DIR to a DLSS SDK checkout. Absent, v4 still builds and runs --
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

