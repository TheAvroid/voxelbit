"""Rescale the birch and pine .vox models so the tallest tree stands 100 ft.

    "revoxelize the birch trees where the tallest one is 100 feet. I also want you
     revoxelize the pine trees as well to be 100 feet tall. make sure to use the
     same color schemes across voxelization."               - user, 2026-09-06

    python tools/revoxel_trees_tall.py              show the plan  (default: measure only)
    python tools/revoxel_trees_tall.py --apply      rewrite the .vox files
    python tools/revoxel_trees_tall.py --ft 90      some other target

WHY THIS SPLITS FILES IN TWO
  100 ft is 30.48 m, and on the 10 cm grid this engine renders (VOXEL_M in
  scene/voxelworld.h) that is 305 voxels. A .vox XYZI record packs each
  coordinate in a SINGLE BYTE, so no one model may exceed 256 on a side -- 25.6 m.
  A 100 ft tree simply does not fit in one model, and every previous tool here
  clamped rather than face that: see the note in loadRocks about Big_2 sampling
  at 3.42x instead of 4x.

  MagicaVoxel's own answer is the scene graph. A file may hold several models,
  each placed by an nTRN transform, so a 30 m tree is two stacked objects and the
  256 ceiling applies to each PIECE rather than to the tree. This writes that,
  and voxParse in scene/vox.h composes it back into one grid on load. The files
  stay ordinary MagicaVoxel documents: open one and you get two pieces sitting on
  top of each other, both editable.

WHY RESCALE THE SHIPPED .vox RATHER THAN RE-BAKE FROM THE FBX
  The same reason birch_rescale.py gives, and it has only got stronger since: the
  shipped folder carries the owner's hand edits, the white bark retexture, the
  welded twigs and the CURATION -- the source has 27 birches and 16 were kept. A
  re-bake resurrects the eleven that were deleted and loses everything else. The
  shipped models are the source of truth and this grows them.

NEAREST-NEIGHBOUR, AND WHY NOTHING ELSE WILL DO
  Palette ids are IDENTITY here, not colour. loadModelSet in gpu/world.h keys
  every material off the exact RGBA the id carries -- forModelColor(mo.pal[e-1])
  -- so any filter that blends neighbours invents a colour that is not in the
  file, and the shared 255-entry table floods. Nearest-neighbour cannot do that:
  every output voxel is some input voxel, wearing an id the file already had.
  The RGBA chunk is copied through byte for byte, so birch keeps its white bark
  and pine its brown, exactly as authored.

  Going UP rather than down there is no many-to-one collapse to resolve, so
  birch_rescale.py's majority-colour rule has nothing to decide here. Each source
  voxel simply becomes a 1x1x1 or 2x1x2 block depending on where it falls.

WHAT IT COSTS
  birch_rescale.py shrank these 0.70x for a reason: tree trace cost follows the
  crown's surface-brick count, which goes as s^2. This goes the other way --
  birch x1.266 and pine x1.338 -- so expect ~1.6x and ~1.8x the bricks, and
  roughly 2x and 2.4x the voxels. That was asked for with the cost known.

  RE-RUN tools/birch_bridge.py AFTERWARDS if a scale ever leaves a cluster
  attached only diagonally. Growing cannot disconnect what was connected, so
  this pass should not need it -- unlike the shrink.

IDEMPOTENT. The target is an absolute height, and the reader composes a split
file back into one grid, so running this twice is a no-op rather than a
compounding scale.

Stdlib only.
"""
import glob
import math
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIRCH = os.path.join(ROOT, 'game', 'assets', 'foilage', 'birch_trees')
PINE = os.path.join(ROOT, 'game', 'assets', 'foilage', 'pine9')

VOXEL_M = 0.1       # scene/voxelworld.h
FT_M = 0.3048
MAX_AXIS = 255      # the .vox single-byte coordinate ceiling

# Chunks that carry authoring intent we did not create and must not drop: the
# MagicaVoxel materials, the saved camera, the note strip. They are rewritten
# verbatim. The scene graph (nTRN/nGRP/nSHP) is NOT among them -- that describes
# a layout we are about to replace.
CARRY = (b'MATL', b'rOBJ', b'rCAM', b'NOTE', b'IMAP')


# ---------------------------------------------------------------------------
# reading
# ---------------------------------------------------------------------------
def rd_dict(b, o):
    """A MagicaVoxel DICT: int32 count, then that many (STRING, STRING) pairs."""
    n = struct.unpack_from('<i', b, o)[0]
    o += 4
    d = {}
    for _ in range(n):
        kl = struct.unpack_from('<i', b, o)[0]
        o += 4
        k = b[o:o + kl]
        o += kl
        vl = struct.unpack_from('<i', b, o)[0]
        o += 4
        d[k] = b[o:o + vl]
        o += vl
    return d, o


def read_vox(path):
    """Compose a .vox into one grid, honouring the scene graph if it has one.

    Returns (size, voxels, rgba, carried) with voxels a dict keyed (x, y, z).
    Composing on the way IN is what makes the tool idempotent: a file this
    script already split reads back as the single tall tree it represents,
    not as its two halves.
    """
    d = open(path, 'rb').read()
    assert d[:4] == b'VOX ', '%s is not a .vox' % path

    models, nodes, carried = [], {}, []
    rgba = None
    size = None
    i = 8 + 12  # skip the file header and the MAIN chunk header
    while i + 12 <= len(d):
        cid = d[i:i + 4]
        n, k = struct.unpack_from('<II', d, i + 4)
        body, i = d[i + 12:i + 12 + n], i + 12 + n + k
        if cid == b'SIZE':
            size = struct.unpack_from('<III', body, 0)
        elif cid == b'XYZI' and size is not None:
            cnt = struct.unpack_from('<I', body, 0)[0]
            models.append((size, [tuple(body[4 + q * 4:8 + q * 4]) for q in range(cnt)]))
            size = None
        elif cid == b'RGBA':
            rgba = body[:1024]
        elif cid in (b'nTRN', b'nGRP', b'nSHP'):
            nid = struct.unpack_from('<i', body, 0)[0]
            _, o = rd_dict(body, 4)
            if cid == b'nTRN':
                child, _res, _layer, frames = struct.unpack_from('<iiii', body, o)
                o += 16
                t = (0, 0, 0)
                for f in range(frames):
                    fr, o = rd_dict(body, o)
                    if f == 0 and b'_t' in fr:
                        t = tuple(int(v) for v in fr[b'_t'].split())
                nodes[nid] = ('T', t, [child])
            elif cid == b'nGRP':
                cn = struct.unpack_from('<i', body, o)[0]
                o += 4
                kids = list(struct.unpack_from('<%di' % cn, body, o)) if cn else []
                nodes[nid] = ('G', (0, 0, 0), kids)
            else:
                mn = struct.unpack_from('<i', body, o)[0]
                o += 4
                kids = []
                for _ in range(mn):
                    kids.append(struct.unpack_from('<i', body, o)[0])
                    _, o = rd_dict(body, o + 4)
                nodes[nid] = ('S', (0, 0, 0), kids)
        elif cid in CARRY:
            carried.append((cid, body))

    assert models, '%s has no models' % path

    # Place every piece. One model needs no graph; several are walked from the
    # root, summing translations. nTRN names a piece's CENTRE, so its minimum
    # corner is that translation less half its size, truncated -- the same
    # integer halving MagicaVoxel does on the way in.
    placed = []
    if len(models) == 1:
        placed.append((0, (0, 0, 0)))
    else:
        stack, seen = [(0, (0, 0, 0))], set()
        while stack:
            nid, acc = stack.pop()
            if nid in seen or nid not in nodes:
                continue
            seen.add(nid)
            kind, t, kids = nodes[nid]
            acc = (acc[0] + t[0], acc[1] + t[1], acc[2] + t[2])
            for k in kids:
                if kind == 'S':
                    if 0 <= k < len(models):
                        placed.append((k, acc))
                else:
                    stack.append((k, acc))
        assert placed, '%s has models but no graph reaching them' % path

    corners = []
    for m, t in placed:
        (sx, sy, sz), _ = models[m]
        corners.append((t[0] - sx // 2, t[1] - sy // 2, t[2] - sz // 2))
    ox = min(c[0] for c in corners)
    oy = min(c[1] for c in corners)
    oz = min(c[2] for c in corners)

    grid = {}
    for (m, _t), c in zip(placed, corners):
        (sx, sy, sz), vox = models[m]
        bx, by, bz = c[0] - ox, c[1] - oy, c[2] - oz
        for (x, y, z, col) in vox:
            if x < sx and y < sy and z < sz:
                grid[(bx + x, by + y, bz + z)] = col

    w = max(c[0] - ox + models[m][0][0] for (m, _t), c in zip(placed, corners))
    h = max(c[1] - oy + models[m][0][1] for (m, _t), c in zip(placed, corners))
    dpt = max(c[2] - oz + models[m][0][2] for (m, _t), c in zip(placed, corners))
    return (w, h, dpt), grid, rgba, carried


# ---------------------------------------------------------------------------
# writing
# ---------------------------------------------------------------------------
def s_str(b):
    return struct.pack('<i', len(b)) + b


def s_dict(d):
    out = struct.pack('<i', len(d))
    for k, v in d.items():
        out += s_str(k) + s_str(v)
    return out


def chunk(cid, body, kids=b''):
    return cid + struct.pack('<II', len(body), len(kids)) + body + kids


def slabs_for(dz):
    """Split a height into as few near-equal pieces as the format allows.

    Near-equal rather than 255-and-the-remainder so neither piece sits at the
    ceiling: a later hand edit that grows one by a voxel should not have to be
    re-split. 305 becomes 153 + 152, not 255 + 50.
    """
    n = max(1, int(math.ceil(dz / float(MAX_AXIS))))
    base, rem = dz // n, dz % n
    return [base + (1 if i < rem else 0) for i in range(n)]


def write_vox(path, size, grid, rgba, carried):
    dx, dy, dz = size
    heights = slabs_for(dz)
    assert dx <= MAX_AXIS and dy <= MAX_AXIS, 'x/y still exceed the .vox ceiling'

    pieces, z0 = [], 0
    for h in heights:
        vox = [(x, y, z - z0, c) for (x, y, z), c in grid.items() if z0 <= z < z0 + h]
        pieces.append(((dx, dy, h), z0, vox))
        z0 += h
    pieces = [p for p in pieces if p[2]]  # an empty slab would be a stray model

    body = b''
    for (sx, sy, sz), _z0, vox in pieces:
        body += chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
        body += chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))

    if len(pieces) > 1:
        # A root nTRN over an nGRP over one nTRN/nSHP pair per piece: the
        # shape MagicaVoxel writes, and the shape voxParse walks.
        body += chunk(b'nTRN', struct.pack('<i', 0) + s_dict({}) +
                      struct.pack('<iiii', 1, -1, -1, 1) + s_dict({}))
        kids = [2 + 2 * i for i in range(len(pieces))]
        body += chunk(b'nGRP', struct.pack('<i', 1) + s_dict({}) +
                      struct.pack('<i', len(kids)) + b''.join(struct.pack('<i', k) for k in kids))
        for i, ((sx, sy, sz), z0, _vox) in enumerate(pieces):
            t = b'%d %d %d' % (sx // 2, sy // 2, z0 + sz // 2)
            body += chunk(b'nTRN', struct.pack('<i', 2 + 2 * i) + s_dict({}) +
                          struct.pack('<iiii', 3 + 2 * i, -1, 0, 1) + s_dict({b'_t': t}))
            body += chunk(b'nSHP', struct.pack('<i', 3 + 2 * i) + s_dict({}) +
                          struct.pack('<i', 1) + struct.pack('<i', i) + s_dict({}))
        for layer in range(8):
            body += chunk(b'LAYR', struct.pack('<i', layer) +
                          s_dict({b'_name': b'', b'_hidden': b'0'}) + struct.pack('<i', -1))

    if rgba:
        body += chunk(b'RGBA', rgba)
    for cid, cbody in carried:
        body += chunk(cid, cbody)

    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) +
                           chunk(b'MAIN', b'', body))
    return len(pieces)


# ---------------------------------------------------------------------------
# scaling
# ---------------------------------------------------------------------------
def upscale(size, grid, f):
    """Nearest-neighbour, sampled exactly as tools/rescale_vox.py does.

    rescale_vox.py walks the OUTPUT and samples min(s-1, int(D/f)) for each
    cell, which at these sizes is 5 million iterations a tree. This inverts it:
    build the per-axis output-to-source map once, invert it, and visit only the
    cells that are actually occupied. Same sampling rule, same result, a
    fraction of the work.
    """
    sx, sy, sz = size
    out_size = tuple(max(1, int(round(v * f))) for v in size)

    fanout = []
    for axis, (s, d) in enumerate(zip(size, out_size)):
        rev = [[] for _ in range(s)]
        for D in range(d):
            rev[min(s - 1, int(D / f))].append(D)
        fanout.append(rev)
    fx, fy, fz = fanout

    out = {}
    for (x, y, z), c in grid.items():
        if x >= sx or y >= sy or z >= sz:
            continue
        for X in fx[x]:
            for Y in fy[y]:
                for Z in fz[z]:
                    out[(X, Y, Z)] = c
    return out_size, out


def main():
    args = sys.argv[1:]
    apply_ = '--apply' in args
    ft = 100.0
    if '--ft' in args:
        ft = float(args[args.index('--ft') + 1])
    target = int(round(ft * FT_M / VOXEL_M))

    birch = sorted(glob.glob(os.path.join(BIRCH, '*.vox')),
                   key=lambda p: int(os.path.splitext(os.path.basename(p))[0]))
    pine = sorted(glob.glob(os.path.join(PINE, 'pine_*.vox')),
                  key=lambda p: int(os.path.basename(p)[5:-4]))

    print('target %.1f ft = %.2f m = %d voxels at %.2f m each'
          % (ft, ft * FT_M, target, VOXEL_M))
    print('.vox ceiling is %d per axis, so anything over that ships as stacked pieces\n'
          % MAX_AXIS)

    jobs = []
    for name, paths, uniform in (('birch', birch, True), ('pine', pine, False)):
        read = [(p,) + read_vox(p) for p in paths]
        tallest = max(r[1][2] for r in read)
        # BIRCH KEEPS ITS VARIETY, PINE DOES NOT. "the tallest one is 100 feet"
        # is one factor across the set, so the 144-voxel saplings stay saplings
        # in proportion. "pine trees ... to be 100 feet tall" is every tree at
        # the target, which is barely a change of intent -- the nine pines
        # already stand within 3 voxels of each other.
        for p, size, grid, rgba, carried in read:
            f = (target / float(tallest)) if uniform else (target / float(size[2]))
            jobs.append((name, p, size, grid, rgba, carried, f))
        print('%s: %d models, tallest %d voxels -> %s'
              % (name, len(read), tallest,
                 'uniform x%.4f' % (target / float(tallest)) if uniform
                 else 'per-model, each to %d' % target))

    print('')
    hdr = '%-22s %-18s %-18s %8s %9s %6s'
    print(hdr % ('file', 'was (voxels)', 'now (voxels)', 'height', 'voxels', 'pieces'))
    total_before = total_after = 0
    for name, p, size, grid, rgba, carried, f in jobs:
        out_size, out_grid = upscale(size, grid, f)
        pieces = len(slabs_for(out_size[2]))
        total_before += len(grid)
        total_after += len(out_grid)
        print(hdr % (os.path.basename(p),
                     '%dx%dx%d' % size,
                     '%dx%dx%d' % out_size,
                     '%.1f ft' % (out_size[2] * VOXEL_M / FT_M),
                     len(out_grid),
                     pieces))
        if apply_:
            wrote = write_vox(p, out_size, out_grid, rgba, carried)
            assert wrote == pieces or not out_grid

    print('\nvoxels %d -> %d  (x%.2f)' % (total_before, total_after,
                                          total_after / float(total_before)))
    if apply_:
        print('written.')
    else:
        print('measure only -- pass --apply to rewrite the .vox files')


if __name__ == '__main__':
    main()
