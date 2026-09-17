"""Voxelize source/glb/building.glb -- the building AND the platform under it --
at the engine's own 10 cm grid, and write it as one multi-piece .vox.

Reads:  source/glb/building.glb                     (the sculpt, gitignored)
Writes: game/assets/level/building.vox              (the level, 4 pieces)

WHAT THIS IS FOR: the building is a LEVEL, not a decoration. Nothing scatters it,
nothing places twenty of them on a hillside; it is loaded once, stood in one
place, and walked around. So unlike voxelize_rocks.py and its siblings this tool
writes ONE file out of the WHOLE SCENE -- all sixty-five meshes in their authored
positions, in one grid -- rather than one file per mesh with its transform thrown
away.

    world extent   33.0 x 36.0 x 22.6 m   ->  331 x 360 x 227 voxels at 10 cm

THE PLATFORM IS PART OF IT (user: "voxelize the platform that its on as well").
It is node 71 / mesh Plane.020_0, a 33 x 22.6 m slab 1.33 m thick sitting from
y = -1.33 to y = 0, and it is the only thing in the file with anything under the
building. Voxelizing the building alone would give a level with no floor outside
its own walls.

-- WHY A SURFACE PASS AND THEN A FLOOD, RATHER THAN EITHER ALONE --------------

Voxelizing the triangles gives a SHELL: the platform comes out as a hollow box
with 10 cm skin, which is wrong twice over -- the mesher emits the inside faces
as well as the outside ones (double the triangles, all of them invisible), and a
tool that cuts into it finds a cavity where there should be concrete.

Flooding from outside and filling whatever the flood cannot reach fixes both,
and -- this is the part that matters for a BUILDING rather than a rock -- it
fixes them WITHOUT sealing the rooms. A doorway is a hole the flood pours
through, so the inside of the building stays air and you can walk into it; the
gap between the two faces of a wall is not, so it fills. One rule, and it tells
the difference on its own because the difference is real.

    solid = (everything the outside flood never reached) + (the sampled shell)

THE SHELL IS DILATED FOR THE FLOOD ONLY, which is revoxel_rocks_scaled.py's
trick and its comment is worth repeating: a seam narrower than a voxel lets the
exterior pour into a solid and it comes back hollow. The dilation is a BARRIER,
not part of the answer -- the ring is thrown away, so nothing is fattened. At
10 cm it cannot close a door or a window, which are metres.

-- COLOUR ---------------------------------------------------------------------

Sampled from each material's baseColorTexture at the sample's own UV, times its
baseColorFactor, exactly as the rock tool does. Then AVERAGED per voxel rather
than first-sample-wins, which is why there is no 3-pass blur here: the blur in
the rock tool exists to undo the speckle that taking one texel per voxel causes,
and averaging the twenty-odd samples that land in a voxel never makes it.

Quantized to PAL_N shades by k-means. That number is not cosmetic -- v2's
material table holds 255 entries for the WHOLE WORLD and the wood already spends
about 190 of them, so a level that asks for its own hundred gets served
mat::AIR and renders as nothing. See the palette notes in scene/voxelworld.h.

-- THE FILE FORMAT'S CEILING, AND WHY THERE ARE FOUR PIECES --------------------

A .vox XYZI record packs each coordinate in a byte, so no piece may exceed 256
on a side. In .vox axes (z up) this model is 331 x 227 x 360, which busts it on
x and on z. MagicaVoxel's own answer is the scene graph -- several models placed
by nTRN -- and v2's voxParse composes exactly that back into one grid, which is
what tools/revoxel_trees_tall.py already relies on for the 30 m pines. That
writer only slabs along z because a tree is tall and narrow; this one splits on
all three axes because a building is none of those things.

Run with the python that has numpy, scipy and Pillow:  py tools/voxelize_building.py
"""
import io
import json
import math
import os
import struct
import sys
import time

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB = os.path.join(ROOT, 'source', 'glb', 'building.glb')
OUTDIR = os.path.join(ROOT, 'game', 'assets', 'level')
OUT = os.path.join(OUTDIR, 'building.vox')

VOX = 0.1          # metres per voxel -- the engine's grid, never anything else
PAL_N = 28         # shades to quantize the whole level to; see the note above
MAX_AXIS = 250     # per .vox piece, with room under the format's 256
SAMPLE = 0.5       # barycentric lattice step, in voxels
CAVITY_FILL = 20000  # a sealed pocket smaller than this is a wall seam, not a room


# ---------------------------------------------------------------------------
# glb
# ---------------------------------------------------------------------------
d = open(GLB, 'rb').read()
clen = struct.unpack_from('<I', d, 12)[0]
js = json.loads(d[20:20 + clen])
boff = 20 + clen
blen, _btype = struct.unpack_from('<II', d, boff)
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
        rows = [np.frombuffer(BIN, dt, ncomp, off + i * stride) for i in range(a['count'])]
        return np.stack(rows)
    return np.frombuffer(BIN, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)


def img_pixels(ti):
    """The RGB of a TEXTURE index. Grayscale and paletted maps are promoted --
    several materials here point baseColorTexture at an L or P image and a
    voxel has to be given some colour."""
    tex = js['textures'][ti]
    im = js['images'][tex['source']]
    bv = js['bufferViews'][im['bufferView']]
    off = bv.get('byteOffset', 0)
    raw = BIN[off: off + bv['byteLength']]
    return np.asarray(Image.open(io.BytesIO(raw)).convert('RGB'))


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


_texcache = {}


def get_tex(ti):
    if ti not in _texcache:
        _texcache[ti] = img_pixels(ti)
    return _texcache[ti]


# ---------------------------------------------------------------------------
# every primitive in the scene, in world space
# ---------------------------------------------------------------------------
class Prim:
    __slots__ = ('wp', 'uv', 'idx', 'tex', 'bcf', 'name')


prims = []
for ni, n in enumerate(js['nodes']):
    if 'mesh' not in n:
        continue
    M = world_mat(ni)
    mesh = js['meshes'][n['mesh']]
    for pr in mesh['primitives']:
        p = Prim()
        pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
        p.wp = (M[:3, :3] @ pos.T).T + M[:3, 3]
        p.uv = (acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64)
                if 'TEXCOORD_0' in pr['attributes'] else None)
        p.idx = (acc_data(pr['indices']).astype(np.int64).ravel()
                 if 'indices' in pr else np.arange(len(pos)))
        mat = js['materials'][pr['material']] if 'material' in pr else {}
        pbr = mat.get('pbrMetallicRoughness', {})
        p.bcf = np.array(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3], dtype=np.float64)
        bct = pbr.get('baseColorTexture')
        p.tex = get_tex(bct['index']) if bct is not None else None
        p.name = mesh.get('name', '?')
        prims.append(p)

lo = np.min([p.wp.min(0) for p in prims], axis=0)
hi = np.max([p.wp.max(0) for p in prims], axis=0)
dims = np.ceil((hi - lo) / VOX).astype(np.int64) + 1     # world x, y(up), z
NX, NY, NZ = (int(v) for v in dims)
print('building.glb: %d primitives, %d triangles' % (
    len(prims), sum(len(p.idx) // 3 for p in prims)))
print('  world  lo %s  hi %s' % (lo.round(2), hi.round(2)))
print('  grid   %d x %d x %d voxels at %.0f cm  (%.1f x %.1f x %.1f m)  %.1f M cells'
      % (NX, NY, NZ, VOX * 100, NX * VOX, NY * VOX, NZ * VOX, NX * NY * NZ / 1e6))
if NX * NY * NZ > 64 * (1 << 20):
    sys.exit('grid exceeds voxParse\'s 64 M cell ceiling -- it would refuse the file')


# ---------------------------------------------------------------------------
# surface pass
#
# One flat index per sample, colours averaged per voxel with bincount. A GROUP
# goes into one pair of arrays: the pieces the file is split into later are a
# packaging detail and must not be visible here, or a wall that straddles the
# cut would be sampled twice with two different answers.
# ---------------------------------------------------------------------------
def surface(group):
    flat_parts, col_parts = [], []
    for p in group:
        tris = p.idx.reshape(-1, 3)
        v0, v1, v2 = p.wp[tris[:, 0]], p.wp[tris[:, 1]], p.wp[tris[:, 2]]
        e = np.maximum.reduce([np.linalg.norm(v1 - v0, axis=1),
                               np.linalg.norm(v2 - v0, axis=1),
                               np.linalg.norm(v2 - v1, axis=1)])
        # Triangles are batched by how finely they have to be sampled so the
        # lattice for a batch is built once. n is driven by the LONGEST edge,
        # which is what guarantees the lattice pitch is under half a voxel on
        # every edge and therefore that the shell has no holes in it.
        nreq = np.maximum(1, np.ceil(e / (VOX * SAMPLE)).astype(np.int64))
        for n in np.unique(nreq):
            sel = np.nonzero(nreq == n)[0]
            ii, jj = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing='ij')
            keep = (ii + jj) <= n
            a = (ii[keep] / n).astype(np.float64)
            b = (jj[keep] / n).astype(np.float64)
            c = 1.0 - a - b
            # (tri, lattice, 3)
            pts = (v0[sel][:, None, :] * a[None, :, None] +
                   v1[sel][:, None, :] * b[None, :, None] +
                   v2[sel][:, None, :] * c[None, :, None])
            vs = np.floor((pts - lo) / VOX).astype(np.int64)
            np.clip(vs[..., 0], 0, NX - 1, out=vs[..., 0])
            np.clip(vs[..., 1], 0, NY - 1, out=vs[..., 1])
            np.clip(vs[..., 2], 0, NZ - 1, out=vs[..., 2])
            flat = (vs[..., 0] * NY + vs[..., 1]) * NZ + vs[..., 2]

            if p.uv is not None and p.tex is not None:
                u0, u1, u2 = p.uv[tris[sel, 0]], p.uv[tris[sel, 1]], p.uv[tris[sel, 2]]
                ut = (u0[:, None, :] * a[None, :, None] +
                      u1[:, None, :] * b[None, :, None] +
                      u2[:, None, :] * c[None, :, None])
                h, w = p.tex.shape[:2]
                px = (np.mod(ut[..., 0], 1.0) * (w - 1)).astype(np.int64)
                py = (np.mod(ut[..., 1], 1.0) * (h - 1)).astype(np.int64)
                cols = p.tex[py, px].astype(np.float32) * p.bcf[None, None, :]
            else:
                cols = np.broadcast_to((p.bcf * 255.0).astype(np.float32),
                                       pts.shape).copy()
            flat_parts.append(flat.ravel())
            col_parts.append(cols.reshape(-1, 3))

    flat = np.concatenate(flat_parts)
    cols = np.concatenate(col_parts).astype(np.float64)
    uniq, inv = np.unique(flat, return_inverse=True)
    cnt = np.bincount(inv).astype(np.float64)
    avg = np.empty((len(uniq), 3))
    for ch in range(3):
        avg[:, ch] = np.bincount(inv, weights=cols[:, ch]) / cnt
    return uniq, np.clip(avg, 0, 255)


def flood_solid(shell, cavity_fill, what):
    """The shell, plus every sealed pocket this caller wants filled.

    -- A ROOM IS NOT A SEAM, AND SIZE IS WHAT TELLS THEM APART -------------

    Filling everything the outside flood misses is right for a rock and WRONG
    for a building. This model's rooms have no door cut through the exterior
    wall, so they are sealed as far as a flood is concerned -- and filling them
    turns the building into a 33 x 36 x 23 m block of concrete you can only
    stand on top of. Measured on the first run: 3.0 M voxels of "cavity",
    against a 294 k shell.

    The gap between the two skins of a wall and the inside of a room are both
    unreachable. What separates them is that one is a few hundred voxels and
    the other is hundreds of thousands, so the rule is a volume: fill a cavity
    below `cavity_fill`, leave a bigger one as air. The report below prints
    what was kept and what was filled so that stays checkable rather than
    assumed -- on this model the largest FILLED pocket is under 20 k and the
    smallest KEPT one is 729 k, so the threshold is nowhere near delicate.

    `cavity_fill` of None fills everything, which is what a closed slab wants
    and is why the platform does not come through here with the building.
    """
    pad = np.zeros((NX + 2, NY + 2, NZ + 2), dtype=bool)
    ix, iy, iz = np.unravel_index(shell, (NX, NY, NZ))
    pad[ix + 1, iy + 1, iz + 1] = True

    # The barrier: the shell, thickened by one, used ONLY to stop the flood.
    # A seam narrower than a voxel otherwise lets the exterior pour into a
    # solid and it comes back hollow. The ring is thrown away, so nothing is
    # fattened -- and at 10 cm it cannot close a door or a window.
    barrier = pad.copy()
    for ax in (0, 1, 2):
        barrier |= np.roll(pad, 1, axis=ax)
        barrier |= np.roll(pad, -1, axis=ax)

    lbl, ncomp = ndimage.label(~barrier, structure=ndimage.generate_binary_structure(3, 1))
    outside_id = lbl[0, 0, 0]
    if outside_id == 0:
        sys.exit('the corner of the grid is inside the barrier -- nothing to flood from')
    sizes = np.bincount(lbl.ravel())
    sizes[0] = 0
    sizes[outside_id] = 0

    if cavity_fill is None:
        fillable = np.nonzero(sizes > 0)[0]
        kept = np.empty(0, dtype=np.int64)
    else:
        fillable = np.nonzero((sizes > 0) & (sizes < cavity_fill))[0]
        kept = np.nonzero(sizes >= cavity_fill)[0]

    out = np.isin(lbl, fillable)[1:-1, 1:-1, 1:-1].copy()
    out[ix, iy, iz] = True
    print('  %-8s %d shell voxels; %d of %d sealed pockets filled (%d voxels)'
          % (what, len(shell), len(fillable), ncomp - 1,
             int(out.sum()) - len(shell)))
    for c in kept[np.argsort(-sizes[kept])][:6]:
        print('            left %9d voxels (%6.0f m3) as air -- a room'
              % (sizes[c], sizes[c] * VOX ** 3))
    return out


# -- THE PLATFORM IS FILLED SOLID; THE BUILDING IS NOT ----------------------
#
# The volume rule above cannot separate these two on its own, and the run that
# proved it is worth keeping: the platform's inside is 729 140 voxels and the
# building's rooms are 1 794 385, so ANY threshold that leaves the rooms open
# leaves the slab hollow as well. They are not the same kind of thing and no
# measurement of their size will ever say so.
#
# So the platform is named. It is one mesh -- Plane.020_0, twelve triangles, a
# closed 33 x 22.6 m cuboid 1.33 m thick from y = -1.33 to y = 0 -- and it is
# the ground of this level: the user asked for it in the same breath as the
# building ("voxelize the platform that its on as well") precisely because it
# is the part you stand on. A hollow one would mesh its own underside, double
# the triangles for nothing visible, and give way the first time a tool cut
# into it.
PLATFORM_MESHES = {'Plane.020_0'}

t0 = time.time()
plat = [p for p in prims if p.name in PLATFORM_MESHES]
bld = [p for p in prims if p.name not in PLATFORM_MESHES]
if not plat:
    sys.exit('no platform mesh found -- %s is not in this file' % PLATFORM_MESHES)

pflat, pcol = surface(plat)
bflat, bcol = surface(bld)
print('  sampled in %.1f s' % (time.time() - t0))

# THE SECOND FLOOD SEES BOTH SHELLS, and it has to. Run over the building's own
# triangles alone, every room in this model comes back OPEN: the building has no
# floor of its own -- the platform IS its floor -- so the flood walks in from
# underneath and the size rule never gets a sealed pocket to judge. Measured:
# 13 pockets, all of them seams, against 15 and a 1.79 M room when the slab is
# there to close the bottom. So the barrier is the whole level and only the
# FILLING RULE differs between the two passes.
psolid = flood_solid(pflat, None, 'platform')
csolid = flood_solid(np.union1d(pflat, bflat), CAVITY_FILL, 'building')
solid = psolid | csolid
del psolid, csolid

# The shell is both groups' together -- a voxel the building painted keeps the
# building's colour even where the platform also filled it, because the
# building is what you would be looking at there.
shell_flat = np.concatenate([pflat, bflat])
shell_col = np.concatenate([pcol, bcol])
_o = np.argsort(np.concatenate([np.zeros(len(pflat), np.int8),
                                np.ones(len(bflat), np.int8)]), kind='stable')
shell_flat, shell_col = shell_flat[_o], shell_col[_o]
sx_, sy_, sz_ = np.unravel_index(shell_flat, (NX, NY, NZ))

nsolid = int(solid.sum())
print('  solid   %d voxels (%.1f%% of the grid) in %.1f s'
      % (nsolid, 100.0 * nsolid / (NX * NY * NZ), time.time() - t0))


# ---------------------------------------------------------------------------
# palette -- k-means over the shell colours, weighted by how many voxels wear them
# ---------------------------------------------------------------------------
def kmeans(x, w, k, iters=40, seed=12345):
    rng = np.random.default_rng(seed)
    # k-means++ over the weighted points, so a colour a thousand voxels wear is
    # a thousand times likelier to seed a centre than one that six do.
    c = x[rng.choice(len(x), p=w / w.sum())][None, :]
    while len(c) < k:
        d2 = ((x[:, None, :] - c[None, :, :]) ** 2).sum(2).min(1)
        pr = d2 * w
        if pr.sum() <= 0:
            break
        c = np.vstack([c, x[rng.choice(len(x), p=pr / pr.sum())]])
    for _ in range(iters):
        lab = ((x[:, None, :] - c[None, :, :]) ** 2).sum(2).argmin(1)
        for j in range(len(c)):
            m = lab == j
            if m.any():
                c[j] = (x[m] * w[m, None]).sum(0) / w[m].sum()
    return c, ((x[:, None, :] - c[None, :, :]) ** 2).sum(2).argmin(1)


# Dedupe to whole shades first: 1.4 M shell voxels against a few tens of
# thousands of distinct 8-bit colours, and k-means over the duplicates is the
# same answer at forty times the cost.
q = np.round(shell_col).astype(np.int64)
key = (q[:, 0] * 256 + q[:, 1]) * 256 + q[:, 2]
ukey, uinv, ucnt = np.unique(key, return_inverse=True, return_counts=True)
upts = np.stack([(ukey >> 16) & 255, (ukey >> 8) & 255, ukey & 255], axis=1).astype(np.float64)
print('  colour  %d distinct shades on the shell -> %d palette entries'
      % (len(upts), PAL_N))
centres, ulab = kmeans(upts, ucnt.astype(np.float64), PAL_N)
shell_idx = ulab[uinv].astype(np.uint8)

# The interior takes the shade its own walls most often wear -- it is only ever
# seen where a tool has cut into the thing, and that is the colour the cut face
# beside it will be.
inner_idx = np.uint8(np.bincount(shell_idx, minlength=PAL_N).argmax())

PAL = np.clip(np.round(centres), 0, 255).astype(np.uint8)
err = np.sqrt((((upts - centres[ulab]) ** 2).sum(1) * ucnt).sum() / ucnt.sum())
print('  quantized at %.1f/255 RMS' % err)

idxgrid = np.zeros((NX, NY, NZ), dtype=np.uint8)
idxgrid[solid] = inner_idx + 1                       # 1-based; 0 is empty
idxgrid[sx_, sy_, sz_] = shell_idx + 1


# ---------------------------------------------------------------------------
# write -- world y-up becomes .vox z-up, and the grid is split to fit the bytes
# ---------------------------------------------------------------------------
def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def s_dict(d):
    out = struct.pack('<i', len(d))
    for k, v in d.items():
        out += struct.pack('<i', len(k)) + k + struct.pack('<i', len(v)) + v
    return out


def cuts(n):
    """Piece boundaries along one axis: as few as fit, and even, so no piece is
    a sliver. Even matters -- a 331 split as 250 + 81 meshes the same but reads
    as a mistake in MagicaVoxel, and the tall-tree writer's slabs_for does the
    same thing for the same reason."""
    k = int(math.ceil(n / float(MAX_AXIS)))
    edges = [int(round(i * n / float(k))) for i in range(k + 1)]
    return [(edges[i], edges[i + 1]) for i in range(k)]


# .vox axes: x = world x, y = world z, z = world y (height)
vgrid = np.transpose(idxgrid, (0, 2, 1))
VX, VY, VZ = vgrid.shape

pieces = []
for x0, x1 in cuts(VX):
    for y0, y1 in cuts(VY):
        for z0, z1 in cuts(VZ):
            sub = vgrid[x0:x1, y0:y1, z0:z1]
            nz = np.nonzero(sub)
            if not len(nz[0]):
                continue    # an empty piece would be a stray model in the graph
            pieces.append((x0, y0, z0, x1 - x0, y1 - y0, z1 - z0,
                           np.stack([nz[0], nz[1], nz[2],
                                     sub[nz].astype(np.int64)], axis=1)))

body = b''
for (_x0, _y0, _z0, sx, sy, sz, vox) in pieces:
    body += chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body += chunk(b'XYZI', struct.pack('<I', len(vox)) +
                  vox.astype(np.uint8).tobytes())

if len(pieces) > 1:
    # A root nTRN over an nGRP over one nTRN/nSHP pair per piece -- the shape
    # MagicaVoxel writes and the shape voxParse walks. nTRN names a piece's
    # CENTRE and voxParse takes `origin = translation - size / 2` with C's
    # truncating divide, so the translation written here is the minimum corner
    # plus that same halving. Getting this wrong shifts a piece by one voxel,
    # which shows up as a seam and nothing else.
    body += chunk(b'nTRN', struct.pack('<i', 0) + s_dict({}) +
                  struct.pack('<iiii', 1, -1, -1, 1) + s_dict({}))
    kids = [2 + 2 * i for i in range(len(pieces))]
    body += chunk(b'nGRP', struct.pack('<i', 1) + s_dict({}) +
                  struct.pack('<i', len(kids)) +
                  b''.join(struct.pack('<i', k) for k in kids))
    for i, (x0, y0, z0, sx, sy, sz, _v) in enumerate(pieces):
        t = b'%d %d %d' % (x0 + sx // 2, y0 + sy // 2, z0 + sz // 2)
        body += chunk(b'nTRN', struct.pack('<i', 2 + 2 * i) + s_dict({}) +
                      struct.pack('<iiii', 3 + 2 * i, -1, 0, 1) + s_dict({b'_t': t}))
        body += chunk(b'nSHP', struct.pack('<i', 3 + 2 * i) + s_dict({}) +
                      struct.pack('<i', 1) + struct.pack('<i', i) + s_dict({}))
    for layer in range(8):
        body += chunk(b'LAYR', struct.pack('<i', layer) +
                      s_dict({b'_name': b'', b'_hidden': b'0'}) + struct.pack('<i', -1))

rgba = b''
for i in range(256):
    c = PAL[i] if i < len(PAL) else (0, 0, 0)
    rgba += struct.pack('<BBBB', int(c[0]), int(c[1]), int(c[2]), 255)
body += chunk(b'RGBA', rgba)

os.makedirs(OUTDIR, exist_ok=True)
open(OUT, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))

print('  wrote   %s' % OUT)
print('          %d x %d x %d .vox voxels in %d pieces, %.1f MB'
      % (VX, VY, VZ, len(pieces), os.path.getsize(OUT) / 1e6))
print('          %.1f x %.1f x %.1f m in the world'
      % (VX * VOX, VY * VOX, VZ * VOX))

# An estimate of what the mesher will make of it, because the one thing that
# can make this asset unusable is arriving as several million triangles.
faces = 0
for ax in (0, 1, 2):
    for sh in (1, -1):
        faces += int((solid & ~np.roll(solid, sh, axis=ax)).sum())
print('          ~%d exposed faces -> ~%d triangles once meshed' % (faces, faces * 2))


# ---------------------------------------------------------------------------
# PREVIEW -- four elevations, RAW beside QUANTIZED.
#
# Not decoration. Everything above this line is measured in numbers that cannot
# tell you the one thing you actually need to know, which is whether the level
# looks like the building; and the only other way to find out costs a 4 GB
# build and a teleport. The two columns are there because the failures they
# catch are different: the RAW side is what the sampler read out of the
# textures, so a wall that is wrong there is a UV or a material problem, and
# the QUANTIZED side is what the file holds, so a wall that is right on the
# left and wrong on the right is PAL_N being too mean.
# ---------------------------------------------------------------------------
def elevations(rgb_of_shell):
    """Nearest-hit projection of the solid, four ways round, flat lit."""
    vol = np.zeros((NX, NY, NZ, 3), dtype=np.uint8)
    vol[solid] = (90, 90, 96)          # filled interior, seen only in section
    vol[sx_, sy_, sz_] = rgb_of_shell
    shots = []
    for axis, flip, name in ((2, False, 'front'), (2, True, 'back'),
                             (0, False, 'left'), (0, True, 'right')):
        occ = np.moveaxis(solid, axis, 0)
        col = np.moveaxis(vol, axis, 0)
        if flip:
            occ, col = occ[::-1], col[::-1]
        first = np.argmax(occ, axis=0)
        img = np.take_along_axis(col, first[None, :, :, None], 0)[0]
        img = img.copy()
        img[~occ.any(axis=0)] = (18, 18, 26)
        # axis 0 leaves (x|z, height); height must go UP the page
        shots.append((name, np.transpose(img, (1, 0, 2))[::-1]))
    return shots


try:
    raw = elevations(np.clip(np.round(shell_col), 0, 255).astype(np.uint8))
    qnt = elevations(PAL[shell_idx])
    cw = max(s.shape[1] for _, s in raw)
    ch = max(s.shape[0] for _, s in raw)
    sheet = Image.new('RGB', (2 * (cw + 8), 4 * (ch + 16)), (0, 0, 0))
    for r, ((name, a), (_n, b)) in enumerate(zip(raw, qnt)):
        for c, im in ((0, a), (1, b)):
            sheet.paste(Image.fromarray(im), (c * (cw + 8), r * (ch + 16) + 12))
    prev = os.path.splitext(OUT)[0] + '_preview.png'
    sheet.save(prev)
    print('          preview %s   (left: sampled, right: %d-entry palette)'
          % (prev, PAL_N))
except Exception as exc:            # a preview must never fail the asset
    print('          preview skipped: %s' % exc)
