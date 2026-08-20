"""Carve a food model into its EAT_N eaten frames and write one .vox per frame.

This is the SAME carve assets/held-items.js runs at load (`eatStrip`), lifted out so the
frames exist as files the artist can open, check and re-author. The game still carves its
own strip procedurally at boot — these files are the readable copy of what it produces, so
if you change the carve on one side, re-run this to keep the other in step.

  python tools/eat_frames.py game/assets/food/meat/meat.vox game/assets/food/meat/steak
  python tools/eat_frames.py <src.vox> <outdir> [--frames=21] [--keep=0.0]

WHY IT MATCHES. modelToItem is a 1:1 grid copy (w,d,h = sx,sy,sz; index x + y*w + z*w*d),
so item coordinates ARE .vox coordinates and the carve can be reproduced here exactly:

  * bite point at (w-1, (d-1)/2, h-1) — one top corner of the model's own box
  * every voxel ordered by squared distance from it, ties broken on z, then x, then y,
    so the order is total and the strip is identical every run
  * frame f drops the nearest round(gone * f/(N-1)) of them

Two details that will silently desync the frames if they are "tidied":
  * JS Math.round is round-half-UP; Python's round() is round-half-to-EVEN, so 0.5 goes
    the other way and a frame comes out one voxel off. floor(x + 0.5) is used instead.
  * `keep` is the fraction still in your hand on the LAST frame, and 0 means the last
    frame is EMPTY. The max(3, …) floor only applies when keep > 0 — it is there to stop
    rounding emptying a food that meant to leave a remnant (the apple's core), not to
    stop a food that asked to disappear from disappearing (the steak).

The source's OWN palette is carried through unchanged, so a frame opens in the colours the
art was authored in rather than in whatever the game's 256-entry palette snapped them to.
"""

import os
import struct
import sys


def parse_vox(path):
    b = open(path, 'rb').read()
    if b[:4] != b'VOX ':
        raise SystemExit('%s is not a .vox' % path)
    size = None
    vox = None
    pal = None

    def walk(off, end):
        nonlocal size, vox, pal
        while off < end:
            cid = b[off:off + 4]
            n, cn = struct.unpack('<II', b[off + 4:off + 12])
            if cid == b'MAIN':
                walk(off + 12, off + 12 + cn)
            elif cid == b'SIZE' and size is None:
                size = struct.unpack('<III', b[off + 12:off + 24])
            elif cid == b'XYZI' and vox is None:
                cnt = struct.unpack('<I', b[off + 12:off + 16])[0]
                vox = [tuple(b[off + 16 + i * 4: off + 20 + i * 4]) for i in range(cnt)]
            elif cid == b'RGBA':
                pal = b[off + 12:off + 12 + 1024]
            off += 12 + n + cn

    walk(8, len(b))
    if not size or vox is None:
        raise SystemExit('%s has no SIZE/XYZI' % path)
    return size, vox, pal


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    xyzi = chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
    rgba = chunk(b'RGBA', pal) if pal else b''
    body = size + xyzi + rgba
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    opts = dict(a[2:].split('=', 1) for a in sys.argv[1:] if a.startswith('--') and '=' in a)
    if len(args) < 2:
        raise SystemExit(__doc__)
    src, outdir = args[0], args[1]
    n_frames = int(opts.get('frames', 21))
    keep = float(opts.get('keep', 0.0))

    (sx, sy, sz), vox, pal = parse_vox(src)
    # The bite corner, in the model's own box — held-items.js: bx = w-1, by = (d-1)/2, bz = h-1
    bx, by, bz = sx - 1, (sy - 1) / 2.0, sz - 1
    cl = []
    for (x, y, z, ci) in vox:
        d2 = (x - bx) ** 2 + (y - by) ** 2 + (z - bz) ** 2
        cl.append((d2, z, x, y, ci))
    cl.sort(key=lambda q: (q[0], q[1], q[2], q[3]))

    total = len(cl)
    remnant = max(3, int(total * keep + 0.5)) if keep > 0 else 0
    gone = max(0, total - remnant)

    os.makedirs(outdir, exist_ok=True)
    written = []
    for f in range(n_frames):
        eaten = 0 if n_frames < 2 else int(gone * f / (n_frames - 1) + 0.5)   # half-UP, like JS
        left = cl[eaten:]
        out = [(x, y, z, ci) for (_d, z, x, y, ci) in left]
        path = os.path.join(outdir, '%02d.vox' % f)
        write_vox(path, sx, sy, sz, out, pal)
        written.append(len(out))

    print('%s -> %s' % (src, outdir))
    print('  %d x %d x %d, %d voxels, keep=%g -> remnant %d' % (sx, sy, sz, total, keep, remnant))
    print('  %d frames, voxels per frame: %s' % (n_frames, written))
    if written and written[-1] == 0:
        print('  last frame is EMPTY - the food is eaten away to nothing')


if __name__ == '__main__':
    main()
