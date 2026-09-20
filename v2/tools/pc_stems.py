"""pc_stems -- measure real stems/ha, to replace realStemsPerHa's floor.

    python pc_stems.py <plot.las> [plot2.las ...] [--cell 1.0]

WHAT IS BEING REPLACED, AND WHY IT IS WORSE THAN IT LOOKS.
`VoxelTerrain::realStemsPerHa` is a nine-point altitude curve for the Colorado
Front Range:

    {1800 m, 120}, {2300 m, 175}, {2650 m, 550}, {2950 m, 1750}, ...

and its first line is `if (aslM <= k[0].m) return k[0].stems;` -- so
**everything at or below 1800 m returns 120 stems/ha**. Ouachita tops out at
513 m and Acadia at 465.6 m, so for both of those worlds the entire table
collapses to one constant chosen for a montane conifer zone two vertical
kilometres above them. Only the three Colorado windows ever touch the curve.

This counts trees in plots where every tree was annotated by hand, so the
answer is a measurement rather than a table.

THE AREA IS OCCUPANCY, NOT A BOUNDING BOX. Plots are irregular and a bounding
box over-counts area wherever the plot is not square, which biases stems/ha
DOWN -- in the same direction as the bug being investigated, which is the worst
possible direction for an error to point. Occupied terrain cells give an area
that follows the real footprint.

WHICH POINTS COUNT AS A TREE. The readMe's classes:
    0 unclassified   1 low vegetation   2 terrain   3 OUT-points
    4 stem           5 live branches    6 woody branches
Class 3 is explicitly "trees outside of the measured plot", so counting it
would inflate the density with trees whose ground is not in the denominator.
Only 4/5/6 count, and treeID 0 is not a tree.
"""
import io, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lasread import LasFile

TREE_CLASSES = (4, 5, 6)
TERRAIN = 2
OUT_POINTS = 3


def measure(path, cell, budget):
    las = LasFile(path)
    trees = set()
    ground = set()
    out_trees = set()
    allcells = set()
    n = 0
    for (x, y, z, cls, ret, nret, r, g, b, ex) in las.points(want=budget, blocks=10):
        n += 1
        k = (int(math.floor(x / cell)), int(math.floor(y / cell)))
        allcells.add(k)
        tid = ex.get("treeID", 0)
        if cls == TERRAIN:
            ground.add(k)
        elif cls in TREE_CLASSES and tid:
            trees.add(tid)
        elif cls == OUT_POINTS and tid:
            out_trees.add(tid)
    las.close()
    # Prefer the terrain footprint; fall back to all points where a plot has
    # little ground return (dense canopy), and say which was used.
    if len(ground) >= 0.25 * len(allcells) and len(ground) > 50:
        area_cells, basis = len(ground), "terrain"
    else:
        area_cells, basis = len(allcells), "all-pts"
    area_ha = area_cells * cell * cell / 10000.0
    return {
        "trees": len(trees), "out": len(out_trees),
        "area_ha": area_ha, "basis": basis, "points": n,
        "stems_ha": (len(trees) / area_ha) if area_ha > 1e-9 else 0.0,
    }


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cell = 1.0
    budget = 3_000_000
    if "--cell" in sys.argv:
        cell = float(sys.argv[sys.argv.index("--cell") + 1])
    if "--budget" in sys.argv:
        budget = int(sys.argv[sys.argv.index("--budget") + 1])
    if not args:
        print(__doc__)
        return 2

    print("area cell %.2f m; tree classes 4/5/6; class 3 (out-points) excluded\n" % cell)
    print("%-34s %7s %6s %8s %9s %8s" %
          ("plot", "trees", "out", "area ha", "stems/ha", "basis"))
    rows = []
    for p in args:
        try:
            m = measure(p, cell, budget)
        except Exception as e:
            print("%-34s  skip: %s" % (os.path.basename(p)[:34], e))
            continue
        if m["trees"] == 0 or m["area_ha"] <= 0:
            print("%-34s  no annotated trees" % os.path.basename(p)[:34])
            continue
        rows.append(m)
        print("%-34s %7d %6d %8.3f %9.0f %8s" %
              (os.path.basename(p)[:34], m["trees"], m["out"],
               m["area_ha"], m["stems_ha"], m["basis"]))

    if not rows:
        print("\nnothing measured")
        return 1
    v = sorted(r["stems_ha"] for r in rows)
    n = len(v)
    med = v[n // 2]
    print("\n%d plots" % n)
    print("  stems/ha   median %.0f   min %.0f   max %.0f" % (med, v[0], v[-1]))

    print("\n-- AGAINST THE ENGINE -------------------------------------------")
    print("  realStemsPerHa returns 120 for every elevation at or below 1800 m,")
    print("  which is the whole of Ouachita (max 513 m) and Acadia (max 466 m).")
    print("  Measured here: median %.0f stems/ha, i.e. the floor is %.1fx %s."
          % (med, (med / 120.0) if med > 120 else (120.0 / med),
             "LOW" if med > 120 else "HIGH"))
    print("\n  NOTE: these are managed boreal/temperate research plots, not")
    print("  Arkansas oak-hickory or Maine coastal birch. Treat the number as")
    print("  the right ORDER for a closed-canopy stand, not as Ouachita's own.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
