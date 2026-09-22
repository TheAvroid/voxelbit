"""geoimg -- read a directory of georeferenced 8-bit imagery tiles.

WHY THERE IS A SECOND IMAGERY SOURCE AT ALL. naip2cov's other path asks
exportImage for the window, and that service caps a request at 2048 px and
RESAMPLES per request. Everything ugly downstream follows from those two
facts: one coarse request gives flat squares of colour, several give a JOIN,
and the whole overlap-and-cross-fade machinery in naip2cov.main exists to hide
the join. It also pins the imagery's resolution to whatever the cap works out
to over the window -- which was tolerable against a 10.29 m posting and is not
against a 1 m one.

A DIRECTORY OF SUPPLIER TILES HAS NEITHER PROBLEM. The pixels are the ones
that were flown: no resampling, no exposure drift between requests, no join.
State orthoimagery runs to 15 cm, so a 1 m posting contains 44 real samples
instead of a fraction of one.

    Maine      15 cm statewide, MEGIS GeoLibrary
    Arkansas   ADOP 6-inch statewide, 4-band RGB+NIR  (and 3-inch in places)

THE FOURTH BAND IS THE POINT, not the resolution. NDVI and NDWI are what
separate a granite dome from a dry meadow and a lake from a shadow; see the
classifier in naip2cov.py. A 3-band tile is read and used, but it comes with
no NIR, so `at()` reports that and the caller must not pretend otherwise.

CRS: geographic and UTM are read. ANYTHING ELSE IS REFUSED BY NAME -- a
misread projection puts the window hundreds of kilometres away and still
returns a perfectly plausible-looking cover, which is the same pixel-for-pixel
silent failure the floating-point predictor gave dem2raw.cpp. State Plane in
particular is common in supplier deliveries and is NOT handled; reproject to
UTM or geographic before pointing this at it.
"""
import io, math, os, struct, zlib

A_WGS = 6378137.0
F_WGS = 1.0 / 298.257223563
E2_WGS = F_WGS * (2.0 - F_WGS)
K0_UTM = 0.9996


def utm_zone_for(epsg):
    """EPSG -> (zone, south) or None. Same table as tools/dem2raw.cpp."""
    if 26901 <= epsg <= 26923:
        return (epsg - 26900, False)
    if 32601 <= epsg <= 32660:
        return (epsg - 32600, False)
    if 32701 <= epsg <= 32760:
        return (epsg - 32700, True)
    if 6330 <= epsg <= 6348:
        return (epsg - 6329, False)
    return None


def utm_forward(zone, south, lon, lat):
    """lon/lat degrees -> easting/northing metres. Snyder; sub-mm inside a zone."""
    lon0 = math.radians(zone * 6 - 183)
    phi, lam = math.radians(lat), math.radians(lon)
    e2 = E2_WGS
    ep2 = e2 / (1.0 - e2)
    sp, cp, tp = math.sin(phi), math.cos(phi), math.tan(phi)
    N = A_WGS / math.sqrt(1.0 - e2 * sp * sp)
    T, C, A = tp * tp, ep2 * cp * cp, (lam - lon0) * cp
    M = A_WGS * ((1 - e2 / 4 - 3 * e2 ** 2 / 64 - 5 * e2 ** 3 / 256) * phi
                 - (3 * e2 / 8 + 3 * e2 ** 2 / 32 + 45 * e2 ** 3 / 1024) * math.sin(2 * phi)
                 + (15 * e2 ** 2 / 256 + 45 * e2 ** 3 / 1024) * math.sin(4 * phi)
                 - (35 * e2 ** 3 / 3072) * math.sin(6 * phi))
    east = K0_UTM * N * (A + (1 - T + C) * A ** 3 / 6
                         + (5 - 18 * T + T * T + 72 * C - 58 * ep2) * A ** 5 / 120) + 500000.0
    north = K0_UTM * (M + N * tp * (A * A / 2
                      + (5 - T + 9 * C + 4 * C * C) * A ** 4 / 24
                      + (61 - 58 * T + T * T + 600 * C - 330 * ep2) * A ** 6 / 720))
    if south:
        north += 10000000.0
    return east, north


def utm_inverse(zone, south, east, north):
    """easting/northing -> lon/lat. Needed once per tile, to place its bbox."""
    lon0 = math.radians(zone * 6 - 183)
    if south:
        north -= 10000000.0
    e2 = E2_WGS
    ep2 = e2 / (1.0 - e2)
    M = north / K0_UTM
    mu = M / (A_WGS * (1 - e2 / 4 - 3 * e2 ** 2 / 64 - 5 * e2 ** 3 / 256))
    e1 = (1 - math.sqrt(1 - e2)) / (1 + math.sqrt(1 - e2))
    p1 = (mu + (3 * e1 / 2 - 27 * e1 ** 3 / 32) * math.sin(2 * mu)
          + (21 * e1 ** 2 / 16 - 55 * e1 ** 4 / 32) * math.sin(4 * mu)
          + (151 * e1 ** 3 / 96) * math.sin(6 * mu)
          + (1097 * e1 ** 4 / 512) * math.sin(8 * mu))
    sp, cp, tp = math.sin(p1), math.cos(p1), math.tan(p1)
    C1, T1 = ep2 * cp * cp, tp * tp
    N1 = A_WGS / math.sqrt(1 - e2 * sp * sp)
    R1 = A_WGS * (1 - e2) / (1 - e2 * sp * sp) ** 1.5
    D = (east - 500000.0) / (N1 * K0_UTM)
    phi = p1 - (N1 * tp / R1) * (D ** 2 / 2
                                 - (5 + 3 * T1 + 10 * C1 - 4 * C1 * C1 - 9 * ep2) * D ** 4 / 24
                                 + (61 + 90 * T1 + 298 * C1 + 45 * T1 * T1
                                    - 252 * ep2 - 3 * C1 * C1) * D ** 6 / 720)
    lam = lon0 + (D - (1 + 2 * T1 + C1) * D ** 3 / 6
                  + (5 - 2 * C1 + 28 * T1 - 3 * C1 * C1 + 8 * ep2
                     + 24 * T1 * T1) * D ** 5 / 120) / cp
    return math.degrees(lam), math.degrees(phi)


def _lzw(data):
    """TIFF LZW: MSB-first codes, early change. Same variant as dem2raw.cpp."""
    out = bytearray()
    base = [bytes([i]) for i in range(256)] + [b"", b""]
    dic = list(base)
    width, pos, prev = 9, 0, b""
    n = len(data)
    nbits = n * 8
    while pos + width <= nbits:
        byte, bit = pos >> 3, pos & 7
        chunk = data[byte] << 16
        if byte + 1 < n:
            chunk |= data[byte + 1] << 8
        if byte + 2 < n:
            chunk |= data[byte + 2]
        code = (chunk >> (24 - bit - width)) & ((1 << width) - 1)
        pos += width
        if code == 256:
            dic = list(base)
            width, prev = 9, b""
            continue
        if code == 257:
            break
        if code < len(dic) and not (256 <= code < 258):
            entry = dic[code]
        elif prev:
            entry = prev + prev[:1]
        else:
            break
        out += entry
        if prev:
            dic.append(prev + entry[:1])
        prev = entry
        if len(dic) + 1 >= (1 << width) and width < 12:
            width += 1
    return bytes(out)


def _unpredict(buf, w, spp, rows):
    """TIFF predictor 2, horizontal differencing, 8-bit."""
    stride = w * spp
    for r in range(rows):
        b = r * stride
        for x in range(spp, stride):
            buf[b + x] = (buf[b + x] + buf[b + x - spp]) & 0xFF


class GeoTile:
    """One georeferenced 8-bit tile: where it is, and its pixels."""

    def __init__(self, path):
        self.path = path
        d = io.open(path, "rb").read()
        if d[:2] == b"II":
            en = "<"
        elif d[:2] == b"MM":
            en = ">"
        else:
            raise ValueError("not a TIFF")
        if struct.unpack_from(en + "H", d, 2)[0] != 42:
            raise ValueError("BigTIFF is not read here")

        SZ = {1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 11: 4, 12: 8}
        FMT = {1: "B", 2: "B", 3: "H", 4: "I", 11: "f", 12: "d"}
        tags = {}
        off = struct.unpack_from(en + "I", d, 4)[0]
        for i in range(struct.unpack_from(en + "H", d, off)[0]):
            e = off + 2 + i * 12
            tid, typ, cnt = struct.unpack_from(en + "HHI", d, e)
            sz = SZ.get(typ, 0)
            if not sz:
                continue
            base = e + 8 if sz * cnt <= 4 else struct.unpack_from(en + "I", d, e + 8)[0]
            if typ == 5:
                pairs = struct.unpack_from(en + str(cnt * 2) + "I", d, base)
                vals = [pairs[j * 2] / pairs[j * 2 + 1] if pairs[j * 2 + 1] else 0.0
                        for j in range(cnt)]
            else:
                vals = list(struct.unpack_from(en + str(cnt) + FMT[typ], d, base))
            tags[tid] = vals
        g = lambda t, dflt=None: tags[t][0] if t in tags else dflt

        self.w, self.h = g(256), g(257)
        self.spp = g(277, 1)
        self.comp = g(259, 1)
        self.pred = g(317, 1)
        bps = g(258, 8)
        if bps != 8:
            raise ValueError("%d-bit samples; imagery must be 8-bit" % bps)
        if self.comp in (6, 7, 34712):
            raise ValueError("JPEG-in-TIFF; re-export as raw, LZW or deflate")
        if self.comp not in (1, 5, 8, 32946):
            raise ValueError("unsupported compression %d" % self.comp)

        scale, tie = tags.get(33550), tags.get(33922)
        if not scale or not tie or len(tie) < 6:
            raise ValueError("no ModelPixelScale/ModelTiepoint; not georeferenced")
        self.sx, self.sy = float(scale[0]), float(scale[1])
        self.ox, self.oy = float(tie[3]), float(tie[4])

        self.epsg, self.zone, self.south, self.projected = 0, 0, False, False
        gk = tags.get(34735)
        if gk and len(gk) >= 8:
            for i in range(gk[3]):
                b = 4 + i * 4
                if b + 3 >= len(gk):
                    break
                if gk[b] == 3072 and gk[b + 1] == 0:
                    self.epsg = gk[b + 3]
        if self.epsg:
            z = utm_zone_for(self.epsg)
            if not z:
                raise ValueError("EPSG:%d is neither geographic nor UTM" % self.epsg)
            self.zone, self.south = z
            self.projected = True

        self.px = bytearray(self.w * self.h * self.spp)
        if 324 in tags:
            tw, th = g(322), g(323)
            across = (self.w + tw - 1) // tw
            offs, cnts = tags[324], tags.get(325, [])
            for idx, toff in enumerate(offs):
                blk = self._block(d, toff, cnts[idx] if idx < len(cnts) else 0, tw, th)
                tx, ty = (idx % across) * tw, (idx // across) * th
                ncol = min(tw, self.w - tx)
                if ncol <= 0:
                    continue
                for r in range(th):
                    y = ty + r
                    if y >= self.h:
                        break
                    src = r * tw * self.spp
                    dst = (y * self.w + tx) * self.spp
                    self.px[dst:dst + ncol * self.spp] = blk[src:src + ncol * self.spp]
        else:
            rps = g(278, self.h)
            offs, cnts = tags[273], tags.get(279, [])
            for idx, soff in enumerate(offs):
                y0 = idx * rps
                rows = min(rps, self.h - y0)
                if rows <= 0:
                    break
                blk = self._block(d, soff, cnts[idx] if idx < len(cnts) else 0, self.w, rows)
                o = y0 * self.w * self.spp
                self.px[o:o + rows * self.w * self.spp] = blk[:rows * self.w * self.spp]
        del d
        self.lo0, self.la0, self.lo1, self.la1 = self._bbox()

    def _block(self, d, off, cnt, bw, bh):
        raw = d[off:off + cnt] if cnt else d[off:]
        if self.comp == 1:
            out = bytearray(raw[:bw * bh * self.spp])
        elif self.comp == 5:
            out = bytearray(_lzw(raw))
        else:
            out = bytearray(zlib.decompress(raw))
        need = bw * bh * self.spp
        if len(out) < need:
            out.extend(bytes(need - len(out)))
        if self.pred == 2:
            _unpredict(out, bw, self.spp, bh)
        return out

    def _bbox(self):
        """A LON/LAT BOX ROUND A UTM TILE IS NOT ITS FOUR CORNERS -- grid north
        is not true north away from the central meridian, so the extreme
        longitude lies on an edge. The whole border is walked; it is only an
        index, and `at()` is exact."""
        x0, x1 = self.ox, self.ox + self.sx * self.w
        y0, y1 = self.oy - self.sy * self.h, self.oy
        if not self.projected:
            return x0, y0, x1, y1
        los, las = [], []
        for k in range(17):
            t = k / 16.0
            for px, py in ((x0 + (x1 - x0) * t, y0), (x0 + (x1 - x0) * t, y1),
                           (x0, y0 + (y1 - y0) * t), (x1, y0 + (y1 - y0) * t)):
                lo, la = utm_inverse(self.zone, self.south, px, py)
                los.append(lo)
                las.append(la)
        return min(los), min(las), max(los), max(las)

    def at(self, lon, lat):
        """Nearest source pixel as (r, g, b, nir), or None if outside."""
        if lon < self.lo0 or lon > self.lo1 or lat < self.la0 or lat > self.la1:
            return None
        if self.projected:
            x, y = utm_forward(self.zone, self.south, lon, lat)
        else:
            x, y = lon, lat
        px = int((x - self.ox) / self.sx)
        py = int((self.oy - y) / self.sy)
        if px < 0 or py < 0 or px >= self.w or py >= self.h:
            return None
        o = (py * self.w + px) * self.spp
        b = self.px
        if self.spp >= 4:
            return b[o], b[o + 1], b[o + 2], b[o + 3]
        if self.spp == 3:
            return b[o], b[o + 1], b[o + 2], b[o]
        return b[o], b[o], b[o], b[o]


class LocalImagery:
    """A directory of tiles, indexed by what each one says it covers."""

    def __init__(self, dirpath, quiet=False):
        self.tiles = []
        self.has_nir = True
        names = sorted(n for n in os.listdir(dirpath)
                       if n.lower().endswith((".tif", ".tiff")))
        if not names:
            raise ValueError("no .tif files in %s" % dirpath)
        for n in names:
            try:
                t = GeoTile(os.path.join(dirpath, n))
            except Exception as ex:
                print("  skip %s: %s" % (n, ex))      # named, never silent
                continue
            self.tiles.append(t)
            if t.spp < 4:
                self.has_nir = False
            if not quiet:
                print("  %s  %dx%d  %d-band  %s  %.3f m/px"
                      % (n, t.w, t.h, t.spp,
                         ("EPSG:%d" % t.epsg) if t.projected else "geographic",
                         t.sx if t.projected else t.sx * 111320.0))
        if not self.tiles:
            raise ValueError("no usable tiles in %s" % dirpath)
        self.lo0 = min(t.lo0 for t in self.tiles)
        self.la0 = min(t.la0 for t in self.tiles)
        self.lo1 = max(t.lo1 for t in self.tiles)
        self.la1 = max(t.la1 for t in self.tiles)
        self.metres = self.tiles[0].sx if self.tiles[0].projected \
            else self.tiles[0].sx * 111320.0
        self.last = 0

    def at(self, lon, lat):
        """(r, g, b, nir) or None. A row-major sweep stays in one tile, so the
        last hit is tried first and the scan runs about once per crossing."""
        v = self.tiles[self.last].at(lon, lat)
        if v is not None:
            return v
        for i, t in enumerate(self.tiles):
            if i == self.last:
                continue
            v = t.at(lon, lat)
            if v is not None:
                self.last = i
                return v
        return None
