"""Voxelize source/fbx/nuket.fbx -- the Nuketown map -- at the engine's own 10 cm
grid, and write it as one multi-piece .vox.

Reads:  source/fbx/nuket.fbx                        (the download, gitignored)
        source/glb/nuketown.glb                     (Blender's conversion, cached)
Writes: game/assets/level/nuketown.vox              (the level)

WHAT THIS IS FOR: this is the LEVEL on [O]. It replaced building.vox there, and
voxelize_building.py is its parent -- the surface-then-flood pass, the averaged
colours, the multi-piece writer and the elevation sheet are all that file's, and
its header is still the place to read why any of them are shaped the way they
are. What follows is only what is DIFFERENT about this model.

    world extent   48.8 x 28.4 x 97.5 m  ->  489 x 285 x 977 voxels at 10 cm

BUILT AT 3x THE AUTHORED SIZE -- see SCALE, which carries the argument and the
list of what scales with it. The .fbx is a 16 x 33 m model of Nuketown with
3.8 m houses; this is a 49 x 98 m place with 11 m ones, at the same 10 cm
voxel. Set SCALE back to 1.0 and everything here still works.

-- 1. IT IS AN .fbx, AND EVERYTHING HERE READS .glb ---------------------------

The building arrived as a .glb and this arrived as a 2021 .fbx, so there is a
Blender round trip in front of the reader rather than a second parser behind it.
Blender's importer is the reference implementation for .fbx and voxelize_fir.py
and voxelize_birch.py both already lean on it for exactly this reason; the only
new part is that the conversion is CACHED, keyed on the .fbx's mtime, because
re-running this tool to retune a colour should not pay for Blender twice.

Blender is Z-up and glTF is Y-up, and the exporter does that conversion. The
long 32.5 m axis of the map -- yard, street, yard -- is the .fbx's Y and comes
out as the world's Z, which is why the grid below is 164 wide and 327 long.

-- 2. THE MODEL IS 1,374 BOXES, AND THAT DECIDES THE FILL RULE ----------------

Every object in this file is a cuboid: 1,374 of them, 8 vertices and 12
triangles each, 16,488 triangles for the whole map. Nothing is a shell with a
room in it the way the building was -- a wall here is a solid box, a roof is a
solid box, a car is a handful of them.

So the flood's cavity rule is doing a different job than it did for the
building. There it separated a wall seam from a room. Here it separates a BOX'S
OWN INSIDE from a room, and the two are far apart: the largest box in the file
encloses about 4,500 voxels at 1x and a house interior is hundreds of
thousands, so CAVITY_FILL sits between them with a wide margin either side. The
report prints both ends of that gap every run so it stays measured rather than
assumed.

AND IT IS THE ONE THING SCALE CAN BREAK SILENTLY -- a voxel count is a VOLUME,
so it has to move as SCALE**3 or every box in the map comes back hollow. See
the constant.

-- 3. THERE IS NO SLAB UNDER THE MAP, SO ONE IS ADDED -------------------------

The building came with its platform and the only question was whether to fill
it. This model's ground is a handful of thin painted slabs -- two lawns, a strip
of grass, the road, the driveways -- each about 5 cm thick, which is HALF A
VOXEL, and they do not cover the footprint: x < -6.4 and x > 4.6 have nothing
under them at all. A level like that is one you fall out of.

So the tool lays a foundation: every cell up to the measured ground row is
filled, across the whole footprint, and the grid is extended BASE_M downward to
give it thickness. That row is READ OFF THE MODEL rather than named -- see the
note beside BASE_M, which is where a constant here quietly buried the lawns the
moment the map was scaled.
It is the platform the building had, except that it is made here rather than
found in the file. The painted slabs still sit on top of it and still carry
their own colours -- the foundation is only what is underneath them.

-- 4. THE TEXTURE IS MISSING FROM THE DOWNLOAD, AND 60% OF THE MAP WEARS IT ---

The .fbx names one map, Default_texture.png, by an absolute 3ds Max path. It is
not in the archive -- that holds the .fbx and nothing else -- and no copy of it
exists in this tree. Five of the twenty materials point at it, and between them
they carry 863 of the 1,374 objects: the road, the driveways, the house siding,
most of the trim.

Their DiffuseColor in the .fbx is 0.8/0.8/0.8, which is not a colour anybody
chose -- it is the value a material takes when its colour is supposed to come
from a texture. Sampled literally it is near-white (231/231/231), and a
near-white road under a path tracer is a light source.

So UNTEXTURED is what they are painted instead, and it is the one INVENTED
number in this file. Everything else is read out of the model. If a textured
export of this map ever turns up, delete the constant and the branch that uses
it and the tool will sample the real thing -- that is the whole change.

The remaining fifteen materials DO carry authored colours and are used as they
are: the lawns' green, the brick red, the yellow of the bus, the greys.

-- 5. COLOUR IS FLAT, SO THE PALETTE IS EXACT --------------------------------

The building's colours came out of textures -- tens of thousands of distinct
shades -- and k-means over them was the only way to reach a palette v2 could
afford. Nothing here is textured, so the model has a couple of dozen distinct
colours in total and the palette is just that set. No quantization, no k-means,
no quantization error to report, and the level costs the 255-entry material
table that many ids instead of the building's twenty-eight. See the palette
notes in scene/voxelworld.h for why that number is worth keeping small.

Run with the python that has numpy, scipy and Pillow:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_nuketown.py
"""
import io
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import time

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FBX = os.path.join(ROOT, 'source', 'fbx', 'nuket.fbx')
GLB = os.path.join(ROOT, 'source', 'glb', 'nuketown.glb')
OUTDIR = os.path.join(ROOT, 'game', 'assets', 'level')
OUT = os.path.join(OUTDIR, 'nuketown.vox')

VOX = 0.1          # metres per voxel -- the engine's grid, never anything else
MAX_AXIS = 250     # per .vox piece, with room under the format's 256
SAMPLE = 0.5       # barycentric lattice step, in voxels

# -- HOW MUCH BIGGER THAN THE .fbx THE MAP IS BUILT ------------------------
#
# (user 2026-09-17: "revoxelize the nuketown scene and make it 3x bigger.
# everything still on the 10cm grid of course".)
#
# THE VOXELS DO NOT CHANGE SIZE -- that is what the second half of the ask
# means and it is the whole reason this is a scale on the MODEL and not on
# VOX. Tripling VOX would give the same 164 x 99 x 327 grid rendered as 30 cm
# cubes, which is the same map drawn coarser; tripling the model gives 489 x
# 285 x 977 of the SAME 10 cm cubes, which is a bigger map at the same detail.
#
# The source is also simply undersized for a person to walk about in: measured
# off the .fbx, its houses are 3.8 m to the eaves and its street is 7 m across,
# which is a model of Nuketown rather than a place. At 3x that is 11.4 m and
# 21 m, and the map goes from 16 x 33 m to 49 x 98 m.
#
# WHAT SCALES WITH IT, AND WHAT DOES NOT. Every constant below that is a
# measurement OF THE MODEL is multiplied by this; every constant that is a
# choice about the grid is not. Getting that split wrong is silent -- see
# CAVITY_FILL, which is the one that actually bites.
SCALE = 3.0

# A sealed pocket smaller than this is a box's inside, not a room.
#
# IT HAS TO SCALE AS A VOLUME AND THAT IS WHY IT IS WRITTEN THIS WAY. The
# threshold counts VOXELS, the model's boxes get SCALE times bigger on every
# axis, so the inside of one gets SCALE**3 bigger: the largest box in this file
# encloses 4,473 voxels at 1x and about 121,000 at 3x. Left at a flat 20,000
# every box in the map would come back HOLLOW -- which does not fail, does not
# warn, and shows up only as a map you can shoot through.
#
# The run prints the largest pocket filled and the smallest left as air, so the
# margin either side of this is checkable rather than assumed.
CAVITY_FILL = int(20000 * SCALE ** 3)

# -- THE FOUNDATION (see 3 above) -------------------------------------------
#
# HOW HIGH IT COMES IS MEASURED, NOT NAMED, AND THAT IS A BUG FIX. This used to
# be a constant -- GROUND_M = 0.10, "the first line above all the painted ground
# slabs" -- and at 1x it worked. At 3x, scaled to 0.30, it BURIED THE MAP: the
# lawns top out at 0.09 and the foundation filled flat to 0.30, so every lawn,
# the road and the markings on it came out two voxels under a sheet of concrete.
# The plan view went from green to solid grey and nothing else said a word.
#
# What is worse is WHY it survived 1x, because it was not correctness: at 0.10
# the fill stopped at row 6 and the lawn's top voxel was row 7, one clear. It
# was a rounding accident, and tripling the model is what collected on it.
#
# So the ground is read off the model instead. Every column that has anything
# in it stands on the same one or two rows -- the painted slabs are 1-2 voxels
# thick and everything in the map rests on them -- so the ground is the most
# common column TOP among the low columns, and the foundation is filled to
# EXACTLY that row. High enough that the bare apron outside the lawns is flush
# with them, never higher, so nothing painted is ever buried. It is right at
# any SCALE because it is not a length.
#
# BASE_M DOES NOT SCALE, and unlike the above it never should have. It is not a
# measurement of anything in the file -- it is how much concrete to put under
# the map so there is something there when you cut into the edge of it, and
# half a metre is as much as that needs at any size. Scaling it would add 1.5 m
# of grid under a 49 x 98 m footprint: 7 million cells holding nothing anybody
# will ever see.
BASE_M = 0.5
# ...and what it is made of. This is NOT only seen edge-on, which is what the
# first cut assumed: the painted slabs cover the middle of the map and nothing
# covers x < -6.4 or x > 4.6, so about a quarter of the floor you can stand on
# IS the foundation. At (62, 62, 66) that strip read as a pit in the plan. A
# mid concrete grey instead, which reads as the paved apron round a lot -- and
# still sits well clear of the lawns so the edge of play is legible.
FOUNDATION = (112, 109, 104)

# -- THE ONE INVENTED COLOUR (see 4 above) ----------------------------------
# A light warm concrete. Not white: the materials that land here include the
# road and the driveways, and 231/231/231 is both wrong and far too hot.
UNTEXTURED = (188, 182, 172)
UNTEXTURED_MATS = ('Default_texture',)   # ...and its .001 -- .004 siblings


# ---------------------------------------------------------------------------
# STEP 1: .fbx -> .glb VIA BLENDER, cached on the .fbx's mtime
#
# Nothing else installed here reads .fbx, and Blender's importer is the
# reference implementation -- voxelize_fir.py's header makes the same argument
# at more length. The dead texture nodes are cut on the way through: the image
# they name is not in the download (see 4), and left in place the exporter
# writes a broken image reference that the reader below would then have to know
# about.
# ---------------------------------------------------------------------------
def find_blender():
    env = os.environ.get('BLENDER')
    if env and os.path.exists(env):
        return env
    for pf in (os.environ.get('ProgramFiles', r'C:\Program Files'),
               os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')):
        root = os.path.join(pf, 'Blender Foundation')
        if not os.path.isdir(root):
            continue
        for d in sorted(os.listdir(root), reverse=True):
            exe = os.path.join(root, d, 'blender.exe')
            if os.path.exists(exe):
                return exe
    return None


BLENDER_SRC = '''
import bpy
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.fbx(filepath=r"{fbx}")
for m in bpy.data.materials:
    if not m.use_nodes:
        continue
    for n in list(m.node_tree.nodes):
        if n.type == "TEX_IMAGE" and (n.image is None or tuple(n.image.size) == (0, 0)):
            for l in list(n.outputs["Color"].links):
                m.node_tree.links.remove(l)
            m.node_tree.nodes.remove(n)
bpy.ops.export_scene.gltf(filepath=r"{glb}", export_format="GLB",
                          export_yup=True, export_apply=True,
                          export_materials="EXPORT")
'''


def ensure_glb():
    if not os.path.exists(FBX):
        sys.exit('no %s -- the download is gitignored; put nuket.fbx there' % FBX)
    if os.path.exists(GLB) and os.path.getmtime(GLB) >= os.path.getmtime(FBX):
        print('glb      cached %s' % GLB)
        return
    blender = find_blender()
    if not blender:
        sys.exit('no Blender found -- set $BLENDER to blender.exe. '
                 'Nothing else here reads .fbx.')
    os.makedirs(os.path.dirname(GLB), exist_ok=True)
    fd, tmp = tempfile.mkstemp(suffix='.py')
    os.write(fd, BLENDER_SRC.format(fbx=FBX, glb=GLB).encode('utf8'))
    os.close(fd)
    print('glb      converting %s with %s' % (os.path.basename(FBX), blender))
    t = time.time()
    r = subprocess.run([blender, '--background', '--factory-startup', '--python', tmp],
                       capture_output=True, text=True)
    os.unlink(tmp)
    if not os.path.exists(GLB):
        sys.exit('blender conversion failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])
    print('         wrote %s (%.1f MB) in %.1f s'
          % (GLB, os.path.getsize(GLB) / 1e6, time.time() - t))


ensure_glb()


# ---------------------------------------------------------------------------
# glb -- voxelize_building.py's reader, unchanged
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


# glTF's baseColorFactor is LINEAR and the .vox palette is display-referred, so
# a flat material has to be encoded on the way out. The building never noticed
# this -- almost everything in it was textured, and a texture is already sRGB --
# but EVERY surface here is a flat factor, and skipping the transfer would take
# the brick red from 138 to 64 and put the whole map in shadow.
def lin_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92,
                    1.055 * c ** (1.0 / 2.4) - 0.055) * 255.0


class Prim:
    __slots__ = ('wp', 'uv', 'idx', 'tex', 'rgb', 'name')


prims = []
untex_prims = 0
for ni, n in enumerate(js['nodes']):
    if 'mesh' not in n:
        continue
    M = world_mat(ni)
    mesh = js['meshes'][n['mesh']]
    for pr in mesh['primitives']:
        p = Prim()
        pos = acc_data(pr['attributes']['POSITION']).astype(np.float64)
        # SCALE IS APPLIED ONCE, HERE, and to the world-space vertices rather
        # than to the node transforms -- so it is a scale of the finished scene
        # and cannot interact with the hierarchy. Everything downstream works in
        # metres and simply sees a bigger map; nothing else in this file knows
        # the model was scaled at all, apart from the two constants that had to
        # scale with it.
        p.wp = ((M[:3, :3] @ pos.T).T + M[:3, 3]) * SCALE
        p.uv = (acc_data(pr['attributes']['TEXCOORD_0']).astype(np.float64)
                if 'TEXCOORD_0' in pr['attributes'] else None)
        p.idx = (acc_data(pr['indices']).astype(np.int64).ravel()
                 if 'indices' in pr else np.arange(len(pos)))
        mat = js['materials'][pr['material']] if 'material' in pr else {}
        pbr = mat.get('pbrMetallicRoughness', {})
        p.name = mat.get('name', '?')
        bct = pbr.get('baseColorTexture')
        p.tex = get_tex(bct['index']) if bct is not None else None
        if p.name.split('.')[0] in UNTEXTURED_MATS:
            # The material whose colour was supposed to come from the map that
            # is not in the download. See 4 in the header.
            p.rgb = np.array(UNTEXTURED, dtype=np.float64)
            untex_prims += 1
        else:
            p.rgb = lin_to_srgb(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3])
        prims.append(p)

lo = np.min([p.wp.min(0) for p in prims], axis=0)
hi = np.max([p.wp.max(0) for p in prims], axis=0)
# The foundation hangs below the model, so the grid starts below the model too.
lo[1] -= BASE_M
dims = np.ceil((hi - lo) / VOX).astype(np.int64) + 1     # world x, y(up), z
NX, NY, NZ = (int(v) for v in dims)
print('nuket.fbx: %d primitives, %d triangles, %d of them untextured (%.0f%%)'
      % (len(prims), sum(len(p.idx) // 3 for p in prims), untex_prims,
         100.0 * untex_prims / len(prims)))
print('  scale  %gx the authored model' % SCALE)
print('  world  lo %s  hi %s' % (lo.round(2), hi.round(2)))
print('  grid   %d x %d x %d voxels at %.0f cm  (%.1f x %.1f x %.1f m)  %.1f M cells'
      % (NX, NY, NZ, VOX * 100, NX * VOX, NY * VOX, NZ * VOX, NX * NY * NZ / 1e6))
# scene/vox.h's kVoxMaxCells. It was 64 M and this map at 3x is 136 M, so that
# constant was raised to 256 M -- the two MUST agree or the tool writes a file
# the engine reports as "implausible model dimensions".
if NX * NY * NZ > 256 * (1 << 20):
    sys.exit('grid exceeds voxParse\'s kVoxMaxCells (256 M) -- it would refuse the file')


# ---------------------------------------------------------------------------
# surface pass -- voxelize_building.py's, with the flat-colour branch taking
# the prim's own resolved rgb rather than a linear factor times 255
# ---------------------------------------------------------------------------
def surface(group):
    flat_parts, col_parts = [], []
    for p in group:
        tris = p.idx.reshape(-1, 3)
        v0, v1, v2 = p.wp[tris[:, 0]], p.wp[tris[:, 1]], p.wp[tris[:, 2]]
        e = np.maximum.reduce([np.linalg.norm(v1 - v0, axis=1),
                               np.linalg.norm(v2 - v0, axis=1),
                               np.linalg.norm(v2 - v1, axis=1)])
        nreq = np.maximum(1, np.ceil(e / (VOX * SAMPLE)).astype(np.int64))
        for n in np.unique(nreq):
            sel = np.nonzero(nreq == n)[0]
            ii, jj = np.meshgrid(np.arange(n + 1), np.arange(n + 1), indexing='ij')
            keep = (ii + jj) <= n
            a = (ii[keep] / n).astype(np.float64)
            b = (jj[keep] / n).astype(np.float64)
            c = 1.0 - a - b
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
                cols = p.tex[py, px].astype(np.float32)
            else:
                cols = np.broadcast_to(p.rgb.astype(np.float32), pts.shape).copy()
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
    """The shell, plus every sealed pocket smaller than `cavity_fill`.

    voxelize_building.py's function verbatim; its docstring is the place the
    rule is argued. What differs here is only what the two sides of the
    threshold ARE -- a box's own inside rather than a wall seam. See 2 in the
    header, and the gap this prints at the end of the run.
    """
    pad = np.zeros((NX + 2, NY + 2, NZ + 2), dtype=bool)
    ix, iy, iz = np.unravel_index(shell, (NX, NY, NZ))
    pad[ix + 1, iy + 1, iz + 1] = True

    barrier = pad.copy()
    for ax in (0, 1, 2):
        barrier |= np.roll(pad, 1, axis=ax)
        barrier |= np.roll(pad, -1, axis=ax)

    lbl, ncomp = ndimage.label(~barrier,
                               structure=ndimage.generate_binary_structure(3, 1))
    outside_id = lbl[0, 0, 0]
    if outside_id == 0:
        sys.exit('the corner of the grid is inside the barrier -- nothing to flood from')
    sizes = np.bincount(lbl.ravel())
    sizes[0] = 0
    sizes[outside_id] = 0

    fillable = np.nonzero((sizes > 0) & (sizes < cavity_fill))[0]
    kept = np.nonzero(sizes >= cavity_fill)[0]

    out = np.isin(lbl, fillable)[1:-1, 1:-1, 1:-1].copy()
    out[ix, iy, iz] = True
    print('  %-8s %d shell voxels; %d of %d sealed pockets filled (%d voxels)'
          % (what, len(shell), len(fillable), ncomp - 1,
             int(out.sum()) - len(shell)))
    # THE GAP THE THRESHOLD SITS IN, printed rather than assumed -- the largest
    # thing filled and the smallest thing kept. If those two ever approach each
    # other, CAVITY_FILL has stopped telling a box from a room.
    if len(fillable):
        print('            largest FILLED pocket %7d voxels -- a box\'s inside'
              % sizes[fillable].max())
    for c in kept[np.argsort(-sizes[kept])][:6]:
        print('            left %9d voxels (%6.0f m3) as air -- a room'
              % (sizes[c], sizes[c] * VOX ** 3))
    return out


t0 = time.time()
sflat, scol = surface(prims)
print('  sampled in %.1f s' % (time.time() - t0))

solid = flood_solid(sflat, CAVITY_FILL, 'map')

# -- THE FOUNDATION --------------------------------------------------------
# Laid AFTER the flood rather than before it, because it would otherwise be
# part of the barrier, seal the underside of the map, and turn every gap
# between the ground slabs into a "pocket" for the rule above to judge -- which
# is not a question that has an answer. See 3 in the header.
#
# The ground row is the mode of the low columns' tops -- see the note on
# BASE_M for why this is measured rather than written down.
tops = NY - 1 - np.argmax(solid[:, ::-1, :], axis=1)        # (NX, NZ)
has = solid.any(axis=1)
# The bottom eighth of the grid: high enough to take every ground slab, low
# enough that no roof, awning or car bonnet can vote on where the ground is.
low = has & (tops < max(1, NY // 8))
if not low.any():
    sys.exit('no low columns -- nothing here looks like ground')
GROUND_ROW = int(np.bincount(tops[low]).argmax())
before = int(solid.sum())
solid[:, :GROUND_ROW + 1, :] = True
print('  base    %d voxels of foundation up to row %d (y=%.2f m), %d%% of columns '
      'already stood there'
      % (int(solid.sum()) - before, GROUND_ROW, lo[1] + GROUND_ROW * VOX,
         int(round(100.0 * float((tops == GROUND_ROW).sum()) / tops.size))))

sx_, sy_, sz_ = np.unravel_index(sflat, (NX, NY, NZ))
nsolid = int(solid.sum())
print('  solid   %d voxels (%.1f%% of the grid) in %.1f s'
      % (nsolid, 100.0 * nsolid / (NX * NY * NZ), time.time() - t0))


# ---------------------------------------------------------------------------
# palette -- the EXACT set of colours, because nothing here is textured
#
# See 5 in the header. The building needed k-means over tens of thousands of
# sampled shades; this model has a couple of dozen, and the only reason the
# count is not simply len(materials) is that a voxel straddling two of them
# averages to a shade that is neither. Those are snapped to the nearest
# authored colour rather than given palette entries of their own -- a seam
# between the road and the kerb is not a colour, and v2's material table cannot
# afford to think it is.
# ---------------------------------------------------------------------------
authored = {tuple(int(v) for v in np.round(p.rgb)) for p in prims if p.tex is None}
authored.add(FOUNDATION)
# -- ...MINUS THE ONES THAT ARE THE SAME COLOUR TWICE --------------------
#
# MEASURED, AND IT IS WORTH TWO ENTRIES. v2's material table is 255 for the
# WHOLE WORLD and it is genuinely full -- adding this level and the rifle that
# goes with it overran it by seven and the wheat quietly lost two of its
# colours, which is the exact silent failure the palette notes in
# scene/voxelworld.h keep warning about.
#
# Two pairs in this model are the same colour with rounding on them:
#
#   (79,213,231) and (83,201,231)   the tree canopies -- 12.6 apart
#   (105,105,105) and (112,109,104) road grey and the foundation -- 8.1 apart
#
# Neither pair is a ramp across a surface the way the steak's reds are (see
# HeldItem::kSteakMergeTol for when folding is the WRONG answer); they are two
# materials the model's author picked separately and landed on the same shade.
# So they fold, and 16 is the threshold that takes exactly those two pairs and
# nothing else -- the next merge up is 131 against 144, which is a real
# difference across a whole wall.
#
# -- IT WENT TO 24 FOR AN AFTERNOON AND CAME BACK ---------------------------
#
# 24 was set to buy two palette entries when the engine's 255-entry table went
# full, and it cost 105 grey and 144 grey -- two real differences -- to do it.
# That is not needed any more: the LEVEL HAS A TABLE OF ITS OWN now (user:
# "surely we can have multiple color paletes for multiple worlds?"), so the map
# no longer competes with the wood's trees and animals for entries, and
# World::buildLevelPalette prints how many are still free. It is not two.
#
# -- AND IT IS BACK AT 24, BECAUSE THE GUN WON THE ARGUMENT ----------------
#
# The level's own table never happened (see World::setLevel), so the map and the
# wood still share 255 entries -- and the assault rifle went BYTE-EXACT after
# being reported twice ("the guns color pallete is off", then "its missing
# color"). Eleven exact entries instead of eight took the table to 255 of 255
# with two colours refused.
#
# 24 costs this map two greys -- 105 folds into the foundation's 112, and 144
# into 131. That is the right way round: the map is a backdrop seen at tens of
# metres and the gun is held 90 cm from the eye. Give these back the day the
# level gets a table of its own.
MERGE_TOL = 24
_reps = []
for c in sorted(authored):
    if any((c[0] - q[0]) ** 2 + (c[1] - q[1]) ** 2 + (c[2] - q[2]) ** 2 <= MERGE_TOL ** 2
           for q in _reps):
        continue
    _reps.append(c)
# THE FOUNDATION HAS TO SURVIVE IT, because FOUND_IDX below indexes this list
# by identity. It is the later of its pair in sorted order, so it is the one
# that would be dropped -- swap the representative rather than exempting it,
# which would put the pair back.
if FOUNDATION not in _reps:
    for i, q in enumerate(_reps):
        if ((FOUNDATION[0] - q[0]) ** 2 + (FOUNDATION[1] - q[1]) ** 2
                + (FOUNDATION[2] - q[2]) ** 2 <= MERGE_TOL ** 2):
            _reps[i] = FOUNDATION
            break
print('  merge   %d authored shades fold to %d at tolerance %d'
      % (len(authored), len(_reps), MERGE_TOL))
order = sorted(_reps)
PAL = np.array(order, dtype=np.uint8)
qk = np.round(scol).astype(np.int64)
print('  colour  %d authored shades; %d distinct on the shell'
      % (len(PAL), len(np.unique((qk[:, 0] * 256 + qk[:, 1]) * 256 + qk[:, 2]))))

# nearest authored colour for every sampled voxel
dd = ((scol[:, None, :] - PAL[None, :, :].astype(np.float64)) ** 2).sum(2)
shell_idx = dd.argmin(1).astype(np.uint8)
snapped = int((dd.min(1) > 1.0).sum())
print('          %d shell voxels (%.1f%%) sat between two materials and were snapped'
      % (snapped, 100.0 * snapped / len(scol)))

FOUND_IDX = np.uint8(order.index(FOUNDATION))

idxgrid = np.zeros((NX, NY, NZ), dtype=np.uint8)
# The interior -- the inside of every box, and the foundation -- takes the
# foundation's own shade. It is only ever seen where something has cut into the
# map, and that is the colour the cut face beside it will be.
idxgrid[solid] = FOUND_IDX + 1                       # 1-based; 0 is empty
idxgrid[sx_, sy_, sz_] = shell_idx + 1


# ---------------------------------------------------------------------------
# write -- world y-up becomes .vox z-up, and the grid is split to fit the bytes
# (voxelize_building.py's writer, unchanged -- see its notes on nTRN/nGRP)
# ---------------------------------------------------------------------------
def chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def s_dict(dct):
    out = struct.pack('<i', len(dct))
    for k, v in dct.items():
        out += struct.pack('<i', len(k)) + k + struct.pack('<i', len(v)) + v
    return out


def cuts(n):
    k = int(math.ceil(n / float(MAX_AXIS)))
    edges = [int(round(i * n / float(k))) for i in range(k + 1)]
    return [(edges[i], edges[i + 1]) for i in range(k)]


vgrid = np.transpose(idxgrid, (0, 2, 1))
VX, VY, VZ = vgrid.shape

pieces = []
for x0, x1 in cuts(VX):
    for y0, y1 in cuts(VY):
        for z0, z1 in cuts(VZ):
            sub = vgrid[x0:x1, y0:y1, z0:z1]
            nz = np.nonzero(sub)
            if not len(nz[0]):
                continue
            pieces.append((x0, y0, z0, x1 - x0, y1 - y0, z1 - z0,
                           np.stack([nz[0], nz[1], nz[2],
                                     sub[nz].astype(np.int64)], axis=1)))

body = b''
for (_x0, _y0, _z0, sx, sy, sz, vox) in pieces:
    body += chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body += chunk(b'XYZI', struct.pack('<I', len(vox)) +
                  vox.astype(np.uint8).tobytes())

if len(pieces) > 1:
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

faces = 0
for ax in (0, 1, 2):
    for sh in (1, -1):
        faces += int((solid & ~np.roll(solid, sh, axis=ax)).sum())
print('          ~%d exposed faces -> ~%d triangles once meshed' % (faces, faces * 2))


# ---------------------------------------------------------------------------
# PREVIEW -- voxelize_building.py's elevation sheet, PLUS a plan.
#
# The building was a thing you walked around and four elevations described it.
# A MAP is a thing you walk about IN, and the one view that says whether this
# is Nuketown -- two houses facing each other across a street, a yard behind
# each -- is the one from above. It is the first tile for that reason.
# ---------------------------------------------------------------------------
def shots(rgb_of_shell):
    vol = np.zeros((NX, NY, NZ, 3), dtype=np.uint8)
    vol[solid] = FOUNDATION
    vol[sx_, sy_, sz_] = rgb_of_shell
    out = []
    for axis, flip, name in ((1, True, 'plan'), (2, False, 'front'),
                             (2, True, 'back'), (0, False, 'left'),
                             (0, True, 'right')):
        occ = np.moveaxis(solid, axis, 0)
        col = np.moveaxis(vol, axis, 0)
        if flip:
            occ, col = occ[::-1], col[::-1]
        first = np.argmax(occ, axis=0)
        img = np.take_along_axis(col, first[None, :, :, None], 0)[0].copy()
        img[~occ.any(axis=0)] = (18, 18, 26)
        # WHICH WAY UP THE TILE GOES DEPENDS ON WHICH AXIS WAS COLLAPSED, and
        # voxelize_building.py gets this wrong -- its left and right elevations
        # come out lying on their side. Collapsing an axis leaves the other two
        # in their original order, so:
        #   axis 2 (front/back) leaves (x, height)  -> transpose, height up
        #   axis 1 (plan)       leaves (x, z)       -> transpose, so z runs up
        #                                              the page and -z is north
        #   axis 0 (left/right) leaves (height, z)  -> ALREADY rows-are-height;
        #                                              transposing stands the
        #                                              map on its end
        # Either way the row flip is what puts the top of the image at the top.
        out.append((name, (img if axis == 0 else np.transpose(img, (1, 0, 2)))[::-1]))
    return out


try:
    tiles = shots(np.clip(np.round(scol), 0, 255).astype(np.uint8))
    plan = tiles[0][1]
    elev = tiles[1:]
    cw = max(s.shape[1] for _, s in elev)
    ch = max(s.shape[0] for _, s in elev)
    W = max(plan.shape[1] + 8, 2 * (cw + 8))
    sheet = Image.new('RGB', (W, plan.shape[0] + 16 + 2 * (ch + 16)), (0, 0, 0))
    sheet.paste(Image.fromarray(plan), (0, 8))
    y0 = plan.shape[0] + 16
    for i, (name, im) in enumerate(elev):
        sheet.paste(Image.fromarray(im),
                    ((i % 2) * (cw + 8), y0 + (i // 2) * (ch + 16) + 8))
    prev = os.path.splitext(OUT)[0] + '_preview.png'
    sheet.save(prev)
    print('          preview %s   (plan on top, then front/back/left/right)' % prev)
except Exception as exc:            # a preview must never fail the asset
    print('          preview skipped: %s' % exc)
