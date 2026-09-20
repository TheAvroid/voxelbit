"""Grow the seven oak .vox models by a uniform factor.

    "I want you to revoxelize the oak trees and increase the size of the trees
     porportinally by 50%."                                  - user, 2026-09-16

    python tools/revoxel_oak_scaled.py              show the plan (measure only)
    python tools/revoxel_oak_scaled.py --apply      rewrite the .vox files
    python tools/revoxel_oak_scaled.py --scale 1.25 some other factor

WHY THIS BORROWS revoxel_trees_tall.py RATHER THAN REPEATING IT
  Everything hard about rescaling a .vox is already solved there and the reasons
  are written out at length: nearest-neighbour is the ONLY filter allowed
  because palette ids are identity rather than colour, and anything past 255 on
  an axis has to ship as stacked pieces because a .vox XYZI record packs each
  coordinate in one byte. Both apply here unchanged. What differs is only which
  files and what factor, so this file is the difference and nothing else.

PROPORTIONAL, WHICH IS NOT THE SAME AS "TO A TARGET HEIGHT"
  revoxel_trees_tall.py takes a height and scales the set so the TALLEST hits
  it. "Increase the size by 50%" is the other kind of ask: one factor on every
  model, so the bush stays a bush relative to the big oak and the set keeps the
  spread it was authored with. 1.5 is applied to all three axes, so a crown
  widens exactly as much as the trunk rises.

WHAT 1.5 DOES TO THE CEILING
  oak_7 is the constraint at 170x167x171. At 1.5 it is 255x251x257 -- the two
  horizontal axes land ON the limit and the height goes one voxel over it, so
  that one model ships as two stacked pieces and the other six stay single.
  A larger factor would push X past 255 as well, which this tool CANNOT split:
  slabs_for divides the Z axis only, because that is the axis a tree is long on.
  So 1.5 is close to the most this set can take without a second kind of split.
"""

import glob
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import revoxel_trees_tall as rt  # noqa: E402  -- the path has to be set first

OAK = os.path.join(rt.ROOT, 'game', 'assets', 'foilage', 'oak_trees')


def main():
    args = sys.argv[1:]
    apply_ = '--apply' in args
    scale = 1.5
    if '--scale' in args:
        scale = float(args[args.index('--scale') + 1])

    # WHATEVER IS IN THE FOLDER, not a hardcoded 1..7. The seven this was
    # written for were replaced by three FBX-voxelised oaks on 2026-09-19 and
    # parked in _replaced-*/ -- a fixed range then fails on oak_4 and the set
    # it should be growing is the one that is actually shipping. Non-recursive,
    # so the parked folder is not picked up.
    paths = sorted(glob.glob(os.path.join(OAK, 'oak_*.vox')),
                   key=lambda q: int(os.path.basename(q)[4:-4]))
    if not paths:
        print('no oak_*.vox in %s' % OAK)
        return 1

    read = [(p,) + rt.read_vox(p) for p in paths]
    print('uniform x%.3f on all three axes, %d models' % (scale, len(read)))
    print('.vox ceiling is %d per axis; over that on Z ships as stacked pieces\n'
          % rt.MAX_AXIS)

    hdr = '%-12s %-16s %-16s %8s %10s %7s'
    print(hdr % ('file', 'was (voxels)', 'now (voxels)', 'tall', 'voxels', 'pieces'))
    before = after = 0
    over = []
    for p, size, grid, rgba, carried in read:
        out_size, out_grid = rt.upscale(size, grid, scale)
        pieces = len(rt.slabs_for(out_size[2]))
        before += len(grid)
        after += len(out_grid)
        # THE TWO HORIZONTAL AXES CANNOT BE SPLIT -- see the note at the top.
        # Caught and reported rather than written, because a model over the
        # ceiling on X or Y writes a file whose coordinates wrap silently.
        if out_size[0] > rt.MAX_AXIS or out_size[1] > rt.MAX_AXIS:
            over.append((os.path.basename(p), out_size))
        print(hdr % (os.path.basename(p),
                     '%dx%dx%d' % size,
                     '%dx%dx%d' % out_size,
                     '%.1f m' % (out_size[2] * rt.VOXEL_M),
                     len(out_grid),
                     pieces))

    if over:
        print('\nREFUSED -- these exceed %d on X or Y, which slabs_for cannot split:'
              % rt.MAX_AXIS)
        for name, sz in over:
            print('  %-12s %dx%dx%d' % (name, sz[0], sz[1], sz[2]))
        print('lower --scale, or teach slabs_for to divide a horizontal axis.')
        return 1

    print('\nvoxels %d -> %d  (x%.2f)' % (before, after, after / float(before)))
    if not apply_:
        print('measure only -- pass --apply to rewrite the .vox files')
        return 0

    for p, size, grid, rgba, carried in read:
        out_size, out_grid = rt.upscale(size, grid, scale)
        wrote = rt.write_vox(p, out_size, out_grid, rgba, carried)
        assert wrote == len(rt.slabs_for(out_size[2])) or not out_grid
    print('written.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
