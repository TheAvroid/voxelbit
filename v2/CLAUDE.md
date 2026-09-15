# v2 -- working here alongside another Claude tab

**More than one Claude session works this tree at once.** That is normal here
and it is the single most common cause of a failure that looks like a code bug
and is not. Read this before you build or run anything.

## Your session id

Your scratchpad path is

    C:\Users\mrwbh\AppData\Local\Temp\claude\c--Users-mrwbh-pbrt-v4\<uuid>\scratchpad

That `<uuid>` is your identity for everything below. Pass it as `-Session`.

## Build and run through the helper, not directly

```powershell
$S = '<your-uuid>'
$T = 'C:\voxelbit\v2\tools\claude-session.ps1'

& $T status -Session $S                    # who holds what, before you touch anything
& $T build  -Session $S                    # takes the build lock, then build.bat
& $T build  -Session $S -Wait              # ...and queue instead of giving up
& $T run    -Session $S -Args '--shot C:\...\out.png --shot-frame 2'
```

`build` takes a lock so two tabs cannot be inside ninja together. `run` forces
`--background` and **reaps the engine afterwards**, which is the thing that
actually keeps the peace.

## The three rules

**1. NEVER kill a v2.exe that is not yours.** `status` prints `YOURS` or
`OTHER` for each one; ownership is read out of the process command line,
because a render writes its `.png` into the owning tab's scratchpad. On
2026-09-14 a session killed two renders to clear its own build and they
belonged to the other tab. If a foreign engine blocks your link, wait or ask —
`build` refuses rather than killing, on purpose.

**2. Always reap your own.** A `--shot` run returns to the shell while the
engine is still alive. That survivor holds `Falcor.dll` and `v2.exe`, and the
next build — yours or theirs — dies on it. `run` does this for you; if you
launch the exe by hand, sweep it yourself.

**3. Build from PowerShell, never from Git Bash.** `build.bat` guards against a
running engine with `tasklist | find /I "v2.exe"`, and Git Bash puts its own
`find(1)` ahead of `C:\Windows\System32\find.exe`. The guard then fails with
`find: '/I': No such file or directory` and misfires — so you wait out a full
build and collect the bare `LNK1104` at the end that the guard exists to
prevent.

## What these failures look like

None of them names contention. All of them are it:

| symptom | cause |
|---|---|
| `LNK1104: cannot open file 'Falcor.dll'` / `'v2.exe'` | someone's engine is running |
| `C1083: cannot open compiler intermediate file ...pch: Invalid argument` | two builds in ninja at once |
| `ImportError: DLL load failed while importing falcor_ext` | a freshly linked binary is still held |
| `v2: build failed -- nothing was changed` | the guard caught it; nothing is broken |

A build that reports "nothing was changed" changed nothing. The tree is fine;
check `status` and try again.

## Verify a build really ran

Exit code is not enough — check `v2.exe`'s mtime moved:

```powershell
Get-Item C:\voxelbit\v2\build\bin\Release\v2.exe | Select-Object LastWriteTime
```

## Leave a note for the other tab

There is no message channel between sessions. If you are mid-way through
something another tab would trip over — a header half-edited, a long build
queued, a flag temporarily disabled for an A/B — write it in
`tools/IN-FLIGHT.md` and delete it when you are done. Check that file before
you start.
