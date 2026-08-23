"""The Phase B gate: boot the real game and report everything a refactor could break.

Phase A could prove itself with `git diff` - the rebuild was byte-identical, so behaviour
was identical by construction. Phase B changes scope and evaluation order, so that proof
is gone and every step needs the game actually running. This is that check.

What it reports, and why each one is here:

  boot            the game reached a live __vb. Two whole classes of Phase B bug (a const
                  read before its declaration, a duplicate top-level name) show up as a
                  page that never gets here.
  errors          uncaught exceptions and console errors. tick() deliberately survives an
                  exception rather than freezing, so a broken frame loop is SILENT except
                  for this.
  __vb keys       the CDP tests depend on 267 debug entry points. A missing key means a
                  fragment did not run - the most likely Phase B failure, and invisible
                  on screen.
  screenshot      mean/stddev of the real pixels. CDP capture, never an in-page readback:
                  drawImage of a WebGPU canvas returns all zeros and reads as a false
                  black screen.
  deepHash        FNV over a fixed world-coordinate block, DEEP ONLY (y 24..72). Worldgen
                  is a pure function of world coords, so this is bit-exact across runs and
                  is the strongest signal here for anything touching the generator.
                  The depth band is not fussiness. The full-column hash is NOT stable
                  across boots: snow and grid-stamped creatures are real writes into W, so
                  a surface block carries whatever the weather and the animals were doing
                  that session. Measured: full-column came back 2632970165 / 2595177345 /
                  2444320179 on identical code while the deep band never moved. Anything
                  the generator does still shows up down here, because the surface is
                  built on top of it.
  gtest           re-runs the inline sweep over pool-built terrain: it returns 0 when the
                  worker pool and the main thread still agree bit-for-bit.

The game 404s ~17 assets on EVERY boot and always has: the frame loaders walk 00, 01, 02
... until one 404s, which is how they discover the sequence length, and sound/bow/reload
was deliberately removed. So "no console errors" is not the gate - an uncaught EXCEPTION
is, and console errors are compared against the baseline instead.
  frame           __vb.ft(), sampled over three windows with the MEDIAN compared. The
                  standing rule is that perf work must be perceptually lossless and a
                  refactor is held to the same bar - but a single window put p99 anywhere
                  in 6.2-6.7 ms on byte-identical code, so one sample cannot carry that
                  judgement. Re-baseline if the machine's load has changed: measured
                  against a quiet-machine baseline, a busy one reads as a regression.

  python tools/vbtest.py                    run, print a report
  python tools/vbtest.py --save NAME        also write the report to docs/baselines/NAME.json
  python tools/vbtest.py --against NAME     diff against a saved baseline, exit 1 on regression

Serves on its own probed-free port (never 8080) and runs Chrome offscreen in a per-run
profile, so it can neither fight the game you are playing, nor another agent's test run,
nor touch your cursor.
"""
import json, os, subprocess, sys, time, hashlib, struct, zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..'))
sys.path.insert(0, HERE)
import cdp

SLOT = cdp.SLOT                                      # per-run unless VB_SLOT is set
PORT = cdp.free_port(8100 + (sum(ord(c) for c in SLOT) % 200))   # never 8080
BASE = os.path.join(ROOT, 'docs', 'baselines')

# A fixed world coordinate, so worldHash covers the same terrain on every run. Nothing
# special about it beyond being away from spawn, which varies (spawn walks to a lake).
PROBE = (4096, 96, 4096)

# The bit-exact generator probe. Deep band only - see the note on deepHash above.
DEEP_HASH_JS = ("(()=>{let h=2166136261;"
                "for(let z=4096;z<4128;z++)for(let y=24;y<72;y++)for(let x=4096;x<4128;x++)"
                "{h=Math.imul(h^__vb.idAt(x,y,z),16777619)>>>0;}return h})()")


def png_stats(data):
    """Mean and stddev of the decoded PNG, without PIL."""
    pos, w, h, raw = 8, 0, 0, b''
    while pos < len(data):
        ln = struct.unpack('>I', data[pos:pos + 4])[0]
        typ = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + ln]
        if typ == b'IHDR':
            w, h = struct.unpack('>II', body[:8])
        elif typ == b'IDAT':
            raw += body
        elif typ == b'IEND':
            break
        pos += 12 + ln
    px = zlib.decompress(raw)
    stride = w * 4 + 1                                # RGBA + the per-row filter byte
    tot = n = 0
    sq = 0
    prev = bytearray(w * 4)
    out = bytearray(w * 4)
    for y in range(0, h, 8):                          # every 8th row is plenty for a mean
        row = px[y * stride:(y + 1) * stride]
        if len(row) < stride:
            break
        f = row[0]
        line = bytearray(row[1:])
        if f == 2:                                    # Up - the only filter we bother to undo
            for i in range(len(line)):
                line[i] = (line[i] + prev[i]) & 255
        elif f != 0:
            continue                                  # sampled rows only; skip odd filters
        prev = line
        for i in range(0, len(line) - 3, 4 * 4):
            v = (line[i] + line[i + 1] + line[i + 2]) / 3
            tot += v
            sq += v * v
            n += 1
    if not n:
        return {'mean': None, 'std': None, 'w': w, 'h': h}
    m = tot / n
    return {'mean': round(m, 2), 'std': round(max(0.0, sq / n - m * m) ** 0.5, 2), 'w': w, 'h': h}


def run():
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = ws = None
    try:
        time.sleep(1.0)
        url = 'http://127.0.0.1:%d/?cdp' % PORT
        print('booting', url)
        t0 = time.time()
        # __vb.tp is the readiness signal: __vb is assigned in one statement, so the
        # moment any key is callable the whole surface is up.
        proc, ws, errors = cdp.boot(url, ready_expr='typeof (window.__vb||{}).tp=="function"')
        boot_s = round(time.time() - t0, 1)
        print('  up in %.1fs' % boot_s)

        rep = {'boot_s': boot_s}

        keys = cdp.ev(ws, 'Object.keys(window.__vb).sort()')
        rep['vb_keys'] = len(keys)
        rep['vb_key_list'] = keys

        # Settle, then park at a fixed spot so the world block is the same every run.
        cdp.ev(ws, '__vb.tp(%d,%d,%d,0,0)' % PROBE)
        t1 = time.time()
        while time.time() - t1 < 90:                  # wait for the stream to catch up
            d = cdp.ev(ws, 'JSON.stringify(__vb.gen().deficit)')
            if d and max(json.loads(d).values()) <= 0:
                break
            time.sleep(1.0)
        rep['gen_caught_up'] = max(json.loads(cdp.ev(ws, 'JSON.stringify(__vb.gen().deficit)')).values()) <= 0

        rep['worldHash'] = cdp.ev(ws, '__vb.worldHash(%d,%d,64)' % (PROBE[0], PROBE[2]))
        rep['deepHash'] = cdp.ev(ws, DEEP_HASH_JS)
        g = cdp.ev(ws, 'JSON.stringify(__vb.gtest(64))')
        rep['gtest'] = json.loads(g) if g else None

        # THREE sampling windows in one boot, and the MEDIAN is what gets compared.
        # A single 6 s window put p99 anywhere in 6.2-6.7 ms on identical code, which was
        # enough to report a phantom 8% regression; the spread is the machine, not the
        # game. Sampling inside one boot also keeps the windows in the same conditions.
        samples = []
        for _ in range(3):
            cdp.ev(ws, '__vb.ftReset && __vb.ftReset()')
            time.sleep(6)
            ft = cdp.ev(ws, 'JSON.stringify(__vb.ft())')
            if ft:
                samples.append(json.loads(ft))
        rep['frame_samples'] = samples
        rep['frame'] = ({k: sorted(x[k] for x in samples)[len(samples) // 2]
                         for k in samples[0] if isinstance(samples[0][k], (int, float))}
                        if samples else None)
        rep['prof'] = cdp.ev(ws, 'JSON.stringify(__vb.prof())') and json.loads(cdp.ev(ws, 'JSON.stringify(__vb.prof())'))

        rep['screen'] = png_stats(cdp.shot(ws))
        errs = errors()
        rep['uncaught'] = [e for e in errs if e.startswith('uncaught:')]
        rep['errors'] = [e for e in errs if not e.startswith('uncaught:')]
        rep['err_404'] = sum(1 for e in rep['errors'] if '404' in e)
        return rep
    finally:
        try:
            if ws:
                ws.s.close()
        except Exception:
            pass
        if proc:
            proc.terminate()                          # the job object is the real backstop
        srv.terminate()


def report(rep):
    print()
    print('  boot          %.1fs' % rep['boot_s'])
    print('  __vb keys     %d' % rep['vb_keys'])
    s = rep['screen']
    print('  screen        %dx%d  mean %s  std %s' % (s['w'], s['h'], s['mean'], s['std']))
    print('  deepHash      %s   (gen caught up: %s)' % (rep['deepHash'], rep['gen_caught_up']))
    print('  worldHash     %s   (surface block - varies per session, informational)'
          % rep['worldHash'])
    print('  gtest         %s' % json.dumps(rep['gtest']))
    print('  frame(med3)   %s' % json.dumps(rep['frame']))
    print('     p99 spread %s' % [x['p99'] for x in rep.get('frame_samples', [])])
    print('  uncaught      %d' % len(rep['uncaught']))
    for e in rep['uncaught'][:10]:
        print('     ' + e)
    print('  console       %d  (%d are the expected asset 404 probes)'
          % (len(rep['errors']), rep['err_404']))
    for e in [x for x in rep['errors'] if '404' not in x][:6]:
        print('     ' + e)


def verdict(rep, base):
    """What actually counts as a regression, as opposed to noise."""
    bad = []
    if rep['uncaught']:
        bad.append('%d uncaught exception(s): %s' % (len(rep['uncaught']), rep['uncaught'][0]))
    # console errors are noisy by design (the 404 probes), so only a RISE counts
    if len(rep['errors']) > len(base['errors']):
        new = [e for e in rep['errors'] if e not in base['errors']]
        bad.append('%d new console error(s): %s' % (len(new), '; '.join(new[:3])))
    missing = set(base['vb_key_list']) - set(rep['vb_key_list'])
    if missing:
        bad.append('__vb lost %d key(s): %s' % (len(missing), ', '.join(sorted(missing)[:12])))
    if rep['deepHash'] != base['deepHash']:
        bad.append('deepHash changed %s -> %s : the generator no longer produces the same '
                   'world' % (base['deepHash'], rep['deepHash']))
    if rep['screen']['mean'] is not None and rep['screen']['mean'] < 2:
        bad.append('screen is black (mean %s)' % rep['screen']['mean'])
    if rep['gtest']:                       # gtest returns 0 when clean, {bad, list} when not
        bad.append('gtest reports %s pool-vs-inline voxel diffs' % rep['gtest'].get('bad'))

    # MEASURED noise floor: two identical runs came in 0.8% apart on avg and identical on
    # p99, so 8% is well clear of noise and still catches anything a player would feel.
    # The standing rule is that perf work is perceptually lossless; a refactor is held to
    # the same bar.
    a, b = rep.get('frame') or {}, base.get('frame') or {}
    for k in ('avg', 'p99'):
        if a.get(k) and b.get(k) and a[k] > b[k] * 1.08:
            bad.append('frame %s regressed %.2f -> %.2f ms (%+.0f%%)'
                       % (k, b[k], a[k], 100 * (a[k] / b[k] - 1)))
    return bad


if __name__ == '__main__':
    rep = run()
    report(rep)
    os.makedirs(BASE, exist_ok=True)

    if '--against' in sys.argv:
        name = sys.argv[sys.argv.index('--against') + 1]
        base = json.load(open(os.path.join(BASE, name + '.json'), encoding='utf8'))
        bad = verdict(rep, base)
        print()
        if bad:
            print('REGRESSION vs %s:' % name)
            for b in bad:
                print('  - ' + b)
            sys.exit(1)
        print('OK - no regression vs %s' % name)

    if '--save' in sys.argv:
        name = sys.argv[sys.argv.index('--save') + 1]
        with open(os.path.join(BASE, name + '.json'), 'w', encoding='utf8', newline='\n') as f:
            json.dump(rep, f, indent=1, sort_keys=True)
        print('\nsaved docs/baselines/%s.json' % name)
