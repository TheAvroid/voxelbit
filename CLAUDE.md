# voxelbit

A voxel game that ships as one self-contained HTML file the player double-clicks. That
has not changed. What changed is that the 16,226-line file is no longer what you edit.

## Do not use sub-agents

Work on this repo directly. Do not spawn sub-agents / Task agents for any part of it — not
to scout, not to build, not to verify. This is a standing instruction from the repo owner
(2026-08-19), not a per-task preference, and it holds regardless of how large the batch is
or how parallelisable the work looks.

Two reasons it matters here, both observed:

* **Concurrent agents each boot their own Chrome, and that corrupts every timing number.**
  Three at once made two batches of frame-time measurements worthless — one agent measured
  the same +0.13 ms drift at a pose where its change was provably a no-op. Anything with a
  measured ms in it needs one browser on the box.
* **A wrong result from an agent reads as plausible.** A fixed-crosshair chop test once
  reported "the hive lost zero voxels" and looked like a real defect; the swing animation is
  570 ms with impact at 250 ms, and the test was re-arming it every 150 ms so no chop ever
  landed. Catching that needed the person who wrote the test to distrust it.

## Edit `src/`, never `game/index.html`

`game/index.html` is a **build artifact**. `tools/bundle.py` overwrites it from the 78
ordered fragments in `src/`. An edit made directly to it is destroyed by the next build,
silently, and will not be in the game.

```
src/manifest.txt        the build order. This file IS the architecture.
src/<area>/<name>.js    a fragment
tools/bundle.py         src/ -> game/index.html
tools/lint-vb.py        the 11 checks that catch a black screen before the browser does
tools/vbtest.py         boots the real game and diffs it against a baseline
tools/vbharness.py      keeps ONE booted game alive; queries it in ~0.2s instead of ~10s
tools/where.py          an index.html line -> the fragment that wrote it, and back
docs/architecture.md    what lives in which fragment
```

## Asking the running game a question

`tools/vbharness.py` keeps one booted game alive and answers queries against it. Use it
whenever you need more than one look at the game — which is nearly always. Booting a
browser per question costs ~10 s each and, in practice, far more in scaffolding; a query
against a live instance costs ~0.2 s.

```
python tools/vbharness.py start --win 1792x865     # ~10 s, once
python tools/vbharness.py eval "__vb.badgeDbg()"   # ~0.2 s, as often as you like
python tools/vbharness.py eval --file probe.js     # multi-line / async probes
python tools/vbharness.py shot out.png
python tools/vbharness.py reset                    # undo the last test's leftovers
python tools/vbharness.py reset --at 4096 4096     # …and wipe the WORLD (full regen, ~7 s)
python tools/vbharness.py reload                   # re-read src/, fresh page (no bundle.py)
python tools/vbharness.py errors                   # non-404 errors on this page
python tools/vbharness.py slots                    # live harnesses — check before dispatching an agent
python tools/vbharness.py stop
```

Four things about it that are not guessable:

- **`reload` re-bundles from `src/` in memory** (it serves through `serve-nocache.py`), so
  the edit → look → edit loop never runs `tools/bundle.py`. Run that before committing, not
  before looking. Note a reload re-randomises the spawn, so probe absolute coordinates.
- **Read a uniform only after a frame has ticked.** `__vb.badgeDbg()` and friends report
  what the render loop last wrote, so a probe that sets a value and reads it back in the
  same synchronous block gets the stale number and looks like a broken feature. `await` a
  couple of `requestAnimationFrame`s between the write and the read, and settle ~120 frames
  after a `giveIt`/teleport before trusting anything — the swap animation moves the model
  for about a second, and it will swamp a small effect.
- **Storms are held off by default.** `__vb.snow(0)` — which every older script in `tools/`
  calls — only cancels the storm in progress; the next one still arrives 120 s after load
  and every 5 minutes after that, and snow writes into the world. The harness calls
  `__vb.snowHold(false)` at boot and after every reload. Pass `start --snow` if you are
  actually testing snow.
- **One world answers every query.** `eval` is free for read-only probes; anything that
  chops, fells, stamps, teleports or drops leaves the world changed for the next test.
  `reset` clears what a teleport does not (felled bodies, editor, camera, counters);
  `reset --at X Z` additionally zeroes the world, because a teleport further than 200
  voxels triggers a full regen and worldgen is a pure function of world coordinates.

`--slot NAME` (or `VB_SLOT`) gives each agent its own instance, so concurrent agents never
collide — but the machine still only takes 2–3 booted games, so treat a slot as a resource
you claim, not one you spawn per task. An idle instance reaps itself after 30 minutes, and
killing the daemon by any means takes its Chrome with it.

`tools/vbharness.py reap` deletes Chrome profiles from finished runs (never one in use).
`tools/cdp.py` has never removed them, and they reach tens of GB.

## The loop

```
python tools/serve-nocache.py     # or double-click start.bat
```

The dev server builds `src/` in memory on every request, so **edit a fragment and hit
refresh** — there is no build step while you work. Run `python tools/bundle.py` before
committing, so the artifact in git matches the source.

Before you commit:

```
python tools/bundle.py       # refresh the artifact
python tools/lint-vb.py      # 11 static checks; exit 1 on any problem
```

**This is enforced, not remembered.** `tools/hooks/pre-commit` runs both of the above and
stages the rebuilt `game/index.html` into the commit; a failing lint aborts the commit.
`tools/hooks/pre-push` re-checks and refuses to push a bundle that disagrees with `src/`.
`tools/hooks/post-merge` covers the one commit the other two cannot: git does not run
`pre-commit` for an automatic merge, and `game/index.html` carries `merge=ours`, so a
merge keeps THIS side's bundle and the branch you just merged is missing from the
artifact - the game then runs without the work you merged. It rebuilds, stages, and asks
you for a `git commit --amend --no-edit`.
Both are verified to fire: a deliberately stale artifact is rejected with the first
differing line. So the rule is simply **edit `src/`, commit, push** — never run
`bundle.py` by hand and never edit `game/index.html`.

Hooks live in the repo but `core.hooksPath` is local config, so **a fresh clone or a new
worktree must run this once**:

```
git config core.hooksPath tools/hooks
```

The hook FILES are tracked, so a worktree created before they landed still has none —
`hooksPath` then points at a directory that is not there and git runs nothing, silently,
with a clean exit code. Merge `main` into each existing worktree once; `lint-vb.py`
check 11 is what tells you a worktree is in that state.

`git commit --no-verify` skips them. Do that only when you already rebuilt by hand — the
pre-push backstop will catch you if you did not. One caveat: the pre-commit hook builds
from the **working tree**, so a partial commit (`git commit <paths>`) can stage an
artifact built from fragments the commit does not include. Commit `src/` wholesale.

If you changed anything the linter cannot reason about - worldgen, the frame loop, a
shader, the uniform buffer - boot the real game and compare against a baseline:

```
python tools/vbtest.py --against pre-phase-a
```

It runs offscreen on its own port, so it neither touches your cursor nor fights the game
you are playing on 8080. It checks: the page boots, nothing threw, all 267 `__vb` keys
are present, the screen is not black, the generator is bit-exact (`deepHash`), the worker
pool and main thread still agree (`gtest`), and the frame time has not regressed.

Two things about it are worth knowing before you trust a red result:

- **17 console 404s are normal.** The frame loaders walk `00, 01, 02 …` until one 404s -
  that is how they find the sequence length. Only an *uncaught exception* is a failure.
- **`worldHash` is not stable across boots and the gate does not check it.** Snow and
  grid-stamped creatures are real writes into `W`, so a surface block carries whatever the
  weather and the animals were doing. `deepHash` (y 24..72, below all of that) is the
  bit-exact generator check. Re-baseline if the machine's load has changed - a busy
  machine measured against a quiet baseline reads as a perf regression.

## A fragment is a slice of text, not a module

Everything from `core/boot.js` to `main/99-close.js` lives inside the single
`(async () => { ... })()` that `core/boot.js` opens and `main/99-close.js` closes. Thirteen
fragments have their own scope (see "Making a fragment a module"); the other 55 do not, and
none of them use `import`/`export`. Three consequences that matter every time you edit:

- **A fragment may use anything declared in a fragment above it**, exactly as the one big
  file did. Order is `src/manifest.txt`, top to bottom — not alphabetical, not inferred
  from the directory names.
- **One lexical scope spans the 55 non-module fragments.** `const rad` in `sim/tools.js`
  collides with `const rad` in `ui/hud.js`; the whole game is then a SyntaxError and a
  black screen. Check 5 catches it, and it is the failure this layout newly makes possible
  — run the linter. Converting a fragment to a module removes its private names from that
  risk entirely.
- **Some fragments open a brace that a later one closes.** `main/tick-body.js` opens
  `function tickBody(now) {`; the next six fragments are its body; `main/tick-passes.js`
  closes it. They are cut at statement boundaries, so ordinary editing inside one is
  safe, but do not touch a fragment's first or last line without checking its neighbour.

## Dispatching agents: six rules, each of them paid for

These are not style preferences. Each one was measured on 2026-08-18, on a session where agents did
~40 minutes of wall-clock work and roughly 40% of it was wasted.

**1. Give the agent the harness. Never let it boot its own Chrome.** Two verification agents cost
12.5 and 5.9 minutes, and almost all of it was scaffolding — each launched a browser, generated a
world, and wrote its own probe rig. `tools/vbharness.py` reduces that to `eval` at ~0.2 s. Put the
slot in the prompt: *"use `python tools/vbharness.py --slot <name> eval`, do NOT launch Chrome"*, and
name a slot nobody else is on (`vbharness slots` lists the live ones). Tell it the three traps too —
await a frame before reading a uniform, settle ~120 frames after a teleport, and re-click the canvas
if `__vb.ft()` returns null — or it will rediscover them at your expense.

**2. Scope to a DECISION, not a topic.** The prompts that paid off asked numbered questions and
demanded `file:line`. The one that asked for "a survey of X" returned 200k tokens of which maybe a
tenth was used. Ask what you need to decide, and say what you will do with the answer.

**3. Cap the output in the prompt.** "Ranked list, ≤1500 words, file:line, no quote longer than 10
lines." Without it you get correct, exhaustive, unreadable reports and pay for all of it.

**4. Split by HYPOTHESIS, never by file.** The slider bug was solved by one agent asking "is it the
maths?" and another "is it the DOM?" — they could not overlap, and the second found four defects
nobody would have gone looking for. Split by file and both read the same code and report it twice.

**5. Cheap review BEFORE expensive verification, never concurrently.** Running a static review and a
CDP verification in parallel looks efficient and is not: the review found two real defects, which
invalidated the finished 12-minute verification, and it had to be run again. Static review is
minutes; in-game verification is expensive. Serialise them in that order, then verify ONCE against a
final build.

**6. Never block on an agent that can only confirm.** If you already found the cause by reading the
code, the agent's job is to BROADEN — "what else is wrong in here" — not to agree with you. Start
fixing immediately either way. 3.4 minutes were spent waiting to be told something already known.

Two more things worth planning around. Agents parallelise BREADTH, not DEPTH: once work becomes
fix → verify → next-fix-depends-on-result, they stop helping, and the answer is to shorten the chain
rather than add agents to it. And they die — two hit API 529s mid-session — so prefer several small
agents over one long one, because a death at minute eleven costs everything it had not reported.

## Several agents at once

One agent per fragment. `docs/architecture.md` maps subsystem to file; pick disjoint files
and the *edits* cannot collide. Four things are still shared, and they are the whole list:

- **The lexical scope**, for the 55 fragments that are not yet modules: two agents can pick
  the same top-level name in different files and neither will see it. That is a SyntaxError and a
  black screen, and it only surfaces when the two branches meet. `lint-vb.py` check 5 is
  what catches it — run it after merging, not just before pushing.

  **Ask before you invent a name**, while it is still cheap to pick another:

  ```
  python tools/lint-vb.py --name tmpBox,rad
  ```

  Three answers, and the third matters: `TAKEN` with the file and line, `free`, or *free
  here but private to `ui/editor.js`* — a module's private names are yours to reuse, which
  is the whole point of scoping, and knowing that stops you renaming around a collision
  that does not exist. Exits 1 if any name is taken, so a script can gate on it. This is
  the only check worth running as you type; every other one answers "did we already break
  it", and check 5 cannot fire for either agent alone — it fires once, at the merge, on
  whoever happens to be doing the merging.
- **`game/index.html`** is generated. Both agents will rebuild it, so it conflicts on every
  merge; take either side and re-run `tools/bundle.py`. Check 7 refuses a stale artifact,
  so a bad resolution cannot survive a lint.
- **`src/manifest.txt`**, but only when adding or removing a fragment.
- **Port 8080.** `start.bat` kills whatever already holds it, so running it takes down the
  server someone else is using. Agents should run `tools/vbtest.py`, which serves and
  debugs on probed-free ports under a per-run Chrome profile, and never touches 8080.

Two fragments are big enough to be contention points on their own —
`main/tick-creatures.js` (916 lines, one indivisible `for`) and `main/debug-api.js`
(1,066 lines, `window.__vb`). Expect to queue on those rather than parallelise them.

## Worktrees — one per TASK, not one per directory

A worktree is created for a job and removed when that job merges. It is **not** a standing
home for a directory. Six standing per-directory trees (`wt-main`, `wt-render`, `wt-sim`,
`wt-ui`, `wt-world`, `wt-assets`) were tried and removed on 2026-08-11, because ownership
by directory does not match how this codebase changes:

- Every commit that has touched `src/` since the fragment split touched **more than one**
  directory — 5 of 5, averaging 5.0 of the 6.
- Structurally there are 486 cross-fragment reference edges, 8.2 per fragment on average.
- Worked example: a four-line "hold right-click to keep eating" change touched
  `ui/audio.js`, `sim/life/reactions.js`, `main/tick-camera.js` and `main/debug-api.js`.
  Under directory ownership that is three trees for one small feature, and none of the
  three can boot the game to test its own third.

A feature here is state + simulation + render + UI by nature. Directory is the one axis it
never respects, so a per-directory tree either idles or forces a three-way merge for a
four-line change. **Give an agent every file its job needs, and give that job its own
tree.** The unit of isolation is the task; the file list comes from `docs/architecture.md`.

```
git worktree add C:/voxelbit-wt/<task> -b task/<task>
...work, commit...                # hooks need NO setup: core.hooksPath lives in the
                                  # shared config, so a fresh worktree inherits it
git merge task/<task>          # from C:/voxelbit
python tools/bundle.py         # game/index.html conflicts every time; regenerate, never merge it
python tools/lint-vb.py        # RUN AFTER THE MERGE — see below
git worktree remove C:/voxelbit-wt/<task> && git branch -d task/<task>
git worktree list              # what is currently live
```

The Agent tool's `isolation: "worktree"` does the create/remove half automatically, one
tree per agent, cleaned up if the agent changed nothing. Prefer it over standing trees.

The lint matters here specifically: two branches can each declare `const rad` in different
fragments and both pass their own lint, because the clash does not exist until the edits
are in one tree. Ask first with `python tools/lint-vb.py --name rad` while picking another
name is still free.

**Never run two agents or two Claude tabs against the same directory.** They share a
filesystem with no coordination — both editing `src/`, both running `bundle.py`, both
overwriting `game/index.html` — and the damage happens before anything is committed, where
no check can see it. One worktree per task is what makes that impossible.

**Two agents can edit in parallel more easily than they can verify in parallel.** A booted
game costs ~1.7 GB (measured: 1744 MB JS heap, a 1.5 GB CPU world plus its GPU copy), so
the machine takes 2–3 concurrent `vbtest.py`/CDP runs regardless of how many agents are
typing. Serialise the verification, not the editing.


## Working rule: parallelise independent subsystem tasks

When given two or more *independent subsystem* tasks at once, run them as parallel
subagents rather than in sequence (user's standing instruction, 2026-08-09).

- **Assign by FILE, not by topic.** "Improve the fish" wanders into `tick-creatures.js` and
  `nav.js` and collides with whoever else is working. "You own `sim/life/fish.js` and
  `sim/nav.js`, edit nothing else" is the briefing that works — derive that file list from
  `docs/architecture.md` before launching, and hand the agent ALL of it, across directories
  if that is where the job goes. The file list IS the task boundary.
- **Launch them in one message** so they actually run concurrently.
- **Isolate anything that writes** — one worktree per task (see above), or they fight over
  the generated `game/index.html`.
- **Run `tools/lint-vb.py` after merging their work.** Each agent's own lint passes; a
  duplicate top-level name between two of them only exists once both edits are in one tree.
- **`tools/vbtest.py` is a per-machine resource**, not per agent: ~2 concurrent runs before
  they contend, and frame-time numbers are only trustworthy when nothing else is running.
  Serialise it.

Does not apply to sequential or dependent steps, or to edits small enough that briefing an
agent costs more than doing the work.

## Making a fragment a module

Thirteen fragments now have their own scope. Put these two lines at the very top of a
fragment and `tools/bundle.py` wraps it in an IIFE that returns exactly the named list:

```js
  // @module — one line on what this owns
  // @exports foo, bar, baz
```

Everything else it declares becomes invisible to the other 77 fragments, so two agents can
both invent `edIdx` and the merge is still fine. Names arrive in the shared scope as
ordinary consts at the module's own position, so every use below reads exactly as before,
at the same cost.

You do not have to work out the export list. Guess, run `python tools/lint-vb.py`, and
check 10 tells you precisely which names to add or drop — it derives the real answer from
what the rest of the build actually reaches for.

**A module cannot export a `let` that another fragment assigns.** The shared scope gets a
const copy, so those writes would land on the copy and the module would never see them —
silently. Check 10 refuses it and names the writer. Two fixes, both real:

- The name does not belong here. `cmpOn` was the compass setting sitting in
  `ui/video-editor.js` purely because that is where a Phase A cut fell; it moved to
  `ui/input.js`, where every use already was.
- The state is genuinely shared. Fold it into an object the module already exports —
  `veLastPaint` became `VE.lastPaint`, so the frame loop's write lands on the object both
  sides hold.

**A module may not `await` at its top level.** The program is one `(async () => {` opened
in `core/boot.js`, so `await` is legal at a fragment's top level — but `wrap_module`'s IIFE
is *not* async, and scoping such a fragment makes that line
`SyntaxError: Unexpected reserved word`. Every other check passes, the bundle builds, and
the only symptom is a page that never boots: vbtest says just "game never became ready
within 180s". Check 10 now catches it and names the file and line. `assets/models.js` is
the live case — line 7 is `await stage('loading decorations…')`, and that one line is the
whole reason it cannot be a module.

Currently modules (13): `render/wgsl/vis.js`, `sim/life/mammals.js`, `sim/life/reactions.js`,
`sim/particles.js`, `sim/projectiles.js`, `sim/solver.js`, `sim/support.js`, `sim/tools.js`,
`ui/console.js`, `ui/editor.js`, `ui/video-editor.js`, `world/gen-worker.js`,
`world/terrain.js`. The other 55 fragments share one scope, so check 5 still matters.

**Scoping is close to exhausted, and that is a finding rather than a to-do.** Every
fragment was measured against the three gates; the shared surface is 993 names and only
about 18 more could be removed by scoping what is left:

| why not | count | what it means |
|---|---|---|
| already a module | 13 | done |
| exported `let` that something assigns | 28 | **the real blocker** — shared mutable state |
| exports everything it declares | 22 | a pure interface; scoping hides nothing |
| opens/closes a brace across fragments | 4 | all of `src/main/`, plus `core/boot.js` |
| top-level `await` | 1 | `assets/models.js` |

The 28 are the same wall the original ES-modules analysis hit: 211 of 334 top-level `let`s
are assigned from a fragment other than the one declaring them. Until that shared mutable
state is folded into objects, no amount of `// @module` will shrink the scope much further
— so treat check 5 and check 10 at merge time as the real defence, not scoping.

## Adding a fragment

Write the file, then add its path to `src/manifest.txt` **in the position where it should
be evaluated**. A fragment that is not listed is not in the build; the linter fails on
that rather than letting the code silently vanish.

## House rules that outrank convenience

- **Perf work must be perceptually lossless.** Anything lossy needs explicit sign-off.
- **The game is CPU-bound** (GPU ~1.7 ms against a 2.5–5.4 ms frame). Profile JS with the
  Chrome sampler; shader toggles mislead.
- **Never commit or push** without being asked.
- **Put `//` comments at the end of a line, never mid-way into a dense one-liner** — the
  comment eats the rest of the line, taking the closing braces with it.
- **No backticks inside WGSL comments** — a `` ` `` ends the JS template literal early and
  the boot dies with no useful error.
