# -- CATCH THE TRANSIENT, NOT THE STEADY STATE --------------------------------
# tools/flicker.py settles for several seconds before its first capture, which
# means every audit it has ever passed was taken after ALL streaming had finished.
# That is the one state a paging fault cannot be in. A brick that pages late, a
# descriptor that points at a recycled slot, a tile that publishes early -- all of
# them are visible for a handful of frames while the queue drains and are gone by
# the time anything has settled. Measuring after the settle is measuring the answer
# you want to hear.
#
# So this does the opposite: MOVE far enough to force a streaming burst, then start
# capturing on the very next frame and keep the camera absolutely still while the
# queue drains. Nothing in the scene is moving except what the streamer is doing,
# so any blocky change between two of those frames is the defect, not motion.
#
#   python tools/glitchhunt.py --slot default --hops 8 --burst 6 --jump 900
#
# Per hop it also samples the pool's own invariants EVERY captured frame rather than
# once at the end -- holes (an occupied brick with no descriptor) and freeOwn (a live
# descriptor naming a slot that is on the free list, i.e. a use-after-free, which
# renders as some OTHER part of the world appearing inside a brick). Both are zero at
# rest; the question is whether they are zero DURING the drain.
import argparse, json, os, sys, time, zlib, hashlib
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from pngio import png_rgb, png_write            # shared codec; importing them from flicker.py would run ITS argparse

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--hops', type=int, default=8)      # how many streaming bursts to induce
ap.add_argument('--burst', type=int, default=6)     # frames captured per hop, starting immediately
ap.add_argument('--jump', type=float, default=900)  # voxels per hop -- big enough to force real paging
ap.add_argument('--gap', type=float, default=0.22)  # seconds between captures inside a burst
ap.add_argument('--thresh', type=int, default=30)
ap.add_argument('--step', type=int, default=3)
ap.add_argument('--grid', type=int, default=16)
ap.add_argument('--alt', type=float, default=300.0)
ap.add_argument('--pitch', type=float, default=-0.34)
ap.add_argument('--biome', default='')
ap.add_argument('--out', default='')
a = ap.parse_args()

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable'); ws.call('Page.enable')
out = a.out or os.path.join(os.environ['TEMP'], 'claude', 'glitch')
if not os.path.isdir(out): os.makedirs(out)

if a.biome:
    print('goto', a.biome, cdp.ev(ws, 'JSON.stringify(__vb.gotoBiome(%r))' % a.biome)); time.sleep(4)
cdp.ev(ws, 'window.__TFREEZE = 1; __vb.P.fly = true;')

# -- THE CHEAP PER-FRAME INVARIANT -- poolAudit walks the whole descriptor space and costs
# a quarter second, which cannot run inside a drain without changing what it measures. This
# walks a SLICE per call and cycles, so a burst covers a useful fraction at a few ms a frame.
PROBE = r'''
(() => {
  const r = __vb.poolProbe ? __vb.poolProbe() : null;
  const g = __vb.ring();
  return JSON.stringify({ probe: r, dirty: g.dirty|0, filled: g.filled|0, err: g.err,
    ovf: g.overflow|0, aband: g.abandoned|0, ev: g.evicted|0, ad: g.adopted|0 });
})()
'''

HOP = r'''
(() => {
  const P = __vb.P, D = %f, ALT = %f, PITCH = %f;
  P.x += D; P.fly = true; P.y = ALT; P.vy = 0; P.yaw = 1.5708; P.pitch = PITCH;
  return JSON.stringify({ x: Math.round(P.x), z: Math.round(P.z) });
})()
''' % (a.jump, a.alt, a.pitch)

report = []
for hop in range(a.hops):
    where = cdp.ev(ws, HOP)
    where = json.loads(where) if isinstance(where, str) else where
    frames, probes, hashes = [], [], []
    for k in range(a.burst):
        png = cdp.shot(ws)
        hashes.append(hashlib.sha1(png).hexdigest()[:10])
        frames.append(png_rgb(png))
        probes.append(cdp.ev(ws, PROBE))
        time.sleep(a.gap)
    W, H, NC, _ = frames[0]
    G = a.grid
    worst = (0.0, 0, None)
    for i in range(1, len(frames)):
        if hashes[i] == hashes[i-1]: continue          # dup capture reads 0.0 and proves nothing
        A = frames[i-1][3]; B = frames[i][3]
        cells = [0]*(G*G); n = 0; tot = 0
        for y in range(0, H, a.step):
            ro = y*W*NC; gy = (y*G)//H
            for x in range(0, W, a.step):
                o = ro + x*NC
                d = abs(A[o]-B[o])
                t = abs(A[o+1]-B[o+1]); d = t if t > d else d
                t = abs(A[o+2]-B[o+2]); d = t if t > d else d
                tot += 1
                if d >= a.thresh: n += 1; cells[gy*G + (x*G)//W] += 1
        pct = 100.0*n/tot
        if pct > worst[0]: worst = (pct, i, cells, tot)
    pr = [json.loads(p) if isinstance(p, str) else p for p in probes]
    dirty = [q['dirty'] for q in pr]
    holes = [(q['probe'] or {}).get('hole', -1) for q in pr]
    fown  = [(q['probe'] or {}).get('freeOwn', -1) for q in pr]
    print('hop %d at x=%d  worstPair %.3f%%  dirty %s  hole %s  freeOwn %s  err %s'
          % (hop, where['x'], worst[0], dirty, holes, fown, pr[-1]['err']))
    report.append({'hop': hop, 'x': where['x'], 'worstPct': round(worst[0], 3),
                   'dirty': dirty, 'hole': holes, 'freeOwn': fown})
    if worst[2] and worst[0] >= 0.5:
        pct, i, cells, tot = worst
        per = tot/(G*G)
        print('   where:')
        for gy in range(G):
            print('     ' + ''.join('.' if cells[gy*G+gx]*100.0/per < 1 else
                  ('%d' % min(9, int(cells[gy*G+gx]*100.0/per/10))) for gx in range(G)))
        A = frames[i-1][3]; B = frames[i][3]
        img = bytearray(W*H*3)
        for y in range(H):
            ro = y*W*NC; wo = y*W*3
            for x in range(W):
                o = ro + x*NC; q = wo + x*3
                for c in range(3):
                    d = abs(A[o+c]-B[o+c]) * 6
                    v = (A[o+c] >> 2) + (255 if d > 255 else d)
                    img[q+c] = 255 if v > 255 else v
        p1 = os.path.join(out, 'hop%02d_diff.png' % hop); png_write(p1, W, H, bytes(img))
        open(os.path.join(out, 'hop%02d_a.png' % hop), 'wb').write(b'')
        print('   diff ->', p1)

json.dump(report, open(os.path.join(out, 'report.json'), 'w'), indent=1)
print('\nerrLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-4))'))
cdp.ev(ws, 'window.__TFREEZE = 0')
