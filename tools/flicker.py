# ── DOES ANYTHING CHANGE WHEN NOTHING SHOULD? ────────────────────────────────
# "The terrain flickers" is a claim about TIME, and every other audit here is a
# snapshot. A settled screenshot cannot see it and a counter cannot either: the
# descriptor tables can be perfectly self-consistent in every frame while the
# picture still changes between them.
#
# So: pin the clock, pin the camera, and take consecutive frames. Anything that
# moves now is either a genuine animation (water, clouds, creatures, foliage),
# denoiser convergence, or the defect. The three are told apart by SHAPE, which
# is why --diffout exists, and why the decisive test is ALIGNMENT rather than eyeballing
# "blocky vs dust": a fault aligned to 8 is a brick or a descriptor, aligned to RING_TILE
# (128) is a far-ring tile, and aligned to neither is almost certainly neither.
#
# WHAT A CLEAN RESULT LOOKS LIKE (arctic, clock and pose pinned, 2026-08-30): every changing
# region followed a scene feature -- pine crowns, terrain silhouette edges, the held
# viewmodel, creatures -- and nothing was aligned to any grid. Peak 1.9% of sampled pixels,
# 0 duplicate captures, snowStats on:false so none of it was precipitation.
#
#   python tools/flicker.py --slot default --frames 8 --settle 3
#
# NO PIL ON THIS BOX, so PNG is decoded here: IHDR for the geometry, the IDATs
# concatenated and inflated, then the five scanline filters undone. Only truecolour
# 8-bit is handled, which is what CDP returns.
#
# TWO TRAPS THIS TOOL EXISTS TO AVOID.
#   Capture streams contain DUPLICATE frames — the same buffer read twice — and a
#   difference metric reads exactly 0.0 across a dup pair, which looks like proof of
#   stability and is proof of nothing. Dups are detected and reported separately.
#   And in-page canvas readback of a WebGPU surface returns all-zero, so frames must
#   come from CDP screenshots and never from drawImage.
import argparse, json, os, sys, time, zlib, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from pngio import png_rgb, png_write

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--frames', type=int, default=8)
ap.add_argument('--gap', type=float, default=0.45)   # seconds between captures
ap.add_argument('--settle', type=float, default=3.0) # let the denoiser converge before frame 0
ap.add_argument('--thresh', type=int, default=24)    # per-channel delta that counts as a change
ap.add_argument('--step', type=int, default=2)       # pixel stride, for speed
ap.add_argument('--grid', type=int, default=16)
ap.add_argument('--biome', default='')
ap.add_argument('--diffout', default='')     # write an AMPLIFIED diff png of the worst pair
a = ap.parse_args()

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable'); ws.call('Page.enable')

if a.biome:
    print('goto', a.biome, cdp.ev(ws, 'JSON.stringify(__vb.gotoBiome(%r))' % a.biome)); time.sleep(3)
# freeze the clock, and pin the pose ONCE — reassigning it every frame would set
# resetHist and hand us raw single-ray noise to call a rendering bug.
cdp.ev(ws, 'window.__TFREEZE = 1; __vb.P.fly = true; __vb.P.vy = 0;')
pose = cdp.ev(ws, 'JSON.stringify([__vb.P.x, __vb.P.y, __vb.P.z, __vb.P.yaw, __vb.P.pitch])')
print('pose', pose, ' settling %.1fs' % a.settle)
time.sleep(a.settle)

frames, hashes = [], []
for i in range(a.frames):
    png = cdp.shot(ws)
    hashes.append(hashlib.sha1(png).hexdigest()[:10])
    frames.append(png_rgb(png))
    time.sleep(a.gap)

W, H, NC, _ = frames[0]
G = a.grid
print('\n%d frames  %dx%d  stride %d  thresh %d' % (len(frames), W, H, a.step, a.thresh))
dups = sum(1 for i in range(1, len(hashes)) if hashes[i] == hashes[i-1])
print('duplicate consecutive captures: %d  (a dup pair reads 0.0%% and proves nothing)' % dups)

worst = None
for i in range(1, len(frames)):
    if hashes[i] == hashes[i-1]:
        print('pair %d-%d  DUPLICATE, skipped' % (i-1, i)); continue
    _, _, _, A = frames[i-1]; _, _, _, B = frames[i]
    cells = [0]*(G*G); n = 0; tot = 0
    for y in range(0, H, a.step):
        ro = y*W*NC
        gy = (y*G)//H
        for x in range(0, W, a.step):
            o = ro + x*NC
            d = abs(A[o]-B[o])
            d2 = abs(A[o+1]-B[o+1]);  d = d2 if d2 > d else d
            d2 = abs(A[o+2]-B[o+2]);  d = d2 if d2 > d else d
            tot += 1
            if d >= a.thresh: n += 1; cells[gy*G + (x*G)//W] += 1
    pct = 100.0*n/tot
    print('pair %d-%d  changed %.3f%%  (%d of %d sampled)' % (i-1, i, pct, n, tot))
    if worst is None or pct > worst[0]: worst = (pct, i, cells, tot)

if worst:
    pct, i, cells, tot = worst
    per = tot/(G*G)
    print('\nworst pair ends at frame %d — where the change is (each cell = %% of its own area):' % i)
    for gy in range(G):
        print('  ' + ''.join('.' if cells[gy*G+gx]*100.0/per < 1 else
                             ('%d' % min(9, int(cells[gy*G+gx]*100.0/per/10))) for gx in range(G)))
    print('\n  . = under 1%% changed   1-9 = tens of %%   a defect is BLOCKY and moves; denoiser residue is fine dust')
print('\nerrLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-3))'))
# -- THE DIFF IMAGE IS THE ONLY THING THAT SHOWS SHAPE -- a grid says "something changed in
# this cell" and a percentage says how much; neither can tell a swaying crown from a
# brick-aligned paging fault. The change is amplified 6x so a four-level difference is
# visible at all, and laid over the original at quarter brightness so it can be LOCATED in
# the scene -- a bright patch with no context is unreadable.
if a.diffout and worst:
    wi = worst[1]
    A = frames[wi-1][3]; B = frames[wi][3]
    out = bytearray(W*H*3)
    for y in range(H):
        ro = y*W*NC; wo = y*W*3
        for x in range(W):
            o = ro + x*NC; q = wo + x*3
            for c in range(3):
                d = abs(A[o+c]-B[o+c]) * 6
                v = (A[o+c] >> 2) + (255 if d > 255 else d)
                out[q+c] = 255 if v > 255 else v
    png_write(a.diffout, W, H, bytes(out))
    print('diff image (worst pair %d-%d, 6x amplified over a dimmed original): %s' % (wi-1, wi, a.diffout))

cdp.ev(ws, 'window.__TFREEZE = 0')
