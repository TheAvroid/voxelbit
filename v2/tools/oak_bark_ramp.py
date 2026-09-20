#!/usr/bin/env python3
"""oak_bark_ramp.py -- give the oak trunks the pine's WHOLE bark ramp.

    python tools/oak_bark_ramp.py                 # dry run, prints the split
    python tools/oak_bark_ramp.py --apply

(user 2026-09-19: "can you add more color to the oak tree wood? I need more
brown colors, it looks like just one color is on the tree. fix this.")

WHY THE OAK LOOKED LIKE ONE COLOUR, MEASURED RATHER THAN GUESSED
----------------------------------------------------------------
tools/fbx2vox.cpp writes three bark shades, taken byte for byte off the oaks it
replaced: #544c33 #655943 #716858.  World::loadTrees then folds them onto the
nearest entry the PINES minted, which is the 2026-09-17 decision "match the pine
trees wood color for the oak trees" and is not the problem.  What IS the problem
is which pine entries they reach:

    oak (84, 76, 51)    ->  pine ( 94,  73, 53)   d = 11
    oak (101, 89, 67)   ->  pine (112,  86, 63)   d = 12
    oak (113,104, 88)   ->  pine (129,  99, 72)   d = 23

The pine bark ramp has FIVE shades and runs 94 -> 164 in red.  All three oak
barks land in the DARKEST THREE of it, which span 94 -> 129: the oak wears a
third of the range the pine does, in its flattest end.

...AND THE HALF OF IT YOU CAN SEE IS FLATTER STILL.  fbx2vox ranks bark by
ambient occlusion and gives the darkest shade to the most occluded half of the
voxels -- which in a solid trunk is the INSIDE.  Measured on oak_1: the darkest
shade is 77,146 voxels and only 15,019 of them are on the surface.  So what is
actually visible is two browns twelve units apart, which is the report.

WHAT THIS DOES
--------------
Re-ranks bark by the same ambient-occlusion proxy and spreads it over all FIVE
pine shades, with the cuts taken so that the SURFACE matches the pine's own
distribution (11.6 / 23.3 / 33.1 / 22.1 / 9.9 %, measured over the nine pine
models).  Ranking the surface rather than the whole solid is the fix for the
second half: the inside of a trunk then falls off the dark end of the scale,
where it belongs, instead of eating half the ramp.

IT COSTS NO PALETTE ENTRIES.  The five colours it writes are the pines' own, so
Palette::forModelColor finds each of them already in the table under its own
quantisation bucket and hands back the existing id -- an exact hit, not a fold.
Verified: 246 of 255 before and after.

IT IS IDEMPOTENT, unlike tools/revoxel_oak_scaled.py beside it (whose note says
so the hard way).  Bark is recognised by either the old three colours or the new
five, the shade is derived from GEOMETRY every time, and nothing here scales
anything -- so a second --apply produces a byte-identical file.

IT DOES NOT TOUCH THE GEOMETRY.  Only colour index bytes inside XYZI and five
entries of RGBA are rewritten; every chunk keeps its length, so the file size is
unchanged and the models stay the 25.6 m ones.
"""
import argparse
import collections
import glob
import os
import struct
import sys

# The pines' own five, darkest first -- read out of pine9/*.vox, not invented.
PINE_BARK = [(94, 73, 53), (112, 86, 63), (129, 99, 72), (146, 113, 82), (164, 126, 92)]
# ...and how much of the pines' visible bark wears each, measured over all nine
# models (183,124 surface voxels).  The oak is cut to the same proportions so
# the two woods read as the same material rather than as two settings of one.
PINE_SHARE = [0.116, 0.233, 0.331, 0.221, 0.099]
# What fbx2vox wrote, and what this replaces.
OLD_BARK = [(84, 76, 51), (101, 89, 67), (113, 104, 88)]

# The palette SLOTS the five shades live in.  1..3 are the three fbx2vox already
# used for bark; 8 and 9 are free (the file uses 1..7).  Leaf ids 4..7 are not
# touched, which is what keeps this a bark change.
BARK_IDS = [1, 2, 3, 8, 9]

# AMBIENT OCCLUSION, as the occupied fraction of a ball of this radius.  fbx2vox
# uses 2; 3 is used here because the ranking is now over the SURFACE only, where
# the values bunch up -- 123 cells give a gradient a five-way cut can use where
# 33 give ties.
AO_R = 3


def ao_offsets(r):
    out = []
    for dz in range(-r, r + 1):
        for dy in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if dx * dx + dy * dy + dz * dz <= r * r:
                    out.append((dx, dy, dz))
    return out


OFF = ao_offsets(AO_R)
NB6 = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))


def hash_jitter(p):
    """A deterministic +/-2% wobble on the AO, so the quantile cuts do not draw
    contour lines round the trunk.  fbx2vox does the same and for the same
    reason; the value is per voxel, so it survives a re-run unchanged."""
    h = (p[0] * 73856093) ^ (p[1] * 19349663) ^ (p[2] * 83492791)
    return ((h & 0xFFFF) / 65535.0 - 0.5) * 0.04


def parse(path):
    d = bytearray(open(path, 'rb').read())
    pal = [None]
    pal_off = [None]
    models = []
    cur = [None]

    def walk(off, end):
        while off < end:
            cid = bytes(d[off:off + 4])
            n, m = struct.unpack_from('<ii', d, off + 4)
            o = off + 12
            if cid == b'SIZE':
                cur[0] = struct.unpack_from('<iii', d, o)
            elif cid == b'XYZI':
                c = struct.unpack_from('<i', d, o)[0]
                vs = [struct.unpack_from('<BBBB', d, o + 4 + k * 4) for k in range(c)]
                models.append((cur[0], vs, o + 4))
            elif cid == b'RGBA':
                pal[0] = [struct.unpack_from('<BBBB', d, o + i * 4) for i in range(256)]
                pal_off[0] = o
            if m:
                walk(o + n, o + n + m)
            off = o + n + m

    walk(20, len(d))
    if pal[0] is None:
        raise SystemExit(path + ": no RGBA chunk -- see the note in vox-file-traps")
    return d, pal[0], pal_off[0], models


def reshade(path, apply_it):
    d, pal, pal_off, models = parse(path)
    bark_rgb = set(OLD_BARK) | set(PINE_BARK)
    bark_ids = {i for i in range(1, 256) if tuple(pal[i - 1][:3]) in bark_rgb}

    # THE MODELS ARE STACKED ON Z.  A 25.6 m oak is two 128-slabs, because an
    # XYZI coordinate is a byte -- see vox-file-traps.  Ambient occlusion has to
    # be measured across the seam or there is a bright ring round every trunk at
    # 12.8 m, so the slabs are merged before anything is asked of them.
    occ = set()
    col = {}
    where = {}
    z0 = 0
    for mi, (sz, vs, off) in enumerate(models):
        for k, (x, y, z, i) in enumerate(vs):
            p = (x, y, z + z0)
            occ.add(p)
            col[p] = i
            where[p] = (mi, k)
        z0 += sz[2]

    bark = [p for p, i in col.items() if i in bark_ids]
    if not bark:
        return None

    ao = {}
    for p in bark:
        x, y, z = p
        n = 0
        for dx, dy, dz in OFF:
            if (x + dx, y + dy, z + dz) in occ:
                n += 1
        ao[p] = n / float(len(OFF)) + hash_jitter(p)

    shown = [p for p in bark
             if any((p[0] + a, p[1] + b, p[2] + c) not in occ for a, b, c in NB6)]
    # THE CUTS COME OFF THE SURFACE AND ARE THEN APPLIED TO EVERYTHING.  That
    # is the whole correction: interior bark has a higher AO than any lit voxel,
    # so it falls past the last cut into the darkest shade -- which is what the
    # inside of a log looks like when you chop one open -- instead of taking
    # half the ramp with it.
    # MORE OCCLUDED IS DARKER, which is the direction fbx2vox already shades
    # in and the only one that is a light field rather than a paint job.  So the
    # list runs from the most exposed voxel to the most buried one and the SHADE
    # runs the other way -- the shares are read backwards with it.
    shown.sort(key=lambda p: ao[p])
    cuts = []
    acc = 0.0
    for f in list(reversed(PINE_SHARE))[:-1]:
        acc += f
        cuts.append(ao[shown[min(len(shown) - 1, int(acc * len(shown)))]])
    top = len(PINE_SHARE) - 1

    def shade(p):
        v = ao[p]
        for k, c in enumerate(cuts):
            if v <= c:
                return top - k
        return 0   # past every cut: the inside of the trunk, and the darkest

    shownset = set(shown)
    shist = collections.Counter()
    for p in shownset:
        shist[shade(p)] += 1

    print("  %-12s bark %6d (%6d visible)  ->  %s" %
          (os.path.basename(path), len(bark), len(shown),
           "  ".join("%d%%" % round(100.0 * shist[k] / max(1, len(shown)))
                     for k in range(len(PINE_SHARE)))))

    if not apply_it:
        return len(bark)

    # ---- the five entries, in the model's own palette --------------------
    for k, rgb in enumerate(PINE_BARK):
        idx = BARK_IDS[k] - 1
        a = pal[idx][3] if pal[idx] else 255
        struct.pack_into('<BBBB', d, pal_off + idx * 4, rgb[0], rgb[1], rgb[2], a or 255)

    # ---- and the index byte of every bark voxel --------------------------
    for p in bark:
        mi, k = where[p]
        off = models[mi][2] + k * 4 + 3
        d[off] = BARK_IDS[shade(p)]

    open(path, 'wb').write(bytes(d))
    return len(bark)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dir", default="C:/voxelbit/game/assets/foilage/oak_trees")
    a = ap.parse_args()
    files = sorted(glob.glob(os.path.join(a.dir, "oak_*.vox")))
    if not files:
        raise SystemExit("no oak_*.vox under " + a.dir)
    print("%s %d model(s), surface split per shade (dark -> light):" %
          ("REWRITING" if a.apply else "dry run over", len(files)))
    for f in files:
        reshade(f, a.apply)
    if not a.apply:
        print("  (nothing written -- pass --apply)")


if __name__ == "__main__":
    main()
