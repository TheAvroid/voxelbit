# voxelbit v2.0 — engine rewrite plan

**Status:** proposal. Nothing is implemented. Written 2026-08-21 against v1.2 (`b25cde9` + uncommitted
working tree).

**Target artifact:** `game/index2.html`, built from a second source tree. `game/index.html` and `src/`
are not touched until v2 is playable and the owner says to switch.

---

## 0. What this document is

Five things were asked for, in the owner's order of priority:

1. **A spherical world** — wrap the flat terrain around a planet.
2. **Shadows that keep up with moving objects** — the felling tree is the canonical failure.
3. **One rendering path for everything** — trace-injected objects must render exactly like grid voxels.
4. **Optimisation** — 120 fps, with felling the known worst case.
5. **True volumetric clouds.**

They are not five independent features. Three of them (2, 3, 4) are the *same defect* seen from three
angles: the engine has seven ways to put a voxel on screen, and six of them are approximations bolted
onto a tracer that was written for one. Fixing the representation fixes all three. That is the spine of
this plan, and the sphere and the clouds hang off it.

This document is deliberately specific about numbers, because the hardest constraint in the whole
rewrite is the frame budget and every design choice below has to be paid for out of it.

---

## 1. The frame budget — read this first

### 1.1 The target, stated in milliseconds

The dev box reports **NVIDIA RTX 4070**, desktop **7680×2160 @ 119 Hz** (two 4K panels), 32 GB RAM.
A fullscreen game window is therefore **3840×2160**, and "120 fps" means **8.33 ms per frame, wall
clock, including present**.

Working split, to be held to:

| | budget |
|---|---|
| GPU total | **6.5 ms** |
| CPU main thread (overlapped, must not be the pacer) | 1.5 ms |
| headroom for present/driver/compositor | 0.3 ms |

### 1.2 Where v1.2 actually is

All measured, not estimated (see `memory/voxelbit-cpu-bound.md`, `voxelbit-perf-profile.md`):

| scene | resolution | GPU | frame | fps |
|---|---|---|---|---|
| open terrain | 1792×865 render, 2240×1081 canvas | 3.32 ms | 3.77 ms | 265 |
| **forest** | **2560×1440 canvas** | **13.6 ms** | **14.6 ms** | **68** |
| test window | 900×600 | 1.5 ms | 3.7 ms | CPU-bound |

(The forest row's *render* resolution was not recorded — spike 0a re-measures it. It is treated
below as 0.7 scale, i.e. 1792×1008.)

Inside the GPU frame at shipping resolution: **trace 63%** (2.10 of 3.32 ms), blit 16%, composite 10%,
everything else 11% combined. At the forest measurement trace is ~80%. Inside trace, the historical
split was AO rays 45% of the whole frame, sun ray ~19%, primary + shading ~18%.

On the CPU: `life` is **79%** of per-frame CPU work (1.02 of 1.28 ms); the frame-time *tail* is the
terrain stream budget, not GC (36 of 40 logged spikes).

### 1.3 The gap

3840×2160 at renderScale 0.7 is **2688×1512 = 4.06 Mpx** traced, against 1.55 Mpx in the open-terrain
measurement. Trace is close to linear in pixel count (its *distance* scaling is sublinear at
`d^0.16`; its pixel scaling is not). Scaling each pass by what it actually depends on — trace,
composite and the denoisers by *render* pixels (×2.62), blit by *canvas* pixels (×3.42):

| pass | measured | at 4K@0.7 |
|---|---|---|
| trace | 2.10 | 5.50 |
| blit | 0.54 | 1.85 |
| composite | 0.34 | 0.89 |
| temporal/taa/spatial/vis | 0.34 | 0.89 |
| **GPU total** | **3.32 ms** | **≈ 9.1 ms → ~110 fps** |

The forest measurement scales the same way to **≈ 30 ms → ~33 fps**. So:

> **v1.2's renderer, unchanged, is at ~110 fps in open terrain and ~33 fps in forest at 4K.**
> Open terrain is already borderline. **The forest is the case that matters, and it needs 3.6×** —
> before adding a single new feature, and this plan adds volumetric clouds, exact dynamic shadows and
> a unified scene traversal on top of it.

That is the real problem statement, and note where it puts the emphasis: this is not a "make it a bit
faster" job, it is "the forest at 4K needs to be nearly four times cheaper". Every section below
states what it costs and what it buys.

### 1.4 The four levers that close the gap

In expected order of value. Items marked ⚠ conflict with a standing "do not re-propose" and need
explicit sign-off — see §11.

| lever | expected | notes |
|---|---|---|
| **A. Temporal upscaling (TAAU)** ✅ | **1.8–2.2×** | Render at 50% and resolve to native with jittered accumulation, replacing "render at 70% and bilinearly blit". Biggest 4K lever; the engine already has 80% of the machinery (YCoCg variance clipping + Catmull-Rom history). **Cleared 2026-08-21** — the owner already plays at 70% and is happy with it, and at a 4K output 50% still means tracing a full 1920×1080. |
| **B. Decouple the lighting rate from the visibility rate** ⚠ | **1.4–1.8×** | Sun + AO rays are ~64% of trace. Run them in a **separate, genuinely smaller dispatch** (half-res, bilateral upsample) rather than a per-pixel skip inside the full-res pass. |
| **C. ReSTIR-style reservoir reuse for direct light** | quality, not speed | Keeps 1 ray/pixel but raises the effective sample count 10–20×. This is what lets moving shadows converge in 2–3 frames instead of 64 — see §5. |
| **D. Unified TLAS traversal** | 1.05–1.3× | Replaces `bodyTrace`'s linear body loop, the `visb` tile bitmask and the `cshad` AABB array with one grid walk. Mostly a correctness/consistency win; the speed win shows up when things are moving. |

Multiplied out, A×B×D spans **2.6× to 5.1×**, against the 3.6× the forest case needs. The target is
reachable but it is not comfortable, and it does not survive losing lever B — which is why lever B
appears in §13 as a decision rather than a plan item. It also assumes a fullscreen 4K window; see
§13 item 1, which may make the whole question smaller.

⚠ **Lever B carries history.** half-rate lighting was built twice in
`rt.html` (per-pixel checkerboard, then wave-aligned 8-wide columns) and reverted both times — "no fps
gain: GPU waves stall on their traced lanes, so per-pixel skips save nothing", and the second attempt
was reverted without a stated reason. **What is proposed here is architecturally different**: not a
skip inside the full-res dispatch, but a smaller dispatch that never launches the lanes at all, which
is the failure mode the first attempt hit head-on. It still needs sign-off before it is built, and it
needs an A/B at 4K, not at the test window.

### 1.5 Rule: no perf claim without a 4K measurement

`memory/voxelbit-cpu-bound.md` is unambiguous: whether this engine is CPU- or GPU-bound **flips with
resolution**. Every number in this plan that matters was taken at ≥1440p. v2's test harness must
default to `Emulation.setDeviceMetricsOverride` at 3840×2160 — a 900×600 measurement is not evidence
about anything in here.

---

## 2. Scope — what is rewritten and what is ported

29,000 lines of `src/` is not thrown away. The rewrite is surgical about which layers change.

| layer | lines (v1) | v2 |
|---|---|---|
| `render/wgsl/*`, `render/*` | ~3,300 | **rewritten** — new scene representation, new pass graph |
| `world/window.js`, `stream.js`, `gen-pool.js` | ~1,300 | **rewritten** — chart coordinates, GPU generation |
| `world/terrain.js`, `gen-noise.js` | ~1,550 | **ported, inputs changed** — the height expressions survive verbatim; only what feeds them changes (§3.4) |
| `sim/physics.js`, `chop*.js`, `support*.js`, `solver.js` | ~2,300 | **rewritten off-thread** (§6); the *rules* port |
| `main/tick*.js` | ~4,700 | **restructured** — instance publishing replaces seven bespoke uniform lanes |
| `sim/life/*`, `nav.js` | ~2,200 | ported; rendering side collapses into instances |
| `assets/*` | ~2,900 | **ported nearly verbatim** — palette, models, held items, bow |
| `ui/*`, `html/*`, css | ~4,200 | **ported verbatim** — HUD, settings, editor, video editor, console, audio, achievements |

Roughly **40% rewritten, 60% ported**. The ported 60% is where a decade of hard-won behaviour lives
and it must not be retyped from memory — see §12.

---

## 3. Pillar 1 — the planet

### 3.1 The honest framing

"Wrap the terrain around a sphere" has two independent halves, and conflating them is the trap:

- **Topology** — does walking east forever bring you back? Are there poles, latitudes, a globe?
- **Curvature** — can you *see* that it's a ball? A horizon that objects sink below?

v1.2 already has a form of the first (a toroidal window over an endlessly cycling biome band) and none
of the second. They need different machinery and they have different costs. Both are in scope.

### 3.2 The radius — DECIDED: as big as it takes to be invisible

**Owner decision, 2026-08-21: "I want the world to be so big, you can't tell that the world is a
sphere."** That settles it, and it settles a lot more than the radius — see §3.5, which now does
nothing.

The threshold is easy to compute. Ground drops away as `d²/2R`. For the drop at maximum view distance
to be under **one voxel (10 cm)** — i.e. literally not representable in the grid, so provably
invisible:

| view distance | R needed for sub-voxel drop |
|---|---|
| 100 m (today) | 50 km |
| 300 m (v2 target) | 450 km |

Anything past that is free, because the world is procedural — **yes, in theory the world can be as
big as you want.** The only real ceilings are integer ones, and they are far away (§3.6).

**Recommendation: R = Earth, 6371 km.** Not for realism theatre — because when every radius above
450 km costs exactly the same, you may as well take the number that means something:

| | at R = 6371 km |
|---|---|
| drop at 300 m | **0.007 m = 0.07 voxels** — zero |
| horizon | 4.65 km, i.e. 15× beyond view distance; the world fades out exactly as it does today |
| circumference | 40,075 km |
| surface | 510 million km² |
| sprint around | 55 days, continuous |
| latitude/longitude | real, with real-Earth intuition for day length and sun elevation |

### 3.2b What this decision buys, and what it does not

This is the honest accounting, because "so big you can't tell" removes the visual payoff entirely.

**What you no longer get** (and would have got at R = 3 km): a visible curved horizon; trees sinking
below it bottom-first; the world ending because it curves rather than because it fades. **The view out
of the window on any given day will look exactly as it does now.**

**What you do get:**

1. **A finite, closed world.** A planet rather than an endless plane. Latitude/longitude coordinates
   instead of unbounded x/z. "You have explored 0.0003% of the world" becomes a real number.
2. **Poles and latitude — a genuinely new content axis.** Ice caps, an equatorial belt, polar summer
   where the sun does not set. None of this is expressible on an infinite plane, and none of it can
   be retrofitted later without redoing worldgen.
3. **Worldgen on a sphere has no seam and no period, by construction.** Today the biome layout has
   both: `BIOP = 10800` voxels, so **the biome sequence repeats every 1.08 km walking east** (pine,
   desert, pine, oak, cherry, repeat) and is near-invariant walking north. On a sphere, 3D noise over
   the direction vector gives irregular, non-repeating biome regions for free — no wrap arithmetic,
   no `pwrap`, no phase bookkeeping, no meander tuning to hide the tiling.

   ✅ **DECIDED 2026-08-21: noise-driven biome regions**, not the current bands. See §3.4b.

**Coupling to view distance is now optional.** The earlier draft argued the sphere and a 300 m view
distance justified each other, because curvature only shows if you can see the horizon. With
curvature deliberately invisible, that argument is void. 300 m view distance is still worth doing —
`memory/voxelbit-view-distance-3x.md` measured it as bounded **only by storage**, at `d^0.16` trace
scaling (+20% for 3×), with the brick pool's 8× memory win already banked — but it is now an
independent decision, on its own merits.

### 3.3 Architecture: a cubed-sphere chart + a flat tangent window

```
  PLANET SPACE          CHART SPACE              WINDOW SPACE
  unit direction   →    (face, u, v)        →    (x, y, z) toroidal voxels
  on sphere R           6 square faces           2048 × 384 × 2048, as today
  ───────────────       ────────────────         ────────────────────────────
  sun, stars,           worldgen input,          DDA, physics, support,
  latitude, the         streaming, the           snow, nav, everything
  globe/map view        biome bands              downstream — UNCHANGED
```

- **Planet space** is a unit direction. Worldgen becomes a function of a 3D direction on the sphere,
  which is strictly *nicer* than the current 2D noise: no seams, no distortion, no special-casing at a
  wrap boundary. Sun elevation, day length and star field fall out of latitude for free.
- **Chart space** is the standard cubed-sphere: 6 faces, each a square `(u,v)` grid, with a
  tangent-adjusted mapping so cell area stays within ~1.3:1 across a face. Streaming and the biome
  bands work in chart space.
- **Window space** is what exists today and does not change. The resident 2048×384×2048 toroidal
  window is a locally-flat patch of the chart. **The DDA, `W`, `hmap`, bricks, physics, support, snow,
  nav and every `*At()` in `terrain.js` keep their current form.**

Across a 2048-voxel window (204.8 m) at R = 6371 km, the local vertical tilts by `r/R` =
**0.0009°** and the sagitta is **1.6 mm**. The window is not an approximation of a chart at that
scale — it is flat to well below the resolution of a voxel, and every downstream system can treat it
as exactly flat with no error term at all.

### 3.4 How `terrain.js` changes (and mostly doesn't)

Every `fillColumn` / `*At(x, z)` / `stamp*` keeps its body. What changes is the **coordinate the
noise is evaluated at**: today `fbm(x*0.008, z*0.008)`, in v2 `fbm3(dir * k)` where `dir` is the unit
direction for that column. The biome bands (`oakM`, `desertM`, `cherryM`, `BAND_MIRROR`, `BIOP`)
become bands in **latitude or in a great-circle angle** instead of bands in `x` — which is both
simpler and finally gives them a real, non-repeating global layout instead of a period.

`memory/voxelbit-desert-biome.md` warns that a biome term must go into **all three copies of `H`**
(main, `makeHRow`, `makeHCol`) and that `gtest = 0` proves it. That invariant survives the rewrite
and the gate must be ported before terrain work starts.

### 3.4b Biome layout: regions, not bands — DECIDED

**Owner decision, 2026-08-21.** The band scheme goes. What replaces it:

**Delete:** `BIOP`, `pwrap`, `BAND_MIRROR`, `OAKOFF`/`OAKC`/`DESOFF`/`DESC`/`CHOFF`/`CHHALF`, the
half-plane masks, the meander wobble that exists to hide the tiling, and the ~200 lines of comment in
`world/window.js` explaining how the six strips were made to come out equal. All of it is bookkeeping
around a repeating x-axis pattern, and on a sphere there is no x axis to repeat along.

**Replace with a Whittaker field** — the classic, and the one that makes latitude actually mean
something, which is the whole reason for the sphere:

```
  temperature(dir) = latitudeTerm(dir)            // pole → equator, the sphere's contribution
                   + noise(dir · kT)              // regional variation
                   - altitudeLapse(H)             // mountains are cold
  moisture(dir)    = noise(dir · kM)              // independent field, different seed

  biome            = table[temperature][moisture]  // oak / pine / cherry / desert / (ice / …)
```

Three properties this has that bands do not: it never repeats, it has no seam anywhere on the sphere,
and it puts deserts where deserts go (hot + dry) rather than wherever the strip counter lands.

**Two invariants that must survive, both already load-bearing in v1:**

1. **The field must BLEND, never switch.** `terrain.js` is explicit that the desert rim "BLENDS over
   `DESB` rather than switching: `H` is continuous noise, so a hard mask would cut a cliff along the
   border." A biome table lookup is a *hard* switch by nature, so the blend has to be reintroduced
   deliberately — carry the two or three nearest biomes with weights and blend `H` across them, the
   way `oakRoll` blends today. Skip this and every biome border is a cliff.
2. **All three copies of `H`.** Main, `makeHRow`, `makeHCol`. `gtest = 0` is the proof, and it is the
   check that catches it.

**Scale is a tunable, and it should start familiar.** Today's strips are 2160 voxels = **216 m** wide,
so the player crosses a biome roughly every 47 seconds of walking. Set the noise frequency so regions
have a similar characteristic width and the world will feel like it does now; widen `kT`/`kM` later if
the crossings turn out to be too frequent once they are irregular rather than rhythmic. Judge this by
walking, not by a map.

**Latitude gets one new biome for free:** polar ice caps, at the two places on the world where the
temperature field bottoms out by construction. Worth building in Phase 4 purely as proof the latitude
axis is real.

### 3.5 Curvature is not rendered. At all.

The earlier draft of this plan spent its longest and riskiest section here: a **segmented DDA** that
bent the primary ray into nine straight pieces to make the ground fall away as `d²/2R`, plus a
matching warp in every other pass and in the TAA reprojection.

**The "so big you can't tell" decision deletes all of it.** At R = 6371 km the ground drops **0.07 of
a voxel** across the entire view distance — below the resolution of the grid it would be drawn into.
There is nothing to render.

What that removes from the plan, all of it risk:

- No segmented DDA, no ray bending, no `+10–20%` on the primary trace.
- No warp in the water reflection/refraction marches, the volumetrics, the clouds, the far field or
  the held viewmodel.
- **No change to the TAA/SVGF reprojection**, which was the subtlest and most bug-prone item in the
  whole rewrite — a warp applied about the *previous* camera position, which would have shipped as
  unexplainable edge-of-frame ghosting if it were ever slightly wrong.
- **Phase 0 spike 0b is cancelled** (§10), and with it the gate that could have forced a redesign.
- The far field becomes an ordinary flat heightfield march instead of a spherical one.

The DDA, the tracer, the denoiser and the pass graph are now **completely unaware that the world is a
sphere.** The sphere lives entirely in the CPU-side coordinate system that feeds worldgen. That is a
much better place for it to live.

### 3.6 The one real trap: coordinate precision at planet scale

A 6371 km planet is 400,750,000 voxels around. Two places care.

**Integers — fine.** The noise lattice is indexed with `Math.imul` 32-bit hashes; int32 holds
±2.1 billion, and a cubed-sphere face edge at Earth radius is ~100 M voxels. Comfortable, with room
for a planet ~5× larger before anything wraps. Worldgen itself runs in JS doubles (exact to 2^53), so
the generator has no precision problem at any radius worth having.

**f32 uniforms — NOT fine, and this is the trap.** `u.winO` (the window's world-corner coordinate) is
passed to the shader as f32. An f32 is exact only to 2^24 = 16.7 M, so a winO of 400 M would quantise
to steps of **32 voxels**. The grain hash uses `vcW = floor(pos) + winO` specifically so per-voxel
noise stays world-stable and does not swim when the window shifts — feed it a quantised winO and the
grain swims by 3.2 m at a time.

> **Rule: never hand the GPU an absolute planet coordinate.** Pass `winO mod 2²⁰` (or any large power
> of two ≥ the noise period). The grain only needs a *locally* consistent world coordinate; wrapping
> it at a power of two is invisible, and the modulus is exact in f32.

Audit for this before Phase 4 lands: every f32 uniform lane that currently carries a world coordinate.

### 3.7 What the player gets

Stated plainly, because §3.2b already made the case that this list is shorter than it would have been
at R = 3 km:

- **The view out of the window is unchanged.** No visible horizon curve. That is the decision.
- A world that is **finite and closed** — walk far enough in one direction (55 days of sprinting) and
  you return. Latitude/longitude replaces unbounded x/z on the HUD.
- **Poles and latitude**: ice caps, an equatorial belt, a sun whose elevation and day length depend on
  where you are. New content that an infinite plane cannot express.
- **No `BIOP` period.** Worldgen stops needing wrap arithmetic to hide a tiling — *if* the biome
  layout is also moved to noise-driven regions (§3.2b, point 3, needs a yes).
- Beyond the resident window: a **far field** heightfield march for the distant silhouette — flat,
  cheap, and no longer entangled with curvature.

---

## 4. Pillar 2 — one scene, one tracer

### 4.1 The seven paths

v1.2 puts voxels on screen seven different ways. This is measured from the source, not asserted:

| # | path | where | lighting |
|---|---|---|---|
| 1 | static world voxels | `trace()` hierarchical DDA over `W` | full: sun ray, AO hemisphere, sky, fog, SVGF |
| 2 | rigid bodies (felled chunks) | `bodyTrace()` — **linear loop over all bodies**, dense per-body grid, ≤320 steps | full |
| 3 | trace-injected creatures | primary trace, per-slot model grid + `visb` tile cull | full, but `h.vox = 0` so **no material identity** |
| 4 | analytic composite objects | `composite.js` drop loop — fireflies, through-water creatures | no SVGF, no AO, hand-written fog |
| 5 | grid-stamped creatures | real voxels in `W` (mammals, perched birds) | perfect — but grid-quantised motion |
| 6 | held viewmodel | `blit.js`, camera space | **no irradiance at all**; sun/sky visibility are JS-marched scalars |
| 7 | overlays (hearts, particles, craft preview) | analytic composite blocks | invented camera-space key light |

The owner's complaint — *"the rendering of the static objects is fine, it's making the trace-injected
objects render like the static objects"* — is exactly the gap between row 1 and rows 2–7.

`memory/voxelbit-unify-render-paths.md` already diagnosed the mechanism precisely: a creature hit sets
`h.vox = 0u`, so **every test keyed on a material id goes dead for it** — `isRockV` (sun sheen),
`isSandV` (glisten), the foliage bit, water/lava faceId, snow. Every new material feature has to be
re-plumbed into the injected path by hand, and gets forgotten. That is the whole historical bug list
(`voxelbit-trace-injected-lighting`, `voxelbit-creature-fog-and-arrows`,
`voxelbit-drop-slot-bands`).

### 4.2 The v2 representation: instances over volumes

```wgsl
struct Instance {          // ~48 bytes
  pos      : vec3<f32>,    // window space
  scale    : f32,
  rot      : vec4<f32>,    // quaternion — full 6-DOF, and it is what motion vectors come from
  volume   : u32,          // index into Volume[]
  matBase  : u32,          // index into the material table
  flags    : u32,          // STATIC | DYNAMIC | TRANSLUCENT | EMISSIVE | NO_SHADOW
  prevSlot : u32,          // index into PrevInstance[] for reprojection
}

struct Volume {
  dims     : vec3<u32>,
  brickBase: u32,          // into the shared brick pool — SAME pool the terrain uses
  mipBase  : u32,          // 2-level occupancy, SAME format the terrain uses
}
```

- **Terrain** is an instance with identity rotation. Not a special case in the traversal — only in
  how its volume is filled.
- **Rigid bodies, creatures, drops, held items, decorative models** are all instances. Same struct,
  same traversal, same shading.
- **TLAS = a coarse uniform grid** (32-voxel cells, reusing the `bricks2` layout) holding instance-id
  lists, rebuilt each frame in one compute dispatch. A uniform grid beats a BVH here because
  instances are small, numerous, and the world is *already* a grid. Rays walk it with the DDA that
  already exists.
- **BLAS** = the per-volume brick/mip hierarchy. `memory/voxelbit-view-distance-3x.md` measured that
  the paged brick pool is **1.94× memory and 6.5% faster** than the dense array, because the dense
  inner loop strides `1 / WX / WX*WY` across 384 MB while the pool strides `1 / 8 / 64` inside one
  512-byte page. **Every volume in v2 lives in that pool** — a 40×200×40 felled pine gets the same
  paging, the same empty-brick skipping and the same cache behaviour as the ground it landed on.

This one change deletes, in a single stroke: `bodyTrace`'s linear body loop, the 320-step dense body
walk, the `visb` per-tile bitmask, the 16-slot `cshad` AABB array, the drop-slot band arithmetic
(and its long-standing off-by-one — `memory/voxelbit-drop-slot-bands.md`), and the compacted-slot
bookkeeping in `tick-creatures.js`.

### 4.3 Material identity by tag, not by palette id

**Do not give injected voxels palette ids.** `memory/voxelbit-unify-render-paths.md` and
`voxelbit-palette-full.md` are explicit about why: the palette is at 256/256 with **zero free slots**,
and injected cells currently carry raw RGB, which is what lets the desert band alone hold ~110
authored colours for free.

Instead: every Volume names a **material table base**. A cell value indexes that table and yields
`{rgb, class}`. Terrain's table *is* the 256-entry palette. A creature's table is its own authored RGB
set, each entry tagged with a class. Then:

```wgsl
  isRock(hit)  →  mat(hit).class == MAT_ROCK      // works identically for terrain and a creature
```

Every material test in the engine becomes one interface with two backings. No palette ceiling, no
grid-quantised motion (which is what got the worm/duck grid-stamp reverted —
`memory/voxelbit-worm-gridstamp.md`), and **new material features reach injected objects
automatically instead of being forgotten.**

Two deliberate divergences to preserve, because they were tuned by eye and are not bugs:
- **Grain amplitude**: ±12% for world/bodies, ±5% for creatures (an authored colour transition must
  dominate the noise). Becomes a per-material-table constant.
- **Baked self-AO** in creature cell colours. In v2, with real ray AO reaching injected geometry
  properly, this should be *removed* from the art and taken from the ray — but that is an art change
  and needs a side-by-side before it happens.

### 4.4 The pass graph

```
  CPU  ── publish instances (transform + prev transform) ──┐
                                                           ▼
  0  GEN        (as needed)  worldgen → brick pool
  1  TLAS       build instance grid + dynamic occupancy volume     §5.2
  2  GBUF       primary trace, full render res
                → albedo·rgb8 | normal·oct | matClass+flags | motion·rg16 | instanceId·r32u | depth
  3  LIGHT      sun + AO + local lights, HALF res, ReSTIR reservoirs        §1.4 B/C
  4  DENOISE    temporal (reservoir-aware) → spatial (bilateral, à-trous)
  5  UPSAMPLE   bilateral upsample of irradiance to render res
  6  CLOUDS     quarter res, temporally amortised                            §7
  7  TRANSLUCENT one thin layer: water surface, wings, glass                 §4.5
  8  COMPOSITE  shade = albedo × irradiance; fog, volumetrics, water, clouds
  9  TAAU       jittered temporal resolve, render res → native               §1.4 A
 10  POST       god rays, DOF, exposure, vignette
 11  BLIT       present
```

Every visible thing enters at pass 2 as an instance. **There is no second place to draw a voxel.**

### 4.5 The one documented exception: translucency

The tracer resolves one opaque surface per pixel; alpha genuinely cannot live there. v1.2's answer was
`ITEMMAP.w` as a free coverage lane in the composite overlay
(`memory/voxelbit-translucency.md`), which works but is another bespoke path.

v2: **a single translucent layer.** Pass 7 traces the nearest `TRANSLUCENT`-flagged surface in front
of the opaque hit and writes `{rgb, alpha, depth}`. Water, fly/dragonfly wings and any future glass
all use it. Composite blends one layer over one opaque hit.

This is the *only* sanctioned exception to "one path", and it is written down here so it does not
quietly become seven again.

### 4.6 The held viewmodel stops being special

Today the viewmodel lives in `blit.js` in camera space with **no irradiance**, and its sun/sky
visibility are scalars marched in JavaScript and shipped in `heldCfg`
(`memory/voxelbit-helditem-lighting.md`, and note the trap recorded there: `solidTab` excludes
foliage, so it was the wrong occlusion test to begin with).

In v2 it is an instance pinned to the camera with a zero motion vector, traced by the same tracer,
lit by the same rays. `memory/helditem-rigid-svgf.md` (animate by rigid anchor motion, never
intra-buffer voxel moves) is *automatically satisfied* by the instance model, because a pose change is
a transform change.

The same fix carries the open item from `memory/voxelbit-resume-2026-08-15.md`: **the floating hearts
become viewmodel geometry** rather than an analytic overlay with an invented key light.

---

## 5. Pillar 3 — shadows that keep up

### 5.1 Root cause, from the source

Three distinct defects stack up, and only one of them is a denoiser problem.

**(a) The occluder query is fake for creatures.** `trace.js` tests **axis-aligned bounding boxes**
against 16 uniform slots (`u.cshad[s*2]`, culled at a 40-voxel radius). A bird's shadow is a *box*.
That is not a tuning issue; it is a different shape.

**(b) The reprojection is translation-only, and a falling tree rotates.** `denoise.js` reprojects a
creature pixel by `lifeMotV(slot).xyz` — a rigid **translation** delta. The comment in the source says
it outright: *"rotation (not in the rigid delta) and newly revealed surfaces fail this distance check
naturally."* A felling tree's dominant motion **is rotation**. So every pixel on and around it fails
the history test, every frame, and falls back to 1 sample.

**(c) The reactive bound is one sphere over ALL bodies.** `u.physBound` is a single enclosing sphere;
`trace.js` then flags reactive anything within `physBound.w + 24` (the AO reach). Fell one tree and a
large region of screen drops to a 10-frame history — hence noise everywhere near a fall, not just on
the shadow.

And then the denoiser is asked to fix it: 1 binary sun ray per pixel, `maxHist` 64, capped to
`min(hBase, 10)` on reactive pixels. **10 samples of a binary variable has a ~16% standard error** —
so the spatial pass filters at radius 2.6 px, which is the smear. At 120 fps, 10 frames is 83 ms of
visible lag.

Note the tuning already in there is *good* — the ease toward a 10-frame floor rather than a snap to 4,
and the sign-carried reactive flag that stops moving-shadow pixels mixing with settled ones. Those are
correct decisions fighting a structural problem. **The structure is what v2 changes.**

### 5.2 The fix: a dynamic occupancy volume

This is the Teardown lesson and it is the single most important change in this section. Teardown keeps
a **1-bit volumetric shadow map of the whole play area** (1752×100×1500 in Marina, 2×2×2 per texel,
3 mips), updated by CPU uploads *in the middle of the render*, and every shadow / AO / local-light ray
queries **that** — not a per-object list. A moving object's shadow is correct **in the frame it
moves**, and the denoiser only ever removes sampling noise, never chases a lagging signal.

For v2:

```
  DYNAMIC OCCUPANCY VOLUME
  512 × 256 × 512 voxels, 1 bit          = 8.4 MB
  + 64 × 32 × 64 brick mip (8³), 1 bit   = 16 KB
  centred on the player, cleared and rebuilt every frame in one compute dispatch
  every DYNAMIC instance conservatively rasterises its voxels into it
```

Then:

```wgsl
  occluded = staticOcc(p) || dynamicOcc(p)     // one query, no body loop, no AABB, no 16-slot cap
```

- **Cost**: one dispatch of `Σ dynamic voxel count` threads. A felled 86,365-voxel oak plus 372
  creature slots is well under 200k threads — sub-0.1 ms. Clear is a 8.4 MB fill, or better a
  dirty-region clear.
- **Sized for weak devices**, per `memory/voxelbit-v1-plan.md`: 8.4 MB is affordable on an iGPU, and
  the volume can drop to 256³ (1 MB) on the low tier with dynamic shadows limited to 12.8 m.
- Dynamic objects beyond ~25 m fall back to the static structure — their shadows are sub-pixel there.

**What this buys, all at once:** exact voxel-shaped creature shadows (currently boxes); shadows that
are correct in the current frame (no lag, at any history length); the deletion of the `cshad` array
and its 16-slot limit; and secondary rays that never touch the body list.

### 5.3 The fix: full 6-DOF reprojection

Publish `prevTransform` alongside `transform` for every instance. A pixel's motion vector is then
computed from the instance's **full rigid pose delta** (rotation included), not a translation. A
rotating trunk's pixels reproject correctly and keep their history.

This is a ~30-line change in the new architecture and it is arguably the *single highest-value fix*
for the specific complaint about felling trees.

### 5.4 The fix: ReSTIR for direct light

With (5.2) and (5.3) in place, the remaining problem is honest Monte-Carlo noise from the penumbra
cone jitter. The modern answer is spatiotemporal reservoir resampling:

- Keep 1 sun ray per pixel.
- Maintain a per-pixel **reservoir** (chosen light sample + weight) and resample it from temporal and
  spatial neighbours, validating against the current frame's occupancy.
- Effective sample count rises to 10–20 with one trace, and — the important part — the reservoir is
  **validated, not accumulated**, so a reservoir invalidated by a moved occluder is replaced this
  frame instead of averaged away over 64.

Expected outcome: a moving shadow that is sharp and correct at 2–3 frames of history (17–25 ms) rather
than smeared at 10 (83 ms).

### 5.5 Split the signals

`gIrr.rg` currently carries sun and sky in one texture with one history length. They have completely
different temporal characteristics: **shadows change fast, ambient occlusion changes slowly.** Split
them so AO can keep a 64-frame history (where it belongs, and where its 45%-of-frame cost is amortised)
while the shadow term runs short.

### 5.6 Tighten the reactive bound

Once 5.2–5.4 land, the global `physBound` sphere should not exist. Reactivity becomes per-pixel and
derives from the dynamic occupancy volume itself — if the sun ray for this pixel crossed a dynamic
brick, this pixel is reactive. Exact, local, and free (the ray already walked it).

---

## 6. Pillar 4 — physics that doesn't hitch

### 6.1 Measured cost, from `memory/voxelbit-fell-profile.md`

On the largest oak (86,365 voxels, box 114×112×114):

| | ms |
|---|---|
| **the fell swing** | **83–130** — `phSeparate` 78–107 |
| ↳ gather (`phComponent`) | 22 |
| ↳ **body build** | **107** ← the spike |
| ↳ `gpuPatch` of 67–86k cells | ~28 |
| ↳ `coneWake` | 22–36 |
| every other biting swing | ~11 — `phFlood` ~11 |

At 120 fps that is a **10–16 frame stall** on the fell and a 1.3-frame stall on every swing.

**Critical history:** a 2026-08-20 optimisation pass took the fell from ~121 ms to ~89 ms across four
changes (sparse `phMark` clear, `coneWake` index hoist, `phBuildBody` Set→byte-scratch, gpuPatch
stamp/list) and the owner reported *"you screwed up the tree felling mechanics, revert changes"*. It
was reverted wholesale, **and which of the four broke it was never established.** The *measurements*
still stand; the *changes* are unproven. In v2 these are rewritten from a clean design rather than
retried as patches — but the lesson holds: **felling behaviour is judged by feel, and a perf change
that alters it is a failure regardless of the numbers.** Land one thing at a time, with a felling
behaviour check between each.

### 6.2 The v2 design

**Get it off the main thread.** Physics, connectivity and body construction run in a worker over a
`SharedArrayBuffer`. The main thread publishes cut events and reads back transforms. A 107 ms body
build off-thread is invisible; on-thread it is a 13-frame stall. This alone converts the worst case
from "unplayable hitch" to "nothing".

**Incremental connectivity.** Maintain **persistent component labels** per voxel (union-find with path
compression). A cut re-labels only within a bounded region seeded from the cut plane, instead of
flooding the whole tree box. `phFlood`'s ~11 ms per swing is a whole-box operation over a 1.43 M-cell
box against 67 k occupied voxels — the asymmetry is the bug.

**Amortise the split.** A fell is *cut → separate → simulate*. The tree can begin falling as one body
on the frame of the cut, and split into components one or two frames later when the labelling lands.
Nobody can see the difference; everybody feels the stall.

**Bodies use the shared brick pool.** From §4.2 — a body is a Volume like any other. This deletes the
dense per-body grid, its 320-step walk, and the `gpuPatch` of 67–86 k cells (the body's voxels move
into pool pages; the world's erased region is a brick-level invalidate, not a per-cell patch).

**Solver upgrade.** v1 is sequential-impulse PGS at a fixed 60 Hz. v2 target: **substepped
TGS / XPBD** with solver islands and proper sleeping. Substepping gives better stacking and
convergence for the same iteration budget — which for a game whose signature moment is a tree
collapsing into a pile of logs is the right place to spend.

**Hierarchical contacts.** Voxel-body vs. voxel-world contact generation tests at brick level first,
then per-voxel only inside bricks that overlap.

### 6.3 The support system

`sim/support.js` (492 lines) plus `support-rules.js` and `tick-support.js` are a large, subtle,
much-debugged subsystem — see `voxelbit-support-queue-saturation`, `voxelbit-floater-cap`,
`voxelbit-gen-orphans`, `voxelbit-pinecone-hanger`, `voxelbit-treeaudit-drape`. **Port the rules
verbatim.** Change only where the queue lives (worker) and how orphans are found (persistent labels
instead of a flood). Every audit tool (`__vb.floatAudit`, `bodySupport`, `supFlood`) must be ported
*first*, because they are how any of this is verifiable.

---

## 7. Pillar 5 — volumetric clouds

### 7.1 Required first step: find out what failed

`memory/voxelbit-cloud-attempts.md` records **three** cloud approaches built and reverted in
`rt.html`, and it says plainly: *"If clouds come up again, first ask what specifically failed (look /
perf / artifacts) before building anything."*

- 2D Worley / value-noise sheets on the sky dome — read as a flat overcast sheet.
- **Cloud-slab raymarch** — a finite height band of `cfbm` density, Beer extinction, short sun-march
  self-shadow, Henyey-Greenstein forward scatter. *"It did not work."* Reverted.
- **Voxel clouds** — a 10 cm cloud-voxel slab at y = 200–244, `fbm3` density, front-to-back
  transmittance, 1-tap sun shadow. Reverted.

Also relevant: **cloud shadows** were built in the 2026-08-09 tier-2 pass (one deterministic sample up
the sun ray to mid-deck, +0.120 ms) and **rejected** along with eight other terms — cost was never the
objection.

**So: no cloud code gets written in v2 until the owner supplies a reference** — a screenshot, a game,
a photo — of what the sky should look like. That is a plan item, not a stalling tactic: three
implementations have already been paid for against an unstated target.

### 7.2 What is genuinely different this time

Assuming the reference lands, here is what v2 has that the three failed attempts did not:

1. **A spherical shell, not a flat slab.** Clouds live between radii `R+alt₀` and `R+alt₁`. On a
   sphere the deck **converges toward the horizon** and individual clouds get foreshortened and
   stacked — which is most of why a real sky reads as a real sky, and is precisely what a flat slab
   between two `y` planes cannot do. This is a new capability the sphere unlocks, not a retry.
2. **A weather map, so clouds have *types*.** A low-frequency 2D texture (coverage, cloud type,
   precipitation) driving the density profile, so the sky holds cumulus *and* stratus *and* clear
   lanes, instead of one uniform noise field everywhere. Every previous attempt was one density
   function applied uniformly, which is why they all read as "mush" or "sheet".
3. **Perlin-Worley erosion at two scales**, not fbm. Worley's cellular structure is what makes
   cauliflower edges; value-noise fbm cannot produce them at any octave count.
4. **Energy-conserving multiple scattering** (the multi-octave approximation), not single-scatter
   Beer. Single-scatter clouds are grey and dead in the interior — a specific, recognisable failure
   that matches "it did not work".
5. **Quarter resolution + temporal amortisation.** Render 1/16 of the cloud pixels per frame in a
   4×4 Bayer pattern, reproject the rest. This is what makes the cost affordable at 4K.

**Budget: ≤1.2 ms** at 3840×2160. At quarter res (960×540 = 0.52 Mpx) with 1/16 amortisation that is
32k pixels of march per frame at 32–64 adaptive steps with early-out at transmittance < 0.01. That is
comfortably inside 1.2 ms; the risk is temporal artefacts on fast camera motion, not cost.

**Cloud shadows: hold.** They were built and rejected once. Re-propose them only after real volumetric
clouds exist and look right, and expect the same verdict if the clouds don't move convincingly.

---

## 8. Cross-cutting: lighting that resembles real life

The owner's phrasing was *"lighting needs to resemble real life while still performing at 120 fps"*.
Concretely, in v2:

- **A real, physical sky model** (Hosek-Wilkie or a precomputed multi-scatter LUT) instead of the
  hand-tuned `skyBase` gradient — the ambient term then *is* the sky, at every time of day, with no
  separate tuning.
- **Physical exposure + tonemapping.** Teardown does 256×256 → 1×1 luminance downsampling with
  temporal smoothing, then exposure + gamma. v1 has no auto-exposure at all, which is a large part of
  why night and noon need separately tuned constants everywhere (see the `nightK()` blends
  throughout `trace.js` and `memory/rt-daynight-truesun.md`).
- **Keep the day/night blend invariant.** `memory/rt-daynight-truesun.md`: every light term must
  blend on `select(sunDir.y, -sunDir.y, isMoon())` or it jumps at the dusk/dawn moon swap. **No
  `isMoon()` hard-cuts.** Port this rule into v2's shading code as a comment at the top of the file.
- **One bounce of indirect light.** v1 has AO standing in for GI (Teardown does the same — it has no
  GI). With a unified TLAS and ReSTIR machinery in place, a single diffuse bounce per pixel at half
  res is within reach and is the biggest remaining step toward "real". **Costed as a stretch goal, not
  a Phase 1 commitment.**
- **Local lights as area lights** with the reservoir sampler — the fireflies and lava glow become real
  area lights instead of the current analytic glow field.

---

## 9. Build, tooling, and `index2.html`

### 9.1 The second target

`game/index.html` is a build artifact of `tools/bundle.py` from `src/manifest.txt`. To add a second
artifact without disturbing the first:

```
  src2/manifest.txt          v2's fragment order (its own architecture)
  src2/**                    v2 fragments
  tools/bundle.py --target 2 → game/index2.html
  game/assets, game/sound    SHARED — v2 loads the same art and audio
```

Every tool needs a target selector, and **the hooks must build both** or the artifact goes stale
silently — which is exactly the failure `tools/hooks/post-merge` exists to catch:

- `tools/bundle.py --target {1,2}` (default 1)
- `tools/lint-vb.py --target` — all 11 checks, per tree
- `tools/vbtest.py --target` — with its own baseline under `docs/baselines/v2-*.json`
- `tools/vbharness.py --target` — `?v2` or a separate slot
- `tools/serve-nocache.py` — serve both, `/index2.html` builds from `src2/`
- `tools/hooks/pre-commit` — rebuild **both** artifacts, lint **both**
- `tools/where.py --target`

### 9.2 Start with modules

`memory` / `CLAUDE.md` record that source scoping in v1 is **exhausted**: 13 of 78 fragments are
modules, and only ~18 more names could ever be removed, because **28 fragments export a `let` that
something else assigns** — 211 of 334 top-level `let`s are written from a fragment other than the one
declaring them. That shared mutable state is the wall.

v2 is the one chance not to inherit it. **Every v2 fragment is `// @module` from its first line**, and
shared mutable state lives in explicit exported objects (`PH.x`, `VE.lastPaint`) rather than bare
`let`s. This costs a little discipline up front and removes an entire class of merge failure (`const
rad` declared twice in two branches → black screen at merge time) permanently.

The single-file ship constraint stays: `index2.html` is one self-contained file the player
double-clicks. Fragment concatenation is the right build model and it is not changing.

### 9.3 Working rules that carry over unchanged

From `CLAUDE.md` and memory — these are not negotiable and apply to v2 work from day one:

- **No sub-agents on this repo.** Standing owner instruction, 2026-08-19.
- **Never commit or push without being asked.**
- **A push lands as ONE commit** — squash the pile first.
- **Perf work must be perceptually lossless**; lossy levers need explicit sign-off (this plan flags
  two: §1.4 A and B).
- **Every animation runs at 24 fps** unless told otherwise.
- **One Chrome on the box for any timing measurement.**
- `//` comments at end of line, never mid-way into a dense one-liner. No backticks inside WGSL
  comments.

### 9.4 Branch from a committed baseline

`C:/voxelbit` currently has **17 modified files and 3 untracked files uncommitted** (including
`src/render/wgsl/trace.js`, `src/world/window.js` and two new `src/ui/mp4-*.js`). v2 work must not
start on top of an uncommitted tree — the baseline it forks from has to be a commit, or the first
"what changed?" question is unanswerable. Commit or stash v1.2's working tree first.

---

## 10. Phasing

Each phase ends in something runnable and measurable. **Phase 0 exists because three of this plan's
load-bearing assumptions are unproven, and finding out in Phase 3 is expensive.**

### Phase 0 — spikes (prove the assumptions before committing)

Four throwaway experiments, run against the *existing* engine wherever possible:

| spike | question | how |
|---|---|---|
| **0a** | What is v1.2 actually doing at 3840×2160? | `vbharness` + CDP `setDeviceMetricsOverride`, `__vb.profMin()` per pass, open terrain / forest / felling / snow. **This is the baseline every v2 number is measured against.** |
| ~~0b~~ | ~~What does a segmented DDA cost?~~ | **CANCELLED** — curvature is not rendered (§3.5). |
| **0c** | Does the dynamic occupancy volume fix the felling shadow? | Standalone: rasterise the felled body into a 512³ bitmask each frame, query it in the sun ray, drop the `physBound` reactive hack. Judge by screenshot per `memory/voxelbit-ab-screenshot-mad.md` — amplified diffs + `__TFREEZE`, never `mad`. |
| **0d** | What does the sky need to look like? | **Ask the owner for a cloud reference.** No code. |

**Gate:** 0a sets the baseline every later number is measured against, and it decides whether the
forest really needs 3.6× or whether the window the game is actually played in makes that number
smaller. Run it first; nothing else in Phase 0 is on the critical path.

### Phase 1 — the shell (2 weeks)

- `src2/` + `bundle.py --target 2` + every tool's target flag + hooks building both.
- `core/`, `gpu.js`, telemetry, the loading page, `__vb` skeleton.
- **The invariant harvest (§12)** — this is Phase 1 work, not documentation done afterwards.
- Ported verbatim: palette, models, held items, bow, material tabs, audio, achievements, keybinds.
- **End state:** `index2.html` boots to a black canvas with a working console and `__vb`.

### Phase 2 — the unified tracer (3–4 weeks) ← *the spine*

- Instance/Volume representation; shared brick pool for *all* volumes.
- Uniform-grid TLAS, rebuilt per frame.
- One `traceScene()`. G-buffer with instanceId + 6-DOF motion vectors.
- Material tables and the `mat(hit).class` interface (§4.3).
- Flat world only. No sphere, no clouds, no physics.
- **End state:** flat terrain + a few static models + one moving instance, rendering through one path.
  Screenshot-identical to v1 for static terrain (this is the gate — see `voxelbit-ab-screenshot-mad`).

### Phase 3 — lighting and shadows (3 weeks)

- Split sun/sky signals; half-res light pass + bilateral upsample (⚠ sign-off).
- Dynamic occupancy volume; delete `cshad`.
- ReSTIR reservoirs for direct light.
- Reworked denoiser: reservoir-aware temporal, à-trous spatial.
- Physical sky + auto-exposure + tonemap.
- **End state:** a moving object casts an exact, sharp, *current-frame* shadow. This is where the
  headline defect is closed.

### Phase 4 — the planet (3 weeks)

§3.5 removed the hard rendering part; §3.4b put a real content change back in, so the estimate holds.

- Cubed-sphere chart; `terrain.js` ported onto direction-space noise.
- **Whittaker biome field replacing the bands** (§3.4b) — with the blend-not-switch invariant and
  `gtest = 0` across all three copies of `H`. This is the largest single piece of the phase.
- Polar ice caps, as proof the latitude axis is real.
- **f32 world-coordinate audit** (§3.6) — the one real trap at planet scale.
- Lat/long on the HUD, replacing unbounded x/z.
- Far-field heightfield march for the distant silhouette (flat — no curvature).
- View distance 100 m → 300 m, on the brick pool. *Independent of the sphere; can be dropped or
  deferred without affecting anything else in this phase.*
- GPU worldgen (kills the stream-budget frame tail *and* the 11.7 s boot).
- **End state:** the world is finite, has poles and ice caps, and biomes are irregular regions that
  never repeat. The horizon still looks flat, by design.

### Phase 5 — physics off-thread (2–3 weeks)

- Worker + SharedArrayBuffer; persistent component labels; amortised separation.
- Substepped TGS/XPBD solver, islands, sleeping.
- Support system ported with its audit tools.
- **End state:** felling the largest oak costs **zero** main-thread frames.

### Phase 6 — content and life (3 weeks)

- Creatures, nav, fish, birds, mammals, reactions ported onto instances.
- Snow, water, particles, projectiles, tools, vitals.
- Creature simulation moved off the main thread (`life` is 79% of CPU).

### Phase 7 — clouds and finish (2 weeks)

- Volumetric clouds, *against the reference from spike 0d*.
- TAAU (⚠ sign-off), god rays, DOF, post.
- UI/HUD/editor/video-editor ported.
- Settings tiers (low/medium/high/ultra) with the weak-device ladder (`memory/voxelbit-v1-plan.md`:
  the game ships to players' own machines, not to a 4070).

**Rough total: 18–22 weeks.** The three phases that carry real schedule risk are 2, 3 and 4 —
everything after them is porting against a working target.

---

## 11. Standing "do not re-propose" list

These were built, measured, and rejected. They are recorded here so v2 does not pay for them a second
time. **None of them is on this plan.**

**Clouds** (`voxelbit-cloud-attempts`): 2D sky-dome noise sheets; the flat cloud-slab raymarch; 10 cm
voxel clouds. Also: half-rate lighting as a per-pixel checkerboard *or* as wave-aligned 8-wide
columns — reverted twice.

**Graphics terms** (`voxelbit-tier1-discarded`, nine of them): sun haze (HG forward lobe), valley fog,
±1 LSB dither, wet shore, snow sparkle, **cloud shadows**, bloom, sharpen, far AO. Cost was never the
objection; the owner did not want the look. Only the TAA resolve survived.

**Tracer optimisations** (`rt-perf-lossless-frontier`): SDF marching, octahedral encoding, **wavefront
path tracing**, workgroup-shared schemes — all measured, all washed. Tracer cost is distributed; there
is no single lever.

**Water** (`voxelbit-water-is-free`): every WBIT feature is <0.006 ms and waves *off* is 0.7 ms
*slower*. Do not optimise water.

**Structural** (`voxelbit-dynamic-life-render`, `voxelbit-worm-gridstamp`,
`voxelbit-pinecone-sway-reverted`): a dynamic voxel layer was rejected in favour of trace injection;
grid-stamping *moving* creatures causes jitter and was reverted; the pinecone sway was reverted twice
(off-grid = smooth but wrong lighting, grid-stamp = right lighting but chunky).

> That last one is worth reading carefully, because **v2's unified instance model is precisely the
> "both" the owner asked for and neither previous attempt could deliver**: correct grid-quality
> lighting *and* smooth off-grid motion, because the instance is traced with the same rays but is not
> quantised to the world lattice. If v2 delivers one thing, it should be that.

**Also rejected:** coral reefs on the seafloor; the blossom shed (hiding geometry costs the lost
*occlusion*, ~2 ms, not the test); the rock-gradient seamless-vertical variant (the diagonal
top-corner-apex version is the one that stands); leading arrows onto birds.

---

## 12. The invariant harvest (Phase 1, and it is not optional)

The most valuable asset in `src/` is not the code — it is the **comments**. `world/window.js` alone
carries the full history of why the biome bands are 2160 wide, why `BAND_MIRROR` exists, and why
`RAIN_ON` and `OAK_SNOW` are two flags and not one. Retyping the engine from memory loses all of it,
and the bugs come back one at a time over six months.

Before Phase 2 begins, extract from `src/` and from the memory files a single checked-in list of
**hard invariants v2 must satisfy**, each with the symptom that appears when it is violated. A
starting set, from what this survey already turned up:

| invariant | symptom if violated |
|---|---|
| Snow 0.9755 / 0.0245 sync ratio | flakes and accumulation disagree |
| A biome term must go into **all three** copies of `H` | `gtest ≠ 0`; worker and main thread disagree |
| Day/night terms blend on `select(sunDir.y, -sunDir.y, isMoon())` | light jumps at the dusk/dawn moon swap |
| The GW Gerstner table is the single source of truth (shader **and** JS floaters) | floaters ride a different wave than the water |
| Held/decor `.vox` must be tagged `held` (`noTol`) in `DECOR_LOAD` | ramps collapse within `PAL_TOL`; wrong colours |
| Mammal ground seat = `model h/2 − z0`, seated on the **max** ground under the whole footprint | creatures buried or floating |
| Heading alignment auto-derives; bake the `armOffset` parity **once**, in south | per-heading drift |
| A flake's centre is in `roP` space — `floor(ctr − offR)` for `voxAt` | flakes shrink into the ground |
| Pinecone support link must be **overhead and symmetric** | felled cones borrow support sideways; floaters |
| `SUP.cap` must exceed the largest formation (56 k rocks26 > 32768) | undecided seeds never re-queued; floaters |
| Foliage refusal gate on canopy snow | 2 voxels of tree snow instead of 145 |
| Toroidal AABBs must not be min/max over wrapped x/z | a 2048-wide box; the 88 ms snow hitch |
| Pickups key on shared palette ids — bound by model geometry, not a flood | a fallen log picks up as a twig |
| `layout: 'auto'` prunes unread bindings | black canvas at absurd fps, console warning only |

The harvest itself is a half-day of reading with `grep`, and it is the difference between v2 being an
improvement and v2 being a re-run of 2026.

---

## 13. Open decisions for the owner

**Settled 2026-08-21:**

- ~~**Planet radius.**~~ **DECIDED: big enough that curvature is invisible; recommendation R = Earth,
  6371 km** (§3.2). This deleted the segmented DDA, the cross-pass warp, the reprojection change and
  Phase 0 spike 0b — the riskiest quarter of the rewrite.
- ~~**Sub-native rendering.**~~ **DECIDED: acceptable.** The game already runs at renderScale 0.70 and
  the owner is happy with it, so lever A (§1.4) is cleared to build. Note the upgrade is not "render
  smaller" — it is *resolve better*: TAAU at 50% typically beats a plain 70% stretch, because at a 4K
  output 50% still means tracing a full 1920×1080 and 1080p→4K is the best-behaved case temporal
  upscaling has.

- ~~**Play resolution.**~~ **CONFIRMED: fullscreen 3840×2160.** So §1 stands as written: the forest
  case needs **3.6×** and it is the binding constraint on the entire rewrite.
- ~~**Biome layout.**~~ **DECIDED: noise-driven regions** (§3.4b).

**Still open:**

1. **Half-res lighting** (§1.4 B) — and with 4K confirmed, this is no longer optional. The arithmetic:
   levers A × D alone give **1.9–2.9×** against a required 3.6×. **Without lever B the 120 fps target
   is not reachable**, and the fallbacks are all worse — a lower render scale (45% instead of 50%
   still leaves it short), abandoning the 300 m view distance, or cutting ray counts by distance.
   Given that half-rate lighting was built and reverted twice in `rt.html`, this needs an explicit
   yes, and it deserves to be the first thing built in Phase 3 so a "no" surfaces early rather than at
   the end.
2. **The cloud reference** (§7.1). One screenshot ends three failed attempts' worth of guessing.
3. **Baked self-AO in creature art** (§4.3). With real ray AO reaching injected geometry, the baked
   term double-darkens. Removing it is an art change and needs a side-by-side.

---

## 14. One-paragraph summary

v2's spine is a **single scene representation** — every voxel object, from the terrain to a felled
oak to the axe in your hand, is an instance over a volume in one shared brick pool, traced by one
tracer, shaded by one lighting pass, denoised once. That change alone retires the seven-render-path
problem, gives injected objects real material identity, and lets a rotating body reproject correctly
for the first time. On top of it, a **dynamic occupancy volume** — Teardown's actual trick — makes a
moving object's shadow correct in the frame it moves rather than converged over 64, and ReSTIR
reservoirs make it sharp. The **planet** is a cubed-sphere chart over the same flat window that
exists today, at a radius chosen so the curve is invisible — which means the renderer never learns
the world is a sphere at all, and the whole ray-bending design is deleted; what remains is a finite,
closed, pole-having world with no repeating biome period. **Physics** moves to a
worker with persistent connectivity labels, turning a 107 ms fell into zero main-thread frames.
**Clouds** wait for a reference picture. And the whole thing is measured at 3840×2160 from the first
day, because at this scale the 4K budget is the design constraint — not a thing to check at the end.
