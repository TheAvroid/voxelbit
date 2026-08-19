"""Bake the asset editor's per-frame offsets INTO the .vox frames they describe.

    python tools/bake_offsets.py game/assets/life/flamingo offsets.json
    python tools/bake_offsets.py game/assets/life/flamingo --inline '{"flamingo":[{"frame":0,...}]}'

WHAT THIS IS FOR. The editor's export button hands you a table like

    {"flamingo":[{"frame":0,"name":"00.vox","ox":2,"oy":0,"oz":0}, ...]}

and until now the only way to use it was to paste it into a *_BAKE constant in src/ui/editor.js. That works
for a GRID-STAMPED creature, because buildArmPoses reads the bake and the world stamps from those poses. It
does NOT work for a TRACE-INJECTED one: the flamingo's world frames come from parseBunny reading the raw .vox
into the item table, which never sees a bake. So the editor showed the tuned bird and the world showed the
untuned one, and the two could not be made to agree by editing code alone (user 2026-08-18: "make sure the
flamingo in the cherry forest matches the one in the asset editor").

Writing the offsets into the ART is what makes them agree, permanently and for every render path at once:
after this there is nothing left to apply, so there is nothing left to apply INCONSISTENTLY.

THE AXIS MAP, WHICH IS THE ONE THING TO GET RIGHT. The editor names its axes for the STAGE, the .vox names
them for the model, and they are not the same three letters:

    editor ox  ->  vox x     (stage left/right)
    editor oz  ->  vox y     (stage fore/aft — NOT vox z)
    editor oy  ->  vox z     (height; the editor's hopY)

Read ui/editor.js edLayout: `xBase = bx + ox`, `zBase = bz + oz`, and oy feeds hopY. bx/bz index the model's
sx/sy, and height is the model's z.

AFTERWARDS, CLEAR THE CODE-SIDE BAKE. The offsets now live in the file, so a *_BAKE constant still holding
them would apply them a SECOND time. Same for the editor's own localStorage under its offKey — bump the key
so a stale saved copy is orphaned rather than re-added on top.

Reversible: the source models are still in the folder's base.vox, so a bad bake is undone by re-splitting with
tools/split_vox_frames.py and running this again with the corrected numbers.

Stdlib only; run with the plain `python` on PATH.
"""
import glob, json, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_vox(path):
    b = open(path, 'rb').read()
    size, vox, pal = None, [], None

    def walk(off, end):
        nonlocal size, pal
        while off < end - 12:
            cid = b[off:off + 4].decode('latin1')
            n1, csz = struct.unpack_from('<II', b, off + 4)
            if cid == 'SIZE':
                size = struct.unpack_from('<III', b, off + 12)
            elif cid == 'XYZI':
                n = struct.unpack_from('<I', b, off + 12)[0]
                vox.extend(tuple(b[off + 16 + i * 4: off + 20 + i * 4]) for i in range(n))
            elif cid == 'RGBA':
                pal = b[off + 12: off + 12 + 1024]
            elif cid == 'MAIN':
                walk(off + 12 + n1, off + 12 + n1 + csz)
                off += 12 + n1 + csz
                continue
            off += 12 + n1 + csz

    walk(8, len(b))
    return size, vox, pal


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    body = (chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
            + chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
            + (chunk(b'RGBA', pal) if pal else b''))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    if not os.path.isabs(d):
        d = os.path.join(ROOT, d)

    if sys.argv[2] == '--inline':
        data = json.loads(sys.argv[3])
    else:
        p = sys.argv[2] if os.path.isabs(sys.argv[2]) else os.path.join(ROOT, sys.argv[2])
        data = json.load(open(p, encoding='utf-8'))

    # The export is {"<lane name>": [frames]}. One lane is the normal case; take the first if unnamed.
    frames_in = data if isinstance(data, list) else data[sorted(data)[0]]
    off = {f['name']: (int(f.get('ox', 0)), int(f.get('oy', 0)), int(f.get('oz', 0))) for f in frames_in}

    files = sorted(glob.glob(os.path.join(d, '[0-9]*.vox')))
    if not files:
        raise SystemExit('no NN.vox frames in %s' % d)

    shifted = []
    for f in files:
        name = os.path.basename(f)
        size, vox, pal = read_vox(f)
        if not vox:
            raise SystemExit('%s has no voxels' % f)
        ox, oy, oz = off.get(name, (0, 0, 0))
        # editor ox -> vox x, editor oz -> vox y, editor oy -> vox z. See the axis-map note in the header.
        shifted.append({'f': f, 'name': name, 'd': (ox, oz, oy), 'sy0': size[1],
                        'vox': [(v[0] + ox, v[1] + oz, v[2] + oy, v[3]) for v in vox], 'pal': pal})

    # ── ONE SHARED GRID, SOLVED RATHER THAN NORMALISED ── every frame must end on the SAME grid, because the
    # runtime places a model bottom-centre (`bx = wx - (fw >> 1)`, stampModel) and the trace path centres on the
    # model's own w/d/h too. What the world sees is therefore `voxel - (depth >> 1)`, and THAT is the quantity a
    # bake must preserve, plus the requested offset. Nothing else about the grid is meaningful.
    #
    # THE BUG THIS REPLACES, TWICE OVER (2026-08-18). The first version re-based by subtracting the minimum
    # corner. That does two wrong things at once:
    #   * a frame whose offset was ZERO still moved, because its depth changed and `depth >> 1` with it —
    #     measured: frames 5 and 9 went 12 deep to 15 deep and shifted a voxel each with an offset of 0,0,0;
    #   * and the whole SET slid, because the minimum is taken across all frames: one frame pushed to y = 0 by
    #     its own -2 dragged the other nine along with it. Measured: every frame moved +2 more than asked, so
    #     the relative animation was right and the entire bird sat two voxels out of place.
    # Both are the same mistake — treating raw voxel coordinates as the thing to normalise, when the anchored
    # coordinate is the thing that matters.
    #
    # THE SOLVE. For a candidate depth `sy`, each frame's shift is forced: s = (sy >> 1) - (its own depth >> 1),
    # which is exactly what holds `voxel - (depth >> 1)` constant. So the only free variable is `sy`. Walk it
    # upward from the largest input depth and take the first that keeps every voxel inside [0, sy). That is the
    # smallest grid on which the bake is exact — no normalising, no drift, and a zero offset is provably a no-op.
    lox = min(min(v[0] for v in fr['vox']) for fr in shifted)
    hix = max(max(v[0] for v in fr['vox']) for fr in shifted)
    loz = min(min(v[2] for v in fr['vox']) for fr in shifted)
    hiz = max(max(v[2] for v in fr['vox']) for fr in shifted)
    sx, sz = hix - lox + 1, hiz - loz + 1

    sy = None
    for cand in range(max(fr['sy0'] for fr in shifted), 256):
        ok = True
        for fr in shifted:
            sft = (cand >> 1) - (fr['sy0'] >> 1)
            for v in fr['vox']:
                if not (0 <= v[1] + sft < cand):
                    ok = False
                    break
            if not ok:
                break
        if ok:
            sy = cand
            break
    if sy is None or max(sx, sy, sz) > 255:
        raise SystemExit('no shared grid under the .vox 255 limit fits this bake')

    print('%-8s %-12s %s' % ('frame', 'shift x,y,z', 'voxels'))
    for fr in shifted:
        sft = (sy >> 1) - (fr['sy0'] >> 1)            # forced by the solve above: this is what holds `voxel - (depth >> 1)` constant for this frame
        out = [(v[0] - lox, v[1] + sft, v[2] - loz, v[3]) for v in fr['vox']]
        assert all(0 <= q[0] < sx and 0 <= q[1] < sy and 0 <= q[2] < sz for q in out), 'rebase left the grid'
        write_vox(fr['f'], sx, sy, sz, out, fr['pal'])
        print('%-8s %-12s %d' % (fr['name'][:-4], '%+d,%+d,%+d' % fr['d'], len(out)))
    print('%d frames baked onto a shared %dx%dx%d grid — now CLEAR the code-side *_BAKE for this model'
          % (len(shifted), sx, sy, sz))


main()
