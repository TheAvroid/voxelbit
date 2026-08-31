# ── VISUAL FLY-THROUGH ─────────────────────────────────────────────────────────
# The ring's audits are counters, and a counter cannot see a floating tree fragment,
# a plate of untextured stone, or a hill that is the wrong shape. Those are reported
# by eye and they have to be FOUND by eye. This flies a straight path through a named
# biome and saves a settled screenshot every few steps, so a whole transect can be
# looked at as a strip rather than one lucky pose at a time.
#
#   python tools/flyshots.py --slot default --biome arctic --shots 12 --out DIR
#
# WHY THE STEPS ARE DRIVEN FROM PYTHON AND NOT A rAF LOOP
#   ringflight.py drives its flight in-page because it samples every frame and the two
#   must agree frame for frame. Here the opposite is wanted: each shot should be of a
#   SETTLED frame. The SVGF denoiser needs history to converge, and the streamer needs
#   time to page what the move exposed, so a shot taken mid-move shows raw single-ray
#   noise and half-arrived terrain — both of which read as rendering bugs and are not.
#   So: move, wait, shoot. Slow on purpose.
#
# WHY IT NEVER CALLS tp() OR tod()
#   Both set resetHist, which wipes the denoiser every time they are called — the trap
#   that made two earlier A/Bs lie. P.x/P.y/P.z are assigned directly instead.
#   gotoBiome IS used once before the transect, because world coordinates are not
#   portable across loads (the seed is re-randomised, biome layout is spawn-relative).
#
# THE CLOCK IS PINNED unless --tod is left at -1. Sun angle changes what a surface
# looks like far more than most defects do, and a transect that takes two minutes of
# wall time otherwise has a moving sun in it — so a strip of shots would not be
# comparable with each other, let alone with a rerun.
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--biome', default='')             # '' = wherever the world put us
ap.add_argument('--shots', type=int, default=10)
ap.add_argument('--stride', type=float, default=260.0)   # voxels between shots
ap.add_argument('--heading', type=float, default=0.7)
ap.add_argument('--alt', type=float, default=0.0)  # 0 = follow the ground at +28
ap.add_argument('--pitch', type=float, default=-0.22)
ap.add_argument('--settle', type=float, default=2.2)     # seconds to let the stream and the denoiser catch up
ap.add_argument('--tod', type=float, default=0.42)       # pinned clock; -1 leaves it running
ap.add_argument('--out', default='')
a = ap.parse_args()

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r - start it first' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target())
ws.call('Runtime.enable'); ws.call('Page.enable')

out = a.out or os.path.join(os.environ['TEMP'], 'claude', 'flyshots')
if not os.path.isdir(out): os.makedirs(out)

if a.biome:
    print('goto', a.biome, cdp.ev(ws, 'JSON.stringify(__vb.gotoBiome(%r))' % a.biome))
    time.sleep(3.0)
# the clock first, so the settle below is already at the angle every shot will use
if a.tod >= 0: cdp.ev(ws, '__vb.tod(%f)' % a.tod)

# ── THE POSE IS SET IN ONE EVAL PER STEP ── and `locked` is forced, because the boot
# click lands before the game is ready often enough that the ESC menu is still up, and
# a menu that is up drifts the camera on its own (8 px of wander between two shots that
# were meant to be identical). Nothing here reads a mouse, so pinning yaw/pitch is safe.
STEP = r'''
(() => {
  const P = __vb.P, ALT = %f, PITCH = %f, HDG = %f, D = %f;
  P.fly = true;
  // forward is (sin yaw, cos yaw) in xz — see aimVox/dbgEye, which all build the ray that way.
  // Moving along (cos, sin) while pointing the camera with yaw = HDG looks NINETY DEGREES off the path,
  // so a transect shot every step is a series of side views of ground the flight never crosses.
  if (window.__fsN === undefined) window.__fsN = 0; else { P.x += Math.sin(HDG) * D; P.z += Math.cos(HDG) * D; window.__fsN++; }
  P.y = ALT > 0 ? ALT : (__vb.gnd(Math.round(P.x), Math.round(P.z)) + 28);
  P.vy = 0; P.yaw = HDG; P.pitch = PITCH;
  return JSON.stringify({ n: window.__fsN, x: Math.round(P.x), z: Math.round(P.z), y: Math.round(P.y) });
})()
''' % (a.alt, a.pitch, a.heading, a.stride)

# __vb.hAt returns {hmap, analytic}, not a number — hmap is the STREAMED height (what is
# actually under you) and analytic is what H() says it should be. Prefer hmap and fall back,
# because outside the streamed window hmap is stale or zero and would fly us into the ground.
cdp.ev(ws, 'window.__vb.gnd = (x, z) => { const h = __vb.hAt(x, z); return (h.hmap > 0 ? h.hmap : h.analytic); }')
if cdp.ev(ws, 'typeof __vb.gnd') != 'function': sys.exit('could not install the ground-height helper')

cdp.ev(ws, 'window.__fsN = undefined')
man = []
for i in range(a.shots):
    where = cdp.ev(ws, STEP)
    where = json.loads(where) if isinstance(where, str) else where
    time.sleep(a.settle)
    png = cdp.shot(ws)
    p = os.path.join(out, '%s%02d_%d_%d.png' % (a.biome or 'here', i, where['x'], where['z']))
    open(p, 'wb').write(png)
    man.append({'i': i, 'file': os.path.basename(p), 'at': [where['x'], where['y'], where['z']], 'bytes': len(png)})
    print('shot %2d  %6d %5d %6d  %s' % (i, where['x'], where['y'], where['z'], os.path.basename(p)))

err = cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-3))')
json.dump({'biome': a.biome, 'tod': a.tod, 'heading': a.heading, 'pitch': a.pitch,
           'stride': a.stride, 'shots': man, 'err': err, 'dir': out},
          open(os.path.join(out, 'manifest.json'), 'w'), indent=1)
print('\nwrote %d shots to %s' % (len(man), out))
print('errLog tail:', err)
