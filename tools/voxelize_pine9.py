"""Voxelize game/assets/foilage/pine_trees/EuropeanPine.obj into NINE pine trees, 50 feet tall.

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH - numpy and Pillow live there:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_pine9.py

Input:  game/assets/foilage/pine_trees/EuropeanPine.obj   (one scene holding nine trees in a row)
        source/pine9/tex/*.png                            (albedo maps unpacked from Textures.zip)
Output: game/assets/foilage/pine9/pine_1..9.vox           (shared palette, ids 1..N)
        source/pine9/pine9.json                           (the bake: ramps + per-tree voxel lists)

── NINE TREES OUT OF ONE FILE ──────────────────────────────────────────────────────────────
The OBJ is a single scene with 46 objects laid out in a row along X, and they are PARTS, not
trees: Leaves.00N, Bark.00N, Trunk.00N, Knots.00N, Blend.00N. There are exactly nine Trunk
objects, nine Leaves and nine Blend, so the tree count is not a guess - the parts are grouped
by which trunk centre they sit nearest in X, which is what makes nine trees out of 46 objects.

── AXES ── the OBJ is Y-UP (every Trunk starts at y = 0 and rises). The game's convention is
x = width, y = depth, z = height, which voxelize_rocks.py and gen_birch.py also write, so the
axis swap here is game(x, y, z) = model(x, z, y). Getting this wrong lays the forest on its side.

── 50 FEET ── the engine's voxel is 10 cm (see voxelize_fir.py), so 50 ft = 15.24 m = 152
voxels. The nine source trees are NOT the same height (24.0 to 35.6 model units), and each is
scaled to 152 on its OWN height rather than by one shared factor, because the request was nine
trees of 50 feet and a shared factor would deliver one 50-foot tree and eight shorter ones.
Proportions are kept: the same scalar drives all three axes.

── WHY SAMPLES AND NOT CORNERS ── a leaf card is a quad with a cutout alpha, and most of its
area is transparent. Testing the alpha at triangle CORNERS throws the whole canopy away
(recorded against the fir bake); testing it per SAMPLE keeps exactly the needles the texture
draws. Triangles are sampled at a density set by their own area in voxels, so a leaf spray
gets hundreds of samples and a trunk quad gets a few - the cost follows the detail.
"""
import os, sys, json, struct, collections
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OBJ  = os.path.join(ROOT, 'game/assets/foilage/pine_trees/EuropeanPine.obj')
TEX  = os.path.join(ROOT, 'source/pine9/tex')
OUT  = os.path.join(ROOT, 'game/assets/foilage/pine9')
BAKE = os.path.join(ROOT, 'source/pine9/pine9.json')

TALL_VOX   = 152          # 50 ft at the engine's 10 cm voxel
ALPHA_MIN  = 128          # leaf-card cutout
SAMPLE_DEN = 14.0         # samples per square voxel of triangle area. At 2.2 the trees came out
                          # 1.2% full against pine5.vox's 5.8% - a surface the sampler kept MISSING,
                          # not a sparse tree, so the canopy read as scattered needles.
K_BARK     = 5            # shared shades, all nine trees
K_NEEDLE   = 5

# material -> (texture file, class).  'n' = needle/foliage, 'w' = wood.
MAT = {
    'A_Branch_(04)': ('branch.png',   'n'),
    'A_Pine_Bark':   ('pinebark.png', 'w'),
    'A_Base':        ('basebark.png', 'w'),
    'A_BaseBlend':   ('blend.png',    'w'),
    'A_Stump':       ('stump.png',    'w'),
    'stump01':       ('stump.png',    'w'),
    'stump01.001':   ('stump.png',    'w'),
    'stump01.003':   ('stump.png',    'w'),
    'stump01.004':   ('stump.png',    'w'),
}


def load_tex():
    out = {}
    for f in sorted({m[0] for m in MAT.values()}):
        im = Image.open(os.path.join(TEX, f)).convert('RGBA')
        out[f] = np.asarray(im, dtype=np.uint8)
        print('  tex %-14s %s' % (f, out[f].shape))
    return out


def parse_obj():
    """-> V (n,3) float32, VT (m,2) float32, tris: list of (obj, mat, (i0,i1,i2), (t0,t1,t2))"""
    V, VT, tris = [], [], []
    cur_o, cur_m = None, None
    with open(OBJ, 'r', errors='ignore') as f:
        for line in f:
            if line.startswith('v '):
                p = line.split(); V.append((float(p[1]), float(p[2]), float(p[3])))
            elif line.startswith('vt '):
                p = line.split(); VT.append((float(p[1]), float(p[2])))
            elif line.startswith('o '):
                cur_o = line[2:].strip()
            elif line.startswith('usemtl'):
                cur_m = line.split(None, 1)[1].strip()
            elif line.startswith('f '):
                p = line.split()[1:]
                idx = []
                for t in p:
                    a = t.split('/')
                    vi = int(a[0]); vi = vi - 1 if vi > 0 else len(V) + vi
                    ti = -1
                    if len(a) > 1 and a[1]:
                        ti = int(a[1]); ti = ti - 1 if ti > 0 else len(VT) + ti
                    idx.append((vi, ti))
                for k in range(1, len(idx) - 1):        # fan-triangulate
                    a, b, c = idx[0], idx[k], idx[k + 1]
                    tris.append((cur_o, cur_m, (a[0], b[0], c[0]), (a[1], b[1], c[1])))
    return (np.asarray(V, dtype=np.float32), np.asarray(VT, dtype=np.float32), tris)


def group_trees(V, tris):
    """Nine trunk centres in X; every object joins the nearest one."""
    per = collections.defaultdict(list)
    for o, m, vi, ti in tris:
        per[o].append(vi)
    cen = {}
    for o, faces in per.items():
        idx = np.unique(np.asarray(faces, dtype=np.int64).ravel())
        cen[o] = float(V[idx, 0].mean())
    trunks = sorted([o for o in per if o.startswith('Trunk')], key=lambda o: cen[o])
    assert len(trunks) == 9, 'expected nine trunks, found %d' % len(trunks)
    tx = [cen[t] for t in trunks]
    owner = {}
    for o in per:
        owner[o] = int(np.argmin([abs(cen[o] - t) for t in tx]))
    return owner, trunks


def sample_tree(V, VT, tris, texs, keep):
    """Rasterise one tree's triangles into {(x,y,z): [rsum,gsum,bsum,n,needle_n]}."""
    acc = {}
    # group triangles by material so each batch samples ONE texture
    by_mat = collections.defaultdict(list)
    for o, m, vi, ti in tris:
        if o in keep:
            by_mat[m].append((vi, ti))
    # tree bbox and scale, over the triangles we are actually keeping
    allv = np.unique(np.concatenate([np.asarray([t[0] for t in v], dtype=np.int64).ravel()
                                     for v in by_mat.values()]))
    lo = V[allv].min(axis=0); hi = V[allv].max(axis=0)
    scale = TALL_VOX / float(hi[1] - lo[1])             # model Y is UP
    for mat, lst in by_mat.items():
        texf, cls = MAT[mat]
        T = texs[texf]; th, tw = T.shape[0], T.shape[1]
        vi = np.asarray([a for a, b in lst], dtype=np.int64)
        ti = np.asarray([b for a, b in lst], dtype=np.int64)
        P = (V[vi] - lo) * scale                        # (n,3,3) in voxel units
        # area in voxel^2 -> sample count
        e1 = P[:, 1] - P[:, 0]; e2 = P[:, 2] - P[:, 0]
        area = 0.5 * np.linalg.norm(np.cross(e1, e2), axis=1)
        n = np.clip(np.ceil(area * SAMPLE_DEN), 1, 6000).astype(np.int64)
        # chunk so the flattened sample array stays a sane size
        start = 0
        while start < len(n):
            end = start
            tot = 0
            while end < len(n) and tot < 4_000_000:
                tot += int(n[end]); end += 1
            rep = np.repeat(np.arange(start, end), n[start:end])
            r1 = np.random.random_sample(len(rep)).astype(np.float32)
            r2 = np.random.random_sample(len(rep)).astype(np.float32)
            su = np.sqrt(r1)
            bu = (1.0 - su); bv = su * (1.0 - r2); bw = su * r2
            pos = (P[rep, 0] * bu[:, None] + P[rep, 1] * bv[:, None] + P[rep, 2] * bw[:, None])
            uvs = None
            if (ti[rep] >= 0).all():
                UV = VT[ti[rep]]                        # (k,3,2)
                uvs = (UV[:, 0] * bu[:, None] + UV[:, 1] * bv[:, None] + UV[:, 2] * bw[:, None])
            if uvs is None:
                col = np.full((len(rep), 4), 200, dtype=np.uint8)
            else:
                px = np.mod((uvs[:, 0] * tw).astype(np.int64), tw)
                py = np.mod(((1.0 - uvs[:, 1]) * th).astype(np.int64), th)
                col = T[py, px]
            if T.shape[2] == 4:
                m_ok = col[:, 3] >= ALPHA_MIN
                pos = pos[m_ok]; col = col[m_ok]
            # game axes: x = model x, y = model z, z = model y(up)
            gx = np.floor(pos[:, 0]).astype(np.int32)
            gy = np.floor(pos[:, 2]).astype(np.int32)
            gz = np.floor(pos[:, 1]).astype(np.int32)
            for x, y, z, c in zip(gx.tolist(), gy.tolist(), gz.tolist(), col[:, :3].tolist()):
                a = acc.get((x, y, z))
                if a is None:
                    acc[(x, y, z)] = [c[0], c[1], c[2], 1, 1 if cls == 'n' else 0]
                else:
                    a[0] += c[0]; a[1] += c[1]; a[2] += c[2]; a[3] += 1
                    if cls == 'n': a[4] += 1
            start = end
    return acc


def is_green(c):
    return c[1] > c[0] + 6 and c[1] > c[2] + 6


def kmeans(X, k, iters=28):
    X = X.astype(np.float32)
    rs = np.random.RandomState(7)
    C = X[rs.choice(len(X), k, replace=False)].copy()
    for _ in range(iters):
        d = ((X[:, None, :] - C[None, :, :]) ** 2).sum(axis=2)
        lab = d.argmin(axis=1)
        for i in range(k):
            m = lab == i
            if m.any(): C[i] = X[m].mean(axis=0)
    return C, lab


def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def write_vox(path, vox, pal, sx, sy, sz):
    # ── THE RGBA CHUNK IS SHIFTED BY ONE ── MagicaVoxel stores the colour for voxel index ci at
    # POSITION ci-1 (index 0 is not addressable; a 0 voxel is empty). palette.js reads it as
    # vpal[(ci - 1) * 4], so the table written here must start at the colour of index 1 or every
    # voxel comes out wearing its neighbour's shade - and with a ramp, that is a silent bug.
    pal = pal[1:]
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    vv = b''.join(struct.pack('<BBBB', x, y, z, i) for (x, y, z, i) in vox)
    xyzi = chunk(b'XYZI', struct.pack('<I', len(vox)) + vv)
    rgba = b''
    for i in range(256):
        c = pal[i] if i < len(pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150)
                           + chunk(b'MAIN', b'', size + xyzi + chunk(b'RGBA', rgba)))


def main():
    print('reading', os.path.relpath(OBJ, ROOT))
    texs = load_tex()
    V, VT, tris = parse_obj()
    print('  %d verts, %d uvs, %d triangles' % (len(V), len(VT), len(tris)))
    owner, trunks = group_trees(V, tris)
    members = collections.defaultdict(set)
    for o, t in owner.items():
        members[t].add(o)
    trees = []
    for t in range(9):
        acc = sample_tree(V, VT, tris, texs, members[t])
        trees.append(acc)
        zs = [k[2] for k in acc]
        print('  tree %d  %-58s %6d voxels  h=%d' %
              (t + 1, ','.join(sorted(members[t])[:4]), len(acc), max(zs) - min(zs) + 1))
    # ── ONE RAMP FOR THE WHOLE STAND ── the nine trees are one species and share a bark and a
    # needle ramp, so the forest costs K_BARK + K_NEEDLE palette ids in total rather than nine
    # times that, and a felled trunk beside a standing one is the same wood.
    # ── A LEAF-CARD TEXEL IS NOT AUTOMATICALLY A NEEDLE ── the branch card draws the twig it grows
    # on as well as the needles, so classifying by MATERIAL alone put three browns in a five-shade
    # needle ramp and the canopy came out brown. A voxel joins the needle ramp only if the leaf
    # material won it AND the colour it actually sampled is green; the card's woody texels fall
    # through to the bark ramp, which is what they are.
    wood, need = [], []
    for acc in trees:
        for a in acc.values():
            c = (a[0] / a[3], a[1] / a[3], a[2] / a[3])
            (need if (a[4] * 2 >= a[3] and is_green(c)) else wood).append(c)
    wood = np.asarray(wood, dtype=np.float32); need = np.asarray(need, dtype=np.float32)
    rs = np.random.RandomState(3)
    Cw, _ = kmeans(wood[rs.choice(len(wood), min(40000, len(wood)), replace=False)], K_BARK)
    Cn, _ = kmeans(need[rs.choice(len(need), min(40000, len(need)), replace=False)], K_NEEDLE)
    Cw = np.clip(np.round(Cw), 0, 255).astype(int)
    Cn = np.clip(np.round(Cn), 0, 255).astype(int)
    Cw = Cw[np.argsort(Cw.sum(axis=1))]                 # dark -> light, so a ramp reads as one
    Cn = Cn[np.argsort(Cn.sum(axis=1))]
    pal = [[0, 0, 0]] + [list(map(int, c)) for c in Cw] + [list(map(int, c)) for c in Cn]
    print('  bark ramp  ', [list(c) for c in Cw])
    print('  needle ramp', [list(c) for c in Cn])
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(os.path.dirname(BAKE), exist_ok=True)
    bake = {'tall': TALL_VOX, 'bark': [list(map(int, c)) for c in Cw],
            'needle': [list(map(int, c)) for c in Cn], 'trees': []}
    for t, acc in enumerate(trees):
        xs = [k[0] for k in acc]; ys = [k[1] for k in acc]; zs = [k[2] for k in acc]
        x0, y0, z0 = min(xs), min(ys), min(zs)
        sx, sy, sz = max(xs) - x0 + 1, max(ys) - y0 + 1, max(zs) - z0 + 1
        assert max(sx, sy, sz) < 256, 'tree %d is %dx%dx%d - past the .vox 255 limit' % (t + 1, sx, sy, sz)
        vox, jvox = [], []
        for (x, y, z), a in acc.items():
            c = np.asarray([a[0] / a[3], a[1] / a[3], a[2] / a[3]], dtype=np.float32)
            if a[4] * 2 >= a[3] and is_green(c):
                i = int(((Cn - c) ** 2).sum(axis=1).argmin()) + 1 + K_BARK
            else:
                i = int(((Cw - c) ** 2).sum(axis=1).argmin()) + 1
            vox.append((x - x0, y - y0, z - z0, i))
            jvox.append([x - x0, y - y0, z - z0, i])
        p = os.path.join(OUT, 'pine_%d.vox' % (t + 1))
        write_vox(p, vox, pal, sx, sy, sz)
        bake['trees'].append({'name': 'pine_%d' % (t + 1), 'sx': sx, 'sy': sy, 'sz': sz, 'vox': jvox})
        print('  wrote %-12s %3d x %3d x %3d  %6d voxels  %5.0f KB'
              % ('pine_%d.vox' % (t + 1), sx, sy, sz, len(vox), os.path.getsize(p) / 1024))
    json.dump(bake, open(BAKE, 'w'))
    print('bake -> %s (%.1f MB)' % (os.path.relpath(BAKE, ROOT), os.path.getsize(BAKE) / 1e6))


if __name__ == '__main__':
    main()
