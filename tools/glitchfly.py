# -- IS THE WORLD EVER WRONG WHILE YOU FLY? -----------------------------------
# Every audit in this repo is a SNAPSHOT taken at rest, and a paging fault lives for
# a few frames while the queue drains. So this flies continuously -- no teleports,
# because a teleport rebuilds everything and measures ordinary pop-in rather than a
# defect -- and asks the pool's own invariant EVERY FRAME, in-page, at flight speed.
#
#   python tools/glitchfly.py --slot default --biome arctic --frames 900 --speed 4.25
#
# WHAT COUNTS AS WRONG, and the distinction is the whole point:
#   ghostReal  a live descriptor over a brick whose W holds no voxels. The tracer draws
#              whatever that page last held -- terrain from somewhere else, appearing out
#              of nothing and vanishing when the real page lands. This is the reported
#              "flashing glitched terrain" if anything is.
#   holeReal   no descriptor over a brick that does hold voxels: see-through world.
#   stuck      the same faults, minus the ones still sitting in poolDirty. A queued fault
#              is the streaming budget working as designed; an unqueued one never repairs
#              itself. Only `stuck` is unambiguously a bug.
# poolProbe walks 1/32 of the bricks per call, so a 900-frame flight sweeps the world
# about 28 times over.
import argparse, hashlib, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'default'))
ap.add_argument('--biome', default='arctic')
ap.add_argument('--frames', type=int, default=900)
ap.add_argument('--speed', type=float, default=4.25)   # fly sprint, the fastest the game holds
ap.add_argument('--heading', type=float, default=1.5708)
ap.add_argument('--alt', type=float, default=0.0)      # 0 = follow the ground at +30
ap.add_argument('--frac', type=int, default=32)
ap.add_argument('--waitclean', type=float, default=0)   # seconds to wait for poolDirty to reach 0 before flying
ap.add_argument('--json', default='')
a = ap.parse_args()

def srcHash():
    import hashlib as H
    h = H.sha1(); root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src')
    for d, _, fs in sorted(os.walk(root)):
        for f in sorted(fs):
            if f.endswith('.js'): h.update(open(os.path.join(d, f), 'rb').read())
    return h.hexdigest()[:12]

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target()); ws.call('Runtime.enable')

if a.biome:
    print('goto', a.biome, cdp.ev(ws, 'JSON.stringify(__vb.gotoBiome(%r))' % a.biome)); time.sleep(4)

# -- WAIT FOR A GENUINELY IDLE QUEUE FIRST --------------------------------------
# gotoBiome teleports, and a teleport marks essentially the whole brick space dirty.
# Starting the flight three seconds later measures the TELEPORT draining, not the
# flight, and the backlog it reports is borrowed from an event no player performs.
# So: poll until poolDirty is actually empty (or give up and say so loudly, because a
# queue that will not drain AT REST is itself the finding).
if a.waitclean:
    t0 = time.time(); last = None
    while time.time() - t0 < a.waitclean:
        last = cdp.ev(ws, '__vb.poolProbe(256).dirty')
        if last == 0: break
        time.sleep(1.0)
    print('pre-flight queue: dirty=%s after %.0fs%s' % (last, time.time()-t0,
          '' if last == 0 else '   <-- NEVER DRAINED AT REST'))
    if last != 0:
        print('   the flight below therefore starts on a backlog, and its numbers are NOT attributable to flying')

FLIGHT = r'''
(() => {
  const N = %d, SPD = %f, HDG = %f, ALT = %f, FR = %d;
  const P = __vb.P, bad = [], s = [], yh = new Array(12).fill(0);
  P.fly = true;
  const dx = Math.sin(HDG), dz = Math.cos(HDG);
  let i = 0;
  const step = () => {
    P.x += dx * SPD; P.z += dz * SPD;
    P.y = ALT > 0 ? ALT : (__vb.hAt(Math.round(P.x), Math.round(P.z)).hmap + 30);
    P.vy = 0; P.yaw = HDG;
    const q = __vb.poolProbe(FR);
    s.push([q.ghostReal|0, q.holeReal|0, q.stuck|0, q.dirty|0, q.ghost|0, q.hole|0, q.holeStuck|0, q.ghostStuck|0, q.drained|0]);
    for (let k = 0; k < 12; k++) yh[k] += q.stuckYh[k];
    if (q.holeStuck && bad.length < 40)
      bad.push({ f: i, x: Math.round(P.x), z: Math.round(P.z), ghostReal: q.ghostReal,
                 holeReal: q.holeReal, stuck: q.stuck, dirty: q.dirty, ex: q.ex });
    if (++i < N) requestAnimationFrame(step);
    else window.__GF = { s, bad, yh };
  };
  requestAnimationFrame(step);
  return N;
})()
''' % (a.frames, a.speed, a.heading, a.alt, a.frac)

H0 = srcHash()
cdp.ev(ws, 'window.__GF = null; ' + FLIGHT)
deadline = time.time() + max(60.0, a.frames / 15.0 + 40.0)
while time.time() < deadline:
    time.sleep(1.0)
    if cdp.ev(ws, '!!window.__GF') is True: break
else:
    sys.exit('flight did not finish - is the sim ticking? (canvas focus / ?cdp)')

R = cdp.ev(ws, 'JSON.stringify(window.__GF)')
R = json.loads(R) if isinstance(R, str) else R
S, bad, yh = R['s'], R['bad'], R.get('yh', [])
gr = [r[0] for r in S]; hr = [r[1] for r in S]; st = [r[2] for r in S]
out = {
  'srcHash': H0, 'biome': a.biome, 'speed': a.speed, 'frames': len(S), 'probeFrac': a.frac,
  'ghostRealFrames': sum(1 for v in gr if v), 'ghostRealMax': max(gr) if gr else 0, 'ghostRealTotal': sum(gr),
  'holeRealFrames':  sum(1 for v in hr if v), 'holeRealMax':  max(hr) if hr else 0, 'holeRealTotal':  sum(hr),
  'stuckFrames':     sum(1 for v in st if v), 'stuckMax':     max(st) if st else 0, 'stuckTotal':     sum(st),
  'holeStuckFrames': sum(1 for r in S if r[6]), 'holeStuckMax': max(r[6] for r in S), 'holeStuckTotal': sum(r[6] for r in S),
  'ghostStuckTotal': sum(r[7] for r in S),
  'drainedMax': max(r[8] for r in S), 'drainedMean': round(sum(r[8] for r in S)/len(S), 1),
  'dirtyMax': max(r[3] for r in S), 'dirtyEnd': S[-1][3],
  'ghostBookkeepTotal': sum(r[4] for r in S), 'holeQueuedTotal': sum(r[5] for r in S),
  'stuckByHeight_32vox': yh,
  'firstBad': bad[:6],
}
print(json.dumps(out, indent=1))
# holeRealStuck is the only unambiguous picture bug: real voxels, no descriptor, and in
# nobody's queue, so it stays see-through. Everything else is either latency or bookkeeping.
verdict = 'CLEAN' if out['holeStuckTotal'] == 0 else 'SEE-THROUGH WORLD, UNQUEUED'
print('\nVERDICT:', verdict)
print('errLog:', cdp.ev(ws, 'JSON.stringify(__vb.errLog().slice(-4))'))
if a.json: json.dump(out, open(a.json, 'w'), indent=1)
