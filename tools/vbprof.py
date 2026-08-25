# ── V8 SAMPLING PROFILER against a RUNNING vbharness slot ──────────────────────
# The cprof buckets say WHICH PHASE a millisecond went to; this says which FUNCTION.
# It attaches to the harness's own Chrome (no second browser — see the one-chrome rule
# for timing runs), samples at 1 MHz for a chosen window, and prints self-time by
# function, then by source line for the file you care about.
#
#   python tools/vbprof.py --slot birch --seconds 12 --pre "__vb.snowHold(true)" --file tick-snow
import argparse, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default='birch')
ap.add_argument('--seconds', type=float, default=10)
ap.add_argument('--pre', default='')          # JS to run before sampling starts
ap.add_argument('--post', default='')         # …and after it stops
ap.add_argument('--file', default='')         # substring of a url/function to break down by line
ap.add_argument('--top', type=int, default=22)
a = ap.parse_args()

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r - start it first' % a.slot)
st = json.load(open(SP))
cdp.PORT = st['dbg']
ws = cdp.WS(cdp.wait_target())
ws.call('Runtime.enable')
if a.pre:
    cdp.ev(ws, a.pre)
    time.sleep(1.0)
ws.call('Profiler.enable')
ws.call('Profiler.setSamplingInterval', {'interval': 1000})   # µs
ws.call('Profiler.start')
time.sleep(a.seconds)
prof = ws.call('Profiler.stop')['result']['profile']
ws.call('Profiler.disable')
if a.post: cdp.ev(ws, a.post)

nodes = {n['id']: n for n in prof['nodes']}
self_hits = {}
for n in prof['nodes']:
    self_hits[n['id']] = n.get('hitCount', 0)
total = sum(self_hits.values()) or 1
by_fn, by_line = {}, {}
for nid, hits in self_hits.items():
    if not hits: continue
    cf = nodes[nid]['callFrame']
    name = cf.get('functionName') or '(anonymous)'
    url = cf.get('url') or ''
    key = '%s  [%s]' % (name, url.rsplit('/', 1)[-1] or 'native')
    by_fn[key] = by_fn.get(key, 0) + hits
    if a.file and (a.file in url or a.file in name):
        by_line[(name, cf.get('lineNumber', -1) + 1)] = by_line.get((name, cf.get('lineNumber', -1) + 1), 0) + hits

ms = a.seconds * 1000.0
print('sampled %.1fs, %d samples\n' % (a.seconds, total))
print('%-58s %8s %8s' % ('self time by function', 'share', 'ms'))
for k, v in sorted(by_fn.items(), key=lambda kv: -kv[1])[:a.top]:
    print('%-58s %7.1f%% %8.1f' % (k[:58], 100.0 * v / total, ms * v / total))
if by_line:
    print('\n%-46s %8s %8s' % ('…within ' + a.file + ', by declaration line', 'share', 'ms'))
    for (nm, ln), v in sorted(by_line.items(), key=lambda kv: -kv[1])[:a.top]:
        print('%-46s %7.1f%% %8.1f' % (('%s:%d' % (nm, ln))[:46], 100.0 * v / total, ms * v / total))
