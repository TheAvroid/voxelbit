"""
patch_gfx_interposer.py -- make slang-gfx load Streamline instead of D3D12.

WHY THIS EXISTS
---------------
DLSS Frame Generation needs Streamline to own the swapchain, and Streamline only
owns it if D3D12CreateDevice and CreateDXGIFactory2 resolve into
sl.interposer.dll. The documented way to arrange that is to link
sl.interposer.lib in place of d3d12.lib -- which v6 cannot do, because v6 does
not create the device. Falcor does, through a PREBUILT slang-gfx.dll.

Nor can the import table be redirected: gfx.dll imports nothing at all from
d3d12.dll or dxgi.dll. It loads them at runtime, from two bare base names stored
in its .rdata -- "d3d12" and "dxgi" -- to which it appends ".dll".

The obvious trick, dropping a copy of sl.interposer.dll beside the exe named
d3d12.dll, does not work: the interposer itself resolves the real d3d12.dll BY
THAT NAME, finds itself, and the process dies during device creation (exit 127,
or a segfault once slInit is involved).

So instead of changing which file answers to "d3d12", this changes what gfx.dll
ASKS FOR. The two base names are replaced with names of exactly the same length
-- "slp12" and "slgi". gfx.dll then loads Streamline, while Streamline's own
lookup of "d3d12.dll" still finds the real system one. The collision is gone.

Only ONE of the two is a copy of the interposer. slp12 is the interposer;
slgi is a FORWARDER built by build_slgi_forwarder.bat, because two copies would
be two module instances with separate state and the viewer segfaults during
swapchain creation. See the note beside the install step below.

REVERSIBILITY
-------------
The original is saved beside it as gfx.dll.v6-backup before anything is written,
and --restore puts it back. Nothing else in the tree is touched, so restoring
returns v6 to a stock Falcor exactly.

Run with --restore to undo. Run with no arguments to apply.
"""
import io
import os
import shutil
import sys

BUILD = r'C:\voxelbit\v6\build\bin\Release'
GFX = os.path.join(BUILD, 'gfx.dll')
BACKUP = GFX + '.v6-backup'
INTERPOSER = r'C:\Users\mrwbh\Streamline\bin\x64\sl.interposer.dll'
# Built by build_slgi_forwarder.bat. See below for why a COPY will not do.
FORWARDER = r'C:\voxelbit\v6\build\slgi\slgi.dll'

# (what gfx asks for now, what it should ask for). Lengths MUST match: these are
# null-terminated strings packed in .rdata, and a longer replacement would run
# into whatever follows.
RENAMES = [(b'd3d12', b'slp12'), (b'dxgi', b'slgi')]
SHIMS = ['slp12.dll', 'slgi.dll', 'sl.interposer.dll']


# Characters that can appear inside a C or C++ identifier, including the '@'
# that MSVC name mangling uses as a separator.
_IDENT = set(b'abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@')


def find_exact(data, name):
    """Offsets where `name` is a module base name in its own right.

    A plain substring search is far too eager. In this binary "d3d12" occurs 41
    times: once as the module name, once inside the message
    "error: failed load 'd3d12.dll'", and thirty-nine times inside mangled C++
    symbols like "?createBufferImpl@d3d12@gfx@@". Patching any of the others
    would corrupt a diagnostic string or an entry point name.

    Two conditions isolate the real one:

      followed by NUL      the name is a complete string, which rules out both
                           "d3d12.dll" (followed by '.') and every mangled
                           symbol (followed by '@').

      not preceded by an   rules out matching the TAIL of a longer identifier,
      identifier character which is how "Adxgidebug.dll" would otherwise look
                           like a match for "dxgi".

    Note that the module name is NOT preceded by a NUL -- it is packed directly
    against the binary data before it -- so requiring one, which is the obvious
    way to write this test, finds nothing at all and the patch refuses.
    """
    out = []
    i = data.find(name)
    while i != -1:
        end = i + len(name)
        if end < len(data) and data[end] == 0 and (i == 0 or data[i - 1] not in _IDENT):
            out.append(i)
        i = data.find(name, i + 1)
    return out


def restore():
    if not os.path.exists(BACKUP):
        print('  nothing to restore -- no backup found')
        return 1
    shutil.copyfile(BACKUP, GFX)
    for s in SHIMS:
        p = os.path.join(BUILD, s)
        if os.path.exists(p):
            os.remove(p)
    print('  gfx.dll restored from backup, shims removed')
    print('  v6 is back to stock Falcor (no frame generation)')
    return 0


def apply():
    if not os.path.exists(GFX):
        print('  no gfx.dll at', GFX)
        return 1
    if not os.path.exists(INTERPOSER):
        print('  no sl.interposer.dll at', INTERPOSER)
        return 1

    # Always patch from the pristine original, so running twice is safe.
    if not os.path.exists(BACKUP):
        shutil.copyfile(GFX, BACKUP)
        print('  backed up gfx.dll ->', os.path.basename(BACKUP))
    data = bytearray(io.open(BACKUP, 'rb').read())

    for old, new in RENAMES:
        assert len(old) == len(new), 'replacement must be the same length'
        hits = find_exact(data, old)
        if len(hits) != 1:
            print(f'  expected exactly one standalone "{old.decode()}" string, found {len(hits)}')
            print('  refusing to patch -- gfx.dll is not the build this was written for')
            return 1
        data[hits[0]:hits[0] + len(old)] = new
        print(f'  {old.decode():6} -> {new.decode():6} at 0x{hits[0]:06x}')

    io.open(GFX, 'wb').write(bytes(data))

    # The interposer keeps its OWN NAME. Streamline locates its plugins and its
    # OTA cache from its module path, and it finds that path by looking for a
    # module called sl.interposer.dll. Renamed, it cannot -- "ota.cpp: Unable to
    # determine SL Interposer DLL path" -- and its plugins then load twice and
    # are thrown away as duplicates.
    shutil.copyfile(INTERPOSER, os.path.join(BUILD, 'sl.interposer.dll'))

    # BOTH SHIMS ARE THE SAME FORWARDER, and that is safe precisely because a
    # forwarder holds no state -- it is a name change, not an interposer.
    #
    # Windows keys loaded modules by PATH, so a second copy of sl.interposer.dll
    # under another name is a SECOND INTERPOSER with its own state. slInit runs
    # on slp12; the copy never receives it, and Streamline ends up with a
    # half-initialised second self underneath the DXGI factory. The result is
    # not a polite failure -- the process segfaults during swapchain creation,
    # before the engine prints a single line.
    #
    # This script previously copied the interposer to both names, which meant
    # every RE-RUN silently clobbered the forwarder built by
    # build_slgi_forwarder.bat and reintroduced the crash. That cost a long
    # detour into debugging slang-gfx, which was never at fault.
    if not os.path.exists(FORWARDER):
        print('  no forwarder at', FORWARDER)
        print('  run build_slgi_forwarder.bat first -- a copy of the interposer')
        print('  under that name crashes the viewer, so this refuses to install one.')
        return 1
    shutil.copyfile(FORWARDER, os.path.join(BUILD, 'slp12.dll'))
    shutil.copyfile(FORWARDER, os.path.join(BUILD, 'slgi.dll'))
    print('  installed: sl.interposer.dll (real), slp12.dll + slgi.dll (forwarders)')
    print('  gfx.dll now loads Streamline for device and factory creation')
    return 0


if __name__ == '__main__':
    print('v6: slang-gfx / Streamline interposer patch')
    sys.exit(restore() if '--restore' in sys.argv else apply())
