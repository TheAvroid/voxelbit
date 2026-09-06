"""Re-voxelize the big and mid rocks AT A LARGER WORLD SIZE, from the sculpt.

Reads:  source/glb/rocks.glb                       (the stone_pack sculpt, gitignored)
        game/assets/decoration/rocks26.json        (only for its shared 12-shade palette)
Writes: game/assets/decoration/rocks/<name>.vox    (the 5 big and 6 mid, replaced)

WHY THIS EXISTS -- upscaling is not voxelizing.
------------------------------------------------------------------------------
gpu/world.h used to grow these two classes with upscale2x, which replaces every
voxel with a 2x2x2 block of itself. That makes the model bigger in metres and
leaves the mesh at 10 cm voxels, so it is easy to believe it is the same thing.
It is not. Nearest-neighbour replication cannot invent detail it was not given:
at 2x every surface feature is 20 cm and at 4x it is 40 cm, so a boulder ends up
built out of blocks four times the size of the terrain it is standing on, in an
engine whose entire visual grammar is 10 cm cubes.

The sculpt has the detail. BIG_1 is 50 x 50 x 57 voxels when sampled at 10 cm
because that is how big the mesh IS -- not because that is all the mesh knows.
Sampling the SAME mesh on a grid four times finer, and calling the result four
times bigger, gives a 200 x 200 x 228 rock whose steps are 10 cm and whose shape
is the sculptor's rather than a staircase of a staircase.

So: scale the mesh, voxelize at 10 cm, and drop the upscale in loadRocks.

    class   mesh scale   voxels                 world size
    big         4x       up to 200 x 200 x 228   12.8 - 22.8 m
    mid         2x       up to  86 x  80 x  76    5.6 -  7.6 m
    others      1x       unchanged, not touched

THE PALETTE IS REUSED, NOT REBUILT. The other fifteen rocks are staying exactly
as they are, and a fresh quantization over only eleven models would pick
different shades -- so the big and mid stones would no longer match the small
ones beside them. Colours are sampled from the texture as before and then
snapped to the palette rocks26.json already shipped.

Run with the python that has numpy and Pillow:  py tools/revoxel_rocks_scaled.py
"""
import struct, json, io, math, os, sys, time
from collections import deque
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GLB = os.path.join(ROOT, 'source', 'glb', 'rocks.glb')
PALSRC = os.path.join(ROOT, 'game', 'assets', 'decoration', 'rocks26.json')
OUTDIR = os.path.join(ROOT, 'game', 'assets', 'decoration', 'rocks')
VOX = 0.1  # metres per voxel, always -- the SCALE is applied to the mesh

# Which meshes to rebuild and how much bigger to make them. Names are the glb's
# own mesh names, which are also the .vox filenames.
SCALE = {}
for n in ['BIG_1_BiG_0', 'Big_2_BiG_0', 'Big_3_BiG_0', 'Big_4_BiG_0', 'Big_5_BiG_0']:
    SCALE[n] = 4.0
for n in ['Mid_1_MID_0', 'Mid_2_MID_0', 'Mid_3_MID_0', 'Mid_4_MID_0', 'Mid_5_MID_0']:
    SCALE[n] = 2.0

# Mid_4 appears TWICE in the sculpt and ships as two files -- the second is
# Mid_4_MID_0_001. They are separate nodes with separate transforms, so the
# second occurrence of the name is written to the _001 file.
DUP = {'Mid_4_MID_0': 'Mid_4_MID_0_001'}

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
        rows = [np.frombuffer(BIN, dt, ncomp, off + i * stride) for i in range(a['count'])]
        return np.stack(rows)
    return np.frombuffer(BIN, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)


def img_pixels(ti):
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


texcache = {}
def get_tex(ti):
    if ti not in texcache:
        texcache[ti] = img_pixels(ti)
    return texcache[ti]


# ── the shared palette the other fifteen rocks already use ──────────────────
PAL = np.array(json.load(open(PALSRC))['pal'][:], dtype=np.float64)
print('palette: %d shades reused from rocks26.json' % len(PAL))


def quantize(cols):
    """Nearest palette entry for each RGB row. Returns 0-based indices."""
    d2 = ((cols[:, None, :] - PAL[None, :, :]) ** 2).sum(2)
    return d2.argmin(1)


def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def write_vox(path, sx, sy, sz, voxels):
    """voxels: list of (x, y, z, palette_index0)."""
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    vv = b''.join(struct.pack('<BBBB', x, y, z, ci + 1) for (x, y, z, ci) in voxels)
    xyzi = chunk(b'XYZI', struct.pack('<I', len(voxels)) + vv)
    rgba = b''
    for i in range(256):
        c = PAL[i] if i < len(PAL) else [0, 0, 0]
        rgba += struct.pack('<BBBB', int(c[0]), int(c[1]), int(c[2]), 255)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150)
                           + chunk(b'MAIN', b'', size + xyzi + chunk(b'RGBA', rgba)))


def sample_surface(wp, uv, idx, tex, bcf, mn, dims):
    """Barycentric-sample every triangle densely enough to leave no gaps.

    VECTORIZED PER TRIANGLE, unlike voxelize_rocks.py's nested python loops. At
    4x the surface area is sixteen times what that tool was written for and the
    scalar version takes hours; this takes minutes.
    """
    surf = {}
    tris = idx.reshape(-1, 3)
    for t in tris:
        p0, p1, p2 = wp[t[0]], wp[t[1]], wp[t[2]]
        e = max(np.linalg.norm(p1 - p0), np.linalg.norm(p2 - p0), np.linalg.norm(p2 - p1))
        n = max(1, int(math.ceil(e / (VOX * 0.5))))
        # barycentric lattice: a + b <= 1
        ii, jj = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing='ij')
        keep = (ii + jj) <= n
        a = (ii[keep] / n)[:, None]
        b = (jj[keep] / n)[:, None]
        c = 1.0 - a - b
        pts = p0 * a + p1 * b + p2 * c
        vs = ((pts - mn) / VOX).astype(np.int32)
        if uv is not None and tex is not None:
            u0, u1, u2 = uv[t[0]], uv[t[1]], uv[t[2]]
            ut = u0 * a + u1 * b + u2 * c
            h, w2 = tex.shape[:2]
            px = (np.mod(ut[:, 0], 1.0) * (w2 - 1)).astype(np.int32)
            py = (np.mod(ut[:, 1], 1.0) * (h - 1)).astype(np.int32)
            cols = tex[py, px].astype(np.float64) * np.array(bcf[:3])[None, :]
        else:
            cols = np.repeat((np.array(bcf[:3]) * 255.0)[None, :], len(vs), 0)
        for k in range(len(vs)):
            key = (int(vs[k, 0]), int(vs[k, 1]), int(vs[k, 2]))
            if key not in surf:
                surf[key] = cols[k]
    return surf


def solid_fill(surf, dims):
    """Everything not reachable from outside is inside.

    THE SHELL IS DILATED BEFORE THE FLOOD, and only for the flood. The original
    tool floods against the raw sampled shell, which is fine at the scale it was
    written for and is not fine here: a seam in the sculpt that is narrower than
    one voxel at 1x is three voxels wide at 4x, the exterior pours straight in
    through it, and the rock comes out as a hollow shell. Big_2 did exactly that
    -- 128 125 voxels in a 6.5 M cell grid, 2% full, against 30-50% for its
    neighbours.

    Dilating by one closes those pinholes without fattening the result, because
    the dilation is used ONLY as a barrier: the solid that comes back is the
    interior plus the ORIGINAL shell, so the extra ring is thrown away again."""
    g = np.zeros(tuple(dims + 2), dtype=np.uint8)   # 0 unknown, 1 barrier, 2 outside
    shell = np.zeros(tuple(dims + 2), dtype=bool)
    for (x, y, z) in surf:
        shell[x + 1, y + 1, z + 1] = True
    blocked = shell.copy()
    for ax in (0, 1, 2):
        blocked |= np.roll(shell, 1, axis=ax)
        blocked |= np.roll(shell, -1, axis=ax)
    g[blocked] = 1
    D = dims + 2
    q = deque([(0, 0, 0)])
    g[0, 0, 0] = 2
    while q:
        x, y, z = q.popleft()
        for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
            nx, ny, nz = x + dx, y + dy, z + dz
            if 0 <= nx < D[0] and 0 <= ny < D[1] and 0 <= nz < D[2] and g[nx, ny, nz] == 0:
                g[nx, ny, nz] = 2
                q.append((nx, ny, nz))
    return g


seen = {}
built = 0
for ni, n in enumerate(js['nodes']):
    if n.get('mesh') is None:
        continue
    mesh = js['meshes'][n['mesh']]
    name = mesh.get('name', '')
    if name not in SCALE:
        continue
    # the duplicated Mid_4 writes to the _001 file on its second appearance
    out_name = name
    seen[name] = seen.get(name, 0) + 1
    if seen[name] > 1:
        if name not in DUP:
            continue
        out_name = DUP[name]

    t0 = time.time()
    pr = mesh['primitives'][0]
    pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
    uv = acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64) \
        if 'TEXCOORD_0' in pr['attributes'] else None
    idx = acc_data(pr['indices']).astype(np.int64).ravel() if 'indices' in pr \
        else np.arange(len(pos))
    mat = js['materials'][pr['material']]
    pbr = mat.get('pbrMetallicRoughness', {})
    bcf = pbr.get('baseColorFactor', [1, 1, 1, 1])
    bct = pbr.get('baseColorTexture')
    tex = get_tex(bct['index']) if bct is not None else None

    M = world_mat(ni)
    wp0 = (M[:3, :3] @ pos.T).T + M[:3, 3]

    # THE SCALE IS CLAMPED TO WHAT THE FORMAT CAN HOLD. A .vox XYZI record packs
    # x, y and z as single BYTES, so no axis may exceed 255 -- that is a hard
    # limit of the file format, not of this tool or of the engine. Big_2 wants
    # 297 voxels across at a flat 4x and Big_5 wants 269, so those two take as
    # much scale as fits and the rest take the full amount. They were already
    # different sizes; this makes the largest two slightly less different.
    want = SCALE[name]
    ext = wp0.max(0) - wp0.min(0)
    need = np.ceil(ext * want / VOX).astype(int) + 1
    if need.max() > 255:
        want = want * (254.0 / float(need.max()))
    wp = wp0 * want

    mn = wp.min(0)
    dims = np.ceil((wp.max(0) - mn) / VOX).astype(int) + 1

    surf = sample_surface(wp, uv, idx, tex, bcf, mn, dims)
    # the same 3-pass shell blur the original does, for the same reason: raw
    # texels quantize into white speckle beside black speckle otherwise.
    for _ in range(3):
        sm = {}
        for v, c in surf.items():
            acc = c.copy(); cnt = 1
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    for dz in (-1, 0, 1):
                        if dx or dy or dz:
                            nb = surf.get((v[0] + dx, v[1] + dy, v[2] + dz))
                            if nb is not None:
                                acc = acc + nb; cnt += 1
            sm[v] = acc / cnt
        surf = sm

    g = solid_fill(surf, dims)

    keys = list(surf.keys())
    cols = np.array([surf[k] for k in keys])
    qi = quantize(cols)
    colof = {k: int(qi[i]) for i, k in enumerate(keys)}
    # interior takes the darkest shade in use on this rock's shell, which is
    # what the surface blur would have converged to anyway.
    inner = int(np.bincount(qi).argmax())

    # WORLD Y-UP -> MODEL Z-UP. glTF measures height in y; the engine's model
    # axes are x width, y depth, z height, so world z becomes depth and world y
    # becomes height. voxelize_rocks.py does exactly this on its way out and a
    # rock built without it lies on its side.
    voxels = []
    for x in range(dims[0]):
        for y in range(dims[1]):
            for z in range(dims[2]):
                # solid = interior (never reached by the flood) plus the
                # real shell. The dilation ring is neither, and is dropped.
                if g[x + 1, y + 1, z + 1] == 2:
                    continue
                if g[x + 1, y + 1, z + 1] == 1 and (x, y, z) not in colof:
                    continue
                voxels.append((x, z, y, colof.get((x, y, z), inner)))

    msx, msy, msz = int(dims[0]), int(dims[2]), int(dims[1])
    write_vox(os.path.join(OUTDIR, out_name + '.vox'), msx, msy, msz, voxels)
    built += 1
    fill = 100.0 * len(voxels) / float(msx * msy * msz)
    print('  %-22s %3d x %3d x %3d  (%.1f m tall)  %8d voxels  %4.1f%% full  scale %.2fx  %5.1fs'
          % (out_name, msx, msy, msz, msz * VOX, len(voxels), fill, want, time.time() - t0))
    if fill < 8.0:
        print('     !! suspiciously hollow -- the flood may still be leaking')
    sys.stdout.flush()

print('rebuilt %d rocks' % built)
