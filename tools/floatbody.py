"""Is a felled trunk that reports a `gap` actually hanging in the air?

bodyAudit's `gap` is the clearance under the body's single lowest voxel in that one
column, which a tree lying at an angle fails routinely while resting perfectly. So this
asks three independent questions about the same body and only calls it a floater when
all three agree:

  1. bodySupport() - does ANY voxel of the body have solid ground within 1.5 beneath it?
  2. phWakeAll()   - wake every sleeping body. One that was never supported FALLS; one
                     that was resting stays put and goes straight back to sleep.
  3. a screenshot  - the thing the user actually sees.

Fells trees in one clearing until a suspect appears, then runs all three on it.
"""
import base64, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))
SHOTDIR = os.path.join(HERE, '..', 'floatshots')


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

# park the camera so a given world point fills the view, then hold still
LOOKAT = r"""
(() => {
  const P = __vb.P, tx = %f, ty = %f, tz = %f, back = %d;
  const px = tx + back, pz = tz + back;
  __vb.tp(px, Math.max(2, ty - 30), pz, Math.atan2(tx - px, tz - pz), 0);
  const eye = __vb.dbgEye();
  P.pitch = Math.atan2(ty - eye, Math.hypot(tx - P.x, tz - P.z));
  __vb.eyeSync();
  return [+P.x.toFixed(1), +P.y.toFixed(1), +P.z.toFixed(1)];
})()
"""


def shot(ws, name):
    os.makedirs(SHOTDIR, exist_ok=True)
    d = ws.call('Page.captureScreenshot', {'format': 'png'})
    b = d.get('result', {}).get('data')
    if not b:
        return None
    p = os.path.join(SHOTDIR, name + '.png')
    with open(p, 'wb') as fh:
        fh.write(base64.b64decode(b))
    return p


def settle(ws, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        time.sleep(1.0)


def suspects(ws):
    bs = j(ws, '__vb.bodySupport()') or []
    return [b for b in bs if b['floating'] and b['n'] > 200]


def run(seed, ntrees):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://127.0.0.1:%d/?cdp' % PORT,
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
        settle(ws, 4.0)

        pines = j(ws, FIND_PINE)
        felled, found = 0, None
        for pine in pines:
            if felled >= ntrees or found:
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
            settle(ws, 14.0)
            sus = suspects(ws)
            print('[%d] pine %d,%d  bodies=%d  suspects=%d'
                  % (felled, tx, tz, len(j(ws, '__vb.bodySupport()') or []), len(sus)), flush=True)
            for s in sus:
                print('     ', json.dumps(s), flush=True)
            if sus:
                found = sus[0]

        if not found:
            print('\nNO FLOATING BODY REPRODUCED in %d fells' % felled, flush=True)
            print('all bodies:', json.dumps(j(ws, '__vb.bodySupport()'))[:1500], flush=True)
            return

        print('\n=== SUSPECT ===', json.dumps(found), flush=True)
        # 1. look at it
        cam = j(ws, LOOKAT % (found['pos'][0], found['pos'][1], found['pos'][2], 46))
        time.sleep(2.5)
        print('camera at', cam, '->', shot(ws, 'suspect-before'), flush=True)

        # 2. wake it and see whether it falls
        before = j(ws, '__vb.bodySupport()')
        b0 = [b for b in before if b['i'] == found['i']]
        woke = j(ws, '__vb.phWakeAll()')
        print('woke', woke, 'sleeping bodies', flush=True)
        settle(ws, 12.0)
        after = j(ws, '__vb.bodySupport()')
        print('shot:', shot(ws, 'suspect-after'), flush=True)
        b1 = [b for b in after if b['n'] == found['n']]
        print('BEFORE wake:', json.dumps(b0), flush=True)
        print('AFTER  wake:', json.dumps(b1), flush=True)
        drop = None
        if b0 and b1:
            drop = round(b0[0]['pos'][1] - b1[0]['pos'][1], 2)
        print('\nVERDICT: the body %s when woken (dy = %s)'
              % ('FELL' if (drop or 0) > 0.5 else 'did NOT move', drop), flush=True)
        print('still floating after wake:',
              json.dumps([b for b in after if b['floating'] and b['n'] > 200])[:600], flush=True)
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
    run((sx, sz), n)
