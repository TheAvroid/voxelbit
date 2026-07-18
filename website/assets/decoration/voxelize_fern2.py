"""Voxelize fern.glb (single large fern plant) to 10cm voxels, quantize colors, emit JSON.

Output: C:/voxelbit/website/assets/decoration/fern2.json
  { "pal": [[r,g,b],...], "ferns": [ {"name", "sx","sy","sz", "vox":[x|y<<8|z<<16|ci<<24, ...]} ] }
Model axes match the engine's item convention: x = width, y = depth, z = height.

fern.glb quirks vs the ferns_grass pipeline:
- raw units are ~cm-scale (695 x 309 x 657) with no real-world scale — normalized so the
  plant stands TARGET_H voxels tall
- the diffuse texture lives in KHR_materials_pbrSpecularGlossiness.diffuseTexture
  (alphaMode BLEND leaf cards) — sample RGBA there, skip texels with alpha < 128
"""
import struct, json, io, math
import numpy as np
from PIL import Image

GLB = r"C:\voxelbit\website\assets\decoration\fern.glb"
OUT = r"C:\voxelbit\website\assets\decoration\fern2.json"
TARGET_H = 11   # voxels tall (~1.1 m fern)
PALN = 10

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
    dt = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}[a['componentType']]
    stride = bv.get('byteStride')
    itemsize = np.dtype(dt).itemsize * ncomp
    if stride and stride != itemsize:
        rows = [np.frombuffer(BIN, dt, ncomp, off + i * stride) for i in range(a['count'])]
        return np.stack(rows)
    return np.frombuffer(BIN, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)

def img_pixels(ti):
    tex = js['textures'][ti]
    im = js['images'][tex['source']]
    bv = js['bufferViews'][im['bufferView']]
    off = bv.get('byteOffset', 0)
    raw = BIN[off: off + bv['byteLength']]
    return np.asarray(Image.open(io.BytesIO(raw)).convert('RGBA'))

def node_mat(n):
    if 'matrix' in n:
        return np.array(n['matrix'], dtype=np.float64).reshape(4, 4).T
    T = n.get('translation', [0, 0, 0]); R = n.get('rotation', [0, 0, 0, 1]); S = n.get('scale', [1, 1, 1])
    x, y, z, w = R
    rm = np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                   [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                   [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])
    m = np.eye(4); m[:3, :3] = rm * np.array(S)[None, :]; m[:3, 3] = T
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

out_ferns = []
all_cols = []
fern_voxels = []
names = []
for ni, n in enumerate(js['nodes']):
    if n.get('mesh') is None:
        continue
    mesh = js['meshes'][n['mesh']]
    name = mesh.get('name', f'fern{ni}')
    pr = mesh['primitives'][0]
    pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
    uv = acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64)
    idx = acc_data(pr['indices']).astype(np.int64).ravel() if 'indices' in pr else np.arange(len(pos))
    mat = js['materials'][pr['material']]
    sg = mat.get('extensions', {}).get('KHR_materials_pbrSpecularGlossiness', {})
    dt_ = sg.get('diffuseTexture') or mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
    dfac = np.array((sg.get('diffuseFactor') or [1, 1, 1, 1])[:3])
    tex = img_pixels(dt_['index'])

    M = world_mat(ni)
    wp = (M[:3, :3] @ pos.T).T + M[:3, 3]
    mn = wp.min(0); mx = wp.max(0)
    vox_size = (mx[1] - mn[1]) / TARGET_H     # normalize: world-Y (up) extent -> TARGET_H voxels
    print(f"{name}: raw {mx[0]-mn[0]:.0f} x {mx[1]-mn[1]:.0f} x {mx[2]-mn[2]:.0f} units -> vox unit {vox_size:.1f}")

    surf = {}
    tris = idx.reshape(-1, 3)
    for t in tris:
        p0, p1, p2 = wp[t[0]], wp[t[1]], wp[t[2]]
        u0, u1, u2 = uv[t[0]], uv[t[1]], uv[t[2]]
        e = max(np.linalg.norm(p1 - p0), np.linalg.norm(p2 - p0), np.linalg.norm(p2 - p1))
        nsub = max(1, int(math.ceil(e / (vox_size * 0.5))))
        for i in range(nsub + 1):
            for j in range(nsub + 1 - i):
                a = i / nsub; b = j / nsub; c = 1 - a - b
                p = p0 * a + p1 * b + p2 * c
                v = tuple(int(q) for q in ((p - mn) / vox_size))
                if v not in surf:
                    ut = u0 * a + u1 * b + u2 * c
                    h, w2 = tex.shape[:2]
                    px = int(ut[0] % 1.0 * (w2 - 1)); py = int(ut[1] % 1.0 * (h - 1))
                    rgba = tex[py, px].astype(float)
                    if rgba[3] < 128:
                        continue
                    surf[v] = rgba[:3] * dfac
    print(f"  voxels {len(surf)}")
    for c in surf.values():
        all_cols.append(c)
    fern_voxels.append(surf)
    names.append(name)

cols = np.clip(np.array(all_cols), 0, 255)

def median_cut(colors, n):
    boxes = [colors]
    while len(boxes) < n:
        bi = max(range(len(boxes)), key=lambda i: np.ptp(boxes[i], 0).max() * (len(boxes[i]) > 1))
        b = boxes[bi]
        if len(b) <= 1:
            break
        ax = np.ptp(b, 0).argmax()
        med = np.median(b[:, ax])
        lo = b[b[:, ax] <= med]; hi = b[b[:, ax] > med]
        if len(lo) == 0 or len(hi) == 0:
            order = b[:, ax].argsort()
            lo = b[order[:len(b) // 2]]; hi = b[order[len(b) // 2:]]
        boxes[bi:bi + 1] = [lo, hi]
    return np.array([b.mean(0) for b in boxes])

pal = median_cut(cols, PALN)
print("palette:", [[int(c) for c in p] for p in pal])

def nearest(c):
    return int(((pal - c) ** 2).sum(1).argmin())

for name, vx in zip(names, fern_voxels):
    xs = [k[0] for k in vx]; ys = [k[1] for k in vx]; zs = [k[2] for k in vx]
    x0, y0, z0 = int(min(xs)), int(min(ys)), int(min(zs))
    sx = int(max(xs)) - x0 + 1; sy = int(max(ys)) - y0 + 1; sz = int(max(zs)) - z0 + 1
    packed = []
    for (x, y, z), c in vx.items():
        mx2 = x - x0; my2 = z - z0; mz2 = y - y0   # world Y-up -> model z-up
        packed.append(mx2 | (my2 << 8) | (mz2 << 16) | (nearest(c) << 24))
    out_ferns.append(dict(name=name, sx=sx, sy=sz, sz=sy, vox=sorted(packed)))

out = dict(pal=[[int(round(c)) for c in p] for p in pal], ferns=out_ferns)
s = json.dumps(out, separators=(',', ':'))
open(OUT, 'w').write(s)
print(f"wrote {OUT} ({len(s)/1024:.0f} KB)")
for rr in out['ferns']:
    print(f"  {rr['name']:24s} {rr['sx']}x{rr['sy']}x{rr['sz']} vox {len(rr['vox'])}")
