"""Turn the BLACK twig voxels inside a birch canopy into leaves, in place, across all 26 .vox files.

    "monitor for this color: 322f24. if you detect nearby light green voxels (leafs) then turn those
     voxels into the light green voxels"                                        - user, 2026-08-23
    "the dark voxels are not lighter grey. make them light green"               - user, 2026-08-23

    python tools/birch_deblack.py              measure only, change nothing  (this is the default)
    python tools/birch_deblack.py --apply      rewrite the .vox files
    python tools/birch_deblack.py --apply --near=2 --min=4

WHY THIS IS A .vox PASS AND NOT A BAKE CHANGE. game/assets/foilage/birch_trees/*.vox is the SHIPPED asset,
not an authoring original - the game reads it at runtime (assets/bow.js), so editing these files IS editing
the game. That makes this a seconds-long pass over the folder instead of the hour a re-voxelize costs, and it
means the result can be inspected in MagicaVoxel like any other edit.
RE-RUN IT AFTER EVERY BAKE. A bake writes the .vox files from scratch and therefore throws this away.

WHAT IT IS FIXING. Birch_bark05 is the fine twig network - 637k triangles threaded through the whole crown.
At 10 cm a twig is thinner than a voxel, so what the eye should see up there is the foliage growing off it,
not the twig; instead the canopy speckles grey. Fixing it in the BAKE goes only so far: biasing the wood/leaf
area contest asymptotes (3x -> 7024 wood, 8x -> 6409, 20x -> 6165) because thousands of those voxels have NO
leaf competing for them, and no contest can reach an uncontested voxel. They are reachable only AFTER the
fact, by asking what is AROUND them - which is what this does.

IT IS EVERY BARK SHADE, NOT JUST THE DARKEST, AND THAT WAS THE FIRST VERSION'S BUG (user 2026-08-23: "double
check the foilage again, the dark voxels are not lighter grey. make them light green"). This used to pick a
single target - the darkest entry, 322f24 then 54524e - and convert only that. Measured over the shipped 26,
that shade is 7,625 voxels and only 273 of them were still embedded in foliage, while the MID shade 716b5e is
113,959 voxels with 75,464 embedded and the light a6a195 is 27,738 with 6,711. So the pass reported success
having left ~82,000 grey voxels speckled through the canopy - an order of magnitude more than it converted.
The twig network wears ALL THREE shades, so all three are candidates.

THE RULE. A voxel is converted when it is bark AND at least `min` of its neighbours within Chebyshev distance
`near` are leaves AND it has at most `thick` bark neighbours. It then takes the leaf shade the MAJORITY of
those leaf neighbours wear, so a converted voxel blends into the part of the crown it sits in rather than
flattening it to one green. Defaults near=1 (the 26 neighbours), min=3, thick=8.

THE `thick` GUARD IS WHAT PROTECTS THE TREE, now that every shade is a candidate. A voxel on the surface of
the bole has ~17 bark neighbours; a 2x2 branch has ~11; a one-voxel twig has 2. So 8 keeps every solid mass
and takes only the threads. It matters most in the trunk band: of 1,089 low bark voxels that pass the leaf
test, the guard rejects 100 and the other 989 really are twigs in the low crown. The lash marking on the bole
- the thing that makes a birch look like a birch - is doubly safe: it is thick AND has no leaf neighbours.

IT IS NOT IDEMPOTENT, AND THAT IS THE ONE TRAP HERE. Converting a voxel makes it a LEAF NEIGHBOUR for the bark
voxels beside it, so a second run finds a fresh crop that now passes the test - measured, the first pass took
60,397 down to 14,405 and a second would have taken another 2,387. Run to convergence and the rule erodes
inward until the branches dissolve. Run it ONCE per bake. What remains is the bole and the solid branches.

MULTI-PART FILES. A tree taller than 256 ships as a STACK of models plus an nTRN/nGRP/nSHP scene graph, because
a .vox coordinate is one byte. This walks the chunk stream and rewrites ONLY the colour byte inside each XYZI,
leaving every other byte of the file identical - so the scene graph, the layers and the palette survive
untouched. Neighbours are looked up in TREE space across the whole stack, so a leaf in the part above still
counts for a twig at the top of the part below; a per-part pass would leave a black seam every 256 voxels.

Stdlib only; runs with the plain `python` on PATH.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
TARGET = (0x32, 0x2f, 0x24)                            # 322f24 - the shade the user first named; kept as documented intent
NEAR, MINLEAF, THICK, APPLY = 1, 3, 8, False
for a in sys.argv[1:]:
    if a == '--apply':
        APPLY = True
    elif a.startswith('--near='):
        NEAR = int(a[7:])
    elif a.startswith('--min='):
        MINLEAF = int(a[6:])
    elif a.startswith('--thick='):
        THICK = int(a[8:])
    else:
        sys.exit('unknown argument %s' % a)


def chunks(d):
    """-> [(id, start_of_content, content_len)] over MAIN's children, in file order."""
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid != b'MAIN':
            out.append((cid, i, cs))
        i += cs
    return out


def rd_dict(d, o):
    n = struct.unpack_from('<I', d, o)[0]
    o += 4
    m = {}
    for _ in range(n):
        ln = struct.unpack_from('<I', d, o)[0]; o += 4
        k = bytes(d[o:o + ln]).decode('utf-8', 'replace'); o += ln
        ln = struct.unpack_from('<I', d, o)[0]; o += 4
        v = bytes(d[o:o + ln]).decode('utf-8', 'replace'); o += ln
        m[k] = v
    return m, o


def part_offsets(d, ch, sizes):
    """The z base of each model, read out of the scene graph. Falls back to sequential stacking."""
    shp, trn = {}, []
    for cid, o, cs in ch:
        if cid == b'nSHP':
            nid = struct.unpack_from('<i', d, o)[0]
            o2 = o + 4
            _, o2 = rd_dict(d, o2)
            o2 += 4                                     # num_models
            shp[nid] = struct.unpack_from('<i', d, o2)[0]
        elif cid == b'nTRN':
            o2 = o + 4
            _, o2 = rd_dict(d, o2)
            child = struct.unpack_from('<i', d, o2)[0]
            o2 += 16                                    # child, reserved, layer, num_frames
            fr, o2 = rd_dict(d, o2)
            tz = int(fr['_t'].split(' ')[2]) if '_t' in fr else 0
            trn.append((child, tz))
    base = [None] * len(sizes)
    for child, tz in trn:
        mi = shp.get(child)
        if mi is not None and mi < len(sizes):
            base[mi] = tz - (sizes[mi][2] // 2)
    if any(b is None for b in base):                    # no usable scene graph -> stack in file order
        run, base = 0, []
        for sz in sizes:
            base.append(run)
            run += sz[2]
    lo = min(base)
    return [b - lo for b in base]


files = sorted(glob.glob(os.path.join(DIR, '*.vox')))
if not files:
    sys.exit('no .vox in %s' % DIR)
print('%-16s %5s %9s %10s %8s  %s' % ('tree', 'parts', 'bark', 'converted', 'left', 'share in foliage'))
tot_dark = tot_conv = 0
for path in files:
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    sizes = [struct.unpack_from('<III', d, o) for cid, o, cs in ch if cid == b'SIZE']
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    rgba = next((bytes(d[o:o + 1024]) for cid, o, cs in ch if cid == b'RGBA'), None)
    if not xyzi or rgba is None or len(sizes) != len(xyzi):
        print('%-16s  malformed (%d SIZE, %d XYZI) - skipped' % (os.path.basename(path)[:-4], len(sizes), len(xyzi)))
        continue
    leaf_ids = set(i + 1 for i in range(256)
                   if (rgba[i * 4] or rgba[i * 4 + 1] or rgba[i * 4 + 2])
                   and rgba[i * 4 + 1] > rgba[i * 4] + 12 and rgba[i * 4 + 1] > rgba[i * 4 + 2] + 12)
    # ── EVERY BARK SHADE IS A CANDIDATE ── this used to pick ONE target and convert only that: first by hex
    # (322f24), which a re-bake silently invalidated when the ramp re-derived to 322e24 and the pass then
    # matched nothing at all; then by ROLE, "the darkest non-leaf entry", which matched but was still one
    # shade out of three. The twig network wears all three, and the darkest is the RAREST of them, so the
    # by-role version converted 273 voxels while leaving ~82,000. Bark is simply "not a leaf and not empty".
    bark_ids = set(i + 1 for i in range(256)
                   if (rgba[i * 4] or rgba[i * 4 + 1] or rgba[i * 4 + 2]) and (i + 1) not in leaf_ids)
    if not bark_ids:
        print('%-16s  no bark shades in this palette - skipped' % os.path.basename(path)[:-4])
        continue
    lum = lambda i: rgba[(i - 1) * 4] * 0.299 + rgba[(i - 1) * 4 + 1] * 0.587 + rgba[(i - 1) * 4 + 2] * 0.114
    dhex = '%02x%02x%02x' % tuple(rgba[(min(bark_ids, key=lum) - 1) * 4:(min(bark_ids, key=lum) - 1) * 4 + 3])
    zoff = part_offsets(d, ch, sizes)
    occ, slots = {}, []                                 # TREE space, so neighbours cross the part seams
    for pi, (o, cs) in enumerate(xyzi):
        n = struct.unpack_from('<I', d, o)[0]
        for k in range(n):
            b = o + 4 + k * 4
            key = (d[b], d[b + 1], d[b + 2] + zoff[pi])
            occ[key] = d[b + 3]
            slots.append((key, b))
    conv, ndark = {}, 0
    for key, c in occ.items():
        if c not in bark_ids:
            continue
        ndark += 1
        x, y, z = key
        counts, nbark = {}, 0
        for dx in range(-NEAR, NEAR + 1):
            for dy in range(-NEAR, NEAR + 1):
                for dz in range(-NEAR, NEAR + 1):
                    if dx or dy or dz:
                        nb = occ.get((x + dx, y + dy, z + dz))
                        if nb is None:
                            continue
                        if nb in leaf_ids:
                            counts[nb] = counts.get(nb, 0) + 1
                        else:
                            nbark += 1
        # nbark counts a CHEBYSHEV-NEAR shell, so the guard must scale with it or --near=2 would convert the
        # whole trunk: the shell holds (2*NEAR+1)^3 - 1 cells, and THICK is stated against the 26 of NEAR=1.
        if sum(counts.values()) >= MINLEAF and nbark <= THICK * (((2 * NEAR + 1) ** 3 - 1) / 26.0):
            conv[key] = max(counts.items(), key=lambda kv: kv[1])[0]
    if APPLY and conv:
        for key, b in slots:
            nc = conv.get(key)
            if nc and d[b + 3] in bark_ids:
                d[b + 3] = nc                           # ONLY the colour byte moves; every other byte is untouched
        open(path, 'wb').write(bytes(d))
    tot_dark += ndark
    tot_conv += len(conv)
    print('%-16s %5d %9d %10d %8d  %5.1f%%  %s' % (os.path.basename(path)[:-4], len(xyzi), ndark, len(conv),
                                                   ndark - len(conv), 100.0 * len(conv) / max(1, ndark), dhex))
print('%-16s %5s %9d %10d %8d  %5.1f%%' % ('ALL', '', tot_dark, tot_conv, tot_dark - tot_conv,
                                           100.0 * tot_conv / max(1, tot_dark)))
print('near=%d min=%d thick=%d  %s' % (NEAR, MINLEAF, THICK, 'APPLIED' if APPLY else 'measured only - pass --apply to rewrite'))
