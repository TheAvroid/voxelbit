# What we tried that failed

**Status:** living record. Written 2026-09-10 from notes taken in session, and meant to be appended to
every time something is abandoned, reverted, or blocked.

**Read this before proposing anything that sounds like a fresh idea.** Everything below was tried
against this engine and did not survive. Each entry says what failed, why, and where the code went —
enough to decide whether a retry is worth it, and enough that nobody re-derives the reason from
scratch. Related: [tech-stack.md](tech-stack.md), [v2-engine-rewrite.md](v2-engine-rewrite.md),
[architecture.md](architecture.md).

**Path caveat:** v7 was renamed to v2, and v4/v5/v6 were deleted. Findings recorded against those
engines still hold; the paths in them do not.

---

## 1. Closed for good — do not propose again

| what | why it is closed |
|---|---|
| ~~**GVDB as the voxel store**~~ **REOPENED 2026-09-10** | **This row was wrong.** The 256-brick cap is a **DLL-boundary artifact, not a GVDB limit.** The 2026-09-08 evaluation recorded the right hypothesis and never tested it: `gvdb.dll` is built with different flags than its caller, so `DataPtr`/`Vector3DI` disagree on layout — reads pass HANDLES and work, allocations write STRUCTS and crash. Built **STATIC** with `GVDB_STATIC` (which `gvdb_types.h:16` has supported all along; GVDB’s own CMakeLists just hardcodes `add_library(gvdb SHARED)`), it allocates **1,331 / 4,096 / 17,576 / 64,000 bricks** — the last is **250x the old cap**, against ~640 needed for a 128 m chunk. Proof, recipe and the five non-obvious build requirements: `C:/voxelbit/v3/tools/gvdb_probe`. Still avoid `ClearChannel` and `AtlasRetrieveBrickXYZ` — those are real GVDB bugs. |
| **RTX Texture Streaming, Neural Texture Compression** | There is nothing to stream or compress. Materials are a 255-entry palette (`V6Material`: albedo, roughness, specular, translucency — no maps); per-voxel shade is `hashVoxel()`, an integer hash of the voxel coordinate, computed at hit time. |
| **Opacity Micromaps** | Nothing to accelerate. Every ray is `RAY_FLAG_FORCE_OPAQUE` and there is no alpha test anywhere, because the needles are real voxel faces rather than cut-outs. |
| **Skipping the held-tool primary trace outside a bounding sphere** (2026-09-23) | Measured in one `V2_ABLATE` run: tracing the held mask on every pixel cost +2% and -1% in two phases -- noise. A ray whose mask culls every instance at the top level is nearly free, so there is nothing to skip. |
| **Reading the gfx query pool directly to avoid Falcor's getElement flush** (2026-09-23) | Measured in one run with groups alternating: 0.31 ms against 0.32 ms a read. gfx's own `getResult` waits as well. The fix that worked was not a different read of the pool but no pool at all -- see `World::emitCompactedSize`. |
| **A seam margin in `nearestWater` to fix "/locate frog, no frogs"** (2026-09-23) | Required the wet column to be in the frog's wood 60 m either side. The arrival moved 7 m and still no frog was born: `woodBit` blends near a seam, so the frog's own wood test passes there too and the wood is not what stops it. Reverted. The open lead is the bank list (`lake_.bankSpots`, 8 spots near the player) against the 30 m birth floor. |
| **Reflex's sleep as the frame-rate limit** (2026-09-23) | ~17 ms of every main-thread frame is `slReflexSleep`, which looks like waste and is not: skipping it moved the same wait into present and the period did not change. The frame is GPU-bound. |
| **DLSS-RR responsivity mask against creature ghosting** (2026-09-24) | `pInResponsivityMask` is in the RR 310.7 header but not the guide. A mask of 1.0 over the WHOLE screen left the output byte-identical on presets E and D: it is ignored. The bias-current-colour mask was already out (preset F only). Plumbing removed. |
| **Per-voxel pose motion vectors, 4 spp, and RR's disocclusion mask for creature ghosting** (2026-09-24) | The ghost is POSE SWAPS: `V2_FREEZE_POSE=1` makes a hopping rabbit at 20 m crisp (rabbit-region diff 25-30/255), and ducks (one mesh) never smear. Pose vectors (per voxel, anchor-aligned, proven live on swap frames) moved the region ~2/255 -- the same as a deliberately wrong, 3x, sign-flipped vector: RR drops history where albedo/normals changed, whatever the vector says. 4 spp and a full-screen disocclusion mask changed nothing either. Code removed. The lever left is fewer swaps at a distance. |
| **Reverse-Z depth for "ghosting worse at a distance"** (2026-09-24) | The standard depth puts a 20-30 m creature ~3e-5 from the ground behind it, which looked like the cause. A rabbit followed at 20 m (`V2_CINEMA_FAR=1`) ghosted identically with reverse-Z and `DepthInverted`, and the same at Balanced and DLAA. Reverted across Trace, Tonemap's flare, ReSTIR and Streamline. |
| **Spatial denoisers on the canopy** | Detail here is geometric, not textural. A filter that infers blur from visible noise cannot tell a needle from the grain between two needles, and the canopy comes out as felt. DLSS Ray Reconstruction is the exception and the reason it works: RR separates noise in the *lighting* from detail in the *surface*, and this engine's detail is surface. |

## 2. Reverted or deleted — the code exists somewhere, the tree does not have it

**Water (v2).** Reverted 2026-09-10 on instruction; the 273-line implementation is in `git stash`,
message *"pre-revert 2026-09-10: per-chunk water…"*. `waterLevel` is back to 2.6 m. The root cause
shared by **every** attempt is the terrain's own distribution — 2 km square, uncompacted:
`min 20.6 / p1 30.4 / p5 35.2 / p10 38.0 / p25 42.8 / med 48.5 / max 74.6` metres. A waterline of
2.6 sits 18 m below the lowest ground in the world, so nothing was ever going to be underwater. A
retry also hits a known BLAS compaction bug.

**OpenVDB / NanoVDB voxel store.** Landed as `f4ced72` and verified running (voxel ground, grass and
water coexisting), then `main` was reset to `fa1234f` and the triangle engine became the baseline
again. Nothing was destroyed — the work is on branch `vdb-live`. If it is ever restored, **rebuild by
hand**: the `rebuild-engines` post-merge hook fails with *"'build.bat' is not recognized"* and leaves
the old exe in place, so you would otherwise be testing the previous binary.

**v3, the ground-up NanoVDB engine.** Deleted 2026-09-09; the source survives only in a zip beside
where it stood. It was not a fork of v2 — procedural pine forest, three concentric LOD grids at
10 cm / 40 cm / 1.6 m, a watertight surface shell rather than a solid, trees stamped at ring
resolution, an analytic Gerstner-wave lake with sandy banks. It rendered. Ray Reconstruction was wired
via raw NGX and returned SUCCESS while writing black; Frame Generation was never wired.

**DLSS Frame Generation via the Falcor interposer patch.** Final verdict: the patch works and then
**crashes the swapchain**. Reverted — do not retry without a plan for the crash. Two things learned in
the attempt are worth keeping: Falcor's `build_scripts/deploycommon.bat` re-deploys `gfx.dll` on every
build and silently undoes the patch; and 3x/4x frame generation needs a 50-series card — Ada does 2x
only, and DLSS-G *clamps* silently rather than failing, so a menu offering 3x/4x on a 4070 gives three
settings that behave identically with no clue why.

**The tray-icon launcher.** `quiet.vbs` + `tray.ps1` were destroyed by a `git clean` and would have to
be rewritten. The launcher now just relaunches itself minimised. Worth knowing before trying again: a
`.bat` cannot avoid creating a console at all — Windows makes the window before cmd reads a line of the
file, so it can only be *moved*, never prevented.

## 3. Blocked — root cause narrowed, still unsolved

**NRC resolve.** NVIDIA's Neural Radiance Cache (RTXGI 2.x) is wired in behind `--nrc-sdk`, runs on
D3D12, and the network demonstrably learns. What is unsolved is the last step — getting its prediction
back into an accumulating film at the right weight. Both halves fail in opposite directions:

| path | RMSE |
|---|---|
| plain path tracer | 0.0100 |
| custom resolve | 0.0295 — contributes nothing |
| SDK built-in `Resolve` | 0.0483 — adds far too much |

**CLAS → BLAS clusters.** The builder compiles and then **removes the device**, so it sits behind
`--cluster-test`, default off. Bisection is the expensive part and it is done: `GET_SIZES` succeeds on
the *same* inputs and args, so the inputs struct, args stride, scratch and result-size array are all
correct and every size and alignment checks out — the fault is in the **per-cluster args** (vertex
slices / shared index buffer), not the operation setup. Already ruled out: CLAS alignment,
`addressResolutionFlags`, `ResourceBindFlags::AccelerationStructure`, and the desc fill itself (diffed
against RTXMG's `translateClusterTriangleDesc` — it matches). `IMPLICIT_DESTINATIONS` is refused
outright with `NvAPI_Status -1`; `EXPLICIT_DESTINATIONS` is accepted and then faults the GPU. Two real
bugs were found and fixed along the way and did *not* resolve it (a missing `NonPixelShader`
transition on the shared index buffer, and `clasAddrs` changing role between ops).

**One backend for everything.** Cooperative vectors are verified working end-to-end on **Vulkan**;
DLSS, DDGI and Streamline are all D3D12-coded (`NVSDK_NGX_D3D12_*`, `rtxgi-d3d12.lib`,
`sl::RenderAPI::eD3D12`). All three have Vulkan APIs available, so this is a port rather than a dead
end — but it is unpaid work standing between the two halves. Note the irony: clusters land on D3D12,
neural lands on Vulkan.

## 4. Solved, but recorded so the failure is not re-derived

**NRC filling with NaN.** Not the optimiser. Every failing run ended in the *same* state — 228 of 7312
weights NaN, 2904 pinned at the ±8 clamp, mean |w| 3007 — and an identical fingerprint across two
completely different optimisers is the tell: two optimisers cannot agree on garbage unless neither
produced it. The cause was that **the NaN guards were being compiled away**: a shader compiler may
assume finite arithmetic, and under that assumption every NaN test is provably false and simply is not
in the binary. Test the exponent bits instead. With that plus Adam, weights transfer between worlds.

**A new Falcor external app not starting.** Five traps, none of which name themselves — `plugins.json`,
`FalcorPython`, `/Zc:__cplusplus`, `colorFormat`, and gamma. Falcor must also stay the top-level CMake
project, with the app pulled in via `-DFALCOR_EXTERNAL_APP_DIR`.

## 5. If a revert took away something you need

`main` was reset to `fa1234f` on 2026-09-09, discarding three commits — `de20c67` (nothing-floats),
`18e36ff` (water-as-voxels) and `bef5daf` (landform). `bef5daf` is safe on branch `v4bat`. The other
two are unreferenced: they exist only as unreachable objects, `git gc` will prune them, and unreachable
reflog entries expire after 30 days by default. Recover with `git reflog` while they last.
