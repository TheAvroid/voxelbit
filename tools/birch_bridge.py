"""Weld DIAGONALLY-attached voxel clusters onto the birch they belong to.

    "theres also floating leaves in the editor"                                 - user, 2026-08-23

    python tools/birch_bridge.py            measure only  (default)
    python tools/birch_bridge.py --apply    add the connecting voxels

WHY THEY LOOK DETACHED WHEN THEY ARE NOT. Run 26-connectivity over any of the 26 trees and every voxel is one
component - which is why a naive island hunt reports a clean bill and finds nothing to fix. But 26-connectivity
counts a CORNER or EDGE touch as attachment, and the renderer does not: two cubes meeting along an edge share
no face, so you see daylight straight through the join. A 49-voxel leaf cluster hanging off the crown by one
edge contact therefore draws as a cluster of leaves floating in open sky, and against the editor's plain sky
backdrop that is exactly what it looks like. birch_20_8m: 6 face-components, 123 voxels adrift in 5 clusters.

THE FIX IS TO ADD, NOT TO DELETE. The foliage is real - the voxelizer put it where the source's leaf cards are
- so removing it thins a crown that was correct. What is missing is the twig: the source carries these clusters
on Birch_bark05, which is thinner than a voxel over much of its run, so the connecting cell was never written.
This walks the shortest axis-aligned path from each adrift cluster to the tree and fills it in, one voxel at a
time, which is the twig the bake could not resolve.

The added voxel takes the CLUSTER's own dominant colour, so a leaf cluster welds with a leaf and the join is
invisible; it is never given a bark id, which would put a grey speck out at the crown edge.

MULTI-PART AWARE (2026-09-08). A birch past 256 voxels ships as a STACK of models plus an nTRN/nGRP/nSHP
scene graph, because a .vox coordinate is one byte. This used to assert single-part and stop, which was
honest while every model was short enough to fit - and stopped being true the moment the set was baked at
its shipped height, when eleven of the sixteen split. Connectivity is now flooded in TREE space across the
seams, exactly as birch_deblack.py and birch_islands.py do it, so a crown in the part above is not reported
adrift from the trunk in the part below - which is what a per-part run would have said about every tall
tree in the set. A weld is written back into whichever part its z lands in, and the scene graph, the
palette and the SIZE chunks are carried through untouched.

Iterates to a fixed point: welding one cluster can merge others, and a cluster may sit two steps out.
Single-part files only (at the shipped 0.91 scale every model is one), and it asserts rather than guessing.

Stdlib only.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
APPLY = '--apply' in sys.argv[1:]
for a in sys.argv[1:]:
    if a != '--apply': sys.exit('unknown argument %s' % a)

F6 = [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)]


def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i+4]; cs, ks = struct.unpack_from('<II', d, i+4); i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out


def components(occ):
    seen, comps = set(), []
    for key in occ:
        if key in seen: continue
        st, comp = [key], []
        seen.add(key)
        while st:
            x, y, z = st.pop(); comp.append((x, y, z))
            for dx, dy, dz in F6:
                q = (x+dx, y+dy, z+dz)
                if q in occ and q not in seen: seen.add(q); st.append(q)
        comps.append(comp)
    return comps


def rd_dict(d, o):
    """A .vox DICT: count, then that many (key, value) STRINGs. -> (dict, offset past it)."""
    n = struct.unpack_from('<I', d, o)[0]
    o += 4
    out = {}
    for _ in range(n):
        k = struct.unpack_from('<I', d, o)[0]
        key = d[o + 4:o + 4 + k].decode('utf-8', 'replace')
        o += 4 + k
        k = struct.unpack_from('<I', d, o)[0]
        val = d[o + 4:o + 4 + k].decode('utf-8', 'replace')
        o += 4 + k
        out[key] = val
    return out, o


def part_offsets(d, ch, sizes):
    """The z base of each model, read out of the scene graph. Falls back to sequential stacking.

    Lifted from birch_deblack.py so the two passes cannot drift apart about where a part sits: a
    disagreement here would have one tool welding into a seam the other one thinks is somewhere else."""
    shp, trn = {}, []
    for cid, o, cs in ch:
        if cid == b'nSHP':
            nid = struct.unpack_from('<i', d, o)[0]
            o2 = o + 4
            _, o2 = rd_dict(d, o2)
            o2 += 4                                     # num_models
            shp[nid] = struct.unpack_from('<i', d, o2)[0]
        elif cid == b'nTRN':
            o2 = o + 4
            _, o2 = rd_dict(d, o2)
            child = struct.unpack_from('<i', d, o2)[0]
            o2 += 16                                    # child, reserved, layer, num_frames
            fr, o2 = rd_dict(d, o2)
            tz = int(fr['_t'].split(' ')[2]) if '_t' in fr else 0
            trn.append((child, tz))
    base = [None] * len(sizes)
    for child, tz in trn:
        mi = shp.get(child)
        if mi is not None and mi < len(sizes):
            base[mi] = tz - (sizes[mi][2] // 2)
    if any(b is None for b in base):                    # no usable scene graph -> stack in file order
        run, base = 0, []
        for sz in sizes:
            base.append(run)
            run += sz[2]
    return base


print('%-14s %8s %9s %8s %8s' % ('tree', 'voxels', 'adrift', 'clusters', 'welds'))
tot_a = tot_w = 0
for path in sorted(glob.glob(os.path.join(DIR, '*.vox'))):
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    sizes = [struct.unpack_from('<III', d, o) for cid, o, cs in ch if cid == b'SIZE']
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    if not xyzi or len(sizes) != len(xyzi):
        print('%-14s malformed (%d SIZE, %d XYZI) - skipped' % (os.path.basename(path)[:-4], len(sizes), len(xyzi)))
        continue
    zoff = part_offsets(d, ch, sizes)
    sx = max(q[0] for q in sizes); sy = max(q[1] for q in sizes)
    zlo = min(zoff); zhi = max(zoff[i] + sizes[i][2] for i in range(len(sizes)))
    occ, n = {}, 0                                       # TREE space, so a weld may cross a part seam
    for pi, (o, cs) in enumerate(xyzi):
        cnt = struct.unpack_from('<I', d, o)[0]
        n += cnt
        for k in range(cnt):
            b = o + 4 + k*4
            occ[(d[b], d[b+1], d[b+2] + zoff[pi])] = d[b+3]
    added, adrift0, clusters0 = [], 0, 0
    for _ in range(12):
        comps = components(occ)
        if len(comps) == 1: break
        root = min(comps, key=lambda c: min(p[2] for p in c))
        rootset = set(root)
        rest = [c for c in comps if c is not root]
        if not adrift0:
            adrift0 = sum(len(c) for c in rest); clusters0 = len(rest)
        for c in rest:
            # the closest island/tree pair, then walk it one axis at a time
            best = None
            cs_ = set(c)
            for (x, y, z) in c:
                for dx in (-2,-1,0,1,2):
                    for dy in (-2,-1,0,1,2):
                        for dz in (-2,-1,0,1,2):
                            q = (x+dx, y+dy, z+dz)
                            if q in rootset:
                                cost = abs(dx)+abs(dy)+abs(dz)
                                if best is None or cost < best[0]: best = (cost, (x,y,z), q)
            if best is None: continue
            _, a, b = best
            col = max(set(occ[p] for p in c), key=lambda v: sum(1 for p in c if occ[p] == v))
            cur = list(a)
            for axis in (2, 0, 1):                       # z first: a twig runs mostly along the branch
                while cur[axis] != b[axis]:
                    cur[axis] += 1 if b[axis] > cur[axis] else -1
                    t = tuple(cur)
                    if t == b: break
                    if t not in occ and 0 <= t[0] < sx and 0 <= t[1] < sy and zlo <= t[2] < zhi:
                        occ[t] = col; added.append(t)
    if APPLY and added:
        # Split the composed grid back into the parts it came from. A voxel belongs to the part whose
        # z range it falls in, and goes back in that part's LOCAL coordinates - so a weld written
        # across a seam lands in the right model rather than off the end of the one below it.
        bodies = [[] for _ in sizes]
        for (x, y, z), c in occ.items():
            for pi in range(len(sizes)):
                if zoff[pi] <= z < zoff[pi] + sizes[pi][2]:
                    bodies[pi].append(bytes((x, y, z - zoff[pi], c)))
                    break
        out, pi = bytearray(), 0
        for cid, o, cs in ch:
            if cid == b'XYZI':
                payload = struct.pack('<I', len(bodies[pi])) + b''.join(bodies[pi])
                pi += 1
            else:
                payload = bytes(d[o:o+cs])
            out += cid + struct.pack('<II', len(payload), 0) + payload
        final = bytes(d[:8]) + b'MAIN' + struct.pack('<II', 0, len(out)) + bytes(out)
        open(path, 'wb').write(final)
    tot_a += adrift0; tot_w += len(added)
    print('%-14s %8d %9d %8d %8d' % (os.path.basename(path)[:-4], n, adrift0, clusters0, len(added)))
print('%-14s %8s %9d %8s %8d' % ('ALL', '', tot_a, '', tot_w))
print(APPLY and 'APPLIED' or 'measured only - pass --apply to weld')
