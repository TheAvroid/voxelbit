"""Find (and optionally delete) DISCONNECTED voxel islands in the birch .vox files.

    "theres also floating leaves in the editor"                                 - user, 2026-08-23

    python tools/birch_islands.py            measure only  (default)
    python tools/birch_islands.py --apply    delete the islands
    python tools/birch_islands.py --apply --keep=40   ...but spare islands of 40+ voxels

WHAT THEY ARE. The source birches carry their foliage on a twig network (Birch_bark05) that is THINNER THAN A
VOXEL over much of its run. Where a twig falls entirely between sample points the voxelizer writes no voxel for
it, and the leaf cluster at that branch tip becomes an ISLAND: geometry with no path back to the trunk. On the
ground the crown hides it; against the sky, which is what the editor shows, it reads as leaves hanging in mid
air. This is not the deblack pass's doing - converting a twig to a leaf keeps the voxel and keeps the path.

THE TEST IS 26-CONNECTIVITY, deliberately the loosest one: a twig that survives only as a diagonal chain is
still a real connection, and 6-connectivity would call most of a crown's fine structure disconnected and
delete it. The kept component is the one containing the LOWEST voxel (the trunk foot), not merely the largest,
so a tree can never be reduced to its own canopy with the bole thrown away.

Multi-part aware (a tall model ships as stacked models plus an nTRN/nGRP/nSHP scene graph) - the flood runs in
TREE space so it crosses part seams, exactly as birch_deblack.py does.

Deleting rewrites the XYZI chunks, so unlike birch_deblack this changes chunk LENGTHS: the file is rebuilt
rather than patched in place, and the SIZE/RGBA/scene-graph chunks are copied through untouched.

Stdlib only.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
APPLY, KEEP = False, 0
for a in sys.argv[1:]:
    if a == '--apply': APPLY = True
    elif a.startswith('--keep='): KEEP = int(a[7:])
    else: sys.exit('unknown argument %s' % a)


def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out


def rd_dict(d, o):
    n = struct.unpack_from('<I', d, o)[0]; o += 4
    for _ in range(n):
        ln = struct.unpack_from('<I', d, o)[0]; o += 4 + ln
        ln = struct.unpack_from('<I', d, o)[0]; o += 4 + ln
    return o


def part_offsets(d, ch, sizes):
    shp, trn = {}, []
    for cid, o, cs in ch:
        if cid == b'nSHP':
            nid = struct.unpack_from('<i', d, o)[0]
            o2 = rd_dict(d, o + 4) + 4
            shp[nid] = struct.unpack_from('<i', d, o2)[0]
        elif cid == b'nTRN':
            o2 = rd_dict(d, o + 4)
            child = struct.unpack_from('<i', d, o2)[0]
            o2 = rd_dict(d, o2 + 16)
    base = [None] * len(sizes)
    if any(b is None for b in base):
        run, base = 0, []
        for sz in sizes:
            base.append(run); run += sz[2]
    return base


NB = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dy or dz]
files = sorted(glob.glob(os.path.join(DIR, '*.vox')))
print('%-14s %8s %8s %7s %9s  %s' % ('tree', 'voxels', 'islands', 'lost', 'biggest', 'largest island size'))
tot_v = tot_l = tot_i = 0
for path in files:
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    sizes = [struct.unpack_from('<III', d, o) for cid, o, cs in ch if cid == b'SIZE']
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    zoff = part_offsets(d, ch, sizes)
    occ = {}
    for pi, (o, cs) in enumerate(xyzi):
        n = struct.unpack_from('<I', d, o)[0]
        for k in range(n):
            b = o + 4 + k * 4
            occ[(d[b], d[b + 1], d[b + 2] + zoff[pi])] = d[b + 3]
    seen, comps = set(), []
    for key in occ:
        if key in seen: continue
        stack, comp = [key], []
        seen.add(key)
        while stack:
            x, y, z = stack.pop()
            comp.append((x, y, z))
            for dx, dy, dz in NB:
                q = (x + dx, y + dy, z + dz)
                if q in occ and q not in seen:
                    seen.add(q); stack.append(q)
        comps.append(comp)
    root = min(comps, key=lambda c: min(p[2] for p in c))       # the component holding the LOWEST voxel = the bole
    islands = [c for c in comps if c is not root and len(c) <= KEEP or (c is not root and KEEP == 0)]
    islands = [c for c in comps if c is not root and (KEEP == 0 or len(c) <= KEEP)]
    drop = set()
    for c in islands: drop.update(c)
    big = max((len(c) for c in comps if c is not root), default=0)
    if APPLY and drop:
        out = bytearray(d[:8])
        for cid, o, cs in ch:
            if cid == b'XYZI':
                pi = [i for i, (o2, c2) in enumerate(xyzi) if o2 == o][0]
                n = struct.unpack_from('<I', d, o)[0]
                kept = []
                for k in range(n):
                    b = o + 4 + k * 4
                    if (d[b], d[b + 1], d[b + 2] + zoff[pi]) not in drop:
                        kept.append(bytes(d[b:b + 4]))
                body = struct.pack('<I', len(kept)) + b''.join(kept)
            else:
                body = bytes(d[o:o + cs])
            out += cid + struct.pack('<II', len(body), 0) + body
        # MAIN wraps everything after the 8-byte header
        final = bytearray(d[:4]) + struct.pack('<I', 150) + b'MAIN' + struct.pack('<II', 0, len(out) - 8) + out[8:]
        open(path, 'wb').write(bytes(final))
    tot_v += len(occ); tot_l += len(drop); tot_i += len(islands)
    print('%-14s %8d %8d %7d %9d' % (os.path.basename(path)[:-4], len(occ), len(islands), len(drop), big))
print('%-14s %8d %8d %7d   (%.2f%% of all voxels)' % ('ALL', tot_v, tot_i, tot_l, 100.0 * tot_l / max(1, tot_v)))
print('keep=%d  %s' % (KEEP, 'APPLIED' if APPLY else 'measured only - pass --apply to delete'))
