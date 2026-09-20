"""geoimg_test -- prove tools/geoimg.py puts pixels in the right place.

    python geoimg_test.py [workdir]

WHAT IT IS ACTUALLY TESTING. Reading a TIFF is the easy half. The half that
fails silently is the GEOREFERENCING: a misread tie point, a wrong UTM zone or
a transposed pixel scale all produce a cover map that is complete, plausible
and describes the wrong piece of the earth. Nothing downstream can detect it.

So this writes tiles whose content is a known FUNCTION OF POSITION -- a band of
"water" at a known easting, a quadrant of "forest" -- reads them back through
LocalImagery at lon/lat computed independently, and checks the right thing came
out. It also runs every compression and band count the reader claims to take,
and checks that the ones it refuses are refused BY NAME rather than guessed at.
"""
import io, math, os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import geoimg


def write_tiff(path, w, h, spp, pixels, ox, oy, sx, sy, epsg, comp=1, pred=1):
    """A minimal 8-bit GeoTIFF: strips, one strip, optional deflate."""
    data = bytes(pixels)
    if pred == 2:
        b = bytearray(data)
        stride = w * spp
        for r in range(h):
            base = r * stride
            for x in range(stride - 1, spp - 1, -1):
                b[base + x] = (b[base + x] - b[base + x - spp]) & 0xFF
        data = bytes(b)
    if comp == 8:
        data = zlib.compress(data)

    # GeoKeyDirectory: version 1.1.0, 2 keys -> GTModelType=1, ProjectedCSType
    geokeys = [1, 1, 0, 2, 1024, 0, 1, 1, 3072, 0, 1, epsg]
    bps = [8] * spp

    entries = []          # (tag, type, count, payload-bytes-or-None)
    extra = bytearray()
    HDR = 8

    def stage(raw):
        """Put `raw` after the IFD and return its offset placeholder index."""
        extra.extend(raw)
        return len(extra) - len(raw)

    bps_off = stage(struct.pack("<%dH" % spp, *bps)) if spp > 2 else None
    scale_off = stage(struct.pack("<3d", sx, sy, 0.0))
    tie_off = stage(struct.pack("<6d", 0.0, 0.0, 0.0, ox, oy, 0.0))
    gk_off = stage(struct.pack("<%dH" % len(geokeys), *geokeys))

    tags = [
        (256, 4, 1, w),
        (257, 4, 1, h),
        (258, 3, spp, bps_off if bps_off is not None else bps[0]),
        (259, 3, 1, comp),
        (262, 3, 1, 2 if spp >= 3 else 1),
        (273, 4, 1, 0),            # strip offset, patched below
        (277, 3, 1, spp),
        (278, 4, 1, h),
        (279, 4, 1, len(data)),
        (317, 3, 1, pred),
        (33550, 12, 3, scale_off),
        (33922, 12, 6, tie_off),
        (34735, 3, len(geokeys), gk_off),
    ]
    tags.sort()
    ifd_size = 2 + 12 * len(tags) + 4
    extra_base = HDR + ifd_size
    strip_off = extra_base + len(extra)

    out = bytearray()
    out += b"II" + struct.pack("<HI", 42, HDR)
    out += struct.pack("<H", len(tags))
    OFFSET_TAGS = {258, 33550, 33922, 34735}
    for tid, typ, cnt, val in tags:
        if tid == 273:
            val = strip_off
        elif tid in OFFSET_TAGS and not (tid == 258 and spp <= 2):
            val = extra_base + val
        sz = {1: 1, 3: 2, 4: 4, 12: 8}[typ] * cnt
        if sz <= 4:
            fmt = {1: "<B", 3: "<H", 4: "<I"}[typ]
            pay = struct.pack(fmt, val).ljust(4, b"\x00")
        else:
            pay = struct.pack("<I", val)
        out += struct.pack("<HHI", tid, typ, cnt) + pay
    out += struct.pack("<I", 0)
    out += extra
    assert len(out) == strip_off, (len(out), strip_off)
    out += data
    io.open(path, "wb").write(bytes(out))


# ------------------------------------------------------------------ the fixture
#
# Ouachita's window, so the numbers line up with the rest of the toolchain:
# zone 15N, NAD83 -> EPSG:26915.
CLON, CLAT, EPSG, ZONE = -93.30, 34.66, 26915, 15
TILE_M = 2000          # 2 km tiles
PX_M = 0.5             # 50 cm pixels -> 4000 px a side... too big; see below
PX_M = 4.0             # 4 m pixels: 500 px a side, enough to place a band


def ground_truth(east, north):
    """What the fixture paints, as a function of POSITION ON THE EARTH.

    A 200 m band of water on a known easting, and forest north of a known
    northing. Both edges are straight lines in UTM, which is what makes a
    misplacement obvious: the band moves, or stops being straight."""
    band_e = 474000.0
    fence_n = 3836000.0
    if abs(east - band_e) < 100.0:
        return "water"
    return "forest" if north > fence_n else "rock"


def paint(spp, east0, north0, npx):
    px = bytearray(npx * npx * spp)
    for j in range(npx):
        north = north0 - (j + 0.5) * PX_M
        for i in range(npx):
            east = east0 + (i + 0.5) * PX_M
            what = ground_truth(east, north)
            if what == "water":
                r, g, b, n = 30, 60, 70, 12       # NIR absorbed -> NDWI high
            elif what == "forest":
                r, g, b, n = 40, 70, 35, 190      # dark + NDVI high
            else:
                r, g, b, n = 150, 145, 140, 150   # grey, NDVI flat
            o = (j * npx + i) * spp
            px[o] = r
            if spp > 1:
                px[o + 1] = g
            if spp > 2:
                px[o + 2] = b
            if spp > 3:
                px[o + 3] = n
    return px


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
        os.environ.get("TEMP", "."), "geoimg_fixture")
    os.makedirs(work, exist_ok=True)
    for n in os.listdir(work):
        if n.lower().endswith((".tif", ".tiff")):
            os.remove(os.path.join(work, n))

    npx = int(TILE_M / PX_M)
    # Two tiles side by side, so the index has to choose between them and the
    # seam between them is crossed by the read-back sweep.
    e0, n0 = 472000.0, 3838000.0
    layout = [(e0, n0), (e0 + TILE_M, n0)]
    bad = 0

    for variant, (comp, pred, spp) in enumerate(
            [(1, 1, 4), (8, 1, 4), (8, 2, 4), (1, 1, 3)]):
        for n in os.listdir(work):
            if n.lower().endswith(".tif"):
                os.remove(os.path.join(work, n))
        for k, (east0, north0) in enumerate(layout):
            write_tiff(os.path.join(work, "fixture_%d.tif" % k),
                       npx, npx, spp, paint(spp, east0, north0, npx),
                       east0, north0, PX_M, PX_M, EPSG, comp=comp, pred=pred)

        label = "comp=%d pred=%d spp=%d" % (comp, pred, spp)
        try:
            src = geoimg.LocalImagery(work, quiet=True)
        except Exception as ex:
            print("FAIL %-24s could not open: %s" % (label, ex))
            bad += 1
            continue
        if src.has_nir != (spp >= 4):
            print("FAIL %-24s has_nir reported %s" % (label, src.has_nir))
            bad += 1

        # ---- read back at lon/lat, not at pixels -------------------------
        wrong = 0
        checked = 0
        for je in range(60):
            east = e0 + 100.0 + je * 63.0
            for jn in range(40):
                north = n0 - 100.0 - jn * 45.0
                lon, lat = geoimg.utm_inverse(ZONE, False, east, north)
                got = src.at(lon, lat)
                if got is None:
                    continue
                checked += 1
                r, g, b, nir = got
                want = ground_truth(east, north)
                # Classify by the same logic the real classifier uses, reduced
                # to the three things this fixture paints.
                ndvi = (nir - r) / float(nir + r + 1)
                ndwi = (g - nir) / float(g + nir + 1)
                if spp >= 4:
                    saw = "water" if ndwi > 0.05 else ("forest" if ndvi > 0.14 else "rock")
                else:
                    # No NIR: nir==r, so both indices collapse. Fall back to
                    # colour, which is all a 3-band tile ever had.
                    saw = "water" if b > r else ("forest" if g > r else "rock")
                if saw != want:
                    wrong += 1
        if checked < 1500:
            print("FAIL %-24s only %d samples landed on a tile" % (label, checked))
            bad += 1
        elif wrong:
            print("FAIL %-24s %d/%d samples read the wrong ground"
                  % (label, wrong, checked))
            bad += 1
        else:
            print("ok   %-24s %d samples, all on the right ground" % (label, checked))

    # ---- and the refusals, which must name themselves --------------------
    for n in os.listdir(work):
        if n.lower().endswith(".tif"):
            os.remove(os.path.join(work, n))
    write_tiff(os.path.join(work, "jpeg.tif"), 16, 16, 3, bytearray(16 * 16 * 3),
               e0, n0, PX_M, PX_M, EPSG, comp=7)
    try:
        geoimg.GeoTile(os.path.join(work, "jpeg.tif"))
        print("FAIL JPEG-in-TIFF was accepted")
        bad += 1
    except ValueError as ex:
        print("ok   refused by name        %s" % ex)
    # A projection that is neither geographic nor UTM: NAD83 / Arkansas North.
    write_tiff(os.path.join(work, "spcs.tif"), 16, 16, 4, bytearray(16 * 16 * 4),
               e0, n0, PX_M, PX_M, 26951)
    try:
        geoimg.GeoTile(os.path.join(work, "spcs.tif"))
        print("FAIL State Plane was accepted")
        bad += 1
    except ValueError as ex:
        print("ok   refused by name        %s" % ex)

    print("\ngeoimg_test %s" % ("FAILED" if bad else "PASS"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
