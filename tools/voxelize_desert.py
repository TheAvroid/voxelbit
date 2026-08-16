"""Quantize the desert creature set to ONE shared palette and emit JSON.

Input:  game/assets/life/{ant,cobra,desert_mouse,gecko,scorpion,spider,fly}.vox
Output: game/assets/life/desert7.json
  { "pal": [[r,g,b],...], "models": [ {"name","frames":[{"sx","sy","sz","vox":[x|y<<8|z<<16|ci<<24,...]}]} ] }

These are multi-model .vox files - the models ARE the animation frames, same convention as the
bow strip and the cardinal's flight (see fetchVoxStrip / parseVoxAll). Every frame is read and
every frame feeds the quantizer: reading only the first one undercounts the set badly (39 of
the 69 authored colours) and would bake a palette that the later frames then miss.

Same shape and the same reason as rocks26 (tools/voxelize_rocks.py): 26 rocks ship as "12
shared quantized shades" because a model that mints one palette id per authored shade is what
fills a 256-entry table. Measured 2026-08-15, these seven models hold 69 distinct colours
between them and only 26 already exist in the live palette - so wiring them as authored costs
39 new ids against ZERO free (see __vb.palAudit()). Quantized to a shared ramp they cost ~11.

Pure Python on purpose: voxelize_rocks.py needs numpy/PIL for the .glb sculpt, but a .vox is
already voxels, and this repo's tools are expected to run on a bare interpreter (see cdp.py).

Axes pass straight through: MagicaVoxel is z-up and the engine's sparse model format is too
(x = width, y = depth, z = height), which is why models.js can map model z to world y directly.
"""
import json, os, struct, sys

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFE = os.path.join(_ROOT, 'game', 'assets', 'life')
OUT = os.path.join(LIFE, 'desert7.json')
NAMES = ['ant', 'cobra', 'desert_mouse', 'gecko', 'scorpion', 'spider', 'fly']
PALN = 32   # ── THE KNEE, measured ── not 12. rocks26 gets away with 12 because 26 rocks are ONE
            # material; these are seven species spanning green, orange, pink and grey, and 12 shades
            # crushes them (9.5% mean shift, one voxel 62/255 out). Past ~24 the curve flattens for a
            # reason worth knowing: the extra shades land close enough to colours the palette ALREADY
            # holds that palShare's tolerance path reuses them, so they cost nothing. Measured against
            # the live table at the shipped PAL_TOL=2, N=24 and N=32 both cost 22 new ids - and N=32
            # halves the error (2.6% mean, worst 23/255). Cheaper is not available; better is free.


def parse_vox(path):
    """-> [(sx, sy, sz, [(x, y, z, (r,g,b)), ...]), ...] - EVERY model in the file, in order."""
    d = open(path, 'rb').read()
    if d[:4] != b'VOX ':
        raise SystemExit('%s: not a MagicaVoxel file' % path)
    sizes, xyzis, pal = [], [], [None]

    def walk(o, end):
        while o < end - 12:
            cid = d[o:o + 4]
            sz, csz = struct.unpack_from('<II', d, o + 4)
            body = o + 12
            if cid == b'SIZE':
                sizes.append(struct.unpack_from('<III', d, body))
            elif cid == b'XYZI':
                n = struct.unpack_from('<I', d, body)[0]
                xyzis.append([tuple(d[body + 4 + i * 4: body + 8 + i * 4]) for i in range(n)])
            elif cid == b'RGBA':
                pal[0] = [tuple(d[body + i * 4: body + i * 4 + 3]) for i in range(256)]
            if csz:
                walk(body + sz, body + sz + csz)
            o = body + sz + csz

    walk(8, len(d))
    if not sizes or not xyzis:
        raise SystemExit('%s: no SIZE/XYZI chunk' % path)
    if pal[0] is None:
        raise SystemExit('%s: no RGBA chunk - re-export with its palette' % path)
    out = []
    for (sx, sy, sz), vs in zip(sizes, xyzis):
        out.append((sx, sy, sz, [(x, y, z, pal[0][ci - 1]) for (x, y, z, ci) in vs if ci >= 1]))
    return out


def median_cut(colors, n):
    """The same split-the-widest-box quantizer voxelize_rocks.py uses, without numpy.

    `colors` is one entry per VOXEL, not per distinct shade, so the cut follows where the
    voxels actually are - a shade covering 400 voxels pulls a box, a 3-voxel highlight does not.
    """
    boxes = [list(colors)]
    while len(boxes) < n:
        cand = [i for i, b in enumerate(boxes) if len(b) > 1]
        if not cand:
            break
        def spread(b):
            return max(max(c[a] for c in b) - min(c[a] for c in b) for a in range(3))
        bi = max(cand, key=lambda i: spread(boxes[i]))
        b = boxes[bi]
        ax = max(range(3), key=lambda a: max(c[a] for c in b) - min(c[a] for c in b))
        b = sorted(b, key=lambda c: c[ax])
        med = b[len(b) // 2][ax]
        lo = [c for c in b if c[ax] <= med]
        hi = [c for c in b if c[ax] > med]
        if not lo or not hi:
            lo, hi = b[:len(b) // 2], b[len(b) // 2:]
        boxes[bi:bi + 1] = [lo, hi]
    out = []
    for b in boxes:
        out.append(tuple(int(round(sum(c[a] for c in b) / len(b))) for a in range(3)))
    return out


def nearest(pal, c):
    return min(range(len(pal)), key=lambda i: sum((pal[i][a] - c[a]) ** 2 for a in range(3)))


def main():
    n = PALN
    for a in sys.argv[1:]:
        if a.startswith('--n='):
            n = int(a[4:])
    models, allcols = [], []
    for name in NAMES:
        p = os.path.join(LIFE, name + '.vox')
        if not os.path.exists(p):
            print('  %-14s MISSING - skipped' % name)
            continue
        frames = parse_vox(p)
        models.append((name, frames))
        for (_, _, _, vox) in frames:
            allcols.extend(v[3] for v in vox)

    pal = median_cut(allcols, n)
    print('shared palette (%d shades):' % len(pal))
    for c in pal:
        print('   ', list(c))

    out_models, worst, tot_shift, tot_vox = [], 0, 0, 0
    print()
    print('%-14s %6s %6s %8s %6s %9s %7s' % ('model', 'frames', 'voxels', 'authored', 'used', 'mean d', 'max d'))
    for (name, frames) in models:
        out_frames, shift, mx, nvox = [], 0, 0, 0
        used, authored = set(), set()
        for (sx, sy, sz, vox) in frames:
            packed = []
            for (x, y, z, c) in vox:
                ci = nearest(pal, c)
                used.add(ci)
                authored.add(c)
                dd = max(abs(pal[ci][a] - c[a]) for a in range(3))
                shift += dd
                mx = max(mx, dd)
                packed.append(x | (y << 8) | (z << 16) | (ci << 24))
            out_frames.append(dict(sx=sx, sy=sy, sz=sz, vox=sorted(packed)))
            nvox += len(vox)
        out_models.append(dict(name=name, frames=out_frames))
        worst = max(worst, mx)
        tot_shift += shift
        tot_vox += nvox
        print('%-14s %6d %6d %8d %6d %8.1f%% %6d' % (name, len(frames), nvox, len(authored),
                                                     len(used), (shift / nvox / 2.55) if nvox else 0, mx))
    out = dict(pal=[list(c) for c in pal], models=out_models)
    s = json.dumps(out, separators=(',', ':'))
    open(OUT, 'w').write(s)
    print('\nauthored distinct colours: %d  ->  shared palette: %d' % (len({c for c in allcols}), len(pal)))
    print('mean shift %.1f%% of full scale, worst single voxel %d/255' % (tot_shift / tot_vox / 2.55, worst))
    print('wrote %s (%.1f KB), %d models, %d voxels' % (OUT, len(s) / 1024, len(out_models), tot_vox))


main()
