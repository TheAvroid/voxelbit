# Contributing to voxelbit

Fork it, branch, open a pull request. You do not need to be invited or to ask first.

Read this before you start, though — there are three things about this repo that will
otherwise waste your afternoon.

## 1. The licence assigns your contribution to the project

voxelbit is source-available and commercial, not open source. Section 4 of
[license](license) means that by opening a pull request you assign your rights in that
contribution to the project.

That is a real thing to agree to, so it is stated first rather than buried. If you would
rather not, that is completely reasonable — open an issue describing the bug instead. A
good bug report is worth as much as a patch.

## 2. Nothing is loaded by an absolute path

Every asset the engine opens goes through `asset()` in
[`engine/src/core/assetroot.h`](engine/src/core/assetroot.h), which joins a **repository-relative**
path onto a root resolved at startup:

```cpp
std::string wheat = asset("game/assets/decoration/wheat.vox");
```

Do not write `C:/voxelbit/...` anywhere. Forty-four paths did until September 2026, and
every one of them was a reason the game could not run on anybody else's machine. The
packaged build lays its content out in the same shape as the repository precisely so
that this one function is the whole of the difference between the two.

## 3. The engine's artifact is an exe git cannot see

`game/index.html` used to be a tracked build artifact with hooks guarding it. The engine
that replaced it builds to `engine/build/bin/Release/v1.exe`, which is **not** in the
repository — so a merge or a rebase can change the source underneath a binary you are
still running, and git will report a perfectly clean tree while you debug a mixture of
last week's C++ and today's shaders.

`tools/hooks/post-merge` runs `tools/rebuild-engines.sh` for exactly this. If you pull
and something inexplicable starts happening, rebuild before you investigate.

## The loop

```
engine\build.bat        # build
run.bat              # run, --help for every option
```

The first run after a build is **cold**: Falcor compiles shaders at startup and charges
that time to the first frame. A 9x regression that disappears on the second run was
never there.

## Before you open the PR

The engine carries its own headless diagnostics. They need a GPU but no window, and they
are the fastest way to find out whether you broke something structural:

```
v1.exe --background --float-test    # nothing severed is left hanging in the air
v1.exe --background --dig-test      # every bite comes out of ground that was there
v1.exe --background --clip-test     # no creature stands inside a solid
v1.exe --background --fell-test     # a felled tree comes apart and settles
v1.exe --background --kill-test     # every species dies correctly and drops what it should
```

`--background` is not optional in a script. It opens the window straight to the taskbar
and refuses the cursor, so a test run cannot take the screen away from whoever is at the
keyboard.

Pin the world with `--spawn <n>` when you are comparing two runs. The spawn point is
random per launch and a 54% swing in anything measured is usually that, not your change.

## House style

The thing to match is the **comments**. This codebase explains *why*, not *what*, and it
records what was measured and rejected as well as what shipped — so that nobody spends a
weekend rediscovering that an idea was already tried twice. If you change something
because a measurement told you to, put the measurement in the comment.

Small, focused PRs. One idea each.

## Reporting a bug instead

Open an issue. Useful things to include:

- what you were doing, and what happened
- your GPU and driver version
- **`v2-crash.log`**, if the game closed on its own. It sits beside the exe — in
  `%LOCALAPPDATA%\voxelbit\<build>\app\` for the packaged game — and holds the exception,
  the faulting module and offset, a stack, and what the engine was doing at the time.
  That file is the difference between a fixable report and a guess.
- the console output, if you launched from `run.bat`; the window is minimised in the
  taskbar rather than closed.
