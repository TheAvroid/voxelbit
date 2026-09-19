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
    # NDWI -- the standard water index. Water absorbs near infrared, so green
    # exceeds NIR over it; land does the opposite.
    ndwi = (g_ - nir) / float(g_ + nir + 1)
    # Water first: NIR is absorbed almost completely, so it is the one class
    # that is unambiguous. Snow next, before vegetation, because bright snow
    # over trees would otherwise score as meadow.
    # ------------------------------------------------- WATER, BY NDWI FIRST
    # The old test was `nir < 35 and vis < 72` -- DARK water only. Colorado's
    # alpine lakes are bright blue and turquoise, sailed straight past it, and
    # landed in ROCK: Grand Lake, the largest natural lake in the state, came
    # out as rock, and then took a blue-grey entry off the ground ramp. That is
    # the "blue terrain that should be water".
    #
    # NDWI catches water at any brightness because it keys on the one thing
    # water always does -- absorb near infrared. Measured over Grand Lake's
    # centre: median NDWI +0.14, 61% of pixels above +0.05.
    #
    # Capped at vis 170 so sun glint stays with the snow test below, and gated
    # on ndvi so a wet meadow does not become a pond. Still returns SHADOW, not
    # WATER: the flatness check downstream is what separates a lake from a wet
    # rock face, and that check has already earned its place twice.
    if ndwi > 0.05 and vis < 170 and ndvi < 0.20:
        return SHADOW
    # Dark and infrared-dead: deep or shadowed water, and shade under a spruce,
    # which are the same three numbers. Also resolved downstream.
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
    # THE WINDOW'S PIXELS, RGBA, kept so the blocks can be blended into each
    # other and the ramp built from what was actually classified. Four bytes a
    # cell -- 94 MB on rmnp50 -- and it REPLACES a list of w*h three-tuples,
    # which was gigabytes of object header for the same information.
    pix = bytearray(w * h * 4)
    seen = bytearray(w * h)
    tmp = os.path.join(os.environ.get("TEMP", "."), "_naip_block.tif")

    # -----------------------------------------------------------------------
    # FULL RESOLUTION, AND NO SEAM: OVERLAPPING BLOCKS, CROSS-FADED.
    #
    # (user 2026-09-18, in order: "odd generation formations like this straight
    # line", then "the data appears as squares that are visible. can we get more
    # detail?")
    #
    # THE TWO COMPLAINTS ARE THE SAME TRADE AND THIS IS THE WAY OUT OF IT.
    # exportImage caps a request at 2048 px, so a 4,859-sample window is either
    # ONE coarse request -- 24.4 m a sample, which at shrink 6 is four world
    # metres of flat colour, the "squares" -- or several full-resolution ones
    # with a JOIN, and the join is where the straight line came from.
    #
    # WHAT THE JOIN ACTUALLY IS, measured rather than assumed. Fetching the same
    # 512 m of ground as two requests and as one:
    #
    #     separate requests, across the join   39.5% of pixels change class
    #     ONE request, across the same join    22.5%   <- the natural variation
    #     a +2 DN shift on r, g, b              4.0%
    #
    # So the difference in the PIXELS is about two levels out of 255 -- the
    # service resamples per request and that is all it costs -- and the
    # classifier turns those two levels into seventeen points of class change,
    # correlated along the whole row. Correlated is what the eye sees. An
    # exposure match alone was tried and measured: it moves 4% and leaves the
    # line.
    #
    # SO THE BLOCKS OVERLAP AND FADE INTO EACH OTHER. Each request is 2048 wide
    # but they step by kStride, so every join has kOverlap of ground that both
    # blocks saw; across that band the pixels are mixed linearly, which turns a
    # straight discontinuity into a gradient a kilometre wide. The classifier
    # still flips whatever it flips, but it flips it RAGGEDLY and over ground
    # that already varies by 22% row to row.
    #
    # AND THE EXPOSURE IS MATCHED FIRST, cheaply: one reference request covers
    # the whole window in a single exposure, and each block is mapped onto it
    # with ONE 256-entry table -- bytes.translate, a C-speed pass -- built from
    # the mean and spread of all four channels together. Per-channel tables
    # would be a per-pixel Python loop over 37 M pixels; the channels differ by
    # less than a level here (x0.95/0.94/0.93/0.92), so one table takes almost
    # all of it.
    # -----------------------------------------------------------------------
    MAXPX = 2048
    kOverlap = 192
    kStride = MAXPX - kOverlap
    rw, rh = min(w, MAXPX), min(h, MAXPX)
    west, east = olon, olon + slon * w
    north, south = olat, olat - slat * h
    print("exposure  one reference request of %dx%d over the whole window" % (rw, rh))
    ref_tmp = os.path.join(os.environ.get("TEMP", "."), "_naip_ref.tif")
    if not fetch((west, south, east, north), (rw, rh), ref_tmp):
        print("    FAILED -- the cover would be empty, so nothing is written")
        return 1
    rtw, rth, rspp, rpx = read_tiff_rgbn(ref_tmp)

    def stats(buf, spp_, x0, y0, bw, bh, stride, step):
        """mean and spread over all channels of a rectangle, every `step` px."""
        n = 0
        s = 0.0
        q = 0.0
        for yy in range(y0, y0 + bh, step):
            base = yy * stride * spp_
            for xx in range(x0, x0 + bw, step):
                o = base + xx * spp_
                for c in range(min(4, spp_)):
                    v = buf[o + c]
                    s += v
                    q += v * v
                n += min(4, spp_)
        if n == 0:
            return 128.0, 1.0
        mean = s / n
        return mean, max(1.0, (q / n - mean * mean) ** 0.5)

    nbx = max(1, (w - kOverlap + kStride - 1) // kStride)
    nby = max(1, (h - kOverlap + kStride - 1) // kStride)
    print("tiles   %dx%d requests of %dpx stepping %d (%.2f m a sample, %d px of overlap)"
          % (nbx, nby, MAXPX, kStride, mx, kOverlap))

    for by in range(nby):
        for bx in range(nbx):
            x0, y0 = bx * kStride, by * kStride
            bw, bh = min(MAXPX, w - x0), min(MAXPX, h - y0)
            if bw <= 0 or bh <= 0:
                continue
            bwest, beast = olon + slon * x0, olon + slon * (x0 + bw)
            bnorth, bsouth = olat - slat * y0, olat - slat * (y0 + bh)
            print("  block %d,%d  %dx%d at %d,%d" % (bx, by, bw, bh, x0, y0))
            if not fetch((bwest, bsouth, beast, bnorth), (bw, bh), tmp):
                print("    FAILED -- left as unknown")
                continue
            tw, th, spp, px = read_tiff_rgbn(tmp)
            if tw != bw or th != bh:
                print("    got %dx%d, wanted %dx%d" % (tw, th, bw, bh))
            bm, bs = stats(px, spp, 0, 0, min(tw, bw), min(th, bh), tw, 16)
            rx0, ry0 = x0 * rtw // w, y0 * rth // h
            rbw, rbh = max(1, bw * rtw // w), max(1, bh * rth // h)
            rm, rs = stats(rpx, rspp, rx0, ry0, min(rbw, rtw - rx0), min(rbh, rth - ry0), rtw, 4)
            gain = min(2.5, max(0.4, rs / bs))
            print("    exposure x%.3f, shift %+.1f" % (gain, rm - bm))
            lut = bytes(min(255, max(0, int((v - bm) * gain + rm + 0.5))) for v in range(256))
            px = bytes(px).translate(lut)
            # ---- write it in, fading across the overlap ---------------------
            for r in range(min(th, bh)):
                gy = y0 + r
                ty = 1.0 if (by == 0 or r >= kOverlap) else (r + 0.5) / kOverlap
                srow = r * tw * spp
                drow = gy * w
                if ty >= 1.0 and bx == 0:
                    # Nothing to fade against on this row: one memcpy.
                    n = min(tw, bw)
                    pix[drow * 4:(drow + n) * 4] = px[srow:srow + n * 4]
                    for i in range(n):
                        seen[drow + i] = 1
                    continue
                for c in range(min(tw, bw)):
                    gx = x0 + c
                    tx = 1.0 if (bx == 0 or c >= kOverlap) else (c + 0.5) / kOverlap
                    t = tx * ty
                    o = srow + c * spp
                    k = drow + gx            # the CELL, not the block-local one
                    d = k * 4
                    if not seen[k] or t >= 1.0:
                        pix[d] = px[o]
                        pix[d+1] = px[o+1]
                        pix[d+2] = px[o+2]
                        pix[d+3] = px[o+3] if spp > 3 else px[o]
                        seen[k] = 1
                    else:
                        u = 1.0 - t
                        pix[d] = int(pix[d] * u + px[o] * t)
                        pix[d+1] = int(pix[d+1] * u + px[o+1] * t)
                        pix[d+2] = int(pix[d+2] * u + px[o+2] * t)
                        nv = px[o+3] if spp > 3 else px[o]
                        pix[d+3] = int(pix[d+3] * u + nv * t)
    try:
        os.remove(tmp)
        os.remove(ref_tmp)
    except OSError:
        pass

    # ---- and now classify the whole window from one consistent image --------
    print("classifying %d samples..." % (w * h))
    for k in range(w * h):
        o = k * 4
        cov[k] = classify(pix[o], pix[o+1], pix[o+2], pix[o+3])

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
    # -----------------------------------------------------------------------
    # THE DEM KNOWS WHERE THE WATER IS. THE PHOTOGRAPH ONLY GUESSES.
    #
    # (user 2026-09-18, with a picture of a flat grey polygon lying in the
    # middle of a wood: "heres some more artifacting" / "it should be able to
    # detect and fix the terrain artifacts".)
    #
    # THAT POLYGON IS A LAKE. USGS HYDRO-FLATTENS water surfaces: every posting
    # inside a mapped water body is written at ONE elevation, TO THE BIT. So a
    # lake in a .vbdem is a connected region of exactly equal float -- which is
    # a thing that does not otherwise happen in a lidar or photogrammetric
    # surface, where even a car park wanders by centimetres. When the imagery
    # classifier misses it, the engine draws that surface as what it is: dead
    # flat bare ground, with straight edges where the hydrography polygon ran.
    #
    # MEASURED on rmnp50: 225 regions of 60 postings or more, 323,846 samples,
    # 3,429 ha of it. The biggest is 178,647 samples at 2,522.65 m -- Lake
    # Granby -- and the imagery had only 75.8% of that as water. Several
    # 20-to-70 ha lakes were at 0.1%.
    #
    # THE DRAIN TEST IS ABOUT DEPTH, NOT COUNT, AND THAT IS THE WHOLE TRICK.
    # Water sits in a hollow, so the first cut of this refused any region with
    # a lower neighbour -- and it threw away every lake in the window, Granby
    # included. The reason is the reservoir shoreline: 43% of Granby's 38,468
    # rim cells read BELOW full pool, because the drawdown zone is exposed lake
    # bed and the resampled waterline lands a centimetre or two under. Measured
    # across all 225 regions, the worst drop anywhere on any rim is under 5 cm.
    #
    # A bench -- a flat spot on a hillside, a mine bench, a runway -- is not
    # like that. It is CUT INTO a slope, so its downhill side falls metres
    # within one posting. So the test is kDrainDropM, and on this window it
    # rejects nothing at all, which is exactly the point: it is the guard that
    # lets bit-exact flatness do the work.
    #
    # It runs AFTER the shadow resolution, so the photograph has already had
    # its say, and BEFORE the shore bake, so the banks are measured from the
    # corrected water.
    # -----------------------------------------------------------------------
    kFlatMinCells = 60          # 0.6 ha at a 10 m posting -- a pond, not a step
    kDrainDropM = 0.25          # a rim cell this far down means it is not water
    kDrainMaxFrac = 0.02        # and a couple of those is an outlet, not a slope
    print("\nflat water from the DEM (hydro-flattened surfaces)...")
    demg = array.array("f")
    with io.open(dem_path, "rb") as fdem:
        fdem.seek(struct.calcsize(HFMT))
        demg.fromfile(fdem, w * h)
    flatseen = bytearray(w * h)
    lakes = 0
    moved = 0
    area = 0
    for s in range(w * h):
        if flatseen[s]:
            continue
        e = demg[s]
        stack = [s]
        flatseen[s] = 1
        cells = []
        edge = 0
        drains = 0
        while stack:
            k = stack.pop()
            cells.append(k)
            i, j = k % w, k // w
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                a, b = i + di, j + dj
                if a < 0 or b < 0 or a >= w or b >= h:
                    continue
                t = b * w + a
                v = demg[t]
                if v == e:
                    if not flatseen[t]:
                        flatseen[t] = 1
                        stack.append(t)
                else:
                    edge += 1
                    if e - v > kDrainDropM:
                        drains += 1
        if len(cells) < kFlatMinCells:
            continue
        if edge and float(drains) / float(edge) > kDrainMaxFrac:
            continue
        lakes += 1
        area += len(cells)
        for k in cells:
            if cov[k] != WATER:
                moved += 1
            cov[k] = WATER
    print("   %d flat bodies, %d samples, %.0f ha -- %d of them the photograph"
          " had called something else"
          % (lakes, area, area * mx * my / 10000.0, moved))

    # -----------------------------------------------------------------------
    # AND THE SAME RULE BACKWARDS: WATER THAT IS NOT ON A FLAT SURFACE IS NOT
    # WATER.
    #
    # The pass above puts water back where the photograph missed a lake. This
    # one takes it away where the photograph invented one, and it is the same
    # question asked the other way round -- the DEM gets the last word both
    # times, because standing water is level and a photograph does not know
    # that.
    #
    # WHAT THE PHOTOGRAPH GETS WRONG is dark, blue-grey and high up: cloud
    # shadow on rock, wet talus, and above all SNOW IN SHADOW, which sits in
    # the same corner of colour space as deep water. Measured on rmnp50 before
    # this existed: 14.8% of every water sample stood on ground steeper than
    # 2%, and 2.6% of it on ground steeper than 12%, which is a hillside.
    # Found by standing at 4,103 m asl -- above every lake in Colorado, 240 m
    # under the highest summit in the window -- in an ocean with waves on it.
    #
    # THE TEST IS ANCHORING, NOT SLOPE, and that distinction is the whole
    # reason this works. A slope threshold would eat the SHORE of every real
    # lake, because a rim cell's central difference straddles the bank and
    # reads steep. So the water is flooded into connected bodies first, and a
    # body is kept if enough of it stands on ground the DEM has hydro-flattened
    # -- if it is anchored to a surface that is flat to the bit.
    #
    # IT SEPARATES CLEANLY, which is how you know it is the right question.
    # 336 bodies came back anchored, at 70-97% flat cells each, and they are
    # the lakes. 20,225 came back unanchored at ZERO flat cells -- 78,968
    # samples, 17.7% of all the water, at a median of four samples a body,
    # which is what speckle looks like. The largest was 12,076 samples with
    # 55 m of relief across it, and the one after that had 129.5 m. Nothing
    # sat in between; no body was a close call.
    # -----------------------------------------------------------------------
    kAnchorFrac = 0.10          # a tenth of the body on hydro-flat ground
    print("\nwater the photograph invented (not anchored to a flat surface)...")
    # A cell is "flat" if it agrees exactly with most of its neighbours. This is
    # the cheap local form of the flood fill above -- it does not need the
    # region, only the evidence that one is there.
    flat = bytearray(w * h)
    for j in range(1, h - 1):
        b = j * w
        for i in range(1, w - 1):
            k = b + i
            e = demg[k]
            if (demg[k - 1] == e) + (demg[k + 1] == e) + \
               (demg[k - w] == e) + (demg[k + w] == e) >= 3:
                flat[k] = 1
    wseen = bytearray(w * h)
    kept = 0
    dropped = 0
    dropcells = 0
    for s in range(w * h):
        if wseen[s] or cov[s] != WATER:
            continue
        stack = [s]
        wseen[s] = 1
        cells = []
        nflat = 0
        while stack:
            k = stack.pop()
            cells.append(k)
            nflat += flat[k]
            i, j = k % w, k // w
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                a, b = i + di, j + dj
                if a < 0 or b < 0 or a >= w or b >= h:
                    continue
                t = b * w + a
                if not wseen[t] and cov[t] == WATER:
                    wseen[t] = 1
                    stack.append(t)
        if nflat >= kAnchorFrac * len(cells):
            kept += 1
            continue
        # NOT A LAKE. Give the ground back to whatever surrounds it, so a
        # misread snowfield becomes snow and a misread scree becomes rock,
        # rather than everything becoming one default class.
        ring = {}
        for k in cells:
            i, j = k % w, k // w
            for di, dj in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                a, b = i + di, j + dj
                if a < 0 or b < 0 or a >= w or b >= h:
                    continue
                c = cov[b * w + a]
                if c != WATER:
                    ring[c] = ring.get(c, 0) + 1
        fill = ROCK
        if ring:
            fill = max(ring.items(), key=lambda p: p[1])[0]
        for k in cells:
            cov[k] = fill
        dropped += 1
        dropcells += len(cells)
    print("   %d bodies kept, %d dropped (%d samples, %.1f%% of the water)"
          % (kept, dropped, dropcells,
             100.0 * dropcells / max(1, dropcells + sum(1 for k in range(w * h)
                                                        if cov[k] == WATER))))


    print("\nbuilding the ground ramp from the imagery...")
    hist = {}
    for k in range(0, w * h, 3):
        if cov[k] in (ROCK, SNOW):
            o = k * 4
            q = (pix[o] >> 4, pix[o+1] >> 4, pix[o+2] >> 4)
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
            o = k * 4
            r, g_, b = pix[o], pix[o+1], pix[o+2]
            best, bd = 0, 1 << 30
            for i, e in enumerate(ramp):
                d = (r-e[0])**2 + (g_-e[1])**2 + (b-e[2])**2
                if d < bd: bd, best = d, i
            cov[k] = (c << 4) | best
        else:
            cov[k] = c << 4

    # ---------------------------------------------- DISTANCE TO SHORE, BAKED
    # The engine used to measure this at runtime, inside heightM, by ringing
    # outward until it left the water: about 950 lookups PER COLUMN in the
    # hottest function there is. That was the hitching. It was also the jagged
    # lake bed, because a ring search returns a QUANTISED distance and the
    # engine's cover sampler jitters, so the depth stepped and wobbled.
    #
    # A two-pass chamfer transform does the whole grid in O(n) once, here, and
    # the engine reads one byte and interpolates it. Distances are in REAL
    # metres, clamped to 255, so the engine keeps its own depth curve.
    print("baking the distance to shore...")
    INF = 1 << 30
    dist = array.array("i", [0]) * 0
    dist = array.array("i", bytes(4 * w * h))
    for k in range(w * h):
        dist[k] = 0 if (cov[k] >> 4) != WATER else INF
    D1, D2 = 10, 14                      # chamfer 3x4 weights, /10
    for j in range(h):
        row = j * w
        for i in range(w):
            k = row + i
            if dist[k] == 0: continue
            best = dist[k]
            if i: best = min(best, dist[k-1] + D1)
            if j:
                best = min(best, dist[k-w] + D1)
                if i:     best = min(best, dist[k-w-1] + D2)
                if i<w-1: best = min(best, dist[k-w+1] + D2)
            dist[k] = best
    for j in range(h - 1, -1, -1):
        row = j * w
        for i in range(w - 1, -1, -1):
            k = row + i
            if dist[k] == 0: continue
            best = dist[k]
            if i<w-1: best = min(best, dist[k+1] + D1)
            if j<h-1:
                best = min(best, dist[k+w] + D1)
                if i:     best = min(best, dist[k+w-1] + D2)
                if i<w-1: best = min(best, dist[k+w+1] + D2)
            dist[k] = best
    shore = bytearray(w * h)
    deepest = 0
    for k in range(w * h):
        d = dist[k]
        if d >= INF: d = 255 * 10
        m = int(d * mx / 10.0)           # chamfer units -> real metres
        if m > 255: m = 255
        shore[k] = m
        if m > deepest: deepest = m
    print("   farthest any water is from a shore: %d m" % deepest)

    flat = []
    for c in ramp:
        flat.extend(c)
    hdr = struct.pack("<8s2i6d i %dB 28i" % (RAMP_N * 3), b"VBCOV03", w, h,
                      olon, olat, slon, slat, mx, my, RAMP_N, *(flat + [0] * 28))
    with open(out_path, "wb") as f:
        f.write(hdr)
        f.write(bytes(cov))
        f.write(bytes(shore))   # second plane: distance to shore, real metres

    tot = float(w * h)
    print("\nclass mix:")
    for k, name in enumerate(NAMES):
        n = sum(1 for v in cov if (v >> 4) == k)
        if n:
            print("  %-8s %10d  %5.1f%%" % (name, n, 100.0 * n / tot))
    print("wrote   %s  (%.1f MB)" % (out_path, (len(hdr) + len(cov) + len(shore)) / 1048576.0))
    return 0


if __name__ == "__main__":
    sys.exit(main())
