"""Voxelize source/fbx/Birch_forest_24.8m.Fbx to 10 cm voxels and write a MagicaVoxel .vox.

Input:  source/fbx/Birch_forest_24.8m.Fbx        (unrar'd from Birch_forest_24.8mFbx.rar - gitignored)
        source/fbx/textures/birch/*.jpg          (unrar'd from Downloads/Maps.rar - see THE TEXTURES)
Output: game/assets/decoration/birch_forest.vox  (drag onto the page with the asset editor open, or
                                                  __vb.edLoad('assets/decoration/birch_forest.vox'))

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH - numpy and Pillow live there:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_birch.py

This is voxelize_fir.py's pipeline (Blender fbx->obj, then rasterize triangles into voxels and
quantize the sampled colours) with three differences the source forced, each written up below.

-- DIFFERENCE 1: EIGHT OBJECTS, NOT TWO MATERIALS -------------------------------------------
The fir splits on MATERIAL NAME - it names its two materials Bark and Branches. This file cannot:
it is a 3ds Max V-Ray export and every one of its eleven materials is called "Material #4xx".
The OBJECTS, though, are named after the very textures they wear, and each one uses exactly one
material (measured: 8 meshes, 8 distinct (object, material) pairs). So the split is by OBJECT,
and OBJ_TEX below is that measured table. Four are wood, four are leaf cards.

-- DIFFERENCE 2: THE OPACITY IS A SEPARATE FILE ----------------------------------------------
The fir's branch card is an RGBA .png and the alpha test reads its alpha channel. Birch leaves
ship as V-Ray does them: an opaque *_diff.jpg plus a greyscale *_opac.jpg, both 600x800, white =
leaf. tex_of() stacks the pair into the RGBA the rasterizer already knows how to alpha-test.
Note the diff/opac PAIRING is by FAMILY, not by filename: the _0101/_0202/_0301/_0401 diffs are
2020 recolours of the 2018 _01/_02/_03/_04 sheets and share their masks - same UV layout, and
the mask files simply have no recoloured twin.

-- THE TEXTURES ARE IN A SECOND ARCHIVE ------------------------------------------------------
The FBX names 27 maps by an absolute 3ds Max path that does not exist here, so Blender imports
the materials with no image wired at all (measured: every material's node tree has zero TEX_IMAGE
nodes, though bpy.data.images does hold the 27 names). The maps are in Downloads/Maps.rar, which
is the whole vendor library for the forest set; extract just this tree's into textures/birch:
  cd source/fbx/textures/birch && unrar e /path/to/Maps.rar "Maps\\Betula_*" "Maps\\Birch_*" .
Missing textures are not fatal - MISS below falls back to a flat colour and says so - because a
map absent from the library should degrade one object, not fail the bake.

-- DIFFERENCE 3: THE PALETTE - THIS ONE BAKES ITS OWN COLOURS --------------------------------
voxelize_fir.py defaults to REPOINTING its shades onto pine5.vox's, because the 256-entry table
is full and a fir is green like a pine, so the repoint is nearly free and nearly invisible.
A BIRCH CANNOT DO THAT. Its trunk is WHITE, and white is the species: repointed onto pine5's
bark the tree comes out brown and reads as a dead pine. gen_birch.py's header makes the same
point from the other side - "vertical marks would read as pine" - and the bark colour is the
stronger cue of the two.

It is affordable because THIS IS AN EDITOR ASSET and the editor import path is not edCol. It is
edColExact (ui/editor.js, the `exact` argument edImportBufs passes and every world pose builder
leaves unset), which BORROWS an inert palette id per colour and hands it back at edExit. So the
ten shades below cost the world table nothing permanent. Check what it actually spent with
__vb.palAudit() while the model is staged: edSubs > 0 means the borrow ran out and some shades
were substituted, and the fix is a lower --nbark/--nleaf rather than a different ramp.
`--repoint` bakes the fir's zero-id behaviour instead, for anyone putting this tree in the WORLD,
where edCol is what runs and white bark is not on offer.

-- HEIGHT: 19.9 m, THE TALLEST THE STAGE RELIABLY HOLDS, THOUGH THE SOURCE IS 24.6 ----------
The mesh really is 24.6 m tall - it is sold as a 24.8 m tree and it measures 24.60 - which at
10 cm per voxel is a 246-voxel model. THAT DOES NOT FIT, and the ceiling is arithmetic rather
than a tuning choice, so here it is once:

    a model is stamped from the stage plane + 1, and edLayout drops any voxel at y >= WY - 1,
    so it occupies ED.y + 1 .. 382 and its height is capped at 382 - ED.y.

    ED.y can never go below (WL + 8) & ~7 = 160        ->  ABSOLUTE ceiling  = 222 voxels (22.2 m, 73 ft)
    but it follows the GROUND, not the waterline       ->  measured room 199-215 over 14 worlds
                                                      ->  RELIABLE ceiling  = 199 voxels (19.9 m, 65 ft)

Why the plane follows the ground rather than dropping to 160 and cutting through the hillside: it
costs 2,230,000 earth voxels to carve instead of 152,000 of vegetation, which is past what gpuPatch
moves in a frame - the stage streamed in over seconds with the world showing through, and one run
took the browser down. The full note is in edEnter (ui/editor.js).

199 is the floor of 14 measured worlds, so the tree keeps its crown on all of them. A true-scale
birch is 246 and overshoots even the absolute ceiling by 24. It does not fit
the WORLD either, never mind the stage: ground runs 176-240, so a 246-voxel trunk tops out at
422-486 against a 384 ceiling. Only a taller WY could change that, and WY is what core/gpu.js
trades against the window width (2048² x 384 = a 1.5 GB storage buffer), so it is a whole-game
decision about view distance and weak-GPU support, not a tree decision. User picked 73 ft over
paying that on 2026-08-23.

WHAT MAKES EVEN 199 REACHABLE is the stage carve in ui/editor.js: the platform clears every
solid voxel in its own column, so it sits on the GROUND instead of above the CANOPY. Before that
the room measured 95-215 voxels depending on what the world happened to grow there, and it is now a
flat 222 on every world.
Do not raise TALL_M past 199 expecting the stage to cope: on a world that only affords 199 it will
silently lose the crown, because edLayout drops any voxel at or above WY - 1.

`--tall=24.6` still bakes the true-scale tree, for a future world that can hold it.

-- DIFFERENCE 4: A SUB-VOXEL ALPHA TEST NEEDS ITS OWN SAMPLE RATE ----------------------------
This one cost a bake, so it is written down. voxelize_fir.py picks a triangle's sample count from
its size IN VOXELS - nsub = ceil(edge / SUB) - which is the right question for "which voxels does
this triangle touch" and the WRONG question for "is this texel a hole". At 11.6 m the birch's leaf
triangles measure 0.34 voxels across (median; p90 0.42), so every one of them got nsub = 1, which
in a barycentric fan means the three VERTICES and nothing else.

A leaf card maps its corners to the corners of a 600x800 sheet, and a leaf sits in the MIDDLE of
that sheet with transparent background around it. Measured on the three big leaf objects: alpha
>= 128 at the triangle corners 0.0%, at the centroids 40-90%. So the alpha test threw away the
entire canopy - the first bake came out "9125 wood, 0 leaf", a bare white skeleton, and the only
symptom downstream was median_cut crashing on an empty colour array.

MINSUB is the floor that fixes it: an alpha-tested triangle is always sampled at least 6 steps
(28 points) across its own UV span, however small it is in voxels. Every one of those points lands
in the same 0.34-voxel footprint, which is the correct answer for geometry finer than the grid -
the fraction of them that survive the alpha test IS the card's coverage, and dedupe() averages the
ones that do into that voxel's colour. The fir never needed this because its branch card is ~2 m
across, i.e. 20 voxels, so nsub was already ~40.

CHUNK is the other half of it: 358k triangles x 28 points is 10M samples in one numpy expression
and about 600 MB of temporaries, so raster() batches the triangles rather than allocating that.

-- AXES -- model x = width, y = depth, z = height, as voxelize_fir.py and gen_birch.py write.
Blender is Z-up and the OBJ is exported in Blender's own axes, so the FBX importer's Y-up -> Z-up
conversion is the only axis change and Blender does it.
"""
import os, struct, subprocess, sys
from array import array
import numpy as np
from PIL import Image

Image.MAX_IMAGE_PIXELS = None                      # Birch_trunk_0701_diff is 1729 x 13376 = 23 Mpx, past Pillow's decompression-bomb guard

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FBX = os.path.join(ROOT, 'source', 'fbx', 'Birch_forest_24.8m.Fbx')
OBJ = os.path.join(ROOT, 'source', 'fbx', 'birch_forest.obj')     # Blender's conversion, cached
TEXD = os.path.join(ROOT, 'source', 'fbx', 'textures', 'birch')
PINE5 = os.path.join(ROOT, 'game', 'assets', 'foilage', 'pine5.vox')
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'birch_forest.vox')

# -- THE OBJECT TABLE -- measured from the import, not guessed: every object's name IS its texture's
# name. kind 'w' = wood (opaque, no alpha test), 'l' = leaf card (alpha-tested against its own mask).
# 6_Blend_... is a 3ds Max Blend material layering bark_13 over bark_12 through a mask that the FBX
# does not carry, so it bakes as its base sheet, bark_12; the two differ only in weathering.
# The 4th field is the reduce factor: an 8K trunk sheet on a 4-voxel-wide trunk is 2000 texels/voxel.
OBJ_TEX = {
    '6_Birch_trunk_0701':                         ('w', 'Birch_trunk_0701_diff.jpg', None, 4),  # the bole, 21.9 m of it
    '6_Birch_bark006':                            ('w', 'Birch_bark05_diff.jpg', None, 1),      # the branch network - 336k tris, the biggest object in the file
    '6_Blend_Betula_bark_13_Betula_bark_12_Mask': ('w', 'Betula_bark_12_diff.jpg', None, 1),    # limbs
    '6_Birch_trunk_03':                           ('w', 'Birch_trunk_03.jpg', None, 4),         # a 2.7 m stub at the base
    '6_Betula_leaf_0101':                         ('l', 'Betula_leaf_0101_diff.jpg', 'Betula_leaf_01_opac.jpg', 1),
    '6_Betula_leaf_0202':                         ('l', 'Betula_leaf_0202_diff.jpg', 'Betula_leaf_02_opac.jpg', 1),
    '6_Betula_leaf_0301':                         ('l', 'Betula_leaf_0301_diff.jpg', 'Betula_leaf_03_opac.jpg', 1),
    '6_Betula_leaf_0401':                         ('l', 'Betula_leaf_0401_diff.jpg', 'Betula_leaf_04_opac.jpg', 1),
}
MISS = {'w': (208, 205, 196), 'l': (120, 158, 74)}   # flat stand-in when a map is not on disk, so one absent file costs one object rather than the bake

VOX = 0.10          # metres per voxel - THE 10 cm grid every asset in this game is on
TALL_M = 19.9       # 199 voxels = the tallest model this world can hold. See the HEIGHT note - it is a ceiling, not a taste
NBARK = 5           # bark shades...
NLEAF = 5           # ...and leaf shades. 10 total, the same spend gen_birch.py's authored birch makes
ALPHA_MIN = 128     # opac texels below this are hole, not leaf. The masks are hard-edged, so the exact value barely matters
OWN_COLOURS = True  # see DIFFERENCE 3 - white bark is the species. --repoint for the fir's zero-id behaviour
SUB = 0.5           # triangle sample spacing, in voxels
MINSUB = 6          # ...but never fewer than this many steps on an ALPHA-TESTED triangle. See DIFFERENCE 4
CHUNK = 40000       # triangles per rasterizer batch, so MINSUB's sample count cannot blow the heap

for _a in sys.argv[1:]:
    if _a.startswith('--tall='):
        TALL_M = float(_a[7:])
    elif _a.startswith('--nbark='):
        NBARK = int(_a[8:])
    elif _a.startswith('--nleaf='):
        NLEAF = int(_a[8:])
    elif _a.startswith('--alpha='):
        ALPHA_MIN = int(_a[8:])
    elif _a == '--repoint':
        OWN_COLOURS = False
    elif _a.startswith('--out='):
        _o = _a[6:]
        OUT = _o if os.path.isabs(_o) else os.path.join(ROOT, _o)
    else:
        sys.exit('unknown argument %s' % _a)


# -- STEP 1: FBX -> OBJ VIA BLENDER ------------------------------------------------------------
# Nothing installed reads .fbx from Python (trimesh has no fbx loader, pyassimp is absent) and FBX
# is a closed deflate-compressed binary, so there is no parse-it-yourself fallback the way .glb has
# one. Blender's importer is the reference implementation. See voxelize_fir.py's header for the
# full write-up; this is the same round-trip and the .obj is cached beside the .fbx.
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
                      export_materials=False, forward_axis="Y", up_axis="Z")
'''


def to_obj():
    if os.path.exists(OBJ) and os.path.getmtime(OBJ) >= os.path.getmtime(FBX):
        print('obj cache hit: %s' % os.path.relpath(OBJ, ROOT))
        return
    blender = find_blender()
    if not blender:
        sys.exit('no Blender found - set $BLENDER to blender.exe. Nothing else installed reads .fbx.')
    tmp = os.path.join(os.path.dirname(OBJ), '_fbx2obj.py')
    open(tmp, 'w').write(CONVERT % (FBX, OBJ))
    print('converting fbx -> obj with %s (a couple of minutes - 642k triangles)' % blender)
    r = subprocess.run([blender, '--background', '--factory-startup', '--python', tmp],
                       capture_output=True, text=True)
    os.remove(tmp)
    if not os.path.exists(OBJ):
        sys.exit('blender conversion failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])


# -- STEP 2: PARSE THE OBJ, GROUPED BY OBJECT --------------------------------------------------
def load_obj(path):
    """-> {object: (tri_positions Nx3x3, tri_uvs Nx3x2)}. Plain text, no library needed.

    Vertices land in array('d')/array('i') rather than lists of lists. This OBJ is 94 MB - 1.3M
    verts and 642k faces - and the list-of-lists the fir's 244 KB file could afford costs about
    half a gigabyte here, most of it per-object header rather than coordinates."""
    V, VT = array('d'), array('d')
    faces, cur = {}, 'none'
    for line in open(path, 'r', errors='replace'):
        t = line[:2]
        if t == 'v ':
            V.extend(map(float, line.split()[1:4]))
        elif t == 'vt':
            VT.extend(map(float, line.split()[1:3]))
        elif t == 'f ':
            vi, ti = [], []
            for tok in line.split()[1:]:
                p = tok.split('/')
                vi.append(int(p[0]) - 1)
                ti.append(int(p[1]) - 1 if len(p) > 1 and p[1] else 0)
            a = faces.setdefault(cur, array('i'))
            for k in range(1, len(vi) - 1):                     # fan-triangulate, though the export is already tris
                a.extend((vi[0], vi[k], vi[k + 1], ti[0], ti[k], ti[k + 1]))
        elif t == 'o ':
            cur = line[2:].strip()
    V = np.frombuffer(V, np.float64).reshape(-1, 3)
    VT = np.frombuffer(VT, np.float64).reshape(-1, 2) if len(VT) else np.zeros((1, 2))
    out = {}
    for k, a in faces.items():
        t = np.frombuffer(a, np.int32).reshape(-1, 6)
        out[k] = (V[t[:, :3]], VT[np.maximum(t[:, 3:], 0)])
    return out


def tex_of(name):
    """The RGBA sheet for one object: its diff, with its opac stacked in as the alpha where there
    is one. Prints what it paired, because the pairing is the assumption most likely to be wrong."""
    kind, diff, opac, red = OBJ_TEX[name]
    dp = os.path.join(TEXD, diff)
    if not os.path.exists(dp):
        print('  %-44s MISSING %s -> flat %s' % (name, diff, MISS[kind]))
        return np.array(MISS[kind] + (255,), np.uint8).reshape(1, 1, 4)
    im = Image.open(dp).convert('RGB')
    if red > 1:
        im = im.reduce(red)
    a = np.asarray(im, np.uint8)
    if opac:
        op = os.path.join(TEXD, opac)
        if not os.path.exists(op):
            sys.exit('%s: opacity mask %s missing - a leaf card with no mask bakes as a solid slab' % (name, opac))
        m = Image.open(op).convert('L')
        if m.size != im.size:                                   # they measure the same today; resample rather than trust it
            m = m.resize(im.size, Image.BILINEAR)
        a = np.dstack([a, np.asarray(m, np.uint8)])
    else:
        a = np.dstack([a, np.full(a.shape[:2], 255, np.uint8)])
    print('  %-44s %s %s%s' % (name, diff, a.shape[1::-1], ' + ' + opac if opac else ''))
    return a


# -- STEP 3: RASTERIZE -------------------------------------------------------------------------
def raster(pos, uv, tex, org, scale, alpha_min):
    """Every triangle sampled at SUB-voxel spacing -> (voxel coords, RGB), one row per SAMPLE.
    voxelize_fir.py's barycentric fan, bucketed by subdivision count so each bucket is one numpy
    op - then split into CHUNK-triangle batches, because MINSUB makes those buckets far larger
    than the fir's ever were. See DIFFERENCE 4.

    Also returns a WEIGHT per sample: the triangle's area in voxel units divided by the number of
    points it was sampled at, so summing the weights of the samples that survive the alpha test
    gives that triangle's OPAQUE AREA however it happened to be sampled. See DIFFERENCE 5."""
    P, C, W = [], [], []
    wp = (pos * scale - org) / VOX                                # straight into voxel space
    A, B, Cc = wp[:, 0], wp[:, 1], wp[:, 2]
    area = 0.5 * np.linalg.norm(np.cross(B - A, Cc - A), axis=1)
    e = np.maximum.reduce([np.linalg.norm(B - A, axis=1), np.linalg.norm(Cc - A, axis=1),
                           np.linalg.norm(Cc - B, axis=1)])
    nsub = np.clip(np.ceil(e / SUB), 1, 128).astype(int)
    if alpha_min > 0:
        nsub = np.maximum(nsub, MINSUB)       # a hole is a TEXTURE-space question: sample the card's UV span, not its voxel span
    UA, UB, UC = uv[:, 0], uv[:, 1], uv[:, 2]
    th, tw = tex.shape[:2]
    for ns in np.unique(nsub):
        ij = np.array([(i, j) for i in range(ns + 1) for j in range(ns + 1 - i)], np.float64)
        a, b = ij[:, 0] / ns, ij[:, 1] / ns
        c = 1.0 - a - b
        idx = np.nonzero(nsub == ns)[0]
        for k in range(0, len(idx), CHUNK):
            sel = idx[k:k + CHUNK]
            pts = (A[sel][:, None, :] * a[None, :, None] + B[sel][:, None, :] * b[None, :, None]
                   + Cc[sel][:, None, :] * c[None, :, None]).reshape(-1, 3)
            ut = (UA[sel][:, None, :] * a[None, :, None] + UB[sel][:, None, :] * b[None, :, None]
                  + UC[sel][:, None, :] * c[None, :, None]).reshape(-1, 2)
            wt = np.repeat(area[sel] / len(ij), len(ij))
            # OBJ v is bottom-up, images are top-down; wrap first so tiling bark keeps working
            px = tex[((1.0 - ut[:, 1]) % 1.0 * (th - 1)).astype(int), (ut[:, 0] % 1.0 * (tw - 1)).astype(int)]
            if alpha_min > 0:
                keep = px[:, 3] >= alpha_min                      # THE ALPHA TEST - a leaf card is mostly hole
                pts, px, wt = pts[keep], px[keep], wt[keep]
            if len(pts):
                P.append(np.floor(pts).astype(np.int32))
                C.append(px[:, :3].astype(np.float64))
                W.append(wt)
    if not P:
        return np.zeros((0, 3), np.int32), np.zeros((0, 3)), np.zeros(0)
    return np.concatenate(P), np.concatenate(C), np.concatenate(W)


def dedupe(vx, col, w, dims):
    """sample list -> unique voxels with their AREA-WEIGHTED mean colour and their total area
    (vectorised; see voxelize_oaks.py). Weighted rather than plain-mean because the samples are
    not equal: a big triangle sampled at nsub 1 stands for far more surface than one of MINSUB's
    28 points on a leaf card, and the colour of a voxel should follow the surface, not the count."""
    if not len(vx):
        return vx, col, w
    lin = (vx[:, 0].astype(np.int64) * dims[1] + vx[:, 1]) * dims[2] + vx[:, 2]
    u, inv = np.unique(lin, return_inverse=True)
    tot = np.bincount(inv, weights=w)
    mean = np.stack([np.bincount(inv, weights=col[:, k] * w) / tot for k in range(3)], axis=1)
    z = u % dims[2]
    y = (u // dims[2]) % dims[1]
    x = u // (dims[2] * dims[1])
    return np.stack([x, y, z], axis=1).astype(np.int32), mean, tot


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
    already holds ids for, so anything snapped onto one is free. Only --repoint uses them."""
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
    """N quantized shades -> N of pine5's, matched by RELATIVE lightness. voxelize_fir.py's trick,
    kept here only for --repoint; the default bakes the birch's own colours. See DIFFERENCE 3."""
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
    ci = cidx[xs, ys, zs].astype(np.uint8)
    data = np.stack([xs.astype(np.uint8), ys.astype(np.uint8), zs.astype(np.uint8), ci], 1).tobytes()
    rgba = bytearray(1024)
    for i, c in enumerate(pal):
        o = i * 4
        rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3] = int(c[0]), int(c[1]), int(c[2]), 255

    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    body = (chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
            + chunk(b'XYZI', struct.pack('<I', len(xs)) + data)
            + chunk(b'RGBA', bytes(rgba)))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))
    return sx, sy, sz, len(xs)


# -- RUN ---------------------------------------------------------------------------------------
to_obj()
print('parsing %s (%.0f MB)' % (os.path.relpath(OBJ, ROOT), os.path.getsize(OBJ) / 1e6))
objs = load_obj(OBJ)
print('obj: ' + ', '.join('%s %d tris' % (k, len(v[0])) for k, v in sorted(objs.items())))
unknown = [k for k in objs if k not in OBJ_TEX]
if unknown:
    sys.exit('objects not in OBJ_TEX: %s - add them to the table with their texture and kind' % unknown)

allv = np.concatenate([v[0].reshape(-1, 3) for v in objs.values()])
tall = allv[:, 2].max() - allv[:, 2].min()
SCALE = TALL_M / tall
org = allv.min(0) * SCALE
dims = np.maximum(1, np.ceil((allv.max(0) * SCALE - org) / VOX).astype(int) + 1)
print('source %.2f x %.2f x %.2f units -> %.1f m tall (scale %.5f) -> grid %d x %d x %d'
      % (*(allv.max(0) - allv.min(0)), TALL_M, SCALE, *dims))
del allv

print('textures:')
wv, wc, ww, lv, lc, lw = [], [], [], [], [], []
for name in sorted(objs):
    kind = OBJ_TEX[name][0]
    vx, col, w = raster(*objs[name], tex_of(name), org, SCALE, ALPHA_MIN if kind == 'l' else 0)
    vx = np.clip(vx, 0, dims - 1)
    (wv if kind == 'w' else lv).append(vx)
    (wc if kind == 'w' else lc).append(col)
    (ww if kind == 'w' else lw).append(w)
del objs
bvx, bcol, bw = dedupe(np.concatenate(wv), np.concatenate(wc), np.concatenate(ww), dims)
nvx, ncol, nw = dedupe(np.concatenate(lv), np.concatenate(lc), np.concatenate(lw), dims)
del wv, wc, ww, lv, lc, lw

# -- DIFFERENCE 5: WHICH MATERIAL OWNS A SHARED VOXEL, BY AREA -------------------------------
# voxelize_fir.py settles this by class: WOOD WINS, because a spruce's branch cards clip through
# its trunk and must not repaint it. On a birch that rule deletes the tree. Birch leaves grow ON
# the twigs, and at 10 cm a twig and the leaves hanging off it are THE SAME VOXEL: measured, 15560
# of the 17057 leaf voxels - 91% of the canopy - collided with a twig, so the crown baked as bare
# brown wood with a green fleck here and there.
#
# Nor is it right to flip the priority: leaf-wins would eat the bole wherever a card clips it,
# which is the very thing the fir's rule exists to prevent.
#
# So neither class wins - the one with more SURFACE in the voxel does. That is what the weights
# out of raster() measure: opaque area, in voxel units, independent of how densely each triangle
# happened to be sampled. On the bole the trunk sheet is orders of magnitude more area than a leaf
# card grazing it; on a twig one leaf card dwarfs the 1 cm cross-section of the twig itself. Both
# answers fall out of the same comparison, and neither is a special case.
wgW, wgL = np.zeros(dims), np.zeros(dims)
wgW[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = bw
wgL[nvx[:, 0], nvx[:, 1], nvx[:, 2]] = nw
grid = np.zeros(dims, np.uint8)                                  # 0 air, 1 wood, 2 leaf
grid[(wgW > 0) & (wgW >= wgL)] = 1
grid[(wgL > 0) & (wgL > wgW)] = 2
kb = grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] == 1                  # keep only the colours of the voxels each class actually won
kn = grid[nvx[:, 0], nvx[:, 1], nvx[:, 2]] == 2
print('rasterized: %d wood + %d leaf voxels -> %d wood, %d leaf after the area contest (%d shared)'
      % (len(bvx), len(nvx), kb.sum(), kn.sum(), len(bvx) + len(nvx) - (grid != 0).sum()))
bvx, bcol = bvx[kb], bcol[kb]
nvx, ncol = nvx[kn], ncol[kn]
del wgW, wgL

# -- FILL THE BOLE -- the trunk mesh is an open-ended tube, so flood the outside and call every
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

# -- TRIM TO THE OCCUPIED EXTENT -- the outer texels of a leaf card are transparent, so the mesh
# bounds carry a few voxels of empty margin. Slack under the model hangs it in the air on the stage.
occ = np.argwhere(grid != 0)
mn, mx = occ.min(0), occ.max(0)
grid = grid[mn[0]:mx[0] + 1, mn[1]:mx[1] + 1, mn[2]:mx[2] + 1]
bvx -= mn
nvx -= mn
dims = np.array(grid.shape)

# -- QUANTIZE -- two separate cuts. One cut over both would spend every shade on the leaves: they
# outnumber the bark several to one and a population-weighted split follows the population.
cut_b = median_cut(np.clip(bcol, 0, 255), NBARK)
cut_n = median_cut(np.clip(ncol, 0, 255), NLEAF)
if OWN_COLOURS:
    pal_b = np.clip(np.round(cut_b), 0, 255).astype(int)
    pal_n = np.clip(np.round(cut_n), 0, 255).astype(int)
else:
    ref_b, ref_n = pine5_palette()
    pal_b, pal_n = repoint(cut_b, ref_b), repoint(cut_n, ref_n)
print('bark sampled %s\n     ->      %s' % ([[int(round(c)) for c in s] for s in cut_b],
                                            [list(map(int, c)) for c in pal_b]))
print('leaf sampled %s\n     ->      %s' % ([[int(round(c)) for c in s] for s in cut_n],
                                            [list(map(int, c)) for c in pal_n]))
print('palette: %s' % ("the birch's OWN shades - the editor BORROWS ids for these (edColExact); "
                       'check __vb.palAudit().edSubs while it is staged'
                       if OWN_COLOURS else 'repointed onto pine5.vox - zero new ids, and BROWN bark'))
pal = np.concatenate([pal_b, pal_n])

# WHICH shade a voxel gets is decided by the SAMPLED colour against the SAMPLED cut, so the texture
# keeps its own light/dark structure even when --repoint puts the pine's ramp underneath it.
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
