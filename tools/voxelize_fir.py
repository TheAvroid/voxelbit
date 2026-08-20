"""Voxelize source/fbx/source/fir_spruce.fbx to 10 cm voxels and write a MagicaVoxel .vox.

Input:  source/fbx/source/fir_spruce.fbx        (authoring original - gitignored, never shipped)
        source/fbx/textures/wgvqdap_8K_Albedo.jpg   the BARK albedo
        source/fbx/textures/qgpuG2_Preview.png      the BRANCH card, RGBA with a cutout alpha
Output: game/assets/decoration/fir_spruce.vox  (the asset-editor stage loads .vox, see below)

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH - numpy and Pillow live there:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_fir.py

── DOES .FBX WORK? YES, BUT NOT FROM PYTHON. ────────────────────────────────────────────────
This tool exists to answer that question, so the answer is written down here rather than left
implicit in the code. NOTHING INSTALLED CAN READ AN .FBX FROM PYTHON: trimesh 4.12 is present
and its loader table is 3dxml/3mf/ctm/dae/glb/gltf/obj/off/ply/step/stl/xaml/xyz/zae - no fbx.
pyassimp, pygltflib and aspose.threed are all absent, and FBX is a closed binary format with a
deflate-compressed node tree, so there is no "just parse it" fallback the way .glb has one
(voxelize_rocks.py parses GLB with struct + json and no library at all).

WHAT DOES READ IT is Blender, which is installed at 4.4.1 and whose FBX importer is the
reference implementation. So step 1 below shells out to Blender headless and converts the FBX
to an .obj next to it; everything after that is the SAME pipeline the .glb voxelizers use.
The conversion is cached: delete source/fbx/fir_spruce.obj to force it again.

That means the honest answer for the next asset is: an .fbx is usable, at the cost of one
Blender round-trip, and it costs nothing at runtime because what ships is a .vox either way.
The one thing the round-trip does NOT carry is the textures - see below.

── THE TEXTURES ARE NOT IN THE FBX, AND ONE OF THEM IS MISSING ─────────────────────────────
The FBX names two materials, Bark and Branches, and references its maps by a path that does
not exist on this machine, so Blender imports both materials with NO image at all (bpy.data.
images is empty after the import). The maps have to be paired up by hand, from what is in
source/fbx/textures:
  * wgvqdap_8K_Albedo.jpg  is the bark: a brown, vertically-scratched trunk sheet. Bark UVs
    tile (v spans -0.48 .. 2.66), which is what a trunk sheet does, so it is sampled wrapped.
  * qgpuG2_Preview.png     is the branch card - a single spruce spray on transparent
    background at 360x360. It is called a "preview" but it IS the Branches albedo: the
    Branches UVs are a clean 0..1 square, the file is square, and its qgpuG prefix pairs it
    with qgpuG_4K_Normal.jpg. The 4K ALBEDO for the branches was simply not in the download -
    only its normal map was. 360 px is well past enough here: a branch card is ~2 m across,
    which is 20 voxels, so the sampler is throwing away 18 texels per voxel as it is.
The 8K bark sheet is reduced 4x on load for the same reason - the trunk is four voxels wide.

── PALETTE ── MEASURED, NOT ASSUMED: __vb.palAudit() in the running game reports len 254 of 256
with TWO ids free, so there is no budget here at all. Baking the fir's own sampled shades cost
2 mints and 3 SUBSTITUTIONS (edSubs 3, max-channel error 20/16/12) - i.e. three of its colours
came out as whatever the nearest surviving id happened to be, which is the documented price of
a full table and not a price worth paying for a stage exhibit.

So the default repoints the shades onto pine5.vox's OWN bark and needle colours, by relative
lightness, exactly as voxelize_oaks.py repoints its oak bark onto the pine's. Cost: ZERO new
ids, ZERO substitutions - edCol (ui/editor.js) takes an exact palette match before it mints
anything, and pine5's colours are already in the table because the pine forest is made of them.
What the TEXTURE still decides is which voxel gets which shade: the albedo drives the light/dark
structure and the ramp underneath it is the pine's. `--own-colours` bakes the sampled shades
instead, for anyone who wants to spend the ids and see the fir's true greys and olives.

── AXES ── model x = width, y = depth, z = height, the same convention voxelize_rocks.py and
gen_birch.py write. Blender is Z-up natively and the OBJ is exported in Blender's own axes, so
the FBX importer's Y-up -> Z-up conversion is the only axis change and Blender does it.
"""
import math, os, struct, subprocess, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FBX = os.path.join(ROOT, 'source', 'fbx', 'source', 'fir_spruce.fbx')
OBJ = os.path.join(ROOT, 'source', 'fbx', 'fir_spruce.obj')      # Blender's conversion, cached
TEXD = os.path.join(ROOT, 'source', 'fbx', 'textures')
BARK_TEX = os.path.join(TEXD, 'wgvqdap_8K_Albedo.jpg')
NEEDLE_TEX = os.path.join(TEXD, 'qgpuG2_Preview.png')
PINE5 = os.path.join(ROOT, 'game', 'assets', 'foilage', 'pine5.vox')
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'fir_spruce.vox')

VOX = 0.10         # metres per voxel - THE 10 cm grid every asset in this game is on
# ── HOW TALL ── the source measures 29.9 units and nothing in the file says what a unit is. 11.6 m
# is not a guess about firs, it is pine5.vox's own height (116 voxels): the user's question is how
# this .fbx compares to the .glb pine already in the forest, and the comparison is only readable if
# both trees are the same height. It also happens to be the engine's canopy envelope - see the long
# note on TALLEST in voxelize_oaks.py before raising it.
TALL_M = 11.6
NBARK = 4          # bark shades…
NEEDLE = 5         # …and needle shades. 9 total, the birch this replaces on the stage cost 10
ALPHA_MIN = 110    # branch-card texels below this alpha are hole, not needle
OWN_COLOURS = False  # True = bake the fir's sampled shades and pay the ids; see the header
BARK_REDUCE = 4    # 8192 -> 2048; the trunk is 4 voxels wide, see the header
SUB = 0.5          # triangle sample spacing, in voxels

for _a in sys.argv[1:]:
    if _a.startswith('--tall='):
        TALL_M = float(_a[7:])
    elif _a.startswith('--nbark='):
        NBARK = int(_a[8:])
    elif _a.startswith('--needle='):
        NEEDLE = int(_a[9:])
    elif _a.startswith('--alpha='):
        ALPHA_MIN = int(_a[8:])
    elif _a == '--own-colours':
        OWN_COLOURS = True
    elif _a.startswith('--out='):
        _o = _a[6:]
        OUT = _o if os.path.isabs(_o) else os.path.join(ROOT, _o)


# ── STEP 1: FBX -> OBJ VIA BLENDER ──────────────────────────────────────────────────────────
def find_blender():
    env = os.environ.get('BLENDER')
    if env and os.path.exists(env):
        return env
    pf = os.environ.get('ProgramFiles', r'C:\Program Files')
    root = os.path.join(pf, 'Blender Foundation')
    if os.path.isdir(root):
        for d in sorted(os.listdir(root), reverse=True):        # newest version first
            exe = os.path.join(root, d, 'blender.exe')
            if os.path.exists(exe):
                return exe
    return None


CONVERT = r'''
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=r"%s")
for o in bpy.data.objects:
    o.select_set(o.type == "MESH")
bpy.ops.wm.obj_export(filepath=r"%s", export_selected_objects=True, apply_modifiers=True,
                      export_triangulated_mesh=True, export_uv=True, export_normals=False,
                      export_materials=True, forward_axis="Y", up_axis="Z")
'''


def to_obj():
    if os.path.exists(OBJ) and os.path.getmtime(OBJ) >= os.path.getmtime(FBX):
        print('obj cache hit: %s' % os.path.relpath(OBJ, ROOT))
        return
    blender = find_blender()
    if not blender:
        sys.exit('no Blender found - set $BLENDER to blender.exe. Nothing else installed reads .fbx '
                 '(see the header); trimesh has no fbx loader and pyassimp is not installed.')
    tmp = os.path.join(os.path.dirname(OBJ), '_fbx2obj.py')
    open(tmp, 'w').write(CONVERT % (FBX, OBJ))
    print('converting fbx -> obj with %s' % blender)
    r = subprocess.run([blender, '--background', '--factory-startup', '--python', tmp],
                       capture_output=True, text=True)
    os.remove(tmp)
    if not os.path.exists(OBJ):
        sys.exit('blender conversion failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


# ── STEP 2: PARSE THE OBJ ───────────────────────────────────────────────────────────────────
def load_obj(path):
    """-> {material: (tri_positions Nx3x3, tri_uvs Nx3x2)}. Plain text, no library needed."""
    V, VT, tris = [], [], {}
    cur = 'none'
    for line in open(path, 'r', errors='replace'):
        if line.startswith('v '):
            V.append([float(x) for x in line.split()[1:4]])
        elif line.startswith('vt '):
            VT.append([float(x) for x in line.split()[1:3]])
        elif line.startswith('usemtl '):
            cur = line.split(None, 1)[1].strip()
        elif line.startswith('f '):
            f = line.split()[1:]
            vi, ti = [], []
            for tok in f:
                p = tok.split('/')
                vi.append(int(p[0]) - 1)
                ti.append(int(p[1]) - 1 if len(p) > 1 and p[1] else -1)
            for k in range(1, len(vi) - 1):                       # fan-triangulate, though the export is already tris
                tris.setdefault(cur, []).append((vi[0], vi[k], vi[k + 1], ti[0], ti[k], ti[k + 1]))
    V = np.array(V, np.float64)
    VT = np.array(VT, np.float64) if VT else np.zeros((1, 2))
    out = {}
    for m, t in tris.items():
        t = np.array(t, np.int64)
        out[m] = (V[t[:, :3]], VT[np.maximum(t[:, 3:], 0)])
    return out


# ── STEP 3: RASTERIZE ───────────────────────────────────────────────────────────────────────
def raster(pos, uv, tex, org, alpha_min):
    """Every triangle sampled at SUB-voxel spacing -> (voxel coords, RGB), one row per SAMPLE.
    Same barycentric-fan scheme voxelize_oaks.py uses, bucketed by subdivision count so each
    bucket is a single numpy op."""
    P, C = [], []
    wp = (pos * SCALE - org) / VOX                                # straight into voxel space
    A, B, Cc = wp[:, 0], wp[:, 1], wp[:, 2]
    e = np.maximum.reduce([np.linalg.norm(B - A, axis=1), np.linalg.norm(Cc - A, axis=1),
                           np.linalg.norm(Cc - B, axis=1)])
    nsub = np.clip(np.ceil(e / SUB), 1, 128).astype(int)
    UA, UB, UC = uv[:, 0], uv[:, 1], uv[:, 2]
    th, tw = tex.shape[:2]
    for ns in np.unique(nsub):
        sel = nsub == ns
        ij = np.array([(i, j) for i in range(ns + 1) for j in range(ns + 1 - i)], np.float64)
        a, b = ij[:, 0] / ns, ij[:, 1] / ns
        c = 1.0 - a - b
        pts = (A[sel][:, None, :] * a[None, :, None] + B[sel][:, None, :] * b[None, :, None]
               + Cc[sel][:, None, :] * c[None, :, None]).reshape(-1, 3)
        ut = (UA[sel][:, None, :] * a[None, :, None] + UB[sel][:, None, :] * b[None, :, None]
              + UC[sel][:, None, :] * c[None, :, None]).reshape(-1, 2)
        # OBJ v is bottom-up, images are top-down; wrap first so tiling bark keeps working
        px = tex[((1.0 - ut[:, 1]) % 1.0 * (th - 1)).astype(int), (ut[:, 0] % 1.0 * (tw - 1)).astype(int)]
        if alpha_min > 0:
            keep = px[:, 3] >= alpha_min                          # THE ALPHA TEST - a branch card is mostly hole
            pts, px = pts[keep], px[keep]
        if len(pts):
            P.append(np.floor(pts).astype(np.int32))
            C.append(px[:, :3].astype(np.float64))
    if not P:
        return np.zeros((0, 3), np.int32), np.zeros((0, 3))
    return np.concatenate(P), np.concatenate(C)


def dedupe(vx, col, dims):
    """sample list -> unique voxels with their MEAN colour (vectorised; see voxelize_oaks.py)."""
    lin = (vx[:, 0].astype(np.int64) * dims[1] + vx[:, 1]) * dims[2] + vx[:, 2]
    u, inv = np.unique(lin, return_inverse=True)
    n = np.bincount(inv).astype(np.float64)
    mean = np.stack([np.bincount(inv, weights=col[:, k]) / n for k in range(3)], axis=1)
    z = u % dims[2]
    y = (u // dims[2]) % dims[1]
    x = u // (dims[2] * dims[1])
    return np.stack([x, y, z], axis=1).astype(np.int32), mean


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


def pine5_palette():
    """pine5.vox's colours, split into needles and bark by hue. These are what the running game
    already holds ids for, so anything snapped onto one is free."""
    d = open(PINE5, 'rb').read()
    i, pal, used = 8, None, set()
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid == b'MAIN':
            continue
        if cid == b'RGBA':
            pal = d[i:i + 1024]
        if cid == b'XYZI':
            n = struct.unpack_from('<I', d, i)[0]
            used.update(d[i + 4 + k * 4 + 3] for k in range(n))
        i += cs
    green, brown = [], []
    for ci in sorted(used):
        o = (ci - 1) * 4
        r, g, b = pal[o], pal[o + 1], pal[o + 2]
        (green if g > r else brown).append((r, g, b))
    return np.array(brown, float), np.array(green, float)


LUM = np.array([0.2126, 0.7152, 0.0722])


def repoint(shades, ref):
    """The fir's N quantized shades -> N of pine5's, matched by RELATIVE LIGHTNESS: darkest to
    darkest, lightest to lightest, the rest spread evenly between. This is voxelize_oaks.py's
    trick for oak bark and it is here for the same reason - the shared 256-entry table has two
    ids free, and an exact match costs edCol nothing at all.

    RELATIVE, not absolute: the fir's bark spans a much narrower range than pine5's, so matching
    absolute luminance would collapse all four onto the same brown and flatten the trunk. Spread
    across the reference ramp instead and the texture's own light/dark structure survives."""
    if not len(ref):
        return np.clip(np.round(shades), 0, 255).astype(int)
    ref = ref[np.argsort(ref @ LUM)]
    order = np.argsort(np.asarray(shades) @ LUM)
    out = np.zeros((len(shades), 3), int)
    n = max(1, len(shades) - 1)
    for rank, i in enumerate(order):
        out[i] = ref[int(round(rank / n * (len(ref) - 1)))]
    return out


def write_vox(path, grid, cidx, pal):
    """grid: (x,y,z) uint8 material, cidx: same shape 1-based palette index. MagicaVoxel 150."""
    xs, ys, zs = np.nonzero(grid)
    sx, sy, sz = grid.shape
    data = bytearray()
    for x, y, z in zip(xs, ys, zs):
        data += bytes((x, y, z, int(cidx[x, y, z])))
    rgba = bytearray(1024)
    for i, c in enumerate(pal):
        o = i * 4
        rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3] = int(c[0]), int(c[1]), int(c[2]), 255

    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    body = (chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
            + chunk(b'XYZI', struct.pack('<I', len(xs)) + bytes(data))
            + chunk(b'RGBA', bytes(rgba)))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))
    return sx, sy, sz, len(xs)


to_obj()
mats = load_obj(OBJ)
print('obj: ' + ', '.join('%s %d tris' % (k, len(v[0])) for k, v in sorted(mats.items())))
if 'Bark' not in mats or 'Branches' not in mats:
    sys.exit('expected Bark + Branches materials, got %s' % sorted(mats))

allv = np.concatenate([v[0].reshape(-1, 3) for v in mats.values()])
tall = allv[:, 2].max() - allv[:, 2].min()
SCALE = TALL_M / tall
org = allv.min(0) * SCALE
dims = np.maximum(1, np.ceil((allv.max(0) * SCALE - org) / VOX).astype(int) + 1)
print('source %.2f x %.2f x %.2f units -> %.1f m tall (scale %.5f) -> grid %d x %d x %d'
      % (*(allv.max(0) - allv.min(0)), TALL_M, SCALE, *dims))

bark_tex = Image.open(BARK_TEX).convert('RGB')
bark_tex = np.asarray(bark_tex.reduce(BARK_REDUCE).convert('RGBA'))
needle_tex = np.asarray(Image.open(NEEDLE_TEX).convert('RGBA'))
print('bark tex %s, branch tex %s' % (bark_tex.shape[:2], needle_tex.shape[:2]))

bvx, bcol = raster(*mats['Bark'], bark_tex, org, 0)               # trunk: opaque, no alpha test
nvx, ncol = raster(*mats['Branches'], needle_tex, org, ALPHA_MIN)
bvx = np.clip(bvx, 0, dims - 1)
nvx = np.clip(nvx, 0, dims - 1)
bvx, bcol = dedupe(bvx, bcol, dims)
nvx, ncol = dedupe(nvx, ncol, dims)

grid = np.zeros(dims, np.uint8)                                  # 0 air, 1 wood, 2 needle
grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = 1
keep = grid[nvx[:, 0], nvx[:, 1], nvx[:, 2]] == 0                # WOOD WINS - a branch card clipping
nvx, ncol = nvx[keep], ncol[keep]                                # through the trunk must not repaint it
grid[nvx[:, 0], nvx[:, 1], nvx[:, 2]] = 2
print('rasterized: %d wood, %d needle' % (len(bvx), len(nvx)))

# ── FILL THE BOLE ── the trunk mesh is an open-ended tube, so flood the outside and call every
# unreached air voxel heartwood. Seal the ground plane first or the flood walks straight up the
# inside of the trunk and the fill never fires (the exact trap voxelize_oaks.py documents). Only
# cavities walled mostly in WOOD are filled: a pocket inside the crown must stay air.
from collections import deque
D = dims + 2
seen = np.zeros(D, bool)
seen[1:-1, 1:-1, 1:-1] = grid != 0
seen[:, :, 0] = True                                             # z is height here - seal the base
q = deque([(0, 0, D[2] - 1)])
seen[0, 0, D[2] - 1] = True
while q:
    x, y, z = q.popleft()
    for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
        nx, ny, nz = x + dx, y + dy, z + dz
        if 0 <= nx < D[0] and 0 <= ny < D[1] and 0 <= nz < D[2] and not seen[nx, ny, nz]:
            seen[nx, ny, nz] = True
            q.append((nx, ny, nz))
inside = (~seen[1:-1, 1:-1, 1:-1]) & (grid == 0)
heart = 0
if inside.any():
    lab = np.zeros(dims, np.int32)
    nl = 0
    for seed in map(tuple, np.argwhere(inside)):
        if lab[seed]:
            continue
        nl += 1
        comp, wall_w, wall_n = [], 0, 0
        st = [seed]
        lab[seed] = nl
        while st:
            v = st.pop()
            comp.append(v)
            for dx, dy, dz in ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)):
                n = (v[0] + dx, v[1] + dy, v[2] + dz)
                if not (0 <= n[0] < dims[0] and 0 <= n[1] < dims[1] and 0 <= n[2] < dims[2]):
                    continue
                if grid[n] == 1:
                    wall_w += 1
                elif grid[n] == 2:
                    wall_n += 1
                elif inside[n] and not lab[n]:
                    lab[n] = nl
                    st.append(n)
        if wall_w >= 0.85 * (wall_w + wall_n):                   # a wooden cavity is the inside of a bole
            for v in comp:
                grid[v] = 1
                heart += 1
print('heartwood filled: %d voxels' % heart)

# ── TRIM TO THE OCCUPIED EXTENT ── the outer texels of a branch card are transparent, so the mesh
# bounds carry a few voxels of empty margin. Slack under the model hangs it in the air on the stage.
occ = np.argwhere(grid != 0)
mn, mx = occ.min(0), occ.max(0)
grid = grid[mn[0]:mx[0] + 1, mn[1]:mx[1] + 1, mn[2]:mx[2] + 1]
bvx -= mn
nvx -= mn
dims = np.array(grid.shape)

# ── QUANTIZE ── two separate cuts. One cut over both would spend every shade on the needles: they
# outnumber the bark several to one and a population-weighted split follows the population.
cut_b = median_cut(np.clip(bcol, 0, 255), NBARK)
cut_n = median_cut(np.clip(ncol, 0, 255), NEEDLE)
ref_b, ref_n = pine5_palette()
if OWN_COLOURS:
    pal_b = np.clip(np.round(cut_b), 0, 255).astype(int)
    pal_n = np.clip(np.round(cut_n), 0, 255).astype(int)
else:
    pal_b, pal_n = repoint(cut_b, ref_b), repoint(cut_n, ref_n)
print('bark   sampled %s\n       ->      %s' % ([[int(round(c)) for c in s] for s in cut_b],
                                                [list(map(int, c)) for c in pal_b]))
print('needle sampled %s\n       ->      %s' % ([[int(round(c)) for c in s] for s in cut_n],
                                                [list(map(int, c)) for c in pal_n]))
print('palette: %s' % ('the fir\'s OWN shades - expect new ids + substitutions on a full table'
                       if OWN_COLOURS else 'repointed onto pine5.vox - costs ZERO new ids'))
pal = np.concatenate([pal_b, pal_n])

# WHICH shade a voxel gets is still decided by the SAMPLED colour against the SAMPLED cut, so the
# texture keeps its light/dark structure even when the ramp underneath it is the pine's.
cidx = np.zeros(dims, np.uint8)
for vx, col, base, shades in ((bvx, bcol, 1, cut_b), (nvx, ncol, 1 + NBARK, cut_n)):
    if not len(vx):
        continue
    d = np.abs(col[:, None, :] - shades[None, :, :].astype(float)).max(2)
    cidx[vx[:, 0], vx[:, 1], vx[:, 2]] = base + d.argmin(1)
cidx[(grid == 1) & (cidx == 0)] = 1 + int((pal_b @ LUM).argmin())   # heartwood -> the darkest bark shade

sx, sy, sz, n = write_vox(OUT, grid, cidx, pal)
print('%s  %d x %d x %d  %d voxels  = %.1f m tall at %d cm per voxel  (%d shades)'
      % (os.path.relpath(OUT, ROOT), sx, sy, sz, n, sz * VOX, int(VOX * 100), len(pal)))
