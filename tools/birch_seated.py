# READ-ONLY AUDIT: what a birch loses when it is SEATED, and what that strands.
#
# tools/birch_connect.py flooded each model from its own lowest voxels and reported 100% connected. That is
# true of the MODEL and not of the TREE: stampBirch seats the bole on the ground (gy = groundMin - sink - tbz)
# and writes in MODE 1, so every course below the trunk's own base is inside the hill and REFUSED. Anything
# whose only path to the trunk ran through those buried courses is cut off the moment the tree is planted.
# This floods only the part that actually reaches the world and reports what is left hanging.
import glob, os, struct, sys
from collections import deque

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
# tbz per model, in BIRCHV order (sorted short-to-tall), read from __vb.birchModel
TBZ = [20, 7, 0, 6, 3, 0, 30, 44, 0, 29, 0, 0, 0, 4, 0, 0]
F26 = [(x, y, z) for x in (-1, 0, 1) for y in (-1, 0, 1) for z in (-1, 0, 1) if x or y or z]

def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i+4]; cs, ks = struct.unpack_from('<II', d, i+4); i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out

paths = sorted([q for q in glob.glob(os.path.join(DIR, '*.vox')) if os.path.basename(q)[:-4].isdigit()],
               key=lambda q: int(os.path.basename(q)[:-4]))
# BIRCHV is sorted short-to-tall, which is the order TBZ is in; read heights to match files to that order
info = []
for p in paths:
    d = open(p, 'rb').read()
    sz = [struct.unpack_from('<III', d, o) for cid, o, cs in chunks(d) if cid == b'SIZE'][0]
    info.append((sz[2], p))
info.sort()

print('%-6s %5s %5s %9s %11s %9s' % ('model', 'tbz', 'sz', 'voxels', 'seated', 'STRANDED'))
tot_s = tot_v = 0
for k, (h, p) in enumerate(info):
    d = open(p, 'rb').read()
    o0 = [o for cid, o, cs in chunks(d) if cid == b'XYZI'][0]
    n = struct.unpack_from('<I', d, o0)[0]
    occ = set()
    for i in range(n):
        x, y, z, c = struct.unpack_from('<BBBB', d, o0 + 4 + i * 4)
        occ.add((x, y, z))
    tbz = TBZ[k] if k < len(TBZ) else 0
    kept = {q for q in occ if q[2] >= tbz}          # what survives the seat: everything at or above the bole base
    if not kept:
        print('%-6s %5d %5d %9d  (nothing kept)' % (k, tbz, h, len(occ))); continue
    z0 = min(q[2] for q in kept)
    seeds = [q for q in kept if q[2] <= z0 + 1]     # the courses that meet the ground
    seen = set(seeds); dq = deque(seeds)
    while dq:
        x, y, z = dq.popleft()
        for dx, dy, dz in F26:
            q = (x+dx, y+dy, z+dz)
            if q in kept and q not in seen: seen.add(q); dq.append(q)
    stranded = len(kept) - len(seen)
    tot_s += stranded; tot_v += len(kept)
    print('%-6s %5d %5d %9d %11d %9d  %5.1f%%' % (k, tbz, h, len(occ), len(kept), stranded,
                                                  100.0 * stranded / len(kept)))
print('-' * 56)
print('%-6s %5s %5s %9s %11d %9d  %5.1f%%' % ('all', '', '', '', tot_v, tot_s, 100.0 * tot_s / max(1, tot_v)))
