#!/usr/bin/env python3
"""mushroom.vox -> mushroom.json  (run from the repo root: py tools/voxelize_mushroom.py)

The game loads game/assets/decoration/mushroom.json, NOT the .vox — the .vox is the
authoring file. Re-run this after editing it or the change never reaches the game.

The .vox holds one MODEL per mushroom, laid out in MagicaVoxel's scene graph (nTRN
translations). This flattens that layout into the single sparse cluster stampMush
stamps: model x -> world x, model y -> world z, model z -> world HEIGHT (MagicaVoxel
is z-up), every mushroom's base pulled down to z = 0 so it sits ON the ground rather
than at whatever height it was authored at.
"""
import json, os, struct, sys
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'game', 'assets', 'decoration', 'mushroom.vox')
DST = os.path.join(ROOT, 'game', 'assets', 'decoration', 'mushroom.json')


def read_str(b, o):
    n = struct.unpack('<I', b[o:o + 4])[0]
    return b[o + 4:o + 4 + n].decode('utf-8', 'replace'), o + 4 + n


def read_dict(b, o):
    n = struct.unpack('<I', b[o:o + 4])[0]
    o += 4
    m = {}
    for _ in range(n):
        k, o = read_str(b, o)
        v, o = read_str(b, o)
        m[k] = v
    return m, o


def parse(path):
    d = open(path, 'rb').read()
    if d[:4] != b'VOX ':
        raise SystemExit('not a .vox file: ' + path)
    _, main_children = struct.unpack('<II', d[12:20])
    off, end = 20, 20 + main_children
    models, pal, nodes = [], None, {}
    size = None
    while off < end:
        cid = d[off:off + 4].decode()
        n, m = struct.unpack('<II', d[off + 4:off + 12])
        cs = off + 12
        if cid == 'SIZE':
            size = struct.unpack('<III', d[cs:cs + 12])
        elif cid == 'XYZI':
            cnt = struct.unpack('<I', d[cs:cs + 4])[0]
            vox = [struct.unpack('<BBBB', d[cs + 4 + j * 4: cs + 8 + j * 4]) for j in range(cnt)]
            models.append({'size': size, 'vox': vox})
        elif cid == 'RGBA':
            pal = [struct.unpack('<BBBB', d[cs + j * 4: cs + 4 + j * 4]) for j in range(256)]
        elif cid == 'nTRN':
            nid = struct.unpack('<I', d[cs:cs + 4])[0]
            o = cs + 4
            _, o = read_dict(d, o)
            child, _res, _layer, nfr = struct.unpack('<iiiI', d[o:o + 16])
            o += 16
            frames = []
            for _ in range(nfr):
                f, o = read_dict(d, o)
                frames.append(f)
            t = [0, 0, 0]
            if frames and '_t' in frames[0]:
                t = [int(v) for v in frames[0]['_t'].split()]
            nodes[nid] = ('nTRN', child, t)
        elif cid == 'nGRP':
            nid = struct.unpack('<I', d[cs:cs + 4])[0]
            o = cs + 4
            _, o = read_dict(d, o)
            cnt = struct.unpack('<I', d[o:o + 4])[0]
            o += 4
            kids = [struct.unpack('<I', d[o + 4 * i:o + 4 * i + 4])[0] for i in range(cnt)]
            nodes[nid] = ('nGRP', kids, None)
        elif cid == 'nSHP':
            nid = struct.unpack('<I', d[cs:cs + 4])[0]
            o = cs + 4
            _, o = read_dict(d, o)
            cnt = struct.unpack('<I', d[o:o + 4])[0]
            o += 4
            ms = []
            for _ in range(cnt):
                ms.append(struct.unpack('<I', d[o:o + 4])[0])
                o += 4
                _, o = read_dict(d, o)
            nodes[nid] = ('nSHP', ms, None)
        off = cs + n + m
    return models, pal, nodes


def placements(nodes):
    """walk the scene graph -> [(model index, world translation of the model's centre)]"""
    out = []

    def walk(nid, acc):
        kind, payload, t = nodes[nid]
        if kind == 'nTRN':
            acc = [acc[i] + t[i] for i in range(3)]
            walk(payload, acc)
        elif kind == 'nGRP':
            for k in payload:
                walk(k, acc)
        else:
            for mi in payload:
                out.append((mi, acc))

    walk(0, [0, 0, 0])
    return out


def main():
    models, pal, nodes = parse(SRC)
    place = placements(nodes) if nodes else [(i, [0, 0, 0]) for i in range(len(models))]
    print('%d model(s), %d placement(s)' % (len(models), len(place)))

    # MagicaVoxel centres a model on its translation, so its min corner is t - (size >> 1).
    cells = {}                                           # (wx, wy, wz) -> .vox palette index
    for mi, t in place:
        m = models[mi]
        sx, sy, sz = m['size']
        ox, oy, oz = t[0] - (sx >> 1), t[1] - (sy >> 1), t[2] - (sz >> 1)
        for x, y, z, c in m['vox']:
            cells[(ox + x, oy + y, oz + z)] = c
        print('  model %d %s at %s -> x %d..%d  y %d..%d  z %d..%d'
              % (mi, m['size'], t, ox, ox + sx - 1, oy, oy + sy - 1, oz, oz + sz - 1))

    xs = [p[0] for p in cells]; ys = [p[1] for p in cells]; zs = [p[2] for p in cells]
    x0, y0, z0 = min(xs), min(ys), min(zs)
    sx, sy, sz = max(xs) - x0 + 1, max(ys) - y0 + 1, max(zs) - z0 + 1

    # ── EVERY MUSHROOM ON THE GROUND ── each connected body is one mushroom; drop it by its own
    # lowest voxel so it rests on z = 0. Authored heights differ, and a body left floating is the
    # bug this is here to stop. (The game grounds each body to its OWN terrain height at stamp
    # time as well — this only guarantees the model itself starts flush.)
    occ = {(p[0] - x0, p[1] - y0, p[2] - z0): c for p, c in cells.items()}
    seen, bodies = set(), []
    for start in occ:
        if start in seen:
            continue
        body, q = [], deque([start])
        seen.add(start)
        while q:
            p = q.popleft(); body.append(p)
            for d in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                n = (p[0] + d[0], p[1] + d[1], p[2] + d[2])
                if n in occ and n not in seen:
                    seen.add(n); q.append(n)
        bodies.append(body)
    bodies.sort(key=len, reverse=True)
    grounded = {}
    for b in bodies:
        drop = min(p[2] for p in b)
        for p in b:
            grounded[(p[0], p[1], p[2] - drop)] = occ[p]
        print('  body: %5d vox  dropped %d to rest on z=0' % (len(b), drop))
    occ = grounded
    sz = max(p[2] for p in occ) + 1

    used = sorted({c for c in occ.values()})
    rgb = [list(pal[c - 1][:3]) for c in used]           # XYZI indices are 1-based into RGBA
    idx = {c: i for i, c in enumerate(used)}
    if len(rgb) > 24:
        raise SystemExit('%d shades — the shared 256-entry world palette will not take that many '
                         '(quantize the art first)' % len(rgb))

    vox = sorted(x | (y << 8) | (z << 16) | (idx[c] << 24) for (x, y, z), c in occ.items())
    out = {'pal': rgb, 'sx': sx, 'sy': sy, 'sz': sz, 'vox': vox}
    with open(DST, 'w') as f:
        json.dump(out, f, separators=(',', ':'))
    print('wrote %s  %dx%dx%d  %d voxels  %d shades' % (DST, sx, sy, sz, len(vox), len(rgb)))


if __name__ == '__main__':
    main()
