"""pc_roughness -- fit VoxelTerrain::roughFor from real high-density ground.

    python pc_roughness.py <plot.las> [plot2.las ...] [--cell 0.10] [--class 2]

WHAT IS BEING REPLACED. `roughFor` in src/world/voxelworld.h carries two
numbers per cover class -- a peak-to-peak amplitude in real metres and a
wavelength -- and the comment over it says plainly that they are "terrain
intuition, not fitted ... there is no high-resolution exemplar in the tree to
fit against yet". This is that exemplar.

`roughPostingFactor` additionally assumes relief below wavelength L grows as
L^H with **H = 0.75**, which was asserted, never measured. That exponent is the
thing that makes a 1 m source invent 17% of what a 10.29 m one does, so it is
load-bearing. This measures it.

THE METHOD IS A STRUCTURE FUNCTION, NOT AN RMS.
A single RMS of a detrended surface answers "how rough" with one number and
cannot tell you at WHICH SCALE that roughness lives -- and scale is the entire
question, because everything above the DEM posting is already measured and only
what is below it has to be invented. The structure function

    D(h) = < ( z(x) - z(x+h) )^2 >^(1/2)

answers both at once: for a self-affine surface D(h) = A * h^H, so a
straight-line fit on a log-log plot gives the amplitude A at h = 1 m AND the
exponent H. Those are exactly the two constants the engine needs, and the fit
residual says whether "self-affine" was a fair description in the first place.

WHY THE PLANE COMES OUT FIRST. A plot on a hillside has a mean slope, and slope
is not roughness -- left in, it dominates every lag and reports the hill. A
least-squares plane over the plot is removed before anything is measured, which
is the same thing the engine does implicitly by adding roughness ON TOP of an
interpolated DEM surface.

CAVEAT THAT MUST TRAVEL WITH THE NUMBERS: FOR-instance plots are FOREST FLOOR
in boreal/temperate/managed stands. They fit the Forest row honestly. Rock,
Meadow and Snow have no exemplar here and must not be quietly fitted from it.
"""
import io, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lasread import LasFile


def grid_ground(path, cell, want_class, budget):
    """Mean Z per `cell`-metre bin over points of class `want_class`."""
    las = LasFile(path)
    acc = {}
    seen = kept = 0
    # Ground is a minority of a forest plot's points, so the budget is spent
    # generously and filtered down rather than sampled thinly.
    for (x, y, z, cls, ret, nret, r, g, b, ex) in las.points(want=budget, blocks=6):
        seen += 1
        if cls != want_class:
            continue
        kept += 1
        k = (int(math.floor(x / cell)), int(math.floor(y / cell)))
        s = acc.get(k)
        if s is None:
            acc[k] = [z, 1]
        else:
            s[0] += z
            s[1] += 1
    las.close()
    zs = [v[0] / v[1] for v in acc.values()]
    zrange = (min(zs), max(zs)) if zs else (0.0, 0.0)
    return {k: v[0] / v[1] for k, v in acc.items()}, seen, kept, zrange


def is_normalised(zrange):
    """Has this plot's ground been flattened to zero?

    HEIGHT-NORMALISED CLOUDS ARE THE TRAP IN THIS DATASET AND THEY ARE SILENT.
    Many forest point clouds are published with Z replaced by height-above-
    ground, so every terrain point sits at z ~ 0 by construction. Fitting a
    structure function to that does not fail -- it returns a beautifully
    consistent answer describing the RESIDUAL NOISE of whatever produced the
    normalisation, which is not terrain and never was.

    Measured in FOR-instance: SCION's terrain spans -0.11..0.13 m, while CULS,
    TUWIEN and RMIT sit at 261..337 m above sea level and span 1.9..2.9 m. The
    five SCION plots all returned H = 0.00 +- 0.01 with r2 as low as 0.04 --
    a FLAT structure function, i.e. the height difference at 10 cm equals the
    height difference at 11 m, which is white noise wearing a terrain costume.
    They were the majority of the sample, so the pooled median came out at
    H = 0.003 against a real value near 0.33.

    Two tests, because either alone can be fooled: ground centred on zero, and
    a vertical span too small for a plot tens of metres across to have any
    slope at all.
    """
    lo, hi = zrange
    centred_on_zero = abs(lo) < 1.0 and abs(hi) < 1.0
    too_flat = (hi - lo) < 0.40
    return centred_on_zero and too_flat


def detrend(cells):
    """Remove the least-squares plane. Slope is not roughness."""
    n = len(cells)
    if n < 16:
        return cells
    sx = sy = sz = sxx = sxy = syy = sxz = syz = 0.0
    for (i, j), z in cells.items():
        sx += i; sy += j; sz += z
        sxx += i * i; sxy += i * j; syy += j * j
        sxz += i * z; syz += j * z
    # normal equations for z = a*i + b*j + c
    m = [[sxx, sxy, sx], [sxy, syy, sy], [sx, sy, float(n)]]
    v = [sxz, syz, sz]
    for c in range(3):                       # Gaussian elimination, 3x3
        p = max(range(c, 3), key=lambda r: abs(m[r][c]))
        if abs(m[p][c]) < 1e-12:
            return cells
        m[c], m[p] = m[p], m[c]
        v[c], v[p] = v[p], v[c]
        for r in range(c + 1, 3):
            f = m[r][c] / m[c][c]
            for k in range(c, 3):
                m[r][k] -= f * m[c][k]
            v[r] -= f * v[c]
    sol = [0.0, 0.0, 0.0]
    for c in (2, 1, 0):
        s = v[c] - sum(m[c][k] * sol[k] for k in range(c + 1, 3))
        sol[c] = s / m[c][c]
    a, b, c0 = sol
    return {(i, j): z - (a * i + b * j + c0) for (i, j), z in cells.items()}


def structure_function(cells, cell, lags):
    """D(h) over axis-aligned lags -- cheap, unbiased, and enough for a slope."""
    out = []
    for k in lags:
        s = 0.0
        n = 0
        for (i, j), z in cells.items():
            o = cells.get((i + k, j))
            if o is not None:
                d = z - o; s += d * d; n += 1
            o = cells.get((i, j + k))
            if o is not None:
                d = z - o; s += d * d; n += 1
        if n >= 64:
            out.append((k * cell, math.sqrt(s / n), n))
    return out


def fit_power(pts):
    """log D = log A + H log h, least squares -> (A at h=1 m, H, r2)."""
    n = len(pts)
    if n < 3:
        return None
    X = [math.log(h) for h, d, _ in pts]
    Y = [math.log(d) for h, d, _ in pts]
    mx = sum(X) / n; my = sum(Y) / n
    sxy = sum((X[i] - mx) * (Y[i] - my) for i in range(n))
    sxx = sum((X[i] - mx) ** 2 for i in range(n))
    if sxx < 1e-12:
        return None
    H = sxy / sxx
    lnA = my - H * mx
    ss_res = sum((Y[i] - (lnA + H * X[i])) ** 2 for i in range(n))
    ss_tot = sum((Y[i] - my) ** 2 for i in range(n))
    r2 = 1.0 - ss_res / ss_tot if ss_tot > 1e-12 else 0.0
    return math.exp(lnA), H, r2


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cell = 0.10
    want_class = 2
    budget = 3_000_000
    if "--cell" in sys.argv:
        cell = float(sys.argv[sys.argv.index("--cell") + 1])
    if "--class" in sys.argv:
        want_class = int(sys.argv[sys.argv.index("--class") + 1])
    if "--budget" in sys.argv:
        budget = int(sys.argv[sys.argv.index("--budget") + 1])
    if not args:
        print(__doc__)
        return 2

    # Lags in CELLS, log-spaced from one cell to ~10 m -- the band between a
    # voxel and a DEM posting, which is the band the engine has to invent.
    lags = sorted(set(int(round(1.6 ** k)) for k in range(0, 12)))
    lags = [k for k in lags if k * cell <= 12.0 and k >= 1]

    print("cell %.2f m, class %d (2 = Terrain), lags %.2f..%.1f m\n"
          % (cell, want_class, lags[0] * cell, lags[-1] * cell))
    print("%-34s %8s %8s %8s %7s %7s" %
          ("plot", "gnd pts", "cells", "A@1m cm", "H", "r2"))

    fits = []
    skipped = []
    for p in args:
        try:
            cells, seen, kept, zrange = grid_ground(p, cell, want_class, budget)
        except Exception as e:
            print("%-34s  skip: %s" % (os.path.basename(p)[:34], e))
            continue
        if is_normalised(zrange):
            print("%-34s %8d %8d   NORMALISED (ground z %.2f..%.2f) -- excluded"
                  % (os.path.basename(p)[:34], kept, len(cells), zrange[0], zrange[1]))
            skipped.append(os.path.basename(p))
            continue
        if len(cells) < 400:
            print("%-34s %8d %8d   too few ground cells" %
                  (os.path.basename(p)[:34], kept, len(cells)))
            continue
        d = detrend(cells)
        sf = structure_function(d, cell, lags)
        f = fit_power(sf)
        if not f:
            print("%-34s  no fit" % os.path.basename(p)[:34])
            continue
        A, H, r2 = f
        fits.append((os.path.basename(p), A, H, r2, len(cells), sf))
        print("%-34s %8d %8d %8.2f %7.3f %7.3f" %
              (os.path.basename(p)[:34], kept, len(cells), A * 100.0, H, r2))

    if skipped:
        print("\n  %d plot(s) excluded as height-normalised: %s"
              % (len(skipped), ", ".join(skipped[:6])))
        print("  Their ground was flattened to zero before publication, so a fit")
        print("  on them describes noise, not terrain. See is_normalised().")
    if not fits:
        print("\nnothing fitted")
        return 1

    # ---- the numbers the engine wants ------------------------------------
    n = len(fits)
    As = sorted(f[1] for f in fits)
    Hs = sorted(f[2] for f in fits)
    medA = As[n // 2]
    medH = Hs[n // 2]
    print("\n%d plots fitted" % n)
    print("  A at h=1 m   median %.2f cm   (p10 %.2f, p90 %.2f)"
          % (medA * 100, As[max(0, n // 10)] * 100, As[min(n - 1, 9 * n // 10)] * 100))
    print("  H            median %.3f      (p10 %.3f, p90 %.3f)"
          % (medH, Hs[max(0, n // 10)], Hs[min(n - 1, 9 * n // 10)]))

    print("\n-- WHAT THIS SAYS ABOUT THE ENGINE ------------------------------")
    print("  roughPostingFactor assumes H = 0.75; measured median is %.3f." % medH)
    if abs(medH - 0.75) < 0.08:
        print("  That is close enough that the assumption stands.")
    else:
        print("  That is a REAL difference. At a 1 m posting the factor")
        print("    assumed  (1/10.29)^0.75 = %.4f" % (10.29 ** -0.75))
        print("    measured (1/10.29)^%.2f = %.4f" % (medH, 10.29 ** -medH))

    # roughFor's ampM is PEAK TO PEAK over the noise, and the fbm there spans
    # roughly +-0.5 of its amplitude, so a structure-function RMS at the
    # posting scale maps to ampM by ~2*sqrt(2) (RMS of a difference of two
    # independent samples is sqrt(2) times the RMS of one).
    d10 = medA * (10.29 ** medH)
    print("\n  D(h) at the 10.29 m posting: %.1f cm rms of height DIFFERENCE" % (d10 * 100))
    print("  -> single-point rms %.1f cm -> peak-to-peak ampM ~ %.2f m"
          % (d10 * 100 / math.sqrt(2), 2.0 * d10 / math.sqrt(2)))
    print("\n  roughFor currently has Forest at 0.55 m. Measured: %.2f m."
          % (2.0 * d10 / math.sqrt(2)))
    print("\n  These plots are FOREST FLOOR. Rock/Meadow/Snow have no exemplar")
    print("  here and must not be fitted from this.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
