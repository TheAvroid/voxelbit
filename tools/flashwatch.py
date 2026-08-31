# -- CATCH A TERRAIN FLASH WITH THE CAMERA HELD STILL ---------------------------
# A flash cannot be seen in a counter and cannot be diffed out of a moving camera:
# at sprint the view changes everywhere between two captures, so every pixel differs
# and the metric is meaningless (measured: 91% of pixels "changed" on a clean run).
#
# So: FLY to build real streaming pressure, then FREEZE the camera and take several
# frames while it is stationary. Anything that changes now is not parallax — it is
# the world being rewritten under a still camera, which is exactly what a flash is.
# Repeat. The flight leg is what creates the backlog; the still leg is what sees it.
#
#   python tools/flashwatch.py --cycles 8 --fly 1.5 --shots 4
#
# The clock is frozen (__TFREEZE) so sun and clouds cannot register as change. Live
# creatures still move and will show as small drifting blobs — they are why the report
# gives a BLOCKINESS score as well as a percentage: terrain faults are large and
# axis-aligned, creatures are small and scattered. Diff images are written for the
# worst cycle so the shape can be judged by eye rather than by threshold.
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from pngio import png_rgb, png_write

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--cycles', type=int, default=8)
ap.add_argument('--fly', type=float, default=1.5)     # seconds of motion per cycle
ap.add_argument('--shots', type=int, default=4)       # stationary frames per cycle
ap.add_argument('--gap', type=float, default=0.18)
ap.add_argument('--speed', type=float, default=4.25)
ap.add_argument('--thresh', type=int, default=18)
ap.add_argument('--det', type=int, default=1)         # 1 = strip the STOCHASTIC light terms so a frozen frame is deterministic
ap.add_argument('--out', default='')
a = ap.parse_args()

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable'); ws.call('Page.enable')
out = a.out or os.path.join(os.environ['TEMP'], 'claude', 'flash')
if not os.path.isdir(out): os.makedirs(out)
cdp.ev(ws, 'window.__TFREEZE = 1; window.__WFREEZE = 1; window.__GEN = 0;')   # __WFREEZE pins u.time: waves, ripples and star twinkle would otherwise swamp a still-camera diff
# -- AND STRIP THE RANDOM TERMS, or the test can only ever measure its own noise --
# The renderer is stochastic by design: one jittered AO ray per pixel, TAA jitter and
# three dither grains. All of them redraw every frame, so a "did anything change"
# test on a frozen camera reads them and nothing else. Measured: they ARE the residual
# 0.5-2.5% that survived freezing the camera, the sun, the clouds and the water.
# With them off, a frozen frame should be bit-stable, and any change left is the world.
if a.det:
    m = cdp.ev(ws, 'JSON.stringify((()=>{const l=__vb.lgt();return [l.all, l.mask]})())')
    ALL, CUR = json.loads(m) if isinstance(m, str) else m
    NOISE = 2 | 256 | 512 | 1024 | 2048          # ao | taa | bodyGrain | terrainGrain | creatureGrain
    cdp.ev(ws, '__vb.lgt(%d)' % (ALL & ~NOISE))
    print('deterministic mode: lgt %d -> %d (stripped ao/taa/grains)' % (CUR, ALL & ~NOISE))

def halt(): cdp.ev(ws, '++window.__GEN'); time.sleep(0.35)

# stay in the arctic: find the direction the biome runs furthest and follow it
cdp.ev(ws, "__vb.gotoBiome('arctic')"); time.sleep(3.0)
DIRQ = r'''(() => { const P=__vb.P; let best=[0,-1];
  for (let i=0;i<8;i++){ const th=i*Math.PI/4, dx=Math.sin(th), dz=Math.cos(th); let run=0;
    for (let d=0; d<=20000; d+=250){ if (__vb.bioAt(P.x+dx*d, P.z+dz*d).arctic < 0.5) break; run=d; }
    if (run>best[1]) best=[th,run]; }
  return JSON.stringify(best); })()'''
hdg, run = json.loads(cdp.ev(ws, DIRQ))
LIFT = r'''(() => { const P=__vb.P; let hi=0;
  for (let dx=-256; dx<=256; dx+=16) for (let dz=-256; dz<=256; dz+=16){
    const x=Math.round(P.x)+dx, z=Math.round(P.z)+dz;
    for (let y=380; y>hi; y--) if (__vb.vox(x,y,z)) { if (y>hi) hi=y; break; } }
  return hi; })()'''
top = cdp.ev(ws, LIFT); alt = min(376, (top if isinstance(top,int) else 320) + 42)
print('arctic runs %d vox on heading %.3f; flying at y=%d' % (run, hdg, alt))

FLY = r'''
(() => { const P=__vb.P, SPD=%f, HDG=%f, ALT=%f, MY=++window.__GEN;
  P.fly = true;
  const step = () => { if (window.__GEN!==MY) return;
    P.x += Math.sin(HDG)*SPD; P.z += Math.cos(HDG)*SPD;
    P.y = ALT; P.vy = 0; P.yaw = HDG; P.pitch = -0.26;
    requestAnimationFrame(step); };
  requestAnimationFrame(step); return MY; })()''' % (a.speed, hdg, alt)

def score(A, B):
    """% of sampled pixels changed, and how BLOCKY the change is (runs of >=8 px in a row)."""
    W,H,NC,d1 = A; _,_,_,d2 = B
    n = 0; tot = 0; blocky = 0; cells = {}
    for y in range(0, H, 3):
        ro = y*W*NC; runlen = 0
        for x in range(0, W, 3):
            o = ro + x*NC
            dv = max(abs(d1[o]-d2[o]), abs(d1[o+1]-d2[o+1]), abs(d1[o+2]-d2[o+2]))
            tot += 1
            if dv >= a.thresh:
                n += 1; runlen += 1
                cells[(y*6//H, x*8//W)] = cells.get((y*6//H, x*8//W), 0) + 1
            else:
                if runlen >= 8: blocky += runlen
                runlen = 0
        if runlen >= 8: blocky += runlen
    return (100.0*n/max(1,tot), 100.0*blocky/max(1,tot), cells)

worst = (0, None)
for c in range(a.cycles):
    cdp.ev(ws, FLY); time.sleep(a.fly)                 # build pressure
    halt()                                             # camera frozen from here
    frames = [png_rgb(cdp.shot(ws))]
    for _ in range(a.shots - 1):
        time.sleep(a.gap); frames.append(png_rgb(cdp.shot(ws)))
    pk = 0; pkb = 0; pkc = None; pki = 0
    for i in range(1, len(frames)):
        pct, blk, cells = score(frames[i-1], frames[i])
        if pct > pk: pk, pkb, pkc, pki = pct, blk, cells, i
    st = cdp.ev(ws, 'JSON.stringify((()=>{const r=__vb.ring(),p=__vb.poolProbe(64);return [p.dirty,r.filled,p.holeReal,p.ghostReal]})())')
    v = json.loads(st) if isinstance(st,str) else st
    print('cycle %d  stillChange %.3f%%  blocky %.3f%%  dirty %-7d filled %-5d holeReal %-5d ghostReal %d'
          % (c, pk, pkb, v[0], v[1], v[2], v[3]))
    if pk > worst[0]:
        worst = (pk, c)
        W,H,NC,A = frames[pki-1]; _,_,_,B = frames[pki]
        img = bytearray(W*H*3)
        for y in range(H):
            ro=y*W*NC; wo=y*W*3
            for x in range(W):
                o=ro+x*NC; q=wo+x*3
                for k in range(3):
                    dv = abs(A[o+k]-B[o+k]) * 7
                    val = (A[o+k] >> 2) + (255 if dv > 255 else dv)
                    img[q+k] = 255 if val > 255 else val
        png_write(os.path.join(out, 'worst_diff.png'), W, H, bytes(img))
        png_write(os.path.join(out, 'worst_frame.png'), W, H, bytes(bytearray(
            b for y in range(H) for x in range(W) for b in (A[y*W*NC+x*NC], A[y*W*NC+x*NC+1], A[y*W*NC+x*NC+2]))))
halt()
print('\nworst still-camera change: %.3f%% on cycle %s -> %s' % (worst[0], worst[1], out))
print('errLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-3))'))
