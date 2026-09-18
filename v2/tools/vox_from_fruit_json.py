#!/usr/bin/env python3
"""Turn the browser engine's fruit BAKE into two plain .vox files v2 can load.

    "I want you to add apples and oranges to some of the trees in the oak
     forest. import the v1 mechanics of this."          - user, 2026-09-17

    python v2/tools/vox_from_fruit_json.py              show the plan
    python v2/tools/vox_from_fruit_json.py --apply      write the .vox files

WHY THIS READS A .json AND NOT THE ART.  The apple is a clean single-model file
but the orange is NOT: game/assets/food/orange.vox is a 117-model CULINARY PACK
(pineapple, carrot, peach, cookie, a cabinet...) and the orange is one shape
node inside it, found by its own _name.  tools/voxelize_fruit.py already solved
that once, off the scene graph's own labels, and the answer it wrote is
game/assets/decoration/fruit.json.  Re-solving it here would be a second copy of
a selection that is only correct by agreement with a file none of this can see.

So this is a FORMAT CONVERSION and deliberately nothing else.  Every shape
decision -- which node is the orange, where the flesh stops and the stem
begins, and the quantization of eleven flesh shades to one -- was made by that
bake and is carried through untouched.  If a fruit ever looks wrong, the bake is
where it is wrong.

WHAT THE BAKE ENCODES, and the one thing worth restating.  Each voxel is
`(z << 16) | (y << 8) | x` in the low 24 bits with a SLOT in the top byte:

    top byte 0        the fruit's FLESH   -- pal[0] apple, pal[1] orange
    top byte non-zero its STEM AND LEAF   -- pal[nbody], one green for both

Two slots rather than colours, because the browser engine's palette mints the
real ids long before this json is fetched.  v2 has no such ordering problem --
it loads models and mints as it goes -- so the slots are written out here as the
authored colours and v2's own loader does what it does with every other model.

THE LEAF IS DELIBERATELY LEFT GREEN-DOMINANT.  v2's loadModelSet replaces a
model's green-dominant colours with a material the caller names (`stemId`), and
the oak's own canopy id is what the fruit's leaf should wear -- exactly what
assets/bow.js does with `near(fj.pal[fj.nbody], OAKLEAF)`.  Writing the leaf as
some already-resolved brown here would take that choice away from the loader.

WHY THE FILES ARE SEPARATE.  One .vox per fruit, because loadModelSet takes a
list of paths and an apple and an orange are two models, not two frames of one.
A pack would put them back inside the problem this tool exists to stay out of.

Deterministic: re-running reproduces both files byte for byte.
"""

import json
import os
import struct
import sys

ROOT = 'C:/voxelbit'
SRC = os.path.join(ROOT, 'game', 'assets', 'decoration', 'fruit.json')
OUTDIR = os.path.join(ROOT, 'game', 'assets', 'decoration')


def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def write_vox(path, size, vox, palette):
    """One model, one RGBA chunk. `vox` is a list of (x, y, z, index-into-palette+1)."""
    sx, sy, sz = size
    assert sx <= 255 and sy <= 255 and sz <= 255, 'a fruit is small; this cannot happen'
    body = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body += chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
    # 256 entries, MagicaVoxel's layout: entry i is palette index i + 1, so the
    # table is written from index 1 and slot 0 of the file is never addressed.
    rgba = bytearray(256 * 4)
    for i, (r, g, b) in enumerate(palette):
        rgba[i * 4:i * 4 + 4] = bytes((r, g, b, 255))
    body += chunk(b'RGBA', bytes(rgba))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    apply_ = '--apply' in sys.argv[1:]
    if not os.path.exists(SRC):
        print('missing: %s' % SRC)
        return 1
    fj = json.load(open(SRC))
    pal, nbody = fj['pal'], fj['nbody']
    leaf = tuple(pal[nbody])

    print('fruit.json: %d fruit, %d body colours, leaf %s'
          % (len(fj['fruit']), nbody, leaf))
    print()
    hdr = '%-8s %-12s %7s %7s %6s  %s'
    print(hdr % ('name', 'size (vox)', 'flesh', 'stem', 'total', 'flesh colour'))

    for i, f in enumerate(fj['fruit']):
        flesh = tuple(pal[i])
        out = []
        nf = ns = 0
        for p in f['vox']:
            x, y, z = p & 255, (p >> 8) & 255, (p >> 16) & 255
            # THE TOP BYTE IS A SLOT, NOT A COLOUR -- see the note above.
            if (p >> 24) & 255:
                out.append((x, y, z, 2))  # stem and leaf -> palette entry 2
                ns += 1
            else:
                out.append((x, y, z, 1))  # flesh -> palette entry 1
                nf += 1
        size = (f['sx'], f['sy'], f['sz'])
        print(hdr % (f['name'], '%dx%dx%d' % size, nf, ns, len(out),
                     '%d,%d,%d' % flesh))
        if apply_:
            path = os.path.join(OUTDIR, 'fruit_%s.vox' % f['name'])
            write_vox(path, size, out, [flesh, leaf])
            print('    -> %s' % path)

    # A FRUIT IS 0.1 m TO A VOXEL IN BOTH ENGINES, which is why nothing is
    # rescaled here: the browser engine's ice-cap note measures 4 voxels at
    # 40 cm and v2's VOXEL_M is 0.1f. The apple ships the size it was authored.
    print()
    print('at 0.1 m per voxel: %s' % ', '.join(
        '%s %.2f x %.2f x %.2f m' % (f['name'], f['sx'] * 0.1, f['sy'] * 0.1, f['sz'] * 0.1)
        for f in fj['fruit']))
    if not apply_:
        print('measure only -- pass --apply to write the .vox files')
    return 0


if __name__ == '__main__':
    sys.exit(main())
