"""demcmp -- compare two .vbdem windows at the same lon/lat.

    python demcmp.py <a.vbdem> <b.vbdem>

WHY THIS EXISTS. A wrong UTM zone, a transposed tie point or a datum muddle all
produce a .vbdem that opens cleanly, has the right extent in its header and is
full of entirely plausible elevations -- the same pixel-for-pixel silent
failure as the floating-point predictor in dem2raw.cpp. The only thing that
catches it is asking a SECOND, independently georeferenced product what it
thinks the ground is doing at the same place on the earth.

Agreement to a few metres means both products are reading the same hillside.
Disagreement in the TENS of metres, with a strong correlation still present,
means a horizontal shift -- look at the projection, not the elevations.
"""
import io, math, struct, sys

HFMT = "<8s2i6d2f8i"


def load(path):
    f = io.open(path, "rb")
    hb = f.read(struct.calcsize(HFMT))
    magic, w, h, olon, olat, slon, slat, mx, my, lo, hi = struct.unpack(HFMT, hb)[:11]
    g = f.read(w * h * 4)
    return dict(w=w, h=h, olon=olon, olat=olat, slon=slon, slat=slat,
                mx=mx, my=my, lo=lo, hi=hi, g=g, path=path)


def at(d, lon, lat):
    """Nearest sample, or None outside the window."""
    i = int((lon - d["olon"]) / d["slon"])
    j = int((d["olat"] - lat) / d["slat"])
    if i < 0 or j < 0 or i >= d["w"] or j >= d["h"]:
        return None
    return struct.unpack_from("<f", d["g"], (j * d["w"] + i) * 4)[0]


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    a, b = load(sys.argv[1]), load(sys.argv[2])
    for d in (a, b):
        print("%-28s %5dx%-5d  %.3f m/sample  %.1f..%.1f m"
              % (d["path"].split("/")[-1], d["w"], d["h"], d["mx"], d["lo"], d["hi"]))

    # Walk the COARSER grid, so every sample asked for exists in both.
    c, fine = (a, b) if a["mx"] >= b["mx"] else (b, a)
    n = 0
    diffs = []
    step = max(1, c["w"] // 400)
    for j in range(1, c["h"] - 1, step):
        lat = c["olat"] - (j + 0.5) * c["slat"]
        for i in range(1, c["w"] - 1, step):
            lon = c["olon"] + (i + 0.5) * c["slon"]
            va, vb = at(c, lon, lat), at(fine, lon, lat)
            if va is None or vb is None:
                continue
            diffs.append(va - vb)
            n += 1
    if not n:
        print("no overlap")
        return 1
    diffs.sort()
    mean = sum(diffs) / n
    rms = math.sqrt(sum(d * d for d in diffs) / n)
    print("\n%d samples compared (coarse grid, every %d)" % (n, step))
    print("  mean   %+.2f m      <- a datum or vertical reference offset" % mean)
    print("  rms    %7.2f m      <- resolution, if the mean is near zero" % rms)
    print("  median %+.2f m" % diffs[n // 2])
    print("  p05 %+.2f   p95 %+.2f   worst %+.2f / %+.2f"
          % (diffs[n // 20], diffs[n - 1 - n // 20], diffs[0], diffs[-1]))

    # A HORIZONTAL shift shows up as difference CORRELATED WITH SLOPE, which is
    # the one thing a vertical offset cannot fake.
    up = [d for d in diffs if d > 0]
    print("  %.1f%% of samples read higher in the coarse product" % (100.0 * len(up) / n))
    if abs(mean) > 2.0:
        print("\n  MEAN OVER 2 m -- suspect the datum or the tie point.")
    if rms > 15.0:
        print("\n  RMS OVER 15 m -- suspect the projection; this is not resolution.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
