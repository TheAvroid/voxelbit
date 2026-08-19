"""Re-align an animation strip so the model's BASE sits in the same place in every frame.

    python tools/align_frames.py game/assets/life/flamingo

WHAT IT FIXES. A walk cycle exported frame by frame usually has each frame cropped to its own tight
bounding box, so the grid changes size as the limbs move and the model's ANCHOR moves with it. The
runtime centres a creature on its grid, so the body then slides around under its own feet as it
animates. Measured on the flamingo before this ran: the base's centre drifted +1.0 to -1.0 voxels in
x and 0.0 to +3.5 in y against the grid centre, on a model 3 voxels wide — the feet swung the entire
width of the bird and up to three and a half voxels fore-and-aft, once per cycle.

WHAT IT DOES. For every frame it finds the BASE — the voxels in the lowest occupied layer, i.e. the
feet — and takes that footprint's centre. Every frame is then re-emitted on ONE shared grid, shifted
so those centres coincide and the base sits on z = 0. The animation is untouched: nothing is scaled,
mirrored or resampled, and no voxel is added or removed. Only the frame's offset within its grid
changes, which is precisely the thing that was never meaningful data in the first place.

WHY THE BASE AND NOT THE CENTROID. A walking bird's body is SUPPOSED to sway; its feet are supposed
to stay on the ground. Aligning centroids would hold the body still and slide the feet — exactly
backwards. Aligning the base pins the part that touches the world and lets the body move over it.

Reversible: it only rewrites NN.vox from NN.vox, so re-exporting from the source art undoes it.
Stdlib only; run with the plain `python` on PATH.
"""
import glob, os, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def read_vox(path):
    b = open(path, 'rb').read()
    size, vox, pal = None, [], None

    def walk(off, end):
        nonlocal size, pal
        while off < end - 12:
            cid = b[off:off + 4].decode('latin1')
            n1, csz = struct.unpack_from('<II', b, off + 4)
            if cid == 'SIZE':
                size = struct.unpack_from('<III', b, off + 12)
            elif cid == 'XYZI':
                n = struct.unpack_from('<I', b, off + 12)[0]
                vox.extend(tuple(b[off + 16 + i * 4: off + 20 + i * 4]) for i in range(n))
            elif cid == 'RGBA':
                pal = b[off + 12: off + 12 + 1024]
            elif cid == 'MAIN':
                walk(off + 12 + n1, off + 12 + n1 + csz)
                off += 12 + n1 + csz
                continue
            off += 12 + n1 + csz

    walk(8, len(b))
    return size, vox, pal


def write_vox(path, sx, sy, sz, vox, pal):
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    body = (chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
            + chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
            + (chunk(b'RGBA', pal) if pal else b''))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    d = sys.argv[1]
    if not os.path.isabs(d):
        d = os.path.join(ROOT, d)
    files = sorted(glob.glob(os.path.join(d, '[0-9]*.vox')))
    if not files:
        raise SystemExit('no NN.vox frames in %s' % d)

    frames = []
    for f in files:
        size, vox, pal = read_vox(f)
        if not vox:
            raise SystemExit('%s has no voxels' % f)
        zmin = min(v[2] for v in vox)
        base = [v for v in vox if v[2] == zmin]        # the feet: the lowest occupied layer
        bcx = (min(v[0] for v in base) + max(v[0] for v in base)) / 2.0
        bcy = (min(v[1] for v in base) + max(v[1] for v in base)) / 2.0
        frames.append({'f': f, 'vox': vox, 'pal': pal, 'zmin': zmin, 'bcx': bcx, 'bcy': bcy})

    # ── ONE SHARED GRID ── each frame is shifted so its base centre lands on a common anchor; the grid is
    # then whatever box holds every shifted frame. Rounded to whole voxels because a .vox coordinate is an
    # integer: a half-voxel alignment is not representable and would silently truncate.
    for fr in frames:
        fr['dx'] = -int(round(fr['bcx']))
        fr['dy'] = -int(round(fr['bcy']))
        fr['dz'] = -fr['zmin']                         # every frame stands on z = 0
    lo = [min(min(v[i] + fr['d' + 'xyz'[i]] for v in fr['vox']) for fr in frames) for i in range(3)]
    hi = [max(max(v[i] + fr['d' + 'xyz'[i]] for v in fr['vox']) for fr in frames) for i in range(3)]
    sx, sy, sz = [hi[i] - lo[i] + 1 for i in range(3)]
    if max(sx, sy, sz) > 255:
        raise SystemExit('aligned grid %dx%dx%d exceeds the .vox 255 limit' % (sx, sy, sz))

    print('%-8s %-12s %-12s %s' % ('frame', 'was', 'now', 'shift (x,y,z)'))
    for fr in frames:
        out = [(v[0] + fr['dx'] - lo[0], v[1] + fr['dy'] - lo[1], v[2] + fr['dz'] - lo[2], v[3]) for v in fr['vox']]
        assert all(0 <= q[0] < sx and 0 <= q[1] < sy and 0 <= q[2] < sz for q in out), 'shift left the grid'
        was = read_vox(fr['f'])[0]
        write_vox(fr['f'], sx, sy, sz, out, fr['pal'])
        print('%-8s %-12s %-12s (%+d,%+d,%+d)' % (os.path.basename(fr['f'])[:-4], '%dx%dx%d' % was,
                                                  '%dx%dx%d' % (sx, sy, sz),
                                                  fr['dx'] - lo[0], fr['dy'] - lo[1], fr['dz'] - lo[2]))
    print('%d frames aligned on a shared %dx%dx%d grid, base centred and standing on z=0' % (len(frames), sx, sy, sz))


main()
