"""Split a multi-model .vox into the numbered per-frame files the runtime loaders read.

    python tools/split_vox_frames.py game/assets/life/pink_bird/flight/base.vox

Input:  one .vox holding N models, ONE MODEL PER ANIMATION FRAME, in file order.
Output: 00.vox, 01.vox … beside it, one model each, sharing the source's palette.

WHY THIS AND NOT tools/bake_desert_life.py. That tool exists for the opposite case and says so in
its own header: the desert creatures animate by KEYFRAMING TRANSFORMS across several objects, so
their models are body PARTS and reading them in file order yields two-voxel blobs rather than
frames. It walks the scene graph to composite each frame. Nothing here needs that, and running it
on a file that is already one-model-per-frame would be strictly worse than this — it would flatten
seven complete birds into seven copies of the same thing.

The check that decides which tool a file wants is its model DIMENSIONS: pink_bird/flight/base.vox
carries 7 models all 5x6x7 at 28 voxels, which is a complete bird seven times over (and matches
cardinal/flight/00.vox exactly, 5x6x7 / 28). Body parts would come out as several different small
grids instead. This tool refuses a file whose models disagree wildly on size, so the two cases
cannot be confused by accident.

Stdlib only; run with the plain `python` on PATH.
"""
import os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_dict(b, off):
    """A VOX DICT: count, then count pairs of (len, bytes) key and value."""
    n = struct.unpack_from('<i', b, off)[0]; off += 4
    d = {}
    for _ in range(n):
        l = struct.unpack_from('<i', b, off)[0]; off += 4
        k = b[off:off + l].decode('latin1'); off += l
        l = struct.unpack_from('<i', b, off)[0]; off += 4
        d[k] = b[off:off + l].decode('latin1'); off += l
    return d, off


def parse_scene(b):
    """The ANIMATION order and the per-frame translations, out of the scene graph.

    This is the half of a .vox that the first version of this tool threw away, and throwing it away cost a
    whole day of wrong flamingo frames. MagicaVoxel stores a multi-frame model as ONE nSHP listing its model
    ids in ANIMATION order — which is NOT the order the XYZI chunks appear in the file — under an nTRN whose
    per-frame dicts carry `_t`, the translation that places each frame in the scene.

    Measured on game/assets/life/flamingo/base.vox: nSHP maps frames to model ids 1,2,3,4,0,5,6,7,8,9 (so the
    FIRST XYZI in the file is animation frame 4, not frame 0), and the `_t` y values run 0,0,0,0,0,0,-1,-2,-1,0
    relative to the first. Reading the file in order therefore produced a scrambled walk whose frames were also
    each mis-placed — and no amount of hand-tuned offsets afterwards could fix the order.

    Returns (order, trans) or (None, None) when the file has no animated shape, in which case the caller keeps
    the old file-order behaviour and its --order/--start escape hatches.
    """
    order, trans = None, None

    def walk(off, end):
        nonlocal order, trans
        while off < end - 12:
            cid = b[off:off + 4].decode('latin1')
            n1, csz = struct.unpack_from('<II', b, off + 4)
            body = off + 12
            if cid == 'MAIN':
                walk(body + n1, body + n1 + csz); off += 12 + n1 + csz; continue
            if cid == 'nSHP':
                o = body + 4
                _, o = read_dict(b, o)
                nm = struct.unpack_from('<i', b, o)[0]; o += 4
                ids = []
                for _ in range(nm):
                    ids.append(struct.unpack_from('<i', b, o)[0]); o += 4
                    _, o = read_dict(b, o)
                if nm > 1:
                    order = ids
            elif cid == 'nTRN':
                o = body + 4
                _, o = read_dict(b, o)
                o += 12                                    # child id, reserved, layer id
                nf = struct.unpack_from('<i', b, o)[0]; o += 4
                fr = []
                for _ in range(nf):
                    fd, o = read_dict(b, o)
                    fr.append(tuple(int(v) for v in fd.get('_t', '0 0 0').split()))
                if nf > 1:
                    trans = fr
            off += 12 + n1 + csz

    walk(8, len(b))
    return order, trans


def parse(path):
    """Every SIZE/XYZI pair in file order, plus the palette. The scene graph is deliberately
    ignored — see the header: a file this tool accepts places each frame at the origin already."""
    b = open(path, 'rb').read()
    assert b[:4] == b'VOX ', '%s is not a .vox' % path
    sizes, models, pal = [], [], None

    def walk(off, end):
        nonlocal pal
        while off < end - 12:
            cid = b[off:off + 4].decode('latin1')
            sz, csz = struct.unpack_from('<II', b, off + 4)
            if cid == 'SIZE':
                sizes.append(struct.unpack_from('<III', b, off + 12))
            elif cid == 'XYZI':
                n = struct.unpack_from('<I', b, off + 12)[0]
                models.append([tuple(b[off + 16 + i * 4: off + 20 + i * 4]) for i in range(n)])
            elif cid == 'RGBA':
                pal = b[off + 12: off + 12 + 1024]
            elif cid == 'MAIN':
                walk(off + 12 + sz, off + 12 + sz + csz)
                off += 12 + sz + csz
                continue
            off += 12 + sz + csz

    walk(8, len(b))
    return sizes, models, pal, b


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    xyzi = chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
    rgba = chunk(b'RGBA', pal) if pal else b''
    body = size + xyzi + rgba
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = sys.argv[1]
    if not os.path.isabs(src):
        src = os.path.join(ROOT, src)
    sizes, models, pal, raw = parse(src)
    if not models:
        raise SystemExit('%s holds no models' % src)
    # ── THE GUARD THAT KEEPS THE TWO PIPELINES APART ── models that disagree wildly on size are
    # body parts, not frames, and belong in tools/bake_desert_life.py. Generous on purpose: a
    # genuine flap strip does vary a voxel or two as a wing extends.
    vol = [x * y * z for (x, y, z) in sizes]
    if len(models) > 1 and max(vol) > 4 * min(vol):
        raise SystemExit('models differ %dx in volume — this looks like a keyframed scene, not a '
                         'frame strip. Use tools/bake_desert_life.py.' % (max(vol) // max(1, min(vol))))

    # ── --start N: WHICH SCENE MODEL IS FRAME 0 ── MagicaVoxel's model order is not always the author's frame
    # order. pink_bird/rotate/base.vox came out with the LAST pose first, so every frame of the turn was one step
    # out of phase with the offsets in CARD_OFF and the bird rotated wrongly. The tell is the DIMENSION SEQUENCE:
    # a correct 11-frame songbird rotate reads 3x6,3x6,3x6,3x6,3x6,6x3,6x3,6x3,6x3,6x3,6x3 as it turns from
    # facing along z to facing along x, and this file read 6x3 first. Rotating the deal by one lines it up with
    # the cardinal exactly. Compare against an existing species before trusting a new strip.
    # --start=N rotates the whole deal so scene model N becomes frame 0.
    # --order=a,b,c,... is the explicit map when the scene is not a simple rotation, which is the case that
    # actually turned up: pink_bird/rotate/base.vox lines up with the cardinal on frames 5..10 already, while its
    # first FIVE are rotated by one among themselves, so no single --start value fixes it. Verified by DIMENSION
    # SEQUENCE against an existing species — a correct 11-frame songbird rotate reads
    #   3x6x7/28 3x6x7/31 3x6x6/28 3x6x6/25 3x6x7/28 | 6x3x7/28 6x3x8/31 6x3x7/31 6x3x6/28 6x3x6/25 6x3x7/28
    # and both the grid AND the voxel count have to match, frame for frame. Always diff against the cardinal
    # before trusting a new strip; guessing an offset off a bad measurement is how this comment got written.
    # ── THE SCENE GRAPH WINS, WHEN THERE IS ONE ── frame order and per-frame placement both come out of it, so
    # a correctly-authored animation needs no flags at all. --order/--start remain for files that have no
    # animated shape (a plain multi-model .vox), and an explicit --order still overrides this.
    scn_order, scn_trans = parse_scene(raw)
    if scn_order and len(scn_order) == len(models) and not any(a.startswith('--order=') for a in sys.argv[2:]):
        models = [models[i] for i in scn_order]
        sizes = [sizes[i] for i in scn_order]
        if scn_trans and len(scn_trans) == len(models):
            # ── PLACE EACH FRAME WHERE THE AUTHOR PLACED IT ── MagicaVoxel's `_t` translates the model's CENTRE,
            # so a voxel's scene position is `v + _t - size/2`. Tight-cropped frames of different sizes are
            # otherwise all flush to their own origin, which is what made a walking bird slide: the runtime
            # centres on the grid, so the crop decided the pose. Reconstructing the scene and writing every
            # frame on ONE shared grid means the animation the author saw is the animation that ships.
            pos = []
            for k, (m, sz, t) in enumerate(zip(models, sizes, scn_trans)):
                ox, oy, oz = t[0] - sz[0] // 2, t[1] - sz[1] // 2, t[2] - sz[2] // 2
                pos.append([(v[0] + ox, v[1] + oy, v[2] + oz, v[3]) for v in m])
            lo = [min(min(v[i] for v in m) for m in pos) for i in range(3)]
            hi = [max(max(v[i] for v in m) for m in pos) for i in range(3)]
            dim = tuple(hi[i] - lo[i] + 1 for i in range(3))
            models = [[(v[0] - lo[0], v[1] - lo[1], v[2] - lo[2], v[3]) for v in m] for m in pos]
            sizes = [dim] * len(models)
            print('scene graph: frame order %s, per-frame placement applied, shared %dx%dx%d grid'
                  % (','.join(map(str, scn_order)), dim[0], dim[1], dim[2]))
        else:
            print('scene graph: frame order %s (no per-frame translations)' % ','.join(map(str, scn_order)))

    start, order_arg = 0, None
    for a in sys.argv[2:]:
        if a.startswith('--start='): start = int(a.split('=')[1])
        elif a.startswith('--order='): order_arg = [int(v) for v in a.split('=')[1].split(',')]
    if order_arg is not None:
        if sorted(order_arg) != list(range(len(models))):
            raise SystemExit('--order must be a permutation of 0..%d' % (len(models) - 1))
        models = [models[i] for i in order_arg]
        sizes = [sizes[i] for i in order_arg]
    elif start:
        models, sizes = models[start:] + models[:start], sizes[start:] + sizes[:start]
    # ── WHERE THE FRAMES GO ── two input shapes exist in this tree and they want different answers:
    #   <dir>/flight/base.vox  -> beside it, because the loader reads <dir>/flight/00.vox
    #   <dir>/<name>.vox       -> into <dir>/<name>/, the convention tools/bake_desert_life.py already uses
    # Defaulting to the source's own directory for BOTH is what dropped ten 00.vox..09.vox files loose into
    # game/assets/life/ the first time a <name>.vox was passed. --out= overrides either way.
    outdir = None
    for a in sys.argv[2:]:
        if a.startswith('--out='):
            outdir = a.split('=', 1)[1]
            if not os.path.isabs(outdir): outdir = os.path.join(ROOT, outdir)
    if outdir is None:
        base = os.path.basename(src)
        outdir = os.path.dirname(src) if base == 'base.vox' else os.path.join(os.path.dirname(src), base[:-4])
    os.makedirs(outdir, exist_ok=True)
    for old in os.listdir(outdir):                     # a shorter animation must not leave stale frames behind
        if old.endswith('.vox') and old[:-4].isdigit():
            os.remove(os.path.join(outdir, old))
    for f, (dim, vox) in enumerate(zip(sizes, models)):
        write_vox(os.path.join(outdir, '%02d.vox' % f), dim[0], dim[1], dim[2], vox, pal)
    print('%s -> %d frames  %s  %s voxels' % (
        os.path.relpath(src, ROOT), len(models),
        'x'.join(map(str, sizes[0])), '-'.join({str(len(m)) for m in models})))


main()
