"""Graft cactus_flower.vox onto the arm tips of the nine revoxelised cacti.

  python tools/cactus_graft_flower.py

Reads   game/assets/foilage/cactus/cactus_flower.vox   the FLOWER alone, hand authored
        <a clean glb2vox bake of the nine bodies>       passed as --bodies, or regenerated
Writes  game/assets/foilage/cactus/cactus_1..9.vox

WHY THIS EXISTS BESIDE v1's tools/cactus_flowers.py. That tool reads the nine BODIES out of
game/assets/decoration/cacti.json, which are the 1x plants baked from the .glb before v2
revoxelised them. Running it here would silently swap the 2x bodies back for the old ones.
Same job, different source for the plant; the flower is read from the same place.

AN INPUT A TOOL OVERWRITES IS NOT AN INPUT -- v1's header earns this line and it is repeated
because the trap is still live. cactus_flower.vox is never written by anything, so this is
idempotent only as long as the BODIES come from somewhere this does not write. It takes them
from a directory you pass in, and writes to the asset directory. Do not point both at the
same place.

THE COLOURS NEED NO APPROXIMATION. The flower authors six entries and the bodies' borrowed
palette holds the same six RGB values exactly -- (227,61,89) through (243,130,153) plus the
(255,203,127) centre -- so the graft is a straight entry remap by RGB equality. A nearest
match is refused rather than guessed at: if a colour is ever missing, that is an asset
question and not something for this tool to paper over.

TIPS, NOT TOPS, which is v1's rule and its wording: a saguaro has several arms and each one
should carry a flower, so this keeps every LOCAL maximum of the height map and then merges
maxima within a few voxels so one arm gets one flower rather than a cluster.

THE MODEL GROWS BY THE FLOWER'S HEIGHT. The tallest arm defines the model's z extent, so
there is no headroom above it by construction; the SIZE chunk is raised so the tallest arm
can carry a flower like every other one.
"""
import argparse
import os
import struct
import sys

ASSETS = "C:/voxelbit/game/assets/foilage/cactus"
FLOWER = os.path.join(ASSETS, "cactus_flower.vox")


def read_vox(path):
    d = open(path, "rb").read()
    off, size, pal, vox = 20, None, None, []
    while off + 12 <= len(d):
        tag = d[off:off + 4]
        ln, kids = struct.unpack_from("<II", d, off + 4)
        if tag == b"SIZE":
            size = struct.unpack_from("<III", d, off + 12)
        elif tag == b"XYZI":
            n, = struct.unpack_from("<I", d, off + 12)
            vox = [tuple(d[off + 16 + i * 4: off + 16 + i * 4 + 4]) for i in range(n)]
        elif tag == b"RGBA":
            pal = [tuple(d[off + 12 + i * 4 + c] for c in range(3)) for i in range(256)]
        off += 12 + ln
        if not ln and not kids:
            break
    return size, pal, vox


def write_vox(path, size, pal, vox):
    """A MAIN with SIZE, XYZI and RGBA under it -- the shape every reader here expects."""
    sz = b"SIZE" + struct.pack("<II", 12, 0) + struct.pack("<III", *size)
    body = struct.pack("<I", len(vox)) + b"".join(bytes(v) for v in vox)
    xy = b"XYZI" + struct.pack("<II", len(body), 0) + body
    rgba = b"RGBA" + struct.pack("<II", 1024, 0) + b"".join(
        bytes((pal[i][0], pal[i][1], pal[i][2], 255)) for i in range(256))
    kids = sz + xy + rgba
    out = b"VOX " + struct.pack("<I", 150) + b"MAIN" + struct.pack("<II", 0, len(kids)) + kids
    open(path, "wb").write(out)


def arm_tips(vox, radius=3, merge=5):
    top = {}
    for x, y, z, _ in vox:
        if (x, y) not in top or z > top[(x, y)]:
            top[(x, y)] = z
    peaks = [(x, y, z) for (x, y), z in top.items()
             if all(top.get((x + dx, y + dy), -1) <= z
                    for dy in range(-radius, radius + 1)
                    for dx in range(-radius, radius + 1))]
    peaks.sort(key=lambda p: -p[2])
    sites = []
    for x, y, z in peaks:
        if all((x - sx) ** 2 + (y - sy) ** 2 > merge * merge for sx, sy, _ in sites):
            sites.append((x, y, z))
    return sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bodies", required=True, help="directory of UNFLOWERED cactus_N.vox")
    ap.add_argument("--out", default=ASSETS)
    args = ap.parse_args()
    if os.path.abspath(args.bodies) == os.path.abspath(args.out):
        sys.exit("refusing: --bodies and --out are the same directory (see the header)")

    fsize, fpal, fvox = read_vox(FLOWER)
    fx, fy, fz = fsize
    print("flower   %dx%dx%d, %d voxels" % (fx, fy, fz, len(fvox)))

    for i in range(1, 10):
        src = os.path.join(args.bodies, "cactus_%d.vox" % i)
        size, pal, vox = read_vox(src)
        sx, sy, sz = size

        # the flower's entries, remapped onto the body's palette by EXACT rgb
        remap = {}
        for _, _, _, c in fvox:
            if c in remap:
                continue
            want = fpal[c - 1]
            hit = next((e for e in range(1, 256) if pal[e - 1] == want), None)
            if hit is None:
                sys.exit("cactus_%d: flower colour %s is not in the body palette" % (i, want))
            remap[c] = hit

        sites = arm_tips(vox)
        grown = (sx, sy, sz + fz)          # headroom for the tallest arm's flower
        solid = {(x, y, z) for x, y, z, _ in vox}
        add = []
        for ax, ay, az in sites:
            for vx, vy, vz, c in fvox:
                p = (ax + vx - fx // 2, ay + vy - fy // 2, az + 1 + vz)
                if not (0 <= p[0] < grown[0] and 0 <= p[1] < grown[1] and 0 <= p[2] < grown[2]):
                    continue
                if p in solid:
                    continue
                solid.add(p)
                add.append((p[0], p[1], p[2], remap[c]))
        write_vox(os.path.join(args.out, "cactus_%d.vox" % i), grown, pal, vox + add)
        print("cactus_%d  %d voxels + %d flower on %d arm(s), z %d -> %d"
              % (i, len(vox), len(add), len(sites), sz, grown[2]))


if __name__ == "__main__":
    main()
