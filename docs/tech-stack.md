# voxelbit — full technology stack

Two shipping codebases plus a research lineage. Windows 11, single dev machine, NVIDIA RTX 4070.

---

## 1. The engine — `C:\voxelbit\v2`

An endless voxel pine forest, path traced in real time. Branch `main`, 204 commits,
~44,600 lines of C++ headers + Slang shaders. Launcher `C:\voxelbit\v2.bat`,
exe `v2\build\bin\Release\v2.exe`.

### Framework and shading language

| | |
|---|---|
| **NVIDIA Falcor** | private source fork at `v2/external/falcor` (75 MB). Falcor stays the top-level CMake project; v2 is pulled in via `-DFALCOR_EXTERNAL_APP_DIR=C:/voxelbit/v2`. Three fork patches: `gfxEnableDebugLayer(true)`, `/wd4996`, `Device::Desc::existingVulkanHandles`. `FALCOR_ENABLE_USD=OFF`, `FALCOR_HAS_NVAPI=0`. |
| **Slang 2026.13.1** | private copy staged from the Vulkan SDK, selected with `FALCOR_LOCAL_SLANG*`. Falcor's own packman Slang is 2024.1.34 and has **zero** cooperative-vector symbols — this substitution is the reason the engine exists in this form. |
| **Backends** | D3D12 (primary, everything works) and Vulkan (via slang-gfx; needed for cooperative vectors on the stock stack). |
| **Ray tracing** | DXR **inline** ray tracing (`RayQuery`) in compute. Per-chunk BLAS + TLAS. `RAY_FLAG_FORCE_OPAQUE` everywhere — no any-hit, no alpha test. |

### Build toolchain

- **MSVC** — Visual Studio 18 Community, MSVC 14.50 (`vcvars64` → cmake → ninja).
- **Ninja Multi-Config**, using **Falcor's packman-shipped cmake and ninja** (system CMake 4 refuses this tree — packman dependencies declare `cmake_minimum_required` below 3.5).
- `/fp:fast /arch:AVX2 /std:c++17`, guarded to `COMPILE_LANGUAGE:CXX` so nvcc never sees them. **These flags are part of the world's definition**, not an optimisation: terrain is ~23 octaves of value noise per column and every decision taken from it is a threshold on that float, so a last-bit change moves trees.
- **CUDA 13.3**, `CUDA_ARCHITECTURES "75;89"` (89 = Ada).
- Single translation unit — the whole engine is headers included by `src/main.cpp`.

### NVIDIA SDKs wired in

| SDK | Version / location | What it does | State |
|---|---|---|---|
| **DLSS (NGX)** | `C:/Users/mrwbh/DLSS`, `nvngx_dlssd.dll` 310.7.0.0 | Super Resolution + **Ray Reconstruction** — the only denoiser that survives a voxel canopy | Working. RR **preset E pinned** (`--rr-preset 5`) |
| **Streamline** | 2.12.0, `C:/Users/mrwbh/Streamline` | Frame Generation (2x/3x/4x) + Reflex | Wired, D3D12 only. **Not linked** against `sl.interposer.lib` — `gfx.dll` is binary-patched instead (`patch_gfx_interposer.py`, `slgi_forwarder`) |
| **RTXGI 1.3 (DDGI)** | `C:/Users/mrwbh/RTXGI-DDGI`, `rtxgi-d3d12.lib` | Probe-based irradiance GI (`--gi 1`, `--gi-depth 2`) | Working, D3D12 only |
| **RTXGI 2.x — NRC** | `C:/Users/mrwbh/RTXGI/Libraries/Nrc`, `NRC_D3D12.dll` + 4 CUDA DLLs | Neural Radiance Cache; trains on tensor cores through its own DLL, so no SM 6.10 floor | Cache **demonstrably learns**; the resolve weighting is the one unsolved step |
| **RTXGI 2.x — SHaRC** | `.../Libraries/Sharc` (header-only) | Spatial hash radiance cache | Working |
| **NVAPI** | `v2/external/nvapi` | **RTX Mega Geometry** — CLAS, `BUILD_BLAS_FROM_CLAS`, partitioned TLAS | Capability query passes; the builder faults the GPU. Gated behind `--cluster-test`, default OFF |
| **PhysX 5.10.0** | `v2/external/physx`, CPU-only | Rigid bodies for things that move | Working (`--physx`). Needs a custom **Ninja** preset — PhysX presets stop at VS2022 |
| **Agility SDK 1.721.2-preview + DXC 1.10 preview** | `v2/external/{agility-sdk,dxcompiler}` | Shader Model **6.10** → cooperative vectors on D3D12 alongside DLSS | Opt-in: `build.bat preview`. Default OFF (stock = SM 6.8), because a failed preview runtime means no D3D12 device at all. Needs Developer Mode **latched at boot** |
| **NanoVDB 32.3.3** | `v2/external/nanovdb` | Sparse voxel storage; `PNanoVDB.h` compiles as Slang as-is | Used by the `v3`/`v4` branches |

### Rendering techniques (own implementations, in Slang)

- **Path tracer** — `Trace.cs.slang`, compute, sun + sky-dome NEE with MIS power heuristic, Russian roulette, Halton (2,3) jitter, accumulating film.
- **ReSTIR GI** — reservoir + RIS, temporal and spatial passes (`Restir*.slang`). Unbiased. ~8.5% RMSE gain in the walking case. Hard rule learned: **never feed spatial output back as temporal history** (2.4x energy gain from double counting).
- **Own neural radiance cache** — 32→64→64→16 MLP, fp16, frequency-encoded position, **in-shader backprop** via `coopVecOuterProductAccumulate`, `coopVecMatMul(transpose=true)` and `coopVecReduceSumAccumulate`. Fit in log space. Vulkan-only on the stock stack.
- **Atmosphere** — precomputed transmittance / multiscatter / sky-view LUTs.
- **Volumetric fog** and **clouds** (froxel injection).
- **Demodulate / remodulate** around the denoiser, **bloom**, auto **exposure**, **tonemap**.
- **Frame governor** — holds a 60–120 fps band on two axes: bounces and sun samples move freely, resolution and DLSS mode only after 3 s out of band, because changing those stalls the device.
- **Motion vectors are derived automatically** in `World::place` by differencing against last frame's transform at the model centre. Anything that animates by swapping BLASes (a butterfly's flap) needs a measured motion vector of its own or DLSS-RR smears it.

### Windows platform APIs (no vendored SDK)

- **Media Foundation** (`mfplat`, `mfreadwrite`, `mfuuid`, `mf`) — the in-game screen recorder and its editing panel. H.264/MP4, constant frame rate. Chosen over NVENC.
- **XAudio2 2.9** (`xaudio2.lib`, in-box since Windows 10) — forest ambience. Media Foundation's source reader is the mp3 decoder, so audio cost one line and no third-party library.
- `d3d12.lib`, `dxgi.lib`, `shell32.lib`.

### Deliberate non-choices — do not re-litigate these

- **Minimum spec is an RTX 4070; NVIDIA-only is a decision, not a gap.** No AMD/Intel paths (FSR Ray Regeneration, FSR Radiance Caching, wave64 DDGI variants). The cross-vendor product is the WebGPU build, not this.
- **There are no material textures anywhere.** Materials are a 255-entry palette (`float3 albedo, roughness, specular, translucency`); per-voxel shade is an integer hash of the voxel coordinate evaluated at hit time. Detail lives in the BVH — 81.8 M triangles, 625 chunks, a 326 MB triangle pool. This permanently rules out **RTXTS** (nothing to stream), **RTX Neural Texture Compression** (nothing to compress) and **Opacity Micromaps** (nothing to accelerate — every ray is force-opaque).
- **No spatial denoiser.** Every one of them turns a needle canopy to felt. DLSS-RR works precisely because it separates noise in the *lighting* from detail in the *surface*, and this engine's detail is geometric.
- **GVDB was abandoned.** Its read paths work; its mutation paths do not (the atlas cannot exceed 256 bricks, and `ClearChannel` corrupts the byte channel). NanoVDB replaced it and worked first try.

### Repo layout

```
v2/src/core      defaults, value noise, vecmath, blue noise
v2/src/scene     chunks, voxel world, collision, .vox loading, sky, day/night
v2/src/render    camera, player, held item, bow, arrows, birds, butterflies,
                 drops, dynamics, governor, recorder (Media Foundation), audio
v2/src/gpu       tracer, world (BLAS/TLAS), ddgi, sharc, nrc, nrcsdk, restir,
                 neural, clusters, dlss, streamline, atmosphere, volfog, clouds,
                 post, cuda + cuda/kernels.cu
v2/src/physics   PhysX
v2/shaders       36 .slang files; Shared.slang is included by BOTH the C++ and
                 the shaders, which is what keeps both sides of every struct in
                 one place
```

Branches `v3` (NanoVDB store, meshed to triangles) and `v4` (NanoVDB traced directly by the
shader through PNanoVDB) live in the same repo; neither is checked out at the moment.

---

## 2. The browser game — voxelbit.net (`C:\voxelbit\src` → `game/index.html`)

A voxel world raytraced in a browser tab. No rasteriser, no meshes. ~42,300 lines.

- **WebGPU** — a hard requirement with no fallback renderer. Chrome or Edge 113+. If `navigator.gpu` is missing the game says so and stops.
- **WGSL compute shaders** — `pre`, `dda` (voxel traversal), `trace`, `pathtrace`, `denoise`, `cloudgen`, `taa`, `composite`, `blit`, `vis`.
- **Vanilla JavaScript. No framework, no npm, no TypeScript, no JS bundler.**
- **Build**: `tools/bundle.py` concatenates the fragments in `src/` in the order given by `src/manifest.txt` into a single `game/index.html`. That manifest order *is* the dependency graph — a fragment may use anything declared above it and nothing below it. `game/index.html` is a build artifact, never source.
- **Dev server**: `tools/serve-nocache.py` builds in memory on every request — edit a fragment, hit refresh, no build step while you work.
- **Tests**: `tools/lint-vb.py` (artifact freshness plus known traps) and `tools/vbtest.py` (boots the real game in a browser over CDP and diffs it against a baseline).
- **Asset pipeline**: a large Python toolset baking MagicaVoxel `.vox` models — pines, birches, cacti, creatures — into packed JS.
- Features: real sun shadows, ambient occlusion, traced water reflection and refraction, a denoiser, trees that fall where you cut them and break apart, water that freezes and thaws, snow that settles voxel by voxel and melts, and an infinite deterministic world generated as you walk into it.

---

## 3. Research lineage — `C:\Users\mrwbh\pbrt-v4\{v3,v4,v5}`

Untracked, last touched 2026-09-04, superseded by the Falcor engine. The same world over three
different intersection backends — useful as reference points, not as products.

| | Stack |
|---|---|
| **v3** | **OpenGL 3.3 + GLSL** fragment-shader path tracer (lighting model lifted from rustracer's Vulkan RTX glTF viewer), GLFW + glad vendored from the pbrt-v4 checkout, MinGW-w64 g++. Live shader reload on `R`. |
| **v4** | **Intel Embree 4.4.1** for intersection + **Intel Open Image Denoise 2.5.1** + **TBB**, CPU, MinGW-w64 g++. Installed to `C:/Users/mrwbh/intel-rt`. |
| **v5** | **NVIDIA OptiX 9.1** (`C:/Users/mrwbh/optix-sdk`, header-only) + **CUDA 13.3** driver API, MSVC host build, `nvcc -optix-ir -arch=compute_75`. No denoiser — every OptiX AI denoiser mode was built, tuned and then removed. |

Also present: the upstream **pbrt-v4** checkout itself (Pharr/Jakob/Humphreys' book renderer) — the
host repo, used as a reference and for its vendored GLFW and glad.

---

## 4. Machine and shared toolchain

- **Windows 11 Home 10.0.26200**, **NVIDIA RTX 4070**.
- **Visual Studio 18 Community**, MSVC 14.50 — the host compiler for everything GPU.
- **CUDA 13.3**, **OptiX 9.1 SDK**, **Vulkan SDK 1.4.357.0** — the Vulkan SDK's `slangc.exe` validates shaders without a link.
- **MSYS2** at `C:\msys64` — g++ 16.2.0, cmake 4.4.2, ninja 1.13.2, ispc 1.31.0, TBB 2023.1, glfw 3.5.1. Used for the CPU/OpenGL lineage and for GPU-free test harnesses (the `scene/*.h` headers are GPU-free, so terrain and storage can be verified with a plain g++ harness).
- **Python 3** — the entire browser-game build, asset pipeline and test tooling.
- **Git**, with several long-lived worktrees (`C:/vb-cacglow`, `C:/vb-rockrefl`, `C:/vb-spread`).

### Cross-cutting build facts worth carrying

- Embree under MinGW needs `common/cmake/gnu.cmake` guarded with `AND NOT WIN32` — it appends the ELF-only `-z noexecstack -z relro -z now` and `-pie`, which the PE linker rejects.
- OIDN under MinGW needs `-Wl,--defsym,__chkstk=___chkstk_ms`, and its device DLL copied to the unprefixed name it loads by literal string (`OpenImageDenoise_device_cpu.dll`).
- OptiX under MSVC needs `advapi32` and `cfgmgr32` — `optix_stubs.h` finds `nvoptix.dll` by walking the config manager and then reading the driver path out of the registry. `cuda.lib` is a static loader shim, not an import library, so `nvcuda.dll` never appears in the exe's imports.
- packman owns `external/packman/*` and recreates those junctions from `dependencies.xml` on every configure — repointing a dependency by hand does not survive. Override from the app's own POST_BUILD step instead.
- MSVC `/fp:fast` and g++ `-ffast-math` are **not** equivalent here: the noise field is bit-exact under MSVC but diverges by one ulp on ~10% of samples under g++, which reassociates more freely. Build the terrain with one compiler or the world shifts.
