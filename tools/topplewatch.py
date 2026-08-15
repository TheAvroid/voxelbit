"""Does a severed trunk fall asleep BEFORE it ever starts to topple?

phStep exempts a toppling trunk from the sleep countdown (`!b.tipping`) but not the phase
before it. tipArm is the window between severing and the tilt starting - up to
PH.tipArmMs = 1600 ms - and while the top is seated on the stump that branch deliberately
zeroes vel.x, vel.z and all of omega. What is left is a body whose measured motion is
~0 for as long as it is armed, against sleepFrames = 40 counts that physStep can spend
up to maxCCD = 12 times per frame.

So this samples the trunk IN THE PAGE every frame from the moment it is severed and
reports the first frame at which sleeping went true, next to tipArm/tipping/up. A trunk
that reads sleeping:true, tipArm:true, up~1.0 never toppled - it went to sleep standing
in the air where it was cut.
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))


def j(ws, expr, timeout=240):
    r = cdp.ev(ws, 'JSON.stringify(%s)' % expr, timeout=timeout)
    return json.loads(r) if r else None


FIND_PINE = r"""
(() => {
  const P = __vb.P, out = [];
  const c0x = Math.floor(P.x / __vb.TCELL), c0z = Math.floor(P.z / __vb.TCELL);
  for (let dz = -5; dz <= 5; dz++) for (let dx = -5; dx <= 5; dx++) {
    const tr = __vb.treeAt(c0x + dx, c0z + dz); if (!tr) continue;
    out.push({ tx: tr.tx, tz: tr.tz, d: +Math.hypot(tr.tx - P.x, tr.tz - P.z).toFixed(1) });
  }
  out.sort((a, b) => a.d - b.d); return out.slice(0, 24);
})()
"""

AIM = r"""
(() => {
  const P = __vb.P, tx = %d, ty = %d, tz = %d, dist = %d, ang = %f;
  const px = tx + Math.round(Math.cos(ang) * dist), pz = tz + Math.round(Math.sin(ang) * dist);
  __vb.tp(px, Math.max(2, ty - 40), pz, Math.atan2(tx - px, tz - pz), 0);
  const eye = __vb.dbgEye();
  P.pitch = Math.atan2(ty + 0.5 - eye, Math.hypot(tx - P.x, tz - P.z));
  __vb.eyeSync();
  return { aim: __vb.aimVox() };
})()
"""

# ── sample IN the page, per rAF ── polling over CDP samples at ~20 Hz and the arming
# window is decided in the first few frames, so the trace has to be taken in-page.
ARM_TRACE = r"""
(() => {
  window.__TRACE = [];
  const t0 = performance.now();
  const tick = () => {
    // the NEWEST big body is the one just severed. Sorting by size picks a trunk felled
    // minutes ago that is already lying flat, which reads as "never tipped" every time.
    const bs = __vb.bodySupport().filter(b => b.n > 500);
    if (bs.length) {
      const b = bs.sort((p, q) => p.ageS - q.ageS)[0];
      window.__TRACE.push([+(performance.now() - t0).toFixed(0), b.n, b.sleeping ? 1 : 0,
                           b.tipArm ? 1 : 0, b.tipping ? 1 : 0, b.seatT ? 1 : 0,
                           b.sleepT, b.up, b.pos[1], b.contacts, b.seated]);
    }
    if (performance.now() - t0 < %d) requestAnimationFrame(tick);
  };
  requestAnimationFrame(tick);
  return 1;
})()
"""


def run(seed, ntrees):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://localhost:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        print('booted', flush=True)
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        time.sleep(3.0)

        pines = j(ws, FIND_PINE)
        felled = 0
        stalled = 0
        for pine in pines:
            if felled >= ntrees:
                break
            tx, tz = pine['tx'], pine['tz']
            base = j(ws, r"""(() => { for (let y = 2; y < 220; y++)
                if (__vb.woodAtW(%d, y, %d)) return y; return null; })()""" % (tx, tz))
            if base is None:
                continue
            cutY = base + 8
            ok = False
            for ang in (0.0, 1.57, 3.14, 4.71):
                for d2 in (9, 7, 11, 6, 13):
                    a = j(ws, AIM % (tx, cutY, tz, d2, ang))
                    if a.get('aim') and a['aim'].get('wood'):
                        ok = True
                        break
                if ok:
                    break
            if not ok:
                continue
            # swing until ONE swing away from severance, then start the trace and finish it
            fell = None
            for sw in range(60):
                cdp.ev(ws, '__vb.physSwing()')
                time.sleep(0.06)
                lf = j(ws, '(__vb.phys().stats.lastFlood||null)')
                if lf and lf.get('orphans', 0) > 300:
                    fell = dict(lf, swings=sw + 1)
                    cdp.ev(ws, ARM_TRACE % 6000)
                    break
            if not fell:
                continue
            felled += 1
            time.sleep(7.5)
            tr = j(ws, 'window.__TRACE') or []
            if not tr:
                print('[%d] pine %d,%d  no trace' % (felled, tx, tz), flush=True)
                continue
            firstSleep = next((r for r in tr if r[2]), None)
            last = tr[-1]
            slept_armed = firstSleep and firstSleep[3] and not firstSleep[4]
            never_tipped = not any(r[4] for r in tr)
            upright = last[7] > 0.9
            bad = bool(last[2] and never_tipped and upright)
            if bad:
                stalled += 1
            print('\n[%d] pine %d,%d  orph=%d  %s' % (felled, tx, tz, fell['orphans'],
                  'STALLED UPRIGHT' if bad else 'toppled'), flush=True)
            print('    samples=%d  firstSleep=%s' % (len(tr), json.dumps(firstSleep)), flush=True)
            print('    sleptWhileArmed=%s  neverTipped=%s  finalUp=%.3f  finalY=%.1f seated=%d'
                  % (slept_armed, never_tipped, last[7], last[8], last[10]), flush=True)
            print('    trace[t,n,sleep,arm,tip,seat,sleepT,up,y,contacts,seated]:', flush=True)
            for r in tr[:6] + (['...'] if len(tr) > 12 else []) + tr[-6:]:
                print('      ', json.dumps(r), flush=True)

        print('\n===== %d of %d fells stalled upright =====' % (stalled, felled), flush=True)
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    sx = int(sys.argv[1]) if len(sys.argv) > 1 else 2400
    sz = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 6
    run((sx, sz), n)
