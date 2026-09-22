#!/usr/bin/env python3
"""Turn the browser engine's fruit BAKE into two plain .vox files v2 can load.

    "I want you to add apples and oranges to some of the trees in the oak
     forest. import the v1 mechanics of this."          - user, 2026-09-17
    "the apples stem is not brown but green."           - user, 2026-09-18

    python v1/tools/vox_from_fruit_json.py              show the plan
    python v1/tools/vox_from_fruit_json.py --apply      write the .vox files

WHY THIS READS A .json AND NOT THE ART.  The apple is a clean single-model file
but the orange is NOT: game/assets/food/orange.vox is a 117-model CULINARY PACK
(pineapple, carrot, peach, cookie, a cabinet...) and the orange is one shape
node inside it, found by its own _name.  tools/voxelize_fruit.py already solved
that once, off the scene graph's own labels, and the answer it wrote is
game/assets/decoration/fruit.json.  Re-solving it here would be a second copy of
a selection that is only correct by agreement with a file none of this can see.

So the SHAPE is a format conversion and deliberately nothing else.  Which node
is the orange, and where the flesh stops and the crown begins, were both decided
by that bake and are carried through untouched.

WHAT THE BAKE ENCODES.  Each voxel is `(z << 16) | (y << 8) | x` in the low 24
bits with a SLOT in the top byte:

    top byte 0        the fruit's FLESH   -- pal[0] apple, pal[1] orange
    top byte non-zero its CROWN           -- pal[nbody], one colour for all of it

Two slots rather than colours, because the browser engine's palette mints the
real ids long before this json is fetched.  v2 has no such ordering problem --
it loads models and mints as it goes -- so the slots are written out here as the
authored colours and v2's own loader does what it does with every other model.

-- ...AND WHY THE CROWN'S ONE COLOUR IS NOT GOOD ENOUGH ---------------------

The apple's crown is a STALK AND A BLADE, and the artist painted them
differently: apple/00.vox puts (143,95,74) -- a real brown -- at (1,1,3) and
(0,1,4), and three greens at (2,1,3), (3,1,3) and (2,1,4).  The bake pools all
five into one voxel-weighted mean, and three greens outvote two browns, so what
fruit.json carries is (171,178,100): an OLIVE.  Painted over the whole crown
that is a green stem, which is what the user saw and reported.

IT CANNOT BE RECOVERED FROM fruit.json BY GEOMETRY.  The browser engine measured
this when it hit the same bug (assets/bow.js, user 2026-08-17: "the apple doesnt
seem to have a brown stem ... the stem is currently green like the leaf on it"):
the five cells are one 26-connected blob and the brown (1,1,3) is FACE-adjacent
to the green (2,1,3), so no connectivity or adjacency rule separates them.

IT CAN BE RECOVERED FROM THE ART, so that is what happens below -- the same
answer bow.js reaches, by the same rule: every crown cell is looked up in the
model it was baked from and classified by its authored hue, `r > g` is the woody
stalk and anything else is the blade.  The split is the only thing taken from
the art; the geometry still comes from the bake.

WHICH MODEL IN THE ART, WITHOUT RE-SOLVING THE SELECTION.  The bake's own voxel
cells are the key: the art model whose cropped cells are EXACTLY the set
fruit.json carries for that fruit is the model it was baked from.  That is a
check rather than a guess, and it is what makes reading orange.vox safe -- a
pinned index has broken three times over there (117 models, then 7, then 1) and
a node name broke once, while a cell set cannot agree by accident.

EVERY FALLBACK LANDS ON BLADE.  No art file, no matching model, or a cell the
art does not know, and that cell is a blade -- exactly what shipped before.  A
re-authored fruit cannot come out wrong here, only unimproved, and the table
printed below says which of the two happened.

THE BLADE IS STILL LEFT GREEN-DOMINANT.  v2's loadModelSet replaces a model's
green-dominant colours with a material the caller names (`stemId`), and the
oak's own canopy id is what the fruit's blade should wear -- exactly what
assets/bow.js does with `near(fj.pal[fj.nbody], OAKLEAF)`.  Writing the blade as
some already-resolved id here would take that choice away from the loader.  The
STALK is deliberately NOT green-dominant, which is how it survives that same
pass and reaches forModelColor as a colour of its own.  See World::loadFruit.

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

# The ART each fruit was baked from, keyed by the name tools/voxelize_fruit.py
# wrote. ONE ENTRY PER FRUIT and not one shared model: the orange's three crown
# cells happen to be the apple's three blade cells today, so reading both out of
# apple/00.vox would answer correctly and for the wrong reason -- it would be the
# apple deciding what an orange's stalk is, and an orange re-authored with a
# stalk would quietly wear the apple's answer. (user 2026-08-17, to the browser
# engine: "do not use the apple model at all for the orange model".)
ART = {
    'apple': os.path.join(ROOT, 'game', 'assets', 'food', 'apple', '00.vox'),
    'orange': os.path.join(ROOT, 'game', 'assets', 'food', 'orange.vox'),
}

FLESH, BLADE, STALK = 1, 2, 3   # palette entries in the files this writes


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


def read_vox(path):
    """(models, palette) -- enough to look a baked cell up in the art it came from.

    Models are (size, [(x, y, z, palette index)]). The scene graph is NOT read:
    the caller identifies its model by the cells in it, which is the one key
    that survives a re-save of a pack. See the note at the head of this file.
    """
    d = open(path, 'rb').read()
    if d[:4] != b'VOX ':
        raise ValueError('not a .vox file')
    models, state = [], {'size': None, 'pal': [(0, 0, 0)] * 256}

    def walk(buf):
        p = 0
        while p + 12 <= len(buf):
            cid = buf[p:p + 4]
            n, m = struct.unpack('<II', buf[p + 4:p + 12])
            content, children = buf[p + 12:p + 12 + n], buf[p + 12 + n:p + 12 + n + m]
            p += 12 + n + m
            if cid == b'SIZE':
                state['size'] = struct.unpack('<III', content)
            elif cid == b'XYZI':
                cnt = struct.unpack('<I', content[:4])[0]
                models.append((state['size'],
                               [tuple(content[4 + i * 4:8 + i * 4]) for i in range(cnt)]))
            elif cid == b'RGBA':
                state['pal'] = [tuple(content[i * 4:i * 4 + 3]) for i in range(256)]
            if children:
                walk(children)

    walk(d[8:])
    return models, state['pal']


def art_cells(path, want):
    """{baked cell: authored (r, g, b)} for the art model the bake used, or None.

    `want` is the set of cells fruit.json carries for this fruit. Each model is
    cropped to its own bounding box -- which is what the bake did -- and the one
    whose cells match exactly is the one that was baked. No match is not an
    error: the caller falls back to the pooled colour and says so.
    """
    models, pal = read_vox(path)
    for _size, vox in models:
        if not vox:
            continue
        lo = [min(v[i] for v in vox) for i in range(3)]
        cells = {(v[0] - lo[0]) | ((v[1] - lo[1]) << 8) | ((v[2] - lo[2]) << 16): v[3]
                 for v in vox}
        if set(cells) == want:
            return {k: pal[ci - 1] for k, ci in cells.items()}
    return None


def mean(cols, fallback):
    """The voxel-weighted mean shade, or `fallback` when the art said nothing."""
    if not cols:
        return tuple(fallback)
    return tuple(int(round(sum(c[i] for c in cols) / len(cols))) for i in range(3))


def main():
    apply_ = '--apply' in sys.argv[1:]
    if not os.path.exists(SRC):
        print('missing: %s' % SRC)
        return 1
    fj = json.load(open(SRC))
    pal, nbody = fj['pal'], fj['nbody']
    pooled = tuple(pal[nbody])

    print('fruit.json: %d fruit, %d body colours, pooled crown %s'
          % (len(fj['fruit']), nbody, pooled))
    print()
    hdr = '%-8s %-12s %5s %5s %5s  %-13s %-13s %s'
    print(hdr % ('name', 'size (vox)', 'flesh', 'blade', 'stalk',
                 'flesh colour', 'blade colour', 'stalk colour'))

    rc = 0
    for i, f in enumerate(fj['fruit']):
        flesh = tuple(pal[i])
        want = set(p & 0xffffff for p in f['vox'])
        art, note = None, ''
        path = ART.get(f['name'])
        if not path or not os.path.exists(path):
            note = '    -- no art for %s; the whole crown stays blade' % f['name']
        else:
            try:
                art = art_cells(path, want)
            except Exception as exc:                       # noqa: BLE001 - reported, not raised
                note = '    -- %s unreadable (%s); the whole crown stays blade' % (path, exc)
            if art is None and not note:
                note = '    -- no model in %s matches the bake; the whole crown stays blade' % path

        out, bladec, stalkc, nf = [], [], [], 0
        for p in f['vox']:
            key = p & 0xffffff
            x, y, z = key & 255, (key >> 8) & 255, (key >> 16) & 255
            if not ((p >> 24) & 255):
                out.append((x, y, z, FLESH))     # THE TOP BYTE IS A SLOT, NOT A COLOUR
                nf += 1
                continue
            c = art.get(key) if art else None
            # BROWN STALK vs GREEN BLADE, read off the authored colour -- the
            # only place the split still exists once the bake has pooled it.
            # A cell the art does not know is a blade, which is what shipped.
            if c and c[0] > c[1]:
                out.append((x, y, z, STALK))
                stalkc.append(c)
            else:
                out.append((x, y, z, BLADE))
                if c:
                    bladec.append(c)

        blade = mean(bladec, pooled)
        # NO STALK IS NOT AN ERROR -- the orange has none, and entry 3 simply
        # goes unreferenced. v2's loadModelSet registers only the entries a
        # model actually uses ("ONLY THE ENTRIES THE MODEL USES"), so an unused
        # slot costs nothing on its 255-entry table.
        stalk = mean(stalkc, blade)
        size = (f['sx'], f['sy'], f['sz'])
        print(hdr % (f['name'], '%dx%dx%d' % size, nf, len(out) - nf - len(stalkc),
                     len(stalkc), '%d,%d,%d' % flesh, '%d,%d,%d' % blade,
                     ('%d,%d,%d' % stalk) if stalkc else '-'))
        if note:
            print(note)
            rc = 1
        if apply_:
            outp = os.path.join(OUTDIR, 'fruit_%s.vox' % f['name'])
            write_vox(outp, size, out, [flesh, blade, stalk])
            print('    -> %s' % outp)

    # A FRUIT IS 0.1 m TO A VOXEL IN BOTH ENGINES, which is why nothing is
    # rescaled here: the browser engine's ice-cap note measures 4 voxels at
    # 40 cm and v2's VOXEL_M is 0.1f. The apple ships the size it was authored.
    print()
    print('at 0.1 m per voxel: %s' % ', '.join(
        '%s %.2f x %.2f x %.2f m' % (f['name'], f['sx'] * 0.1, f['sy'] * 0.1, f['sz'] * 0.1)
        for f in fj['fruit']))
    if not apply_:
        print('measure only -- pass --apply to write the .vox files')
    return rc


if __name__ == '__main__':
    sys.exit(main())
