# voxelbit

A voxel world path traced in real time. No rasteriser and no textures — every frame
casts rays through a grid of 10 cm voxels on the GPU, and what you see is where the
light actually went.

**One file: `voxelbit.exe`.** Double-click it. It unpacks itself the first time and
starts immediately after that. No installer, no admin rights, nothing to install first.

The wood stands on real ground — a Rocky Mountain National Park elevation model, with
pine, birch, oak, cherry and desert bands laid over it. Trees fall where you cut them
and break into pieces on the way down. Nineteen species of animal live in it, and
everything that comes loose from the world obeys the same rule: nothing floats.

## Requirements

**An NVIDIA RTX card.** Ray tracing, DLSS Ray Reconstruction, Frame Generation and the
neural radiance cache all go through the NVIDIA stack, so this is not a portable
renderer and was never meant to be one. An RTX 4070 is the lowest that has been
measured. Windows 10 or 11, 16 GB of RAM, about 3 GB of disk after unpacking.

`voxelbit.exe` unpacks into `%LOCALAPPDATA%\voxelbit` and runs from there. Screenshots
and recordings land in that folder; delete it to remove the game.

## What is in it

- **Path traced**, one bounce per pixel per frame, with **ReSTIR GI** resampling the
  indirect light and a **SHaRC** hash radiance cache terminating paths early.
- **RTXGI DDGI** probes for the diffuse fill, and **DLSS Ray Reconstruction** denoising
  and upscaling the result. Frame Generation and Reflex on top.
- **World-anchored volumetric fog** — a snapped two-cascade clipmap that stores what
  each parcel of air can *see* rather than how it looks, so the medium is the same
  medium from every direction.
- **Volumetric clouds**, a periodic density cache marched per pixel, with the wind
  riding the day cycle.
- **A sun and a moon.** The moon hangs at the anti-solar point with real phases, a
  photographed face, and moonlight ray traced by the same shadow rays the sun uses.
- **PhysX 5** for everything that comes loose, CUDA interop, and RTX Mega Geometry
  clusters.
- **Real terrain.** USGS 3DEP elevation and NAIP land cover, with Acadia, the Ouachitas,
  the Colorado Front Range and Death Valley as alternative worlds.

## Playing it

WASD to walk, mouse to look, left click to swing what you are holding, scroll to change
tools. **I** for settings, **O** for the arcade level, **R** to record, **ESC** to
pause. **/locate** takes you to anything in the world — a species, a summit, a lake by
its real name.

## Building it

```
engine\build.bat        # build the engine  (Visual Studio, "Desktop development with C++")
run.bat              # run it, --help for every option
run.bat --vulkan     # the Vulkan path: neural shaders, no DLSS
```

The engine lives in [`engine/`](engine/). It needs the NVIDIA SDKs it links — `engine/CMakeLists.txt`
says which, and every one of them degrades to "unavailable" at startup rather than
failing to build, so a checkout with none of them still compiles and runs.

### Building the shipped executable

```
launcher\build.bat         # the self-extracting launcher (one .cpp, no dependencies)
python tools\package.py    # stages the game and writes dist\voxelbit.exe
```

`package.py` copies the engine and the content into `dist/stage`, compresses it with the
Windows Compression API and appends it to the launcher — so no third-party packer is
needed to build it and none is needed to run it. It ships ~1.2 GB as a 755 MB exe.

The layout under `dist/stage/data` is the same shape as this repository on purpose:
every asset path in the engine goes through `asset()` in
[`engine/src/core/assetroot.h`](engine/src/core/assetroot.h), which joins a repository-relative
path onto a root, so a path that works in the source tree works in the shipped game with
no translation in between. That header is also the reason the game can run anywhere at
all — until September 2026 forty-four asset paths began `C:/voxelbit/`.

## Where things are

```
engine/        the engine: C++ and Slang, on NVIDIA Falcor
game/          the art -- .vox models, sound, the pixel font
source/        the authoring tree the art is built from
tools/         the voxelisers and bakers that produced it
launcher/      the self-extracting launcher
docs/          architecture notes
voxelbit.exe   the shipped game, built by tools/package.py
```

## History

voxelbit began as a WebGPU renderer in a browser tab — one `index.html` holding a voxel
ray tracer written in JavaScript and WGSL. That engine was retired in September 2026
once the native one had overtaken it on every axis that mattered; it is in the git
history for anyone who wants a look.

Between the two there were five others — attempts on OptiX, on Falcor with inline ray
tracing, on Bevy Solari and on a Rust spectral renderer — which sat side by side for
comparison for a while and were deleted once this one had won. The engine in `engine/` was
called `v7` while they existed, then `v2`, and took `v1` when the browser game was
retired and it became the only one.

## Contributing

Yes, please — see [contributing.md](contributing.md). Fork it, branch, open a pull
request. You do not need to be invited.

## Licence

Source-available, **not** open source. You may read it and fork it; you may not use,
redistribute or build on it without permission. See [license](license) for the exact
terms, including the third-party components that carry their own.
