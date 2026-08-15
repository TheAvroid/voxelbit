"""Clear a forest the way a player does, and watch the rigid-body budget while you do it.

Every previous floater test fells ONE tree into an empty budget and reads clean. The
solver's own comments say the interesting state is the other one:

  "MEASURED, in a calm forest with nothing chopped: PH.bodies pinned at 16/16"

A saturated budget is what forces supDrop to refuse (SUP.blockedNow -> requeue),
phSeparate to hand a whole severed tree to the resolver, and phChopLeaves to erase
instead of drop. So this fells tree after tree in one clearing without teleporting away,
and after every fell reports: floaters, the body count, what is holding the slots, and
what the resolver refused.
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


def settle(ws, secs, force=True):
    """Real play gets 2 ms/frame, not a forced flush. `force=False` measures that."""
    t0 = time.time()
    while time.time() - t0 < secs:
        if force:
            cdp.ev(ws, '__vb.supFlushNow()')
        time.sleep(1.0)


def audit(ws, rad=90):
    f = j(ws, '__vb.floatAudit(%d)' % rad)
    t = j(ws, '__vb.treeAudit(%d)' % rad)
    ph = j(ws, '__vb.phys()')
    sup = j(ws, '__vb.support()')
    # bodySupport, NOT bodyAudit's `gap`: that one scans a single column under the body's
    # single lowest voxel, which a felled pine lying at an angle fails while resting. It
    # reported three floating trunks on this very test; every one had 1000+ seated voxels.
    bd = j(ws, '__vb.bodySupport()') or []
    air = [b for b in bd if b['floating'] and b['n'] > 40]
    return {'floaters': f['floaters'], 'floaterVox': f['floaterVox'],
            'trunksInAir': t['floating'], 'worstDrop': t['worstDrop'],
            'sleepingInAir': len(air),
            'bodies': ph['bodies'], 'awake': ph['awake'],
            'queued': sup['queued'], 'blocked': sup['blocked'],
            'overflow': sup['stats']['overflow'], 'tooBig': sup['stats'].get('tooBig', 0),
            'depthHits': sup['stats']['depthHits'], 'capHits': sup['stats']['capHits'],
            'refused': len(sup.get('refused') or []),
            '_comps': f['top'][:6], '_trunks': t['top'][:3], '_air2': air[:3],
            '_bodyvox': sorted([b['vox'] for b in ph['body']], reverse=True)[:10],
            '_refused': (sup.get('refused') or [])[:5]}


def wood_aim(ws, tx, tz, ty):
    for ang in (0.0, 1.57, 3.14, 4.71):
        for d2 in (9, 7, 11, 6, 13):
            a = j(ws, AIM % (tx, ty, tz, d2, ang))
            if a.get('aim') and a['aim'].get('wood'):
                return True
    return False


def run(seed, ntrees, force):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://localhost:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        print('booted; seed', seed, 'trees', ntrees, 'forcedFlush', force, flush=True)
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        settle(ws, 4.0)
        pines = j(ws, FIND_PINE)
        print('pines found:', len(pines), flush=True)
        felled = 0
        for k, pine in enumerate(pines):
            if felled >= ntrees:
                break
            tx, tz = pine['tx'], pine['tz']
            base = j(ws, r"""(() => { for (let y = 2; y < 220; y++)
                if (__vb.woodAtW(%d, y, %d)) return y; return null; })()""" % (tx, tz))
            if base is None:
                continue
            cutY = base + 8
            if not wood_aim(ws, tx, tz, cutY):
                continue
            fell = None
            for sw in range(60):
                cdp.ev(ws, '__vb.physSwing()')
                time.sleep(0.06)
                lf = j(ws, '(__vb.phys().stats.lastFlood||null)')
                if lf and lf.get('orphans', 0) > 300:
                    fell = dict(lf, swings=sw + 1)
                    break
            if not fell:
                continue
            felled += 1
            settle(ws, 12.0, force)
            a = audit(ws)
            flag = 'FLOATERS' if (a['floaters'] or a['trunksInAir'] or a['sleepingInAir']) else 'clean'
            print('\n[%d] pine %d,%d  orph=%d  %s' % (felled, tx, tz, fell['orphans'], flag), flush=True)
            print('    ', json.dumps({kk: a[kk] for kk in a if not kk.startswith('_')}), flush=True)
            print('     bodies:', json.dumps(a['_bodyvox']), flush=True)
            if a['_comps']:
                print('     comps :', json.dumps(a['_comps'])[:600], flush=True)
            if a['_trunks']:
                print('     trunks:', json.dumps(a['_trunks'])[:400], flush=True)
            if a['_air2']:
                print('     FLOATING BODIES:', json.dumps(a['_air2'])[:500], flush=True)
            if a['_refused']:
                print('     REFUSED:', json.dumps(a['_refused'])[:600], flush=True)

        print('\n=== final, after a long settle ===', flush=True)
        settle(ws, 30.0, force)
        a = audit(ws)
        print(json.dumps({kk: a[kk] for kk in a if not kk.startswith('_')}), flush=True)
        print('bodies:', json.dumps(a['_bodyvox']), flush=True)
        if a['_comps']:
            print('comps:', json.dumps(a['_comps'])[:800], flush=True)
        if a['_trunks']:
            print('trunks:', json.dumps(a['_trunks'])[:500], flush=True)
        if a['_refused']:
            print('REFUSED:', json.dumps(a['_refused'])[:800], flush=True)
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    sx = int(sys.argv[1]) if len(sys.argv) > 1 else 2400
    sz = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    force = not (len(sys.argv) > 4 and sys.argv[4] == 'noforce')
    run((sx, sz), n, force)
