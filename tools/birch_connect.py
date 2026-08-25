# READ-ONLY AUDIT: how connected is each birch model, in the two graphs the felling flood can use?
# The fell decides "what is still attached to the root" with a 6-connected flood, plus a 26-glue for
# cells that are only diagonally adjacent. A crown that reaches the trunk in NEITHER graph is orphaned
# where it stands: it never becomes a rigid body when the trunk is cut, it stays in W, and it floats.
import glob, os, struct, sys
from collections import deque

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
F6 = [(1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)]
F26 = [(x,y,z) for x in (-1,0,1) for y in (-1,0,1) for z in (-1,0,1) if x or y or z]

def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i+4]; cs, ks = struct.unpack_from('<II', d, i+4); i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out

def reach(occ, seeds, nb):
    seen = set(seeds); q = deque(seeds)
    while q:
        x, y, z = q.popleft()
        for dx, dy, dz in nb:
            p = (x+dx, y+dy, z+dz)
            if p in occ and p not in seen: seen.add(p); q.append(p)
    return len(seen)

print('%-6s %8s %8s %8s %8s' % ('tree', 'voxels', 'root6%', 'root26%', 'big6%'))
rows = []
for path in sorted(glob.glob(os.path.join(DIR, '*.vox')),
                   key=lambda p: int(os.path.basename(p)[:-4]) if os.path.basename(p)[:-4].isdigit() else 999):
    base = os.path.basename(path)[:-4]
    if not base.isdigit(): continue
    d = open(path, 'rb').read()
    ch = chunks(d)
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    if len(xyzi) != 1: print('%-6s multi-part, skipped' % base); continue
    o0 = xyzi[0][0]
    n = struct.unpack_from('<I', d, o0)[0]
    occ = set()
    for i in range(n):
        x, y, z, c = struct.unpack_from('<BBBB', d, o0 + 4 + i * 4)
        occ.add((x, y, z))
    zmin = min(p[2] for p in occ)
    seeds = [p for p in occ if p[2] <= zmin + 2]          # the root band, as the flood seeds it
    r6, r26 = reach(occ, seeds, F6), reach(occ, seeds, F26)
    # largest 6-connected component, for scale
    seen, big = set(), 0
    for k in occ:
        if k in seen: continue
        q, cnt = deque([k]), 0; seen.add(k)
        while q:
            x, y, z = q.popleft(); cnt += 1
            for dx, dy, dz in F6:
                p = (x+dx, y+dy, z+dz)
                if p in occ and p not in seen: seen.add(p); q.append(p)
        big = max(big, cnt)
    N = len(occ)
    rows.append((base, N, 100.0*r6/N, 100.0*r26/N, 100.0*big/N))
    print('%-6s %8d %7.1f%% %7.1f%% %7.1f%%' % rows[-1])
if rows:
    print('-' * 44)
    print('%-6s %8d %7.1f%% %7.1f%% %7.1f%%' % ('mean', sum(r[1] for r in rows)//len(rows),
          sum(r[2] for r in rows)/len(rows), sum(r[3] for r in rows)/len(rows),
          sum(r[4] for r in rows)/len(rows)))
