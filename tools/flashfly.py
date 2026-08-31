# -- CATCH A FLASH WHILE THE CAMERA IS STILL MOVING -----------------------------
# flashwatch.py freezes the camera, which only sees faults that OUTLIVE the motion.
# A flash that exists for a frame or two mid-flight is invisible to it, and a plain
# A-vs-B diff cannot help: under motion every pixel differs anyway.
#
# The trick is that motion is CONSISTENT and a flash is not. Take three captures
# A, B, C along the flight. Under smooth motion the scene progresses monotonically,
# so what changed from A to C is about what changed A->B plus B->C. If B contains a
# transient — terrain that appeared for one instant and was gone by C — then A and C
# AGREE with each other while both disagree with B, and
#       ratio = diff(A,C) / (diff(A,B) + diff(B,C))
# collapses well below 1. Pure motion sits near 1; a flash pulls it toward 0.
# Per-region, so a small flash in a corner is not averaged away by a moving horizon.
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp
from pngio import png_rgb, png_write

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--triples', type=int, default=24)
ap.add_argument('--gap', type=float, default=0.10)
ap.add_argument('--speed', type=float, default=4.25)
ap.add_argument('--grid', type=int, default=6)         # regions per axis
ap.add_argument('--minchange', type=int, default=10)   # a region must actually change to be judged
ap.add_argument('--out', default='')
a = ap.parse_args()
SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable'); ws.call('Page.enable')
out = a.out or os.path.join(os.environ['TEMP'], 'claude', 'flashfly')
if not os.path.isdir(out): os.makedirs(out)
cdp.ev(ws, 'window.__TFREEZE = 1; window.__WFREEZE = 1; window.__GEN = 0;')
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
print('arctic %d vox on heading %.3f, y=%d, speed %.2f vox/frame' % (run, hdg, alt, a.speed))
cdp.ev(ws, r'''
(() => { const P=__vb.P, SPD=%f, HDG=%f, ALT=%f, MY=++window.__GEN;
  P.fly = true;
  const step = () => { if (window.__GEN!==MY) return;
    P.x += Math.sin(HDG)*SPD; P.z += Math.cos(HDG)*SPD;
    P.y = ALT; P.vy = 0; P.yaw = HDG; P.pitch = -0.26;
    requestAnimationFrame(step); };
  requestAnimationFrame(step); return MY; })()''' % (a.speed, hdg, alt))

def regdiff(A, B, G):
    W,H,NC,d1 = A; _,_,_,d2 = B
    acc = [0]*(G*G)
    for y in range(0, H, 4):
        ro = y*W*NC; gy = min(G-1, y*G//H)
        for x in range(0, W, 4):
            o = ro + x*NC
            acc[gy*G + min(G-1, x*G//W)] += max(abs(d1[o]-d2[o]), abs(d1[o+1]-d2[o+1]), abs(d1[o+2]-d2[o+2]))
    return acc

G = a.grid
worst = (9.9, None, None)
flags = 0
for t in range(a.triples):
    F = []
    for i in range(3):
        F.append(png_rgb(cdp.shot(ws)))
        if i < 2: time.sleep(a.gap)
    ab = regdiff(F[0], F[1], G); bc = regdiff(F[1], F[2], G); ac = regdiff(F[0], F[2], G)
    st = cdp.ev(ws, 'JSON.stringify((()=>{const r=__vb.ring(),p=__vb.poolProbe(64);return [p.dirty,r.filled]})())')
    v = json.loads(st) if isinstance(st,str) else st
    lo = 9.9; loi = -1
    for i in range(G*G):
        tot = ab[i] + bc[i]
        if tot < a.minchange * 1000: continue          # region barely changed: ratio is noise
        r = ac[i] / tot
        if r < lo: lo, loi = r, i
    hit = lo < 0.55
    if hit: flags += 1
    print('triple %2d  minRatio %.3f  region %2d  %s  dirty %-6d filled %d'
          % (t, lo, loi, 'FLASH' if hit else '.', v[0], v[1]))
    if lo < worst[0]:
        worst = (lo, t, F)
cdp.ev(ws, '++window.__GEN')
print('\nflagged %d of %d triples; worst ratio %.3f on triple %s' % (flags, a.triples, worst[0], worst[1]))
if worst[2]:
    for i, tag in enumerate(('A','B','C')):
        W,H,NC,d = worst[2][i]
        png_write(os.path.join(out, 'worst_%s.png' % tag), W, H,
                  bytes(bytearray(b for y in range(H) for x in range(W) for b in (d[y*W*NC+x*NC], d[y*W*NC+x*NC+1], d[y*W*NC+x*NC+2]))))
    print('wrote worst_A/B/C.png to', out)
print('errLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-3))'))
