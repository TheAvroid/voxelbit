# v4 — an endless voxel pine forest, path traced and reconstructed

The same wood as [v2](../v2), rendered by a different machine underneath: one
compute shader doing DXR inline ray tracing on **NVIDIA Falcor**, with **DLSS
Ray Reconstruction** standing in for the accumulation.

The world is 10 cm voxels — the same grid the `pine_1..9.vox` assets are
authored on, so a trunk stands in ground made of the lattice it is made of — and
it goes on as far as you care to walk. You walk it as a person: 18 voxels to the
eye, gravity, a jump, and a head bob.

```
v4.bat                            walk around in the wood
v4.bat --seed 7 --density 0.8     a different, thicker wood
v4.bat --view 8                   a smaller resident ring
v4.bat --out shot.png --spp 256   render one frame offline and exit
v4.bat --help                     every option
```

## Controls

| | |
|---|---|
| `W A S D` | walk (hold shift to sprint) |
| space | jump |
| **`F`** | toggle fly mode |
| **`Y`** | the settings menu |
| left click | capture the mouse and look freely (ESC gives it back) |
| right-drag | look around without capturing |
| scroll | zoom (field of view) |
| arrow keys | scrub the day/night clock (up/down = fast) |
| **`X` + scroll** | day/night speed — scroll **down** past the slowest notch to run time **backwards** |
| `-` / `=` | exposure down / up |
| `[` / `]` | bounces down / up |
| `P` | screenshot &nbsp;&nbsp; `F1` help |
| `ESC` | release the mouse; **ESC again quits** |

ESC always takes two presses. A rule that is only sometimes true is worse than
one that is never true, and it is the half-remembered one that loses the render
you were part way through.

## The noise

A path tracer that accumulates looks superb standing still and like television
static the moment you walk, because walking throws the film away and one sample
of a path tracer is mostly noise. Every engine in this lineage has looked like
that while you moved, and every spatial denoiser tried against it was removed
for the same reason: a voxel canopy is thousands of needle-sized faces, and a
filter that decides how much to blur from the noise it can see cannot tell a
needle from the grain between two needles. The canopy came out as felt.

**DLSS Ray Reconstruction is the other thing.** It is handed the one-sample
radiance *and* the albedo, normal, roughness, depth and motion that produced it,
so it can separate what is noise in the lighting from what is detail in the
surface — and the surface detail is what a voxel wood is made of. It replaces
the film rather than sitting on top of it: with RR running, the tracer draws one
sample a frame and RR carries the history on the motion vectors.

It is on by default. `--no-dlss` hands the frame back to the accumulator, and so
does a driver or GPU that cannot do it — the engine says which at startup and
carries on.

```
v4.bat                          quality mode, 1280x720 traced -> 1920x1080
v4.bat --dlss dlaa              denoise at full resolution, no upscale
v4.bat --dlss performance       trace fewer pixels still
v4.bat --no-dlss                the accumulator, for comparison
```

Measured on an RTX 4070 at 1920x1080 output, six bounces, a radius-6 ring:
**110 fps** in quality mode, tracing 1280x720.

### What makes it exact here

Every guide the denoiser needs is a **by-product of the primary hit**. A
rasteriser draws the scene a second time to produce a G-buffer; here the first
bounce already knows the position, the normal, the material and the distance,
and writing them out is five stores in a branch that runs once per pixel.

The motion vectors are better than that — they are *exact*. Nothing in this
world moves; only the camera does. So where a surface was on screen last frame
is not something to store or interpolate: it is this frame's hit point projected
through last frame's camera, in closed form, with no reprojection error at all.
That is one dot product per pixel, and it is the single largest reason the
reconstruction holds together while you run through a stand of trunks.

Two things in that path are easy to get wrong and neither fails loudly:

- **The jitter sign.** DLSS defines the jitter the way a rasteriser applies it —
  an offset added to the projection, which moves the image *+jitter* pixels. A
  ray tracer instead builds the ray for pixel *+ jitter*, which moves the image
  the other way. The two conventions are opposites, and passing the wrong one
  does not look broken: it looks like an image that is slightly soft and never
  quite resolves, which is indistinguishable from the denoiser being bad.
- **The history reset.** It is for a resize or a mode change, never for camera
  motion. Resetting on movement would throw the history away on every frame you
  walked, which is the entire thing being bought.

Because it is temporal it cannot be photographed with `--out`, which renders one
frame and has no history. `--shot` runs the real viewer for N frames and then
writes a png:

```
v4.bat --shot walk.png --shot-frame 180 --shot-walk
```

`--shot-dt` pins the simulated time per frame, so two captures of two renderers
running at different frame rates still cover the same ground and can honestly be
put side by side.

## Why this engine exists

v2 traces the same forest on OptiX and it is a good renderer. But OptiX is built
for *rendering*, and the shape it forces on a *game* is visible all through v2:

- **A shader binding table.** Geometry is bound to hit-group records, and a
  world that streams has to allocate those records, publish them over the bus
  whenever the resident ring moves, hand a slot on when a chunk is evicted, and
  never exceed a fixed pool — or chunks silently stop drawing. That was the
  fiddliest part of v2 and none of it is about drawing a forest.
- **A device artefact on disk.** `v2.optixir` shares a struct with the exe, so a
  mismatched pair disagrees about where every field lives and the next launch
  dies with an illegal address that reads as a GPU fault. v2's build stages the
  artefacts and publishes them only on a successful link, specifically to keep
  that from happening.
- **A continuation stack.** Recursive shading is sized for the deepest path any
  thread might take, reserved for every thread in flight. v2 flattened its whole
  path loop into ray generation to avoid paying for it.

Under DXR **inline ray tracing** none of those exist. There is no table, so
"which triangle array does this instance read" is a number the instance carries.
There is no separate artefact, because the shader is compiled from source beside
the exe. There is no stack, because the path loop is an ordinary loop in an
ordinary compute kernel and a ray is a function call.

The result is that [`shaders/Trace.cs.slang`](shaders/Trace.cs.slang) is the
whole renderer — one file, one entry point — and it reads like the CPU tracer
this lineage started as.

**Nothing about the picture changed.** The terrain, the scatter, the walk, the
day/night clock, the Preetham sky, the BSDFs and the integrator are v2's,
carried across with every constant untouched.

## Is it the same picture?

Yes, and it is checked rather than asserted. Rendered from the same seed, camera
and sun at 8192 samples per pixel, in linear radiance before any tone curve,
with the denoiser off:

| | mean relative difference | total energy |
|---|---|---|
| v2 at 2048 spp vs v2 at 8192 spp (its own sampling noise) | 3.94 % | 1.00000 |
| **v2 at 8192 spp vs v4 at 8192 spp** | **0.12 %** | **1.00000** |

The two engines disagree by thirty times less than v2 disagrees with itself
between sample counts. They also agree exactly on the world: same seed, same
5410 instances, same trees in the same places.

That is not a coincidence of similar maths. Both draw from the same PCG32
streams in the same order — `shaders/Vecmath.slang` is a line-by-line
transcription of v2's samplers, kept that way precisely so this comparison is
possible. What is left is float rounding between two shader compilers.

Two things had to be fixed before it held, and both are worth knowing:

- **`/fp:fast /arch:AVX2`.** The terrain generator is roughly twenty-three
  octaves of value noise per column and every decision taken from it is a
  *threshold* on that float. A last-bit difference does not blur anything; it
  flips a comparison. Configured without v2's flags the same seed produced a
  wood five trees different — including one standing in the foreground of the
  reference shot.
- **The day/night clock must not touch an offline render.** `--sun-az` places
  the sun directly; the clock places it from a time of day, and its azimuth is
  `azimuthBase + (tday - 0.5) * 180`, so at any hour but noon the two disagree.
  Running the clock before the offline branch overwrote the sun the flags asked
  for with one 18.4 degrees away, and moved every shadow in the frame.

## Building

Needs a [Falcor 8.0](https://github.com/NVIDIAGameWorks/Falcor) checkout with
its dependencies fetched (`setup.bat` in the Falcor tree), Visual Studio with
"Desktop development with C++", and — for the denoiser — a DLSS SDK.

```
build.bat            build (Falcor first, then v4; a few minutes the first time)
build.bat debug      the Debug configuration
build.bat clean      throw the build directory away
rebuild.bat          the same, double-clickable, pauses at the end
```

`FALCOR_DIR` overrides where Falcor is looked for (default
`C:\Users\mrwbh\Falcor`), and the CMake cache variable `V4_DLSS_DIR` where the
DLSS SDK is (default `C:\Users\mrwbh\DLSS`). Without the SDK the engine still
builds and runs, on the accumulator.

### How it is wired up

Falcor is configured as the **source** tree and this directory is pulled into it
through a `FALCOR_EXTERNAL_APP_DIR` hook, while the **binary** directory points
back here. That arrangement is not the obvious one and it is deliberate:

- Falcor has to stay the top-level CMake project. Several of its rules copy
  `data/` and `scripts/` relative to `CMAKE_SOURCE_DIR`, and making v4 the top
  level breaks them.
- Everything built lands under `v4/build` — `v4.exe`, `Falcor.dll`, slang,
  `nvngx_dlssd.dll`, the shaders, the lot. This tree is self-contained and
  nothing is written into the Falcor checkout.

The hook itself is about five lines appended to Falcor's `CMakeLists.txt`
(`if(FALCOR_EXTERNAL_APP_DIR) add_subdirectory(...)`), guarded so it does
nothing when the variable is unset. Falcor 8 has no install or export rules, so
there is no supported way to link against it from outside its tree; this is the
smallest hook that works. The original file is kept beside it as
`CMakeLists.txt.v4-backup`.

Falcor also ships its own CMake and Ninja through packman, and the build uses
those rather than whatever is on PATH — Falcor 8 and several of its vendored
dependencies declare `cmake_minimum_required` below 3.5, which CMake 4 refuses
outright.

## Where things are

```
shaders/Trace.cs.slang    the whole renderer: one compute shader, inline RT
shaders/Shared.slang      structs compiled by BOTH Slang and cl.exe
shaders/Material.slang    the four BSDFs a conifer wood needs
shaders/Sky.slang         Preetham, evaluated (the fit is on the host)
shaders/Tonemap.cs.slang  ACES, into the swapchain

src/gpu/world.h           acceleration structures, streaming, the triangle pool
src/gpu/tracer.h          the film, the guides, and the dispatches
src/gpu/dlss.h            NGX Ray Reconstruction, and nothing else
src/app.h                 the window, the walk, the menu, the readout
src/scene/                the world: terrain, vox models, scatter, sky, clock
src/render/player.h       a person standing on the ground
src/core/                 vectors, noise, and the baked defaults
```

The one worth reading first is `shaders/Trace.cs.slang`. The one worth reading
second is `src/gpu/world.h`, which is where the shader binding table used to be.

## Known rough edges

- **It carries Falcor's baggage.** `SampleApp::run()` starts an embedded Python
  interpreter before it does anything else, and its constructor loads every
  plugin Falcor's build generated — thirty-odd render passes and scene importers
  v4 never opens. They cost about nine milliseconds at startup. Getting rid of
  them means not using `SampleApp`, which would also mean writing the window,
  the swapchain and the ImGui integration by hand.
- **Mouse capture is done through Win32 directly.** Falcor's `Window` keeps its
  GLFW handle private and exposes no cursor mode, so the capture hides the
  cursor and warps it back to the centre of the client area on the HWND that
  `getApiHandle()` does expose.
- **DLSS comes from a separate SDK, not from Falcor.** The DLSS package Falcor
  builds against is Super Resolution only — it has no Ray Reconstruction headers
  at all — and Super Resolution is an upscaler that assumes a clean input; fed a
  one-sample path trace it upscales the noise. So `src/gpu/dlss.h` talks to NGX
  directly against the full SDK.
- **No specular hit distance is handed to the denoiser.** It is an optional
  guide that measurably helps reflections, and nothing here tracks which lobe
  the first bounce took. A wrong hit distance is worse than none, so it is left
  out rather than guessed at.
- **Offline renders do not denoise.** `--out` is one frame and Ray
  Reconstruction is temporal; the accumulator is the honest estimator there
  anyway.
