# voxelbit v2.0 — engine rewrite plan

**Status:** proposal. Nothing is implemented. Written 2026-08-21 against v1.2 (`b25cde9` + uncommitted
working tree).

**Target artifact:** `game/index2.html`, built from a second source tree. `game/index.html` and `src/`
are not touched until v2 is playable and the owner says to switch.

---

## 0. What this document is

Seven things were asked for, in the owner's order of priority:

1. **A spherical world** — wrap the flat terrain around a planet.
2. **Shadows that keep up with moving objects** — the felling tree is the canonical failure.
3. **One rendering path for everything** — trace-injected objects must render exactly like grid voxels.
4. **Optimisation — as far as it goes, and it stays a top priority** (owner, 2026-08-21). The ask
   is no longer *hit 120 fps*: optimise as hard as possible while staying **perceptually
   lossless**, and let the resolution slider absorb the remainder (§1.1, §1.7). Felling is the
   known worst case, and **big rigid bodies are a first-class requirement, not a sub-case of
   this** — the trees are getting far bigger and body cost scales with them (§6.1, §6.3).
5. **True volumetric clouds.**
6. **The world loads in as fast as possible** — added 2026-08-21.
7. **State-of-the-art lighting and rendering quality** — added 2026-08-21. *"This needs to look
   like a triple-A lighting game."* Not a polish pass at the end: §8 is a pillar, and its GI
   bounce is now a commitment rather than a stretch goal. **The existing look is the floor** — the
   owner rates it good, so this is an upgrade from it, not a replacement of it. Restated more
   strongly on 2026-08-22: *"the lighting in our current engine is fine by me. it looks great
   already."* — paired immediately with *"but also don't be afraid to push for further graphical
   enhancements to push the quality of the graphics even more."* The pair is the point: the first
   half is a **floor** (v2 does not ship worse than v1, and no phase gate depends on §8), the second
   half is an explicit instruction that the floor is **not a ceiling**. So the ambition is unchanged
   and the risk is lower — attempt the state-of-the-art version, judge every rung on a blind
   side-by-side against v1, and fall back without argument when one loses.

The first five are not five independent features. Three of them (2, 3, 4) are the *same defect* seen from three
angles: the engine has seven ways to put a voxel on screen, and six of them are approximations bolted
onto a tracer that was written for one. Fixing the representation fixes all three. That is the spine of
this plan, and the sphere and the clouds hang off it.

Item 6 is a different kind of problem and has its own section: it is not about the frame at all, it
is about the ten seconds before the first one, and the v1 boot is slow *by design* rather than by
neglect — §9. Item 7 is the one that pulls hardest against item 4, and §8.3 does that arithmetic
rather than leaving it to be discovered: state-of-the-art light transport and 120 fps at 4K do not
both fit unless §1.4 B is built.

This document is deliberately specific about numbers, because the hardest constraint in the whole
rewrite is the frame budget and every design choice below has to be paid for out of it.

---

## 1. The frame budget — read this first

### 1.1 The target — a reference point, not a gate

**Owner, 2026-08-21: "don't worry about the fps target. Just optimise the game as much as you possibly
can, and we can adjust the resolution slider to increase fps."** That changes what this section *is*.
The numbers below stay, because they are the measuring stick every claim in this plan is checked
against — but they are no longer a wall the design has to clear.

**The standing instruction is therefore:** optimise as far as it goes **perceptually losslessly**
(`memory/no-quality-sacrifice.md`), take quality wherever it is affordable (§8), and let the player's
resolution slider (§1.6) absorb the difference on whatever hardware they are on.

Which makes one thing considerably more important, and it is §1.7: **the slider only pays for
pixel-bound work.** Everything it cannot pay for moves *up* the priority list, not down it.

The reference numbers, unchanged:

The dev box reports **NVIDIA RTX 4070**, desktop **7680×2160 @ 119 Hz** (two 4K panels), 32 GB RAM.
A fullscreen game window is therefore **3840×2160**, and "120 fps" means **8.33 ms per frame, wall
clock, including present**.

Working split, as a guide rather than a contract:

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
explicit sign-off — see §12.

| lever | expected | notes |
|---|---|---|
| **A. Temporal upscaling (TAAU)** ✅ | **1.8–2.2×** | Render at 50% and resolve to native with jittered accumulation, replacing "render at 70% and bilinearly blit". Biggest 4K lever; the engine already has 80% of the machinery (YCoCg variance clipping + Catmull-Rom history). **Cleared 2026-08-21** — the owner already plays at 70% and is happy with it, and at a 4K output 50% still means tracing a full 1920×1080. |
| **B. Decouple the lighting rate from the visibility rate** ⚠ *(no longer forced — §1.1; judge it on look, not on need)* | **1.4–1.8×** | Sun + AO rays are ~64% of trace. Run them in a **separate, genuinely smaller dispatch** (half-res, bilateral upsample) rather than a per-pixel skip inside the full-res pass. |
| **C. ReSTIR-style reservoir reuse for direct light** | quality, not speed | Keeps 1 ray/pixel but raises the effective sample count 10–20×. This is what lets moving shadows converge in 2–3 frames instead of 64 — see §5. |
| **D. Unified TLAS traversal** | 1.05–1.3× | Replaces `bodyTrace`'s linear body loop, the `visb` tile bitmask and the `cshad` AABB array with one grid walk. Mostly a correctness/consistency win; the speed win shows up when things are moving. |

Multiplied out, A×B×D spans **2.6× to 5.1×**, against the 3.6× the forest case needs. The target is
reachable but it is not comfortable, and it does not survive losing lever B — which is why lever B
appears in §14 as a decision rather than a plan item. It also assumes a fullscreen 4K window, which
the owner **confirmed** on 2026-08-21 (§14, settled) — so the question did not get smaller, and the
3.6× stands as written.

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

### 1.6 The resolution slider ships — it is a player control, not a dev knob

v1 has one and **v2 keeps it, unchanged in feel**:

| | v1 today |
|---|---|
| control | settings panel, `resolution NN%` — `<input type=range min=40 max=100 step=10>` |
| range / default | **40–100% in steps of 10**, default **70%** (`render/targets.js`) |
| keys | `[` and `]` nudge one stop on the **same grid** (`resNudge`, tenths as integers) |
| persistence | `localStorage.vb_scale`, restored at boot |
| applying it | `makeTargets(true)` — every render target reallocates **live**, mid-session, no reload |
| tests | `__vb.res(v)` sets it for an A/B and deliberately **does not persist** — a test must never rewrite the player's `vb_scale` |

Two details worth porting rather than re-deriving. The keys and the slider must share **one grid**: they
stepped 0.125 off a 0.375 floor once, landed on 62.5%, and the slider — which only has 40/50/…/100 — snapped
its knob to a value the renderer was not using. And the 0.375 floor still accepted by `__vb.res()` is
*legacy*: neither shipped control offers it.

This matters more in v2 than it did in v1, for two reasons.

**The game ships to players' own machines** (`memory/voxelbit-v1-plan.md` — pixel streaming was cancelled), so
this slider is the single most effective control a player on a weak GPU can reach. It is the top of Phase 7's
weak-device ladder, not a leftover dev knob.

**Lever A (§1.4) changes what the number means, and the plan has to say so.** With TAAU the slider no longer
picks how blurry the final blit is — it picks the **internal trace resolution feeding the temporal resolve**,
and the output is always native. Therefore:

- The label stays a percentage of native, because that is what it means to a player. Nothing about the UI
  changes.
- Quality at *every* stop should go up: 50% resolved by TAAU is expected to beat today's 70% bilinear
  stretch, which is exactly the §14 "sub-native rendering" decision restated.
- **100% must still mean "trace at native"** — TAAU degenerates to plain TAA at the top of the range rather
  than being switched off. One path, always on; a bypass at 100% is a second render path, which is the thing
  this whole rewrite exists to delete.

Three traps, all of which v2 hits harder than v1 did:

- **Temporal history is resolution-bound.** Changing the slider must reallocate *and* invalidate every
  history buffer — colour, moments, motion, and in v2 the **ReSTIR reservoirs**. v1 mostly gets away with a
  realloc; reservoirs carry many more frames of state and will smear visibly if they survive a resize.
- **The jitter sequence restarts** at the new render size, or the resolve keeps integrating a sample pattern
  for a grid that no longer exists.
- **Anything tuned in pixels has to be expressed as a fraction of the frame** — AO ray counts, the DoF circle
  of confusion, sharpen width, the bilateral upsample radius. A term that reads as "3 pixels" at 70% is a
  different physical size at 40%, and that is how a slider stop ends up looking broken rather than soft.

---

### 1.7 What the resolution slider cannot pay for

The slider is a real lever, and it is why §1.1 can relax — but it scales exactly one class of cost.
Being precise about which is what makes it safe to lean on.

**It pays for pixel-bound GPU work**: trace, composite, the denoisers, and (by canvas pixels) the blit.
That is 63–80% of the GPU frame, so it is a big lever — halving render scale roughly halves it.

**It pays for nothing else.** These are flat in resolution, and a player who drops to 50% still pays
every one of them in full:

| cost | measured | why the slider does not touch it |
|---|---|---|
| **creature simulation** | **79% of per-frame CPU** (1.02 of 1.28 ms) | per-creature CPU work; no pixels involved |
| **terrain streaming** | **36 of 40 logged frame spikes** | the 7/18 ms generation budget — a function of movement, not of pixels |
| **felling a big body** | **83–130 ms**, and §6.1 scales that to 300–700 ms | one main-thread stall, resolution-independent |
| **per-swing `phFlood`** | ~11 ms, scaling to 35–70 ms | same |
| **boot** | ~9.9 s | same |
| **snow / support queues** | saturation-bound | same |

And there is a floor underneath it, already measured: `memory/voxelbit-cpu-bound.md` records that
**whether this engine is CPU- or GPU-bound flips with resolution** — at a 900×600 test window it is
CPU-bound, 3.7 ms frame against 1.5 ms GPU. So dropping the slider far enough stops helping at all, and
what is left is exactly the list above.

**The consequence for this plan, and it is a re-ordering:** resolution-independent work is now the
*higher* priority, because it is the only cost a player cannot escape. In order — physics off-thread
(§6), creature simulation off the main thread (Phase 6), GPU worldgen (§9.7), boot (§9). Shader
micro-optimisation drops below all four, which is convenient, because
`memory/rt-perf-lossless-frontier.md` already records that the tracer has no single lever left anyway.

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
and it must not be retyped from memory — see §13.

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
- **Phase 0 spike 0b is cancelled** (§11), and with it the gate that could have forced a redesign.
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

### 6.1 The constraint that sets the scale: 100+ ft trees

**Owner, 2026-08-21: "we are going to have trees that are 100+ ft tall."** At 10 cm per voxel that is
**305 voxels**, against the tallest tree in the game today at **122**. It is stated first, ahead of the
measurements, because it is not a physics-tuning problem: it resizes the world, and it turns every
number in §6.2 into a different number.

#### It does not fit in today's world

`world/window.js`: `HMAX = min(105 + LIFT, WY − 122)` — and that `− 122` **is** the tallest pine,
reserved as headroom above the terrain ceiling. On a 384-tall world `HMAX` is 233, leaving 151 voxels
of sky. A 305-voxel tree needs `WY ≥ LIFT (128) + relief (105) + 305 ≈ 538`, so **`WY` goes to 576 or
640** — and 120 ft (366 voxels) needs more again.

`WY` is the most expensive dimension in the engine, because it multiplies the entire voxel buffer:

| window | WY 384 | WY 640 |
|---|---|---|
| 768² | 226 MB | 377 MB |
| 1024² | 402 MB | 671 MB |
| 2048² | 1.61 GB | **2.68 GB — past `maxBufferSize` on most adapters** |

So `core/gpu.js`'s weak-device ladder (WXZ 2048/1536/1280/1024/768 × WY 384/256/192) has to be
re-derived around a taller minimum, with the tree height as a term in it rather than a constant hidden
inside `HMAX`. **Decide the final maximum tree height before Phase 2 sizes the brick pool** — §14,
item 5. It is the one number the rest of the memory budget is proportional to.

#### "122" is a four-copy constant, and that is the desert bug again

It appears as a bare literal in four places, each derived independently: `window.js` (the `HMAX`
headroom), `terrain.js` (the brick sky-cap, `maxH + 122`), the `gen-pool.js` worker string (the same
sky-cap, written out separately), and `birds.js` (the flight floor that keeps a dive above the tallest
pine). In v2 it is **one exported constant**. A sky-cap that is one brick short does not throw — it
silently clips the top off every tall tree, and only where something tall happens to be.

#### What it does to the measured felling cost

§6.2's numbers are for an 86,365-voxel oak in a 114×112×114 box. A 305-voxel tree is roughly 3× the box
and, depending on crown shape, 3–6× the voxels:

| | 40 ft oak, measured | 100 ft tree, scaled |
|---|---|---|
| fell swing (`phSeparate`) | 83–130 ms | **~300–700 ms** — a half-second freeze, 36–84 frames at 120 fps |
| every biting swing (`phFlood`) | ~11 ms | **~35–70 ms — a hitch on *every* swing** |
| `gpuPatch` on the fell | ~28 ms (67–86 k cells) | ~100–200 ms (250–500 k cells) |
| dense per-body grid | 1.46 MB | **~4.4 MB per felled body** |

The per-swing row is the one that matters most. A single long stall on the fell is a moment the player
forgives; 35–70 ms on *every bite* is the interaction itself feeling broken.

**These are extrapolations, not measurements** — spike 0f (§11) measures a synthetic 300 k-voxel body
against v1 rather than trusting the scaling.

#### Three consequences, and they promote §6.2 from "planned" to "required"

1. **Off-thread is not optional.** Half a second cannot ship, and no micro-optimisation recovers 5×.
   The 2026-08-20 pass found ~25% and was reverted for changing the feel; this needs 5× and must not
   change the feel at all. Moving the work is the only lever with that shape.
2. **Bodies must not be dense grids.** At ~4.4 MB per body against a 3-entry shape LRU, the dense
   representation is a memory problem before it is a speed problem. The shared-brick-pool body (§4.2)
   stops being an elegance argument and becomes the enabling change.
3. **Every cap in the support system needs re-deriving.** `SUP.cap` is 32,768 and *already* overflowed
   on a 56 k-voxel rocks26 formation (`memory/voxelbit-floater-cap.md`). A 300 k-voxel tree overflows
   it by 10×, and the failure mode is silent floaters, not an error.

#### And it is not only physics

A 305-voxel tree also changes canopy snow accumulation, the drape and support rules for a felled
crown, bird flight floors, the tracer's sky-cap brick striding, and how much empty world above terrain
every ray has to march through. Trees are the tallest thing this engine reasons about, and 122 is baked
into more of it than the four sites above.

### 6.2 Measured cost, from `memory/voxelbit-fell-profile.md`

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

### 6.3 The v2 design

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

### 6.4 The chunking system is ported verbatim

**Owner, 2026-08-21: "keep the chunking physics system."** That is a constraint on §6.3, not a
preference. Everything in §6.3 changes *when* and *where* the work happens; **none of it may change
what the player does with an axe.**

What is covered — every one of these is a tuned number in `sim/physics.js` with history behind it, and
more than one was reverted at least once:

| knob | value | what it decides |
|---|---|---|
| `chopBite` | **30 voxels** | a FIXED count, so every chunk is the same size (user; was 40, cut 25% on 2026-08-02) |
| `absorbSize` | **200** | a chunk over this **refuses** to be carried — break it down first. A felled pine yields ~7, 12 and 139-voxel chunks plus 800+ voxel trunk sections, and 200 is what lets an armful through while making the big sections work |
| `absorbMax` | **2000** | the ceiling on what may become a rigid body at all — a larger separated component is dusted instead |
| `absorbR` / `absorbMs` / `absorbFly` | 16 / 450 / 420 | how far away, and how long after breaking loose, a chunk vacuums to the player (the wait was halved from 900, user) |
| `absorbY` | **−12** | the height it arrives at, relative to the eye. Briefly raised to +1 on 2026-08-02 and **reverted at user request** |
| `crashThrough` | 200 | "this body is a TRUNK, not a chunk" — gates the topple drive and its speed ceiling |
| `chunkLifeMs` / `treeLifeMs` | 600 000 each | everything the player made unstatic is deleted 10 minutes after it broke loose — deliberately equal, two knobs so the trunk can still be retuned apart from the debris |

…and the behaviours wrapped around them: `phChopBody` takes its bite and **can sever a long piece in
two**; a body that is all leaves still gets contacts, or a canopy chunk falls through the world;
chopping an over-size section splits and shrinks it until the parts drop under `absorbSize` and vacuum
up normally.

**The rule for v2.** The solver underneath may change (§6.3 — substepped TGS/XPBD, islands, sleeping,
brick-pool bodies), the thread it runs on changes, and connectivity moves from flood-fill to persistent
labels. **The chunking model, its constants, and the feel of chopping a felled tree do not.** `PH.cfg`
is ported across **as data**, not as a starting point for retuning.

This is §6.2's reverted optimisation pass restated as a design constraint instead of a warning: a
change that alters felling behaviour is a failure regardless of its numbers. And note what §6.1 does
*not* license — 100 ft trees mean far more chunks per tree, which is a **throughput** problem for §6.3
to solve, and explicitly not a reason to raise `chopBite` or `absorbSize` to make the count smaller.

### 6.5 Tools: a sphere of influence, not a hardness scale

**Owner, 2026-08-21: carry the SOI mechanic over — each tool has its own sphere of influence** — and
the reading behind it is right: **this is what lets v2 skip Teardown's hardness scale entirely.**

The two models, plainly:

- **Teardown:** every material carries a scalar hardness, every tool a strength, damage is a function
  of the two. Tuning is a matrix, and every new material has to be positioned against every existing
  tool.
- **voxelbit:** a tool **owns a material class**, and reach and bite scale off that ownership. It is
  categorical, not scalar — and it maps straight onto the material tags v2 already needs for §4.3.

What exists today, in `sim/tools.js`:

| | |
|---|---|
| per-tool sphere | `AXE_SCALE`, `KNIFE_SCALE`, `PICK_SCALE`, `DIG_SCALE` |
| ownership | axe + knife own `woodTab`; the shovel owns `digOnlyTab`; the pick owns `pickOnlyTab` (stone and ore) |
| on its own material | `base × SOI_OWN` — ×2, and ×4 for the shovel |
| off its own material | `base × 0.5` |
| wood | **always** the axe's sphere, whatever is in hand |
| hard gates | the pick breaks stone and nothing else; the shovel moves ground and nothing else — deliberately *not* part of `cut` |
| the bite | `soi()` — the base scale **without** `SOI_OWN`: reach doubles, **bite does not** |

That table is the hardness scale's replacement, and it suits this engine better: material class is
already a tag, so "may this tool work here" is a lookup rather than a threshold comparison, and adding
a material means adding it to a tab instead of choosing a number on a scale that every existing tool is
implicitly tuned against.

#### Carry the measurement over with the mechanic

`memory/voxelbit-soi-is-bite-limited.md`: **raising `SOI_OWN` is inert on ordinary material.** A seeded
A/B on 2026-08-05 — same world, same stand point, shovel 10 → 20 — produced **byte-identical** swings,
voxels removed and max lateral offset, even after 364 carve calls failed to starve the near shells (982
soil voxels still inside radius 10). `phChopDecor` walks shells nearest-first and stops the moment it
holds `mBite` candidates, so a wider sphere changes something only where the material is **scarcer than
one bite** inside the old radius: a patch smaller than a bite, the far lip of a crater.

So SOI is real and worth keeping — it is the **reach into sparse material** knob — but it is not what
differentiates the tools today. Five things actually bind, and v2 keeps all five, named for what they
do:

1. **The ownership tables** — which material a tool owns. This is the hardness scale's real replacement.
2. **The hard gates** — pick-only stone, shovel-only soil. Categorical, not a steep curve.
3. **The bite count** — `PH.chopBite` and the per-tool factor. **If a tool should feel stronger, this
   is the knob**, not the sphere.
4. **The minimum bite** — an axe cuts a *notch*. Dropping the wood minimum to 2 so wood would "have the
   same chunk mechanics as the pick and shovel" was tried and **reverted the same day** (2026-08-20):
   the pick and shovel chew into a solid mass, an axe must not, and taking the first two voxels the ray
   meets makes every swing after the first shave the near fringe of its own hole.
5. **The reach/bite split** — `soiR` and `soi` stay **separate functions**. Doubling one function for
   both would have doubled how much every swing lifts as well, which is exactly the bug the split
   exists to prevent.

### 6.6 The support system

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

## 8. Pillar 6 — state-of-the-art lighting

**Owner, 2026-08-21: "prioritise the lighting and rendering quality. This needs to look like a triple-A
lighting game. State of the art. What we have is good though."** Two constraints live in that sentence
and both matter:

- **The bar is light transport, not effects.** Nine of ten tier-1 graphics features were built,
  measured and rejected on look (§12) — bloom, haze, fog, sparkle, sharpen. That list is the best
  available guide to what "AAA" does *not* mean on this project.
- **v1's look is the floor, not the thing being replaced.** *"What we have is good"* — so this is an
  upgrade path from an already-tuned look, and the felling rule applies verbatim: a change that makes
  the lighting *different* rather than *better* is a failure regardless of how principled it is.

**Owner, 2026-08-22 — two statements, and they must be read together:**

> *"the lighting in our current engine is fine by me. it looks great already."*
>
> *"but also don't be afraid to push for further graphical enhancements to push the quality of the
> graphics even more."*

**The first sets a floor. The second says the floor is not a ceiling.** Either one taken alone
produces the wrong plan: the first alone freezes §8 at "port v1 and stop"; the second alone
re-licenses the nine features §12 already rejected on look. Together they say something specific and
workable — *the look is safe, so go be ambitious with it.*

- **v1's lighting is a shipping state, not a defect.** Nothing in §8 is a fix. If v2 shipped with the
  v1 look ported unchanged, the owner has said that is acceptable — so no phase gate depends on §8,
  and a rung that gets reverted costs build time and nothing else. **That safety is exactly what
  licenses the ambition**: there is a known-good state to fall back to at every step.
- **So push. Timidity is its own failure mode.** The 2026-08-21 ask — *"state of the art", "triple-A"*
  — is not softened by the 08-22 statement; it is de-risked by it. §12's list is a record of features
  rejected on **look**, not a record of a project that regrets attempting them. The cost of building
  a rung that loses its A/B is a few days; the cost of never proposing it is a quality ceiling nobody
  ever measures. **Propose the ambitious version, build it, and let the picture decide.**
- **The bar is "visibly better", judged on a picture, and it cuts both ways.** Build it, A/B it
  against the v1 reference frames (§13) at the same seed, camera and frozen time, and keep it only
  if it wins. A rung that is *arguably* more correct but not visibly better gets reverted rather
  than tuned into acceptability — but "revert" means revert *this implementation*, not close the
  question. Rungs 5 and 8 in particular are ones a weak first attempt will lose and a good second
  attempt will win.
- **§8.2 is a starting list, not a scope limit.** If something not on it makes a bigger visible
  difference, it belongs in the plan. Candidates that fit the "light transport, not post-processing"
  bar and are *not* on §12's rejected list: **water caustics** onto the lakebed and seafloor;
  **subsurface scattering** on leaves and snow (backlit canopy and sunlit snow are two of the
  strongest AAA tells this world happens to contain); **contact-hardening** shadows, where the
  penumbra widens with occluder distance rather than the sun's angle alone; **multi-scatter GGX**
  with specular occlusion; and a **second GI bounce** once the first is affordable. Add to this list
  as the work turns up better ideas.
- **§8.3's budget conflict is unchanged in direction, sharper in method.** The GI bounce stays a
  commitment (owner, 2026-08-21). The 08-22 floor does not cancel it — it means the bounce gets
  bought on evidence rather than on principle: build it, look at it, and if 1.0–1.5 ms buys something
  the owner picks blind, it is cheap. §14 item 5 is where that call gets confirmed.
- **The invariants harden.** §8.5's `select(sunDir.y, -sunDir.y, isMoon())` blend, and v1's hand-tuned
  per-hour constants generally, are now *reference output* — the thing new work is diffed against.
  §13's invariant harvest should capture v1 screenshots at each hour as the comparison set, before
  any of §8.2 is built. **Harvest that set early** — it is what makes an aggressive lighting change
  a cheap experiment instead of a risky one.

### 8.1 Where v1 actually is

| term | v1 today | what state of the art does |
|---|---|---|
| direct sun | one shadow ray, **hard** shadow | soft penumbra from the sun's real 0.53° angular diameter, many-sample via reservoirs |
| sky / ambient | hand-tuned `skyBase` gradient | a physical sky model — the ambient term **is** the sky |
| indirect | **AO standing in for GI** | at least one true diffuse bounce, spatially and temporally resampled |
| local lights | analytic glow field (fireflies, lava) | real emissive area lights, sampled |
| specular | **water only** | GGX with per-material roughness, on everything |
| exposure | **none** — every term hand-tuned per time of day | auto-exposure + filmic tonemap |
| denoise | SVGF-style temporal + à-trous | reservoir-aware temporal + a modern spatial filter |

The pattern is the point: v1 approximates each term separately and then hand-tunes the result until it
looks right. That is why noon and midnight need separate constants throughout `trace.js`, and it is
also the ceiling — **no amount of hand-tuning produces correct interreflection.**

### 8.2 The ladder, ordered by visible effect per millisecond

**Read this list as opt-in, and as a floor rather than a ceiling.** Per the owner's 2026-08-22
statements above, each rung ships only if it wins a blind side-by-side against the v1 look — and the
ordering is by expected value, not by commitment. Attempt them aggressively; the A/B is what makes
that safe, and anything better than a rung on this list should displace it.


1. **Auto-exposure + filmic tonemap** (ACES or AgX). The cheapest item here and the largest single step
   toward "photographed rather than painted". Teardown does 256×256 → 1×1 luminance with temporal
   smoothing. It also deletes a whole class of `nightK()` tuning, because terms stop needing to be
   time-of-day dependent.
2. **Physical sky** (Hosek-Wilkie or a precomputed multi-scatter LUT). The ambient term becomes the sky
   at every hour, with no separate tuning, and it feeds item 3 directly.
3. **ReSTIR direct light.** Already in §5.4 as a *shadow* fix; it is equally a **quality** lever —
   10–20× the effective sample count at one ray per pixel. This is what makes 4 and 7 affordable.
4. **Soft sun shadows from the real angular size.** Nearly free once 3 exists — a cone sample instead
   of a fixed direction — and one of the strongest tells that a scene is rendered rather than painted.
   v1's sun shadows are hard-edged at every distance, which no real sunlight is.
5. **One diffuse bounce of true GI, ReSTIR-resampled. Promoted from stretch goal to a commitment**
   (owner, 2026-08-21). Colour bleeding off a red rock, a green canopy or a snowfield is *the* step
   from "good voxel renderer" to "AAA". AO stops standing in for GI and goes back to being a contact
   term — or is deleted outright, since a real bounce already contains it.
6. **GGX specular with per-material roughness**, on every material rather than water alone. Needs
   §4.3's material tags to carry roughness and reflectance, after which wet stone, ice, sand, bark and
   leaves stop sharing one BRDF. This is where "rendering quality" separates from "lighting quality".
7. **Emissive area lights** through the reservoir sampler — fireflies and lava become real lights that
   spill onto their surroundings instead of an analytic glow painted on top.
8. **A reservoir-aware denoiser.** SVGF was designed for 1-spp path tracing *without* reservoirs;
   carrying reservoir variance into the temporal filter is what stops a GI bounce reading as noise.
   Getting this wrong is the most likely way item 5 ships looking worse than what it replaced.

### 8.3 The budget conflict, stated honestly

§1 says the forest needs **3.6×** to reach 120 fps at 4K *with today's lighting*. This section adds
cost on top of that:

| item | cost at 4K |
|---|---|
| 1–2 (exposure, sky) | ~free — a downsample and a LUT |
| 3 (ReSTIR DI) | already budgeted in §5 |
| 4 (soft shadows) | free — same ray, different direction |
| **5 (GI bounce)** | **1.0–1.5 ms** at half res with resampling — a real fraction of the 6.5 ms GPU budget |
| 6 (GGX + roughness) | cheap in rays, real in shader complexity and register pressure |
| 7–8 | modest |

**And as of 2026-08-21 that conflict is settled by the owner rather than by engineering** (§1.1):
the fps target is a reference point, not a gate, and the resolution slider absorbs the difference.
So the GI bounce is bought at the slider's expense rather than at the lighting's — which is the
right trade, because a temporally-resolved 1440p **with** a bounce beats a native 4K without one.

Two things this does **not** license. It does not license a lossy lever without sign-off — the
standing rule holds (`memory/no-quality-sacrifice.md`), and half-res lighting is now judged on
whether it *looks* right rather than on whether the budget needs it. And it does not license
ignoring §1.7: the GI bounce is affordable because it is pixel-bound, in a way a heavier creature
simulation would not be.

**If the two genuinely conflict, quality wins** — that is the owner's stated priority as of 2026-08-21,
and it is recorded as §14 item 5. The recommended resolution is *not* to drop to 60 fps: hold 120 and
let TAAU carry a 1440p-class internal resolution to 4K, because a temporally-resolved 1440p with a GI
bounce looks unambiguously better than a native 4K without one.

### 8.4 What does not count as AAA here

Worth naming, because "state of the art" invites exactly these and every one has already been rejected
on look (§12): bloom, sun haze / HG forward lobe, valley fog, ±1 LSB dither, wet shore, snow sparkle,
cloud shadows, sharpen, far AO. **The AAA jump in this engine is light transport and exposure, not
post-processing.** Anything on that list needs the owner to re-open it explicitly.

### 8.5 The invariant that survives all of it

`memory/rt-daynight-truesun.md`: every light term must blend on
`select(sunDir.y, -sunDir.y, isMoon())` or it jumps at the dusk/dawn moon swap. **No `isMoon()`
hard-cuts.** This survives the rewrite — put it as a comment at the top of the shading file, because
auto-exposure makes the jump *more* visible, not less.

---

## 9. Cross-cutting: boot time — the loading screen the player never sees

The owner's requirement, added 2026-08-21: **the world loads in as fast as possible.** It is treated
here exactly like the frame budget in §1 — a number, measured, that every design choice is paid out
of. It gets its own section because the v1 boot is not slow by accident or by neglect: it is slow *by
design*, and that design is the one thing in this plan that cannot simply be ported forward.

### 9.1 The target

| | budget |
|---|---|
| **first playable frame** — camera live, spawn bubble traced, player can move | **≤ 1.0 s** |
| full view distance filled *behind* the player | ≤ 4 s, streamed, never blocking a frame |
| cold first-run download | ≤ 25 MB (today ≈ 65 MB — see lever D) |

Stated as a rule: **the loading screen is not a thing to make prettier — it is a thing to delete.** A
warm boot should reach the "press any button" prompt with the world already rendering behind it, and
that is not a trick: the prompt *already* sits over the live game rather than over the overlay
(`ui/input.js` — "after loading, the overlay is removed to reveal the live game").

### 9.2 Where the ten seconds actually go

Measured on the dev box 2026-08-19 and stamped into `core/boot.js`'s own comment; consistent with
`memory/voxelbit-boot-time.md`.

| phase | when | cost |
|---|---|---|
| overlay + wallpaper | first paint | `game/loading.png` is **6.55 MB** — the first thing the player waits for is the loading screen itself |
| assets | → 0.9 s | ~280 requests across 1,007 files; ~300 ms of that is request overhead, not bytes |
| adapter + device | inside that | ~90 ms |
| **world build** | **0.9 s → 8.5 s** | **~77% of boot** |
| occupancy + upload | + ~1.4 s | world upload ~435 ms |
| **total** | | **≈ 9.9 s** — and **11.7 s** measured post-desert on a quiet machine against **7.0 s** pre-desert |

Two things follow, and they set the agenda for this section. **Worldgen is the boot** — everything
else combined is under 2.5 s, so shaving assets or upload cannot reach a 1 s target on its own. And
boot has **already regressed 4–5 s once with nobody noticing for a month**, because no test gates it
(§9.6).

### 9.3 Why it is eight seconds — and why porting it makes v2 *worse*

`world/build.js` opens by stating the decision plainly: *"boot builds EVERYTHING the saved view
distance can reach — no in-game waiting, like the classic full builds"*. It pre-generates the
**entire window** — 512 chunk jobs on the dev box — across `NPOOL = min(16, hc − 4)` workers that
measure **89–98% saturated** for the duration. So:

> boot time = (window volume) ÷ (worker throughput), and the **pool** is the limiter, not the main
> thread.

The window is 768³ to 2048³ — 226 MB to 1.5 GB of voxels — depending on what the adapter binds
(`core/gpu.js`), and `buildWorld` pre-builds all of it. **Boot cost therefore tracks the window, not
what the player can see**, which is the wrong variable to be proportional to. And v2 moves that
variable the wrong way: Phase 4 raises view distance 100 m → 300 m, roughly 9× the area. Pre-building
that is not a ten-second boot, it is a minute.

**The v1 boot structure cannot be ported; it has to be replaced** — and every lever below is about
generating *less*, or generating it *somewhere else*, not about generating faster.

Note also what is **not** still sitting on this path, so it is not re-run: 2.8–4.1× already came out
of it on 2026-08-09 (`memory/voxelbit-boot-time.md`) — the `sweepOrphans` gate (93% of all worker CPU,
for bit-identical cuts), a redundant closing `rebuildBricks` (256 ms → 5 ms for zero changed bits),
and a clamped `setTimeout(…, 2)` doorbell (181 sleeps averaging 4.78 ms for a 2 ms request). And
**more workers is not monotonically better**: `min(20, hc − 4)` once failed to finish inside a 178 s
harness at all. What is left on this path is architecture, not another gate.

### 9.4 The five levers

**A. Generate on the GPU, straight into the brick pool.** ✅ **DECIDED 2026-08-21 — this is
happening; the design is §9.7.** What follows is the argument, kept because the *reasons* constrain
the implementation. Terrain is a pure
function of world coordinates — precisely the precondition a compute shader needs — so the generator
can write bricks where they are consumed. That one move deletes the worker CPU cost, the
`postMessage` transfer, `blitSlab`'s memcpy, the closing occupancy bake *and* the 435 ms upload,
because none of those exist when the voxels are authored on the device. It is the only lever that
attacks the 77% and the 1.4 s tail together.

The catch is the whole spike, and it has to be answered before Phase 2 commits: **the CPU still needs
terrain** — collision, `H()`, spawn, physics, the support flood, and every voxel the player can chop.
A CPU copy is *not* automatically fatal: a bulk read-back of the window is a **bandwidth** cost, not a
generation cost — roughly 0.1–0.5 s for 226–400 MB, against the 7.6 s it replaces. What is fatal is a
**per-query** read-back (a stall per collision test) or a **second CPU implementation of the
generator**, which is the parity trap below. **That is now the settled shape** — GPU generates, CPU
takes a bulk mirror of the near region only, everything beyond physics reach is never mirrored at
all — and §9.7 works it out in full. Spike 0e (§11) still runs, but to *measure and de-risk*, not to
decide.

> **The parity trap, and it is the reason this is a spike and not a plan item.** Today the workers and
> the main thread are bit-exact *by construction* — `gen-pool.js` builds each worker from
> `fn.toString()` of the same functions, so there is only ever one implementation. A WGSL generator is
> a **second implementation in a language with no `f64`**, and this engine has already been bitten by
> exactly that: `gen-worker.js` records that moss rows must stay `Float64Array`, because an f32 round
> flipped the surface material of any column within ~3e-8 of the `mossV > 0.52` threshold and made
> `?nopool` disagree with the pooled path. Every scatter pass in the generator is a threshold like
> that one. **So do not plan on two generators agreeing — plan on one authority.** If the GPU
> generates, the CPU reads back rather than recomputing; `gtest` then compares CPU against a read-back
> of the GPU, not against a re-derivation. See §13.
>
> Second-order, and it only matters because the game ships to players' machines: WGSL float behaviour
> is not identical across vendors (fma contraction, transcendental precision). If the GPU is the
> authority, **the same seed can produce a slightly different world on an AMD card than on an
> NVIDIA one**. Integer-hash noise and avoiding `sin`/`pow` in the height path make this tractable;
> ignoring it makes seed-sharing and screenshots quietly unreliable.

**B. Boot the bubble; stream the rest.** The first frame only needs what its rays can hit: a spawn
bubble, not a 300 m window. Everything beyond it should arrive on the existing demand-driven path —
`world/stream.js` already grows the rect band-by-band toward where the player is heading, at a frame
budget, and already keeps pace with sprint speed. Boot's job shrinks to "fill the bubble, hand over".

With A decided, B is no longer the cheap alternative to it — it is what keeps boot **flat as view
distance grows**. A fast generator still generates 9× the volume at 300 m; not generating it until
it is needed is what makes that free. Both ship.

The honest objection is pop-in, and Phase 4 already carries the answer: the **far-field heightfield
march** draws the distant silhouette from `H` directly. The far field never was voxels, so it can be
*rendered* before it is *generated* — the horizon is there in frame one and voxelises quietly as the
player walks into it. That is not a fake horizon; it is the same height function the generator uses.

**C. Overlap, don't sequence.** Today boot is a list: assets → world → occupancy → upload → first
frame. Nothing about `requestAdapter` (~90 ms), shader compilation, pipeline creation or the asset
fetches depends on worldgen, and vice versa. In v2 boot is a **dependency graph**: the device request
and the asset fetches start on line one, pipelines compile while the generator runs, and bricks upload
as they land rather than in a closing pass. Worth most of the non-worldgen 2.5 s, and it costs nothing
but ordering.

**D. Ship fewer bytes** — because for the player, download *is* load time. v1.0 ships as a local
browser game on the player's own device (`memory/voxelbit-v1-plan.md`), so the first run pays for
every byte: **≈ 65 MB** today — sound 32 MB, assets 23 MB, `loading.png` 6.55 MB, `index.html`
2.9 MB. Named items:

- `game/loading.png` — **6.55 MB**, and it is the wallpaper of the screen it is delaying. At `cover`
  scale a ~200 KB WebP is indistinguishable.
- `assets/decoration/desert_rocks.json` — **5.53 MB** and **never stamped** (`stampDrock` uses the
  grey `ROCK26` set); it only downloads under `?rocks`, so it is dead weight *on disk*, not boot
  time. Deleting it shifts every palette id assigned after it, so it needs `gtest` + a screenshot
  pass, never an `rm`.
- `rocks26.json` **3.23 MB** and `oak_trees.json` **1.97 MB** are real content, but they are *JSON*.
  A packed binary is a fraction of the size and skips the parse.
- 1,007 asset files and ~280 boot requests → **one packed bundle**, fetched once and sliced in
  memory. And a **manifest**, never blind frame probes: the desert prefetch guessed 20 frames per
  species and collected **82 wasted round-trips every boot** until `desert_frames.json` existed.

Measured null — do not re-run it: the old 73 MB `loading.mp4` A/B'd to **zero** boot cost (the number
moved in both directions). Download size and boot time are different problems and each needs its own
measurement.

**E. Cache the built world — but only if the spawn stops being random.** Generation is deterministic,
so a built window is reproducible and therefore cacheable: version-stamped compressed bricks in
OPFS/IndexedDB turn a repeat boot into a disk read, which beats every other lever here combined.
Today it is impossible, and the reason is one line in `world/build.js`: `SPWX`/`SPWZ` are re-rolled to
a random ±20 km point on **every refresh**, so no two boots share a world. Trading that for a fixed or
last-visited spawn is an owner decision, not an engineering one — §14, item 4.

### 9.5 The loading screen itself

- **Keep real progress.** The bar is driven by finished slabs now (`setLoad(22 + done/total * 60)`
  out of `world/build.js`) after a long life as a pure CSS trickle that parked at 90% and read —
  correctly — as stuck. Do not resurrect a time-based fake in v2. If the bar is honest and the boot
  is 1 s, it barely appears, which is the point.
- **Port the polish, not the machinery.** The version drum, the gloss and the `%` readout mirror the
  compositor's live transform and are already correct; they are ~60 lines and carry three fixed bugs
  in their own comments (`transitionend` does not fire reliably; the `loadFinishing` guard;
  both digits roll together or neither does). Copy them verbatim.
- **End state:** the overlay is a *cold-download* screen, not a worldgen screen. On a warm boot the
  player sees the world, with the play prompt over it, while the horizon is still filling in.

### 9.6 How boot is measured — and why it must be gated

The recipe that produced every number above, from `memory/voxelbit-boot-time.md`:

- `window.__bootT` phase stamps at each `setLoad` / `stage`, plus per-phase `performance.now()`
  *inside* the workers, shipped back on the result message.
- `Math.random` pinned via `Page.addScriptToEvaluateOnNewDocument`, so two builds see the same world.
- **One Chrome on the box, on a quiet machine.** Concurrent agents make boot timings junk — the
  standing rule from `memory/perf-run-one-chrome.md`, and boot is the measurement most sensitive to
  it.

And the part that was missing in v1: **boot time is a gate number.** `tools/vbtest.py --target 2`
records first-playable-frame ms against a baseline in `docs/baselines/`, and a regression fails the
gate the way `gtest ≠ 0` does. The desert biome cost 4–5 s of boot; it took a month and a memory note
to notice. With a gate it would have been one commit.

---

### 9.7 GPU worldgen — the design

**DECIDED by the owner, 2026-08-21: worldgen moves to the GPU.** It is no longer a lever weighed
against alternatives or a spike that could come back "no". What follows is the design; §9.4 A is the
argument for it and §11 spike 0e is now a *design* spike — it measures and de-risks, it does not
decide.

#### What moves, and what does not

| pass | v2 home | why |
|---|---|---|
| height field + column fill (`makeHRow`, `fillColumn`, `H`) | **GPU**, one invocation per column | pure function of world coordinates, no cross-column dependency. The majority of the cost. |
| gorge carve (`caveAt` / `stampCave`) | **GPU** | already an AABB-vs-segment test after the 2026-08-09 `caveHitsBox` fix — per-column and independent. |
| decor scatter + stamps (~20 passes: trees, oaks, boulders, cacti, shrubs, ore, ferns, mushrooms, flowers, cones, sticks, logs, lilies, hives) | **GPU**, one dispatch per pass | the tables (`OAKV`, `ROCK26`, …) upload once as storage buffers. **One dispatch per pass, not one fused kernel** — the passes overwrite each other in a fixed order today, and that order is the behaviour. |
| 8³ brick occupancy scan | **GPU** | a reduction over the volume it has just written; it is already done per-slab in the worker for the same reason. |
| `sweepOrphans` (26-connected flood) | **stays CPU, and leaves the boot path** | flood fill is the genuinely GPU-hostile piece. Gen orphans become seeds for the existing support queue instead of a synchronous sweep — the queue was built to resolve exactly this, and the sweep is what made boot 2.8–4.1× slower before it was gated. |
| chop / fell / snow / every `gpuPatch` | **unchanged, CPU** | the world is *generated* on the GPU and *modified* on the CPU. No new synchronisation appears. |

#### One authority: the GPU generates, the CPU reads back

This is the load-bearing decision and it follows directly from the parity trap in §9.4 A. **The CPU
never re-derives terrain.** It reads:

- **`hmap` always** — `WX × WZ` Int16, 1–2 MB. Spawn, nav, life placement, `bfBed`/`bfGlide` and the
  far-field march want *heights*, not voxels, and this covers all of them.
- **The near region as voxels** — the box physics, collision and chopping can actually reach.
  Everything beyond it is never mirrored: the tracer reads it from the pool, and nothing on the CPU
  asks.
- **In bulk, asynchronously** — one `mapAsync` per generated region. **Never per-query.** A read-back
  inside a collision test is a stall per test, and that is the failure mode this design exists to
  avoid.

Bandwidth, not generation: 226–400 MB is roughly 0.1–0.5 s of read-back against the 7.6 s it replaces,
and the near-region-only rule cuts even that to a fraction.

#### The worker pool stays, as the oracle

`gen-pool.js` is **not deleted**. It becomes the reference implementation and the A/B oracle, exactly
as `?nopool` is for the pooled path today; `?cpugen` forces it. That buys three things: a fallback when
an adapter's limits or a driver bug bite, the only credible way to test the GPU generator at all, and a
shipping path for a weak device whose GPU is the bottleneck rather than its CPU.

#### How parity is tested — and the tolerance, stated up front

`gtest` as it stands diffs the main thread against the worker: two *JS* implementations that are
bit-exact by construction. The new oracle diffs a **GPU read-back against the CPU worker** for the same
region, and it **will not be bit-exact** — WGSL has no `f64` (§9.4 A). So the test states a budget
instead of pretending:

| signal | budget |
|---|---|
| `hmap` | **exact.** Heights are integers; a difference means the generator differs, and there is no tolerance to hide in. |
| material ids | **< 1 in 10⁵ columns**, and every difference must be a threshold straddled within an epsilon of its cut — never a structural difference. |
| decor placement | **exact.** Cells are chosen by integer hash, so this tests whether the hash was ported correctly, not float behaviour. |
| known-seed `hmap` hash | pinned in the gate, so a **driver update** that shifts float behaviour fails a test instead of arriving as "the world moved". |

Anything outside those is a port bug, not float noise, and is treated as one.

#### Staging — three stages, each shippable, each with the pool as fallback

| stage | moves | proves |
|---|---|---|
| **1** | height field + column fill + brick scan. Decor is still stamped by the workers, from the read-back. | the read-back path, the `hmap` oracle, and the boot number — the majority of the win, at the smallest surface for a parity bug |
| **2** | decor scatter + stamps, pass order preserved | tables-as-buffers, and the placement oracle |
| **3** | gorge carve; gen orphans become support-queue seeds | that the flood leaves the boot path entirely |

#### Cross-vendor determinism

The one risk here that is not performance. With the GPU as authority, **the same seed can generate a
subtly different world on a different card** — fma contraction and transcendental precision are not
specified across vendors. Keep the height path on integer-hash noise, avoid `sin` / `pow` where a
polynomial will do, and keep the known-seed `hmap` hash in the gate. This matters because the game
ships to players' own machines (`memory/voxelbit-v1-plan.md`), where seed-sharing and screenshots are
expected to mean something.

---

## 10. Build, tooling, and `index2.html`

### 10.1 The second target

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

### 10.2 Start with modules

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

### 10.3 Working rules that carry over unchanged

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

### 10.4 Branch from a committed baseline

`C:/voxelbit` currently has **17 modified files and 3 untracked files uncommitted** (including
`src/render/wgsl/trace.js`, `src/world/window.js` and two new `src/ui/mp4-*.js`). v2 work must not
start on top of an uncommitted tree — the baseline it forks from has to be a commit, or the first
"what changed?" question is unanswerable. Commit or stash v1.2's working tree first.

---

## 11. Phasing — a dependency order, not a delivery schedule

**Owner, 2026-08-21: "complete every phase of the engine all at once, in one go."** So the phases below
are **not** seven deliveries with seven stopping points. The engine is written in a single continuous
pass, and what the phase list becomes is the **order things are built and first brought up** — which
still matters, for one reason that has nothing to do with scheduling.

#### Why the order survives even when the schedule does not

Every failure mode this engine has is **silent**. Not "throws an error" — silent:

- a duplicate top-level `const` is a black screen (`memory/rt-blackscreen-dup-const.md`)
- a `//` comment mid-way into a dense one-liner eats the rest of the line — braces, or assignments
  that simply never run (`rt-inline-comment-braces`)
- a `ReferenceError` in `tickBody` freezes the simulation behind a **perfectly rendered frame**; lint,
  `vbtest` and fps all report healthy (`voxelbit-tick-throw-silent`)
- `layout: 'auto'` prunes a binding the shader never reads and you get a black canvas at absurd fps,
  with a console warning as the only evidence (`webgpu-auto-layout-prunes-bindings`)
- a const read before its declaration inside an earlier shader template literal hangs the boot on
  "uploading world", and `syntax.py` cannot see it (`voxelbit-blackscreen-const-order`)

Write 30,000 lines, boot once, and get a black screen: the failure is somewhere in 30,000 lines, and
none of the tooling will point at it. Bring the same 30,000 lines up **in dependency order** and the
first black screen names its own cause, because only one subsystem is new since the last frame that
worked. **This costs nothing** — it is the boot order, not a delay.

The other half of the order is that **the gates are oracles, not milestones**. "Screenshot-identical to
v1 for static terrain" (Phase 2) is only a meaningful test while terrain is the only thing on screen.
Once creatures, weather, water and clouds are all present, a wrong AO falloff, a wrong sky and a wrong
denoiser are indistinguishable from each other — and `memory/voxelbit-ab-screenshot-mad.md` already
records that the scene's own noise beats most shader terms in a diff.

#### So, in practice

- **Write everything.** One pass, no stopping points, no "we'll do clouds later" scaffolding — the
  intermediate seams Phase 2 would otherwise need (filling the pool from the CPU path purely to replace
  it in Phase 4) are exactly the throwaway work this instruction removes.
- **Boot it in the order below**, and keep the checkpoint after each: it is a few minutes each and it
  is the difference between a named bug and a bisect over the whole engine.
- **Run the cheap checks continuously, not at the end** — `lint-vb.py` (all 11 checks, including the
  duplicate-name and registry checks), `__vb.errLog()`, `gtest`. Every one of the silent failures above
  is caught by something that costs seconds.
- **Phase 0's spikes stay**, but they run *inside* the build rather than before it: 0a's 4K baseline is
  needed to judge any of this, and 0e/0f are measurements that size decisions already made.

#### The three things that genuinely must come first

These block code, not schedule, and guessing wrong means rewriting rather than re-tuning:

1. **Maximum tree height** (§14 item 4) — sets `WY`, which sizes the brick pool and the whole voxel
   budget. Pick it before a buffer is allocated.
2. **Half-res lighting, yes or no** (§14 item 1) — it is a different lighting architecture, not a flag.
3. **The cloud reference** (§7.1) — three attempts have already failed on guessing.

If answers are not available, the build proceeds on stated defaults — tallest tree 320 voxels, half-res
lighting **built** (it is the only path to 4K), clouds deferred to last so the reference can arrive
late — and each is written down where it is assumed, so it can be revisited without archaeology.

---

### Phase 0 — spikes (prove the assumptions before committing)

Four throwaway experiments, run against the *existing* engine wherever possible:

| spike | question | how |
|---|---|---|
| **0a** | What is v1.2 actually doing at 3840×2160? | `vbharness` + CDP `setDeviceMetricsOverride`, `__vb.profMin()` per pass, open terrain / forest / felling / snow. **This is the baseline every v2 number is measured against.** |
| ~~0b~~ | ~~What does a segmented DDA cost?~~ | **CANCELLED** — curvature is not rendered (§3.5). |
| **0c** | Does the dynamic occupancy volume fix the felling shadow? | Standalone: rasterise the felled body into a 512³ bitmask each frame, query it in the sun ray, drop the `physBound` reactive hack. Judge by screenshot per `memory/voxelbit-ab-screenshot-mad.md` — amplified diffs + `__TFREEZE`, never `mad`. |
| **0d** | What does the sky need to look like? | **Ask the owner for a cloud reference.** No code. |
| **0f** | What does a 100 ft tree actually cost to fell? | Build a synthetic ~300 k-voxel body in v1 (scale an oak model, or fuse several) and run `__vb.physChopFull` on it. Read `lastFloodMs` / `lastSepMs`. **Replaces §6.1's extrapolation with a number**, and tells you whether the per-swing hitch or the fell stall is the real problem. Cheap — it needs no v2 code. |
| **0e** | GPU worldgen is **decided** (§9.7) — so: what does it cost, and how far does the read-back mirror have to reach? | Port `H` to a compute shader for one 8-aligned slab. Measure: generation time vs the worker pool, `mapAsync` read-back time for the window and for a near-region box, and the `hmap`/material divergence against the worker (§9.7's budgets). **Output is numbers that size the staging, not a go/no-go.** |

**Gate:** 0a sets the baseline every later number is measured against, and it decides whether the
forest really needs 3.6× or whether the window the game is actually played in makes that number
smaller. Run it first. **0e no longer gates a decision** — GPU worldgen is settled (§9.7) — but it
still runs early, because how far the read-back mirror has to reach changes what Phase 2's pool
interface must support, and its numbers are what size the three stages.

### Phase 1 — the shell

- `src2/` + `bundle.py --target 2` + every tool's target flag + hooks building both.
- `core/`, `gpu.js`, telemetry, the loading page, `__vb` skeleton.
- **The invariant harvest (§13)** — this is Phase 1 work, not documentation done afterwards.
- **`__bootT` phase stamps and a boot number in the test gate, from the first commit** (§9.6). v1 lost
  4–5 s of boot to the desert biome and nobody noticed for a month; v2 does not get to repeat that.
- Ported verbatim: palette, models, held items, bow, material tabs, audio, achievements, keybinds.
- **End state:** `index2.html` boots to a black canvas with a working console and `__vb`.

### Phase 2 — the unified tracer ← *the spine, and the long pole*

- Instance/Volume representation; shared brick pool for *all* volumes.
- Uniform-grid TLAS, rebuilt per frame.
- One `traceScene()`. G-buffer with instanceId + 6-DOF motion vectors.
- Material tables and the `mat(hit).class` interface (§4.3).
- Bricks upload **as they are produced** — no closing full-world occupancy bake, no single upload
  pass at the end (§9.4 C).
- The brick pool takes fills from **either producer** — CPU worker or GPU generator (§9.7). Phase 2
  fills it from the ported worker pool; that path stays afterwards as the oracle, not as scaffolding.
- **`WY` sized for the tallest tree first** (§6.1). World height multiplies the whole voxel budget and
  re-derives the weak-device ladder, so the tree height has to be settled before the pool is sized —
  not discovered in Phase 6 when the tall trees arrive.
- Flat world only. No sphere, no clouds, no physics.
- **End state:** flat terrain + a few static models + one moving instance, rendering through one path.
  Screenshot-identical to v1 for static terrain (this is the gate — see `voxelbit-ab-screenshot-mad`).

### Phase 3 — lighting and shadows

- Split sun/sky signals; half-res light pass + bilateral upsample (⚠ sign-off).
- Dynamic occupancy volume; delete `cshad`.
- ReSTIR reservoirs for direct light.
- Reworked denoiser: reservoir-aware temporal, à-trous spatial.
- Physical sky + auto-exposure + tonemap.
- **End state:** a moving object casts an exact, sharp, *current-frame* shadow. This is where the
  headline defect is closed.
- **Gate:** the shadow work above is the defect fix and it ships. The *look* items — sky, exposure,
  soft sun, GI, GGX — are opt-in per §8.2 and each needs a blind A/B against the harvested v1
  reference frames (§13) before it stays; a rung that does not visibly beat v1 gets reverted rather
  than tuned. Owner, 2026-08-22, and both halves apply here: the current lighting already looks
  great **and** *"don't be afraid to push for further graphical enhancements"* — so build the
  ambitious version of each rung, and treat the fallback as cheap insurance rather than as the
  expected outcome. If Phase 3 finishes early, spend the slack on §8.2's stretch candidates
  (caustics, leaf/snow subsurface, contact-hardening) rather than banking it.

### Phase 4 — the planet

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
- **GPU worldgen** (§9.7) — kills the stream-budget frame tail *and* the 11.7 s boot. **Decided, not
  conditional.** Its only real dependency is Phase 2's shared brick pool, not the sphere, so stage 1
  can land as soon as that pool exists — and the earlier it does, the longer the CPU worker path is
  around to be diffed against.
- **Boot the spawn bubble, stream the rest** (§9.4 B), with the far-field heightfield march covering
  the horizon until it voxelises. This is what makes a 300 m view distance affordable at boot at all.
- **End state:** the world is finite, has poles and ice caps, and biomes are irregular regions that
  never repeat. The horizon still looks flat, by design.

### Phase 5 — physics off-thread

- Worker + SharedArrayBuffer; persistent component labels; amortised separation.
- Substepped TGS/XPBD solver, islands, sleeping.
- Support system ported with its audit tools.
- **End state:** felling the largest oak costs **zero** main-thread frames — **with the chunking
  system behaving identically to v1** (§6.4). A felling behaviour check is the gate on this phase,
  not the frame timings.

### Phase 6 — content and life

- Creatures, nav, fish, birds, mammals, reactions ported onto instances.
- Snow, water, particles, projectiles, tools, vitals.
- Creature simulation moved off the main thread (`life` is 79% of CPU).

### Phase 7 — clouds and finish

- Volumetric clouds, *against the reference from spike 0d*.
- TAAU (⚠ sign-off), god rays, DOF, post.
- UI/HUD/editor/video-editor ported.
- Settings tiers (low/medium/high/ultra) with the weak-device ladder (`memory/voxelbit-v1-plan.md`:
  the game ships to players' own machines, not to a 4070).
- **The resolution slider** (§1.6), ported with its range, its `[` / `]` keys, its `vb_scale`
  persistence and its live `makeTargets` reallocation — now driving TAAU's internal resolution, with
  history invalidation on every change.

**Rough total: 3–6 weeks of build.** *(Revised 2026-08-21. The first draft of this plan said 18–22
weeks; that was a human-team estimate and it does not describe this project.* **Measured from the repo:
`voxelbit.html: new ground-up Teardown-style raytraced engine (v0.9)` lands 2026-07-14, and the next
day's commit already carries the LOD horizon, the water overhaul, 3D held items, 100 m views and
worker-pool worldgen. From that commit to v1.2 is 38 days, 162 commits and 30,265 lines — including all
the content, the creatures, the physics and the weather. The rewrite reuses the design, the art, the
tools and the invariants, and adds harder rendering; it does not plausibly take four times as long as
building the whole thing did.)*

**What actually sets the schedule is not typing.** Every phase estimate above is dominated by
verification, not implementation:

- **A/B measurement is wall-clock.** Every perf claim needs a 4K measurement with one Chrome on a quiet
  box (§1.5, `memory/perf-run-one-chrome.md`). Those runs are serial and cannot be parallelised away.
- **Taste iteration is unbounded and cannot be scheduled.** Nine of ten tier-1 graphics features were
  built, measured and rejected on look, not cost. Phase 3's "does the lighting resemble real life" and
  Phase 7's clouds are the same shape — and clouds already have three failed attempts behind them
  (§7.1), which is why that phase waits on a reference picture rather than an estimate.
- **Behaviour verification is play time.** Phase 5 gates on a felling behaviour check (§6.4), and the
  2026-08-20 optimisation pass proves why: four changes, ~25% faster, reverted wholesale because the
  feel changed, and which one did it was never established.
- **Silent failures cost hours, not minutes.** A duplicate top-level `const` is a black screen; a
  `ReferenceError` in `tickBody` freezes the sim behind a perfectly rendered frame; `layout: 'auto'`
  prunes an unread binding and gives you a black canvas at absurd fps with only a console warning.
  Each of these has happened, and none is a typing-speed problem.

So read the phases as **ordering and dependency, not as a calendar**. Phase 2 gates everything;
Phase 3's ReSTIR and Phase 7's clouds are the two that can overrun without warning; the port phases (5
and 6) are the fastest, because the design is already known and the destination is a working target to
diff against.

The three phases that carry real schedule risk are 2, 3 and 7 — everything after Phase 2 is porting
against a working target, and the two open-ended items are lighting quality and clouds, both of which
are judged by eye rather than by a number.

---

## 12. Standing "do not re-propose" list

These were built, measured, and rejected. They are recorded here so v2 does not pay for them a second
time. **None of them is on this plan.**

*Note on the 2026-08-22 "push further" directive (§8):* it raises the ambition for **light transport**
and does **not** reopen this list. Every item here lost on look after being built, which is a stronger
verdict than never having been tried. Anything below needs the owner to re-open it by name.

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

## 13. The invariant harvest (Phase 1, and it is not optional)

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
| Tool reach (`soiR`) and tool bite (`soi`) stay **separate functions** (§6.5) | doubling reach silently doubles how much every swing lifts |
| `PH.cfg` is ported as **data** — `chopBite` 30, `absorbSize` 200, `absorbY` −12 and the rest unchanged (§6.4) | chopping feels different; the 2026-08-20 felling revert repeats |
| The tallest-model height is **one** exported constant, not a literal in four files (§6.1) | a sky-cap one brick short silently clips the top off every tall tree |
| **One** generator authority — the CPU reads back rather than re-deriving (§9.4 A) | two implementations drift on an f32 threshold; the player falls through terrain the renderer is drawing |
| A `renderScale` change reallocates targets **and** invalidates temporal history + reservoirs (§1.6) | ghosting/smear for seconds after moving the slider |
| The slider and the `[` / `]` keys step the **same** grid (§1.6) | the knob shows a value the renderer is not using |

**Harvest the look, too, not just the invariants.** Owner, 2026-08-22: *"the lighting in our current
engine is fine by me. it looks great already."* That makes the v1 image itself an artifact worth
checking in. Before any of §8.2 is built, capture a fixed reference set from v1 — the same seed,
camera and `__TFREEZE` time at each hour of the day cycle, plus the awkward cases (a snowfield at
noon, canopy shade, underwater, moonlit, the dusk/dawn moon swap) — and store it next to the
invariant list. Every lighting change in v2 is then judged against a picture rather than against a
memory of one. That cuts both ways and the second way is the more important one: it is what makes
§8.2's "revert, don't tune" rule enforceable, *and* it is what makes an aggressive lighting
experiment cheap — with a reference set on disk, trying the state-of-the-art version of a term costs
a build and a diff instead of costing the look.
Reuse the A/B discipline from `memory/voxelbit-ab-screenshot-mad.md`: freeze time, amplify the diff,
and pick a metric that matches the claim — scene noise beats a raw mean-absolute-difference every
time.

The harvest itself is a half-day of reading with `grep`, and it is the difference between v2 being an
improvement and v2 being a re-run of 2026.

---

## 14. Open decisions for the owner

**Settled 2026-08-21:**

- ~~**Planet radius.**~~ **DECIDED: big enough that curvature is invisible; recommendation R = Earth,
  6371 km** (§3.2). This deleted the segmented DDA, the cross-pass warp, the reprojection change and
  Phase 0 spike 0b — the riskiest quarter of the rewrite.
- ~~**Sub-native rendering.**~~ **DECIDED: acceptable.** The game already runs at renderScale 0.70 and
  the owner is happy with it, so lever A (§1.4) is cleared to build. Note the upgrade is not "render
  smaller" — it is *resolve better*: TAAU at 50% typically beats a plain 70% stretch, because at a 4K
  output 50% still means tracing a full 1920×1080 and 1080p→4K is the best-behaved case temporal
  upscaling has. **The slider itself ships either way** — range, keys, persistence and live realloc —
  see §1.6; what changed is that the number now feeds the upscaler instead of a bilinear blit.

- ~~**Play resolution.**~~ **CONFIRMED: fullscreen 3840×2160.** So §1 stands as written: the forest
  case needs **3.6×** and it is the binding constraint on the entire rewrite.
- ~~**Biome layout.**~~ **DECIDED: noise-driven regions** (§3.4b).
- ~~**GPU worldgen.**~~ **DECIDED 2026-08-21: worldgen moves to the GPU** — design in §9.7. The GPU
  becomes the single generation authority and the CPU reads back rather than re-deriving; the worker
  pool is kept as the oracle and the fallback. Spike 0e is demoted from gate to measurement.

**Still open:**

1. **Half-res lighting** (§1.4 B) — **demoted 2026-08-21, and it may now be a "no".** It was
   load-bearing only while 120 fps at 4K was a gate; with the fps target relaxed to "optimise as far
   as it goes, the slider absorbs the rest" (§1.1), lever B is just an optimisation with a look risk
   — and it was built and reverted **twice** in `rt.html`. The standing rule
   (`memory/no-quality-sacrifice.md`) says a lossy lever needs explicit sign-off, and the reason to
   grant it has now largely evaporated. **Recommendation: build it last, judge it purely on look, and
   drop it without argument if it costs anything visible.** The arithmetic it used to rest on, kept
   for reference: levers A × D alone give 1.9–2.9× against the forest's measured 3.6× gap.
2. **The cloud reference** (§7.1). One screenshot ends three failed attempts' worth of guessing.
3. **Baked self-AO in creature art** (§4.3). With real ray AO reaching injected geometry, the baked
   term double-darkens. Removing it is an art change and needs a side-by-side.
4. **How tall, exactly, are the tall trees?** (§6.1). "100+ ft" is 305 voxels; 120 ft is 366. The
   answer sets `WY`, which multiplies the entire voxel buffer, re-derives the weak-device ladder in
   `core/gpu.js`, and sizes Phase 2's brick pool. It is also the difference between a 2048² window
   fitting in `maxBufferSize` and not. **This one blocks Phase 2**, so it is the first of these that
   needs an answer.
5. **If lighting quality and 120 fps genuinely collide, which gives?** (§8.3). Stated priority as
   of 2026-08-21 is **quality** — but the recommendation is that this is a false choice: hold 120
   fps and let TAAU carry a 1440p-class internal resolution to 4K, rather than dropping the frame
   rate. A temporally-resolved 1440p *with* a GI bounce beats a native 4K without one. Confirm, so
   that the answer is on record before the first millisecond is spent defending it.
   **Reframed 2026-08-22** by the paired statements *"the lighting in our current engine is fine by
   me. it looks great already"* and *"don't be afraid to push for further graphical enhancements to
   push the quality of the graphics even more."* The collision is now between an *upgrade* and the
   frame rate rather than between a *fix* and the frame rate — but the upgrade is explicitly wanted,
   so this is not a licence to skip it. The concrete question is narrower and empirical: **is the GI
   bounce worth 1.0–1.5 ms against a baseline you are already happy with?** Answer it with a picture,
   not in the abstract — build the bounce in Phase 3, A/B it blind, keep it if it wins, take the
   milliseconds back if it is a shrug. Same procedure for anything §8.2 grows past its current
   eight rungs.
6. **Does v2 get a networking pillar, or is v2 declared Sandbox-only?** — added 2026-08-22 from
   [arcade.md](arcade.md) §7. Every genre named for the Voxelbit Arcade (CoD-style, Battlefield-style,
   Fortnite-style, battle royale) is **server-authoritative multiplayer**, and this plan contains no
   networking of any kind: the 12-pass frame graph, the worker physics over a `SharedArrayBuffer` and
   the GPU-generates/CPU-reads-back authority all assume one local simulation. Either a networking
   pillar goes in now — it changes physics and simulation decisions *before* they are built — or it is
   recorded here that v2 is the Sandbox engine and the Arcade rides on a later one. **Deciding neither
   is the bad outcome**, because Phase 2 and Phase 5 will otherwise answer it by accident. Note the
   specific hard part: replicating destructible voxel terrain, i.e. exactly the felling case §6 is
   built around.

7. **Random spawn, or a cacheable one?** (§9.4 E). `world/build.js` re-rolls `SPWX`/`SPWZ` to a random
   ±20 km point on every refresh, which is a deliberate feature — and it is also the single reason the
   generated world can never be cached between sessions. A fixed or last-visited spawn turns a repeat
   boot into a disk read, which is worth more than every other boot lever combined. Keeping the random
   spawn is a legitimate answer; it just has to be a chosen one.

---

## 15. One-paragraph summary

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
**Clouds** wait for a reference picture. **Boot** stops being a ten-second problem by ceasing to
pre-generate a world nobody can see yet: the GPU authors bricks where they are consumed, the first
frame needs only a spawn bubble, and the horizon is drawn from the height function until it
voxelises — target, one second to a playable frame. And the whole thing is measured at 3840×2160 from
the first day, because at this scale the 4K budget is the design constraint — not a thing to check at
the end.
