# -- PNG IN AND OUT, WITHOUT PIL ---------------------------------------------
# There is no PIL on this box and no pip in this python, so the two tools that
# compare frames carry their own codec. Truecolour 8-bit only, which is what CDP
# returns. Kept in its own module because importing it from a TOOL runs that
# tool's argparse at import time and eats the caller's arguments.
import zlib

def png_rgb(buf):
    if buf[:8] != b'\x89PNG\r\n\x1a\n': sys.exit('not a png')
    i, idat, w, h, bd, ct = 8, [], 0, 0, 0, 0
    while i < len(buf):
        ln = int.from_bytes(buf[i:i+4], 'big'); typ = buf[i+4:i+8]; data = buf[i+8:i+8+ln]
        if typ == b'IHDR':
            w = int.from_bytes(data[0:4], 'big'); h = int.from_bytes(data[4:8], 'big')
            bd = data[8]; ct = data[9]
            if bd != 8 or ct not in (2, 6): sys.exit('unhandled png bd=%d ct=%d' % (bd, ct))
        elif typ == b'IDAT': idat.append(data)
        elif typ == b'IEND': break
        i += 12 + ln
    raw = zlib.decompress(b''.join(idat))
    nc = 3 if ct == 2 else 4
    stride = w * nc
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]; p += 1
        line = bytearray(raw[p:p+stride]); p += stride
        if f == 1:
            for x in range(nc, stride): line[x] = (line[x] + line[x-nc]) & 255
        elif f == 2:
            for x in range(stride): line[x] = (line[x] + prev[x]) & 255
        elif f == 3:
            for x in range(stride):
                left = line[x-nc] if x >= nc else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif f == 4:
            for x in range(stride):
                A = line[x-nc] if x >= nc else 0
                B = prev[x]; C = prev[x-nc] if x >= nc else 0
                pa = abs(B - C); pb = abs(A - C); pc = abs(A + B - 2*C)
                pr = A if (pa <= pb and pa <= pc) else (B if pb <= pc else C)
                line[x] = (line[x] + pr) & 255
        out[y*stride:(y+1)*stride] = line
        prev = line
    return w, h, nc, bytes(out)

def png_write(path, w, h, rgb):
    """Minimal truecolour writer — filter 0 on every scanline, which costs bytes and no thought."""
    raw = bytearray()
    for y in range(h):
        raw.append(0); raw += rgb[y*w*3:(y+1)*w*3]
    def chunk(t, d):
        c = t + d
        return len(d).to_bytes(4, 'big') + c + (zlib.crc32(c) & 0xffffffff).to_bytes(4, 'big')
    hdr = w.to_bytes(4,'big') + h.to_bytes(4,'big') + bytes([8,2,0,0,0])
    SIG = bytes([137, 80, 78, 71, 13, 10, 26, 10])
    open(path, 'wb').write(SIG + chunk(b'IHDR', hdr) +
                           chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b''))
