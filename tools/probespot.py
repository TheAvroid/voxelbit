"""Look at one voxel, in a world nobody has touched.

Takes a world point and reports what is there, what the resolver thinks holds it up, and
whether floatAudit calls it a floater - with NO chopping at all, so the answer separates
"the player made this" from "the generator did".
"""
import json, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import cdp

PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))


def j(ws, expr, timeout=300):
    r = cdp.ev(ws, 'JSON.stringify(%s)' % expr, timeout=timeout)
    return json.loads(r) if r else None


def run(x, y, z):
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = None
    try:
        time.sleep(1.0)
        proc, ws, errors = cdp.boot('http://127.0.0.1:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).bodySupport=="function"')
        cdp.ev(ws, '__vb.snow(0)')
        cdp.ev(ws, '__vb.tp(%d,%d,%d,0,0)' % (x, max(2, y - 40), z))
        t1 = time.time()
        while time.time() - t1 < 180:
            d = j(ws, '__vb.gen().deficit')
            if d and max(d.values()) <= 0:
                break
            time.sleep(1.0)
        for _ in range(6):
            cdp.ev(ws, '__vb.supFlushNow()')
            time.sleep(1.0)

        print('VIRGIN world, nothing chopped, at %d,%d,%d' % (x, y, z), flush=True)
        cells = {}
        for dy in (-2, -1, 0, 1, 2):
            row = []
            for dz in (-1, 0, 1):
                for dx in (-1, 0, 1):
                    v = j(ws, '__vb.vox(%d,%d,%d)' % (x + dx, y + dy, z + dz))
                    row.append(v)
            cells['y%+d' % dy] = row
        print('3x3 columns y-2..y+2:', json.dumps(cells), flush=True)
        for dy in (0, 1):
            print('supWhy y%+d:' % dy, json.dumps(j(ws, '__vb.supWhy(%d,%d,%d)' % (x, y + dy, z)))[:400], flush=True)
        print('isFolia46=%s isCone46=%s isFolia66=%s isCone66=%s isWood66=%s isSnow66=%s'
              % (j(ws, '__vb.isFoliaId(46)'), j(ws, '__vb.isConeId(46)'),
                 j(ws, '__vb.isFoliaId(66)'), j(ws, '__vb.isConeId(66)'),
                 j(ws, '__vb.isWoodId(66)'), j(ws, '__vb.isSnowId(66)')), flush=True)
        fa = j(ws, '__vb.floatAudit(90)')
        print('floatAudit:', json.dumps({k: fa[k] for k in ('floaters', 'floaterVox', 'inconclusive')}), flush=True)
        print('  comps:', json.dumps(fa['top'][:5])[:600], flush=True)
        print('support:', json.dumps(j(ws, '__vb.support()'))[:300], flush=True)
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('PAGE ERRORS:', errs[:5], flush=True)
    finally:
        if proc: proc.kill()
        srv.kill()


if __name__ == '__main__':
    run(int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3]))
