"""naip2cov -- USGS NAIP aerial imagery -> a land-cover class per DEM posting.

Writes a .vbcov: the SAME grid as the .vbdem it is built against, one byte per
sample, so the engine can index both with one mapping and they cannot drift.

    python naip2cov.py <in.vbdem> <out.vbcov> [--px 2048]

WHY NOT JUST COLOUR THE GROUND. v2 has no material textures -- it is a 255-entry
palette and a per-voxel hash, and the palette is already at 232. So the imagery
is used to decide WHAT GOES WHERE, not to tint anything: forest, meadow, rock,
snow, water. That costs no palette entries at all, and it is the better half of
the information anyway. The treeline in this imagery wanders with aspect and
drainage; an altitude threshold cannot do that.

THE 4TH BAND IS NIR, whatever the file says. The service tags it ExtraSamples=2
("unassociated alpha"), but requesting bandIds=3,0,1 returns byte-identical
values in channel 0, so it is Band_4 = near infrared. That matters: NDVI
separates living vegetation from grey rock far more reliably than brightness,
and at 4,000 m a sunlit granite slope and a dry meadow look alike in RGB.
"""
import array, io, json, math, os, struct, subprocess, sys

SERVICE = ("https://imagery.nationalmap.gov/arcgis/rest/services/"
           "USGSNAIPPlus/ImageServer/exportImage")

UNKNOWN, FOREST, MEADOW, ROCK, SNOW, WATER, SHADOW = range(7)
NAMES = ["unknown", "forest", "meadow", "rock", "snow", "water", "shadow"]
RAMP_N = 10   # reclaimed BWHEAT slots 48..57; the birch wood is unreachable


# ----------------------------------------------------------------- tiff (8-bit)
def read_tiff_rgbn(path):
    """Uncompressed, tiled or stripped, 8-bit, N samples. -> (w, h, spp, bytes)."""
    d = io.open(path, "rb").read()
    if d[:2] == b"II":
        u16 = lambda o: struct.unpack_from("<H", d, o)[0]
        u32 = lambda o: struct.unpack_from("<I", d, o)[0]
    elif d[:2] == b"MM":
        u16 = lambda o: struct.unpack_from(">H", d, o)[0]
        u32 = lambda o: struct.unpack_from(">I", d, o)[0]
    else:
        raise ValueError("not a TIFF (got %r)" % d[:16])
    tags = {}
    off = u32(4)
    n = u16(off)
    for i in range(n):
        e = off + 2 + i * 12
        tid, typ, cnt = u16(e), u16(e + 2), u32(e + 4)
        size = {1: 1, 3: 2, 4: 4}.get(typ, 0)
        if not size:
            continue
        if size * cnt <= 4:
            vals = [(u16 if size == 2 else u32 if size == 4 else (lambda o: d[o]))(e + 8 + j * size)
                    for j in range(cnt)]
        else:
            p = u32(e + 8)
            vals = [(u16 if size == 2 else u32 if size == 4 else (lambda o: d[o]))(p + j * size)
                    for j in range(cnt)]
        tags[tid] = vals
    g = lambda t, dflt=None: tags[t][0] if t in tags else dflt

    w, h, spp = g(256), g(257), g(277, 1)
    if g(259, 1) != 1:
        raise ValueError("compressed TIFF (%d); expected raw" % g(259, 1))
    out = bytearray(w * h * spp)
    if 324 in tags:                                   # tiled
        tw, th = g(322), g(323)
        across = (w + tw - 1) // tw
        for idx, toff in enumerate(tags[324]):
            tx, ty = (idx % across) * tw, (idx // across) * th
            for r in range(th):
                y = ty + r
                if y >= h:
                    break
                src = toff + r * tw * spp
                ncol = min(tw, w - tx)
                dst = (y * w + tx) * spp
                out[dst:dst + ncol * spp] = d[src:src + ncol * spp]
    else:                                             # stripped
        rps = g(278, h)
        for idx, soff in enumerate(tags[273]):
            y0 = idx * rps
            rows = min(rps, h - y0)
            src = soff
            out[y0 * w * spp: (y0 + rows) * w * spp] = d[src: src + rows * w * spp]
    return w, h, spp, bytes(out)


# -------------------------------------------------------------- classification
def classify(r, g_, b, nir):
    """One pixel -> a cover class. Thresholds are for 8-bit NAIP."""
    vis = (r + g_ + b) / 3.0
    ndvi = (nir - r) / float(nir + r + 1)
    # Water first: NIR is absorbed almost completely, so it is the one class
    # that is unambiguous. Snow next, before vegetation, because bright snow
    # over trees would otherwise score as meadow.
    # DARK AND INFRARED-DEAD IS NOT ENOUGH TO CALL WATER. Open water, shade
    # under a spruce, and shade on a granite face are the same three numbers.
    # They are marked SHADOW here and resolved below from their surroundings,
    # because the thing that actually distinguishes them is not in the pixel.
    if nir < 35 and vis < 72 and ndvi < 0.06:
        return SHADOW
    if vis > 175 and ndvi < 0.12:
        return SNOW
    if ndvi > 0.14:
        # Conifer is DARK. That is the whole separation between a spruce stand
        # and the grass park next to it -- both are vigorously green in NDVI,
        # and only one of them is nearly black in visible light.
        return FOREST if vis < 98 else MEADOW
    return ROCK


def fetch(bbox, px, out_path, tries=3):
    args = [
        "curl", "-s", "--max-time", "300", "-o", out_path, "-G", SERVICE,
        "--data-urlencode", "bbox=%.8f,%.8f,%.8f,%.8f" % bbox,
        "--data-urlencode", "bboxSR=4326", "--data-urlencode", "imageSR=4326",
        "--data-urlencode", "size=%d,%d" % px,
        "--data-urlencode", "format=tiff", "--data-urlencode", "f=image",
    ]
    for a in range(tries):
        subprocess.run(args, check=False)
        if os.path.exists(out_path) and os.path.getsize(out_path) > 1000:
            with open(out_path, "rb") as f:
                if f.read(2) in (b"II", b"MM"):
                    return True
        print("    retry %d" % (a + 1))
    return False


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    dem_path, out_path = sys.argv[1], sys.argv[2]
    block = 2048
    if "--px" in sys.argv:
        block = int(sys.argv[sys.argv.index("--px") + 1])

    HFMT = "<8s2i6d2f8i"
    hb = io.open(dem_path, "rb").read(struct.calcsize(HFMT))
    magic, w, h, olon, olat, slon, slat, mx, my, lo, hi = struct.unpack(HFMT, hb)[:11]
    print("grid    %dx%d from %s" % (w, h, os.path.basename(dem_path)))
    print("extent  lon %.5f..%.5f  lat %.5f..%.5f" % (olon, olon + slon * w, olat - slat * h, olat))

    cov = bytearray(w * h)
    rgbs = [(0, 0, 0)] * (w * h)   # kept so the ramp can be built from real pixels
    tmp = os.path.join(os.environ.get("TEMP", "."), "_naip_block.tif")
    nbx, nby = (w + block - 1) // block, (h + block - 1) // block
    print("tiles   %dx%d requests of up to %dpx" % (nbx, nby, block))

    for by in range(nby):
        for bx in range(nbx):
            x0, y0 = bx * block, by * block
            bw, bh = min(block, w - x0), min(block, h - y0)
            west, east = olon + slon * x0, olon + slon * (x0 + bw)
            north, south = olat - slat * y0, olat - slat * (y0 + bh)
            print("  block %d,%d  %dx%d" % (bx, by, bw, bh))
            if not fetch((west, south, east, north), (bw, bh), tmp):
                print("    FAILED -- left as unknown")
                continue
            tw, th, spp, px = read_tiff_rgbn(tmp)
            if tw != bw or th != bh:
                print("    got %dx%d, wanted %dx%d" % (tw, th, bw, bh))
            for r in range(min(th, bh)):
                base = r * tw * spp
                row = (y0 + r) * w + x0
                for c in range(min(tw, bw)):
                    o = base + c * spp
                    cov[row + c] = classify(px[o], px[o+1], px[o+2],
                                            px[o+3] if spp > 3 else px[o])
                    rgbs[row + c] = (px[o], px[o+1], px[o+2])
    try:
        os.remove(tmp)
    except OSError:
        pass

    # -------------------------------------------------- RESOLVING THE SHADOW
    # A shadowed pixel takes the class of what is around it. That is the whole
    # rule, and it is better than any threshold because it uses the one thing a
    # single dark pixel does not have: context. Shade inside a stand is
    # surrounded by forest; shade on a face above the treeline is surrounded by
    # rock; a lake is surrounded by lake and is FLAT.
    #
    # The first run had no such step -- it called 4.7% of the Sawatch water, and
    # the giveaway was not spectral at all: those "lakes" sat on a mean 42%
    # GRADE. A slope gate alone then over-corrected the other way, turning
    # shadowed rock at 4,000 m into 25% forest above the treeline. Context fixes
    # both, and needs no altitude rule to do it.
    elev = array.array("f")
    with io.open(dem_path, "rb") as f:
        f.seek(struct.calcsize(HFMT))
        elev.fromfile(f, w * h)

    OFF = [(-5,-5),(0,-5),(5,-5),(-5,0),(5,0),(-5,5),(0,5),(5,5),
           (-2,-2),(2,-2),(-2,2),(2,2),(-9,0),(9,0),(0,-9),(0,9)]
    lake = steep = 0
    for j in range(h):
        row = j * w
        for i in range(w):
            if cov[row + i] != SHADOW:
                continue
            # flat enough to hold water?
            if 0 < i < w-1 and 0 < j < h-1:
                dx = elev[row+i+1] - elev[row+i-1]
                dz = elev[row+w+i] - elev[row-w+i]
                grade = (dx*dx + dz*dz) ** 0.5 / (2.0 * mx)
            else:
                grade = 1.0
            if grade <= 0.04:
                cov[row + i] = WATER
                lake += 1
                continue
            votes = [0] * 7
            for dx_, dy_ in OFF:
                x, y = i + dx_, j + dy_
                if 0 <= x < w and 0 <= y < h:
                    c = cov[y * w + x]
                    if c not in (SHADOW, UNKNOWN):
                        votes[c] += 1
            cov[row + i] = votes.index(max(votes)) if max(votes) else ROCK
            steep += 1
    print("shadow: %d flat -> water, %d resolved from surroundings" % (lake, steep))
    # ------------------------------------------------- THE COLOUR OF THE GROUND
    # The complaint was "the mountains are still grey", and they were: bare
    # ground was one flat mat::ROCK. Colorado's alpine is not grey, it is tan,
    # buff, and rust, and that is sitting right there in the photograph. So the
    # ramp is BUILT FROM THE WINDOW'S OWN PIXELS rather than invented -- the top
    # RAMP_N colours of the bare ground, by frequency, and every bare sample
    # carries the index of the one nearest it.
    #
    # Only ROCK and SNOW feed the ramp. Forest and meadow already have their own
    # green ramps in the palette, and letting a million dark conifer pixels vote
    # would drag every entry towards black.
    print("\nbuilding the ground ramp from the imagery...")
    hist = {}
    for k in range(0, w * h, 3):
        if cov[k] in (ROCK, SNOW):
            rgb = rgbs[k]
            q = (rgb[0] >> 4, rgb[1] >> 4, rgb[2] >> 4)
            hist[q] = hist.get(q, 0) + 1
    # SPREAD ACROSS THE BRIGHTNESS RANGE, NOT THE TOP TEN BY FREQUENCY. The ten
    # commonest colours of a granite basin are ten nearly identical tans, and a
    # ramp of ten identical entries is a flat wash -- which is what the first
    # build looked like: correct in hue and dead in variation. Sorting the
    # candidates by luminance and taking ten evenly spaced percentiles keeps the
    # real hue while restoring the light-to-dark range that makes ground read as
    # ground. Weighted by frequency, so rare junk cannot claim an entry.
    lum = lambda q: 2 * q[0] + 5 * q[1] + q[2]
    cand = sorted(hist.items(), key=lambda kv: lum(kv[0]))
    total = sum(n for _, n in cand)
    ramp, acc, want, k = [], 0, total / float(RAMP_N * 2), 0
    for q, n in cand:
        acc += n
        while k < RAMP_N and acc >= want * (2 * k + 1):
            ramp.append(((q[0] << 4) | 8, (q[1] << 4) | 8, (q[2] << 4) | 8))
            k += 1
    while len(ramp) < RAMP_N:
        ramp.append(ramp[-1] if ramp else (150, 140, 120))
    for i, c in enumerate(ramp):
        print("   %2d  #%02x%02x%02x" % (i, c[0], c[1], c[2]))

    # nearest ramp entry per bare sample, packed into the low nibble
    for k in range(w * h):
        c = cov[k]
        if c in (ROCK, SNOW):
            r, g_, b = rgbs[k]
            best, bd = 0, 1 << 30
            for i, e in enumerate(ramp):
                d = (r-e[0])**2 + (g_-e[1])**2 + (b-e[2])**2
                if d < bd: bd, best = d, i
            cov[k] = (c << 4) | best
        else:
            cov[k] = c << 4

    flat = []
    for c in ramp:
        flat.extend(c)
    hdr = struct.pack("<8s2i6d i %dB 28i" % (RAMP_N * 3), b"VBCOV02", w, h,
                      olon, olat, slon, slat, mx, my, RAMP_N, *(flat + [0] * 28))
    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(bytes(cov))

    tot = float(w * h)
    print("\nclass mix:")
    for k, name in enumerate(NAMES):
        n = sum(1 for v in cov if (v >> 4) == k)
        if n:
            print("  %-8s %10d  %5.1f%%" % (name, n, 100.0 * n / tot))
    print("wrote   %s  (%.1f MB)" % (out_path, (len(hdr) + len(cov)) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
