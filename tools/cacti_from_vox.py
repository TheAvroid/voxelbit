"""Re-bake game/assets/decoration/cacti.json FROM the nine hand-edited cactus .vox files.

    Reads:  game/assets/foilage/cactus/cactus_1..9.vox   (the plants, as the user edits them)
            tools/cactus_flower.json                     (the flower, only to know its shades)
    Writes: game/assets/decoration/cacti.json            (the nine BODIES, flowers stripped)

    plain `python tools/cacti_from_vox.py`     (stdlib only, no arguments)

WHAT THIS IS FOR — AND WHAT IT IS *NOT* FOR
-------------------------------------------
It is NOT how cactus edits reach the game. The game stopped reading cacti.json: assets/bow.js
fetches the nine .vox directly and parses them with parseVoxModel(share, noTol), so a MagicaVoxel
edit is live on the next reload with no bake step at all. Verified in-game 2026-08-16 — every
voxel the user deleted was gone from the world and every colour matched the file exactly.

What this tool fixes is a FOOTGUN sitting one directory over. tools/cactus_flowers.py reads the
BODIES out of cacti.json and WRITES the nine .vox. That was fine while the .json was the source of
truth, but the user now edits the .vox, so cacti.json is stale — and re-running cactus_flowers.py
would rebuild the nine files from the stale bodies and silently revert every hand edit (182 voxels
as of 2026-08-16). Running this first puts the edits back into the .json, so the flower tool can be
re-run without losing them. That is the whole job.

Order, if you ever need the full chain:
    tools/voxelize_cacti.py    .glb -> .json   REBUILDS FROM SOURCE ART — destroys hand edits
    tools/cacti_from_vox.py    .vox -> .json   (this file) — preserves hand edits
    tools/cactus_flowers.py    .json -> .vox   re-grafts the flowers

DO NOT RUN voxelize_cacti.py AFTER THIS. It rebuilds cacti.json from cactus.glb and throws away
everything the user drew. If the plants ever need re-baking from the source art, run it FIRST and
this one second — never the other way round.

FLOWERS ARE STRIPPED, DELIBERATELY. cactus_flowers.py's contract is that cacti.json holds the bare
bodies; it grafts a flower onto each chosen tip. Feeding it a .json that already contained flowers
would stack flowers on flowers — the same class of bug its own docstring documents, where an output
was used as an input. So every voxel wearing one of the flower's six shades (read from
tools/cactus_flower.json, never hardcoded) is dropped here. Plants 8 and 9 are bare by design and
lose nothing. If a plant is ever deliberately painted in flower pink, this would eat it — the run
prints the per-plant flower count so that is visible rather than silent.

SCHEMA — byte-for-byte the shape voxelize_cacti.py emitted, because bow.js's loader and
cactus_flowers.py both still parse it:
    { "pal":   [[r,g,b], ...],                     one shared palette for all nine plants
      "cacti": [ { "name": "cactus_N",
                   "sx": w, "sy": d, "sz": h,      grid extents, z is UP (MagicaVoxel convention)
                   "vox": [ x | y<<8 | z<<16 | palIndex<<24, ... ] } ... ] }
The models are ordered by grid volume and the palette is sorted by (r,g,b), so a re-run over
unchanged art produces an identical file — a diff means the art really changed.
"""
import json, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOXDIR = os.path.join(ROOT, 'game', 'assets', 'foilage', 'cactus')
FLOWER = os.path.join(ROOT, 'tools', 'cactus_flower.json')
OUT = os.path.join(ROOT, 'game', 'assets', 'decoration', 'cacti.json')
N = 9


def read_vox(path):
    """SIZE + XYZI + RGBA out of a .vox, whichever way MagicaVoxel saved it.

    The nine files are a MIX: the ones cactus_flowers.py wrote are bare version-150 files with
    nothing but SIZE/XYZI/RGBA, while the ones the user re-saved from MagicaVoxel are version 200
    and carry a whole scene graph (nTRN/nGRP/nSHP/LAYR/MATL/rOBJ/rCAM/NOTE) around the same three
    chunks. Walking MAIN's children and taking the FIRST SIZE/XYZI reads both, and is exactly what
    the game's own parseVoxModel does — so this tool and the runtime can never disagree about what
    a file contains. Each plant is a single model; the scene graph's transforms are not applied.
    """
    b = open(path, 'rb').read()
    if b[:4] != b'VOX ':
        raise SystemExit('%s is not a .vox file' % path)
    box = {'size': None, 'xyzi': None, 'pal': None}

    def walk(off, end):
        while off < end:
            cid = b[off:off + 4]
            bsz, csz = struct.unpack_from('<II', b, off + 4)
            if cid == b'SIZE' and box['size'] is None:
                box['size'] = struct.unpack_from('<III', b, off + 12)
            elif cid == b'XYZI' and box['xyzi'] is None:
                n = struct.unpack_from('<I', b, off + 12)[0]
                box['xyzi'] = b[off + 16:off + 16 + n * 4]
            elif cid == b'RGBA':
                box['pal'] = b[off + 12:off + 12 + 1024]
            elif cid == b'MAIN':
                walk(off + 12 + bsz, off + 12 + bsz + csz)
                off += 12 + bsz + csz
                continue
            off += 12 + bsz + csz

    walk(8, len(b))
    if box['xyzi'] is None or box['pal'] is None:
        raise SystemExit('%s has no XYZI/RGBA chunk' % path)
    x = box['xyzi']
    # MagicaVoxel colour indices are 1-based into the 256-entry RGBA table.
    vox = [(x[i], x[i + 1], x[i + 2],
            (box['pal'][(x[i + 3] - 1) * 4], box['pal'][(x[i + 3] - 1) * 4 + 1], box['pal'][(x[i + 3] - 1) * 4 + 2]))
           for i in range(0, len(x), 4)]
    return box['size'], vox


flower_cols = {tuple(v[3]) for v in json.load(open(FLOWER))}
print('flower shades (stripped): %d  %s' % (len(flower_cols), sorted(flower_cols)))

plants, body_cols = [], set()
for n in range(1, N + 1):
    size, vox = read_vox(os.path.join(VOXDIR, 'cactus_%d.vox' % n))
    body = [v for v in vox if v[3] not in flower_cols]
    nf = len(vox) - len(body)
    if not body:
        raise SystemExit('cactus_%d has no body voxels left after stripping flowers' % n)
    # Extents come from the BODY, not from the file's SIZE chunk: cactus_flowers.py grows sx/sy/sz
    # to fit the petals it adds, so the saved SIZE describes the flowered plant. Re-deriving from
    # the body is what makes this round-trip — bake, graft, re-bake and the numbers come back.
    sx = max(v[0] for v in body) + 1
    sy = max(v[1] for v in body) + 1
    sz = max(v[2] for v in body) + 1
    body_cols.update(v[3] for v in body)
    plants.append({'n': n, 'sx': sx, 'sy': sy, 'sz': sz, 'body': body, 'nf': nf, 'file': size})
    print('  cactus_%d  %2d x %2d x %2d  %4d body voxels  %s'
          % (n, sx, sy, sz, len(body), ('%d flower voxels stripped' % nf) if nf else 'BARE'))

pal = sorted(body_cols)                                    # (r,g,b) order — stable across runs
idx = {c: i for i, c in enumerate(pal)}
if len(pal) > 256:
    raise SystemExit('%d body shades — a .vox palette index only has 8 bits' % len(pal))

out = []
for p in plants:
    # x | y<<8 | z<<16 | palIndex<<24, sorted so the array itself is diffable.
    packed = sorted(v[0] | (v[1] << 8) | (v[2] << 16) | (idx[v[3]] << 24) for v in p['body'])
    out.append({'name': 'cactus_%d' % p['n'], 'sx': p['sx'], 'sy': p['sy'], 'sz': p['sz'], 'vox': packed})
out.sort(key=lambda r: r['sx'] * r['sy'] * r['sz'])        # same ordering voxelize_cacti.py used

s = json.dumps({'pal': [list(c) for c in pal], 'cacti': out})
open(OUT, 'w').write(s)
print('\npalette: %d body shades  %s' % (len(pal), pal))
print('wrote %s (%.0f KB), %d plants, %d voxels'
      % (OUT, len(s) / 1024, len(out), sum(len(r['vox']) for r in out)))
