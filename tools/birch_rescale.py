"""Uniformly rescale the birch .vox models in place. Preserves palette, colours and hand edits.

    "go ahead and make the uniform 0.70x"                                       - user, 2026-08-24

    python tools/birch_rescale.py 0.70            show the plan  (default: measure only)
    python tools/birch_rescale.py 0.70 --apply    rewrite the .vox files

WHY RESCALE THE .vox RATHER THAN RE-BAKE FROM THE FBX. A re-bake at a smaller TALL_SCALE would give cleaner
geometry, and it would also throw away everything the folder has become: the owner's hand edits, the white
bark retexture, the welded twigs, and the CURATION (the source has 27 trees; the shipped set is the 16 that
were kept). A re-bake resurrects the eleven that were deleted and loses the rest. So the shipped models are
the source of truth and this shrinks them.

WHY IT IS WORTH DOING AT ALL, measured: the birch band traces at 3.86 ms against the oak band's 2.56, and the
whole difference is the trees (2.09 ms of it, against oak's 0.98). The cause is that EVERY brick a birch
occupies is a SURFACE brick - 5,611 of them, zero full, at 10-18% fill. The tracer skips an empty brick in one
step and a solid brick in one step; a partially-filled brick has to be walked voxel by voxel, and a birch
canopy is nothing but those, spread over a 205-voxel column.
Surface-brick count follows the crown's AREA, not its height, so a uniform shrink pays about s^2 while
squashing height alone pays far less:

    uniform 0.85  -> 70% of the bricks      height-only 0.85 -> 89%
    uniform 0.70  -> 48%                    height-only 0.70 -> 77%
                                            height-only 0.50 -> 61%

0.70 uniform takes the tree cost to ~1.0 ms, i.e. oak's, for a height change of 205 -> 144.

MAJORITY COLOUR, NOT LAST-WRITE. Several source voxels collapse into one output cell; taking whichever
happened to be written last would bias the result toward high coordinates and mottle the bark. Each output
cell takes the colour most of its inputs wore, so the white/dark ratio on a trunk survives the shrink.

RE-RUN tools/birch_bridge.py AFTERWARDS: collapsing cells can leave a cluster attached only diagonally.

Stdlib only.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
args = [a for a in sys.argv[1:]]
APPLY = '--apply' in args
scales = [a for a in args if a != '--apply']
if len(scales) != 1:
    sys.exit('usage: birch_rescale.py <scale> [--apply]')
S = float(scales[0])
if not (0.2 <= S <= 1.0):
    sys.exit('scale must be 0.2..1.0')


def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out


print('%-7s %14s %14s %10s' % ('file', 'size before', 'size after', 'voxels'))
tot_b = tot_a = 0
for path in sorted(glob.glob(os.path.join(DIR, '*.vox')), key=lambda p: int(os.path.basename(p)[:-4])):
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    sizes = [(cid, o, cs) for cid, o, cs in ch if cid == b'SIZE']
    xyzi = [(cid, o, cs) for cid, o, cs in ch if cid == b'XYZI']
    assert len(sizes) == len(xyzi) == 1, '%s is multi-part' % path
    sx, sy, sz = struct.unpack_from('<III', d, sizes[0][1])
    o0 = xyzi[0][1]
    n = struct.unpack_from('<I', d, o0)[0]
    votes = {}
    for k in range(n):
        b = o0 + 4 + k * 4
        key = (int(d[b] * S), int(d[b + 1] * S), int(d[b + 2] * S))
        c = votes.setdefault(key, {})
        c[d[b + 3]] = c.get(d[b + 3], 0) + 1
    nsx, nsy, nsz = max(1, int(sx * S)), max(1, int(sy * S)), max(1, int(sz * S))
    tot_b += n; tot_a += len(votes)
    if APPLY:
        body = struct.pack('<I', len(votes)) + b''.join(
            bytes((x, y, z, max(c.items(), key=lambda kv: kv[1])[0])) for (x, y, z), c in votes.items())
        out = bytearray()
        for cid, o, cs in ch:
            if cid == b'SIZE':
                payload = struct.pack('<III', nsx, nsy, nsz)
            elif cid == b'XYZI':
                payload = body
            else:
                payload = bytes(d[o:o + cs])
            out += cid + struct.pack('<II', len(payload), 0) + payload
        open(path, 'wb').write(bytes(d[:8]) + b'MAIN' + struct.pack('<II', 0, len(out)) + bytes(out))
    print('%-7s %14s %14s %10d' % (os.path.basename(path), '%dx%dx%d' % (sx, sy, sz),
                                   '%dx%dx%d' % (nsx, nsy, nsz), len(votes)))
print('scale %.2f  voxels %d -> %d (%.0f%%)  %s'
      % (S, tot_b, tot_a, 100.0 * tot_a / tot_b, 'APPLIED' if APPLY else 'measured only - pass --apply'))
