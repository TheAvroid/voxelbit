# voxelbit

A voxel game path traced on NVIDIA Falcor, in C++ and Slang. It ships as ONE file a
player double-clicks: `website/voxelbit.exe`, an Inno Setup installer. It was a
self-extracting launcher until 2026-09-22, when Defender started deleting that on
download as `Trojan:Win32/Sabsik.FL.A!ml` -- see tools/voxelbit.iss.

**This document was rewritten on 2026-09-21.** Until that day voxelbit was a WebGPU
renderer in a browser tab, `src/` held 78 JavaScript fragments and `game/index.html`
was the build artifact -- and half of this file was about how to work on them. That
engine has been retired and deleted. The sections that described it are gone rather
than left to send a future session editing files that do not exist; what remains is
the part that was never about the browser.

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

## The engine is `engine/`, and the shipped exe is not it

```
engine/src/        the engine: C++ headers and .inl, compiled as one TU
engine/shaders/    Slang. Deployed to build/bin/Release/shaders/v1/
engine/build.bat   build it            run.bat   run it (--help for every option)
game/assets/       the art the engine loads -- .vox, sound, the pixel font
tools/             the voxelisers and bakers that produced the art
launcher/          the OLD self-extracting launcher -- superseded, still builds
wallet/            the bitcoin price tracker that is becoming the wallet. RUST,
                   not C++ (BDK is the wallet library), and its own process so
                   keys never share one with the game. wallet\build.bat builds
                   and copies wallet\wallet.exe (git-ignored);
                   --check verifies the live feed headless, and
                   tests/render.rs draws the window to PNG with no window.
website/           the download page, and THE SHIPPED INSTALLER it serves:
                   website/voxelbit.exe, built by tools/package.py --installer,
                   never by hand. There is no copy at the repository root any
                   more (user 2026-09-22) -- index.html links to it relatively,
                   so the one beside it is the one players get.
```

### Keep `voxelbit.exe` in step with the code

**Rule (repo owner, 2026-09-21): the shipped exe must match the codebase.** It is a
SNAPSHOT, not a link -- it does not track the engine, and a stale one is the worst
kind of wrong, because it launches and plays perfectly while being last week's build.
A bug you just fixed is still in it.

Refresh it with:

```
engine\build.bat                       # 1. the engine first -- package.py copies a binary
python tools\package.py --installer    # 2. restamp website\voxelbit.exe
```

**THE RULE, stated by the repo owner on 2026-09-21: repackage at the end of
every batch.** Not when it feels significant, not when asked -- every time a
batch of work is finished and handed back. The reason is what kept happening
without it: a fix was reported as done, the owner double-clicked the exe, and
got a build from several fixes ago. A stale exe does not fail, it lies.

So the last two actions of any batch that touched the engine are:

```
engine\build.bat                       # the engine first -- package.py copies a binary
python tools\package.py --installer    # then restamp website\voxelbit.exe
```

and the reply says the new stamp. Also run it:

* before telling the owner the exe is updated, whenever that is not the batch end;
* before a commit intended to ship from.

**When it must NOT be run: on every build.** Packaging reads 530 MB, writes 780 MB
and takes minutes, against a relink of a few seconds -- automatic repackaging would
make a one-line iteration cost a full package, and this loop is iterated dozens of
times an hour. That is why it is a rule and a check rather than a build step.

The tree will tell you when it is overdue: `python tools/package.py --check` answers
in one line (exit 0 fresh, 1 stale, 2 absent), and `engine\build.bat` runs that check
itself and prints the result whenever a `dist/` already exists.

**Every new package unpacks to a new folder** under `%LOCALAPPDATA%\voxelbit\<stamp>`,
because the stamp is a hash of the payload. The old one is dead weight -- delete it.

### Verifying a change

The engine carries headless diagnostics. They need a GPU but no window, and they are
the fastest way to find out whether something structural broke:

```
v1.exe --background --float-test    # nothing severed is left hanging in the air
v1.exe --background --dig-test      # every bite comes out of ground that was there
v1.exe --background --fell-test     # a felled tree comes apart and settles
v1.exe --background --wheat-test    # the crop pays out and is absorbed
v1.exe --background --clip-test     # no creature stands inside a solid
v1.exe --background --kill-test     # every species dies correctly
```

**`--background` is not optional.** It opens the window straight to the taskbar and
refuses the cursor, so a test cannot take the screen away from whoever is at the
keyboard. Pin the world with `--spawn <n>` when comparing two runs -- the spawn point
is random per launch, and a large swing in anything measured is usually that.

**The first run after a build is cold**: Falcor compiles shaders at startup and charges
that time to the first frame. A 9x regression that vanishes on the second run was never
there.

**A test that dies with no verdict is not necessarily your change.** The engine writes
`v2-crash.log` beside the exe with the faulting module, a stack and what it was doing
(see `engine/src/platform/crashlog.h`). Read it before blaming the code under test -- there
is a known intermittent crash during the startup world prime that has nothing to do with
whatever is being tested.

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

## House rules that outrank convenience

- **Never commit or push** without being asked.
- **`voxelbit.exe` must match the codebase** -- see the rule above. It is the only
  artifact a player ever sees, and it is the one thing in this tree that can be wrong
  while looking perfectly right.
- **Perf work must be perceptually lossless.** Anything lossy needs explicit sign-off.
- **Measure on a pinned world.** `--spawn <n>`, or the number you are comparing is the
  spawn point rather than your change.
- **Never put the game on screen.** Every run is `--background`; every capture is
  minimised. Reported three times.
- **Never kill a `v1.exe` you cannot prove is yours.** More than one session works this
  tree, and the other one may be mid-render or the owner may be playing. See
  `engine/CLAUDE.md` and `engine/tools/claude-session.ps1`, which is how builds and runs are
  serialised.
- **Comments say WHY, and record what was measured and rejected** as well as what
  shipped -- so nobody spends a weekend rediscovering that an idea was already tried
  twice. If a measurement made you change something, put the measurement in the comment.
