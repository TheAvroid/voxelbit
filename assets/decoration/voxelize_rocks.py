"""Voxelize the 26 rocks in rocks.glb to 10cm voxels, quantize colors, emit JSON.

Output: C:/voxelbit/website/assets/decoration/rocks26.json
  { "pal": [[r,g,b],...], "rocks": [ {"name", "sx","sy","sz", "vox":[x|y<<8|z<<16|ci<<24, ...]} ... ] }
Model axes match the engine's item convention: x = width, y = depth, z = height.
"""
import struct, json, io, math
import numpy as np
from PIL import Image

GLB = r"C:\voxelbit\website\assets\decoration\rocks.glb"
OUT = r"C:\voxelbit\website\assets\decoration\rocks26.json"
VOX = 0.1  # meters per voxel

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
        rows = []
        for i in range(a['count']):
            rows.append(np.frombuffer(BIN, dt, ncomp, off + i * stride))
        arr = np.stack(rows)
    else:
        arr = np.frombuffer(BIN, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)
    return arr

def img_pixels(ti):
    tex = js['textures'][ti]
    im = js['images'][tex['source']]
    bv = js['bufferViews'][im['bufferView']]
    off = bv.get('byteOffset', 0)
    raw = BIN[off: off + bv['byteLength']]
    img = Image.open(io.BytesIO(raw)).convert('RGB')
    return np.asarray(img)

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

# parent links
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

rocks = []
for ni, n in enumerate(js['nodes']):
    if n.get('mesh') is None:
        continue
    mesh = js['meshes'][n['mesh']]
    name = mesh.get('name', '')
    grp = name.split('_')[0].lower()
    if grp not in ('big', 'mid', 'runic', 'small'):
        continue
    pr = mesh['primitives'][0]
    pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
    uv = acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64) if 'TEXCOORD_0' in pr['attributes'] else None
    idx = acc_data(pr['indices']).astype(np.int64).ravel() if 'indices' in pr else np.arange(len(pos))
    mat = js['materials'][pr['material']]
    pbr = mat.get('pbrMetallicRoughness', {})
    bcf = pbr.get('baseColorFactor', [1, 1, 1, 1])
    bct = pbr.get('baseColorTexture')
    tex = get_tex(bct['index']) if bct is not None else None

    M = world_mat(ni)
    wp = (M[:3, :3] @ pos.T).T + M[:3, 3]

    mn = wp.min(0); mx = wp.max(0)
    size_m = mx - mn
    rocks.append(dict(name=name, ni=ni, wp=wp, uv=uv, idx=idx, tex=tex, bcf=bcf, mn=mn, mx=mx, size=size_m, grp=grp))

print(f"{len(rocks)} rocks found")
for r in sorted(rocks, key=lambda r: -max(r['size'])):
    print(f"  {r['name']:24s} size {r['size'][0]:6.2f} x {r['size'][1]:6.2f} x {r['size'][2]:6.2f} m")

# ── voxelize each rock ──
all_cols = []       # sampled colors for global quantization
rock_voxels = []    # per rock: dict (x,y,z)->color index into all_cols

for r in rocks:
    wp, uv, idx, tex = r['wp'], r['uv'], r['idx'], r['tex']
    mn = r['mn']
    # grid dims (pad 1)
    dims = np.ceil((r['mx'] - mn) / VOX).astype(int) + 1
    surf = {}
    tris = idx.reshape(-1, 3)
    for t in tris:
        p0, p1, p2 = wp[t[0]], wp[t[1]], wp[t[2]]
        u0 = uv[t[0]] if uv is not None else None
        u1 = uv[t[1]] if uv is not None else None
        u2 = uv[t[2]] if uv is not None else None
        # subdivide by longest edge
        e = max(np.linalg.norm(p1 - p0), np.linalg.norm(p2 - p0), np.linalg.norm(p2 - p1))
        nsub = max(1, int(math.ceil(e / (VOX * 0.5))))
        for i in range(nsub + 1):
            for j in range(nsub + 1 - i):
                a = i / nsub; b = j / nsub; c = 1 - a - b
                p = p0 * a + p1 * b + p2 * c
                v = tuple(int(q) for q in ((p - mn) / VOX))
                if v not in surf:
                    if tex is not None and uv is not None:
                        ut = u0 * a + u1 * b + u2 * c
                        h, w2 = tex.shape[:2]
                        px = int(ut[0] % 1.0 * (w2 - 1)); py = int(ut[1] % 1.0 * (h - 1))
                        col = tex[py, px].astype(float) * np.array(r['bcf'][:3])
                    else:
                        col = np.array(r['bcf'][:3]) * 255.0
                    surf[v] = col
    # smooth surface colors — 3 passes of a radius-1 box blur over the surface shell.
    # Raw point-sampled texels put near-white speckles beside deep shadow texels; after the
    # 12-color quantization that reads as a very light voxel touching a very dark one.
    for _ in range(3):
        sm = {}
        for v, c in surf.items():
            acc = c.copy(); n = 1
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        if dx == 0 and dy == 0 and dz == 0:
                            continue
                        nb = surf.get((v[0] + dx, v[1] + dy, v[2] + dz))
                        if nb is not None:
                            acc = acc + nb; n += 1
            sm[v] = acc / n
        surf = sm
    # solid fill: exterior flood on padded grid
    g = np.zeros(dims + 2, dtype=np.uint8)  # 0 unknown, 1 surface, 2 outside
    for (x, y, z) in surf:
        g[x + 1, y + 1, z + 1] = 1
    from collections import deque
    q = deque()
    q.append((0, 0, 0)); g[0, 0, 0] = 2
    D = dims + 2
    while q:
        x, y, z = q.popleft()
        for dx, dy, dz in ((1,0,0),(-1,0,0),(0,1,0),(0,-1,0),(0,0,1),(0,0,-1)):
            nx, ny, nz = x + dx, y + dy, z + dz
            if 0 <= nx < D[0] and 0 <= ny < D[1] and 0 <= nz < D[2] and g[nx, ny, nz] == 0:
                g[nx, ny, nz] = 2
                q.append((nx, ny, nz))
    interior = np.argwhere(g == 0) - 1
    avg = np.mean(list(surf.values()), axis=0) * 0.82  # interior slightly darker
    vx = dict(surf)
    for (x, y, z) in interior:
        vx[(int(x), int(y), int(z))] = avg
    for c in vx.values():
        all_cols.append(c)
    rock_voxels.append(vx)
    print(f"  {r['name']:24s} grid {dims} surface {len(surf)} solid {len(vx)}")

# ── global color quantization (median cut to <=12 colors) ──
cols = np.clip(np.array(all_cols), 0, 255)

def median_cut(colors, n):
    boxes = [colors]
    while len(boxes) < n:
        # split the box with the largest spread
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

PALN = 12
pal = median_cut(cols, PALN)
print("palette:", [[int(c) for c in p] for p in pal])

def nearest(c):
    return int(((pal - c) ** 2).sum(1).argmin())

out_rocks = []
for r, vx in zip(rocks, rock_voxels):
    xs = [k[0] for k in vx]; ys = [k[1] for k in vx]; zs = [k[2] for k in vx]
    x0, y0, z0 = int(min(xs)), int(min(ys)), int(min(zs))
    sx = int(max(xs)) - x0 + 1; sy = int(max(ys)) - y0 + 1; sz = int(max(zs)) - z0 + 1
    # world Y-up -> model z-up (engine item axes: x width, y depth, z height)
    packed = []
    for (x, y, z), c in vx.items():
        mx = x - x0; my = z - z0; mz = y - y0   # world z -> depth, world y -> height
        packed.append((mx | (my << 8) | (mz << 16) | (nearest(c) << 24)))
    out_rocks.append(dict(name=r['name'].split('_' + r['name'].split('_')[-2])[0] if False else r['name'],
                          grp=r['grp'], sx=sx, sy=sz, sz=sy, vox=sorted(packed)))
    # note: sy (depth) = world-z extent, sz (height) = world-y extent

out = dict(pal=[[int(round(c)) for c in p] for p in pal],
           rocks=sorted(out_rocks, key=lambda rr: rr['sx'] * rr['sy'] * rr['sz']))
s = json.dumps(out, separators=(',', ':'))
open(OUT, 'w').write(s)
print(f"wrote {OUT} ({len(s)/1024:.0f} KB), rocks {len(out_rocks)}")
for rr in out['rocks']:
    print(f"  {rr['name']:24s} {rr['sx']}x{rr['sy']}x{rr['sz']} vox {len(rr['vox'])}")
