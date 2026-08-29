# Contributing to voxelbit

Fork it, branch, open a pull request. You do not need to be invited or to ask first.

Read this before you start, though — there are two things about this repo that will
otherwise waste your afternoon.

## 1. Never edit `game/index.html`

It is a **build artifact**. `tools/bundle.py` overwrites it wholesale from `src/`, so any
edit you make there is deleted the moment anyone runs the build, and a PR that touches it
alone changes nothing.

The source is **`src/`**, split into fragments and concatenated in the order given by
**`src/manifest.txt`**. Read that order as the dependency graph: a fragment may use
anything declared above it and nothing below it. [`docs/architecture.md`](docs/architecture.md)
says what each one owns.

## 2. The licence assigns your contribution to the project

voxelbit is source-available and commercial, not open source. Section 4 of
[LICENSE](LICENSE) means that by opening a pull request you assign your rights in that
contribution to the project.

That is a real thing to agree to, so it is stated here rather than buried. If you would
rather not, that is completely reasonable — open an issue describing the bug instead. A
good bug report is worth as much as a patch.

## The loop

```
start.bat                       # or: python tools/serve-nocache.py
```

Open http://127.0.0.1:8080/. The dev server builds `src/` in memory on every request, so
**edit a fragment and hit refresh** — no build step while you work.

## Before you open the PR

```
python tools/bundle.py     # regenerate game/index.html from src/
python tools/lint-vb.py    # must be clean
```

`lint-vb.py` catches a stale artifact and a few traps that are otherwise silent: a `//`
comment part-way into a dense one-liner (it eats the rest of the line), a duplicate
top-level `const`, and a backtick inside a WGSL comment (it ends the JS template literal
early and the game boots to a black screen).

If you have a GPU and a browser handy, `python tools/vbtest.py` boots the real game and
checks it against a baseline. It needs real WebGPU, so it will not run on a machine
without a GPU — say so in the PR and that is fine.

## House style

The thing to match is the **comments**. This codebase explains *why*, not *what*, and it
records what was measured and rejected as well as what shipped — so that nobody spends a
weekend rediscovering that an idea was already tried twice. If you change something
because a measurement told you to, put the measurement in the comment.

Small, focused PRs. One idea each.

## Reporting a bug instead

Open an issue. Useful things to include:

- what you were doing, and what happened
- your browser and GPU (`window.__vbAdapter` in the console prints the adapter actually in use)
- `__vb.mem()` and `__vb.prof()` if it is a performance problem
- anything in the console, especially lines starting `[vb]`
