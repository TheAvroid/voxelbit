"""Uniformly rescale a MagicaVoxel .vox model on the 10 cm grid.

WHY NEAREST-NEIGHBOUR AND NOT A RESAMPLE
  A voxel model's palette ids are IDENTITY here, not colour: world/terrain.js and
  assets/palette.js key foliage, bark and litter off the exact id a voxel carries (see the
  isFol branch in palette.js). Any filter that blends neighbours invents ids that are not in
  the source, and the first one that lands on a decoration id is a pine tree with fence
  voxels in it. Nearest-neighbour cannot do that: every output voxel is some input voxel.

  It does mean a non-integer factor steps unevenly - at 1.5x a source voxel becomes a 1x1x2
  or 2x2x1 block depending on where it falls. At this scale that reads as the model's own
  blockiness and not as an artefact, which is the whole reason the game looks like this.

USE
    python tools/rescale_vox.py <in.vox> <out.vox> 1.5
"""
import struct, sys, os

def read_vox(p):
    d = open(p, 'rb').read()
    assert d[:4] == b'VOX ', 'not a .vox'
    size = None; xyzi = None; rgba = None
    i = 8 + 12                                          # skip header + MAIN chunk header
    while i + 12 <= len(d):
        cid = d[i:i+4]; n = struct.unpack('<I', d[i+4:i+8])[0]; body = d[i+12:i+12+n]
        if cid == b'SIZE' and size is None: size = struct.unpack('<III', body[:12])
        elif cid == b'XYZI' and xyzi is None:
            cnt = struct.unpack('<I', body[:4])[0]
            xyzi = [tuple(body[4+k*4:8+k*4]) for k in range(cnt)]
        elif cid == b'RGBA': rgba = body[:1024]
        i += 12 + n
    return size, xyzi, rgba

def write_vox(p, size, vox, rgba):
    def chunk(cid, body): return cid + struct.pack('<II', len(body), 0) + body
    sz = chunk(b'SIZE', struct.pack('<III', *size))
    xy = chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
    pal = chunk(b'RGBA', rgba) if rgba else b''
    kids = sz + xy + pal
    open(p, 'wb').write(b'VOX ' + struct.pack('<I', 150) +
                        b'MAIN' + struct.pack('<II', 0, len(kids)) + kids)

src, dst, f = sys.argv[1], sys.argv[2], float(sys.argv[3])
size, vox, rgba = read_vox(src)
sx, sy, sz = size
dx, dy, dz = (max(1, int(round(v * f))) for v in size)
grid = {}
for (x, y, z, c) in vox: grid[(x, y, z)] = c
out = []
for Z in range(dz):
    for Y in range(dy):
        for X in range(dx):
            s = (min(sx-1, int(X / f)), min(sy-1, int(Y / f)), min(sz-1, int(Z / f)))
            c = grid.get(s)
            if c: out.append((X, Y, Z, c))
assert len(out) < 2**31 and dx < 256 and dy < 256 and dz < 256, 'exceeds the .vox 256 limit'
write_vox(dst, (dx, dy, dz), out, rgba)
print('%s  %dx%dx%d (%d vox)  ->  %s  %dx%dx%d (%d vox)  x%.2f'
      % (os.path.basename(src), sx, sy, sz, len(vox), os.path.basename(dst), dx, dy, dz, len(out), f))
