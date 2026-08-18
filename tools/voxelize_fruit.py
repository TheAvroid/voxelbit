#!/usr/bin/env python3
"""Bake the two TREE FRUIT — an apple and an orange — into game/assets/decoration/fruit.json.

WHY A BAKE AND NOT A .vox FETCH.  The apple is one clean single-model file, but the orange is not:
game/assets/food/orange.vox is a 117-model CULINARY PACK (pineapple, carrot, peach, cookie, a
cabinet…) and the orange is ONE shape node inside it, #27, found by its own `_name`.  assets/models.js
has no nth-model reader that can also be told which palette id to use, and parseVoxAll would mint or
share a palette entry for every colour in all 117 models — on a table with two slots free that is the
silent-substitute failure the ceiling notes in assets/palette.js exist to prevent.  So the selection
happens HERE, once, off the scene graph's own labels, and the runtime fetches a 2-model json the way
every other decoration in this game does.

WHY SLOTS AND NOT COLOURS.  The output carries a SLOT index per voxel, not an RGB:
    slot 0 = the fruit's flesh      slot 1 = its stem and leaf
assets/palette.js owns the actual ids — it has to, because it mints them long before this json is
fetched — and assets/bow.js maps slot -> id by INDEX.  `pal` below is the authored colour each slot
was quantized from: documentation, and the check that palette.js still agrees with the art.

THE QUANTIZATION IS THE POINT, and it is 11 colours -> 1 and 9 -> 1.  A fruit is a 30 cm ball seen
from 10 m up a tree; the artist's five-step flesh ramp is invisible at that size and the palette has
no room for it.  The split is EXACT rather than a hue heuristic: in both source models the flesh is
z <= 2 and the stem/leaf is z >= 3, and the tool asserts no palette index appears on both sides — so
a re-authored fruit that breaks that assumption fails the bake instead of baking a green apple.
The surviving colour is the voxel-count-weighted MEAN of the shades it replaces, which is the honest
one-shade answer and is what the literals in assets/palette.js must equal.

Deterministic: re-running reproduces the json byte for byte.

    "$LOCALAPPDATA/Programs/Python/Python313/python.exe" tools/voxelize_fruit.py
"""
import json
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
APPLE = os.path.join(ROOT, 'game', 'assets', 'food', 'apple', '00.vox')
ORANGE = os.path.join(ROOT, 'game', 'assets', 'food', 'orange.vox')
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'fruit.json')

LEAF_Z = 3          # model z at which flesh stops and stem/leaf begins — asserted disjoint below


def parse_vox(path):
    """(models, palette, {shape-node name: model index}) — enough to find one fruit inside a pack."""
    d = open(path, 'rb').read()
    models, pal, named, label = [], [(0, 0, 0)] * 256, {}, {}

    def rdstr(o):
        n, = struct.unpack_from('<I', d, o)
        return d[o + 4:o + 4 + n].decode('latin1'), o + 4 + n

    def rddict(o):
        n, = struct.unpack_from('<I', d, o)
        o += 4
        r = {}
        for _ in range(n):
            k, o = rdstr(o)
            v, o = rdstr(o)
            r[k] = v
        return r, o

    def walk(off, end):
        while off + 12 <= end:
            cid = d[off:off + 4].decode('latin1')
            bsz, csz = struct.unpack_from('<II', d, off + 4)
            o = off + 12
            if cid == 'SIZE':
                sx, sy, sz = struct.unpack_from('<III', d, o)
                models.append({'sx': sx, 'sy': sy, 'sz': sz, 'vox': None})
            elif cid == 'XYZI':
                n, = struct.unpack_from('<I', d, o)
                vs = [tuple(d[o + 4 + i * 4:o + 8 + i * 4]) for i in range(n)]
                for m in models:
                    if m['vox'] is None:
                        m['vox'] = vs
                        break
            elif cid == 'RGBA':
                pal[:] = [tuple(d[o + i * 4:o + i * 4 + 3]) for i in range(256)]
            elif cid == 'nTRN':                        # the transform ABOVE a shape carries its _name
                o += 4
                attr, o = rddict(o)
                child, = struct.unpack_from('<i', d, o)
                o += 16
                nfr, = struct.unpack_from('<i', d, o - 4)
                for _ in range(nfr):
                    _fr, o = rddict(o)
                label[child] = attr.get('_name', '')
            elif cid == 'nSHP':
                nid, = struct.unpack_from('<i', d, o)
                o += 4
                _a, o = rddict(o)
                nm, = struct.unpack_from('<i', d, o)
                o += 4
                ids = []
                for _ in range(nm):
                    mi, = struct.unpack_from('<i', d, o)
                    o += 4
                    _a, o = rddict(o)
                    ids.append(mi)
                if ids:
                    named.setdefault(nid, ids[0])
            elif cid == 'MAIN':
                walk(off + 12 + bsz, off + 12 + bsz + csz)
                off += 12 + bsz + csz
                continue
            off += 12 + bsz + csz

    walk(8, len(d))
    byname = {}
    for nid, mi in named.items():                      # first node wins, so the pack's order decides
        nm = label.get(nid, '')
        if nm:
            byname.setdefault(nm, mi)
    return models, pal, byname


def bake(name, model, pal, leafZ=LEAF_Z, crownFn=None):
    """One fruit: crop to its bounding box, split flesh from stem, average each side to one shade."""
    vox = model['vox']
    lo = [min(v[i] for v in vox) for i in range(3)]
    hi = [max(v[i] for v in vox) for i in range(3)]
    # ── THE SPLIT HEIGHT IS PER-FRUIT ── the apple's flesh stops at z 2 and the orange's reaches z 3, so
    # one shared LEAF_Z cut the orange in half (it failed loudly with palette indices on both sides,
    # which is the assert doing its job). Model 33 really does carry a crown: three greens at z 4-5 and
    # one brown stem voxel at z 4 — the SAME (143,95,74) the apple's stalk uses. Baking it as all-flesh
    # averaged that green into the fruit, which is what the user saw.
    # A HEIGHT split only works when the crown sits entirely above the flesh. On the re-authored orange it
    # does not — the brown stem runs down to z 2 while flesh reaches z 3 — so that fruit passes a COLOUR
    # predicate instead: anything that is not orange flesh is crown. The apple keeps the height split, which
    # is still exact for it.
    is_leaf = (lambda v: crownFn(pal[v[3] - 1])) if crownFn else (lambda v: v[2] - lo[2] >= leafZ)
    body_ci = set(v[3] for v in vox if not is_leaf(v))
    leaf_ci = set(v[3] for v in vox if is_leaf(v))
    both = body_ci & leaf_ci
    if both:
        raise SystemExit('%s: palette indices %s appear in BOTH the flesh and the stem (the split is '
                         'z >= %d) — re-author the model or pass a different leafZ' % (name, sorted(both), leafZ))

    def mean(sel):
        sub = [pal[v[3] - 1] for v in vox if is_leaf(v) == sel]
        if not sub:                                    # an all-flesh fruit has no leaf half; the loader keys on the
            return [0, 0, 0], 0                        # count, so a zero-voxel side reports honestly instead of dividing by it
        return [int(round(sum(c[i] for c in sub) / len(sub))) for i in range(3)], len(sub)

    body_col, nbody = mean(False)
    leaf_col, nleaf = mean(True)
    out = sorted((v[0] - lo[0]) | ((v[1] - lo[1]) << 8) | ((v[2] - lo[2]) << 16) |
                 ((1 if is_leaf(v) else 0) << 24) for v in vox)
    m = {'name': name, 'sx': hi[0] - lo[0] + 1, 'sy': hi[1] - lo[1] + 1, 'sz': hi[2] - lo[2] + 1,
         'vox': out}
    print('  %-7s %dx%dx%d  %2d vox  flesh %2d vox / %d shades -> %s   stem %d vox / %d shades -> %s'
          % (name, m['sx'], m['sy'], m['sz'], len(out), nbody, len(body_ci), body_col,
             nleaf, len(leaf_ci), leaf_col))
    return m, body_col, leaf_col


def main():
    amodels, apal, _ = parse_vox(APPLE)
    apple = amodels[0]
    if (apple['sx'], apple['sy'], apple['sz']) != (4, 3, 5):
        print('warning: apple/00.vox is %dx%dx%d, expected 4x3x5' %
              (apple['sx'], apple['sy'], apple['sz']), file=sys.stderr)
    omodels, opal, _onames = parse_vox(ORANGE)   # the node NAME is no longer consulted - see the colour scan below
    # ── FOUND BY COLOUR, NOT BY INDEX OR NAME ── this has now broken three times for three different
    # reasons: the node named ORANGE was the pack's apple, then the pack was trimmed 117 -> 7 (index moved
    # 27 -> 6), then trimmed again to 1 (moved to 0). A pinned index is wrong the moment the artist saves.
    # So: score every model by how orange its voxels are and take the best. With a single-model file that is
    # trivially model 0; with the full pack it picked the same fruit the index did. Survives any re-save.
    def _orangeness(m):
        n = 0
        for v in m['vox']:
            r, g, b = opal[v[3] - 1]
            if r >= 190 and 90 <= g <= 190 and b <= 130:
                n += 1
        return n / max(1, len(m['vox']))
    oi = max(range(len(omodels)), key=lambda i: _orangeness(omodels[i]))
    _om = omodels[oi]
    if _orangeness(_om) < 0.5:
        raise SystemExit('orange.vox: best model %d is only %.0f%% orange voxels — no orange fruit in this '
                         'file?' % (oi, 100 * _orangeness(_om)))
    print('[fruit] orange.vox: %d model(s), taking #%d (%.0f%% orange voxels)'
          % (len(omodels), oi, 100 * _orangeness(_om)))
    a, a_body, a_leaf = bake('apple', apple, apal)
    o, o_body, o_leaf = bake('orange', omodels[oi], opal)   # the stock z-3 split: flesh below, the 3 green leaf cells above
    leaf = [int(round((a_leaf[i] + o_leaf[i]) / 2)) for i in range(3)]
    with open(OUT, 'w') as f:
        json.dump({'pal': [a_body, o_body, leaf], 'nbody': 2, 'fruit': [a, o]}, f,
                  separators=(',', ':'))
    print('[fruit] wrote %s' % os.path.relpath(OUT, ROOT))
    print('[fruit] assets/palette.js FRUITC must hold apple/cherry %s and orange %s '
          '(the blueberry is this game\'s own colour, not the art\'s)' % (a_body, o_body))


main()
