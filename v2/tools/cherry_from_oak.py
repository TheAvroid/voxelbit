#!/usr/bin/env python3
"""cherry_from_oak.py -- the oaks again, in blossom.

    python tools/cherry_from_oak.py            # dry run
    python tools/cherry_from_oak.py --apply

(user 2026-09-19: "can you create a cherry forest biome ... the difference is
the life and the trees leaves are pink instead of green", then "the cherry
forest uses the same trees as the oak forest, except recolor them to pink".)

THE SAME TREES, WHICH IS THE INSTRUCTION AND ALSO v1's DESIGN.  v1 does not
ship a second model set at all -- assets/bow.js calls its version "AN ID MAP,
NOT A SECOND MODEL SET ... 256 numbers, [so] the workers rebuild the pink
crowns from it rather than being handed a second 218k-voxel model set".  It
can, because its crowns are stamped voxel by voxel by a generator that can be
handed a table.  v2 meshes a placed MODEL, so the cheapest equivalent here is
the same geometry with a different palette in it: byte-identical XYZI, four
RGBA entries changed.

WHICH FOUR, AND WHY NOT NEAREST-COLOUR.  v1's note is worth keeping: "a
nearest-colour match would collapse all four greens onto the darkest blossom".
The leaves are ranked by LUMINANCE and the ramp is walked in the same order,
dark to light, so the crown keeps its own shading -- the shape of the light on
it is the art, and matching hues would throw that away.

THE RAMP IS v1's, taken out of its git history (assets/palette.js before the
biome was retired).  Its eight steps run #d06e8f to #f8a8c3; the four here are
that ramp sampled at 0, 2, 5 and 7, so the endpoints are exactly v1's.

    oak (80,112,47)  lum 100 ->  (208,110,143)
    oak (105,143,50) lum 128 ->  (220,126,159)
    oak (115,150,72) lum 137 ->  (238,150,183)
    oak (130,161,101) lum 150 -> (248,168,195)

THE BARK IS NOT TOUCHED.  A cherry has the same trunk as everything else in
this world -- see tools/oak_bark_ramp.py, which put the pine's five browns on
these models an hour before this tool existed and which must be run BEFORE this
one or the cherries inherit whatever the oaks were wearing at the time.

IT IS IDEMPOTENT and it does not touch geometry: the file size is unchanged and
only four RGBA entries differ from the oak it was copied from.  Re-running it
re-copies from the oak, so it also picks up anything that changed there.
"""
import argparse
import glob
import os
import shutil
import struct

# The oak's four leaf greens, and v1's blossom ramp they map onto -- both
# darkest first.  The greens are matched by VALUE rather than by palette index
# so a re-voxelised oak that moved its ids still lands.
OAK_LEAF = [(80, 112, 47), (105, 143, 50), (115, 150, 72), (130, 161, 101)]
BLOSSOM = [(208, 110, 143), (220, 126, 159), (238, 150, 183), (248, 168, 195)]

# -- ...AND A SECOND, PALER TREE (user 2026-09-19: "create a light pink variant
#    of half the cherry trees") -----------------------------------------------
#
# A WOOD OF ONE PINK READS AS ONE TREE REPEATED, which is the thing the four
# shades above exist to avoid inside a single crown -- this is that argument one
# level up. Half the stand wears this instead, chosen per tree.
#
# IT IS THE SAME RAMP CARRIED PAST v1's LIGHT END rather than a new ramp with
# its own character. v1's eight steps stop at #f8a8c3; these four continue on
# the same line toward white, keeping the spacing and therefore the SHADING --
# the crown's light still reads the way the oak's did, which is the whole
# argument in the luminance note above. A paler ramp that was also flatter
# would give a wood of pink blobs beside a wood of trees.
#
# THE GAP IS DELIBERATELY WIDE at the light end (168 -> 226 in green) because
# these are seen against each other across a clearing, not side by side: two
# ramps a few units apart would cost four palette entries to produce a wood
# that looks exactly like one pink.
BLOSSOM_LIGHT = [(240, 182, 203), (246, 196, 214), (251, 212, 226), (255, 226, 237)]

RAMPS = {"cherry": BLOSSOM, "light": BLOSSOM_LIGHT}


def palette_offset(d):
    """The byte offset of the RGBA chunk's payload, walking the chunk tree."""
    found = [None]

    def walk(off, end):
        while off < end:
            cid = bytes(d[off:off + 4])
            n, m = struct.unpack_from('<ii', d, off + 4)
            o = off + 12
            if cid == b'RGBA':
                found[0] = o
            if m:
                walk(o + n, o + n + m)
            off = o + n + m

    walk(20, len(d))
    return found[0]


def convert(src, dst, apply_it, ramp=None):
    d = bytearray(open(src, 'rb').read())
    po = palette_offset(d)
    if po is None:
        raise SystemExit(src + ": no RGBA chunk -- see vox-file-traps")
    pal = [struct.unpack_from('<BBBB', d, po + i * 4) for i in range(256)]

    hits = []
    ramp = ramp or BLOSSOM
    for green, pink in zip(OAK_LEAF, ramp):
        idx = [i for i in range(256) if tuple(pal[i][:3]) == green]
        if not idx:
            # Already converted, or an oak whose leaves moved.  Say which.
            already = [i for i in range(256) if tuple(pal[i][:3]) == pink]
            hits.append((green, pink, already[0] if already else None, True))
            continue
        hits.append((green, pink, idx[0], False))

    missing = [h for h in hits if h[2] is None]
    print("  %-12s -> %-14s %s" % (
        os.path.basename(src), os.path.basename(dst),
        "MISSING %d of 4 leaf greens" % len(missing) if missing
        else ("already pink" if all(h[3] for h in hits) else "4 leaf greens found")))
    if missing:
        for g, p, _, _ in missing:
            print("      no voxel wears %s -- has the oak been re-voxelised?" % (g,))
        return False
    if not apply_it:
        return True

    for green, pink, idx, _ in hits:
        a = pal[idx][3] or 255
        struct.pack_into('<BBBB', d, po + idx * 4, pink[0], pink[1], pink[2], a)
    open(dst, 'wb').write(bytes(d))
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--apply", action="store_true")
    ap.add_argument("--dir", default="C:/voxelbit/game/assets/foilage/oak_trees")
    ap.add_argument("--variant", default="cherry", choices=sorted(RAMPS),
                    help="cherry = v1's blossom; light = the paler half of the wood")
    a = ap.parse_args()
    oaks = sorted(glob.glob(os.path.join(a.dir, "oak_*.vox")))
    if not oaks:
        raise SystemExit("no oak_*.vox under " + a.dir)
    out = os.path.join(a.dir, "..", "cherry_trees")
    out = os.path.normpath(out)
    if a.apply:
        os.makedirs(out, exist_ok=True)
    print("%s %d model(s) -> %s" % ("WRITING" if a.apply else "dry run over", len(oaks), out))
    ok = True
    for f in oaks:
        # cherry_1.vox and cherry_light_1.vox, side by side in one folder --
        # world.h loads them as two consecutive ranges and the scatter picks
        # between them per tree. See ChunkMesher::cherryLightBase.
        stem = "cherry_" if a.variant == "cherry" else "cherry_light_"
        n = os.path.basename(f).replace("oak_", stem)
        ok = convert(f, os.path.join(out, n), a.apply, RAMPS[a.variant]) and ok
    if not a.apply:
        print("  (nothing written -- pass --apply)")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
