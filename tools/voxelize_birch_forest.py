"""Bake ALL the birch trees in source/fbx/birch_trees to one shared-palette .json, oak-style.

Input:  source/fbx/birch_trees/Birch_forest_<H>mFbx.rar   x26  (gitignored authoring originals)
        source/fbx/textures/birch/*.jpg                        (from Maps.rar - see voxelize_birch.py)
Output: game/assets/decoration/birch_trees.json

RUN IT WITH THE WINDOWS PYTHON, not the msys2 one on PATH - numpy and Pillow live there:
  "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_birch_forest.py
It takes roughly an hour: 26 Blender round-trips over 26 MB - 90 MB of .fbx each. --only=17m bakes
one tree by name, which is the way to iterate on the classifier without paying for the set - it writes
birch_trees.only.json and CANNOT touch the shipped file, for the reason recorded at the --only branch.

Every voxelizing decision here is voxelize_birch.py's, imported rather than copied - the object
split, the MINSUB alpha floor, the area contest, the heartwood fill. Read that file's header first;
this one only records what is different about doing TWENTY-SIX of them at once.

-- HOW MANY TREES THERE ACTUALLY ARE: 26, NOT 27 ---------------------------------------------
The set is sold as 27 and the folder holds 76 archives, so this is worth writing down once:
    26 x Birch_forest_<H>mFbx.rar    one .fbx each (verified by listing every archive) <- THESE
    26 x Birch_forest_<H>m_Std.rar   the SAME 26 trees, standard-material .fbx, identical name set
    23 x Birch_forest_part_NN(...)   .max scenes only - 12 parts x Corona/V-ray, part_02 Corona only
     1 x Maps.rar                    the shared texture library
The 27th tree is part_06, at 22.8 m - a height that appears in NO Fbx archive. It exists only as a
3ds Max scene, and Blender cannot read .max, so it is unobtainable here. 26 is what can be baked.

-- THE OBJECT NAMES CARRY A PER-TREE PREFIX --------------------------------------------------
voxelize_birch.py keys its object table on exact names because it has exactly one tree. Across the
set the prefix is the tree's own id and it changes every file - the same leaf card is `6_Betula_
leaf_0101` in the 24.8 m tree, `8_...` in the 26.4 m, `17_...` in the 17 m, `22_...` in the 12.1 m.
So the split is by PATTERN on the name with the prefix stripped, and the stripped name IS the
texture's name. Meshes per tree vary (7-8) and so does which trunk sheet each one wears.

-- ONE SHARED PALETTE FOR THE WHOLE FOREST ---------------------------------------------------
Exactly like oak_trees.json: quantize ONCE over the pooled colours of all 26 trees rather than per
tree, so the forest is one ramp and costs the 256-entry table one set of ids instead of 26. NBARK/
NLEAF match the oak file's shape (3 + 4 = 7) so the two forests are directly comparable.

-- THE PACKING NEEDED A WIDER HEIGHT FIELD ---------------------------------------------------
oak_trees.json packs a voxel as x | z<<8 | y<<16 | colour<<24 - eight bits per axis, so 255 is the
tallest model it can express. Oaks top out at 114 and never noticed. BIRCHES REACH 264 (26.4 m at
10 cm), which silently wraps in that layout. So this file gives height NINE bits and the colour
seven, which is still one positive int32 and still the same field ORDER:

    x | depth<<8 | height<<16 | colour<<25          height 0..511, colour 0..127

Anything reading birch_trees.json must use those shifts, not oak_trees.json's.

-- AND THE VOXEL LIST IS DELTA-VARINT + BASE64, NOT A JSON ARRAY -----------------------------
26 trees is 1,194,089 voxels, and as a JSON array of decimal integers that is 11.6 MB - against
oak_trees.json's 1.97 MB. That is real boot time and real repo weight, so the list ships encoded:

    sort ascending (it already is)  ->  delta  ->  LEB128 varint  ->  base64

MEASURED on this set: 697,850 deltas fit in one byte and 468,636 in two, so the stream is 1.72 MB
and the base64 of it 2.29 MB. That is 5.1x smaller than the array, and it PARSES faster too - a
base64 -> Uint8Array -> cumulative-sum walk beats JSON.parse over a 1.2M-element number array.
Plain int32 + base64 was the obvious alternative and is only 6.37 MB, because it spends four bytes
on every voxel where the deltas mostly need one.
Sorting matters and is not arbitrary: colour sits in the HIGH bits, so ascending order groups by
colour, then height, then depth, then x - and consecutive x in a row differ by 1, which is the
whole reason the deltas are small. Do not re-sort by position.

THE READER, in JS:

    const b = atob(t.vox); let v = 0, sh = 0, acc = 0, out = new Int32Array(t.n), k = 0;
    for (let i = 0; i < b.length; i++) { const c = b.charCodeAt(i);
      v |= (c & 127) << sh;
      if (c & 128) { sh += 7; } else { acc += v; out[k++] = acc; v = 0; sh = 0; } }

`enc` names the codec so a future change is detectable; `n` is the voxel count, for sizing the
array and for asserting the decode.
"""
import glob, json, os, re, shutil, struct, subprocess, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import voxelize_birch as vb                            # the single-tree tool, imported as a library

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'source', 'fbx', 'birch_trees')
TEXD = os.path.join(ROOT, 'source', 'fbx', 'textures', 'birch')
WORK = os.path.join(SRC, '_work')                      # extraction + .obj scratch, emptied per tree
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'birch_trees.json')
UNRAR = os.environ.get('UNRAR', r'C:\Program Files\WinRAR\UnRAR.exe')

# -- WHY WOOD HAS TO WIN BY A MARGIN, NOT BY A HAIR (user 2026-08-23: "I see black or dark grey voxels on the
# leaves that shouldnt be there", "the tops of the trees seem to be cut off") -- both complaints are the same
# defect. A plain wgW >= wgL contest hands a voxel to whichever material has marginally more opaque area, and
# inside a birch crown that is constantly the TWIG NETWORK: Birch_bark05 is 637k triangles of dark brown
# (sampled [56,49,39]) threaded through the whole canopy, so a crown baked as ~25% bark reads as a dark twiggy
# skeleton with green flecks rather than as foliage - and on the tall models, whose crowns are narrow to begin
# with, that skeleton reads as a tree whose top has been cut off.
# A BIAS, NOT A PRIORITY FLIP: leaf-wins-outright would eat the bole wherever a card grazes it, which is the
# thing the contest exists to prevent. The trunk carries orders of magnitude more area than any leaf card, so
# it is untouched by a factor of 3; what changes is the borderline voxel out in the crown, which is exactly
# where a twig should lose to the leaves growing off it.
WOOD_BIAS = 8.0
BARK_DARK = (84, 82, 78)                             # the lighter grey the darkest bark shade is forced to; see the note at the cut
BARK_MID  = (57, 54, 47)                             # ...and the MIDDLE shade, 716b5e -> 39362f (user 2026-08-23). Note this is DARKER than BARK_DARK: the two swap rank, which is intended and is why both are assigned from the ORIGINAL luma order below
NBARK, NLEAF = 3, 4                                    # the oak file's shape, so the two forests compare
ONLY = None
REENCODE = False
for _a in sys.argv[1:]:
    if _a.startswith('--only='):
        ONLY = _a[7:]
    elif _a.startswith('--nbark='):
        NBARK = int(_a[8:])
    elif _a.startswith('--nleaf='):
        NLEAF = int(_a[8:])
    elif _a == '--own-leaf':
        pass                                           # handled at the palette step, see the repoint note
    elif _a == '--reencode':
        REENCODE = True
    else:
        sys.exit('unknown argument %s' % _a)

vb.TEXD = TEXD                                         # tex_of() reads this


VOXDIR = os.path.join(ROOT, 'game', 'assets', 'foilage', 'birch_trees')
VOXMAX = 256                                           # MagicaVoxel stores a coordinate in ONE BYTE - write_vox splits on it
TALL_SCALE = 0.91                                      # HALF the 1.82 that shipped: 26.4 m -> 24.0 m. See the note in build()


def _chunk(cid, content, children=b''):
    return cid + struct.pack('<II', len(content), len(children)) + content + children


def _s(t):
    b = t.encode('utf-8')
    return struct.pack('<I', len(b)) + b


def _dict(d):
    out = struct.pack('<I', len(d))
    for k, v in d.items():
        out += _s(k) + _s(v)
    return out


def write_vox(path, m, pal):
    """One MagicaVoxel 150 file. Axes go out in the order they are already in: a .vox is z-up and so is the
    packed model. Carries the SHARED forest palette, so every tree indexes the same seven colours.

    -- AND IT SPLITS, BECAUSE A .vox COORDINATE IS ONE BYTE --------------------------------------------
    No model may exceed 256 on any axis. The birches are now scaled well past that (see TALL_SCALE), so a
    tree is written as a STACK of parts, each <= VOXMAX tall, plus the nTRN/nGRP/nSHP scene graph that tells
    MagicaVoxel where to put them. Open one and it assembles into the whole tree; the game reads the same
    translations back (assets/bow.js) rather than assuming an order, so moving a part in the editor moves it
    in the world too.
    A tree short enough to fit in one model is still written as a single plain model - no scene graph, no
    behaviour change, byte-identical in shape to what shipped before."""
    parts = []
    for z0 in range(0, m['sz'], VOXMAX):
        pz = [q for q in m['packed'] if z0 <= ((q >> 16) & 511) < z0 + VOXMAX]
        if pz:
            parts.append((z0, min(VOXMAX, m['sz'] - z0), pz))
    body = b''
    for z0, szp, pz in parts:
        xyzi = b''.join(struct.pack('<BBBB', q & 255, (q >> 8) & 255, (((q >> 16) & 511) - z0), (q >> 25) + 1)
                        for q in pz)
        body += _chunk(b'SIZE', struct.pack('<III', m['sx'], m['sy'], szp))
        body += _chunk(b'XYZI', struct.pack('<I', len(pz)) + xyzi)
    if len(parts) > 1:
        # root nTRN(0) -> nGRP(1) -> [ nTRN(2+2i) -> nSHP(3+2i) ] per part. MagicaVoxel places a model by its
        # CENTRE, so the translation is the centre of that part's box in tree-local space.
        body += _chunk(b'nTRN', struct.pack('<i', 0) + _dict({}) + struct.pack('<iiii', 1, -1, -1, 1) + _dict({}))
        kids = b''.join(struct.pack('<i', 2 + 2 * i) for i in range(len(parts)))
        body += _chunk(b'nGRP', struct.pack('<i', 1) + _dict({}) + struct.pack('<I', len(parts)) + kids)
        for i, (z0, szp, pz) in enumerate(parts):
            t = '%d %d %d' % (0, 0, z0 + szp // 2 - m['sz'] // 2)
            body += _chunk(b'nTRN', struct.pack('<i', 2 + 2 * i) + _dict({}) +
                           struct.pack('<iiii', 3 + 2 * i, -1, 0, 1) + _dict({'_t': t}))
            body += _chunk(b'nSHP', struct.pack('<i', 3 + 2 * i) + _dict({}) +
                           struct.pack('<I', 1) + struct.pack('<i', i) + _dict({}))
    rgba = b''
    for i in range(256):
        c = pal[i] if i < len(pal) else (0, 0, 0)
        rgba += struct.pack('<BBBB', int(c[0]), int(c[1]), int(c[2]), 255)
    body += _chunk(b'RGBA', rgba)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + _chunk(b'MAIN', b'', body))
    return len(parts)


def enc_vox(sorted_ints):
    """ascending ints -> base64 of LEB128-varint DELTAS. See the header for the JS decoder."""
    out = bytearray()
    prev = 0
    for q in sorted_ints:
        d = q - prev
        prev = q
        while d >= 128:
            out.append((d & 127) | 128)
            d >>= 7
        out.append(d)
    import base64
    return base64.b64encode(bytes(out)).decode('ascii')


def reencode():
    """Rewrite an existing birch_trees.json into the encoded form, so changing the codec does not
    cost the hour the bake costs. Idempotent: a file already encoded is left alone."""
    d = json.load(open(OUT))
    if d.get('enc') == 'd64':
        print('already encoded'); return
    before = os.path.getsize(OUT)
    for t in d['trees']:
        t['n'] = len(t['vox'])
        t['vox'] = enc_vox(t['vox'])
    d['enc'] = 'd64'
    open(OUT, 'w').write(json.dumps(d, separators=(',', ':')))
    print('%s  %.2f MB -> %.2f MB  (%.1fx)' % (os.path.relpath(OUT, ROOT), before / 1e6,
                                               os.path.getsize(OUT) / 1e6, before / os.path.getsize(OUT)))


# -- WHICH TREES, AND HOW TALL ------------------------------------------------------------------
def tree_list():
    """-> [(label, height_m, rar_path)], sorted short to tall. The height in the FILENAME is the
    product's own spec and is what the model is scaled to; the mesh bbox agrees to a few cm
    (measured 12.1 -> 12.42, 17 -> 17.05, 26.4 -> 26.45, the excess being crown overhang)."""
    out = []
    for p in sorted(glob.glob(os.path.join(SRC, '*Fbx.rar'))):
        b = os.path.basename(p)
        m = re.match(r'Birch_forest_([0-9.]+)m(\(\d\))?Fbx\.rar$', b)
        if not m:
            print('  skipping unparsed archive name: %s' % b)
            continue
        h = float(m.group(1))
        label = m.group(1).replace('.', '_') + 'm' + (m.group(2)[1:-1] if m.group(2) else '')
        out.append(('birch_' + label, h, p))
    out.sort(key=lambda t: t[1])
    return out


# -- WHICH MATERIAL AN OBJECT IS, AND WHICH SHEET IT WEARS --------------------------------------
PREFIX = re.compile(r'^\d+_')


def is_twig(name):
    """The fine twig network - Birch_bark05, 637k triangles threaded through the whole canopy against
    the bole's 62k. It is wood and it stamps as wood; it is simply not allowed to set the bark ramp."""
    return 'bark05' in PREFIX.sub('', name).lower()


def classify(name):
    """object name -> ('w'|'l', diff jpg, opac jpg or None, reduce). Pattern, not a lookup table:
    the numeric prefix is the tree's id and changes per file (see the header)."""
    base = PREFIX.sub('', name)
    low = base.lower()
    if 'leaf' in low:
        # Betula_leaf_0101 -> its own recoloured diff, masked by the FAMILY's 2018 opacity sheet.
        # The family is the first two digits: 0101/01 -> 01, 0202 -> 02. See voxelize_birch.py.
        m = re.search(r'leaf_(\d{2})', low)
        fam = m.group(1) if m else '01'
        return ('l', base + '_diff.jpg', 'Betula_leaf_%s_opac.jpg' % fam, 1)
    # WOOD. The sheets are named inconsistently in the vendor library - some carry _diff, some do
    # not - so try the spellings that exist rather than assuming one. A Blend material names both
    # of its layers and the FBX does not carry the mask, so it bakes as its base sheet (see header).
    if low.startswith('blend_'):
        m = re.findall(r'bark_(\d+)', low)
        cand = ['Betula_bark_%s_diff.jpg' % m[-1]] if m else []
    else:
        cand = [base + '_diff.jpg', base + '.jpg']
        m = re.match(r'birch_trunk_0*(\d+)$', low)     # Birch_trunk_013 -> the library only has _13/_12/_10...
        if m:
            cand += ['Birch_trunk_%s.jpg' % m.group(1), 'Birch_trunk_%s_diff.jpg' % m.group(1),
                     'Birch_trunk_%02d.jpg' % int(m.group(1))]
    for c in cand:
        if os.path.exists(os.path.join(TEXD, c)):
            return ('w', c, None, 4 if 'trunk' in low else 1)
    print('      no sheet for %-42s -> Birch_trunk_0701_diff.jpg' % base)
    return ('w', 'Birch_trunk_0701_diff.jpg', None, 4)


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


def build(label, height_m, rar):
    """One tree, start to finish -> dict or None. Leaves WORK empty so 26 x ~130 MB of .fbx and
    .obj never coexist: at 3 GB of scratch this would be a disk problem rather than a bake."""
    if os.path.isdir(WORK):
        shutil.rmtree(WORK)
    os.makedirs(WORK)
    r = subprocess.run([UNRAR, 'e', '-o+', rar, WORK + os.sep], capture_output=True, text=True)
    fbx = [f for f in os.listdir(WORK) if f.lower().endswith('.fbx')]
    if not fbx:
        print('  %-16s UNRAR FAILED: %s' % (label, (r.stdout + r.stderr)[-200:]))
        return None
    fbx = os.path.join(WORK, fbx[0])
    obj = os.path.join(WORK, 'tree.obj')
    blender = vb.find_blender()
    if not blender:
        sys.exit('no Blender found - set $BLENDER to blender.exe')
    tmp = os.path.join(WORK, '_conv.py')
    open(tmp, 'w').write(CONVERT % (fbx, obj))
    subprocess.run([blender, '--background', '--factory-startup', '--python', tmp],
                   capture_output=True, text=True)
    if not os.path.exists(obj):
        print('  %-16s BLENDER FAILED' % label)
        return None
    os.remove(fbx)                                     # the .obj is all that is needed from here
    objs = vb.load_obj(obj)

    allv = np.concatenate([v[0].reshape(-1, 3) for v in objs.values()])
    tall = allv[:, 2].max() - allv[:, 2].min()
    # -- LARGER THAN LIFE, ON PURPOSE (user 2026-08-23: "make them much taller") -- the source trees are
    # 12.1 m to 26.4 m, which are their real heights and which the user finds too short. TALL_SCALE lifts the
    # whole set uniformly, so the RANGE of sizes is preserved and only the overall stature changes.
    # 0.91 is HALF of the 1.82 that shipped (user 2026-08-23: "reduce the trees in half ... scaled
    # porportionally"), so the set runs 11.0 m to 24.0 m. Two consequences worth knowing: every model is
    # then under the 256 .vox axis limit, so NONE of them split any more and each file is a single object
    # in MagicaVoxel again; and the tallest tree fits under WY 504, so core/gpu.js goes back to the FULL
    # 2048 window and the view radius returns to ~100 m.
    # (was 1.82, which put the tallest at 48.0 m against the 50.8 m the world could hold at WY 728, leaving
    # ~3 m of margin for the ground to be higher than the 218 that ceiling was measured against.
    # The 256 cap that used to SHRINK four trees is gone: write_vox splits instead of scaling down.
    scale = (height_m * TALL_SCALE) / tall
    org = allv.min(0) * scale
    dims = np.maximum(1, np.ceil((allv.max(0) * scale - org) / vb.VOX).astype(int) + 1)
    del allv

    wv, wc, ww, lv, lc, lw, rc, rw = [], [], [], [], [], [], [], []
    for nm in sorted(objs):
        kind, diff, opac, red = classify(nm)
        vb.OBJ_TEX[nm] = (kind, diff, opac, red)       # tex_of reads the table; feed it this tree's row
        tex = vb.tex_of(nm)
        vx, col, w = vb.raster(*objs[nm], tex, org, scale, vb.ALPHA_MIN if kind == 'l' else 0)
        vx = np.clip(vx, 0, dims - 1)
        (wv if kind == 'w' else lv).append(vx)
        (wc if kind == 'w' else lc).append(col)
        (ww if kind == 'w' else lw).append(w)
        # -- WHAT THE BARK RAMP IS ALLOWED TO SEE -- the TRUNK and the limbs, never the twig sheet. See the
        # note on pooled(): Birch_bark05 is dark (sampled [56,49,39]) and out-samples the bole several to
        # one, and a ramp fitted to that population puts its steps so low that most of the TRUNK lands on
        # the darkest of them. Measured on the shipped bake: 43% of all bark voxels wore [71,66,56] and only
        # 22% the lightest - a birch trunk that is 43% near-black, which is the opposite of a birch.
        # The twigs still GET a colour; they just do not get a vote on where the three steps sit, so they
        # fall on the dark end where a twig belongs instead of dragging the bole down with them.
        if kind == 'w' and not is_twig(nm):
            rc.append(col)
            rw.append(w)
    del objs
    if not wv or not lv:
        print('  %-16s no wood or no leaf geometry' % label)
        return None
    bvx, bcol, bw = vb.dedupe(np.concatenate(wv), np.concatenate(wc), np.concatenate(ww), dims)
    nvx, ncol, nw = vb.dedupe(np.concatenate(lv), np.concatenate(lc), np.concatenate(lw), dims)

    # THE AREA CONTEST, exactly as voxelize_birch.py argues it: neither class wins by priority, the
    # one with more opaque surface in the voxel does. "Wood wins" deletes a birch canopy.
    wgW, wgL = np.zeros(dims), np.zeros(dims)
    wgW[bvx[:, 0], bvx[:, 1], bvx[:, 2]] = bw
    wgL[nvx[:, 0], nvx[:, 1], nvx[:, 2]] = nw
    grid = np.zeros(dims, np.uint8)
    grid[(wgW > 0) & (wgW >= wgL * WOOD_BIAS)] = 1
    grid[(wgL > 0) & (wgL * WOOD_BIAS > wgW)] = 2
    kb = grid[bvx[:, 0], bvx[:, 1], bvx[:, 2]] == 1
    kn = grid[nvx[:, 0], nvx[:, 1], nvx[:, 2]] == 2
    bvx, bcol = bvx[kb], bcol[kb]
    nvx, ncol = nvx[kn], ncol[kn]
    del wgW, wgL

    # Trim to the occupied extent - mesh bounds carry transparent margin, and slack under a model
    # hangs it in the air when it is stamped on the ground.
    occ = np.argwhere(grid != 0)
    mn, mx = occ.min(0), occ.max(0)
    grid = grid[mn[0]:mx[0] + 1, mn[1]:mx[1] + 1, mn[2]:mx[2] + 1]
    bvx -= mn
    nvx -= mn
    sx, sy, sz = (int(q) for q in grid.shape)
    print('  %-16s %5.1f m -> %3d x %3d x %3d   wood %6d  leaf %6d' % (label, height_m, sx, sy, sz, len(bvx), len(nvx)))
    return dict(label=label, sx=sx, sy=sy, sz=sz, grid=grid, bw=bw[kb],
                rampc=np.concatenate(rc), rampw=np.concatenate(rw),
                bvx=bvx, bcol=bcol, nvx=nvx, ncol=ncol, src=os.path.basename(rar))


# -- RUN ----------------------------------------------------------------------------------------
if REENCODE:
    reencode()
    sys.exit(0)
trees = tree_list()
if ONLY:
    trees = [t for t in trees if ONLY in t[0] or ONLY in os.path.basename(t[2])]
    # -- --only NEVER WRITES THE SHIPPED FILE (2026-08-23) -- it used to, and it cost the user a session: three
    # --only=26_4m runs while tuning the wood bias left game/assets/decoration/birch_trees.json holding ONE tree,
    # the tallest and sparsest of the twenty-six, so every birch in the world became a 26 m trunk with its crown
    # 20 m overhead and the forest read as bare poles. A one-tree bake is a MEASUREMENT, not an asset; it goes
    # to a sidecar and the real file is left alone.
    OUT = OUT.replace('.json', '.only.json')
    print('--only: writing %s (the shipped birch_trees.json is left untouched)' % os.path.basename(OUT))
print('%d birch trees to bake, %.1f - %.1f m' % (len(trees), trees[0][1], trees[-1][1]) if trees else 'nothing to bake')
built = []
for i, (label, h, rar) in enumerate(trees):
    print('[%2d/%d] %s' % (i + 1, len(trees), label))
    try:
        m = build(label, h, rar)
    except Exception as e:
        print('  %-16s FAILED: %s' % (label, e))
        m = None
    if m:
        built.append(m)
if os.path.isdir(WORK):
    shutil.rmtree(WORK)
if not built:
    sys.exit('nothing baked')

# ONE palette for the whole forest - pooled, not per tree. Sampled because the pooled colour list is
# millions of rows and median_cut is O(n) per split; 200k is far more than 7 shades can resolve.
rng = np.random.default_rng(12345)                     # fixed seed: the ramp must not move between runs


def pooled(key, n, wkey=None):
    """The shared ramp for one material. With wkey, the sample is drawn in proportion to each voxel's
    opaque AREA rather than uniformly over voxels - which is the difference between a ramp that describes
    what the eye sees and one that describes what the mesh happens to be made of.

    IT MATTERS FOR THE BARK AND ONLY FOR THE BARK. Birch_bark05, the fine twig network, is 637k triangles
    against the trunk's 62k, so by VOXEL COUNT the twigs outvote the bole several to one and drag all three
    shades down onto their own dark brown (sampled [56,49,39]): the first bake's ramp came out
    [59,53,43] [104,100,89] [144,139,127], and everything wearing that darkest step read as black speckle
    through the canopy. By AREA the trunk dominates, which is also what dominates the picture."""
    allc = np.concatenate([m[key] for m in built])
    w = np.concatenate([m[wkey] for m in built]) if wkey else None
    if len(allc) > 200000:
        if w is not None:
            pr = np.maximum(w, 1e-9); pr = pr / pr.sum()
            pick = rng.choice(len(allc), 200000, replace=True, p=pr)
        else:
            pick = rng.choice(len(allc), 200000, replace=False)
        allc = allc[pick]
    return vb.median_cut(np.clip(allc, 0, 255), n)


cut_b, cut_n = pooled('rampc', NBARK, 'rampw'), pooled('ncol', NLEAF)
# ── THE DARKEST BARK IS LIFTED TO A GREY (user 2026-08-23: "make the darkest shade on the birch trees a
# lighter grey") ── the median cut puts it around (50,46,36), a near-black brown, and that is what speckles
# the canopy and bands the trunk. BARK_DARK replaces it outright rather than nudging the cut, because the
# cut is fitted to the SOURCE and the source really is that dark; this is an art decision on top of it.
# Replaced, not clamped, so the value is exactly what is asked for and readable in the .vox palette.
# KEEP IT CLEAR OF THE MIDDLE SHADE. The first try was (108,105,99) against a mid of (113,107,94) - five
# levels apart, so the ramp collapsed to two tones and the lash marks disappeared entirely. (84,82,78)
# sits ~28 luma below the mid and still reads as a mark rather than a hole.
# The lash marks still read - they are ~70 luma against the trunk's ~166 - but they no longer read as holes.
cut_b = np.asarray(cut_b, float)
# ── BOTH OVERRIDES COME OFF THE ORIGINAL LUMA ORDER ── argsort ONCE and index it, never argmin twice: the
# second override would otherwise re-rank a ramp the first one has already moved, and since BARK_MID is
# darker than BARK_DARK the "darkest" slot is exactly the one that changes hands. Ranks, not hexes, so the
# assignment still lands correctly when the fitted ramp shifts between bakes - the trap that made the first
# birch_deblack (which matched 322f24 by hex) silently convert nothing after a re-bake.
_ord = np.argsort(cut_b @ np.array([0.299, 0.587, 0.114]))
cut_b[_ord[0]] = BARK_DARK
cut_b[_ord[1]] = BARK_MID
# ── THE LEAVES WEAR THE OAK'S GREENS (user 2026-08-23: "make the foilage in the birch forest a lighter
# green. matching the leaves of the lighter green oak tree") ── the sampled birch leaf comes out dark
# (measured [63,77,43]..[86,100,58]) because the V-Ray sheets are lit for an overcast archviz render;
# the oak's ramp is [82,115,47]..[134,167,89], which is the lighter green being asked for.
# So the leaf shades are REPOINTED onto oak_trees.json's own leaf colours by relative lightness -
# darkest to darkest, lightest to lightest. Two things fall out of that beyond the colour:
#   * it is EXACTLY "matching the oak", not an eyeballed lift toward it, because the values ARE the
#     oak's values read out of its .json rather than retyped;
#   * it costs ZERO new palette ids. The table is at 252/256 (measured, __vb.palAudit) with FOUR free,
#     and the bark alone wants three of them - a birch that also minted four greens could not fit.
# The TEXTURE still decides which of the four a voxel gets, so the canopy keeps its own light/dark
# structure and only the ramp underneath it changes. --own-leaf bakes the sampled greens instead.
if '--own-leaf' not in sys.argv:
    oak = json.load(open(os.path.join(ROOT, 'game', 'assets', 'decoration', 'oak_trees.json')))
    oak_leaf = np.array(oak['pal'][oak['nbark']:], float)
    cut_n_pal = vb.repoint(cut_n, oak_leaf)
    print('leaf sampled %s\n     -> oak   %s' % ([[int(round(c)) for c in s] for s in cut_n],
                                                 [list(map(int, c)) for c in cut_n_pal]))
else:
    cut_n_pal = np.clip(np.round(cut_n), 0, 255).astype(int)
pal = [[int(round(c)) for c in s] for s in cut_b] + [list(map(int, c)) for c in cut_n_pal]
print('bark %s\nleaf %s' % (pal[:NBARK], pal[NBARK:]))

out_trees = []
for m in built:
    cidx = np.zeros((m['sx'], m['sy'], m['sz']), np.int32)
    for vx, col, base, shades in ((m['bvx'], m['bcol'], 0, cut_b), (m['nvx'], m['ncol'], NBARK, cut_n)):
        if not len(vx):
            continue
        d = np.abs(col[:, None, :] - shades[None, :, :].astype(float)).max(2)
        cidx[vx[:, 0], vx[:, 1], vx[:, 2]] = base + d.argmin(1)
    heart = (m['grid'] == 1) & (cidx == 0)             # heartwood the contest never coloured -> darkest bark
    dark = int(np.argmin(np.asarray(cut_b).sum(1)))
    cidx[heart] = dark
    xs, ys, zs = np.nonzero(m['grid'])
    cs = cidx[xs, ys, zs]
    # x | depth<<8 | height<<16 | colour<<25 -- NINE bits of height, see the header
    packed = sorted(int(x) | (int(y) << 8) | (int(z) << 16) | (int(c) << 25)
                    for x, y, z, c in zip(xs, ys, zs, cs))
    wood = int((cs < NBARK).sum())
    out_trees.append(dict(src=m['src'], sx=m['sx'], sy=m['sy'], sz=m['sz'],
                          wood=wood, n=len(packed), vox=enc_vox(packed), name=m['label']))
    m['packed'] = packed
    if not REENCODE and not ONLY:
        if not os.path.isdir(VOXDIR):
            os.makedirs(VOXDIR)
        nparts = write_vox(os.path.join(VOXDIR, m['label'] + '.vox'), m, pal)
        if nparts > 1:
            print('  %-16s written as %d stacked parts (%d voxels tall)' % (m['label'], nparts, m['sz']))
    print('  %-16s %3d x %3d x %3d  %6d voxels (%d wood)' % (m['label'], m['sx'], m['sy'], m['sz'], len(packed), wood))

out_trees.sort(key=lambda t: t['sz'])
doc = dict(pal=pal, nbark=NBARK, enc='d64', shift=dict(x=0, z=8, y=16, c=25), trees=out_trees)
open(OUT, 'w').write(json.dumps(doc, separators=(',', ':')))
if not REENCODE and not ONLY:
    print('wrote %d .vox to %s' % (len(out_trees), os.path.relpath(VOXDIR, ROOT)))
print('\n%s  %d trees, %d..%d voxels tall, %.1f MB'
      % (os.path.relpath(OUT, ROOT), len(out_trees), out_trees[0]['sz'], out_trees[-1]['sz'],
         os.path.getsize(OUT) / 1e6))
