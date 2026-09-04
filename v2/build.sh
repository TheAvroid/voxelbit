#!/usr/bin/env bash
# ---------------------------------------------------------------------------
# Build v2. Three compiler invocations, two compilers, no cmake.
#
# THE UNUSUAL PART IS THE TOOLCHAIN SPLIT, and it is worth explaining because
# nothing about it is obvious:
#
#   DEVICE CODE is compiled by nvcc, which on Windows can only drive MSVC as
#   its host compiler. So cl.exe has to exist even though not one line of v2's
#   host code is built with it -- nvcc uses it for preprocessing on the way to
#   OptiX-IR and PTX, and nothing else.
#
#   HOST CODE is compiled by MinGW g++, the toolchain v3 and v4 already use and
#   the one the GLFW in msys64 is built for. It links against the CUDA DRIVER
#   API (cuda.lib), not the runtime: the driver API is plain C with no MSVC ABI
#   anywhere in its interface, so MinGW's ld consumes NVIDIA's import library
#   directly. It prints a handful of "corrupt .drectve" warnings while doing so,
#   which are MSVC linker directives it has no use for. They are harmless.
#
#   OptiX itself needs NO library at link time at all. optix_stubs.h loads
#   nvoptix.dll out of the driver store at runtime, which is why -lcfgmgr32 is
#   in the link line -- that search uses the Windows configuration manager API.
#
# The two device artefacts are loaded from disk beside the exe, so a shader
# change is one nvcc call and a relaunch with no host relink.
# ---------------------------------------------------------------------------
set -euo pipefail

MINGW=${MINGW:-/c/msys64/mingw64}
CUDA=${CUDA_PATH:-/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.3}
OPTIX=${OPTIX_PATH:-/c/Users/mrwbh/optix-sdk}
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$HERE/build"

export PATH="$MINGW/bin:$PATH"

# --- locate cl.exe -----------------------------------------------------------
# nvcc needs it and will not say so clearly if it is missing: the error is a
# preprocessor failure several steps downstream.
if [[ -z "${CCBIN:-}" ]]; then
  CCBIN=$(ls -d "/c/Program Files/Microsoft Visual Studio"/*/*/VC/Tools/MSVC/*/bin/Hostx64/x64 \
            2>/dev/null | sort -V | tail -1 || true)
fi

for f in "$OPTIX/include/optix.h" "$CUDA/bin/nvcc.exe" "$CUDA/lib/x64/cuda.lib"; do
  [[ -e "$f" ]] || { echo "build.sh: missing $f" >&2; exit 1; }
done
[[ -n "$CCBIN" && -x "$CCBIN/cl.exe" ]] || {
  echo "build.sh: no MSVC cl.exe found -- nvcc needs one as its host compiler." >&2
  echo "          Set CCBIN to the folder containing cl.exe." >&2
  exit 1
}

mkdir -p "$OUT"

# --- 1. OptiX device programs -> OptiX-IR ------------------------------------
# compute_75 rather than the 4070's own compute_89: OptiX re-specialises the IR
# for whatever card it finds at runtime, so building for the oldest RTX part
# costs nothing and keeps the artefact portable.
echo "  nvcc  v2.cu -> v2.optixir"
"$CUDA/bin/nvcc" -optix-ir -arch=compute_75 -std=c++17 --use_fast_math \
  -diag-suppress 20044 \
  -I"$OPTIX/include" -I"$CUDA/include" -ccbin "$CCBIN" \
  "$HERE/src/optix/v2.cu" -o "$OUT/v2.optixir"

# --- 2. the display kernel -> PTX --------------------------------------------
# PTX rather than a cubin so the driver JITs it for the installed card.
echo "  nvcc  tonemap.cu -> tonemap.ptx"
"$CUDA/bin/nvcc" -ptx -arch=compute_75 -std=c++17 --use_fast_math \
  -I"$CUDA/include" -ccbin "$CCBIN" \
  "$HERE/src/optix/tonemap.cu" -o "$OUT/tonemap.ptx"

# --- 3. host ------------------------------------------------------------------
CXXFLAGS=(
  -O3 -march=native -ffast-math -fno-finite-math-only
  -std=c++17 -Wall -Wextra -Wno-unused-parameter
  # CUDA's host_defines.h redefines __cdecl, which MinGW has as a builtin, and
  # optix_stubs.h carries an MSVC "#pragma comment(lib, ...)" that GCC has no
  # use for. Both are third-party headers doing something reasonable for the
  # compiler they were written against; neither is actionable here.
  -Wno-builtin-macro-redefined -Wno-unknown-pragmas
  # Where "Bake as default" writes defaults.h back to. The exe is launched from
  # C:/voxelbit, so a path relative to the working directory would land in the
  # wrong tree and report success having written nothing anyone compiles.
  -DV2_SOURCE_DIR="\"$HERE/src\""
  -I"$OPTIX/include" -I"$CUDA/include"
)
LDFLAGS=(
  "$CUDA/lib/x64/cuda.lib"   # the CUDA driver API; see the note above
  -lcfgmgr32                 # optix_stubs uses it to find nvoptix.dll
  -L"$MINGW/lib" -lglfw3 -lgdi32 -lopengl32
  -static-libgcc -static-libstdc++
)

echo "  g++   image.cpp"
# stb is vendored third-party and warns copiously under -Wextra; it is compiled
# on its own so those warnings never bury one of ours.
g++ "${CXXFLAGS[@]}" -Wno-missing-field-initializers -c \
  "$HERE/src/core/image.cpp" -o "$OUT/image.o"

echo "  g++   main.cpp -> v2.exe"
g++ "${CXXFLAGS[@]}" "$HERE/src/main.cpp" "$OUT/image.o" "${LDFLAGS[@]}" -o "$OUT/v2.exe"

echo "built $OUT/v2.exe"
