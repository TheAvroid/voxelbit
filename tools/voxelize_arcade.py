"""Voxelize THE ARCADE -- the FPS maps on [O] -- at the engine's own 10 cm grid,
and write it as one multi-piece .vox plus the sidecar that names the maps in it.

Reads:  whatever each entry in MAPS points at (see the table)
Writes: game/assets/level/arcade.vox        the level
        game/assets/level/arcade.maps       which map is where, for /locate
        game/assets/level/arcade_preview.png

WHAT THIS IS FOR: this is the place on [O]. voxelize_building.py is its
grandparent -- the surface-then-flood pass, the averaged colours, the k-means
palette, the multi-piece writer and the elevation sheet are all that file's,
and its header is still where to read why any of them are shaped the way they
are. What follows is only what is DIFFERENT.

-- 0. IT IS SEVERAL MAPS IN ONE FILE, AND THAT IS NOT A STYLE CHOICE ----------

(user 2026-09-18: "also voxelize the canyon.zip in the downloads. put it next
to the depot map. move the maps 100 meters from one another. I want to be able
to type /locate (map name) for example.")

THE ENGINE HAS EXACTLY ONE LEVEL ASSET. `World::kLevelVox` is a path, not a
list; `levelVol_`, `levelColTop_`, `levelGrounded_` and the block BLAS are all
one grid, and App::clampToLevel fences the player to THAT grid's bounds. So
"next to" is a statement about where a map's voxels go inside one grid, and the
layout is the whole of this file's job. Nothing in C++ knows how many maps
there are.

    MAPS in order, laid out along +z, GAP_M of paved apron between each.
    The LAST one is at the +z end, which is where World::levelSpawn arrives --
    see the note on the table.

WHAT IT COSTS, BECAUSE IT IS NOT FREE. The grid is the bounding box of
everything in it, so a hundred metres of gap is a hundred metres of AIR at the
full frontage and the full height -- 193 M cells for the one gap here, more
than either map beside it. The whole arcade is 563 M cells against a 64 M cap
when this started. voxel/vox.h's kVoxMaxCells carries what that costs in host
memory and why the .vox file does NOT grow with it.

-- 1. /locate NEEDS TO KNOW WHICH RECTANGLE IS WHICH ------------------------

So the tool writes `arcade.maps` beside the voxels: one line per map, "name x0
z0 x1 z1" in grid voxels. World::loadLevelMaps reads it.

A SIDECAR AND NOT A CONSTANT IN THE ENGINE, because every number in it is
decided HERE -- how many maps, how big each model came out once scaled, how far
apart they stand. A copy in C++ is a second description of that, and the two
drift the first time a map is re-voxelized. Written by the same run that writes
the voxels, so it cannot be stale unless the .vox is too.

-- 2. WHAT EACH MODEL NEEDED, AND WHY THEY DIFFER ---------------------------

NUKETOWN is a 2021 .fbx of a model of Nuketown -- 3.8 m houses, a 7 m street --
so it is built at 3x and it is FLAT-COLOURED, with the one texture it names
missing from the download entirely (see UNTEXTURED). Its palette is the exact
set of shades its materials carry.

THE CANYON is a Sketchfab .fbx already at life size -- 94 x 27 x 68 m, rocks
8 to 13 m, a 74 x 47 m floor -- so it is built at 1x, and every one of its 69
meshes is TEXTURED with its own baked diffuse map. Nothing about its colour can
be read off a material: it has to be sampled per voxel and then quantized, which
is voxelize_building.py's k-means, brought back for it.

ONE SCALE PER MODEL AND NOT ONE FOR THE FILE. Two maps on one apron have to
agree about how big a person is, and that is what `scale` is for: it is the
number that makes each model life-size, not a number the arcade has.

-- 3. THE MODELS ARE MOSTLY SHELLS, AND THAT DECIDES THE FILL RULE ----------

voxelize_building.py's flood: the surface, plus every sealed pocket smaller
than a threshold. What the threshold SEPARATES differs per model, which is why
it is per-model here:

  * nuketown is 1,374 solid boxes. The rule is separating a BOX'S OWN INSIDE
    from a room, and the two are far apart -- the largest box encloses 182 k
    voxels at 3x and a house interior is millions.
  * the canyon is rocks and hills. A closed rock SHOULD fill; a factory with a
    door in it must not. The run prints the largest pocket it filled and the
    biggest it left, so the margin either side is checkable rather than assumed.

AND A VOXEL COUNT IS A VOLUME, so a per-model threshold has to move as
scale**3 or every box in a scaled model comes back HOLLOW -- which does not
fail, does not warn, and shows up only as a map you can shoot through.

-- 4. NONE OF THEM BRING GROUND, SO ONE IS LAID ----------------------------

Nuketown's ground is a few painted slabs 5 cm thick -- HALF A VOXEL -- over a
fraction of its footprint. The canyon had a floor but it is a canyon: it did
not reach its own bounding box either.

So the tool lays a foundation: every cell up to one measured ground row, under
each map's own rectangle, and the grid is extended BASE_M below it for
thickness. That row is READ OFF THE MODELS rather than named -- see `seat_row`
-- and it is also what SEATS THEM TOGETHER: every map is lifted so its own
floor lands on the same row.

UNDER EACH MAP AND NOT ACROSS THE WHOLE UNION, which it used to be (user
2026-09-18: "the grey platform that shares both maps is still there. remove
it"). The maps are ISLANDS -- the gap between them is air, /locate is the way
across, and App::clampToLevel fences you to the rectangle of the map you are
standing in. See THE FOUNDATION, which is where that is argued out, and the
note over clampToLevel, which is the other half of it.

-- 5. NUKETOWN'S TEXTURE IS MISSING FROM ITS DOWNLOAD -----------------------

Its .fbx names Default_texture.png by an absolute 3ds Max path; it is not in
the archive and no copy exists in this tree. Five of twenty materials point at
it and carry 863 of the 1,374 objects -- the road, the driveways, the siding,
most trim. Their DiffuseColor is 0.8/0.8/0.8, which is not a colour anybody
chose; sampled literally it is 231 grey, and a near-white road under a path
tracer is a light source.

UNTEXTURED is what they are painted instead and it is the one INVENTED colour
in this file. Blender's default Principled base colour is the same 0.8 grey, so
anything still wearing it was never painted either and takes UNTEXTURED too --
found by VALUE, because the name carries no promise and the value does.

-- 6. THE PALETTE IS SHARED ACROSS THE MAPS, IN ORDER ----------------------

One table for the whole arcade, built in MAPS order: each map's shades fold
into what is already there at MERGE_TOL, so a map added later can never move
the look of one that was already in. The run prints what each map cost.

AND THE ARCADE HAS ITS OWN 255 ENTRIES NOW (user 2026-09-18: "cant you give me
seperate tables? one palete table for the sandbox world and one for the arcade
with the fps maps"). World::buildLevelPalette mints into it and
World::uploadMaterials puts both tables in one buffer -- so the budget here is
no longer a dozen spare entries out of the wood's, it is about 120. That is
what makes a textured map affordable at all.

Run with the python that has numpy, scipy and Pillow:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_arcade.py
"""
import io
import json
import math
import os
import re
import struct
import subprocess
import sys
import tempfile
import time
from collections import deque

import numpy as np
from PIL import Image
from scipy import ndimage

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUTDIR = os.path.join(ROOT, 'game', 'assets', 'level')
OUT = os.path.join(OUTDIR, 'arcade.vox')
MAPSFILE = os.path.splitext(OUT)[0] + '.maps'

VOX = 0.1          # metres per voxel -- the engine's grid, never anything else
MAX_AXIS = 250     # per .vox piece, with room under the format's 256
SAMPLE = 0.5       # barycentric lattice step, in voxels

# -- HOW FAR APART THE MAPS STAND -------------------------------------------
#
# (user 2026-09-18: "move the maps 100 meters from one another".) Edge to edge,
# paved. It is the single most expensive number in this file -- see 0 -- and it
# is a CHOICE, so it is the first thing to turn down if the grid ever has to
# shrink.
GAP_M = 100.0

BASE_M = 0.5        # concrete under the whole thing; does NOT scale -- see 4
FOUNDATION = (112, 109, 104)
UNTEXTURED = (188, 182, 172)
UNPAINTED_FACTOR = (0.8, 0.8, 0.8)

# -- FOLDING (see 6) --------------------------------------------------------
#
# Two pairs in nuketown are the same colour with rounding on them -- the tree
# canopies 12.6 apart, road grey against the foundation 8.1 -- and neither is a
# ramp across a surface the way the steak's reds are (see
# HeldItem::kSteakMergeTol for when folding is the WRONG answer). 24 takes
# those and the next merge up is a real difference across a whole wall.
MERGE_TOL = 24

# Player::stepUp is 0.62 m. The reachability proof walks on that and nothing
# else -- no jump, no crouch -- so anything it says is reachable is reachable
# at a walk.
STEP_UP_VOX = 6


# ---------------------------------------------------------------------------
# THE ONE MAP THAT HAS TO BE PAINTED BY HAND
#
# (user 2026-09-18, told that killhouse.fbx carries no colour at all:
#  "Invent a shoothouse palette".)
#
# EVERY OTHER MAP IN THIS FILE GETS ITS COLOUR FROM THE DOWNLOAD -- a baked
# texture sampled per voxel, or the flat shade a material names. UNTEXTURED is
# the single invented colour the header admits to, and it stands for "this
# surface was never painted by anybody".
#
# THE KILLHOUSE IS NOT THAT CASE AND CANNOT BE TREATED AS IT. Checked in the
# .fbx binary rather than inferred: ONE material record (`renault_12tl`, a
# Phong with no texture), ZERO Video records, no texture filenames anywhere in
# the file, no vertex colours, and 135 of its 136 objects carry no material
# slot at all. 134 of them still have UV layouts, so this model WAS textured
# once and the maps were stripped out of the download. Run through the normal
# path every one of those 135 objects takes baseColorFactor [1,1,1] and comes
# out PURE WHITE -- 38 x 19 m of it, which under a path tracer is not a map,
# it is a light.
#
# SO THESE SHADES ARE MINE AND NOT THE ARTIST'S, and that is the whole of what
# is wrong with this table. It is recorded here rather than argued away: if a
# textured killhouse ever turns up, delete `paint` from its entry and the
# normal path takes over with nothing else to undo.
#
# WHAT THE NAMES TURNED OUT TO MEAN, read off the node inventory (136 meshes):
# a shoothouse yard -- 50 Cubes that are its plywood walls and barriers, 21
# Crates, 20 Cylinders that are stacked tyres and oil drums, 14 tables, two
# shipping containers, two sets of shelves, a ladder, a wheelie bin, a brick
# pile, three small houses, and a Renault 12 parked in it.
#
# THE FLOOR IS DELIBERATELY THE DULLEST THING HERE. `Plane` is one 36 x 19 m
# mesh -- the yard and its perimeter wall -- so it is most of what you see, and
# anything with chroma in it tints the whole map.
KILLHOUSE_PAINT = {
    'Plane':      (128, 126, 122),   # the yard and its perimeter -- worn concrete
    'house':      (196, 186, 166),   # the three structures: painted board
    'Cube':       (168, 132,  86),   # plywood walls and barriers -- the map's body colour
    'Crate':      (146, 106,  62),   # packing crates, a shade down from the ply
    'container':  ( 92, 112,  96),   # shipping containers -- faded green
    'Cylinder':   ( 58,  56,  58),   # stacked tyres and drums -- rubber dark
    'table':      (122,  92,  58),   # trestles and benches
    'Shelves':    (116, 118, 122),   # galvanised steel
    'ladder':     (132, 134, 138),   # ...and so is the ladder
    'bin':        ( 74,  96,  78),   # a wheelie bin
    'brick':      (142,  78,  62),   # the brick pile is the one warm accent
    'TrapjeOud':  (120,  90,  56),   # "old little steps" -- wooden, like the tables
    'Object':     (150, 114,  72),   # unnamed boxes; between the crate and the ply
    'pCube':      (150, 114,  72),   # ...and the Maya-named ones with them
    'Renault':    (142,  46,  42),   # the car, and the only thing meant to draw the eye
}
KILLHOUSE_PAINT_DEFAULT = (150, 146, 138)


# ---------------------------------------------------------------------------
# THE MAPS, IN THE ORDER THEY ARE LAID OUT ALONG +z
#
# THE LAST ENTRY IS WHERE YOU ARRIVE. World::levelSpawn rings outward from the
# middle of the grid's width, four metres in from the far +z end -- so whatever
# is last here is the map [O] drops you into, and everything before it is a
# walk (or a /locate) away. nuketown is last because that is the map the rifle,
# the lamps and the grass were all tuned in.
#
# `gates` CUTS A WAY IN through a model that has none -- see cut_gate. Only the
# depot ever needed it; it is kept live because a walled map is a common shape
# and the next one may be walled too.
#
# THE DEPOT IS HERE AND SWITCHED OFF (user 2026-09-18: "you can actually remove
# the depot map"). Everything it needed is in this table and in the gate code,
# so putting it back is `'on': True` and nothing else.
# ---------------------------------------------------------------------------
MAPS = [
    {
        # -- THE KILLHOUSE (user 2026-09-18: "can you remove the canyons map
        # from the fps level and put in the killhouse.fbx located in downloads.
        # I want you to voxelize it and put it where the canyons map is").
        #
        # It took the canyon's SLOT -- first in the table, so it was the map
        # at the -z end and nuketown was still the one [O] drops you into.
        #
        # -- RETIRED 2026-09-19 (user: "remove killhouse from the fps level").
        # Switched off the way the canyon and the depot below are, and for the
        # same reason: everything it needed to build -- the invented palette,
        # the 2x scale and why the PLAYER and not the arcade sets it, the 8 m3
        # cavity figure -- was measured once and is written down nowhere but
        # here. `'on': True` brings it back.
        #
        # NOTHING ELSE MOVES, because it was FIRST. nuketown is still last and
        # still the map [O] arrives in, and with one map on there is no gap to
        # span: the grid is nuketown's own 49 x 98 m again and every column of
        # it belongs to that map. (THE FOUNDATION's islands cut is what makes
        # dropping a map cost nothing to undo -- the slab was already cut to
        # each map's own rectangle, so there is no shared apron left behind.)
        'on': False,
        'name': 'killhouse',
        # A shoothouse yard: 136 meshes, 735 k triangles, 37.7 x 19.2 x 7.7 m
        # as authored. Plywood barriers, crates, tyre stacks, two containers,
        # a ladder and a Renault 12 parked in the corner.
        'fbx': os.path.join(ROOT, 'source', 'fbx', 'killhouse.fbx'),
        'glb': os.path.join(ROOT, 'source', 'glb', 'killhouse.glb'),
        # -- TWICE, AND THE REASON IS THE PLAYER AND NOT THE ARCADE ---------
        #
        # This model IS life size as authored -- measured against things whose
        # real size is known: the oil drums are 0.95 m (a real one is 0.88) and
        # the Renault is 3.93 m long (a real 12 is 4.34). So 1x is the honest
        # scale for a HUMAN.
        #
        # v2's player is not one. `Player::eye` is 2.00 m and
        # `kBodyHeightM` is 2.0 -- so at 1x the killhouse's 2.56 m ceilings
        # leave half a metre of headroom and its doorways are about the height
        # of the body that has to fit through them. A map you cannot walk into
        # is the failure reach_from exists to catch, and the cheap fix is the
        # one the canyon already used: build it bigger.
        #
        # At 2x the ceilings are 5.1 m, a crate is 1.2 m (chest high on a 2 m
        # player, which is what cover is for) and the yard is 75 x 38 m.
        'scale': 2.0,
        # ZERO, and it is the cheap way round: the grid is a dense bounding box
        # whose width is set by the WIDEST map, and nuketown is 489 voxels
        # against the killhouse's 377 either way. So the yaw cannot save any
        # frontage, and turning its long axis (37.7 m) across the row rather
        # than down it is 185 voxels less DEPTH for nothing.
        'yaw': 0.0,
        # 8 m3 authored, so 64 m3 at 2x. The rule is the one nuketown's note
        # states -- separate a solid prop's own inside from a ROOM -- and here
        # the two are much closer together than they are over there: a crate
        # encloses about 0.2 m3 and a killhouse room about 40, so this sits an
        # order of magnitude either side of both. The run prints the largest
        # pocket it filled against the largest it left, which is where to check
        # that rather than here.
        'cavity_m3': 8.0,
        'untextured_mats': (),
        'drop_mats': (),
        'pal_n': None,          # nothing to k-means: see `paint` below
        # THE COLOURS ARE INVENTED. This download carries no materials, no
        # textures and no vertex colours -- see KILLHOUSE_PAINT, which is where
        # that is argued and where the shades live.
        'paint': KILLHOUSE_PAINT,
        'paint_default': KILLHOUSE_PAINT_DEFAULT,
        'gates': (),
    },
    {
        # -- RETIRED, NOT DELETED (user 2026-09-18: "remove the canyons map
        # from the fps level"). Switched off the way the depot below is, and
        # for the same reason: everything it needed to build -- the k-means
        # palette, the watermark material names, the solid-rock cavity figure
        # and the 90 degree turn -- was measured once and is only written down
        # here. `'on': True` brings it back.
        'on': False,
        'name': 'canyon',
        # A Sketchfab lowpoly TDM map, 69 textured meshes, 435 k triangles.
        # Arrives as a .rar inside a .zip; the .fbx is what this wants.
        'fbx': os.path.join(ROOT, 'source', 'canyon', 'source', 'export', 'Offense.fbx'),
        'glb': os.path.join(ROOT, 'source', 'glb', 'canyon.glb'),
        # TWICE LIFE SIZE (user 2026-09-18: "revoxelize the canyons map. make it
        # twice as large"). It arrives at 94 x 27 x 68 m with 8-13 m rocks and
        # a 74 x 47 m floor, which is already a place rather than a model of
        # one -- so this is a deliberate enlargement and not a correction.
        #
        # IT IS THE MOST EXPENSIVE LINE IN THIS FILE. The grid is a dense
        # bounding box, so doubling a map costs EIGHT times the cells, and it
        # doubles the frontage the 100 m gap has to span as well. See the
        # report the run prints before it writes anything.
        'scale': 2.0,
        # Turned so its LONG axis (94 m) runs down the row and its short one
        # across it. That is not cosmetic: the gap costs frontage x height x
        # 100 m, so the narrow side facing across the row is worth 74 M cells.
        'yaw': 90.0,
        # -- ROCK IS SOLID ROCK (user 2026-09-18: "fill in the canyons map mesh
        # with more rock. match the rock that makes up the mesh of the canyons
        # map").
        #
        # 2000 m3 in the AUTHORED model, so 16,000 m3 once scaled. It was 200,
        # and at that line the three biggest sealed pockets in the map -- 11134,
        # 4803 and 2143 m3 -- came back as AIR: the mesas and the big rock
        # formations were hollow shells with nothing inside them. Shoot into one
        # and you were looking at a cave nobody authored.
        #
        # IT COSTS NOTHING TO DRAW. An interior voxel has no exposed face, so
        # filling a shell REMOVES the triangles that were its inside wall -- the
        # map gets more solid and cheaper at the same time.
        #
        # WHAT THE CEILING IS STILL FOR: a building with a door in it is a
        # sealed pocket too, and filling that would brick up a room. The run
        # prints the largest thing it filled and the largest it left, so the
        # margin between a rock and a room stays measured rather than assumed.
        'cavity_m3': 2000.0,
        'untextured_mats': (),
        # -- THE SKETCHFAB PREVIEW WATERMARK, WHICH IS GEOMETRY -------------
        #
        # (user 2026-09-18: "also remove the white text from the map".)
        #
        # This download is a PREVIEW build and it says so IN THE MODEL: two
        # extruded banners, "This is a preview" (69.1 x 9.3 m) and "Please,
        # download from the google drive link..." (61.2 x 1.7 m), floating 13 m
        # over the canyon floor. They are ordinary meshes with their own baked
        # materials, so nothing else can tell them from a sign -- they have to
        # be named.
        #
        # `Text_Baked` IS NOT IN THIS LIST ON PURPOSE. That one is a 0.2 m
        # label somewhere in the map, which is part of the place; only the two
        # metre-scale banners are the watermark.
        #
        # AND IT MEANS THE FULL MODEL IS SOMEWHERE ELSE -- the banner is asking
        # for a Google Drive download. If that ever turns up, it is a drop-in
        # replacement and this list can go.
        'drop_mats': ('Text.009', 'Text.020'),
        # Textured: k-means over what the surface actually samples. 48 entries
        # out of the arcade's own ~120, which is what the building needed for a
        # comparable amount of rock and metal.
        'pal_n': 48,
        'gates': (),
    },
    {
        'on': True,
        'name': 'nuketown',
        'fbx': os.path.join(ROOT, 'source', 'fbx', 'nuket.fbx'),
        'glb': os.path.join(ROOT, 'source', 'glb', 'nuketown.glb'),
        # THREE (user 2026-09-17: "revoxelize the nuketown scene and make it 3x
        # bigger. everything still on the 10cm grid of course"). THE VOXELS DO
        # NOT CHANGE SIZE -- that is the second half of the ask and the whole
        # reason this is a scale on the MODEL. Tripling VOX would be the same
        # map drawn coarser; tripling the model is a bigger map at the same
        # detail. Its houses go 3.8 m -> 11.4 m and its street 7 m -> 21 m.
        'scale': 3.0,
        'yaw': 0.0,
        # 20,000 voxels at 1x, and it MUST move as the cube of the scale: the
        # largest box in the file encloses 4,473 voxels at 1x and 182,155 at
        # 3x. Left at the 1x number every box in the map comes back hollow.
        'cavity_m3': 20.0,
        'untextured_mats': ('Default_texture',),
        'drop_mats': (),
        'pal_n': None,          # flat-coloured: the palette is the exact set
        'gates': (),
    },
    {
        'on': False,
        'name': 'depot',
        'fbx': None,
        'glb': os.path.join(ROOT, 'source', 'glb', 'depot.glb'),
        'scale': 3.0,
        'yaw': 90.0,
        'cavity_m3': 20.0,
        'untextured_mats': (),
        'drop_mats': (),
        'pal_n': None,
        # A CONTINUOUS 2 m WALL (6 m scaled) ROUND ALL FOUR SIDES with no
        # opening anywhere in it -- `floors_0` is one mesh holding the walls,
        # their footings and their caps. Voxelized as authored it is a box you
        # cannot enter and could not leave. Two gates, so the yard is a route
        # and not a room.
        'gates': ('+z', '-z'),
    },
]

# -- THE GATES (only used by a map that lists them) -------------------------
#
# HOW WIDE: 8 m, a yard gate rather than a door -- something you see from
# across the apron and walk at without aiming. The only chosen number here;
# where a gate goes is measured.
#
# HOW DEEP THE CUT IS: 1.5 m against a 0.75 m wall. Twice the wall, so the cut
# cannot miss the far face and leave a pane standing -- a wall you can see
# through and not walk through, which looks exactly like a gate that worked.
GATE_W_M = 8.0
GATE_CUT_M = 1.5
GATE_CLEAR_M = 2.5
GATE_LIP_M = 0.3


# ---------------------------------------------------------------------------
# .fbx -> .glb VIA BLENDER, cached on the .fbx's mtime
#
# Nothing else installed here reads .fbx and Blender's importer is the
# reference implementation -- voxelize_fir.py's header makes the same argument
# at more length. Cached because re-running this to retune a colour should not
# pay for Blender twice, and for the canyon that is a 40 MB export.
#
# The dead texture nodes are cut on the way through: nuketown names an image
# that is not in its download, and left in place the exporter writes a broken
# reference the reader below would have to know about. A node whose image is
# REAL is untouched, which is the whole of what the canyon needs.
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


def ensure_glb(spec):
    glb, fbx = spec['glb'], spec['fbx']
    if fbx is None:
        if not os.path.exists(glb):
            sys.exit('no %s -- source/glb is gitignored; put it there' % glb)
        return
    if not os.path.exists(fbx):
        if os.path.exists(glb):
            print('  glb    cached %s (the .fbx is gitignored and absent)' % glb)
            return
        sys.exit('no %s and no %s -- both are gitignored downloads' % (fbx, glb))
    if os.path.exists(glb) and os.path.getmtime(glb) >= os.path.getmtime(fbx):
        print('  glb    cached %s' % glb)
        return
    blender = find_blender()
    if not blender:
        sys.exit('no Blender found -- set $BLENDER to blender.exe. '
                 'Nothing else here reads .fbx.')
    os.makedirs(os.path.dirname(glb), exist_ok=True)
    fd, tmp = tempfile.mkstemp(suffix='.py')
    os.write(fd, BLENDER_SRC.format(fbx=fbx, glb=glb).encode('utf8'))
    os.close(fd)
    print('  glb    converting %s with %s' % (os.path.basename(fbx), blender))
    t = time.time()
    r = subprocess.run([blender, '--background', '--factory-startup', '--python', tmp],
                       capture_output=True, text=True)
    os.unlink(tmp)
    if not os.path.exists(glb):
        sys.exit('blender conversion failed:\n' + r.stdout[-3000:] + r.stderr[-2000:])
    print('         wrote %s (%.1f MB) in %.1f s'
          % (glb, os.path.getsize(glb) / 1e6, time.time() - t))


# ---------------------------------------------------------------------------
# glb -- voxelize_building.py's reader, taught to read more than one file
# ---------------------------------------------------------------------------
class Glb:
    def __init__(self, path):
        d = open(path, 'rb').read()
        clen = struct.unpack_from('<I', d, 12)[0]
        self.js = json.loads(d[20:20 + clen])
        boff = 20 + clen
        blen, _btype = struct.unpack_from('<II', d, boff)
        self.bin = d[boff + 8: boff + 8 + blen]
        self._tex = {}
        self.parent = {}
        for i, n in enumerate(self.js['nodes']):
            for c in n.get('children', []):
                self.parent[c] = i

    def acc(self, ai):
        a = self.js['accessors'][ai]
        bv = self.js['bufferViews'][a['bufferView']]
        off = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
        ncomp = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
        dt = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16,
              5125: np.uint32, 5126: np.float32}[a['componentType']]
        stride = bv.get('byteStride')
        itemsize = np.dtype(dt).itemsize * ncomp
        if stride and stride != itemsize:
            rows = [np.frombuffer(self.bin, dt, ncomp, off + i * stride)
                    for i in range(a['count'])]
            return np.stack(rows)
        return np.frombuffer(self.bin, dt, a['count'] * ncomp, off).reshape(a['count'], ncomp)

    def tex(self, ti):
        if ti not in self._tex:
            t = self.js['textures'][ti]
            im = self.js['images'][t['source']]
            bv = self.js['bufferViews'][im['bufferView']]
            off = bv.get('byteOffset', 0)
            raw = self.bin[off: off + bv['byteLength']]
            img = Image.open(io.BytesIO(raw)).convert('RGB')
            # -- BAKED MAPS ARE BIG AND THE SAMPLER DOES NOT CARE -----------
            # The canyon ships 69 of these at up to 2k square. Held at full
            # size they are the better part of a gigabyte of decoded pixels,
            # for a lattice that samples each one a few thousand times at a
            # tenth of a metre. Halved down to 512 the sampled colour moves by
            # well under one palette step and the whole set fits in 50 MB.
            if max(img.size) > 512:
                img = img.resize((max(1, img.size[0] * 512 // max(img.size)),
                                  max(1, img.size[1] * 512 // max(img.size))),
                                 Image.BILINEAR)
            self._tex[ti] = np.asarray(img)
        return self._tex[ti]

    @staticmethod
    def _node_mat(n):
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

    def world_mat(self, ni):
        m = self._node_mat(self.js['nodes'][ni])
        while ni in self.parent:
            ni = self.parent[ni]
            m = self._node_mat(self.js['nodes'][ni]) @ m
        return m


# glTF's baseColorFactor is LINEAR and the .vox palette is display-referred, so
# a flat material has to be encoded on the way out. voxelize_building.py never
# hit this -- almost everything in it was textured, and a texture is already
# sRGB -- but nuketown is flat throughout, and skipping the transfer takes its
# brick red from 138 to 64 and puts the whole map in shadow.
def lin_to_srgb(c):
    c = np.clip(np.asarray(c, dtype=np.float64), 0.0, 1.0)
    return np.where(c <= 0.0031308, c * 12.92,
                    1.055 * c ** (1.0 / 2.4) - 0.055) * 255.0


class Prim:
    __slots__ = ('wp', 'uv', 'idx', 'tex', 'rgb', 'name', 'node')


# -- WHAT AN OBJECT IS CALLED, WHICH IS SOMETIMES ALL THERE IS ---------------
#
# `name` above is the MATERIAL's name, and it is what every other map here is
# coloured by. The killhouse has no materials at all (see its entry in MAPS),
# so the only thing left that tells a wall from a crate is the node name the
# artist gave it -- `Cube.042`, `Crate.007`, `ladder`, `house.001`.
#
# THE TOKEN IS THE NAME WITHOUT ITS COPY NUMBER. Blender numbers duplicates
# `.001`, `.002`; Maya numbers them inline (`pCube19`). So the family is the
# leading run of letters and everything after it is which copy -- splitting on
# both is what makes one table entry cover all fifty Cubes.
#
# AN EXACT TOKEN MATCH AND NOT A PREFIX. `Cube` and `pCube` are two different
# families, and a prefix rule would have to be ordered carefully to keep them
# apart -- a rule that breaks silently the first time a model adds a name.
# paint_prims reports every token it did not recognise, so a new family shows
# up as a line in the run rather than as a grey object nobody notices.
def name_token(s):
    return re.split(r'[._0-9]', s or '')[0]


def load_prims(spec):
    """Every primitive in the file, in world metres, scaled and turned."""
    g = Glb(spec['glb'])
    js = g.js
    scale, yaw = spec['scale'], spec['yaw']
    c, s = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    prims, untex, textured, dropped = [], 0, 0, 0
    for ni, n in enumerate(js['nodes']):
        if 'mesh' not in n:
            continue
        M = g.world_mat(ni)
        mesh = js['meshes'][n['mesh']]
        for pr in mesh['primitives']:
            p = Prim()
            p.node = n.get('name', '?')
            pos = g.acc(pr['attributes']['POSITION']).astype(np.float64)
            # SCALE IS APPLIED ONCE, HERE, and to the world-space vertices
            # rather than to the node transforms -- so it is a scale of the
            # finished scene and cannot interact with the hierarchy. The YAW
            # goes on in the same breath and for the same reason. Everything
            # downstream works in metres and simply sees a bigger or a turned
            # model; nothing else knows either happened.
            wp = ((M[:3, :3] @ pos.T).T + M[:3, 3]) * scale
            if yaw:
                x, z = wp[:, 0].copy(), wp[:, 2].copy()
                wp[:, 0] = c * x + s * z
                wp[:, 2] = -s * x + c * z
            p.wp = wp
            p.uv = (g.acc(pr['attributes']['TEXCOORD_0']).astype(np.float64)
                    if 'TEXCOORD_0' in pr['attributes'] else None)
            p.idx = (g.acc(pr['indices']).astype(np.int64).ravel()
                     if 'indices' in pr else np.arange(len(pos)))
            mat = js['materials'][pr['material']] if 'material' in pr else {}
            pbr = mat.get('pbrMetallicRoughness', {})
            p.name = mat.get('name', '?')
            # Dropped before anything is sampled -- see `drop_mats` in MAPS.
            if any(p.name.startswith(k) for k in spec.get('drop_mats', ())):
                dropped += 1
                continue
            bct = pbr.get('baseColorTexture')
            p.tex = g.tex(bct['index']) if bct is not None else None
            if p.tex is not None and p.uv is not None:
                textured += 1
            fac = pbr.get('baseColorFactor', [1, 1, 1, 1])[:3]
            unpainted = all(abs(float(f) - u) < 1e-3
                            for f, u in zip(fac, UNPAINTED_FACTOR))
            if p.name.split('.')[0] in spec['untextured_mats'] or unpainted:
                # A colour that was supposed to come from a map that is not in
                # the download, or that nobody ever painted. See 5.
                p.rgb = np.array(UNTEXTURED, dtype=np.float64)
                p.tex = None
                untex += 1
            else:
                p.rgb = lin_to_srgb(fac)
            prims.append(p)
    paint_prims(prims, spec)
    return prims, untex, textured, dropped


def paint_prims(prims, spec):
    """Override every primitive's colour from a per-node-name table, if the map has one.

    Runs AFTER the normal material pass rather than instead of it, so a map
    with a `paint` table that also carries real materials keeps everything the
    table does not name.
    """
    table = spec.get('paint')
    if not table:
        return
    hit, miss = {}, {}
    for p in prims:
        tok = name_token(p.node)
        rgb = table.get(tok)
        (hit if rgb is not None else miss).setdefault(tok, 0)
        (hit if rgb is not None else miss)[tok] += 1
        p.rgb = np.array(rgb if rgb is not None else spec['paint_default'], dtype=np.float64)
        p.tex = None
    print('  paint  %s has no materials -- %d objects coloured by node name, '
          '%d families' % (spec['name'], sum(hit.values()) + sum(miss.values()), len(hit)))
    for tok, n in sorted(hit.items(), key=lambda kv: -kv[1]):
        print('           %-12s x%-4d %s' % (tok, n, tuple(int(v) for v in table[tok])))
    # NAMED LOUDLY, because an unrecognised family is the failure this table
    # has: it does not crash, it paints a wall the default and looks almost
    # right. Anything listed here is a line to add above.
    if miss:
        print('         %d object(s) in %d UNNAMED famil(ies) took the default %s: %s'
              % (sum(miss.values()), len(miss), spec['paint_default'],
                 ', '.join('%s x%d' % kv for kv in sorted(miss.items(), key=lambda kv: -kv[1]))))


# ---------------------------------------------------------------------------
# ONE MODEL -> ONE GRID: surface pass, then flood
# ---------------------------------------------------------------------------
def surface(group, lo, NX, NY, NZ):
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


def flood_solid(shell, NX, NY, NZ, cavity_fill, what):
    """The shell, plus every sealed pocket smaller than `cavity_fill`.

    voxelize_building.py's function; its docstring is where the rule is argued.
    What differs per model is only what the two sides of the threshold ARE --
    see 3 in the header, and the gap this prints at the end of the run.
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
    print('  flood  %d shell voxels; %d of %d sealed pockets filled (%d voxels)'
          % (len(shell), len(fillable), ncomp - 1, int(out.sum()) - len(shell)))
    # THE GAP THE THRESHOLD SITS IN, printed rather than assumed -- the biggest
    # thing filled and the biggest thing kept. If those two approach each
    # other, the threshold has stopped telling a rock from a room.
    if len(fillable):
        print('         largest FILLED pocket %9d voxels (%8.0f m3) -- solid'
              % (sizes[fillable].max(), sizes[fillable].max() * VOX ** 3))
    for c in kept[np.argsort(-sizes[kept])][:3]:
        print('         left %11d voxels (%8.0f m3) as air -- a room'
              % (sizes[c], sizes[c] * VOX ** 3))
    return out


def seat_row(solid, NY):
    """The row this model stands on -- the mode of the LOW columns' tops.

    Every column that has anything in it stands on the same one or two rows, so
    the floor is the most common column TOP among the low ones. The bottom
    eighth of the grid is high enough to take any ground slab and low enough
    that no roof, awning or canyon wall can vote on where the ground is.

    IT IS MEASURED AND NOT NAMED, AND THAT IS A BUG FIX. It used to be a
    constant -- "the first line above all the painted ground slabs" -- and at
    1x it worked by a rounding accident. At 3x, scaled with everything else, it
    filled flat OVER nuketown's lawns: the plan view went from green to solid
    grey and nothing else said a word. A row is not a length and must not scale.
    """
    tops = NY - 1 - np.argmax(solid[:, ::-1, :], axis=1)        # (NX, NZ)
    has = solid.any(axis=1)
    low = has & (tops < max(1, NY // 8))
    if not low.any():
        sys.exit('no low columns -- nothing here looks like ground')
    return int(np.bincount(tops[low]).argmax()), tops, has


class Model:
    """A voxelized map: where it sits in metres, and what is solid in it."""

    def __init__(self, spec):
        self.spec = spec
        self.name = spec['name']
        print('%s:' % self.name)
        ensure_glb(spec)
        prims, untex, textured, dropped = load_prims(spec)
        self.prims = prims
        self.textured = textured > 0
        lo = np.min([p.wp.min(0) for p in prims], axis=0)
        hi = np.max([p.wp.max(0) for p in prims], axis=0)
        lo[1] -= BASE_M            # the foundation hangs below the model
        dims = np.ceil((hi - lo) / VOX).astype(np.int64) + 1
        self.lo = lo
        self.NX, self.NY, self.NZ = (int(v) for v in dims)
        print('  model  %d primitives, %d triangles; %d textured, %d unpainted%s'
              % (len(prims), sum(len(p.idx) // 3 for p in prims), textured, untex,
                 ', %d DROPPED (%s)' % (dropped, ', '.join(spec['drop_mats']))
                 if dropped else ''))
        print('  scale  %gx, turned %g deg -> %.1f x %.1f x %.1f m'
              % (spec['scale'], spec['yaw'], self.NX * VOX, self.NY * VOX, self.NZ * VOX))
        print('  grid   %d x %d x %d voxels, %.1f M cells'
              % (self.NX, self.NY, self.NZ, self.NX * self.NY * self.NZ / 1e6))
        t = time.time()
        self.sflat, self.scol = surface(prims, lo, self.NX, self.NY, self.NZ)
        print('  sampled %d shell voxels in %.1f s' % (len(self.sflat), time.time() - t))
        cav = int(spec['cavity_m3'] / (VOX ** 3) * spec['scale'] ** 3)
        self.solid = flood_solid(self.sflat, self.NX, self.NY, self.NZ, cav, self.name)
        self.ground, tops, has = seat_row(self.solid, self.NY)
        # HOW FLAT THE FLOOR IS, because the foundation is laid to ONE row and
        # anything below that row is buried by it. A canyon is not a car park:
        # the number to watch is how much of this model sits UNDER its own
        # modal floor, and if it is ever large the seat wants to be the minimum
        # rather than the mode.
        low = has & (tops < max(1, self.NY // 8))
        under = int((low & (tops < self.ground)).sum())
        print('  seat   row %d (y = %.2f m in the model); %d of %d ground columns sit '
              'BELOW it and will be filled in'
              % (self.ground, lo[1] + self.ground * VOX, under, int(low.sum())))
        print('  solid  %d voxels' % int(self.solid.sum()))


# ---------------------------------------------------------------------------
# THE GATES -- only for a map that lists them. See the note in MAPS.
# ---------------------------------------------------------------------------
def gate_span(solid, ground, far):
    """Where along x to cut a gate through the +z (far) or -z wall.

    THE GATE IS ALWAYS GATE_W_M WIDE; what is measured is WHERE it goes. The
    tool slides that window along the wall and keeps the position with the most
    clear yard behind it, breaking a tie toward the middle of the wall.

    IT USED TO TAKE THE LONGEST PERFECTLY CLEAR RUN AND CUT ONLY THAT, and on a
    yard with its walls lined that is a 3 m doorway in a 49 m wall -- there is
    no 8 m stretch anywhere with nothing behind it. A player walking up to a
    6 m wall looking for a 3 m slot is the thing the gates exist to prevent.
    """
    NX, _NY, NZ = solid.shape
    cut = int(round(GATE_CUT_M / VOX))
    clear = int(round(GATE_CLEAR_M / VOX))
    lip = ground + int(round(GATE_LIP_M / VOX))
    z0, z1 = (NZ - cut - clear, NZ - cut) if far else (cut, cut + clear)
    band = solid[:, lip + 1:, max(0, z0):max(1, z1)]
    free = ~band.any(axis=(1, 2))
    want = min(int(round(GATE_W_M / VOX)), NX)
    csum = np.concatenate(([0], np.cumsum(free.astype(np.int64))))
    score = csum[want:] - csum[:-want]
    best = int(score.max())
    at = np.nonzero(score == best)[0]
    x0 = int(at[np.argmin(np.abs(at + want // 2 - NX // 2))])
    print('  gate   %s wall: %.1f m at x %d, %d of its %d columns clear %.1f m deep'
          % ('+z' if far else '-z', want * VOX, x0, best, want, GATE_CLEAR_M))
    return x0, x0 + want


def cut_gate(grid, x0, x1, ground, far, dx, dz, nz_model):
    """Clear the wall, and only the wall, over the gate's span -- in COMBINED
    coordinates. The floor at `ground` stays: a gate with a lip in it is a gate
    you trip on, and the foundation under it is what you walk across."""
    cut = int(round(GATE_CUT_M / VOX))
    z0, z1 = (nz_model - cut, nz_model) if far else (0, cut)
    grid[dx + x0: dx + x1, ground + 1:, dz + z0: dz + z1] = False


# ---------------------------------------------------------------------------
# THE PROOF -- can you actually walk to each map?
# ---------------------------------------------------------------------------
def col_tops(solid):
    """The highest solid row in every column, or -1 where the column is empty."""
    NX, NY, NZ = solid.shape
    any_ = solid.any(axis=1)
    top = NY - 1 - np.argmax(solid[:, ::-1, :], axis=1)
    return np.where(any_, top, -1).astype(np.int32)


def ring_spawn(top, ground, cx, cz, box=None):
    """Where the engine would put you, by its own ring search. None if nowhere.

    This is World::levelSpawn and World::levelMapSpawn, which are one loop with
    two different centres and two different bounds -- the grid for [O], a map's
    own rectangle for /locate. Written once here for the same reason it is
    written twice over there: the two must not drift.

    `box` is (x0, z0, x1, z1) half-open, or None for the whole grid. The two
    voxel inset off the GRID edge is kept either way -- that is
    App::clampToLevel's fence, and a spawn on the wrong side of it is a spawn
    the player is immediately pushed out of.
    """
    NX, NZ = top.shape
    x0, z0, x1, z1 = box if box else (0, 0, NX, NZ)
    open_top = ground + int(round(1.0 / VOX))
    for r in range(0, max(x1 - x0, z1 - z0) // 2 + 1):
        for dz in range(-r, r + 1):
            for dx in range(-r, r + 1):
                if r > 0 and abs(dx) != r and abs(dz) != r:
                    continue
                x, z = cx + dx, cz + dz
                if x < x0 or z < z0 or x >= x1 or z >= z1:
                    continue
                if x < 2 or z < 2 or x >= NX - 2 or z >= NZ - 2:
                    continue
                if ground <= top[x, z] <= open_top:
                    return (x, z)
    return None


def reach_from(top, start):
    """Every column reachable on foot from `start`, by Player::stepUp and nothing else.

    No jump and no crouch, so a column this reaches is one a player reaches at
    a walk.

    IT IS THE ONLY CHECK THAT CATCHES THE INTERESTING FAILURES. A map seated a
    step too high, a gate cut into the wrong wall, an island whose floor does
    not reach its own edge -- all three look perfectly correct in a preview,
    and all three are a map you cannot get around.
    """
    NX, NZ = top.shape
    vis = np.zeros((NX, NZ), dtype=bool)
    q = deque([start])
    vis[start] = True
    while q:
        x, z = q.popleft()
        t = top[x, z]
        for dx, dz in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nx, nz = x + dx, z + dz
            if nx < 0 or nz < 0 or nx >= NX or nz >= NZ or vis[nx, nz]:
                continue
            nt = top[nx, nz]
            if nt < 0 or abs(int(nt) - int(t)) > STEP_UP_VOX:
                continue
            vis[nx, nz] = True
            q.append((nx, nz))
    return vis


# ---------------------------------------------------------------------------
# RUN
# ---------------------------------------------------------------------------
t_all = time.time()
specs = [m for m in MAPS if m['on']]
if not specs:
    sys.exit('every map in MAPS is switched off -- nothing to build')
models = [Model(s) for s in specs]

# -- LAY THEM OUT ------------------------------------------------------------
#
# In a row along +z, in MAPS order, GAP_M of apron between each and every map
# centred on the width. The LAST one ends at the +z edge, which is where
# World::levelSpawn arrives -- see the note on the table.
#
# SEATED BY THEIR OWN GROUND ROWS, not by their origins: that is what puts
# every map's floor on one level, so the apron between them is flat and the
# walk from one to the next has no step in it.
GAPV = int(round(GAP_M / VOX))
NX = max(m.NX for m in models)
NZ = sum(m.NZ for m in models) + GAPV * (len(models) - 1)
GROUND_ROW = max(m.ground for m in models)
NY = max(m.NY + (GROUND_ROW - m.ground) for m in models)
place = []
_z = 0
for m in models:
    place.append((m, (NX - m.NX) // 2, GROUND_ROW - m.ground, _z))
    _z += m.NZ + GAPV
print('')
print('arcade  %d x %d x %d voxels at %.0f cm  (%.1f x %.1f x %.1f m)  %.1f M cells'
      % (NX, NY, NZ, VOX * 100, NX * VOX, NY * VOX, NZ * VOX, NX * NY * NZ / 1e6))
for m, ox, oy, oz in place:
    print('        %-10s at x %4d z %5d, lifted %d rows  (%.0f x %.0f m)'
          % (m.name, ox, oz, oy, m.NX * VOX, m.NZ * VOX))
if len(models) > 1:
    print('        %.0f m of open air between maps -- they are islands, /locate is the way '
          'across' % GAP_M)
# voxel/vox.h's kVoxMaxCells. The two MUST agree or this writes a file the
# engine reports as "implausible model dimensions" -- see the note there for
# what the count costs in host memory, which is not what the count suggests.
if NX * NY * NZ > 3072 * (1 << 20):
    sys.exit('grid exceeds voxParse\'s kVoxMaxCells (3 G) -- it would be refused. '
             'GAP_M is the cheapest thing to turn down.')

solid = np.zeros((NX, NY, NZ), dtype=bool)
for m, ox, oy, oz in place:
    solid[ox:ox + m.NX, oy:oy + m.NY, oz:oz + m.NZ] |= m.solid

# -- THE FOUNDATION, AND IT STOPS AT EACH MAP'S OWN EDGE NOW -----------------
#
# Laid AFTER each model's own flood, never before: part of the barrier it would
# seal every model's underside and turn each gap between ground slabs into a
# "pocket" for the cavity rule to judge, which is not a question that has an
# answer.
#
# -- THE MAPS ARE ISLANDS (user 2026-09-18: "the grey platform that shares both
# maps is still there. remove it") ------------------------------------------
#
# This used to be `solid[:, :GROUND_ROW + 1, :] = True` -- one slab across the
# whole union footprint. That slab was doing three jobs and only one of them
# was wanted:
#
#   1. THE FLOOR INSIDE A MAP. Kept, and it is not optional: section 4 above
#      measures what the models actually bring, and it is nuketown's half-voxel
#      painted slabs over part of its footprint. Take this away and you fall
#      through the map.
#   2. WHAT markLevelGrounded FLOODS FROM. Kept: each island still runs from
#      row 0 up, so every voxel in a map still has a path to the bottom of the
#      grid and nothing in here reads as floating.
#   3. THE 100 m WALK BETWEEN THE MAPS. GONE, and that is the ask. It was also
#      the whole of what you could see of it -- a map's own pad is hidden under
#      that map, but the apron BETWEEN them is 100 m of bare grey at the full
#      width of the grid, plus a strip down either side of any map narrower
#      than the widest one.
#
# SO THE SLAB IS CUT TO EACH MAP'S OWN RECTANGLE. The gap between them is air,
# and the way across is /locate. App::clampToLevel fences you to the rectangle
# of the map you are in rather than to the grid, so the edge of an island is
# the edge of the world while you are standing on it -- see the note there,
# which is the other half of this change and does not work without it.
_before = int(solid.sum())
for m, ox, oy, oz in place:
    solid[ox:ox + m.NX, :GROUND_ROW + 1, oz:oz + m.NZ] = True
print('  base   %d voxels of foundation up to row %d, under the maps only '
      '(%d of the grid\'s %d columns are now open air)'
      % (int(solid.sum()) - _before, GROUND_ROW,
         NX * NZ - sum(m.NX * m.NZ for m, _ox, _oy, _oz in place), NX * NZ))

for m, ox, oy, oz in place:
    for side in m.spec['gates']:
        gx0, gx1 = gate_span(m.solid, m.ground, side == '+z')
        cut_gate(solid, gx0, gx1, GROUND_ROW, side == '+z', ox, oz, m.NZ)

# -- AND THE CONCRETE NOBODY WILL EVER SEE COMES OFF -------------------------
#
# GROUND_ROW is set by whichever map has the most geometry UNDER its own floor,
# and the canyon has 2.8 m of rock bottoms below its floor plane. Seating
# everything on that row leaves 33 rows of solid foundation under the whole
# 68 x 292 m footprint -- 66.6 M voxels, 55 M of them buried, and a .vox file
# of 291 MB that is mostly a description of concrete.
#
# It is all SOLID and identical, so the rows below the last half-metre carry no
# information at all: cutting them changes nothing you can see, nothing you can
# walk on, and nothing markLevelGrounded can reach (it floods from row 0, which
# is still solid). What it costs is how deep you could dig before hitting the
# bottom of the world, and half a metre of concrete is what BASE_M always meant
# that to be.
#
# MEASURED: 291 MB -> 72 MB, 608 M cells -> 552 M.
KEEP_BASE = int(round(BASE_M / VOX))
_trim = max(0, GROUND_ROW - KEEP_BASE)
if _trim:
    solid = solid[:, _trim:, :]
    NY -= _trim
    GROUND_ROW -= _trim
    place = [(m, ox, oy - _trim, oz) for (m, ox, oy, oz) in place]
    print('  trim   %d rows of buried foundation removed; grid is %d x %d x %d, '
          '%.1f M cells' % (_trim, NX, NY, NZ, NX * NY * NZ / 1e6))
print('  solid  %d voxels (%.1f%% of the grid)'
      % (int(solid.sum()), 100.0 * solid.sum() / (NX * NY * NZ)))


# ---------------------------------------------------------------------------
# PALETTE -- one table for the arcade, built in MAPS order (see 6)
#
# A FLAT map contributes the exact set of shades its materials carry, folded at
# MERGE_TOL. A TEXTURED one cannot: its colour is whatever its baked maps
# sample to, tens of thousands of distinct shades, so it contributes k-means
# centres over what its own surface actually wears -- voxelize_building.py's
# function, weighted by how many voxels wear each shade.
# ---------------------------------------------------------------------------
def kmeans(x, w, k, iters=30, seed=12345):
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
            msk = lab == j
            if msk.any():
                c[j] = (x[msk] * w[msk, None]).sum(0) / w[msk].sum()
    return c


def fold_in(reps, shades, tol):
    """Add what is not already there, within `tol`. Returns how many were new."""
    added = 0
    for s in shades:
        s = tuple(int(v) for v in np.round(s))
        if any((s[0] - q[0]) ** 2 + (s[1] - q[1]) ** 2 + (s[2] - q[2]) ** 2 <= tol * tol
               for q in reps):
            continue
        reps.append(s)
        added += 1
    return added


# THE FOUNDATION GOES IN FIRST so nothing can fold it away. It used to go in
# last and it did NOT survive: nuketown's road grey is 8.1 from it and got
# there first, and FOUND_IDX indexes this list by identity.
reps = [FOUNDATION]
for m in models:
    if m.textured:
        q = np.round(m.scol).astype(np.int64)
        key = (q[:, 0] * 256 + q[:, 1]) * 256 + q[:, 2]
        ukey, ucnt = np.unique(key, return_counts=True)
        upts = np.stack([(ukey >> 16) & 255, (ukey >> 8) & 255, ukey & 255],
                        axis=1).astype(np.float64)
        n = min(m.spec['pal_n'], len(upts))
        t = time.time()
        centres = kmeans(upts, ucnt.astype(np.float64), n)
        # -- AND K-MEANS IS NOT SECOND-GUESSED AT MERGE_TOL ------------------
        #
        # MERGE_TOL exists to fold two AUTHORED shades that are the same colour
        # with rounding on them -- a thing an artist did twice. A k-means centre
        # is not that: it is already the answer to "what are the N colours this
        # surface wears", and 48 of them are 48 because the map needed 48.
        #
        # FOLDING THEM AT 24 THREW AWAY THREE QUARTERS OF THE CANYON. Measured:
        # 48 centres came back as 11 entries, and the map quantized at 11.9/255
        # RMS -- banded rock, for no saving that matters now the arcade has its
        # own table with over a hundred free. Deduped at kDupTol instead, which
        # only catches a centre that is genuinely the same entry as one already
        # in the list.
        added = fold_in(reps, centres, 6)
        print('  colour %-10s %d distinct sampled shades -> %d centres in %.1f s, '
              '%d NEW entries' % (m.name, len(upts), len(centres), time.time() - t, added))
    else:
        authored = sorted({tuple(int(v) for v in np.round(p.rgb))
                           for p in m.prims if p.tex is None})
        added = fold_in(reps, authored, MERGE_TOL)
        print('  colour %-10s %d authored shades, %d NEW entries'
              % (m.name, len(authored), added))
print('  colour the arcade wears %d palette entries of its own 255' % len(reps))
if len(reps) > 200:
    print('         WARNING: World::buildLevelPalette reserves the fixed band, the held '
          'kit and the wood\'s flowers out of that 255 -- this is close.')

PAL = np.array(reps, dtype=np.uint8)
FOUND_IDX = np.uint8(reps.index(FOUNDATION))

# Every shell voxel takes its nearest entry. A voxel that straddled two
# materials averaged to a shade that is neither, and snapping it is right: a
# seam between a road and a kerb is not a colour, and the table cannot afford
# to think it is.
idxgrid = np.zeros((NX, NY, NZ), dtype=np.uint8)
idxgrid[solid] = FOUND_IDX + 1                       # 1-based; 0 is empty
for m, ox, oy, oz in place:
    sx_, sy_, sz_ = np.unravel_index(m.sflat, (m.NX, m.NY, m.NZ))
    sx_, sy_, sz_ = sx_ + ox, sy_ + oy, sz_ + oz
    dd = ((m.scol[:, None, :] - PAL[None, :, :].astype(np.float64)) ** 2).sum(2)
    idx = dd.argmin(1).astype(np.uint8)
    rms = float(np.sqrt(dd.min(1).mean()))
    # OFF THE BOTTOM OF THE GRID IS NOT "row -3", IT IS GONE. The trim above
    # takes a map's below-floor rows with it and `oy` goes negative for the map
    # that set GROUND_ROW -- and a negative index into numpy does not fail, it
    # WRAPS, so those voxels would be painted onto the roof of the level. This
    # test is the whole of what stops that.
    inside = (sy_ >= 0) & (sy_ < NY)
    sx_, sy_, sz_, idx = sx_[inside], sy_[inside], sz_[inside], idx[inside]
    # A shell voxel the foundation buried, or a gate cut away, is not a surface
    # any more. Painting it anyway is how a gate comes out with a pane of wall
    # still standing in it.
    keep = solid[sx_, sy_, sz_]
    idxgrid[sx_[keep], sy_[keep], sz_[keep]] = idx[keep] + 1
    print('  paint  %-10s %d shell voxels at %.1f/255 RMS, %d trimmed off the bottom, '
          '%d buried or cut away'
          % (m.name, len(m.scol), rms, int((~inside).sum()), int((~keep).sum())))
    # THIS MAP'S OWN GROUND SHADE -- the commonest thing it wears within a
    # metre of the floor. Used to hide the concrete apron, below.
    low = keep & (sy_ >= GROUND_ROW) & (sy_ <= GROUND_ROW + 10)
    m.groundIdx = (int(np.bincount(idx[low] + 1).argmax()) if low.any()
                   else int(FOUND_IDX) + 1)
    # ...AND ITS DOMINANT SHADE OVER THE WHOLE MAP, which is what the inside of
    # it should be made of. For the canyon that is rock; for nuketown it is the
    # siding. Taken over everything rather than just the floor, because what
    # this fills is the inside of a CLIFF, not the ground under one.
    m.bodyIdx = (int(np.bincount(idx[keep] + 1).argmax()) if keep.any()
                 else int(FOUND_IDX) + 1)

# -- AND THE GREY PLATFORM UNDER EACH MAP IS NOT GREY ------------------------
#
# (user 2026-09-18: "remove the grey platform you have at the base of the map".)
#
# WHAT IT WAS: the foundation is one flat slab across the whole union footprint
# in FOUNDATION concrete, and a map never covers its own bounding box -- the
# canyon's floor plane reaches 74 x 47 m of a 136 x 188 m footprint. So every
# map stood on a visible grey pad that ran out past its terrain, which reads as
# a diorama on a plinth rather than a place.
#
# IT CANNOT SIMPLY GO, and that has not changed: the slab is still the floor
# wherever a model brought none, and still what `markLevelGrounded` floods
# from. Taking it out from under a map is the "level you fall out of" this tool
# exists to avoid.
#
# SO ONLY ITS EXPOSED TOP CHANGES: where the apron is what you would see, it
# wears THAT MAP's own ground instead of concrete, and the slab underneath is
# untouched.
#
# -- ...AND THE HALF OF IT THAT WAS STILL GREY IS GONE ENTIRELY -------------
#
# (user 2026-09-18, after the above shipped: "the grey platform that shares
# both maps is still there. remove it.")
#
# They were right and this pass was only ever half the answer. It repaints the
# apron INSIDE a map's rectangle -- but the slab ran across the whole union
# footprint, so what was left over was the part you could actually see as a
# platform: 100 m of bare concrete between the maps at the full width of the
# grid, plus a strip down either side of any map narrower than the widest.
# Recolouring that would have been the wrong fix twice, because it is not
# either map's ground and painting it as one would be a lie about where a map
# ends.
#
# THE FOUNDATION IS CUT TO THE MAPS THEMSELVES now -- see THE FOUNDATION above
# -- so every square of slab left in the grid is under a map, and this pass
# repaints all of it rather than the part of it that showed. There is no
# "between the maps" any more to leave concrete.
# -- AND WHAT IS INSIDE A MAP IS MADE OF THAT MAP --------------------------
#
# (user 2026-09-18: "match the rock that makes up the mesh of the canyons map".)
#
# Every solid voxel starts as FOUNDATION concrete and only the SHELL is then
# painted, so the inside of every cliff, mesa and house was a block of grey --
# invisible until a bullet or a pick cut into it, and then obviously wrong.
#
# ABOVE THE GROUND ROW, INSIDE A MAP'S BOX, a voxel still wearing concrete can
# only be that map's own interior: the shared foundation is at or below that
# row by construction, and the apron pass below owns its top face. So the test
# is exactly that, and it cannot reach the slab or the path between the maps.
for m, ox, oy, oz in place:
    box = idxgrid[ox:ox + m.NX, GROUND_ROW + 1:, oz:oz + m.NZ]
    inner = box == FOUND_IDX + 1
    n = int(inner.sum())
    if not n:
        continue
    box[inner] = np.uint8(m.bodyIdx)
    print('  fill   %-10s %d interior voxels are the map\'s own material now '
          '(%d of the palette)' % (m.name, n, m.bodyIdx - 1))

apron = idxgrid[:, GROUND_ROW, :] == FOUND_IDX + 1
if GROUND_ROW + 1 < NY:
    apron &= idxgrid[:, GROUND_ROW + 1, :] == 0     # only what is actually on top
for m, ox, oy, oz in place:
    sel = np.zeros_like(apron)
    sel[ox:ox + m.NX, oz:oz + m.NZ] = apron[ox:ox + m.NX, oz:oz + m.NZ]
    n = int(sel.sum())
    if not n:
        continue
    xs, zs = np.nonzero(sel)
    idxgrid[xs, GROUND_ROW, zs] = np.uint8(m.groundIdx)
    print('  apron  %-10s %d concrete voxels took the map\'s own ground shade '
          '(%d of the palette)' % (m.name, n, m.groundIdx - 1))

solid = idxgrid != 0


# ---------------------------------------------------------------------------
# ...AND PROVE YOU CAN WALK AROUND EVERY ONE OF THEM
#
# ONE FLOOD PER MAP, FROM WHERE THAT MAP'S OWN ARRIVAL PUTS YOU. It used to be
# a single flood from the [O] spawn, which was the right test while one apron
# joined every map: reaching the far map proved the walk between them as well.
# The maps are islands now (see THE FOUNDATION), so a single flood can only
# ever reach the one it starts on, and the question has changed with the
# layout -- it is no longer "can you walk there", it is "once /locate has put
# you there, can you walk around it".
#
# IT IS THE STRICTER TEST OF THE TWO, which is worth saying because it reads
# like a weakening. The old flood could pass a map on its APRON alone: arrive,
# never get up onto the map itself, and still count every column of pad inside
# its rectangle as reached. This one starts where levelMapSpawn actually drops
# you and can only spread from there.
# ---------------------------------------------------------------------------
top = col_tops(solid)
spawn = ring_spawn(top, GROUND_ROW, NX // 2, NZ - int(round(4.0 / VOX)))
if spawn is None:
    sys.exit('no open column anywhere near the [O] spawn -- the arcade has no floor')
print('  walk   [O] arrives at column %s' % (spawn,))
bad = []
for m, ox, oy, oz in place:
    box = (ox, oz, ox + m.NX, oz + m.NZ)
    # levelMapSpawn's own centre and bounds -- see ring_spawn.
    st = ring_spawn(top, GROUND_ROW, (box[0] + box[2]) // 2, (box[1] + box[3]) // 2, box)
    if st is None:
        print('         %-10s NO OPEN COLUMN to arrive on' % m.name)
        bad.append(m.name)
        continue
    sub = reach_from(top, st)[ox:ox + m.NX, oz:oz + m.NZ]
    frac = float(sub.mean())
    print('         %-10s arrives at %s, %5.1f%% of its footprint reachable from there'
          % (m.name, st, 100.0 * frac))
    if frac < 0.10:
        bad.append(m.name)
if bad:
    sys.exit('cannot walk around %s from where /locate puts you. A map is seated wrong, '
             'walled, or its floor does not reach its own edge -- the asset is NOT '
             'written.' % ', '.join(bad))


# ---------------------------------------------------------------------------
# WRITE -- the sidecar first, then the voxels
# ---------------------------------------------------------------------------
os.makedirs(OUTDIR, exist_ok=True)
with open(MAPSFILE, 'w') as f:
    f.write('# the maps in %s, in grid VOXELS: name x0 z0 x1 z1\n' % os.path.basename(OUT))
    f.write('# written by tools/voxelize_arcade.py -- World::loadLevelMaps reads it, and\n')
    f.write('# /locate <name> is what it is for. Do not hand-edit: the next run of the\n')
    f.write('# tool overwrites it, and a stale rectangle is a /locate that lands you in\n')
    f.write('# somebody else\'s map.\n')
    for m, ox, oy, oz in place:
        f.write('%s %d %d %d %d\n' % (m.name, ox, oz, ox + m.NX, oz + m.NZ))
print('  wrote  %s' % MAPSFILE)


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


vgrid = np.transpose(idxgrid, (0, 2, 1))     # world y-up becomes .vox z-up
VX, VY, VZ = vgrid.shape

pieces = []
for x0, x1 in cuts(VX):
    for y0, y1 in cuts(VY):
        for z0, z1 in cuts(VZ):
            sub = vgrid[x0:x1, y0:y1, z0:z1]
            nz = np.nonzero(sub)
            if not len(nz[0]):
                continue      # ...and THIS is why a mostly-empty grid is a small file
            pieces.append((x0, y0, z0, x1 - x0, y1 - y0, z1 - z0,
                           np.stack([nz[0], nz[1], nz[2],
                                     sub[nz].astype(np.int64)], axis=1)))

body = b''
for (_x0, _y0, _z0, sx, sy, sz, vox) in pieces:
    body += chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body += chunk(b'XYZI', struct.pack('<I', len(vox)) + vox.astype(np.uint8).tobytes())

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

open(OUT, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))
print('  wrote  %s' % OUT)
print('         %d x %d x %d .vox voxels in %d pieces, %.1f MB, whole run %.0f s'
      % (VX, VY, VZ, len(pieces), os.path.getsize(OUT) / 1e6, time.time() - t_all))

faces = 0
for ax in (0, 1, 2):
    for sh in (1, -1):
        faces += int((solid & ~np.roll(solid, sh, axis=ax)).sum())
print('         ~%d exposed faces -> ~%d triangles once meshed' % (faces, faces * 2))


# ---------------------------------------------------------------------------
# PREVIEW -- the plan first, because a MAP is a thing you walk about IN and the
# one view that says whether the layout is right is the one from above.
# ---------------------------------------------------------------------------
try:
    # -- AND IT IS DONE ON THE INDEX GRID, NOT A COLOUR ONE ------------------
    #
    # This used to build an (NX, NY, NZ, 3) uint8 volume and read the first hit
    # out of THAT. At the arcade's present size that array is SEVEN GIGABYTES,
    # on top of the 2.5 GB index grid and the 2.5 GB occupancy it is being
    # built beside -- so the last thing the tool did, after a successful
    # write, was try to allocate more than everything else it had used put
    # together. Collapsing the index grid first and colouring the flat 2D
    # result costs three bytes a PIXEL instead of three a CELL.
    tiles = []
    for axis, flip, name in ((1, True, 'plan'), (2, False, 'front'), (0, False, 'left')):
        occ = np.moveaxis(solid, axis, 0)
        idxv = np.moveaxis(idxgrid, axis, 0)
        if flip:
            occ, idxv = occ[::-1], idxv[::-1]
        first = np.argmax(occ, axis=0)
        flat = np.take_along_axis(idxv, first[None, :, :], 0)[0]
        img = np.zeros(flat.shape + (3,), dtype=np.uint8)
        hit = flat > 0
        img[hit] = PAL[flat[hit] - 1]
        img[~occ.any(axis=0)] = (18, 18, 26)
        # Collapsing an axis leaves the other two in their original order:
        # axis 1 (plan) leaves (x, z) -> transpose, so z runs up the page and
        # -z is north; axis 2 leaves (x, height) -> transpose, height up; axis
        # 0 leaves (height, z), which is ALREADY rows-are-height and must not
        # be transposed or the map stands on its end. The row flip is what puts
        # the top of the image at the top.
        tiles.append((name, (img if axis == 0 else np.transpose(img, (1, 0, 2)))[::-1]))
    plan = tiles[0][1]
    W = max(plan.shape[1], max(t.shape[1] for _, t in tiles[1:])) + 8
    H = plan.shape[0] + 16 + sum(t.shape[0] + 8 for _, t in tiles[1:])
    sheet = Image.new('RGB', (W, H), (0, 0, 0))
    sheet.paste(Image.fromarray(plan), (0, 4))
    y = plan.shape[0] + 12
    for _n, im in tiles[1:]:
        sheet.paste(Image.fromarray(im), (0, y))
        y += im.shape[0] + 8
    prev = os.path.splitext(OUT)[0] + '_preview.png'
    sheet.save(prev)
    print('         preview %s   (plan, then front and left elevations)' % prev)
except Exception as exc:            # a preview must never fail the asset
    print('         preview skipped: %s' % exc)
