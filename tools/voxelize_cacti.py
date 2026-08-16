"""Voxelize source/glb/cacti.glb to 10 cm voxels, split it into individual plants, emit JSON.

Input:  source/glb/cactus.glb                 (source sculpt - gitignored, never shipped)
Output: game/assets/decoration/cacti.json     (baked runtime asset the game loads)
  { "pal": [[r,g,b],...], "cacti": [ {"name","sx","sy","sz","vox":[x|y<<8|z<<16|ci<<24, ...]} ... ] }

Same shape, axes and quantization as voxelize_rocks.py - see that file for the format notes.

TWO SOURCE FILES, TWO SHAPES, HENCE `SPLIT`:
  * cactus.glb (current) holds NINE meshes, one per plant, each at its own position - so the mesh
    IS the object and SPLIT='mesh' is exact.
  * cacti.glb holds ten MATERIAL GROUPS over one ~7 m cluster (cactus / spines 01 / saguaro stem /
    saguaro flower / prickly pear / ...), each spanning the whole scene. Splitting THAT by mesh
    hands back ten overlapping half-plants - a saguaro with no spines, and spines with no saguaro
    - so it needs SPLIT='components', which rasterizes everything into one grid and recovers the
    plants as 26-connected components.
Point GLB at the other file and flip SPLIT; nothing else changes.

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_cacti.py
numpy and Pillow live there; the msys2 interpreter has neither, and no pip to add them.
"""
import struct, json, io, math, os, sys
from collections import deque
import numpy as np
from PIL import Image

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB = os.path.join(_ROOT, 'source', 'glb', 'cactus.glb')
SPLIT = 'mesh'     # 'mesh' = one plant per mesh (cactus.glb) | 'components' = recover plants from one grid (cacti.glb)
OUT = os.path.join(_ROOT, 'game', 'assets', 'decoration', 'cacti.json')
VOX = 0.1          # metres per voxel
PALN = 10          # shared quantized shades. Was 16; cut to 10 (user 2026-08-15) because the cactus set was
                   # tipping the shared 256-entry palette over its ceiling — measured, the plants cost 14 ids and
                   # took the table from 14 free to 0 free with 3 colours silently nearest-matched. These nine
                   # plants are a tight green ramp plus spine browns, so they quantize down cheaply.
MINVOX = 60        # a component smaller than this is a stray shard, not a plant
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
    models.append(dict(vx=vx, dims=dims))
    print('  plant %-2d  %2d w x %2d d x %2d h vox  (%.1f x %.1f x %.1f m)  %d voxels'
          % (len(models), dims[0], dims[2], dims[1], dims[0] * VOX, dims[2] * VOX, dims[1] * VOX, len(vx)))


def median_cut(colors, n):
    boxes = [colors]
    while len(boxes) < n:
        bi = max(range(len(boxes)), key=lambda i: np.ptp(boxes[i], 0).max() * (len(boxes[i]) > 1))
        b = boxes[bi]
        if len(b) <= 1:
            break
        ax = np.ptp(b, 0).argmax()
        med = np.median(b[:, ax])
        lo = b[b[:, ax] <= med]
        hi = b[b[:, ax] > med]
        if len(lo) == 0 or len(hi) == 0:
            o = b[:, ax].argsort()
            lo = b[o[:len(b) // 2]]
            hi = b[o[len(b) // 2:]]
        boxes[bi:bi + 1] = [lo, hi]
    return np.array([b.mean(0) for b in boxes])


# ── CUT ON DISTINCT COLOURS, NOT PER-VOXEL ── rocks26 feeds the cut one entry per voxel, which is
# right when every voxel is the same muted rock. Here it silently deletes the small features: the
# prickly-pear flower is genuinely vivid in the source (saturation 125 against the body's 24) but
# it is a few dozen voxels against thousands, so a population-weighted cut spent all 16 shades on
# the green and rounded the flower into it. De-duplicating first lets a rare colour hold its own box.
_uc = np.unique(np.clip(np.array(all_cols), 0, 255).round().astype(int), axis=0).astype(float)
print('%d surface voxels -> %d distinct colours' % (len(all_cols), len(_uc)))
pal = median_cut(_uc, PALN)
print('palette:', [[int(c) for c in p] for p in pal])


def nearest(c):
    return int(((pal - c) ** 2).sum(1).argmin())


out_models = []
for k, m in enumerate(models):
    packed = []
    for (x, y, z), c in m['vx'].items():
        packed.append(int(x) | (int(z) << 8) | (int(y) << 16) | (nearest(c) << 24))   # world Y-up -> model z-up, as rocks26 does. int() because the interior voxels come back from np.argwhere as int64, which json refuses
    out_models.append(dict(name='cactus_%d' % (k + 1), sx=int(m['dims'][0]), sy=int(m['dims'][2]),
                           sz=int(m['dims'][1]), vox=sorted(packed)))

# ── ALSO EMIT ONE .vox PER PLANT ── MagicaVoxel format, so each variant can be opened and edited by hand
# next to pine5.vox and palm_tree.vox. Every file carries the SAME shared 16-shade palette, so the variants
# stay colour-consistent with each other and with cacti.json.
# NOTE (see the decor pipeline): the GAME loads cacti.json, not these. Editing a .vox here changes nothing on
# its own — re-run this tool to regenerate the .json, or rewire the loader.
def write_vox(path, m):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', m['sx'], m['sy'], m['sz']))
    vox = b''.join(struct.pack('<BBBB', p & 255, (p >> 8) & 255, (p >> 16) & 255, ((p >> 24) & 255) + 1)
                    for p in m['vox'])                 # +1: MagicaVoxel colour indices are 1-based into RGBA
    xyzi = chunk(b'XYZI', struct.pack('<I', len(m['vox'])) + vox)
    rgba = b''
    for i in range(256):
        c = out_pal[i] if i < len(out_pal) else [0, 0, 0]
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    body = size + xyzi + chunk(b'RGBA', rgba)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))

out_pal = [[int(round(c)) for c in p] for p in pal]
VOXDIR = os.path.join(_ROOT, 'game', 'assets', 'foilage', 'cactus')
os.makedirs(VOXDIR, exist_ok=True)
for m in out_models:
    fp = os.path.join(VOXDIR, m['name'] + '.vox')
    write_vox(fp, m)
    print('  %-14s %2d x %2d x %2d  %4d voxels -> %s' % (m['name'], m['sx'], m['sy'], m['sz'], len(m['vox']), os.path.basename(fp)))

out = dict(pal=[[int(round(c)) for c in p] for p in pal],
           cacti=sorted(out_models, key=lambda r: r['sx'] * r['sy'] * r['sz']))
s = json.dumps(out, separators=(',', ':'))
open(OUT, 'w').write(s)
print('wrote %s (%.0f KB), %d cacti' % (OUT, len(s) / 1024, len(out_models)))
