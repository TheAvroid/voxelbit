"""The tree-on-tree cases, which every earlier test skipped.

A pine's crown is ~36 voxels wide and TCELL puts neighbours much closer than that, so
overlapping footprints are the normal case - physics.js says so itself. In the overlap,
one pine's needles occupy cells the other's model also claims, and treeShapeAt answers
with whichever it finds first. Felling A therefore takes cells out of W that B's crown
was resting on, and B's bole is discontinuous near the tip (the measured fact the whole
wood-held-by-needles bridge exists for), so B's top is exactly the thing that can be left
without support.

Four cases, each audited against its OWN centre so nothing hides behind the audit wall:
  pair   - fell A, then audit the NEIGHBOUR B
  high   - cut through the crown rather than the bole, where the model is needles+tip
  arrow  - arrowChop, a much smaller carve that chips instead of felling
  base   - cut at the very bottom, where the trunk is buried and the skirt reaches ground
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))


def j(ws, expr, timeout=300):
    r = cdp.ev(ws, 'JSON.stringify(%s)' % expr, timeout=timeout)
    return json.loads(r) if r else None


FIND_PAIRS = r"""
(() => {
  const P = __vb.P, all = [];
  const c0x = Math.floor(P.x / __vb.TCELL), c0z = Math.floor(P.z / __vb.TCELL);
  for (let dz = -5; dz <= 5; dz++) for (let dx = -5; dx <= 5; dx++) {
    const tr = __vb.treeAt(c0x + dx, c0z + dz); if (!tr) continue;
    all.push({ tx: tr.tx, tz: tr.tz });
  }
  const out = [];
  for (let i = 0; i < all.length; i++) for (let k = i + 1; k < all.length; k++) {
    const d = Math.hypot(all[i].tx - all[k].tx, all[i].tz - all[k].tz);
    if (d < 34) out.push({ a: all[i], b: all[k], sep: +d.toFixed(1) });   // crowns overlap
  }
  out.sort((p, q) => p.sep - q.sep);
  return { trees: all.length, pairs: out.slice(0, 8) };
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


def settle(ws, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        cdp.ev(ws, '__vb.supFlushNow()')
        time.sleep(1.0)


def audit_at(ws, x, y, z, label):
    """Audit centred on the thing in question, so it is never the wall-touching component."""
    cdp.ev(ws, '__vb.tp(%d,%d,%d,0,0)' % (x, max(2, y - 40), z))
    time.sleep(2.5)
    cdp.ev(ws, '__vb.supFlushNow()')
    time.sleep(0.5)
    f = j(ws, '__vb.floatAudit(90)')
    t = j(ws, '__vb.treeAudit(90)')
    bs = j(ws, '__vb.bodySupport()') or []
    fl = [b for b in bs if b['floating'] and b['n'] > 40]
    r = {'floaters': f['floaters'], 'floaterVox': f['floaterVox'], 'incon': f['inconclusive'],
         'trunksInAir': t['floating'], 'worstDrop': t['worstDrop'], 'floatBodies': len(fl)}
    bad = r['floaters'] or r['trunksInAir'] or r['floatBodies']
    print('    %-8s %s %s' % (label, 'FLOATERS' if bad else 'clean   ', json.dumps(r)), flush=True)
    if f['top']:
        print('       comps:', json.dumps(f['top'][:5])[:600], flush=True)
    if t['top']:
        print('       trunks:', json.dumps(t['top'][:3])[:400], flush=True)
    if fl:
        print('       bodies:', json.dumps(fl[:3])[:500], flush=True)
    return bad


def base_of(ws, tx, tz):
    return j(ws, r"""(() => { for (let y = 2; y < 220; y++)
        if (__vb.woodAtW(%d, y, %d)) return y; return null; })()""" % (tx, tz))


def aim_wood(ws, tx, tz, ty):
    for ang in (0.0, 1.57, 3.14, 4.71):
        for d2 in (9, 7, 11, 6, 13):
            a = j(ws, AIM % (tx, ty, tz, d2, ang))
            if a.get('aim') and a['aim'].get('wood'):
                return True
    return False


def swing_fell(ws, n=60, gate=300):
    for sw in range(n):
        cdp.ev(ws, '__vb.physSwing()')
        time.sleep(0.06)
        lf = j(ws, '(__vb.phys().stats.lastFlood||null)')
        if lf and lf.get('orphans', 0) > gate:
            return dict(lf, swings=sw + 1)
    return None


def run(seed):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    bad = 0
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://localhost:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        print('booted; seed', seed, flush=True)
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        settle(ws, 4.0)

        pp = j(ws, FIND_PAIRS)
        print('trees=%d overlapping pairs=%d' % (pp['trees'], len(pp['pairs'])), flush=True)
        print(json.dumps(pp['pairs'][:5]), flush=True)

        # ── CASE 1: fell A, audit its overlapping NEIGHBOUR B ──
        for k, pr in enumerate(pp['pairs'][:3]):
            A, B = pr['a'], pr['b']
            bA, bB = base_of(ws, A['tx'], A['tz']), base_of(ws, B['tx'], B['tz'])
            if bA is None or bB is None:
                continue
            print('\n== PAIR %d  A=%d,%d  B=%d,%d  sep=%.1f ==' % (k, A['tx'], A['tz'], B['tx'], B['tz'], pr['sep']), flush=True)
            audit_at(ws, B['tx'], bB + 40, B['tz'], 'B before')
            if not aim_wood(ws, A['tx'], A['tz'], bA + 8):
                print('    (could not aim at A)', flush=True)
                continue
            f = swing_fell(ws)
            print('    fell A:', json.dumps(f), flush=True)
            settle(ws, 16.0)
            if audit_at(ws, B['tx'], bB + 40, B['tz'], 'B after'):
                bad += 1

        # ── CASE 2/3/4 on a fresh tree each ──
        singles = [t for t in [p['a'] for p in pp['pairs']] ][:6]
        seen = set()
        cases = []
        for t in singles:
            kk = (t['tx'], t['tz'])
            if kk in seen:
                continue
            seen.add(kk)
            cases.append(t)
        for label, t in zip(['high', 'arrow', 'base'], cases[3:6] or cases[:3]):
            b0 = base_of(ws, t['tx'], t['tz'])
            if b0 is None:
                continue
            print('\n== %s cut @ %d,%d ==' % (label, t['tx'], t['tz']), flush=True)
            audit_at(ws, t['tx'], b0 + 40, t['tz'], 'before')
            if label == 'high':
                aim_wood(ws, t['tx'], t['tz'], b0 + 70)
                swing_fell(ws, 25, gate=1e9)
            elif label == 'arrow':
                for dy in (20, 34, 48, 62):
                    j(ws, '__vb.arrowChopAt(%d,%d,%d)' % (t['tx'], b0 + dy, t['tz']))
                    time.sleep(0.3)
            else:
                aim_wood(ws, t['tx'], t['tz'], b0 + 2)
                swing_fell(ws, 30, gate=1e9)
            settle(ws, 16.0)
            if audit_at(ws, t['tx'], b0 + 40, t['tz'], 'after'):
                bad += 1

        print('\n===== %d case(s) produced floaters =====' % bad, flush=True)
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    sx = int(sys.argv[1]) if len(sys.argv) > 1 else 2400
    sz = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
    run((sx, sz))
