#!/bin/sh
# voxelbit rebuild-engines -- put the native builds back in step after a pull.
#
# The browser game rebuilds itself: game/index.html is a build artifact and the
# pre-commit and post-merge hooks regenerate it from src/ so a stale bundle can
# never reach anyone. The NATIVE engines had no equivalent, and the failure they
# produce is worse, because nothing reports it:
#
#   * A pull changes v7/shaders/Trace.cs.slang. The exe on disk is unchanged and
#     still runs -- Falcor compiles shaders at startup, so the picture silently
#     becomes a mixture of last week's C++ and today's shader. If a binding was
#     added on either side, the dispatch fails with an unbound resource and the
#     message names a shader that looks perfectly correct on disk.
#   * A pull DELETES a shader. The build system only ever COPIES shaders into
#     build/bin/Release/shaders/, so the deleted file stays there forever. It is
#     inert, but it means the runtime tree is not what the source says it is,
#     and it is the first thing to mislead you when a shader misbehaves.
#
# So: after a merge, work out which engines the merge actually touched, prune
# any shader in their runtime tree that no longer has a source file, and build
# them. Nothing else -- an engine the pull did not touch is not rebuilt.
#
# ---------------------------------------------------------------------------
# THIS NEVER FAILS A PULL, and that is deliberate. The merge has already
# happened by the time a post-merge hook runs, so there is nothing to refuse;
# refusing would only leave the tree merged and the caller confused. Every path
# here reports and returns 0. A build that fails says so, loudly, and leaves the
# previous exe alone -- which is exactly what you want, because that exe is the
# one you were using five minutes ago.
#
# BUILD.BAT, NEVER REBUILD.BAT. Every rebuild.bat in this repo ends in `pause`;
# it exists to be double-clicked, so the window stays open long enough to read.
# Called from a hook it waits for a keypress that is never coming, and the pull
# hangs with no output and no clue why.
#
# A RUNNING ENGINE IS SKIPPED, not killed and not built around. The linker
# cannot replace an exe that is mapped into a live process -- it stops with
# LNK1104 and no explanation of which of the two hundred files it meant -- and
# killing a window somebody is standing in is not a hook's decision to make.
#
# Skip the whole thing with:  VOXELBIT_NO_AUTOBUILD=1 git pull
# Run it by hand with:        sh tools/rebuild-engines.sh [engine ...]
# ---------------------------------------------------------------------------
cd "$(git rev-parse --show-toplevel)" 2>/dev/null || exit 0

[ -n "$VOXELBIT_NO_AUTOBUILD" ] && { echo "rebuild-engines: skipped (VOXELBIT_NO_AUTOBUILD)"; exit 0; }

# Where each engine's exe lands. Used for two things: the name of the process to
# check for, and proof that the engine has ever been built at all -- there is no
# point spending four minutes on a first build of something this pull merely
# grazed, so an engine with no exe is left for its own build.bat.
engine_exe() {
    case "$1" in
        v2) echo "v2/build/v2.exe" ;;
        v5) echo "v5/target/release/v5.exe" ;;
        *)  echo "$1/build/bin/Release/$1.exe" ;;
    esac
}

# The Falcor engines copy shaders/ into their runtime tree at build time. v2 is
# OptiX and v5 is Rust with its shaders compiled in, so neither has one.
engine_shader_dir() {
    case "$1" in
        v4|v6|v7) echo "$1/build/bin/Release/shaders/$1/shaders" ;;
        *) echo "" ;;
    esac
}

running() {
    # tasklist rather than pgrep: this is the Windows process table, and the
    # exe we care about is a Windows process whatever shell asks about it.
    command -v tasklist >/dev/null 2>&1 || return 1
    tasklist 2>/dev/null | grep -qi "^$1\.exe "
}

# ---------------------------------------------------------------------------
# WHICH ENGINES CHANGED.
#
# Arguments win, so this doubles as a hand-run tool. Otherwise the range is
# ORIG_HEAD..HEAD, which git sets for a merge AND for a fast-forward pull. With
# no ORIG_HEAD -- a fresh clone, or a hook invoked out of context -- there is no
# range to read and nothing is built, because "I cannot tell what changed" must
# never mean "rebuild everything".
# ---------------------------------------------------------------------------
ALL="v2 v4 v5 v6 v7"
if [ $# -gt 0 ]; then
    CHANGED="$*"
else
    if ! git rev-parse --verify --quiet ORIG_HEAD >/dev/null; then
        echo "rebuild-engines: no ORIG_HEAD, nothing to compare - skipped"
        exit 0
    fi
    PATHS=$(git diff --name-only ORIG_HEAD HEAD 2>/dev/null)
    [ -z "$PATHS" ] && exit 0
    CHANGED=""
    for e in $ALL; do
        if echo "$PATHS" | grep -q "^$e/"; then CHANGED="$CHANGED $e"; fi
    done
fi

[ -z "$(echo $CHANGED)" ] && exit 0

LOGDIR=".git/rebuild-logs"
mkdir -p "$LOGDIR" 2>/dev/null

FAILED=""
SKIPPED=""
BUILT=""

for e in $CHANGED; do
    [ -d "$e" ] || continue
    [ -f "$e/build.bat" ] || { echo "rebuild-engines: $e has no build.bat - skipped"; continue; }

    exe=$(engine_exe "$e")
    if [ ! -f "$exe" ]; then
        echo "rebuild-engines: $e has never been built - leaving it to you"
        SKIPPED="$SKIPPED $e"
        continue
    fi

    if running "$e"; then
        echo "rebuild-engines: $e is RUNNING - not rebuilt (the linker cannot replace a live exe)"
        echo "                 close it and run:  sh tools/rebuild-engines.sh $e"
        SKIPPED="$SKIPPED $e"
        continue
    fi

    # -- prune shaders the source no longer has ---------------------------
    #
    # Only inside the engine's OWN shader folder. The sibling nrd/shaders tree
    # beside it belongs to Falcor's denoiser and has no counterpart in this
    # repo at all, so measuring it against ours would delete all of it.
    sdir=$(engine_shader_dir "$e")
    if [ -n "$sdir" ] && [ -d "$sdir" ] && [ -d "$e/shaders" ]; then
        for f in "$sdir"/*.slang; do
            [ -e "$f" ] || continue
            b=$(basename "$f")
            if [ ! -f "$e/shaders/$b" ]; then
                rm -f "$f" && echo "rebuild-engines: $e pruned stale shader $b"
            fi
        done
    fi

    log="$LOGDIR/$e.log"
    echo "rebuild-engines: building $e ..."
    # cmd //c, not cmd /c: under MSYS a lone /c is mangled into a path before
    # cmd ever sees it, and cmd then reports a file it was never asked about.
    if (cd "$e" && cmd //c build.bat) >"$log" 2>&1; then
        echo "rebuild-engines: $e ok"
        BUILT="$BUILT $e"
    else
        echo "rebuild-engines: $e FAILED - the exe you had is untouched" >&2
        tail -n 15 "$log" | sed 's/^/    /' >&2
        echo "                 full log: $log" >&2
        FAILED="$FAILED $e"
    fi
done

[ -n "$(echo $BUILT)" ] && echo "rebuild-engines: rebuilt$BUILT"
[ -n "$(echo $FAILED)" ] && echo "rebuild-engines: FAILED$FAILED - see $LOGDIR" >&2

exit 0
