"""Voxelize source/glb/oak_trees.glb to 10 cm voxels, one model per TREE, emit JSON.

Input:  source/glb/oak_trees.glb                    (source sculpt - gitignored, never shipped)
Output: game/assets/decoration/oak_trees.json       (baked runtime asset the game loads)
  { "pal": [[r,g,b],...], "nbark": N, "trees": [ {"name","sx","sy","sz","wood","vox":[...]} ] }
        game/assets/foilage/oak_trees/oak_N.vox     (the same seven trees, hand-editable)

TWO ARTEFACTS, ONE OF WHICH THE GAME DOES NOT READ. The .vox files exist so a tree can be
opened and edited next to pine5.vox and palm_tree.vox; the GAME loads the .json and only the
.json (src/assets/bow.js). So editing a .vox changes NOTHING on its own - re-run this tool, or
rewire the loader. That is the same arrangement voxelize_cacti.py has, and the same trap.
oak_N.vox is OAKV[N-1] at runtime: both orderings are the height sort below, so the file name
is the index world/terrain.js picks its size tiers with.

Same axes and packing as voxelize_cacti.py / voxelize_rocks.py: vox entries are
x | z<<8 | y<<16 | palIdx<<24, with the source's Y-up mapped to the model's z-up, and
sx/sy/sz = width / depth / height in voxels. See voxelize_rocks.py for the format notes.

THREE THINGS THIS FILE DOES THAT THE OTHER VOXELIZERS DO NOT:

  * ONE TREE = SEVERAL MESHES. oak_trees.glb holds 15 meshes that are really 7 trees, each
    split into a `_branches` mesh and a `_leaves` mesh (Large001 has two leaf meshes). So
    neither SPLIT mode in voxelize_cacti.py is right: 'mesh' hands back 15 half-trees, and
    'components' would fuse the seven crowns wherever they touch. They are grouped by the
    NAME PREFIX instead, which is exactly the author's own grouping.

  * BARK AND LEAF ARE DIFFERENT MATERIALS, not just different colours. The game needs to know
    which voxels are canopy (walk-through, foliaTab) and which are wood (solid, axe-only), and
    a palette id is the only channel a stamped voxel carries. So the two are quantized
    SEPARATELY and the palette is emitted bark-first with `nbark` as the split point:
    pal[0:nbark] is wood, pal[nbark:] is leaf. Branches rasterize FIRST and leaves never
    overwrite them, so a trunk stays a trunk where a leaf card clips through it.

  * THE LEAF CARDS ARE ALPHA-BLENDED QUADS. Every leaves material is alphaMode BLEND, i.e. a
    rectangle whose texture is mostly transparent. Rasterizing it without reading the alpha
    turns each card into a solid green tile and the crown comes out as a pile of slabs. Texels
    below ALPHA_MIN are dropped, which is what leaves the crown its open, leafy structure.

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_oaks.py
numpy and Pillow live there; the msys2 interpreter has neither, and no pip to add them.
"""
import struct, json, io, os, sys, re
from collections import deque
import numpy as np
from PIL import Image

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB = os.path.join(_ROOT, 'source', 'glb', 'oak_trees.glb')
OUT = os.path.join(_ROOT, 'game', 'assets', 'decoration', 'oak_trees.json')
VOX = 0.1          # metres per voxel - THE 10 cm grid every other asset in this game is on
# -- SOURCE UNITS ARE NOT METRES -- the seven trees measure 21 to 108 units tall in the file, so at
# 1 unit = 1 m the smallest bush would be a seven-storey building. TALLEST fixes the scale instead:
# the biggest oak becomes this many metres and every other tree keeps its relative size, which is
# the only thing the author actually authored.
#
# 11.5 m IS NOT A LOOK DECISION, IT IS THE ENGINE'S CANOPY ENVELOPE. pine5.vox is 116 voxels tall and
# TWO places reserve exactly that much air above the terrain for it: HMAX = min(105 + LIFT, WY - 122)
# in world/window.js, and the brick-occupancy sky cap `((maxH + 122) >> 3) + 1` which exists TWICE,
# once in terrain.js rebuildBricks and once inside the gen-pool worker source (__vb.gtest diffs the
# two). A model taller than ~120 voxels pokes through that cap and its crown stops being traced.
# 11.5 m lands the tallest oak at 116 voxels, the pine's own height, so nothing has to move. Raising
# it means raising 122 in BOTH copies and re-checking HMAX - do that deliberately, not by nudging
# this number. First bake was 18 m: 789k voxels, a 313k-voxel single tree, and a crown that cleared
# the cap.
TALLEST = 11.5
# -- GROW: EVERY TREE 50% BIGGER, THE BUSH LEFT ALONE (user 2026-09-03) -- TALLEST fixes the whole set
# against one number, so raising it scales the UNDERBRUSH with the canopy and the 2.4 m bush becomes a
# 3.6 m one. That is not what was asked for ("keep the bushes the same though"), and it would also break
# what the bush IS: oakAt spends tiers 0 and 1 on it as walk-past ground cover, and stamped.js refuses to
# perch a bird in it precisely because it is short. So the scale is applied PER GROUP and the shortest
# group -- which is the bush, by a factor of two against the next tree up -- keeps the original SCALE.
#
# IT FITS UNDER THE WORLD CEILING, AND THAT WAS CHECKED RATHER THAN ASSUMED. The header warns that a model
# over ~120 voxels pokes through the sky cap; both numbers it names have since moved and the real ones are
# CANOPY = 265 (world/window.js, used by BOTH brick-occupancy copies) and the TREE RESERVE that pins
# HMAX = WY - 158. At GROW 1.5 the tallest oak goes 114 -> ~171 voxels, which is under CANOPY with room.
# The reserve is the tighter test and it is about the ground the tree stands on: it is sized for a
# 152-voxel PINE on the highest terrain in the world (HMAX 226, WY 384). Oaks do not stand there -- the
# oak band's own field tops out at OAKY + OAKHILL = 162 -- so a 171-voxel oak on the highest oak ground
# reaches 333 against WY 384. Nothing about HMAX or the reserve has to move for this.
GROW = 1.0        # scale applied to every group EXCEPT the shortest; 1.0 = the original bake
PIN_PAL = None    # path to an existing oak_trees.json whose palette should be reused verbatim
# -- THE PALETTE BUDGET IS THE REAL CONSTRAINT ON THESE TWO NUMBERS, AND IT WAS MEASURED, NOT GUESSED --
# the game shares ONE 256-entry table across every material in the world, and booting with ?nooaks reports
# 250/256 with SIX free. A first bake at 3 + 5 asked for eight, and the two it could not have came out of
# somebody else's pocket in silence: palAudit went to 256/256 with over=0 and snaps=0 (so nothing LOOKED
# wrong) while edSubs went 0 -> 2, i.e. the asset editor's own swatches were the ones substituted.
# So the oaks now cost FOUR ids, not eight:
#   * the BARK shades below are quantization targets for this bake only. The loader in src/assets/bow.js
#     repoints them onto the PINE's existing bark ids by relative lightness and mints nothing - the same
#     trick log.vox already uses, and it hands the oak trunk woodTab/axeOnly/solid for free.
#   * only the LEAF shades become new palette entries, because the leaf colour IS the biome and there is
#     nothing already in the table to borrow it from.
# 4 leaves leaves 2 slots free, which survives the +-1 wobble palAudit's len has between boots.
NBARK = 3          # bark quantization targets - see above, these do NOT cost palette ids
NLEAF = 4          # ...leaf shades, and these DO: four new entries out of the six that were free
ALPHA_MIN = 96     # leaf texels below this alpha are not leaf (see the header)
MINVOX = 400       # a group smaller than this is a stray, not a tree
CAVITY_WOOD = 0.9  # an enclosed cavity is filled only if this fraction of its wall is wood - hollow
                   # trunks fill in, leaf pockets stay open. See the fill pass.
for _a in sys.argv[1:]:
    if _a.startswith('--tallest='):
        TALLEST = float(_a[10:])
    if _a.startswith('--nbark='):
        NBARK = int(_a[8:])
    if _a.startswith('--nleaf='):
        NLEAF = int(_a[8:])
    if _a.startswith('--alpha='):
        ALPHA_MIN = int(_a[8:])
    if _a.startswith('--grow='):
        GROW = float(_a[7:])
    if _a.startswith('--pin-pal='):
        PIN_PAL = _a[10:]

d = open(GLB, 'rb').read()
clen = struct.unpack_from('<I', d, 12)[0]
js = json.loads(d[20:20 + clen])
boff = 20 + clen
blen, btype = struct.unpack_from('<II', d, boff)
BIN = d[boff + 8: boff + 8 + blen]


def acc_data(ai):
    a = js['accessors'][ai]
    bv = js['bufferViews'][a['bufferView']]
    off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    ncomp = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    dt = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
          5125: np.uint32, 5126: np.float32}[a['componentType']]
    stride = bv.get('byteStride')
    itemsize = np.dtype(dt).itemsize * ncomp
    if stride and stride != itemsize:
        return np.stack([np.frombuffer(BIN, dt, ncomp, off + i * stride) for i in range(a['count'])])
    return np.frombuffer(BIN, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)


def img_pixels(ti):
    tex = js['textures'][ti]
    im = js['images'][tex['source']]
    bv = js['bufferViews'][im['bufferView']]
    off = bv.get('byteOffset', 0)
    return np.asarray(Image.open(io.BytesIO(BIN[off: off + bv['byteLength']])).convert('RGBA'))


def node_mat(n):
    if 'matrix' in n:
        return np.array(n['matrix'], dtype=np.float64).reshape(4, 4).T
    T = n.get('translation', [0, 0, 0])
    R = n.get('rotation', [0, 0, 0, 1])
    S = n.get('scale', [1, 1, 1])
    x, y, z, w = R
    rm = np.array([
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4)
    m[:3, :3] = rm * np.array(S)[None, :]
    m[:3, 3] = T
    return m


parent = {}
for i, n in enumerate(js['nodes']):
    for c in n.get('children', []):
        parent[c] = i


def world_mat(ni):
    m = node_mat(js['nodes'][ni])
    while ni in parent:
        ni = parent[ni]
        m = node_mat(js['nodes'][ni]) @ m
    return m


texcache = {}


def get_tex(ti):
    if ti not in texcache:
        texcache[ti] = img_pixels(ti)
    return texcache[ti]


# -- GROUP THE MESHES INTO TREES -- 'Tree EZTree1.Bush006_leaves.006_0' -> ('EZTree1.Bush006', 'leaves')
NAME_RE = re.compile(r'^(.*?)_(branches|leaves)[.0-9]*_\d+$')
groups = {}
for ni, n in enumerate(js['nodes']):
    if n.get('mesh') is None:
        continue
    mesh = js['meshes'][n['mesh']]
    nm = mesh.get('name', '?')
    m = NAME_RE.match(nm)
    if not m:
        print('  ?? mesh "%s" matches neither _branches nor _leaves - skipped' % nm)
        continue
    key, kind = m.group(1), m.group(2)
    pr = mesh['primitives'][0]
    pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
    uv = acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64) if 'TEXCOORD_0' in pr['attributes'] else None
    idx = acc_data(pr['indices']).astype(np.int64).ravel() if 'indices' in pr else np.arange(len(pos))
    mat = js['materials'][pr['material']] if pr.get('material') is not None else {}
    sg = mat.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness')
    pbr = mat.get('pbrMetallicRoughness', {})
    if sg is not None:
        bcf, bct = sg.get('diffuseFactor', [1, 1, 1, 1]), sg.get('diffuseTexture')
    else:
        bcf, bct = pbr.get('baseColorFactor', [1, 1, 1, 1]), pbr.get('baseColorTexture')
    M = world_mat(ni)
    wp = (M[:3, :3] @ pos.T).T + M[:3, 3]
    groups.setdefault(key, []).append(dict(kind=kind, wp=wp, uv=uv, idx=idx,
                                           tex=get_tex(bct['index']) if bct is not None else None,
                                           bcf=bcf))

tall = max(max(p['wp'][:, 1].max() for p in parts) - min(p['wp'][:, 1].min() for p in parts)
           for parts in groups.values())
SCALE = TALLEST / tall
# the shortest group is the BUSH and it is the one GROW does not touch -- see the note over GROW
GHEIGHT = {k: max(p['wp'][:, 1].max() for p in parts) - min(p['wp'][:, 1].min() for p in parts)
           for k, parts in groups.items()}
BUSH_KEY = min(GHEIGHT, key=GHEIGHT.get)
print('%d trees; tallest is %.1f source units -> %.1f m (scale %.5f), %.0f cm voxels'
      % (len(groups), tall, TALLEST, SCALE, VOX * 100))
if GROW != 1.0:
    print('grow x%.2f on every group except %r (the bush, %.1f source units tall)'
          % (GROW, BUSH_KEY, GHEIGHT[BUSH_KEY]))


def raster(parts, kinds, org, scl):
    """Every triangle in `parts` whose kind is in `kinds`, sampled at half-voxel spacing.
    Returns (voxel coords, RGB) with one entry per SAMPLE - duplicates are resolved by dedupe()."""
    P, C = [], []
    for p in parts:
        if p['kind'] not in kinds:
            continue
        wp = (p['wp'] * scl - org) / VOX        # `scl` is the GROUP's scale, not the global SCALE: the bush keeps the original one (see GROW)
                                                # straight into voxel space, so the edge lengths below are in voxels
        tri = p['idx'].reshape(-1, 3)
        A, B, Cc = wp[tri[:, 0]], wp[tri[:, 1]], wp[tri[:, 2]]
        e = np.maximum(np.maximum(np.linalg.norm(B - A, axis=1), np.linalg.norm(Cc - A, axis=1)),
                       np.linalg.norm(Cc - B, axis=1))
        nsub = np.clip(np.ceil(e / 0.5), 1, 96).astype(int)
        uv, tex = p['uv'], p['tex']
        if uv is not None:
            UA, UB, UC = uv[tri[:, 0]], uv[tri[:, 1]], uv[tri[:, 2]]
        for ns in np.unique(nsub):                # BUCKETED BY SUBDIVISION so each bucket is one numpy op
            sel = nsub == ns
            ij = np.array([(i, j) for i in range(ns + 1) for j in range(ns + 1 - i)], dtype=np.float64)
            a, b = ij[:, 0] / ns, ij[:, 1] / ns
            c = 1.0 - a - b
            pts = (A[sel][:, None, :] * a[None, :, None] + B[sel][:, None, :] * b[None, :, None]
                   + Cc[sel][:, None, :] * c[None, :, None]).reshape(-1, 3)
            if tex is not None and uv is not None:
                ut = (UA[sel][:, None, :] * a[None, :, None] + UB[sel][:, None, :] * b[None, :, None]
                      + UC[sel][:, None, :] * c[None, :, None]).reshape(-1, 2)
                h2, w2 = tex.shape[:2]
                px = tex[(ut[:, 1] % 1.0 * (h2 - 1)).astype(int), (ut[:, 0] % 1.0 * (w2 - 1)).astype(int)]
                keep = px[:, 3] >= ALPHA_MIN      # -- THE ALPHA TEST -- see the header: a leaf card is mostly hole
                pts, col = pts[keep], px[keep, :3].astype(np.float64) * np.array(p['bcf'][:3])
            else:
                col = np.repeat(np.array(p['bcf'][:3])[None, :] * 255.0, len(pts), axis=0)
            if len(pts):
                P.append(np.floor(pts).astype(np.int32))
                C.append(col)
    if not P:
        return np.zeros((0, 3), np.int32), np.zeros((0, 3))
    return np.concatenate(P), np.concatenate(C)


def dedupe(vx, col, dims):
    """sample list -> unique voxels with their MEAN colour. Vectorised: the per-voxel dict the other
    voxelizers use is fine for a 0.5 m cactus and far too slow for a million-sample crown."""
    lin = (vx[:, 0].astype(np.int64) * dims[1] + vx[:, 1]) * dims[2] + vx[:, 2]
    u, inv = np.unique(lin, return_inverse=True)
    n = np.bincount(inv).astype(np.float64)
    mean = np.stack([np.bincount(inv, weights=col[:, k]) / n for k in range(3)], axis=1)
    z = u % dims[2]
    y = (u // dims[2]) % dims[1]
    x = u // (dims[2] * dims[1])
    return np.stack([x, y, z], axis=1).astype(np.int32), mean


NB6 = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))
models = []
bark_cols, leaf_cols = [], []
for key in sorted(groups):
    parts = groups[key]
    gs = SCALE if key == BUSH_KEY else SCALE * GROW   # the bush keeps the original scale
    allp = np.concatenate([p['wp'] for p in parts]) * gs
    org = allp.min(0)
    dims = np.maximum(1, np.ceil((allp.max(0) - org) / VOX).astype(int) + 1)
    bvx, bcol = raster(parts, ('branches',), org, gs)
    lvx, lcol = raster(parts, ('leaves',), org, gs)
    bvx = np.clip(bvx, 0, dims - 1)
    lvx = np.clip(lvx, 0, dims - 1)
    if len(bvx):
        bvx, bcol = dedupe(bvx, bcol, dims)
    if len(lvx):
        lvx, lcol = dedupe(lvx, lcol, dims)
    grid = np.zeros(dims, np.uint8)               # 0 = air, 1 = wood, 2 = leaf
    if len(bvx):
        grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = 1
    if len(lvx):                                  # WOOD WINS: a leaf card clipping through the trunk must not repaint it green
        keep = grid[lvx[:, 0], lvx[:, 1], lvx[:, 2]] == 0
        lvx, lcol = lvx[keep], lcol[keep]
        grid[lvx[:, 0], lvx[:, 1], lvx[:, 2]] = 2
    # -- DROP THE BURIED LEAVES -- a leaf voxel with a solid voxel on all six sides can never be the first
    # hit of any ray, so removing it cannot change a single pixel: it is the one reduction available here
    # that is lossless by construction rather than by taste (the house rule). It is worth ~20% of the set,
    # because a leaf card is a CLUSTER of leaves in its texture and the dense middle of a cluster rasterizes
    # solid. WOOD IS NEVER CULLED - the trunk has to stay solid for the axe to bite into something, and the
    # heartwood pass below deliberately fills it further.
    if len(lvx):
        occ = grid != 0
        pad = np.zeros(dims + 2, bool)            # ZEROS, not ones: with a solid border a leaf lying ON a bbox
        pad[1:-1, 1:-1, 1:-1] = occ               # face has its outward neighbour counted as solid and gets culled
        openv = ~(pad[:-2, 1:-1, 1:-1] & pad[2:, 1:-1, 1:-1] & pad[1:-1, :-2, 1:-1]   # while still visible - which would break the lossless claim above. Latent while the trim below leaves a margin; live the moment a crown reaches the edge.
                  & pad[1:-1, 2:, 1:-1] & pad[1:-1, 1:-1, :-2] & pad[1:-1, 1:-1, 2:])
        keep = openv[lvx[:, 0], lvx[:, 1], lvx[:, 2]]
        buried = int((~keep).sum())
        lvx, lcol = lvx[keep], lcol[keep]
        grid[:] = 0
        if len(bvx):
            grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = 1
        grid[lvx[:, 0], lvx[:, 1], lvx[:, 2]] = 2
    else:
        buried = 0
    # -- DROP LEAF ISLANDS THAT TOUCH NOTHING -- a 26-component made entirely of leaf and connected to no wood
    # is a clump the source's leaf cards left hanging in the crown envelope. It is invisible where it sits, so
    # it looks harmless, and it is not: foliaTab makes every leaf voxel SUP.DRAPE (sim/support-rules.js), a
    # DRAPE flood may only TERMINATE on an anchored structure cell, and ORPHAN_OK excludes foliage - so the
    # generation-time orphan sweep will not take it either. It survives into the runtime support queue as a
    # permanently unanchored component. Measured in the first bake: one 154-voxel island in EZTree0.Large,
    # which is exactly the floaterVox the in-game floatAudit reported.
    islands = 0
    if len(lvx):
        lab = np.zeros(dims, np.int32)
        stack, nlab = [], 0
        occ = grid != 0
        for seed in map(tuple, np.argwhere(occ & (lab == 0))):
            if lab[seed]:
                continue
            nlab += 1
            comp, wood = [], False
            stack = [seed]
            lab[seed] = nlab
            while stack:
                v = stack.pop()
                comp.append(v)
                if grid[v] == 1:
                    wood = True
                for dx in (-1, 0, 1):
                    for dy in (-1, 0, 1):
                        for dz in (-1, 0, 1):
                            if not (dx or dy or dz):
                                continue
                            n = (v[0] + dx, v[1] + dy, v[2] + dz)
                            if (0 <= n[0] < dims[0] and 0 <= n[1] < dims[1] and 0 <= n[2] < dims[2]
                                    and occ[n] and not lab[n]):
                                lab[n] = nlab
                                stack.append(n)
            if not wood:
                for v in comp:
                    grid[v] = 0
                islands += len(comp)
        if islands:
            keep = grid[lvx[:, 0], lvx[:, 1], lvx[:, 2]] == 2
            lvx, lcol = lvx[keep], lcol[keep]
    if len(bvx) + len(lvx) < MINVOX:
        print('  %-22s only %d voxels - skipped' % (key, len(bvx) + len(lvx)))
        continue
    # -- TRIM TO THE OCCUPIED EXTENT -- `dims` came from the MESH bounds, and the outermost texels of a leaf
    # card are transparent, so every model carried 1-4 voxels of empty margin on most faces. That matters
    # because stampModel anchors on the BOUNDING BOX (bx = wx - (fw >> 1)): slack on one side and not the
    # other walks the tree off the point the scatter chose for it, and slack underneath hangs it in the air.
    mn = np.minimum(bvx.min(0) if len(bvx) else dims, lvx.min(0) if len(lvx) else dims)
    mx = np.maximum(bvx.max(0) if len(bvx) else np.zeros(3, int), lvx.max(0) if len(lvx) else np.zeros(3, int))
    if len(bvx):
        bvx = bvx - mn
    if len(lvx):
        lvx = lvx - mn
    dims = (mx - mn + 1).astype(int)
    grid = np.zeros(dims, np.uint8)
    if len(bvx):
        grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = 1
    if len(lvx):
        grid[lvx[:, 0], lvx[:, 1], lvx[:, 2]] = 2
    models.append(dict(key=key, dims=dims, grid=grid, bvx=bvx, bcol=bcol, lvx=lvx, lcol=lcol))
    bark_cols.append(bcol)
    leaf_cols.append(lcol)
    print('  %-22s %3d w x %3d d x %3d h vox (%.1f x %.1f x %.1f m)  wood %6d  leaf %6d  (-%d buried, -%d island)'
          % (key, dims[0], dims[2], dims[1], dims[0] * VOX, dims[2] * VOX, dims[1] * VOX,
             len(bvx), len(lvx), buried, islands))


def median_cut(colors, n):
    boxes = [colors]
    while len(boxes) < n:
        bi = max(range(len(boxes)), key=lambda i: np.ptp(boxes[i], 0).max() * (len(boxes[i]) > 1))
        b = boxes[bi]
        if len(b) <= 1:
            break
        ax = np.ptp(b, 0).argmax()
        med = np.median(b[:, ax])
        lo, hi = b[b[:, ax] <= med], b[b[:, ax] > med]
        if len(lo) == 0 or len(hi) == 0:
            o = b[:, ax].argsort()
            lo, hi = b[o[:len(b) // 2]], b[o[len(b) // 2:]]
        boxes[bi:bi + 1] = [lo, hi]
    return np.array([b.mean(0) for b in boxes])


# -- TWO SEPARATE CUTS -- one cut over both would spend every shade on the leaves: they outnumber the bark
# several to one in this set, and a population-weighted box split follows the population.
# -- BUT PER-VOXEL, NOT PER-DISTINCT-COLOUR, WHICH IS WHERE THIS DIFFERS FROM voxelize_cacti.py -- that tool
# de-duplicates first so a rare vivid prickly-pear flower cannot be outvoted by thousands of body greens. An
# oak has no such feature: leaves are one smooth continuum whose whole spread is baked lighting, so
# de-duplicating just walks the shades out toward sparsely populated extremes. Measured at the same shade
# count, weighting by population wins on both halves - bark mean max-channel error 8.07 -> 7.30 (p50 7.3 ->
# 5.8), leaf 14.52 -> 14.25 (p50 13.0 -> 12.1). Free, so take it.
def cut(cols, n, what):
    u = np.clip(np.concatenate(cols), 0, 255).round().astype(float)
    print('%s: %d voxels (%d distinct) -> %d shades'
          % (what, len(u), len(np.unique(u.astype(int), axis=0)), n))
    return median_cut(u, n)


# -- PINNED, SO A RE-BAKE CANNOT REPAINT THE FOREST (user 2026-09-03: "also keep the current colors") --
# median_cut is a POPULATION-weighted split (see the note over cut), so it is a function of how many voxels
# each shade covers -- and changing the scale changes exactly that. A re-bake at GROW 1.5 rasterizes ~3.4x
# the voxels and the cut lands on different means, which would repaint every oak in the game as a side
# effect of resizing it. --pin-pal reads the shades out of the existing bake and reuses them verbatim; the
# nearest() snap below then resolves this bake's colours onto them, so the models change shape and NOT hue.
if PIN_PAL:
    _pin = json.load(open(PIN_PAL))
    _pp = np.array(_pin['pal'], float)
    pal_bark, pal_leaf = _pp[:_pin['nbark']], _pp[_pin['nbark']:]
    assert len(pal_bark) == NBARK and len(pal_leaf) == NLEAF, (
        'pinned palette is %d+%d, this bake wants %d+%d' % (len(pal_bark), len(pal_leaf), NBARK, NLEAF))
    print('palette PINNED from %s: %d bark + %d leaf, unchanged' % (PIN_PAL, NBARK, NLEAF))
else:
    pal_bark = cut(bark_cols, NBARK, 'bark')
    pal_leaf = cut(leaf_cols, NLEAF, 'leaf')
pal = np.concatenate([pal_bark, pal_leaf])
print('bark:', [[int(round(c)) for c in p] for p in pal_bark])
print('leaf:', [[int(round(c)) for c in p] for p in pal_leaf])

out_models = []
for m in models:
    dims, grid = m['dims'], m['grid']
    # -- FILL THE HOLLOWS, BUT ONLY THE WOODEN ONES -- flood the exterior, then take each enclosed
    # cavity on its own: a cavity walled almost entirely in wood is the inside of a trunk or a limb and
    # has to be solid (chop one open and you would otherwise be looking into a pipe), while a cavity
    # walled in leaves is a gap in the crown and must stay air, or the whole canopy fills in as one blob.
    D = dims + 2
    pad = np.zeros(D, np.uint8)
    pad[1:-1, 1:-1, 1:-1] = grid
    seen = pad != 0
    # -- SEAL THE GROUND PLANE, THEN SEED FROM THE SKY -- every trunk in this .glb is an open-ended tube: the
    # branches mesh reaches source y = 0 with no bottom cap. Seeded at the bottom corner the flood walked
    # straight up the inside of the bole, so the bole was never a cavity and the fill below never fired on the
    # one thing its own comment exists for - the two big trees shipped with a one-voxel-thick pipe for a trunk
    # at exactly the height a player swings an axe. grid is indexed [x, HEIGHT, z], so height is axis 1.
    seen[:, 0, :] = True
    q = deque([(0, D[1] - 1, 0)])
    seen[0, D[1] - 1, 0] = True
    while q:
        x, y, z = q.popleft()
        for dx, dy, dz in NB6:
            nx, ny, nz = x + dx, y + dy, z + dz
            if 0 <= nx < D[0] and 0 <= ny < D[1] and 0 <= nz < D[2] and not seen[nx, ny, nz]:
                seen[nx, ny, nz] = True
                q.append((nx, ny, nz))
    filled = []
    for v0 in map(tuple, np.argwhere(~seen)):
        if seen[v0]:
            continue
        cav, wall_w, wall_l = [], 0, 0
        q = deque([v0])
        seen[v0] = True
        while q:
            v = q.popleft()
            cav.append(v)
            for dd in NB6:
                nb = (v[0] + dd[0], v[1] + dd[1], v[2] + dd[2])
                if not (0 <= nb[0] < D[0] and 0 <= nb[1] < D[1] and 0 <= nb[2] < D[2]):
                    continue
                if pad[nb] == 1:
                    wall_w += 1
                elif pad[nb] == 2:
                    wall_l += 1
                elif not seen[nb]:
                    seen[nb] = True
                    q.append(nb)
        if wall_w + wall_l and wall_w / (wall_w + wall_l) >= CAVITY_WOOD:
            filled.extend(cav)
    fill = (np.array(filled, np.int32) - 1) if filled else np.zeros((0, 3), np.int32)

    def nearest(c, lo, hi):                       # bark resolves onto bark shades ONLY, leaf onto leaf
        sub = pal[lo:hi]
        return lo + ((sub[None, :, :] - c[:, None, :]) ** 2).sum(2).argmin(1)

    bi = nearest(m['bcol'], 0, NBARK) if len(m['bvx']) else np.zeros(0, int)
    li = nearest(m['lcol'], NBARK, NBARK + NLEAF) if len(m['lvx']) else np.zeros(0, int)
    dark = int(np.argmin(pal_bark.sum(1)))        # heartwood = the darkest bark shade
    vx = np.concatenate([m['bvx'], m['lvx'], fill])
    ci = np.concatenate([bi, li, np.full(len(fill), dark, int)])
    packed = sorted((int(v[0]) | (int(v[2]) << 8) | (int(v[1]) << 16) | (int(c) << 24))
                    for v, c in zip(vx, ci))      # source Y-up -> model z-up, as the other voxelizers do
    out_models.append(dict(src=re.sub(r'[^A-Za-z0-9]', '_', m['key']).strip('_').lower(),
                           sx=int(dims[0]), sy=int(dims[2]), sz=int(dims[1]),
                           wood=int(len(m['bvx']) + len(fill)), vox=packed))
    print('  %-22s %6d voxels (+%d heartwood)' % (m['key'], len(packed), len(fill)))

trees = sorted(out_models, key=lambda r: r['sz'])
for i, m in enumerate(trees):                          # oak_1 = the bush, oak_7 = the biggest oak
    m['name'] = 'oak_%d' % (i + 1)

# -- ALSO EMIT ONE .vox PER TREE -- MagicaVoxel format, carrying the SAME shared palette the .json
# uses, so the seven files stay colour-consistent with each other and with what the game renders.
# Note the axes: a .vox is z-up and so is the packed model (x | z<<8 | y<<16 came out of the source's
# Y-up during packing), so the three bytes go out in the order they are already in. Colour indices in
# a .vox are 1-BASED into the RGBA chunk, which is why every id is written +1.
out_pal = [[int(round(c)) for c in q] for q in pal]


def write_vox(path, m):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', m['sx'], m['sy'], m['sz']))
    vox = b''.join(struct.pack('<BBBB', q & 255, (q >> 8) & 255, (q >> 16) & 255, ((q >> 24) & 255) + 1)
                   for q in m['vox'])
    xyzi = chunk(b'XYZI', struct.pack('<I', len(m['vox'])) + vox)
    rgba = b''
    for i in range(256):
        c = out_pal[i] if i < len(out_pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    body = size + xyzi + chunk(b'RGBA', rgba)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


VOXDIR = os.path.join(_ROOT, 'game', 'assets', 'foilage', 'oak_trees')
os.makedirs(VOXDIR, exist_ok=True)
for m in trees:
    assert max(m['sx'], m['sy'], m['sz']) <= 256, '%s exceeds the .vox 256/axis limit' % m['name']
    fp = os.path.join(VOXDIR, m['name'] + '.vox')
    write_vox(fp, m)
    print('  %-8s %3d x %3d x %3d  %6d voxels  <- %-22s -> %s'
          % (m['name'], m['sx'], m['sy'], m['sz'], len(m['vox']), m['src'], os.path.basename(fp)))

out = dict(pal=out_pal, nbark=NBARK, trees=trees)
s = json.dumps(out, separators=(',', ':'))
os.makedirs(os.path.dirname(OUT), exist_ok=True)
open(OUT, 'w').write(s)
print('wrote %s (%.0f KB), %d trees, %d palette ids (%d bark + %d leaf)'
      % (OUT, len(s) / 1024, len(out_models), len(pal), NBARK, NLEAF))
print('wrote %d .vox to %s' % (len(trees), VOXDIR))
