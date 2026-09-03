# -- WATCH THE SCREEN, NOT THE BOOKKEEPING ------------------------------------
# Every far-ring audit in this repo asks the ENGINE whether it is consistent, and the ring
# has audited clean through several of them while the player still reports flashing. So this
# records the actual PIXELS during a scripted flight and looks for a frame that disagrees
# with BOTH of its neighbours while they agree with each other -- which is what a transient
# is and what steady motion is not.
#
#   python tools/flashcap.py --slot ring --secs 40
#
# Four things that are not guessable:
#   * Page.startScreencast delivers NOTHING here. The harness window is offscreen so the
#     compositor never swaps; captureScreenshot forces a raster and does work. Capture rate
#     is therefore the round trip (~11/s), not the engine's ~120-220 fps.
#   * The flight is TIME-based (255 vox/s, the game's own fly sprint). A per-FRAME step is
#     four times too fast: the harness free-runs where a player is nearer 60-120.
#   * ALTITUDE IS ABSOLUTE, and it has to be. `ground + k` has no lookahead, so at 255 vox/s
#     it flies straight into the next hill and every top-scoring "flash" is the eye buried in
#     dirt. 430 clears the oaks; the far ring is only in frame from up there.
#   * A page that RELOADS mid-run invalidates everything after it -- the flight loop dies with
#     the window and the shots become a stationary spawn. The session marker catches that.
import argparse, base64, json, os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--secs', type=float, default=40.0)
ap.add_argument('--speed', type=float, default=255.0)   # voxels per SECOND -- fly sprint
ap.add_argument('--turn', type=float, default=0.25)     # radians per second
ap.add_argument('--alt', type=float, default=430.0)
ap.add_argument('--pitch', type=float, default=-0.16)
ap.add_argument('--out', default='')
ap.add_argument('--keep', type=int, default=10)
ap.add_argument('--tag', default='')
a = ap.parse_args()

OUT = a.out or os.path.join(os.environ.get('TEMP', '/tmp'), 'vbflash' + (a.tag and '_' + a.tag))
os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):
    if f.endswith('.jpg') or f.endswith('.png'): os.remove(os.path.join(OUT, f))

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable'); ws.call('Page.enable')

SESS = str(int(time.time() * 1000))
FLIGHT = r'''
(() => {
  const SECS = %f, SPD = %f, TURN = %f, ALT = %f, PIT = %f;
  const P = __vb.P; P.fly = true;
  let hdg = P.yaw || 0.7, t0 = performance.now(), tp = t0;
  window.__SESS = '%s'; window.__FC = null;
  const s = [];
  const step = (now) => {
    const dt = Math.min(0.05, (now - tp) / 1000); tp = now;
    hdg += TURN * dt;
    P.x += Math.sin(hdg) * SPD * dt; P.z += Math.cos(hdg) * SPD * dt;
    P.y = ALT; P.vy = 0; P.yaw = hdg; P.pitch = PIT;
    if (now - t0 < SECS * 1000) requestAnimationFrame(step);
    else window.__FC = { rec: __vb.rec(), ring: __vb.rd().ring, t1: now };
  };
  requestAnimationFrame(step);
  return performance.now();
})()
''' % (a.secs, a.speed, a.turn, a.alt, a.pitch, SESS)

pt0 = cdp.ev(ws, FLIGHT)
wt0 = time.time()
print('flight %.0fs at %.0f vox/s, alt %.0f' % (a.secs, a.speed, a.alt))
shots = []
while time.time() - wt0 < a.secs:
    r = ws.call('Page.captureScreenshot', {'format': 'jpeg', 'quality': 82, 'optimizeForSpeed': True})
    shots.append((round((time.time() - wt0) * 1000 + pt0, 1), base64.b64decode(r['result']['data'])))
sess = cdp.ev(ws, 'window.__SESS || "GONE"')
print('captured %d shots in %.1fs (%.1f/s)' % (len(shots), time.time() - wt0, len(shots) / (time.time() - wt0)))
if sess != SESS: sys.exit('PAGE RELOADED MID-RUN (%s) -- the shots are not one flight' % sess)

for i, (t, d) in enumerate(shots): open(os.path.join(OUT, 'f%05d.jpg' % i), 'wb').write(d)
FC = cdp.ev(ws, 'JSON.stringify(window.__FC)')
FC = json.loads(FC) if isinstance(FC, str) else {}
json.dump({'shotMs': [t for t, _ in shots], 'fc': FC}, open(os.path.join(OUT, 'meta.json'), 'w'))

W, H = 160, 80
raw = subprocess.run(['ffmpeg', '-v', 'error', '-f', 'image2', '-i', os.path.join(OUT, 'f%05d.jpg'),
                      '-vf', 'scale=%d:%d' % (W, H), '-pix_fmt', 'gray', '-f', 'rawvideo', '-'],
                     capture_output=True).stdout
n = len(raw) // (W * H)
G = [raw[k * W * H:(k + 1) * W * H] for k in range(n)]
def D(x, y):
    A, B = G[x], G[y]
    return sum(abs(A[k] - B[k]) for k in range(W * H)) / float(W * H)
sc = []
for t in range(1, n - 1):
    d01, d12, d02 = D(t - 1, t), D(t, t + 1), D(t - 1, t + 1)
    sc.append((min(d01, d12) - d02, t, d01, d12, d02))
sc.sort(reverse=True)
print('shots %d   median score %.3f' % (n, sorted(s[0] for s in sc)[len(sc) // 2]))
print('%-6s %-8s %-8s %-8s %-8s' % ('shot', 'score', 'd(t-1,t)', 'd(t,t+1)', 'd(t-1,t+1)'))
for s in sc[:15]: print('%-6d %-8.3f %-8.2f %-8.2f %-8.2f' % (s[1], s[0], s[2], s[3], s[4]))
for s in sc[:a.keep]:
    t = s[1]
    for o in (-1, 0, 1):
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-i', os.path.join(OUT, 'f%05d.jpg' % (t + o)),
                        os.path.join(OUT, 'flash%04d_%+d.png' % (t, o))])
print('dir:', OUT)
r = FC.get('ring', {})
print('ring after: tiles=%s evicted=%s adopted=%s overflow=%s abandoned=%s squash=%s stale=%s zero=%s live=%s/%s' % (
    r.get('tiles'), r.get('evicted'), r.get('adopted'), r.get('overflow'), r.get('abandoned'), r.get('squash'),
    r.get('own', {}).get('stale'), r.get('own', {}).get('zero'),
    (r.get('poolUsed', 0) - r.get('free', 0)), r.get('poolSlots')))
print('errLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-3))'))
