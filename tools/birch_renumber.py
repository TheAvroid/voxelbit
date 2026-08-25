"""Renumber game/assets/foilage/birch_trees/*.vox to a gap-free 1..N, ordered SHORTEST to TALLEST.

    "I also deleted some .vox files for the birch trees. re order them and re number them properly."
                                                                                - user, 2026-08-23

    python tools/birch_renumber.py            show the plan  (default)
    python tools/birch_renumber.py --apply    do it

WHY HEIGHT ORDER. assets/bow.js sorts the loaded set short-to-tall because birchAt's height guard walks a
PREFIX of it — on a column with little headroom it takes the first models that fit. Numbering by height means
the file number and the load order are the same order, so "tree 3" means the same thing in MagicaVoxel, in the
editor and in the engine. It also makes the stage pick legible: only the models under ~199 voxels fit the
editor stage, and with this ordering those are exactly 1..k for some k, which this prints.

WHY IT HAS TO CLOSE GAPS. Deleting a few files by hand leaves holes (1,2,5,11,...). Nothing in the engine
breaks on a hole — VOXDEX is a directory walk and bow.js loads whatever it names — but ui/editor.js addresses
ONE model by filename for the stage, and a hole is how that ends up pointing at a file that is not there.

Two-phase rename through .tmp so a target name that is currently in use by another file cannot clobber it.
Stdlib only.
"""
import glob, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
APPLY = '--apply' in sys.argv[1:]
for a in sys.argv[1:]:
    if a != '--apply': sys.exit('unknown argument %s' % a)

STAGE_FIT = 199                                        # ui/editor.js: ~199 reliable, 222 absolute


def height(path):
    d = open(path, 'rb').read()
    i, h = 8, 0
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid == b'SIZE': h += struct.unpack_from('<III', d, i)[2]
        if cid != b'MAIN': i += cs
    return h


rows = sorted((height(p), os.path.basename(p)) for p in glob.glob(os.path.join(DIR, '*.vox')))
if not rows: sys.exit('no .vox in %s' % DIR)
plan = [(old, '%d.vox' % (n + 1), h) for n, (h, old) in enumerate(rows)]
fits = 0
for old, new, h in plan:
    mark = '' if h > STAGE_FIT else '  (fits the editor stage)'
    if h <= STAGE_FIT: fits = int(new[:-4])
    print('  %-14s h=%3d  ->  %-7s%s' % (old[:-4], h, new, mark))
print('%d trees, numbered 1..%d;  1..%d fit the stage (<=%d voxels)' % (len(plan), len(plan), fits, STAGE_FIT))
if APPLY:
    for old, new, h in plan:
        if old != new: os.rename(os.path.join(DIR, old), os.path.join(DIR, old + '.tmp'))
    for old, new, h in plan:
        src = os.path.join(DIR, old + '.tmp' if old != new else old)
        os.replace(src, os.path.join(DIR, new))
    print('APPLIED — remember: ui/editor.js ED_STAGE names a file, and tools/bundle.py rebuilds VOXDEX')
else:
    print('plan only — pass --apply')
