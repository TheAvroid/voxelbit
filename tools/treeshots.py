"""Fell a stand of pines, then LOOK at it.

Every audit so far reads the world through a rule. floatAudit asks a graph question,
bodySupport asks a geometry question, and both came back clean while the report is that
there are floaters on screen. The instrument neither of them is, is the screen.

Fells N pines in one clearing and captures the scene from several angles and heights
after each one, plus a slow orbit at the end. Frames land in ../floatshots/.
"""
import base64, json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))
SHOTDIR = os.path.abspath(os.path.join(HERE, '..', 'floatshots'))


def j(ws, expr, timeout=300):
    r = cdp.ev(ws, 'JSON.stringify(%s)' % expr, timeout=timeout)
    return json.loads(r) if r else None


FIND_PINE = r"""
(() => {
  const P = __vb.P, out = [];
  const c0x = Math.floor(P.x / __vb.TCELL), c0z = Math.floor(P.z / __vb.TCELL);
  for (let dz = -4; dz <= 4; dz++) for (let dx = -4; dx <= 4; dx++) {
    const tr = __vb.treeAt(c0x + dx, c0z + dz); if (!tr) continue;
    out.push({ tx: tr.tx, tz: tr.tz, d: +Math.hypot(tr.tx - P.x, tr.tz - P.z).toFixed(1) });
  }
  out.sort((a, b) => a.d - b.d); return out.slice(0, 16);
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

# stand back at `d`, at bearing `ang`, and look at (tx,ty,tz)
VIEW = r"""
(() => {
  const P = __vb.P, tx = %f, ty = %f, tz = %f, d = %f, ang = %f, up = %f;
  const px = tx + Math.cos(ang) * d, pz = tz + Math.sin(ang) * d;
  __vb.tp(px, Math.max(2, ty + up), pz, Math.atan2(tx - px, tz - pz), 0);
  P.fly = true; P.y = Math.max(P.y, ty + up); P.vy = 0;
  __vb.eyeSync();
  const eye = __vb.dbgEye();
  P.pitch = Math.atan2(ty - eye, Math.hypot(tx - P.x, tz - P.z));
  __vb.eyeSync();
  return [+P.x.toFixed(0), +P.y.toFixed(0), +P.z.toFixed(0)];
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


def run(seed, ntrees):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://localhost:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        print('booted -> shots in', SHOTDIR, flush=True)
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tod(0.35)')                 # midday: no night frames to squint at
        cdp.ev(ws, '__vb.tp(%d,200,%d,0,0)' % seed)
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        time.sleep(4.0)

        pines = j(ws, FIND_PINE)
        cx, cz = seed
        felled = 0
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
            gy = base
            time.sleep(10.0)
            # the stump, from a little way off and slightly above the cut
            cam = j(ws, VIEW % (tx, gy + 30, tz, 60, 0.6, 45))
            time.sleep(2.5)
            print('[%d] pine %d,%d  cam=%s -> %s'
                  % (felled, tx, tz, cam, shot(ws, 'fell%02d-a' % felled)), flush=True)
            cam = j(ws, VIEW % (tx, gy + 55, tz, 95, 3.4, 60))
            time.sleep(2.5)
            print('     wide -> %s' % shot(ws, 'fell%02d-b' % felled), flush=True)

        # a slow orbit of the whole clearing at the end
        for k, ang in enumerate([0.0, 1.05, 2.09, 3.14, 4.19, 5.24]):
            cam = j(ws, VIEW % (cx, 230.0, cz, 150, ang, 70))
            time.sleep(2.8)
            print('orbit %d cam=%s -> %s' % (k, cam, shot(ws, 'orbit%d' % k)), flush=True)

        bs = j(ws, '__vb.bodySupport()') or []
        print('\nbodies:', len(bs), 'floating:',
              json.dumps([b for b in bs if b['floating']])[:600], flush=True)
        print('floatAudit:', json.dumps(j(ws, '__vb.floatAudit(90)'))[:400], flush=True)
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    sx = int(sys.argv[1]) if len(sys.argv) > 1 else 2400
    sz = int(sys.argv[2]) if len(sys.argv) > 2 else 2400
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 5
    run((sx, sz), n)
