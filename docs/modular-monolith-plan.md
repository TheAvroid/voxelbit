# Modular monolith — migration plan

**Status:** proposed, nothing implemented.
**Target:** `game/index.html` — 15,639 lines / 1.49 MB, one `<style>` block and one
`<script type="module">` containing a single `(async () => { ... })()` IIFE.

---

## 1. What we actually have

Measured, not assumed:

| Region | Lines | What lives there |
|---|---|---|
| `<style>` | 8–336 | every rule: loading meter, HUD, cursor, panels, editor, video editor |
| `<body>` markup | 336–445 | canvas + all HUD/panel DOM |
| IIFE opens | 446 | `(async () => {` |
| boot / mobile gate / loading meter | 457–521 | |
| WebGPU init, freeze forensics, CPU telemetry, upload accounting | 522–610 | |
| world window constants (`WX/WY/WZ`, `RD_FIXED`, `stopY`, light masks, `WATER_BAKE`) | 611–705 | |
| worldgen math + gen worker | 706–964 | noise, rivers, `makeHRow`/`makeHCol`, `fn.toString()` worker |
| palettes, `.vox` loading, material tables | 965–1545 | `solidTab`, `decorTab`, `woodTab`, `rockTopTab`, … |
| support / anchor rules (`SUP`) | 1546–1732 | |
| held items, creature assets, `pickWGSL` generation | 1733–2324 | |
| terrain generation: `fillColumn`, every `*At`/`stamp*`, `rockRowSpan`, bricks | 2325–2980 | |
| `buildWorld`, gen worker **pool**, `blitSlab` | 2981–3244 | |
| GPU buffers, uniform struct (`UF_*` offsets), drop slots, Gerstner table | 3245–3438 | |
| **WGSL** — `PRE`, `FLAKEBLK`, `DDAW`, `TRACE`, `SCATTER`, `PATCHW`, `TEMPORAL`, `SPATIAL`, `COMPOSITE`, `TAA`, `BLIT`, `VIS` + pipelines | 3439–5338 | ~1,900 lines, the single largest block |
| patch encode/flush, targets, streaming rect / band / recenter | 5340–5595 | |
| player movement, swim, camera | 5596–5826 | |
| pickup, hands, hotbar, throw, console commands, bow / arrow / spear | 5827–6384 | |
| `gpuPatch` — the world-mutation choke point | 6385–6450 | |
| rigid bodies (`PH`), chopping, support flood/resolve | 6451–7660 | |
| tools: aim, chop, dig, till | 7861–8500 | |
| creature life sim (birds, fish, ducks, worms, mammals) | 8507–9287 | |
| achievements, particles, audio | 9288–9788 | |
| keybinds | 9789–9841 | |
| screen recording + video editor | 9842–10218 | |
| settings panels (DOF, snow, wind, water, light debug, adjust box) | 10219–10804 | |
| profiling, HUD, editor (`ED`) | 10805–12437 | |
| `tick()` — the frame loop | 12438–13062 | |
| `tickBody()`, nav field, remaining sim | 13063–15636 | |

**One lexical scope holds 1,054 bindings** — 780 `const`, 191 `let`, 83 functions.
There are zero `import`/`export` statements today.

### The three things that make this file hard to cut

1. **Workers are built from live function source.** `game/index.html:3096-3099` serializes
   34 consts, 30 tables and **51 functions** via `fn.toString()` into a blob worker. Any
   serialized function may only reference identifiers the worker source re-declares. If
   modularization turns those references into *imports*, `toString()` still emits the same
   text but the names no longer exist in the worker — a `ReferenceError` on a background
   thread, or worse, a silent fallback to the inline path.
2. **Shaders are JS template literals with interpolation.** `TRACE` embeds `pickWGSL`,
   `PRE` embeds `GERSTH_WGSL`/`GERSTN_WGSL`, several embed `DROP_SLOTS` and `items.length`.
   These evaluate *at module-eval time*, in declaration order — which is exactly why a
   `const` read before its declaration inside an earlier shader literal produces a black
   screen at boot with no useful error.
3. **The game is CPU-bound.** GPU ~1.7 ms against a 2.5–5.4 ms frame. 191 hot mutable
   top-level `let`s are read and written every frame from the closure. Scattering those
   across modules as exported `let` bindings is the one refactor that could measurably
   cost frame time.

---

## 2. What "modular monolith" means here

Modular **source**, single **runtime**, single **artifact**.

- ~30 source modules under `game/src/`, strict downward-only dependencies.
- Dev: native ESM over the existing `tools/serve-nocache.py` — no bundler, no watch step,
  edit-and-refresh stays exactly as it is today.
- Release: one Python build script inlines every module back into a single
  `game/index.html`, preserving today's zero-dependency, double-click-`start.bat`
  distribution. v1.0 ships as a local browser game; a single self-contained HTML file
  stays the shipping format.

Nothing becomes a separate process, worker boundary, or package. This is a file-layout
and scope refactor, not an architecture change to the running program.

### Target layout

```
game/
  index.html          <- <head>, <body> markup, <link> + <script type="module" src>
  src/
    styles/           loading.css hud.css panels.css editor.css
    core/             config.js  telemetry.js  gpu.js
    assets/           vox.js  palette.js  models.js
    world/            gen-core.js  gen-pool.js  window.js
    render/
      wgsl/           pre.js dda.js flake.js trace.js scatter.js patch.js
                      temporal.js spatial.js composite.js taa.js blit.js vis.js
      uniforms.js  pipelines.js  frame.js
    sim/              support.js  physics.js  player.js  tools.js
                      weather.js  nav.js  life/{pool,birds,fish,water,mammals}.js
    ui/               hud.js  settings.js  keybinds.js  audio.js
                      achievements.js  editor.js  video-editor.js
    main.js           boot, tick() orchestration, __vb assembly
tools/
  bundle.py           modules -> single-file index.html
  lint-vb.py          the four boot-killer checks (see section 5)
```

**Layering rule:** `core` -> `assets` -> `world` -> `render`/`sim` -> `ui` -> `main`.
A module may only import from a strictly lower layer. `main.js` is the only wiring point.

---

## 3. Phase A — mechanical split, provably behaviour-identical

This is the phase that removes essentially all the risk, and it should be done first and
completely before any module boundary is designed.

1. Cut the IIFE body into ordered fragments at the region boundaries in section 1.
   Fragments are *not* modules yet: no `import`, no `export`, no scope change. Just text,
   in order, in files.
2. Write `tools/bundle.py` to concatenate the fragments back in order into `index.html`.
3. **The gate: the bundle must be byte-identical to today's `index.html`.**
   `git diff --exit-code` proves it. No screenshot, no playtest, no judgement call —
   if the bytes match, behaviour is identical by construction.
4. Only once that holds, commit the split.

Every cut in Phase A is free. Getting a boundary "wrong" costs a re-cut, not a bug.

**Dev-time note:** during Phase A the browser still loads the bundled single file, so
`start.bat` and the CDP test flow are untouched.

---

## 4. Phase B — fragments become modules

One boundary at a time, leaf-first, each its own commit. The byte-identical gate is gone
from here on, so each step needs the verification in section 6.

Order, chosen so every extracted module has no unresolved outward reference:

| # | Module(s) | Risk | Note |
|---|---|---|---|
| B1 | `styles/*.css` | none | pure `<link>` extraction |
| B2 | `core/config.js`, `core/telemetry.js` | low | leaf constants + `cprof` / frame ring / `vbNoteErr` |
| B3 | `core/gpu.js` | low | device init, buffer helpers |
| B4 | `render/uniforms.js` | **medium — high value** | see below |
| B5 | `render/wgsl/*` | medium | shader factories, see below |
| B6 | `assets/vox.js`, `assets/palette.js`, `assets/models.js` | medium | palette is 256 slots and full |
| B7 | `world/gen-core.js` | **highest** | the worker serialization boundary, see below |
| B8 | `world/window.js`, `world/gen-pool.js` | high | `gpuPatch` choke point, `blitSlab` |
| B9 | `sim/support.js`, `sim/physics.js` | high | `SUP` and `PH` are mutually aware; extract as a pair |
| B10 | `sim/player.js`, `sim/tools.js` | medium | |
| B11 | `sim/life/*` | medium | slot bands are the contract; keep the band table in one place |
| B12 | `sim/weather.js`, `sim/nav.js` | medium | |
| B13 | `ui/*` | low | mostly DOM + listeners; safe and satisfying |
| B14 | `render/pipelines.js`, `render/frame.js` | medium | |
| B15 | `main.js` | — | whatever is left is the boot + `tick()` orchestration |

### B4 — `render/uniforms.js` does real work

The `UF_*` offsets are hand-computed today, and the code comments record why fields are
appended rather than inserted ("no existing offset moves"). Make the struct a **declared
field list** and derive both the WGSL struct text and the JS byte offsets from it. That
turns an append-only-or-else invariant into something the machine enforces, and it is the
single highest-value structural win in the whole migration.

### B5 — shaders become factories, not literals

Each WGSL module exports a **function**, not a string:

```js
export const trace = ({ pickWGSL, DROP_SLOTS, FOL }) => /* wgsl */`...`;
```

Because a function body evaluates when it is *called* — after every module has
initialized — the "const read before its declaration inside an earlier shader literal"
class of black screens becomes structurally impossible. Interpolated values arrive as
explicit arguments, so the dependency is visible in the signature instead of implicit in
file order. Keep the `/* wgsl */` marker; keep the "no backticks inside WGSL comments"
rule (it still kills the boot silently) and move it into `lint-vb.py`.

### B7 — `world/gen-core.js` and the worker contract

This is the one module with a rule the other 29 do not have:

> **`gen-core.js` is self-contained at runtime.** Everything the 51 serialized functions
> touch — the 34 consts, the 30 tables, and each other — lives in this module. It imports
> nothing that its serialized functions reference.

Make the existing registries the module's *explicit exports* rather than an inline object
literal at line 3096:

```js
export const GEN_CONSTS = { WY, LIFT, WL, /* ... */ };
export const GEN_TABLES = { NEEDLE, MOSS, DIRT, /* ... */ };
export const GEN_FNS    = { ihash, sstep, vnoise, /* ... */ };
```

Then add the check that does not exist today: `lint-vb.py` parses every function in
`GEN_FNS`, collects free identifiers, and fails the build if any is not a JS global, a
parameter, a local, or a member of the three registries. Today that rule lives only in a
notes file and has to be remembered; after this it is enforced.

*Optional follow-on, not part of this plan:* once `gen-core.js` is genuinely
self-contained, the blob-plus-`toString()` construction can be replaced with
`new Worker('./src/world/gen-core.js', { type: 'module' })`. Cleaner, but it changes
worker startup and bit-exactness needs re-proving — treat as a separate project.

### The hot-state rule (applies throughout Phase B)

Do **not** export the 191 mutable top-level `let`s as individual live bindings. Group hot
per-frame mutable state into a small number of shared state objects or typed arrays owned
by one module. Frame-loop code touches fields on an object it holds a reference to — same
access shape as today's closure variables. Cold configuration and one-shot flags can be
ordinary exports.

---

## 5. Phase C — make the boundaries hold

`tools/lint-vb.py`, run before every commit. Four checks, each earned by a bug that has
already cost a debugging session:

1. **Layer violations** — an import that points sideways or upward. Fails the build.
2. **Import cycles** — report the cycle; a cycle usually means the boundary is wrong.
3. **`gen-core` free-identifier check** — see B7. The worker contract, enforced.
4. **Boot-killer syntax patterns** — a `//` comment mid-way into a dense one-liner (eats
   the rest of the line: braces -> `SyntaxError`, or silently, assignments that never run),
   and a backtick inside a WGSL comment.

Duplicate top-level `const` and the const-before-declaration black screen both stop being
possible once each module has its own scope — they need no check because the structure
removes them.

---

## 6. Verification, per step in Phase B

Phase A proves itself with `git diff`. Phase B needs the real harness:

- **Boot:** headless CDP with `?cdp` + canvas click, offscreen window, `--mute-audio`,
  own `VB_SLOT`. Screenshot via CDP — in-page canvas readback of a WebGPU surface returns
  all-zero and will report a false black screen.
- **Worldgen bit-exactness (B7/B8):** the worker-battery oracle — generate the same rect
  on the main thread and through the pool and compare bit-exactly. `worldHash` is not
  stable across sessions, so compare within one run. Subtract stamped birds before
  blaming worldgen for a diff.
- **Support/physics (B9):** `__vb.floatAudit` before and after; a felled pine is the
  standard case.
- **Performance (all steps):** `__vb.prof` / `__vb.cprof(true)` phase splits and the
  frame-time ring, before and after. The standing rule is that perf work is perceptually
  lossless — the same bar applies to a refactor. A step that costs frame time gets
  reverted or re-shaped, not accepted.
- **`__vb` surface:** 44 debug entry points exist and the CDP tests depend on them.
  `main.js` reassembles the identical object; treat a missing key as a build failure.

Every step is one commit, so any regression bisects to a single boundary.

---

## 7. Honest cost/benefit

**What this buys**

- Two whole classes of silent boot failure (duplicate top-level `const`, const-order/TDZ
  in shader literals) stop being possible rather than being remembered.
- The uniform-struct append-only invariant becomes machine-checked (B4).
- The worker serialization contract becomes machine-checked (B7) instead of a note.
- Grep, edit, and review operate on 300–900 line files instead of a 15.6k-line one; agents
  can work on separate subsystems without colliding.
- Subsystems become independently testable.

**What it costs**

- A build step for release, where today there is none.
- Phase B is genuinely risky in B7–B9; those three steps deserve their own sessions.
- Real time: Phase A is ~1–2 sessions, Phase B is 15 increments, Phase C is small.

**Recommendation if the whole thing is too much:** Phase A + B4 + B5 + B7 captures most of
the structural value — the shader and worker boundaries are where the silent, expensive
bugs live — and leaves `sim/` and `ui/` as ordered fragments to split later, whenever
they get in the way. Stopping there is a legitimate resting point, not a half-finished
migration.
