"""Graft the pink flower onto the tips of every cactus and rewrite the nine .vox files.

Reads:  tools/cactus_flower.json                  (the FLOWER, hand-authored by the user, extracted once)
        game/assets/decoration/cacti.json         (the nine original bodies, as baked from cactus.glb)
Writes: game/assets/foilage/cactus/cactus_1..9.vox

THE FLOWER IS NOT READ FROM cactus_1.vox, AND THAT IS THE WHOLE POINT. It was, and the tool also
WRITES cactus_1.vox — so a second run read the already-flowered cactus back as "the flower" and
stamped an entire plant onto every tip. The models went 1398 -> 4160 voxels and doubled in height.
An input a tool overwrites is not an input. The flower now lives in its own file that nothing
writes, so this is idempotent: run it as many times as you like.

WHY IT READS THE BODIES FROM THE .json AND NOT FROM THE .vox FILES: cactus_1.vox no longer holds
a cactus. It is 5x5x2 with 10 voxels - the flower alone - so whatever produced it dropped the
plant. The .json still carries all nine original bodies, so that is the trustworthy source for
them, and the .vox file is the trustworthy source for the flower.

DO NOT RUN tools/voxelize_cacti.py AFTER THIS. It rebuilds the same nine .vox files from the .glb
and would silently throw the flowers away. If the plants ever need re-baking, run that first and
this second.

TIPS, not tops: a saguaro has several arms and each one should carry a flower, so the placement
walks the model's height map and keeps every LOCAL maximum (a column at least as high as anything
within radius 2), then merges maxima that sit within 3 voxels of each other so one arm gets one
flower rather than a cluster of them.
"""
import json, os, struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VOXDIR = os.path.join(ROOT, 'game', 'assets', 'foilage', 'cactus')
JSON = os.path.join(ROOT, 'game', 'assets', 'decoration', 'cacti.json')
BARE = {8, 9}          # these two carry NO flowers (user: "have 2 cactus's not have any flowers")
TIP_R = 2              # a tip must be the highest column within this radius
MERGE_R = 3            # maxima closer than this are the same arm
# ── NOT EVERY TIP (user 2026-08-15: "halve the rate... just some of them") ── a flower on every arm of every
# plant read as decoration applied by a machine. Roughly half the tips get one now, chosen by a hash of the
# tip's own position so the choice is STABLE: re-running this tool picks the same arms, and two cacti built
# from the same model still differ because their tip coordinates differ.
FLOWER_RATE = 0.5


def tip_takes_flower(n, tx, ty, tz):
    h = (n * 73856093) ^ (tx * 19349663) ^ (ty * 83492791) ^ (tz * 2971215073)
    return ((h ^ (h >> 13)) & 0xffff) / 65536.0 < FLOWER_RATE


def parse_vox(path):
    d = open(path, 'rb').read()
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
    return sizes[0], xyzis[0], pal[0]


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    size = chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
    body = b''.join(struct.pack('<BBBB', x, y, z, ci) for (x, y, z, ci) in vox)
    xyzi = chunk(b'XYZI', struct.pack('<I', len(vox)) + body)
    rgba = b''
    for i in range(256):
        c = pal[i] if i < len(pal) else (0, 0, 0)
        rgba += struct.pack('<BBBB', c[0], c[1], c[2], 255)
    out = size + xyzi + chunk(b'RGBA', rgba)
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', out))


# ── the flower, from its own file ──
FL = json.load(open(os.path.join(ROOT, 'tools', 'cactus_flower.json')))
fvox = [(v[0], v[1], v[2], tuple(v[3])) for v in FL]   # (x, y, z, rgb)
fsz = (max(v[0] for v in fvox) + 1, max(v[1] for v in fvox) + 1, max(v[2] for v in fvox) + 1)
fcols = sorted({v[3] for v in fvox})
print('flower: %dx%dx%d, %d voxels, %d colours' % (fsz[0], fsz[1], fsz[2], len(fvox), len(fcols)))
fcx, fcy = fsz[0] // 2, fsz[1] // 2                    # flower origin -> its own centre, so it sits ON the tip

# ── the bodies, from the bake ──
J = json.load(open(JSON))
body_pal = [tuple(c) for c in J['pal']]
merged = list(body_pal) + [c for c in fcols if c not in body_pal]
print('palette: %d body shades + %d flower shades = %d' % (len(body_pal), len(merged) - len(body_pal), len(merged)))
idx = {c: i + 1 for i, c in enumerate(merged)}         # MagicaVoxel colour indices are 1-based

by_name = {m['name']: m for m in J['cacti']}
total_f = 0
for n in range(1, 10):
    m = by_name['cactus_%d' % n]
    sx, sy, sz = m['sx'], m['sy'], m['sz']
    vox = {}
    for p in m['vox']:
        x, y, z, ci = p & 255, (p >> 8) & 255, (p >> 16) & 255, (p >> 24) & 255
        vox[(x, y, z)] = idx[body_pal[ci]]

    flowers = 0
    if n not in BARE:
        top = {}                                       # (x,y) -> highest z with a voxel
        for (x, y, z) in vox:
            if top.get((x, y), -1) < z:
                top[(x, y)] = z
        peaks = []
        for (x, y), z in top.items():
            hi = True
            for dx in range(-TIP_R, TIP_R + 1):
                for dy in range(-TIP_R, TIP_R + 1):
                    if top.get((x + dx, y + dy), -1) > z:
                        hi = False
                        break
                if not hi:
                    break
            if hi:
                peaks.append((x, y, z))
        peaks.sort(key=lambda p: -p[2])
        tips = []
        for (x, y, z) in peaks:                        # one flower per arm, not one per column
            if all((x - tx) ** 2 + (y - ty) ** 2 >= MERGE_R * MERGE_R for (tx, ty, _) in tips):
                tips.append((x, y, z))
        for (tx, ty, tz) in tips:
            if not tip_takes_flower(n, tx, ty, tz):
                continue
            for (fx, fy, fz, frgb) in fvox:
                gx, gy, gz = tx + fx - fcx, ty + fy - fcy, tz + 1 + fz
                if gx < 0 or gy < 0 or gx > 254 or gy > 254 or gz > 254:
                    continue
                vox[(gx, gy, gz)] = idx[frgb]
                sx = max(sx, gx + 1); sy = max(sy, gy + 1); sz = max(sz, gz + 1)
            flowers += 1
        total_f += flowers

    out = [(x, y, z, ci) for (x, y, z), ci in sorted(vox.items())]
    write_vox(os.path.join(VOXDIR, 'cactus_%d.vox' % n), sx, sy, sz, out, merged)
    print('  cactus_%d  %2d x %2d x %2d  %4d voxels  %s'
          % (n, sx, sy, sz, len(out), ('%d flower(s)' % flowers) if flowers else 'BARE'))

print('%d flowers (about half the tips); cactus_%s left bare' % (total_f, ' and cactus_'.join(str(b) for b in sorted(BARE))))
