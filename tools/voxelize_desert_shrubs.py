"""SUPERSEDED 2026-08-16 - DO NOT RUN AGAINST game/assets/foilage/desert_shrub/.

The six shrubs in that folder are now HAND-AUTHORED by the user (1.vox..6.vox, "I made modifications
and renamed the files"). This script writes shrub_1.vox..shrub_14.vox from the GLB, which would leave
fourteen bakes sitting beside the six real assets - and src/assets/bow.js fetches 1..6, so the bakes
would be dead weight nobody sees while the real art stayed one directory listing away from being
deleted by hand. The old "PALN pinned to len(SHRUBC)" contract is dead with it: the loader now reads
whatever shades a file contains (models.js voxColsUsed) and resolves them onto SHRUBC + SHRUBF.
Kept for the GLB pipeline itself - the component split, the 10 cm ruler and the saturation pass are
still the reference for any future desert plant. Point OUTDIR somewhere else before running it.

Voxelize source/glb/desert_shrubs.glb to 10 cm voxels, split it into individual plants, emit one .vox each.

Input:  source/glb/desert_shrubs.glb              (source sculpt - gitignored, never shipped)
Output: game/assets/foilage/desert_shrub/shrub_N.vox   (the runtime asset - src/assets/bow.js fetches these)

10 CM, THE SAME VOXEL THE CACTI ARE BAKED AT. VOX below is metres per voxel and it matches
voxelize_cacti.py exactly, which is what makes a 1.6 m bush sit believably beside a 4.4 m saguaro: both
are measured against the same ruler, neither is fitted to a target voxel count.

GREEN, ON THE CACTUS PALETTE (user 2026-08-16). See the CACVOX block near the bottom - the ramp is read
out of the shipped cactus_1.vox, not hand-copied, and PALN is pinned to len(SHRUBC) in src/assets/palette.js.

SPLIT='components': desert_shrubs.glb is MATERIAL GROUPS (agave / yucca leaf / ocotillo branch / creosote /
cholla / bark) all overlapping in one ~6 m cluster, exactly like cacti.glb - so the plants have to be
recovered as 26-connected components, not split by mesh. Splitting by mesh would hand back overlapping
half-plants: a yucca with no leaves, and leaves with no yucca.

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_desert_shrubs.py
numpy and Pillow live there; the msys2 interpreter has neither, and no pip to add them.
"""
import struct, json, io, math, os, sys
from collections import deque
import numpy as np
from PIL import Image

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB = os.path.join(_ROOT, 'source', 'glb', 'desert_shrubs.glb')
SPLIT = 'components'   # desert_shrubs.glb is MATERIAL GROUPS (agave / yucca leaf / ocotillo branch / creosote /
                       # cholla / bark) all overlapping in one ~6 m cluster, exactly like cacti.glb — so the plants
                       # have to be recovered as 26-connected components, not split by mesh.
VOX = 0.1          # metres per voxel — IDENTICAL to voxelize_cacti.py, deliberately (see the docstring)
PALN = 2           # shared quantized shades, PINNED to len(SHRUBC) in src/assets/palette.js. Emitting more
                   # shades than the loader has ids for does not add detail, it MINTS PALETTE ENTRIES on a
                   # 240/256 table — see the CACVOX block below.
MINVOX = 40        # a component smaller than this is a stray shard, not a plant (shrubs are small)
# ── SATURATION (user 2026-08-15: "washed out green, saturate the green a bit more") ── the source textures
# measure 17-28 saturation out of 255, and at that albedo the desert sun washes the plants to near-white beside
# sand at [205,178,122] - they read as bleached posts, not cacti. This pushes each voxel away from its own grey
# and darkens it slightly, which is the pair of moves that makes a muted albedo read as a colour under bright
# light. Applied AFTER the shell blur and BEFORE quantization, so the palette is cut on the final colours.
# 1.0 restores the faithful bake.
SAT = 1.25         # how far to push each colour from its grey
VAL = 0.95         # and how much to darken, so the boost does not just brighten
# NOTE the value depends on the SOURCE. cacti.glb's textures measure 17-28 saturation and needed 2.3 to read as
# green at all; cactus.glb's diffuse maps are already properly saturated, and 2.3 there clamps the blue channel
# to zero and turns the plants radioactive. Override from the command line while tuning: --sat=N --val=N
for _a in sys.argv[1:]:
    if _a.startswith('--sat='): SAT = float(_a[6:])
    if _a.startswith('--val='): VAL = float(_a[6:])

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
    return np.asarray(Image.open(io.BytesIO(BIN[off: off + bv['byteLength']])).convert('RGB'))


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


parts = []
for ni, n in enumerate(js['nodes']):
    if n.get('mesh') is None:
        continue
    mesh = js['meshes'][n['mesh']]
    pr = mesh['primitives'][0]
    pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
    uv = acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64) if 'TEXCOORD_0' in pr['attributes'] else None
    idx = acc_data(pr['indices']).astype(np.int64).ravel() if 'indices' in pr else np.arange(len(pos))
    mat = js['materials'][pr['material']] if pr.get('material') is not None else {}
    # ── TWO PLACES THE ALBEDO CAN LIVE ── rocks.glb and cacti.glb use core pbrMetallicRoughness, but
    # cactus.glb is authored with KHR_materials_pbrSpecularGlossiness (it is in extensionsREQUIRED), where the
    # albedo is `diffuseTexture`/`diffuseFactor` and pbrMetallicRoughness is absent entirely. Reading only the
    # core block gave every material no colour at all, fell back to white, and baked nine plants in a single
    # shade of grey. Prefer the extension when present, else the core block.
    sg = mat.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness')
    pbr = mat.get('pbrMetallicRoughness', {})
    if sg is not None:
        bcf = sg.get('diffuseFactor', [1, 1, 1, 1])
        bct = sg.get('diffuseTexture')
    else:
        bcf = pbr.get('baseColorFactor', [1, 1, 1, 1])
        bct = pbr.get('baseColorTexture')
    tex = get_tex(bct['index']) if bct is not None else None
    M = world_mat(ni)
    wp = (M[:3, :3] @ pos.T).T + M[:3, 3]
    parts.append(dict(name=mesh.get('name', '?'), wp=wp, uv=uv, idx=idx, tex=tex, bcf=bcf))

allmn = np.min([p['wp'].min(0) for p in parts], axis=0)
allmx = np.max([p['wp'].max(0) for p in parts], axis=0)
print('scene %.2f x %.2f x %.2f m from %d material groups' % (allmx[0] - allmn[0], allmx[1] - allmn[1], allmx[2] - allmn[2], len(parts)))

surf = {}
owner = {}                                             # voxel -> index of the mesh that first claimed it
for pi, p in enumerate(parts):
    wp, uv, idx, tex = p['wp'], p['uv'], p['idx'], p['tex']
    for t in idx.reshape(-1, 3):
        p0, p1, p2 = wp[t[0]], wp[t[1]], wp[t[2]]
        e = max(np.linalg.norm(p1 - p0), np.linalg.norm(p2 - p0), np.linalg.norm(p2 - p1))
        nsub = max(1, int(math.ceil(e / (VOX * 0.5))))
        for i in range(nsub + 1):
            for j in range(nsub + 1 - i):
                a = i / nsub
                b = j / nsub
                c = 1 - a - b
                q = (p0 * a + p1 * b + p2 * c - allmn) / VOX
                v = (int(q[0]), int(q[1]), int(q[2]))
                if v in surf:
                    continue
                owner[v] = pi
                if tex is not None and uv is not None:
                    ut = uv[t[0]] * a + uv[t[1]] * b + uv[t[2]] * c
                    h2, w2 = tex.shape[:2]
                    col = tex[int(ut[1] % 1.0 * (h2 - 1)), int(ut[0] % 1.0 * (w2 - 1))].astype(float) * np.array(p['bcf'][:3])
                else:
                    col = np.array(p['bcf'][:3]) * 255.0
                surf[v] = col
print('surface shell %d voxels' % len(surf))

# ONE blur pass, not the three rocks26 uses. Rocks are one muted material and their texels are
# speckled, so three passes only helped. A cactus carries real colour structure at voxel scale -
# green body, pale spines, a yellow flower a few voxels across - and on a 0.5 m plant three passes
# of a 26-neighbour box blur smears every one of them into the same grey-green. Measured: the
# flower's (225,191,92) never survived to the palette at all.
for _ in range(1):
    sm = {}
    for v, c in surf.items():
        accc = c.copy()
        n2 = 1
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    if dx or dy or dz:
                        nb = surf.get((v[0] + dx, v[1] + dy, v[2] + dz))
                        if nb is not None:
                            accc = accc + nb
                            n2 += 1
        sm[v] = accc / n2
    surf = sm

for v in surf:                                         # saturate + darken, per voxel, before anything is quantized
    c = surf[v]
    g = float(c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114)   # luma, not the channel mean: pushing away from the mean turns dark greens muddy
    surf[v] = np.clip((g + (c - g) * SAT) * VAL, 0, 255)

if SPLIT == 'mesh':
    byMesh = {}
    for v, pi in owner.items():
        byMesh.setdefault(pi, []).append(v)
    comps = sorted(byMesh.values(), key=len, reverse=True)
    print('%d meshes -> %d plants (SPLIT=mesh)' % (len(parts), len(comps)))
else:
  NB26 = [(dx, dy, dz) for dx in (-1, 0, 1) for dy in (-1, 0, 1) for dz in (-1, 0, 1) if dx or dy or dz]
  seen = set()
  comps = []
  for v0 in surf:
    if v0 in seen:
        continue
    q = deque([v0])
    seen.add(v0)
    comp = []
    while q:
        v = q.popleft()
        comp.append(v)
        for dd in NB26:
            nb = (v[0] + dd[0], v[1] + dd[1], v[2] + dd[2])
            if nb in surf and nb not in seen:
                seen.add(nb)
                q.append(nb)
    comps.append(comp)
  comps.sort(key=len, reverse=True)
  print('%d connected components; keeping those with >= %d voxels' % (len(comps), MINVOX))

all_cols = []
models = []
for comp in comps:
    if len(comp) < MINVOX:
        continue
    cmn = np.min(comp, axis=0)
    cmx = np.max(comp, axis=0)
    dims = (cmx - cmn) + 1
    g = np.zeros(dims + 2, dtype=np.uint8)
    for v in comp:
        g[v[0] - cmn[0] + 1, v[1] - cmn[1] + 1, v[2] - cmn[2] + 1] = 1
    qq = deque([(0, 0, 0)])
    g[0, 0, 0] = 2
    D = dims + 2
    while qq:                                          # exterior flood; whatever is still 0 is interior
        x, y, z = qq.popleft()
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            nx, ny, nz = x + dx, y + dy, z + dz
            if 0 <= nx < D[0] and 0 <= ny < D[1] and 0 <= nz < D[2] and g[nx, ny, nz] == 0:
                g[nx, ny, nz] = 2
                qq.append((nx, ny, nz))
    vx = {}
    for v in comp:
        vx[(v[0] - cmn[0], v[1] - cmn[1], v[2] - cmn[2])] = surf[v]
    avg = np.mean([surf[v] for v in comp], axis=0) * 0.82
    for (x, y, z) in (np.argwhere(g == 0) - 1):
        vx[(int(x), int(y), int(z))] = avg
    all_cols.extend(surf[v] for v in comp)             # SURFACE colours only. The interior is one averaged grey per plant, and letting those into the median cut let them outvote every real feature: the interior is ~60% of the voxels and none of it is visible until the plant is cut open.
    models.append(dict(vx=vx, dims=dims, scols=[surf[v] for v in comp]))   # scols = this plant's SURFACE colours, kept for the per-model luminance rank at the bottom
    print('  plant %-2d  %2d w x %2d d x %2d h vox  (%.1f x %.1f x %.1f m)  %d voxels'
          % (len(models), dims[0], dims[2], dims[1], dims[0] * VOX, dims[2] * VOX, dims[1] * VOX, len(vx)))


# ── CUT ON DISTINCT COLOURS, NOT PER-VOXEL ── kept only as a report line: the recolour below does not
# quantize the shrubs' own colours at all, it re-ranks them onto somebody else's ramp.
_uc = np.unique(np.clip(np.array(all_cols), 0, 255).round().astype(int), axis=0).astype(int)
print('%d surface voxels -> %d distinct colours' % (len(all_cols), len(_uc)))

# ── GREEN, ON THE CACTUS'S OWN PALETTE (user 2026-08-16: "make them green instead of brown. make them
# follow the same pallette as the cactus") ── replaces the pine-bark browns this tool used to emit. The ramp
# is READ OUT OF game/assets/foilage/cactus/cactus_1.vox rather than hand-copied, so it is literally the
# shipped cactus palette and a cactus re-bake carries the shrubs with it. That file holds body greens, spine
# browns, flower pinks and a pale yellow; only the GREEN-DOMINANT entries are kept (g > b, and g > r by a
# 10/255 margin, which is what drops the olive-browns at the top of the body ramp), then sampled evenly by
# luminance down to PALN shades.
#
# PALN IS PINNED TO len(SHRUBC) IN src/assets/palette.js, and that is not a coincidence: the loader
# (src/assets/bow.js) resolves every shade through parseVoxModel's colMap, pre-filled from SHRUBC, so any
# shade this tool emits that is NOT one of those exact colours would mint a palette entry on a 240/256 table.
# Two shades, two ids, zero new entries. Raise both together or neither.
CACVOX = os.path.join(_ROOT, 'game', 'assets', 'foilage', 'cactus', 'cactus_1.vox')


def vox_palette(path):
    """The RGB entries a .vox file's voxels actually use, in MagicaVoxel index order."""
    d = open(path, 'rb').read()
    pal, used, o = [], set(), 8
    while o < len(d):
        cid = d[o:o + 4]
        n, m = struct.unpack_from('<II', d, o + 4)
        c = d[o + 12: o + 12 + n]
        if cid == b'MAIN':
            o += 12 + n                                  # MAIN's children follow it inline
            continue
        if cid == b'RGBA':
            pal = [list(struct.unpack_from('<BBB', c, i * 4)) for i in range(256)]
        if cid == b'XYZI':
            cnt = struct.unpack_from('<I', c, 0)[0]
            used = {c[4 + i * 4 + 3] for i in range(cnt)}
        o += 12 + n + m
    return [pal[i - 1] for i in sorted(used)]


_lum = lambda c: c[0] * 0.299 + c[1] * 0.587 + c[2] * 0.114
_cac = vox_palette(CACVOX)
GREEN = sorted([c for c in _cac if c[1] > c[0] + 9 and c[1] > c[2]], key=_lum)
print('cactus palette: %d shades -> %d green %s' % (len(_cac), len(GREEN), GREEN))
assert len(GREEN) >= PALN, 'cactus_1.vox carries fewer than %d green shades' % PALN
pal = np.array([GREEN[round(k * (len(GREEN) - 1) / max(1, PALN - 1))] for k in range(PALN)], dtype=float)
print('palette:', [[int(c) for c in p] for p in pal])

# ── RANK BY LUMINANCE, PER MODEL, NOT NEAREST RGB ── two things are wrong with a nearest-colour match here.
# The shrubs' sculpted colours are nowhere near the cactus ramp in RGB space — that is the whole point of a
# recolour — so nearest() collapses every voxel onto whichever ramp entry sits closest to the cloud's centre
# and the plant comes out flat. (The old brown build had exactly this bug: it matched green sculpt colours
# against a brown ramp, and the `_ql` luminance ladder it computed to avoid that was never actually used.)
# And the rank has to be taken over THIS PLANT's own surface colours, not the whole set: measured on the
# global range, five of the fourteen came out a single flat shade, because the spread between plants is
# larger than the shading within one and swamped it. Per model, every plant gets both shades — with a
# two-entry ramp, the light/dark structure inside one bush is what reads at 5 m, not its average tone.
# INTERIOR voxels are excluded from the range (they are one averaged grey per plant, darker than anything on
# the surface, and nothing sees them until the bush is cut open) but still get ranked, hence the clamp.
out_models = []
for k, m in enumerate(models):
    _sl = [_lum(c) for c in m['scols']]
    _lo, _hi = min(_sl), max(_sl)
    span = (_hi - _lo) or 1.0
    packed = []
    for (x, y, z), c in m['vx'].items():
        ci = max(0, min(len(pal) - 1, int((_lum(c) - _lo) / span * len(pal))))
        packed.append(int(x) | (int(z) << 8) | (int(y) << 16) | (ci << 24))   # world Y-up -> model z-up, as rocks26 does. int() because the interior voxels come back from np.argwhere as int64
    out_models.append(dict(name='shrub_%d' % (k + 1), sx=int(m['dims'][0]), sy=int(m['dims'][2]),
                           sz=int(m['dims'][1]), vox=sorted(packed)))

# ── ONE .vox PER SHRUB, AND NOTHING ELSE ── these files ARE the runtime asset now: src/assets/bow.js fetches
# them directly, the way it fetches the cacti, so editing one and reloading is the whole round trip. The
# desert_shrubs.json this tool used to emit is gone — a second copy of the same models that the game ignored
# is precisely the trap the decor pipeline keeps setting.
# Every file carries the SAME PALN-entry palette (the cactus greens picked above) so the variants stay
# consistent with each other, and because the loader maps those exact RGBs onto SHRUBC, hand-editing with a
# colour that is NOT in the file's palette will mint a new palette id on a table that has 16 left.
def write_vox(path, m):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', m['sx'], m['sy'], m['sz']))
    vv = b''.join(struct.pack('<BBBB', p & 255, (p >> 8) & 255, (p >> 16) & 255, ((p >> 24) & 255) + 1)
                  for p in m['vox'])                     # +1: MagicaVoxel colour indices are 1-based into RGBA
    xyzi = chunk(b'XYZI', struct.pack('<I', len(m['vox'])) + vv)
    rgba = b''
    for i in range(256):
        c = out_pal[i] if i < len(out_pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', size + xyzi + chunk(b'RGBA', rgba)))


out_pal = [[int(round(c)) for c in p] for p in pal]
VOXDIR = os.path.join(_ROOT, 'game', 'assets', 'foilage', 'desert_shrub')
os.makedirs(VOXDIR, exist_ok=True)
for m in sorted(out_models, key=lambda r: r['sx'] * r['sy'] * r['sz']):
    write_vox(os.path.join(VOXDIR, m['name'] + '.vox'), m)
    print('  %-12s %2d x %2d x %2d vox  (%.1f x %.1f x %.1f m)  %4d voxels -> %s.vox'
          % (m['name'], m['sx'], m['sy'], m['sz'], m['sx'] * VOX, m['sy'] * VOX, m['sz'] * VOX, len(m['vox']), m['name']))
print('wrote %d shrub .vox into %s' % (len(out_models), VOXDIR))
