"""Build voxelbit.exe -- the whole game as one file a player can double-click.

    python tools/package.py                 stage, compress, write voxelbit.exe
    python tools/package.py --check         is the shipped exe older than the engine?
    python tools/package.py --stage-only    just lay out dist/stage and stop
    python tools/package.py --no-compress   store everything (fast, for testing)
    python tools/package.py --zip           write voxelbit.zip instead of the exe
    python tools/package.py --installer     write voxelbit.exe (Inno Setup) -- THE SHIPPED ONE

---------------------------------------------------------------------------
WHY --check EXISTS, AND WHY THIS DOES NOT RUN ITSELF
---------------------------------------------------------------------------

voxelbit.exe is a SNAPSHOT. It does not track the engine, and a stale one
is the worst kind of wrong: it launches, it plays, and it is last week's build
-- so a bug you just fixed is still there and the next hour goes into chasing a
ghost.

The obvious answer is to repackage at the end of every build. That is a bad
trade and the numbers say so: packaging reads 530 MB, writes 780 MB and takes
minutes, against a v1.exe relink of a few seconds. Bolted onto build.bat it
would make every one-line iteration cost a full package.

So the exe is repackaged DELIBERATELY, and the tree tells you when it is
overdue instead: --check compares its mtime against v1.exe and the engine's
sources, build.bat prints one line when it has just made the exe stale, and
CLAUDE.md carries the rule about when it must be run.

---------------------------------------------------------------------------
WHAT IT PRODUCES
---------------------------------------------------------------------------

    dist/stage/app/     the engine: v1.exe, its DLLs, plugins, shaders, data
    dist/stage/data/    the content, in the SAME SHAPE AS THE REPOSITORY
    voxelbit.exe        launcher/launcher.exe + that tree, compressed,
                        at the repository root beside the run script

The shape of `data/` is load-bearing and not an accident. Every asset path in
the engine goes through `asset()` in engine/src/core/assetroot.h, which is a
repository-relative path joined onto a root -- so `data/game/assets/...` and
`data/engine/assets/dem/...` mean a path that works in the dev tree works in the
shipped game with no translation table in between. See that header.

---------------------------------------------------------------------------
WHAT IS DELIBERATELY LEFT OUT
---------------------------------------------------------------------------

The build directory is 2 GB and most of it is not a game:

  * .pdb -- 928 MB of debug symbols. The crash reporter (engine/src/platform/
    crashlog.h) resolves what it can without them and names the module and
    offset regardless, which is what a bug report actually needs.
  * Falcor's own sample executables -- Mogwai, HelloDXR, FalcorTest and six
    more. None of them is voxelbit.

WHAT LOOKED DROPPABLE AND IS NOT. pythondist/ is 126 MB of CPython standard
library and the first cut left it out, on the reasoning that the game has no
scripting. Falcor initialises an embedded interpreter during device creation
whether anybody scripts or not, so the packaged build died before its first
frame with

    Fatal Python error: init_fs_encoding: failed to get the Python codec
    ModuleNotFoundError: No module named 'encodings'

which names Python and not Falcor, and would have reached a player as "it just
closes". It ships.

Everything else in build/bin/Release ships, DLLs included. Trimming further is
possible but has to be MEASURED, one piece at a time, against a run that gets
past device creation -- a Falcor plugin that fails to load takes a render pass
with it, quietly, and the python directory proves the reasoning-from-first-
principles version of this does not work.
"""

import ctypes
import ctypes.wintypes as wt
import hashlib
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENGINE = os.path.join(ROOT, 'engine')
RELEASE = os.path.join(ENGINE, 'build', 'bin', 'Release')
DIST = os.path.join(ROOT, 'dist')
STAGE = os.path.join(DIST, 'stage')
# -- THE INSTALLER OWNS THE PLAIN NAME NOW (user 2026-09-21) --------------
#
# It used to be the self-extractor's, and the two would have fought over it:
# `package.py` writes OUT_EXE and `package.py --installer` writes OUT_SETUP, so
# leaving both pointed at voxelbit.exe means whichever ran last is the download
# -- and one of them is the build Defender deletes.
#
# So the SFX keeps its own name and the shipped artifact keeps the plain one.
# build_exe is untouched and still works; it is simply no longer what a player
# gets, and will not be again until there is a certificate to sign it with.
OUT_EXE = os.path.join(ROOT, 'voxelbit-sfx.exe')
OUT_ZIP = os.path.join(ROOT, 'voxelbit.zip')
# IN website/, NOT AT THE ROOT (user 2026-09-22). That is where index.html
# links to it from, and keeping one copy is what stops the two drifting -- see
# the OutputDir note in voxelbit.iss, which has to agree with this.
OUT_SETUP = os.path.join(ROOT, 'website', 'voxelbit.exe')
ISS = os.path.join(ROOT, 'tools', 'voxelbit.iss')

# ---------------------------------------------------------------------------
# THE CONTENT MANIFEST.
#
# Every entry is (repo-relative source, stage-relative destination). The nine
# terrain files are named ONE BY ONE rather than copied as a directory: that
# directory also holds the 1 m and 3 m lidar experiments and two abandoned
# windows, which together are another gigabyte the game never opens. The names
# below are exactly the ones engine/src/platform/main.cpp and app.h ask for.
# ---------------------------------------------------------------------------
DEM = [
    'rmnp50.vbdem', 'rmnp50.vbcov',            # the default world
    'acadia10.vbdem', 'acadia10.vbcov',        # --acadia, the birch wood
    'ouachita12.vbdem', 'ouachita12.vbcov',    # --ouachita, the oak wood
    'front60.vbdem', 'front60.vbcov',          # --front
    'deathvalley50.vbdem',                     # the desert
]

CONTENT = [
    # THE NOTICES TRAVEL WITH THE BINARY, and that is not a nicety: the cloud
    # shader is MIT and its licence requires the notice to accompany
    # substantial portions of the work "in binary form" too. Unpacked beside
    # the game so a player has it without going to the repository. See
    # section 6 of the licence itself, which names what voxelbit.exe carries.
    ('license', 'license.txt'),
    ('readme.md', 'readme.txt'),
    ('game/assets', 'data/game/assets'),
    ('game/sound', 'data/game/sound'),
    ('game/3x3-pixel.otf', 'data/game/3x3-pixel.otf'),
    ('game/logo.png', 'data/game/logo.png'),
    ('game/logo.ico', 'data/game/logo.ico'),
    # The one file the engine reads out of the authoring tree -- the level's
    # light bulb. See World::kLevelBulbVox.
    ('source/wip/technology/lightbulb.vox', 'data/source/wip/technology/lightbulb.vox'),
] + [('engine/assets/dem/' + f, 'data/engine/assets/dem/' + f) for f in DEM]

SKIP_EXES = {
    'CudaInterop.exe', 'FalcorTest.exe', 'HelloDXR.exe', 'ImageCompare.exe', 'Mogwai.exe',
    'MultiSampling.exe', 'RenderGraphEditor.exe', 'SampleAppTemplate.exe', 'ShaderToy.exe',
    'Visualization2D.exe',
}
# THE MEASURED TRIM (2026-09-24, "downloads are very slow"). The installer was
# 551 MB, over the 512 MiB (536,870,912 bytes) Cloudflare's free plan will
# cache, so voxelbit.net served every download from the origin. Both files
# below are imported by nothing (dumpbin /dependents over every exe, dll and
# pyd in the build) and a run with them hidden came up with Ray Reconstruction
# ready, frame generation 2x and a normal frame:
#   nvngx_dlss.dll  56 MB  DLSS SUPER RESOLUTION. The game upscales with Ray
#                          Reconstruction (nvngx_dlssd.dll); streamline.h only
#                          ASKS whether SR is supported, for a report line.
#   usd_ms.dll      14 MB  Falcor's USD runtime; no USD importer is built.
SKIP_FILES = {'nvngx_dlss.dll', 'usd_ms.dll'}
# ...and usd/ is that runtime's plugin data.
SKIP_DIRS = {'usd'}


def human(n):
    for unit in ('B', 'KB', 'MB', 'GB'):
        if n < 1024 or unit == 'GB':
            return '%.1f %s' % (n, unit)
        n /= 1024.0


# ---------------------------------------------------------------------------
# STAGING
# ---------------------------------------------------------------------------

def stage():
    if os.path.isdir(STAGE):
        shutil.rmtree(STAGE)
    app = os.path.join(STAGE, 'app')
    os.makedirs(app)

    if not os.path.exists(os.path.join(RELEASE, 'v1.exe')):
        sys.exit('package: no v1.exe in %s -- build it first with engine/build.bat' % RELEASE)

    copied = 0
    for dirpath, dirnames, filenames in os.walk(RELEASE):
        rel = os.path.relpath(dirpath, RELEASE)
        top = rel.split(os.sep)[0]
        if top in SKIP_DIRS:
            dirnames[:] = []
            continue
        for name in filenames:
            if name.endswith('.pdb') or name in SKIP_EXES or name in SKIP_FILES:
                continue
            # WHAT THE ENGINE WRITES BESIDE ITSELF WHEN IT RUNS, never ship it.
            # (2026-09-24, "downloads are very slow".) The build folder is also
            # where every dev run leaves its crash minidumps (v2-crash-*.dmp,
            # 8-11 MB each) and Falcor its per-run v1.exe.N.log: 97 MB of dumps
            # and 651 logs were going into the installer. That pushed it past
            # 512 MiB, the largest file Cloudflare's free plan will cache, so
            # every download came from the origin server instead of an edge
            # near the player -- and a minidump is this machine's memory.
            if name.endswith('.dmp') or name.endswith('.log'):
                continue
            if name.endswith('.exe.manifest') and name[:-9] in SKIP_EXES:
                continue
            src = os.path.join(dirpath, name)
            dst = os.path.join(app, rel, name) if rel != '.' else os.path.join(app, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            copied += os.path.getsize(src)
    print('  app      %s' % human(copied))

    content = 0
    for src_rel, dst_rel in CONTENT:
        src = os.path.join(ROOT, src_rel)
        dst = os.path.join(STAGE, dst_rel)
        if not os.path.exists(src):
            sys.exit('package: missing %s -- the manifest and the tree disagree' % src_rel)
        if os.path.isdir(src):
            shutil.copytree(src, dst)
            for dp, _, fn in os.walk(dst):
                content += sum(os.path.getsize(os.path.join(dp, f)) for f in fn)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            shutil.copy2(src, dst)
            content += os.path.getsize(src)
    print('  content  %s' % human(content))
    return copied + content


# ---------------------------------------------------------------------------
# COMPRESSION -- Windows' own, through Cabinet.dll.
#
# The launcher decompresses with the SAME API, so there is no third-party
# packer to install on this machine and no runtime to ship on the player's.
# XPRESS_HUFF rather than LZMS: it is the one whose buffer semantics are
# identical on both sides of the ctypes/C++ line, and on this payload -- voxel
# art and float terrain -- it gives up little.
# ---------------------------------------------------------------------------
COMPRESS_ALGORITHM_XPRESS_HUFF = 4
CHUNK = 8 << 20
STORED_BIT = 0x80000000

cabinet = ctypes.WinDLL('Cabinet.dll')
cabinet.CreateCompressor.argtypes = [wt.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
cabinet.CreateCompressor.restype = wt.BOOL
cabinet.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p,
                             ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
cabinet.Compress.restype = wt.BOOL
cabinet.CloseCompressor.argtypes = [ctypes.c_void_p]


class Compressor(object):
    def __enter__(self):
        self.h = ctypes.c_void_p()
        if not cabinet.CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF, None,
                                        ctypes.byref(self.h)):
            raise OSError('CreateCompressor failed: %d' % ctypes.get_last_error())
        return self

    def __exit__(self, *a):
        cabinet.CloseCompressor(self.h)

    def chunk(self, data):
        """Returns (bytes, stored). Never larger than the input."""
        buf = ctypes.create_string_buffer(len(data) + 4096)
        out = ctypes.c_size_t(0)
        ok = cabinet.Compress(self.h, data, len(data), buf, len(buf), ctypes.byref(out))
        if not ok or out.value >= len(data):
            return data, True
        return buf.raw[:out.value], False


# ---------------------------------------------------------------------------
# THE ARCHIVE, APPENDED TO THE LAUNCHER.
#
#   [launcher.exe][chunk data ...][index][trailer]
#
# The trailer is last and fixed-width so the launcher can seek to it from the
# END of its own file -- it has no other way to know where its payload begins,
# because the PE header at the front says nothing about anything appended to
# it. Format is mirrored in launcher/launcher.cpp; change one and change both.
# ---------------------------------------------------------------------------

def build_exe(compress=True):
    launcher = os.path.join(ROOT, 'launcher', 'launcher.exe')
    if not os.path.exists(launcher):
        sys.exit('package: no launcher/launcher.exe -- build it with launcher/build.bat')

    files = []
    for dirpath, _, filenames in os.walk(STAGE):
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            files.append((full, os.path.relpath(full, STAGE).replace('\\', '/')))
    files.sort(key=lambda t: t[1])

    # THE STAMP IS THE IDENTITY OF THIS BUILD, and it is what the launcher
    # names its install directory after -- so a new build unpacks beside the
    # old one instead of over it, and an unchanged one starts instantly. Hashed
    # from the relative paths, sizes and mtimes rather than the contents: a
    # gigabyte of SHA-256 would add a minute to every package for a value only
    # ever compared against itself.
    h = hashlib.sha256()
    for full, rel in files:
        st = os.stat(full)
        h.update(rel.encode('utf-8'))
        h.update(struct.pack('<QQ', st.st_size, int(st.st_mtime)))
    stamp = h.hexdigest()[:16].encode('ascii')

    out_path = OUT_EXE
    total_raw = sum(os.path.getsize(f) for f, _ in files)
    done = 0
    t0 = time.time()

    with open(out_path, 'wb') as out:
        with open(launcher, 'rb') as lf:
            shutil.copyfileobj(lf, out, 1 << 20)

        index = [struct.pack('<I', len(files))]
        with Compressor() as comp:
            for full, rel in files:
                offset = out.tell()
                raw = os.path.getsize(full)
                chunks = []
                with open(full, 'rb') as f:
                    while True:
                        block = f.read(CHUNK)
                        if not block:
                            break
                        if compress:
                            data, stored = comp.chunk(block)
                        else:
                            data, stored = block, True
                        out.write(data)
                        chunks.append((len(data) | (STORED_BIT if stored else 0), len(block)))
                        done += len(block)
                        if time.time() - t0 > 1.0:
                            t0 = time.time()
                            pct = 100.0 * done / max(1, total_raw)
                            sys.stdout.write('\r  packing  %5.1f%%  %s' % (pct, rel[:48]))
                            sys.stdout.flush()
                p = rel.encode('utf-8')
                e = [struct.pack('<H', len(p)), p,
                     struct.pack('<QQI', offset, raw, len(chunks))]
                for comp_size, raw_size in chunks:
                    e.append(struct.pack('<II', comp_size, raw_size))
                index.append(b''.join(e))

        index_blob = b''.join(index)
        index_off = out.tell()
        out.write(index_blob)
        out.write(b'VBPK0001')
        out.write(struct.pack('<QQ', index_off, len(index_blob)))
        out.write(stamp)

    sys.stdout.write('\r' + ' ' * 78 + '\r')
    size = os.path.getsize(out_path)
    print('  files    %d' % len(files))
    print('  raw      %s' % human(total_raw))
    print('  exe      %s   (%.0f%% of raw)' % (human(size), 100.0 * size / max(1, total_raw)))
    print('  stamp    %s' % stamp.decode())
    print('\n  %s' % out_path)


# ---------------------------------------------------------------------------
# THE SAME TREE AS A PLAIN ZIP.
#
# WHY THIS EXISTS. The exe above is a launcher stub with the game appended
# after its PE image, unpacked at runtime and executed -- which is, structurally,
# exactly what a dropper does. Defender's ML classifier scores the shape and
# calls it Trojan:Win32/Sabsik.FL.A!ml, and on default Windows settings that is
# not a warning, it is a deletion. A zip has no PE file of its own for a
# classifier to score, so there is nothing left to flag.
#
# THE LAYOUT IS NOT THE STAGE TREE'S. Staging puts the engine in app/ and the
# content in data/ as siblings, and the launcher bridges them by setting
# VOXELBIT_DATA before it starts the game (see launcher.cpp). A zip has no
# launcher to do that, so the layout has to satisfy the engine on its own:
# core/assetroot.h resolves its root by looking for `data/game/assets` BESIDE
# the executable first, so app/ is flattened to the top of the archive and
# data/ lands next to it. Extract, double-click, no environment to set.
#
#     voxelbit/voxelbit.exe     (app/v1.exe, renamed -- see below)
#     voxelbit/Falcor.dll, shaders/, plugins/ ...
#     voxelbit/data/game/assets/ ...
#
# AND v1.exe IS RENAMED. Nothing at runtime reads its own filename -- Falcor
# finds the shaders and the DLSS blobs through getRuntimeDirectory, which is a
# path, not a name -- and "v1.exe" is an internal target name that no player
# should have to recognise as the game.
#
# ONE FOLDER AT THE TOP, so extracting into Downloads lays down one directory
# rather than spraying eight hundred megabytes across it.
# ---------------------------------------------------------------------------

def build_zip(compress=True):
    import zipfile

    files = []
    for dirpath, _, filenames in os.walk(STAGE):
        for name in sorted(filenames):
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, STAGE).replace('\\', '/')
            files.append((full, rel))
    files.sort(key=lambda t: t[1])
    if not files:
        sys.exit('package: nothing staged -- run without --stage-only first')

    def arcname(rel):
        """stage-relative -> archive path. app/ flattens, data/ stays put."""
        if rel == 'app/v1.exe':
            return 'voxelbit/voxelbit.exe'
        if rel.startswith('app/'):
            return 'voxelbit/' + rel[4:]
        return 'voxelbit/' + rel

    total_raw = sum(os.path.getsize(f) for f, _ in files)
    done = 0
    t0 = time.time()
    mode = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED

    # allowZip64 IS REQUIRED AND NOT A PRECAUTION: the staged tree is over
    # 4 GB uncompressed across ~10k entries, which is past every limit the
    # original format has.
    with zipfile.ZipFile(OUT_ZIP, 'w', mode, allowZip64=True,
                         compresslevel=6 if compress else None) as z:
        for full, rel in files:
            z.write(full, arcname(rel))
            done += os.path.getsize(full)
            if time.time() - t0 > 1.0:
                t0 = time.time()
                pct = 100.0 * done / max(1, total_raw)
                sys.stdout.write('\r  zipping  %5.1f%%  %s' % (pct, rel[:48]))
                sys.stdout.flush()

    sys.stdout.write('\r' + ' ' * 78 + '\r')
    size = os.path.getsize(OUT_ZIP)
    print('  files    %d' % len(files))
    print('  raw      %s' % human(total_raw))
    print('  zip      %s   (%.0f%% of raw)' % (human(size), 100.0 * size / max(1, total_raw)))
    print('\n  %s' % OUT_ZIP)


# ---------------------------------------------------------------------------
# THE SAME TREE AS AN INSTALLER.
#
# The decisions that matter are in tools/voxelbit.iss, not here -- above all
# that app/ is flattened into the install directory and data/ lands beside the
# exe, because assetroot.h resolves its root by looking for `data/game/assets`
# next to the binary and an installer sets no VOXELBIT_DATA to help it.
#
# This is only the part that finds the compiler and runs it. ISCC is not on
# PATH after a default install and Inno 6 installs PER-USER by default, so
# Program Files is the wrong place to look first.
# ---------------------------------------------------------------------------

def find_iscc():
    cands = [
        os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Programs', 'Inno Setup 6', 'ISCC.exe'),
        os.path.join(os.environ.get('ProgramFiles(x86)', ''), 'Inno Setup 6', 'ISCC.exe'),
        os.path.join(os.environ.get('ProgramFiles', ''), 'Inno Setup 6', 'ISCC.exe'),
    ]
    for c in cands:
        if c and os.path.exists(c):
            return c
    found = shutil.which('ISCC')
    if found:
        return found
    sys.exit('package: Inno Setup not found -- install it from jrsoftware.org/isdl.php')


def build_installer():
    iscc = find_iscc()
    if not os.path.exists(ISS):
        sys.exit('package: no %s' % ISS)
    if not os.path.exists(os.path.join(STAGE, 'app', 'v1.exe')):
        sys.exit('package: nothing staged -- run without --stage-only first')

    print('  compiler %s' % iscc)
    print('  script   %s' % os.path.relpath(ISS, ROOT))
    print('  (lzma2/max over 1.3 GB -- this takes tens of minutes)')
    sys.stdout.flush()

    t0 = time.time()
    # cwd is the script's own directory: every Source in the .iss is written
    # relative to tools/, which is what keeps the paths readable there.
    rc = subprocess.call([iscc, ISS], cwd=os.path.dirname(ISS))
    if rc != 0:
        sys.exit('package: ISCC failed with %d' % rc)

    size = os.path.getsize(OUT_SETUP)
    print()
    print('  setup    %s' % human(size))
    print('  built in %.0f s' % (time.time() - t0))
    print()
    print('  %s' % OUT_SETUP)


# ---------------------------------------------------------------------------
# IS THE SHIPPED EXE OLDER THAN WHAT IT WAS BUILT FROM?
#
# mtimes, not hashes: a hash would mean staging the whole tree to answer a
# question that is asked in passing, and mtime is exactly the right resolution
# for "has somebody built since this was packaged". Returns 0 for fresh, 1 for
# stale, 2 for absent -- so build.bat and a hook can both act on it without
# parsing the text.
# ---------------------------------------------------------------------------
def check():
    # THE INSTALLER, NOT THE SFX. This answers "is the download stale", and the
    # download is what --installer builds; pointing it at the retired
    # self-extractor would report on a file nobody is given.
    out = OUT_SETUP
    if not os.path.exists(out):
        print('package: voxelbit.exe does not exist yet')
        return 2
    packed = os.path.getmtime(out)
    newest, what = 0.0, None
    cands = [os.path.join(RELEASE, 'v1.exe')]
    for root, _, files in os.walk(os.path.join(ENGINE, 'src')):
        cands += [os.path.join(root, f) for f in files]
    for root, _, files in os.walk(os.path.join(ENGINE, 'shaders')):
        cands += [os.path.join(root, f) for f in files]
    for c in cands:
        try:
            t = os.path.getmtime(c)
        except OSError:
            continue
        if t > newest:
            newest, what = t, c
    if newest > packed:
        print('package: voxelbit.exe is STALE -- %s is newer' %
              os.path.relpath(what, ROOT))
        print('         refresh it with:  python tools/package.py --installer')
        return 1
    print('package: voxelbit.exe is up to date')
    return 0


# ---------------------------------------------------------------------------
# THE STAGE TREE IS SCRATCH AND DOES NOT OUTLIVE THE RUN.
#
# (user 2026-09-22: "cant you delete the dist folder? I thought we already
#  deleted it? do we need it? if we dont need it, remove it and stop
#  regenerating it.")
#
# IT WAS 1.4 GB OF NOTHING ANYBODY NEEDS. dist/ holds one thing -- stage/ --
# and stage() opens by rmtree'ing it and laying the whole tree out again from
# the repository, so nothing in it is ever read across two runs. The shipped
# artefacts do not live there either: OUT_EXE, OUT_ZIP and OUT_SETUP are all
# written to the repository ROOT. So the directory was a byproduct that
# happened to persist, and persisting is the only thing it did.
#
# IT CANNOT SIMPLY NOT EXIST. Both writers walk a laid-out tree -- the SFX
# packer to build its archive, ISCC because the .iss names stage paths -- so
# there has to be somewhere to lay it out. What changes is that it is cleaned
# up afterwards rather than left sitting in the tree.
#
# --stage-only IS THE EXCEPTION AND THAT IS ITS WHOLE PURPOSE: "just lay out
# dist/stage and stop". A flag that asks for the tree gets the tree.
#
# BEST EFFORT ON THE WAY OUT. A half-gigabyte rmtree can lose a race with a
# virus scanner or an open handle, and a packaging run that SUCCEEDED must not
# report failure because it could not tidy up after itself.
def sweep():
    if not os.path.isdir(DIST):
        return
    try:
        shutil.rmtree(DIST)
        print('  cleaned  %s' % os.path.relpath(DIST, ROOT))
    except OSError as e:
        print('  note: could not remove %s (%s)' % (DIST, e))


def main():
    args = sys.argv[1:]
    if '--check' in args:
        sys.exit(check())
    print('voxelbit: packaging')
    stage()
    if '--stage-only' in args:
        print('  staged at %s' % STAGE)
        print('  (left in place because --stage-only asked for it; '
              'an ordinary run sweeps it)')
        return
    try:
        if '--installer' in args:
            build_installer()
        elif '--zip' in args:
            build_zip(compress='--no-compress' not in args)
        else:
            build_exe(compress='--no-compress' not in args)
    finally:
        # IN A finally, so a run that dies half way through compressing does
        # not leave the tree behind either. The artefact is already written by
        # this point on the success path, and on the failure path there is
        # nothing in stage/ worth keeping -- stage() rebuilds it from scratch.
        sweep()


if __name__ == '__main__':
    main()
