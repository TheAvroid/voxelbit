# Architecture — what lives where

`game/index.html` is generated. The source is `src/`, concatenated in the order given by
`src/manifest.txt`. See [CLAUDE.md](../CLAUDE.md) for the rules; this file is the map.

**Read the order as the dependency graph.** A fragment can use anything declared above it
and nothing declared below it. That is the only layering the build enforces, and it is
exactly the layering the single file always had.

**Three fragments are modules** — marked ⓜ below. They open with `// @module` / `// @exports`
and `bundle.py` gives them their own scope, so only the exported names reach the other 77.
That takes 105 names out of the shared scope; the remaining 75 fragments still share one.
See [CLAUDE.md](../CLAUDE.md) for how to convert another.

## The map

| # | Fragment | Lines | What it owns |
|---:|---|---:|---|
| | **page** | | |
| 1 | `html/00-head.html` | 7 | doctype, meta, title, favicon |
| 2 | `ui/style-base.css` | 92 | font face, canvas, crosshair, free cursor, scroll hint, gold wordmark |
| 3 | `ui/style-loading.css` | 28 | loading overlay, `loading.mp4`, the progress meter and its gloss |
| 4 | `ui/style-menu.css` | 23 | esc menu, icon wave, hover labels |
| 5 | `ui/style-video-editor.css` | 57 | recorder button, rec indicator, editor panel, video + scrubber boxes |
| 6 | `ui/style-settings.css` | 54 | settings cards, option rows, slider knobs, the gold rail |
| 7 | `ui/style-panels.css` | 91 | light-debug, water, arrow-orientation, hand, held-stack panels |
| 8 | `ui/style-console.css` | 41 | command line pill and its px3 metrics |
| 9 | `html/10-body.html` | 111 | all HUD/panel DOM, `<canvas>`, opens `<script type="module">` |
| | **core** | | |
| 10 | `core/boot.js` | 82 | opens the IIFE, `$`, `fail`, the mobile gate, the loading meter |
| 11 | `core/gpu.js` | 30 | adapter/device, canvas context, moon texture |
| 12 | `core/telemetry.js` | 59 | `vbNoteErr`, `cprof`, frame-time ring, spike log, upload accounting |
| | **world** | | |
| 13 | `world/window.js` | 199 | `WX/WY/WZ`, `RD_FIXED`, `W`, `hmap`, `stopY`, `WATER_BAKE`, light masks |
| 14 | `world/gen-noise.js` | 94 | `ihash`, `vnoise`, the row-cached noise streams, boulder seating |
| 15 | `world/gen-worker.js` | 61 | the height + moss worker built from `fn.toString()` |
| | **assets** | | |
| 16 | `assets/palette.js` | 117 | the 256 slots, `pine5.vox`, bark smoothing, `solidTab`/`rockTopTab`/`decorTab` |
| 17 | `assets/models.js` | 120 | the sparse decoration format and its loader |
| 18 | `assets/bow.js` | 272 | arrow orientation, the bow frame strip |
| 19 | `assets/material-tabs.js` | 82 | `woodTab`, lily pads, surface scatter, the snow/ice tool rules |
| 20 | `sim/support-rules.js` | 187 | `SUP` — the one anchored-support rule and the `hmap` anchor |
| 21 | `assets/creatures.js` | 65 | cardinal flight frames, foam colour, the mammal ground seat |
| 22 | `assets/held-items.js` | 527 | every carried item as a 3D voxel grid; generates `pickWGSL` |
| 23 | `world/terrain.js` | 668 | `fillColumn` and every `*At` / `stamp*` — the deterministic generator |
| 24 | `world/build.js` | 81 | `buildWorld`, spawn beside a lake |
| 25 | `world/gen-pool.js` | 196 | the worker pool, the main-thread regen path, `blitSlab`, the slab doorbell |
| | **render** | | |
| 26 | `render/buffers.js` | 219 | GPU buffers, `UF_*` uniform offsets, brick upload, drop slots, the `GW` Gerstner table |
| 27 | `render/wgsl/pre.js` | 195 | `PRE_SRC` — sky, sun/moon, the water height field |
| 28 | `render/wgsl/dda.js` | 225 | the shared hierarchical DDA + `FLAKEBLK` |
| 29 | `render/wgsl/trace.js` | 489 | `TRACE_SRC` — the primary tracer (takes `DDAW`, `FLAKEBLK`, `pickWGSL`) |
| 30 | `render/wgsl/scatter-patch.js` | 31 | `SCATTER` + `PATCHW` — voxel patch scatter and the strip blit |
| 31 | `render/wgsl/denoise.js` | 81 | `TEMPORAL` + `SPATIAL` — SVGF |
| 32 | `render/wgsl/composite.js` | 679 | `COMPOSITE_SRC` — water, fog, DOF, creatures, particles, the final image |
| 33 | `render/wgsl/taa.js` | 105 | `TAA` resolve |
| 34 | `render/wgsl/blit.js` | 146 | `BLIT` — upscale, vignette, the held viewmodel |
| 35 | `render/wgsl/vis.js` | 95 | `VIS_SRC` — the debug visualiser, **and the SHADER BUILD block** + every pipeline |
| 36 | `render/targets.js` | 74 | screen textures + bind groups, rebuilt on resize |
| 37 | `world/stream.js` | 167 | demand-driven streaming: the rect, the band, recentre |
| | **sim** | | |
| 38 | `sim/player.js` | 275 | `ED` state, `P`, movement, swim, the rigid-body broad phase |
| 39 | `sim/hands.js` | 89 | right-click pickup, the hotbar, the flight to hand |
| 40 | ⓜ `ui/console.js` | 210 | the command line, `/spawn`, `/locate` |
| 41 | `sim/projectiles.js` | 278 | arrow, bow draw, spear, what a shaft bites |
| 42 | `world/patch.js` | 64 | **`gpuPatch`** — every runtime mutation of `W` passes through here |
| 43 | `sim/physics.js` | 333 | `PH`, body build, mass/COM/inertia, sleep |
| 44 | `sim/chop.js` | 360 | axe bite → falling chunk, scrap shedding, snow and cones coming down with the tree |
| 45 | `sim/chop-tree.js` | 266 | chopping a rigid body, `ok(id)`, the connectivity re-test |
| 46 | `sim/support.js` | 492 | the support flood, `supResolve`, the anchored-guess memo |
| 47 | `sim/tools.js` | 397 | voxel-accurate aim, chop, dig, till, what the swing ran into |
| 48 | `sim/solver.js` | 292 | sequential-impulse PGS, fixed 60 Hz |
| 49 | `sim/life/birds.js` | 112 | the flying cardinal's wandering flight |
| 50 | `sim/life/slots.js` | 146 | the drop-slot ledger, creature death, fair-share priority |
| 51 | `sim/life/fish.js` | 29 | `fishCfg` — every swimmer tunable |
| 52 | `sim/life/stamped.js` | 330 | grid-stamped worms/ducks, `stampedIdx`, the perched cardinal |
| 53 | `ui/audio.js` | 294 | the AudioContext, master volume, ambience, the anthem set, every sfx |
| 54 | `ui/achievements.js` | 47 | `vb_ach`, the discovery banner |
| 55 | `sim/particles.js` | 169 | splash, sparks, smoke, the tear; the 20 slot bands |
| 56 | `sim/life/reactions.js` | 314 | struck = spooked, the ragdoll, coming apart |
| 57 | `ui/keybinds.js` | 31 | rebindable keys, persisted |
| 58 | ⓜ `ui/video-editor.js` | 399 | screen recording, timeline, cut/delete, red annotations, the composited export |
| 59 | `ui/settings.js` | 364 | DOF, snow, wind, water, light debug, the adjust box |
| 60 | `ui/input.js` | 216 | pointer lock, mouse look, the compass, the free cursor |
| 61 | `ui/hud.js` | 132 | the frame-time readout and the HUD text |
| 62 | ⓜ `ui/editor.js` | 472 | the asset editor: platform, gizmos, `.vox` parse, pose bakes |
| 63 | `main/debug-api.js` | 1031 | **`window.__vb`** — 267 entry points the CDP tests depend on |
| 64 | `render/camera.js` | 24 | the cinematic still hold and its vignette |
| 65 | `main/tick.js` | 8 | `tick()` — the rAF wrapper that keeps the loop alive through an exception |
| 66 | `sim/nav.js` | 425 | the navfield: five planes, openness, the arbiter, the walker bands |
| 67 | `sim/life/mammals.js` | 192 | the one ground seat every land mammal uses |
| | **the frame** — fragments 68–74 are all one function, `tickBody(now)` | | |
| 68 | `main/tick-body.js` | 173 | opens `tickBody`; recording cadence, player physics |
| 69 | `main/tick-snow.js` | 222 | the snowfall sweep and flake landing |
| 70 | `main/tick-camera.js` | 292 | day/night, camera basis, the uniform write, held-item sun visibility |
| 71 | `main/tick-support.js` | 272 | DOF autofocus, the unified support resolver |
| 72 | `main/tick-nav.js` | 373 | nav solidity, fish probes, perched birds, butterflies, worms |
| 73 | `main/tick-life.js` | 87 | population scaling, the one spawn rule, the flock's slot budget |
| 74 | `main/tick-creatures.js` | 916 | the 372-slot creature loop — one `for`, indivisible |
| 75 | `main/tick-emit.js` | 168 | perched songbirds, fair-share emit, hit flash, sparks, the life uniforms |
| 76 | `main/tick-passes.js` | 95 | the GPU passes, prev-cam save, HUD; closes `tickBody`; boots the navfield and calls `tick()` |
| 77 | `main/99-close.js` | 1 | `})();` — closes the IIFE |
| 78 | `html/99-tail.html` | 3 | `</script></body></html>` |

## Where the sharp edges are

Four things in this codebase do not behave like ordinary code, and all four survived the
split unchanged. They are worth knowing before editing near them.

**The gen worker is built from live function source.** `world/gen-worker.js` and
`world/gen-pool.js` serialize consts, tables and functions through `fn.toString()` into a
blob worker. A serialized function may only reference identifiers the worker source
re-declares — a new helper called from inside one of them must be registered too, or it
is a `ReferenceError` on a background thread.

**Shaders are factories, called at one site.** Every WGSL literal in `render/wgsl/` is a
function (`PRE_SRC`, `TRACE_SRC`, …), and the only place any of them is called is the
SHADER BUILD block near the top of `render/wgsl/vis.js`, just above the pipelines. A
function body evaluates when it is called, so by then every fragment above has run - which
is why a `const` read before its declaration inside a shader literal is no longer a black
screen at boot. Shader-to-shader dependencies (`DDAW`, `FLAKEBLK`, `pickWGSL`) are passed
as explicit arguments whose destructured names shadow the outer ones, so each shader body
is byte-for-byte what it always was. If a shader ever needs a value produced later than
the build site, move the build block down - one line, instead of reordering files.

**`gpuPatch` is the choke point.** Every runtime mutation of `W` goes through
`world/patch.js`. Anything that must react to the world changing — the `stopY` column
cache, the navfield's `nvTouch`, the support queue — hangs off it.

**Hot state is closure state.** ~191 mutable top-level `let`s are read and written every
frame. They are ordinary variables in one shared scope, and moving one behind an accessor
is a real frame-time change, not a refactor. The game is CPU-bound.

## What this is not (yet)

This is a **modular monolith at the source level**: modular files, one runtime, one
artifact. The fragments are text, not ES modules — no `import`, no `export`, one shared
scope. That was deliberate: the split is provably behaviour-identical because
concatenation reproduces the original bytes, so it carried no risk of a rendering or
physics regression.

Turning fragments into real ES modules is the next step, and it is a different kind of
change — it alters scope, evaluation order, and the worker serialization contract. See
[modular-monolith-plan.md](modular-monolith-plan.md) §4 for the order, the per-step
verification, and which three steps deserve their own session.
