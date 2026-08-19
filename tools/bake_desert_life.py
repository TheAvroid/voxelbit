"""Composite the desert-BAND creatures' ANIMATIONS into flat per-frame .vox files.

Input:  game/assets/life/<name>.vox              (authored, scene-graph animation)
Output: game/assets/life/<name>/00.vox, 01.vox…  (one file per frame, flat, one model each)

WHY THIS EXISTS: only ant.vox and fly.vox are "one model per animation frame". The other five
animate by KEYFRAMING TRANSFORMS across several objects - cobra is 19 separate body segments
each with 11-12 translation keyframes, scorpion is 6 parts, desert_mouse 5, spider 2, and gecko
splits body from tongue. Reading the models in file order (what tools/voxelize_desert.py does)
therefore yields body PARTS sitting at the origin, not frames: cobra's "frames" come out as
two-voxel blobs. game/assets/life/desert7.json is built on that mistake and must not be used.

So the scene graph is walked once per frame here, offline, and each frame is flattened into a
single model. That leaves the runtime with numbered per-frame files, which is the shape every
existing animated creature already uses (the fish/flight loaders in assets/held-items.js walk
`<dir>/00.vox`, `01.vox`, … until one is missing), so the engine needs no new parsing at all.

MagicaVoxel scene graph, as used here:
  nTRN  transform: one child, plus a frame list; each frame may carry `_t` "x y z" and `_r`
  nGRP  group: many children
  nSHP  shape: a frame list, each carrying `_mi` (model index)
Keyframes are SPARSE and HOLD: a node with frames at _f 0,1,2,4 has no 3, and frame 3 keeps
frame 2's pose. Both node types are resolved with the same "latest keyframe at or before f" rule.

A model's voxels are 0..size-1 and its nTRN translation places the model's CENTRE, so the world
position of a voxel is  translation + voxel - floor(size / 2).

Stdlib only; run with the plain `python` on PATH.
"""
import json, os, struct, sys
from collections import OrderedDict

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LIFE = os.path.join(ROOT, 'game', 'assets', 'life')
# APPEND-ONLY. The load order here IS the slot-band order at runtime (species index = position in
# DESERTS, and main/tick-life.js distributes the desert head-count Bresenham-style down that index), so
# re-ordering this list silently re-assigns every band. bee + grass_snake join at the END for that reason,
# even though neither lives in the desert - they ride the desert BAND's machinery, not its biome.
NAMES = ['ant', 'cobra', 'desert_mouse', 'fly', 'gecko', 'scorpion', 'spider', 'bee', 'grass_snake']
# ── AND THE SPECIES THAT ARE NOT IN THE DESERT BAND AT ALL (user 2026-08-18) ── the BETTA is a fish: it does
# not ride the desert band's slots, it has no entry in DESERTS, and it must NOT be appended to NAMES above,
# because that list's position IS the slot band and adding to it re-assigns nothing but still writes the
# species into desert_frames.json where the desert loader would then look for it.
# What it DOES need from this tool is the only thing this tool has: the scene-graph walk. betta.vox is five
# body PARTS on keyframed transforms — 1x1x6, 1x1x4, 1x1x2, 3x2x2 at 4/4/2/6 voxels — so reading its models in
# file order (what tools/split_vox_frames.py does, and what the fish loader would get from flat frames) yields
# body parts sitting at the origin, exactly the failure this file's header describes for the cobra.
# So it is baked here and excluded from the manifest: same walk, same flattening, output in the same
# <name>/00.vox shape the fish loader in assets/held-items.js already reads.
EXTRA = ['betta']
# ── PER-SPECIES FRAME WINDOW ── the gecko's scene runs to 67 frames only because its TONGUE keyframes sit at
# _f 54-66; the body itself has 8. Baking all 67 spent 67 item-table entries on an animation that is a long
# hold plus one flick. Capped at 7 (user 2026-08-15), which is the body loop, and 60 item slots come back.
# A value may be an int (frames 0..n-1, the gecko's original meaning, unchanged) or a (start, count) PAIR,
# because the same "long hold plus one flick" shape does not always put the loop at frame 0. The grass snake
# is the case that needed it: 121 authored frames, of which 0-13 are a wide LEAD-IN swing, 14-25 are the
# steady slither and repeat exactly (pose[14] == pose[26], and every consecutive delta inside the window is
# the same 20 voxels, so it loops seamlessly), and 47-63 are one tongue flick. Baking 0..11 would have
# spliced half the lead-in to half the loop and read as a hitch twice a second; (14, 12) is the loop itself,
# matches the cobra's 12 frames exactly, and drops the flick on the gecko's precedent. Widen to (14, 50) if
# the tongue is ever wanted back - it costs 38 more item-table entries.
# grass_snake WAS (14, 12) — a window into a 121-frame source whose first 14 frames were a lead-in. The
# user re-authored the .vox down to exactly the 12 loop frames on 2026-08-17, so the window is now not
# only unnecessary but WRONG: 14..25 reads past the end of a 12-model file. A plain cap of 12 takes the
# file as authored, and re-trimming the art again cannot silently resurrect the old offset.
FRAME_CAP = {'gecko': 7, 'grass_snake': 12}


def read_dict(d, o):
    """MagicaVoxel DICT: int32 n, then n pairs of (int32 len, bytes)."""
    n = struct.unpack_from('<i', d, o)[0]
    o += 4
    out = {}
    for _ in range(n):
        kl = struct.unpack_from('<i', d, o)[0]; o += 4
        k = d[o:o + kl].decode('utf-8', 'replace'); o += kl
        vl = struct.unpack_from('<i', d, o)[0]; o += 4
        v = d[o:o + vl].decode('utf-8', 'replace'); o += vl
        out[k] = v
    return out, o


def parse(path):
    d = open(path, 'rb').read()
    models, pal, nodes = [], None, {}
    sizes = []

    def walk(o, end):
        nonlocal pal
        while o < end - 12:
            cid = d[o:o + 4]
            csz, ksz = struct.unpack_from('<II', d, o + 4)
            b = o + 12
            if cid == b'SIZE':
                sizes.append(struct.unpack_from('<III', d, b))
            elif cid == b'XYZI':
                n = struct.unpack_from('<I', d, b)[0]
                models.append([tuple(d[b + 4 + i * 4: b + 8 + i * 4]) for i in range(n)])
            elif cid == b'RGBA':
                pal = [tuple(d[b + i * 4: b + i * 4 + 3]) for i in range(256)]
            elif cid == b'nTRN':
                nid = struct.unpack_from('<i', d, b)[0]; p = b + 4
                _, p = read_dict(d, p)
                child, _res, _lay, nfr = struct.unpack_from('<iiii', d, p); p += 16
                frames = []
                for _ in range(nfr):
                    fd, p = read_dict(d, p)
                    frames.append(fd)
                nodes[nid] = ('TRN', child, frames)
            elif cid == b'nGRP':
                nid = struct.unpack_from('<i', d, b)[0]; p = b + 4
                _, p = read_dict(d, p)
                nch = struct.unpack_from('<i', d, p)[0]; p += 4
                kids = list(struct.unpack_from('<' + 'i' * nch, d, p)) if nch else []
                nodes[nid] = ('GRP', kids, None)
            elif cid == b'nSHP':
                nid = struct.unpack_from('<i', d, b)[0]; p = b + 4
                _, p = read_dict(d, p)
                nmo = struct.unpack_from('<i', d, p)[0]; p += 4
                mods = []
                for _ in range(nmo):
                    mi = struct.unpack_from('<i', d, p)[0]; p += 4
                    md, p = read_dict(d, p)
                    md['_mi'] = mi
                    mods.append(md)
                nodes[nid] = ('SHP', None, mods)
            if ksz:
                walk(b + csz, b + csz + ksz)
            o = b + csz + ksz

    walk(8, len(d))
    return sizes, models, pal, nodes


def at_frame(frames, f):
    """The latest keyframe at or before f. Sparse keyframes HOLD the previous pose."""
    best = None
    for fr in frames:
        ff = int(fr.get('_f', 0))
        if ff <= f and (best is None or ff >= int(best.get('_f', 0))):
            best = fr
    return best if best is not None else (frames[0] if frames else {})


def n_frames(nodes):
    hi = 0
    for kind, _c, fr in nodes.values():
        if fr:
            for x in fr:
                hi = max(hi, int(x.get('_f', 0)))
    return hi + 1


def compose(sizes, models, nodes, f):
    """-> [(x, y, z, colour_index)] in world space for animation frame f."""
    out = []
    warned = []

    def rec(nid, tx, ty, tz):
        kind, child, frames = nodes[nid]
        if kind == 'TRN':
            fr = at_frame(frames, f)
            if '_r' in fr and fr['_r'] not in ('0', '4', '', None):
                warned.append(fr['_r'])
            t = fr.get('_t')
            if t:
                a, b2, c = (int(v) for v in t.split())
                tx, ty, tz = tx + a, ty + b2, tz + c
            rec(child, tx, ty, tz)
        elif kind == 'GRP':
            for k in child:
                rec(k, tx, ty, tz)
        else:
            md = at_frame(frames, f)
            mi = int(md.get('_mi', 0))
            if mi >= len(models):
                return
            sx, sy, sz = sizes[mi]
            ox, oy, oz = tx - sx // 2, ty - sy // 2, tz - sz // 2
            for (vx, vy, vz, ci) in models[mi]:
                if ci >= 1:
                    out.append((vx + ox, vy + oy, vz + oz, ci))

    rec(0, 0, 0, 0)
    return out, warned


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body = b''.join(struct.pack('<BBBB', x, y, z, ci) for (x, y, z, ci) in vox)
    xyzi = chunk(b'XYZI', struct.pack('<I', len(vox)) + body)
    rgba = b''
    for i in range(256):
        c = pal[i] if pal and i < len(pal) else (0, 0, 0)
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150)
                           + chunk(b'MAIN', b'', size + xyzi + chunk(b'RGBA', rgba)))


def main():
    # ── THE FRAME MANIFEST ── written beside the frames so the RUNTIME knows how many exist. Without it the
    # loader prefetched a blind 20 per species and ate 82 wasted 404 round-trips every boot (58 frames really
    # exist across the seven). It is emitted HERE, by the tool that writes the frames, so the two cannot drift.
    manifest = {}
    print('%-14s %7s %8s %10s %s' % ('creature', 'frames', 'voxels', 'grid', 'note'))
    for name in NAMES + EXTRA:
        src = os.path.join(LIFE, name + '.vox')
        if not os.path.exists(src):
            print('%-14s MISSING' % name); continue
        sizes, models, pal, nodes = parse(src)
        nf = n_frames(nodes)
        win = FRAME_CAP.get(name, nf)
        f0, nf = (win[0], min(win[1], max(0, nf - win[0]))) if isinstance(win, tuple) else (0, min(win, nf))
        comp, warn = [], []
        for f in range(f0, f0 + nf):
            v, w = compose(sizes, models, nodes, f)
            comp.append(v); warn += w
        # ONE grid for every frame, sized to the union — a per-frame bbox would make the creature
        # jump around its own origin as the animation played.
        allv = [p for fr in comp for p in fr]
        if not allv:
            print('%-14s no voxels' % name); continue
        mnx = min(p[0] for p in allv); mny = min(p[1] for p in allv); mnz = min(p[2] for p in allv)
        mxx = max(p[0] for p in allv); mxy = max(p[1] for p in allv); mxz = max(p[2] for p in allv)
        sx, sy, sz = mxx - mnx + 1, mxy - mny + 1, mxz - mnz + 1
        outdir = os.path.join(LIFE, name)
        os.makedirs(outdir, exist_ok=True)
        for old in os.listdir(outdir):                 # a shorter animation must not leave stale frames behind
            if old.endswith('.vox') and old[:-4].isdigit():
                os.remove(os.path.join(outdir, old))
        counts = []
        for f, v in enumerate(comp):
            vv = sorted({(x - mnx, y - mny, z - mnz): ci for (x, y, z, ci) in v}.items())
            flat = [(k[0], k[1], k[2], ci) for k, ci in vv]
            counts.append(len(flat))
            write_vox(os.path.join(outdir, '%02d.vox' % f), sx, sy, sz, flat, pal)
        note = 'rotated keyframes ignored (%s)' % set(warn) if warn else ''
        print('%-14s %7d %8s %10s %s' % (name, nf, '%d-%d' % (min(counts), max(counts)),
                                         '%dx%dx%d' % (sx, sy, sz), note))
        if name not in EXTRA:
            manifest[name] = nf                        # EXTRA species are not desert-band life, so they stay out of desert_frames.json — that file is read by the desert loader to size ITS bands
    json.dump(manifest, open(os.path.join(LIFE, 'desert_frames.json'), 'w'))
    print('wrote desert_frames.json:', manifest)


main()
