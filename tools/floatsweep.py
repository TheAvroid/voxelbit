"""The wall-free floater sweep, and the one that also looks off-grid.

floatAudit reports a component touching the region wall as INCONCLUSIVE, because it may
continue into the neighbouring slab - which is correct, and which means a single audit can
never clear a crown at the box edge. A forest is full of those: one run reported 44.

So this re-audits every inconclusive component from a centre where it is INTERIOR. A
component that is unreached from bedrock in a box it sits well inside is a real floater;
one that keeps touching a wall wherever it is centred is genuinely huge and reported
separately. Then bodySupport() covers the other half - the rigid bodies, which are not in
W at all - using "does ANY voxel have ground beneath it", not the single-lowest-voxel gap.

Usage: floatsweep.py [x] [z] [nfell]   (nfell 0 = audit the virgin world only)
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))


def j(ws, expr, timeout=300):
    r = cdp.ev(ws, 'JSON.stringify(%s)' % expr, timeout=timeout)
    return json.loads(r) if r else None


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


def settle(ws, secs, force=True):
    t0 = time.time()
    while time.time() - t0 < secs:
        if force:
            cdp.ev(ws, '__vb.supFlushNow()')
        time.sleep(1.0)


def wall_free_audit(ws, label):
    """floatAudit here, then re-audit each inconclusive component from its own centre."""
    a = j(ws, '__vb.floatAudit(90,{all:true})')
    real = [c for c in a['top'] if not c['wall']]
    incon = [c for c in a['top'] if c['wall']]
    confirmed, huge = [], []
    for c in incon[:8]:
        at = c['at']
        cdp.ev(ws, '__vb.tp(%d,%d,%d,0,0)' % (at[0], min(250, at[1] + 20), at[2]))
        time.sleep(2.5)
        cdp.ev(ws, '__vb.supFlushNow()')
        b = j(ws, '__vb.floatAudit(90,{all:true})')
        hit = None
        for c2 in b['top']:
            if abs(c2['at'][0] - at[0]) <= 6 and abs(c2['at'][2] - at[2]) <= 6 and abs(c2['at'][1] - at[1]) <= 8:
                hit = c2
                break
        if hit and not hit['wall']:
            confirmed.append(hit)
        elif hit:
            huge.append(hit)
    print('  [%s] floaters=%d floaterVox=%d  inconclusive=%d -> confirmed=%d stillWalled=%d'
          % (label, a['floaters'], a['floaterVox'], a['inconclusive'], len(confirmed), len(huge)), flush=True)
    if real:
        print('     interior floaters:', json.dumps(real[:5])[:600], flush=True)
    if confirmed:
        print('     CONFIRMED after re-centring:', json.dumps(confirmed[:5])[:700], flush=True)
    return {'floaters': a['floaters'], 'confirmed': len(confirmed), 'real': real, 'conf': confirmed}


def run(seed, nfell):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://localhost:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        print('booted; seed', seed, 'fells', nfell, flush=True)
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        settle(ws, 5.0)
        wall_free_audit(ws, 'virgin')

        if nfell:
            cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
            time.sleep(3.0)
            pines = j(ws, FIND_PINE)
            felled = 0
            for pine in pines:
                if felled >= nfell:
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
                got = False
                for sw in range(60):
                    cdp.ev(ws, '__vb.physSwing()')
                    time.sleep(0.06)
                    lf = j(ws, '(__vb.phys().stats.lastFlood||null)')
                    if lf and lf.get('orphans', 0) > 300:
                        got = True
                        break
                if not got:
                    continue
                felled += 1
                settle(ws, 14.0)
                bs = j(ws, '__vb.bodySupport()') or []
                fl = [b for b in bs if b['floating'] and b['n'] > 60]
                stalled = [b for b in bs if b['n'] > 500 and b['sleeping'] and not b['tipping'] and b['up'] > 0.9]
                print('\n[%d] pine %d,%d  bodies=%d  offGridFloating=%d  stalledUpright=%d'
                      % (felled, tx, tz, len(bs), len(fl), len(stalled)), flush=True)
                if fl:
                    print('     FLOATING BODIES:', json.dumps(fl[:4])[:800], flush=True)
                if stalled:
                    print('     STALLED UPRIGHT:', json.dumps(stalled[:4])[:800], flush=True)
                wall_free_audit(ws, 'fell%d' % felled)
                cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
                time.sleep(2.0)

        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    sx = int(sys.argv[1]) if len(sys.argv) > 1 else 2400
    sz = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    run((sx, sz), n)
