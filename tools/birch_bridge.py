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


print('%-14s %8s %9s %8s %8s' % ('tree', 'voxels', 'adrift', 'clusters', 'welds'))
tot_a = tot_w = 0
for path in sorted(glob.glob(os.path.join(DIR, '*.vox'))):
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    sizes = [struct.unpack_from('<III', d, o) for cid, o, cs in ch if cid == b'SIZE']
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    assert len(xyzi) == 1, '%s is multi-part (%d) - bridge assumes one' % (path, len(xyzi))
    sx, sy, sz = sizes[0]
    o0 = xyzi[0][0]
    occ = {}
    n = struct.unpack_from('<I', d, o0)[0]
    for k in range(n):
        b = o0 + 4 + k*4
        occ[(d[b], d[b+1], d[b+2])] = d[b+3]
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
                    if t not in occ and 0 <= t[0] < sx and 0 <= t[1] < sy and 0 <= t[2] < sz:
                        occ[t] = col; added.append(t)
    if APPLY and added:
        body = struct.pack('<I', len(occ)) + b''.join(
            bytes((x, y, z, c)) for (x, y, z), c in occ.items())
        out = bytearray()
        for cid, o, cs in ch:
            payload = body if cid == b'XYZI' else bytes(d[o:o+cs])
            out += cid + struct.pack('<II', len(payload), 0) + payload
        final = bytes(d[:8]) + b'MAIN' + struct.pack('<II', 0, len(out)) + bytes(out)
        open(path, 'wb').write(final)
    tot_a += adrift0; tot_w += len(added)
    print('%-14s %8d %9d %8d %8d' % (os.path.basename(path)[:-4], n, adrift0, clusters0, len(added)))
print('%-14s %8s %9d %8s %8d' % ('ALL', '', tot_a, '', tot_w))
print(APPLY and 'APPLIED' or 'measured only - pass --apply to weld')
