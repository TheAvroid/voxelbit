"""lasread -- a pure-stdlib LAS 1.0-1.4 point cloud reader.

    python lasread.py <file.las> [--head 5]

WHY THIS EXISTS. This machine has no numpy, no laspy, no PDAL and no laszip
(see tools/geoimg.py for the same story about rasters). LAS is a fixed-stride
binary format and is perfectly readable without any of them; **LAZ is not** --
it is chunked arithmetic coding and would need laszip. So this reads LAS and
refuses LAZ by name rather than producing nonsense.

WHAT IT IS FOR. v2 has two numbers that are currently intuition rather than
measurement:

  * `VoxelTerrain::roughFor` -- per-cover-class terrain roughness. Quoted as
    "terrain intuition, not a fit", because there was no high-resolution
    exemplar in the tree to fit against.
  * `VoxelTerrain::realStemsPerHa` -- an altitude table for the Colorado Front
    Range that floors at 120 stems/ha below 1800 m, applied to every world.

A dense, classified, per-tree-annotated point cloud answers both directly.
See tools/pc_roughness.py and tools/pc_stems.py.

READING STRATEGY: CONTIGUOUS BLOCKS, NOT A STRIDE. A 540 MB LAS holds tens of
millions of points and pure Python cannot touch them all. Sampling every Nth
point would be statistically fine and geometrically useless -- fitting a local
plane needs a point's NEIGHBOURS, and a stride throws exactly those away. So
this reads whole contiguous blocks at several offsets through the file: each
block keeps its neighbourhood, and the several offsets keep it representative.
"""
import io, json, math, os, struct, sys

# ---------------------------------------------------------------- point formats
#
# Two families, and the split is at format 6. Formats 0-5 are the original
# 20-byte core with a 3-bit return number; 6-10 widened the return fields to 4
# bits, moved classification to a byte of its own and made GPS time mandatory.
# Getting the family wrong does not error -- it silently reads the wrong byte
# for classification, which is the field most of this is about.
#
#   fmt : (core size, has_rgb, has_nir, has_gps)
_FMT = {
    0:  (20, False, False, False),
    1:  (28, False, False, True),
    2:  (26, True,  False, False),
    3:  (34, True,  False, True),
    4:  (57, False, False, True),    # + wave packet
    5:  (63, True,  False, True),
    6:  (30, False, False, True),
    7:  (36, True,  False, True),
    8:  (38, True,  True,  True),
    9:  (59, False, False, True),
    10: (67, True,  True,  True),
}


class LasFile:
    def __init__(self, path):
        self.path = path
        if path.lower().endswith(".laz"):
            raise ValueError(
                "LAZ is compressed (chunked arithmetic coding) and needs laszip "
                "or PDAL, neither of which is on this machine. Decompress to "
                ".las first, or fetch the .las distribution.")
        self.f = io.open(path, "rb")
        h = self.f.read(375)
        if h[:4] != b"LASF":
            raise ValueError("not a LAS file (no LASF signature)")
        self.ver_major, self.ver_minor = h[24], h[25]
        self.header_size = struct.unpack_from("<H", h, 94)[0]
        self.offset_to_points = struct.unpack_from("<I", h, 96)[0]
        self.num_vlrs = struct.unpack_from("<I", h, 100)[0]
        self.point_format = h[104] & 0x3F          # top bits flag LAZ in some writers
        self.point_size = struct.unpack_from("<H", h, 105)[0]
        legacy_count = struct.unpack_from("<I", h, 107)[0]
        (self.sx, self.sy, self.sz,
         self.ox, self.oy, self.oz) = struct.unpack_from("<6d", h, 131)
        (self.maxx, self.minx, self.maxy,
         self.miny, self.maxz, self.minz) = struct.unpack_from("<6d", h, 179)

        # LAS 1.4 moved the count to a 64-bit field and may leave the legacy
        # one at zero -- a file that "has no points" and is 540 MB long.
        self.count = legacy_count
        if self.ver_major == 1 and self.ver_minor >= 4 and self.header_size >= 375:
            big = struct.unpack_from("<Q", h, 247)[0]
            if big:
                self.count = big
        if self.point_format not in _FMT:
            raise ValueError("unsupported point data record format %d" % self.point_format)
        core, self.has_rgb, self.has_nir, _gps = _FMT[self.point_format]
        self.core_size = core
        if self.point_size < core:
            raise ValueError("point size %d is under format %d's core of %d"
                             % (self.point_size, self.point_format, core))
        self.extra_bytes = self.point_size - core
        self.new_style = self.point_format >= 6
        self.extra_dims = self._read_extra_dims()

    # ---- the Extra Bytes VLR, which is where a tree id usually lives -------
    def _read_extra_dims(self):
        """[(name, offset_in_record, struct_code)] for LAS 'extra bytes'.

        FOR-instance and friends carry per-point tree ids as extra dimensions
        rather than in any standard field, so this is the difference between
        counting trees and not being able to."""
        dims = []
        self.f.seek(self.header_size)
        _EB_TYPE = {                       # LAS spec data_type -> (code, size)
            1: ("B", 1), 2: ("b", 1), 3: ("H", 2), 4: ("h", 2),
            5: ("I", 4), 6: ("i", 4), 7: ("Q", 8), 8: ("q", 8),
            9: ("f", 4), 10: ("d", 8),
        }
        for _ in range(self.num_vlrs):
            hdr = self.f.read(54)
            if len(hdr) < 54:
                break
            user = hdr[2:18].rstrip(b"\0").decode("ascii", "replace")
            rec_id = struct.unpack_from("<H", hdr, 18)[0]
            length = struct.unpack_from("<H", hdr, 20)[0]
            body = self.f.read(length)
            if user == "LASF_Spec" and rec_id == 4:
                off = self.core_size
                for i in range(len(body) // 192):
                    e = body[i * 192:(i + 1) * 192]
                    dtype = e[2]
                    name = e[4:36].rstrip(b"\0").decode("ascii", "replace")
                    if dtype in _EB_TYPE:
                        code, size = _EB_TYPE[dtype]
                        dims.append((name, off, code))
                        off += size
                    elif dtype == 0:       # undocumented blob: options = length
                        off += struct.unpack_from("<H", e, 0)[0]
        return dims

    def describe(self):
        area = max(1e-9, (self.maxx - self.minx) * (self.maxy - self.miny))
        return {
            "path": os.path.basename(self.path),
            "version": "%d.%d" % (self.ver_major, self.ver_minor),
            "point_format": self.point_format,
            "point_size": self.point_size,
            "count": self.count,
            "rgb": self.has_rgb,
            "nir": self.has_nir,
            "extent_m": [round(self.maxx - self.minx, 2),
                         round(self.maxy - self.miny, 2),
                         round(self.maxz - self.minz, 2)],
            "pts_per_m2": round(self.count / area, 1),
            "spacing_cm": round(100.0 * math.sqrt(area / max(1, self.count)), 2),
            "extra_dims": [d[0] for d in self.extra_dims],
        }

    # ---- reading -----------------------------------------------------------
    def _unpack_fmt(self):
        """A struct format covering the WHOLE record, padding included, so
        iter_unpack can stride it without a per-point slice."""
        if self.new_style:
            f = "<iiiHBBBbhd"            # x y z intensity flags flags2 class user scan_ang src...
            f = "<iiiH" + "BB" + "B" + "B" + "h" + "H" + "d"
        else:
            f = "<iiiH" + "B" + "B" + "b" + "B" + "H"
        return f

    def points(self, want=2_000_000, blocks=8):
        """Yield (x, y, z, cls, ret, nret, r, g, b, extras) in contiguous blocks.

        `want` is a budget, not a guarantee; `blocks` is how many places in the
        file it is spread over. See the strategy note at the top -- neighbours
        are kept, which a stride would destroy."""
        if self.count == 0:
            return
        blocks = max(1, min(blocks, 64))
        per = max(1, want // blocks)
        step = max(per, self.count // blocks)
        rgb_off = self.core_size - (6 if not self.has_nir else 8) if self.has_rgb else -1
        if self.has_rgb:
            # RGB sits immediately after the core's non-colour part; for the
            # formats here it is always the last 6 (or 8, with NIR) bytes.
            rgb_off = self.core_size - (8 if self.has_nir else 6)
        for b in range(blocks):
            start = b * step
            if start >= self.count:
                break
            n = int(min(per, self.count - start))
            self.f.seek(self.offset_to_points + start * self.point_size)
            buf = self.f.read(n * self.point_size)
            if len(buf) < self.point_size:
                break
            n = len(buf) // self.point_size
            for i in range(n):
                o = i * self.point_size
                x, y, z = struct.unpack_from("<iii", buf, o)
                if self.new_style:
                    rb = buf[o + 15]
                    ret, nret = rb & 0x0F, (rb >> 4) & 0x0F
                    cls = buf[o + 16]
                else:
                    rb = buf[o + 14]
                    ret, nret = rb & 0x07, (rb >> 3) & 0x07
                    cls = buf[o + 15] & 0x1F
                if self.has_rgb:
                    r, g, bl = struct.unpack_from("<HHH", buf, o + rgb_off)
                else:
                    r = g = bl = 0
                extras = {}
                for name, eoff, code in self.extra_dims:
                    try:
                        extras[name] = struct.unpack_from("<" + code, buf, o + eoff)[0]
                    except struct.error:
                        pass
                yield (x * self.sx + self.ox,
                       y * self.sy + self.oy,
                       z * self.sz + self.oz,
                       cls, ret, nret, r, g, bl, extras)

    def close(self):
        self.f.close()


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    las = LasFile(sys.argv[1])
    d = las.describe()
    for k, v in d.items():
        print("  %-14s %s" % (k, v))
    # RGB FIELDS CAN EXIST AND BE ALL ZEROS, which is the trap worth catching
    # before anyone plans a pipeline around colour.
    if las.has_rgb:
        nz = 0
        seen = 0
        for p in las.points(want=40000, blocks=4):
            seen += 1
            if p[6] or p[7] or p[8]:
                nz += 1
        print("  %-14s %d of %d sampled points carry non-zero RGB%s"
              % ("rgb_check", nz, seen, "" if nz else "   <- FIELDS ARE EMPTY"))
    if "--head" in sys.argv:
        k = int(sys.argv[sys.argv.index("--head") + 1])
        print("\n  first %d points:" % k)
        for i, p in enumerate(las.points(want=k, blocks=1)):
            if i >= k:
                break
            print("   ", tuple(round(v, 3) if isinstance(v, float) else v for v in p))
    las.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
