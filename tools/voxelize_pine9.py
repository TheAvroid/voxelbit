"""Voxelize game/assets/foilage/pine_trees/EuropeanPine.obj into NINE pine trees, 75 to 100 feet tall.

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

── 75 TO 100 FEET ── the engine's voxel is 10 cm (see voxelize_fir.py), so the nine targets
run 228 voxels (75 ft, 22.86 m) to 305 (100 ft, 30.48 m), evenly spaced. Each tree is scaled to
its OWN target off its OWN measured height rather than by one shared factor, so every target is
met exactly. Proportions are kept: the same scalar drives all three axes.
(Was 152 = 50 ft, then a single 228, then a single 305 - see the TALL_FT note for why it is a
range now, and for why tree N gets the height it gets.)

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

VOX_M      = 0.1          # the engine's voxel, in metres (scene/voxelworld.h)
FT_M       = 0.3048
TALL_FT_LO = 75.0         # ── NINE HEIGHTS, NOT ONE (user 2026-09-08: "I want you to rebake the 9 at
TALL_FT_HI = 100.0        # different height. I want you to bake them anywhere from 75 feet to 100 feet.
                          # to allow for more variation") ── every bake before this one normalised all
                          # nine trees onto a SINGLE constant, so the stand was a row of pines of identical
                          # height and the only variation left in it was crown shape. A real stand is not
                          # that, and at this renderer's draw distance a flat skyline is the tell. The nine
                          # targets are now spread evenly across this range.
                          #
                          # WHICH TREE GETS WHICH HEIGHT IS NOT ARBITRARY. The nine source trees are already
                          # different heights in the OBJ (24.0 to 35.6 model units) and each carries a crown
                          # that suits its own stature - the tall ones are lankier. So the targets are dealt
                          # out by each tree's OWN natural rank: the naturally shortest source tree becomes
                          # the 75-footer and the naturally tallest the 100-footer. Handing them out at
                          # random would put a lanky crown on the stubbiest trunk and read as a mistake
                          # rather than as variation. tree_span measures this BEFORE any sampling, off the
                          # triangles the bake will actually keep, so it is deterministic and a re-bake
                          # reproduces the same stand.
                          #
                          # THE NUMBERING IS DELIBERATELY LEFT ALONE (user: "dont change anything about the
                          # frequency of the trees or anything"). pine_N is still the Nth trunk along X in
                          # the OBJ, exactly as before, so a scatter that hashes to model index N keeps the
                          # same tree in the same place and only its HEIGHT changes. Renumbering the set
                          # short-to-tall would have reshuffled the whole forest to no purpose.
                          #
                          # WHAT IT COSTS: nothing - it REFUNDS. This is a SURFACE voxelizer, so a tree's
                          # voxel count follows its AREA and moves as the square of the scalar. Against the
                          # nine-at-305 set that shipped, eight of the nine come DOWN, so the forest gets
                          # cheaper to trace while gaining the variation.
TALL_VOX_LO = int(round(TALL_FT_LO * FT_M / VOX_M))    # 228
TALL_VOX_HI = int(round(TALL_FT_HI * FT_M / VOX_M))    # 305
                          # ── AND 305 DOES NOT FIT IN A .vox COORDINATE ── an XYZI record packs each axis
                          # in ONE BYTE, so no single model may exceed 256. write_vox therefore SPLITS a
                          # tall tree into stacked parts plus the nTRN/nGRP/nSHP scene graph that places
                          # them, exactly as voxelize_birch_forest.py does, and scene/vox.h composes them
                          # back into one grid on load. A tree that fits in 255 is still written as one
                          # plain model - no scene graph, no behaviour change. Only Z is ever split: X and Y
                          # stay under the byte on their own (the widest crown here measures ~134).
ALPHA_MIN  = 128          # leaf-card cutout
SAMPLE_DEN = 14.0         # samples per square voxel of triangle area. At 2.2 the trees came out
                          # 1.2% full against pine5.vox's 5.8% - a surface the sampler kept MISSING,
                          # not a sparse tree, so the canopy read as scattered needles.
K_BARK     = 5            # shared shades, all nine trees
K_NEEDLE   = 5
LOCK_RAMP  = True         # ── KEEP THE SHIPPED COLOURS ACROSS A RE-BAKE (user 2026-09-03: "I want you to
                          # keep the current color schemes") ── the ten shades are k-means centroids over the
                          # voxels the sampler happened to draw, and sample_tree draws them from the UNSEEDED
                          # global numpy RNG. So the ramp is a function of the sampling, and ANY re-bake moves
                          # it a little even at the same height; at a new height it moves more, because 3.4x
                          # the voxels re-weights every cluster. With this set, the ramps are read back from
                          # the previous bake in source/pine9/pine9.json and reused verbatim, and only the
                          # GEOMETRY is rebuilt. That matters beyond the .vox: palette.js flattens the bark
                          # ramp toward its own computed mean (BARK_FLAT) and mints ids through addCol, whose
                          # PAL_TOL dedups within 6 - so a couple of units of drift here can silently merge two
                          # shades and cost the ramp a step, against a palette already standing at 256/256.
                          # Set False to re-cluster from scratch (a new species, or new source textures).

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


def tree_span(V, tris, keep):
    """One tree's own height in MODEL units, over exactly the triangles the bake will keep.

    Measured BEFORE any sampling, because the height targets are dealt out by natural rank and the
    rank has to exist before the first tree is rasterised (see TALL_FT_LO). Model Y is UP."""
    faces = [vi for o, m, vi, ti in tris if o in keep]
    idx = np.unique(np.asarray(faces, dtype=np.int64).ravel())
    return float(V[idx, 1].max() - V[idx, 1].min())


def sample_tree(V, VT, tris, texs, keep, tall_vox):
    """Rasterise one tree's triangles into {(x,y,z): [rsum,gsum,bsum,n,needle_n]}.

    tall_vox is THIS tree's target height in voxels, not a shared constant - see TALL_FT_LO."""
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
    scale = tall_vox / float(hi[1] - lo[1])             # model Y is UP; tall_vox is per tree
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


def _s(t):
    b = t.encode('utf-8')
    return struct.pack('<I', len(b)) + b


def _dict(d):
    out = struct.pack('<I', len(d))
    for k, v in d.items():
        out += _s(k) + _s(v)
    return out


VOXMAX = 256                                           # the .vox single-byte coordinate ceiling


def write_vox(path, vox, pal, sx, sy, sz):
    # ── THE RGBA CHUNK IS SHIFTED BY ONE ── MagicaVoxel stores the colour for voxel index ci at
    # POSITION ci-1 (index 0 is not addressable; a 0 voxel is empty). palette.js reads it as
    # vpal[(ci - 1) * 4], so the table written here must start at the colour of index 1 or every
    # voxel comes out wearing its neighbour's shade - and with a ramp, that is a silent bug.
    pal = pal[1:]
    # ── AND THE MODEL SPLITS IF IT IS TALLER THAN A BYTE ── see TALL_VOX_HI. Parts are cut on Z only,
    # at VOXMAX; a tree short enough to fit stays a single plain model with no scene graph at all.
    parts = []
    for z0 in range(0, sz, VOXMAX):
        pz = [q for q in vox if z0 <= q[2] < z0 + VOXMAX]
        if pz:
            parts.append((z0, min(VOXMAX, sz - z0), pz))
    body = b''
    for z0, szp, pz in parts:
        vv = b''.join(struct.pack('<BBBB', x, y, z - z0, i) for (x, y, z, i) in pz)
        body += chunk(b'SIZE', struct.pack('<III', sx, sy, szp))
        body += chunk(b'XYZI', struct.pack('<I', len(pz)) + vv)
    if len(parts) > 1:
        # root nTRN(0) -> nGRP(1) -> [ nTRN(2+2i) -> nSHP(3+2i) ] per part. MagicaVoxel places a model
        # by its CENTRE, so the translation is the centre of that part's box in tree-local space.
        body += chunk(b'nTRN', struct.pack('<i', 0) + _dict({}) + struct.pack('<iiii', 1, -1, -1, 1) + _dict({}))
        kids = b''.join(struct.pack('<i', 2 + 2 * i) for i in range(len(parts)))
        body += chunk(b'nGRP', struct.pack('<i', 1) + _dict({}) + struct.pack('<I', len(parts)) + kids)
        for i, (z0, szp, pz) in enumerate(parts):
            t = '%d %d %d' % (0, 0, z0 + szp // 2 - sz // 2)
            body += chunk(b'nTRN', struct.pack('<i', 2 + 2 * i) + _dict({}) +
                          struct.pack('<iiii', 3 + 2 * i, -1, 0, 1) + _dict({'_t': t}))
            body += chunk(b'nSHP', struct.pack('<i', 3 + 2 * i) + _dict({}) +
                          struct.pack('<I', 1) + struct.pack('<i', i) + _dict({}))
    rgba = b''
    for i in range(256):
        c = pal[i] if i < len(pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    body += chunk(b'RGBA', rgba)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))
    return len(parts)


def main():
    print('reading', os.path.relpath(OBJ, ROOT))
    texs = load_tex()
    V, VT, tris = parse_obj()
    print('  %d verts, %d uvs, %d triangles' % (len(V), len(VT), len(tris)))
    owner, trunks = group_trees(V, tris)
    members = collections.defaultdict(set)
    for o, t in owner.items():
        members[t].add(o)
    # ── PASS ONE: THE NATURAL HEIGHTS, SO THE TARGETS CAN BE DEALT OUT BY RANK ── cheap, it is a
    # bbox over the kept triangles and no rasterising happens here. See TALL_FT_LO for why rank and
    # not random, and for why the file NUMBERING is left exactly as it was.
    nat = [tree_span(V, tris, members[t]) for t in range(9)]
    steps = np.linspace(TALL_VOX_LO, TALL_VOX_HI, 9)
    target = [0] * 9
    for rank, t in enumerate(sorted(range(9), key=lambda i: nat[i])):
        target[t] = int(round(steps[rank]))
    print('  height targets, dealt out by natural rank:')
    for t in range(9):
        print('    pine_%d  natural %6.2f u  ->  %3d vox  %5.2f m  %5.1f ft'
              % (t + 1, nat[t], target[t], target[t] * VOX_M, target[t] * VOX_M / FT_M))
    trees = []
    for t in range(9):
        acc = sample_tree(V, VT, tris, texs, members[t], target[t])
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
    # ── THE RAMPS ── locked to the previous bake by default; see LOCK_RAMP at the top of the file.
    # Read BEFORE anything is written, because source/pine9/pine9.json is the file this run overwrites.
    if LOCK_RAMP:
        if not os.path.exists(BAKE):
            sys.exit('LOCK_RAMP is set but %s does not exist - there is no ramp to reuse. Set it False '
                     'to cluster a fresh one.' % os.path.relpath(BAKE, ROOT))
        prev = json.load(open(BAKE))
        Cw = np.asarray(prev['bark'], dtype=int)
        Cn = np.asarray(prev['needle'], dtype=int)
        # A SHAPE CHECK, NOT A COURTESY: a short ramp would not raise here, it would quietly hand every
        # voxel of the missing step its neighbour's shade through the nearest-colour assignment below.
        if Cw.shape != (K_BARK, 3) or Cn.shape != (K_NEEDLE, 3):
            sys.exit('%s carries a %s bark / %s needle ramp, not %d / %d'
                     % (os.path.relpath(BAKE, ROOT), Cw.shape, Cn.shape, K_BARK, K_NEEDLE))
        print('  ramps LOCKED to the previous bake (%s), geometry rebuilt at %d..%d voxels'
              % (os.path.relpath(BAKE, ROOT), TALL_VOX_LO, TALL_VOX_HI))
    else:
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

        # ── THE BARK RAMP IS FORCED ONTO ONE BROWN HUE (user 2026-08-31: "theres seems to be green along
        # with the browns. remove that green in the wood") ── k-means was doing its job here: real Scots
        # pine bark carries lichen, and both bark sheets have olive in them, so two of the five centroids
        # came back khaki - (92,81,40) and (111,106,43), the second with red and green equal and almost no
        # blue, which is the definition of olive. On a trunk beside three true browns they read as green
        # patches rather than as bark.
        # Averaging cannot fix it and neither can dropping the olive texels: the light/dark STRUCTURE the
        # albedo gives is the thing worth keeping, and it is carried by luminance, not by hue. So the ramp
        # keeps its measured luminance SPAN and gets rebuilt on the hue the good centroids already had -
        # (122,94,68), (141,107,78), (164,125,95) are all within a shade of 1.00 : 0.77 : 0.56.
        # Evenly spaced rather than luminance-preserving, because the olive shades sit at almost the same
        # luminance as the browns (100.3 against 99.4) and mapping each to its own brightness would collapse
        # two steps of a five-step ramp into one colour.
        BARK_HUE = np.array([1.00, 0.77, 0.56], dtype=np.float32)
        LUM = np.array([0.299, 0.587, 0.114], dtype=np.float32)
        lums = Cw.astype(np.float32) @ LUM
        span = np.linspace(lums.min(), lums.max(), K_BARK)
        Cw = np.clip(np.round(np.outer(span / float(BARK_HUE @ LUM), BARK_HUE)), 0, 255).astype(int)
    pal = [[0, 0, 0]] + [list(map(int, c)) for c in Cw] + [list(map(int, c)) for c in Cn]
    print('  bark ramp  ', [list(c) for c in Cw])
    print('  needle ramp', [list(c) for c in Cn])
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(os.path.dirname(BAKE), exist_ok=True)
    bake = {'tall': [int(v) for v in target], 'bark': [list(map(int, c)) for c in Cw],
            'needle': [list(map(int, c)) for c in Cn], 'trees': []}
    for t, acc in enumerate(trees):
        xs = [k[0] for k in acc]; ys = [k[1] for k in acc]; zs = [k[2] for k in acc]
        x0, y0, z0 = min(xs), min(ys), min(zs)
        sx, sy, sz = max(xs) - x0 + 1, max(ys) - y0 + 1, max(zs) - z0 + 1
        # Only Z is split (write_vox), so X and Y still have to fit a byte on their own. The widest
        # crown in this set measures ~134 at 100 ft, so this is headroom rather than a live constraint.
        assert max(sx, sy) < 256, 'tree %d is %d x %d wide - past the .vox 255 limit on X/Y' % (t + 1, sx, sy)
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
        nparts = write_vox(p, vox, pal, sx, sy, sz)
        bake['trees'].append({'name': 'pine_%d' % (t + 1), 'sx': sx, 'sy': sy, 'sz': sz,
                              'tall': target[t], 'vox': jvox})
        print('  wrote %-12s %3d x %3d x %3d  %5.2f m (%5.1f ft)  %6d voxels  %d part%s  %5.0f KB'
              % ('pine_%d.vox' % (t + 1), sx, sy, sz, sz * VOX_M, sz * VOX_M / FT_M, len(vox),
                 nparts, '' if nparts == 1 else 's', os.path.getsize(p) / 1024))
    json.dump(bake, open(BAKE, 'w'))
    print('bake -> %s (%.1f MB)' % (os.path.relpath(BAKE, ROOT), os.path.getsize(BAKE) / 1e6))


if __name__ == '__main__':
    main()
