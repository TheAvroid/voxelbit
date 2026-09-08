"""Open the FOURTH bark slot in a freshly baked birch: shift the leaf ids up one, 4..7 -> 5..8.

    python tools/birch_barkslot.py            measure only  (default)
    python tools/birch_barkslot.py --apply    rewrite the .vox files

WHY THIS PASS HAS TO EXIST, AND WHY IT IS EASY TO MISS. Two tools disagree about where bark ends and
leaf begins, and nothing before this checked:

    voxelize_birch_forest.py   NBARK, NLEAF = 3, 4   ->  bark 1..3,  leaf 4..7   (7 shades)
    birch_bark_white.py        NBARK = 4             ->  bark 1..4,  leaf 5..8   (8 shades)

The retexture paints FOUR bark shades - two whites and two greys - onto a ramp the bake fitted with
three, so it needs one more id than the bake hands it. Run straight after a bake it does not fail, it
MISREADS: its only test for "is this voxel bark" is `id <= 4`, so the darkest oak green at id 4 is
taken for bark and painted white, and the birches come out of the pipeline with three leaf shades
instead of four and a canopy that has lost its darks. Nothing raises, and the .vox still opens.

That the shipped set has EIGHT shades with the greens at 5..8 is the evidence the shift belongs here;
it simply was not written down as a step. This is that step.

WHY NOT JUST BAKE WITH --nbark=4. Because the four fitted bark shades would be thrown away a moment
later - bark_white assigns every bark voxel from GEOMETRY (trunk thickness, then the lenticel band
phase), never from the shade the bake gave it. The only thing the bake's bark ramp has to do is sit
below the boundary. Baking a fourth shade to discard it would also push
game/assets/decoration/birch_trees.json to nbark=4, changing a shipped asset's shape for nothing -
that file is the web game's forest and is correct at 3.

IDEMPOTENT, and it has to be: run it twice and the second pass finds the leaves already at 5..8, sees
no leaf sitting in the bark's range, and does nothing. A blind +1 would walk the greens off the end of
the ramp one id per run.

MULTI-PART AWARE. A tall birch ships as stacked models plus an nTRN/nGRP/nSHP scene graph, so every
XYZI chunk is rewritten, not just the first.

Stdlib only.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
NBARK = 4                                              # what birch_bark_white.py needs; the boundary this opens
APPLY = '--apply' in sys.argv[1:]
for a in sys.argv[1:]:
    if a != '--apply':
        sys.exit('unknown argument %s' % a)


def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid != b'MAIN':
            out.append((cid, i, cs))
            i += cs
    return out


# The same green test birch_deblack.py uses, so the two passes cannot disagree about what a leaf is.
def leaf_set(rgba):
    return set(i + 1 for i in range(256)
               if (rgba[i * 4] or rgba[i * 4 + 1] or rgba[i * 4 + 2])
               and rgba[i * 4 + 1] > rgba[i * 4] + 12 and rgba[i * 4 + 1] > rgba[i * 4 + 2] + 12)


print('%-16s %-16s %-16s %s' % ('tree', 'leaf ids', '-> leaf ids', 'voxels moved'))
moved_all, done = 0, 0
for path in sorted(glob.glob(os.path.join(DIR, '*.vox'))):
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    rgba_o = next((o for cid, o, cs in ch if cid == b'RGBA'), None)
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    if rgba_o is None or not xyzi:
        print('%-16s malformed - skipped' % os.path.basename(path)[:-4])
        continue
    rgba = bytes(d[rgba_o:rgba_o + 1024])
    leaves = sorted(leaf_set(rgba))
    if not leaves:
        print('%-16s no leaf shades in this palette - skipped' % os.path.basename(path)[:-4])
        continue
    if leaves[0] > NBARK:
        print('%-16s %-16s %-16s already clear' % (os.path.basename(path)[:-4],
                                                   ','.join(map(str, leaves)), '-'))
        continue
    # The leaves start inside the bark's range. Shift the whole leaf block up by exactly enough to
    # clear it - one id, for the 3-bark bake this is written against.
    shift = NBARK + 1 - leaves[0]
    if leaves[-1] + shift > 255:
        sys.exit('%s: shifting leaves by %d would run past id 255' % (path, shift))
    leafset = set(leaves)
    moved = 0
    for o, cs in xyzi:
        n = struct.unpack_from('<I', d, o)[0]
        for k in range(n):
            b = o + 4 + k * 4 + 3
            if d[b] in leafset:
                if APPLY:
                    d[b] += shift
                moved += 1
    if APPLY:
        pal = bytearray(d[rgba_o:rgba_o + 1024])
        for i in reversed(leaves):                     # high to low, so a write cannot clobber a source
            pal[(i - 1 + shift) * 4:(i - 1 + shift) * 4 + 4] = rgba[(i - 1) * 4:(i - 1) * 4 + 4]
        # Clear the ids the block vacated, so no stale colour is left behind for a later pass to read
        # as a live shade. bark_white writes 1..NBARK over them immediately after this anyway.
        for i in sorted(leafset - {j + shift for j in leaves}):
            pal[(i - 1) * 4:(i - 1) * 4 + 4] = bytes(4)
        d[rgba_o:rgba_o + 1024] = pal
        open(path, 'wb').write(bytes(d))
    moved_all += moved
    done += 1
    print('%-16s %-16s %-16s %8d' % (os.path.basename(path)[:-4], ','.join(map(str, leaves)),
                                     ','.join(str(i + shift) for i in leaves), moved))

print('\n%d trees shifted, %d leaf voxels moved  %s'
      % (done, moved_all, 'APPLIED' if APPLY else 'measured only - pass --apply to rewrite'))
