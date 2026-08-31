# ── FAR-RING REGRESSION HARNESS ────────────────────────────────────────────────
# The ring's audits are all SNAPSHOTS, and the far field only misbehaves while the
# player is MOVING: tiles are adopted from the near window and evicted behind you in
# bulk, and that traffic is the whole mechanism. A standing probe measures the one
# state the bug does not live in.
#
# So this flies a fixed path and samples the ring EVERY FRAME, in-page. It reports
# correctness counters (stale descriptors, ownership, overflow) rather than timings,
# which is deliberate: correctness is safe to sample while another chrome is up, and
# frame numbers are not — see the one-chrome rule.
#
#   python tools/ringflight.py --slot e6 --biome arctic --frames 700
#
# WHY IT DOES NOT USE __vb.tp()
#   tp() and tod() both set resetHist. A per-frame loop that calls either wipes the
#   denoiser history every frame, and raw single-ray noise then reads as a renderer
#   bug. The flight assigns __vb.P.x/z directly instead; the streamer follows P on its
#   own (world/stream.js slides winOX off P.x), which is exactly the traffic we want.
#   gotoBiome IS used, once, before sampling starts — world coordinates are not
#   portable across loads (the seed is re-randomised and biome layout is spawn-relative),
#   so a biome name is the only repeatable way to say where.
import argparse, hashlib, json, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ap = argparse.ArgumentParser()
ap.add_argument('--slot', default=os.environ.get('VB_SLOT', 'e6'))
ap.add_argument('--biome', default='arctic')      # gotoBiome target; '' stays where the world put us
ap.add_argument('--frames', type=int, default=700)  # 700 matches the hand measurement this replaces
ap.add_argument('--speed', type=float, default=2.5) # voxels per frame — fast enough to force adopt/evict traffic
ap.add_argument('--heading', type=float, default=0.7)
ap.add_argument('--alt', type=float, default=90.0)  # fly high: we want streaming traffic, not collision
ap.add_argument('--json', default='')
a = ap.parse_args()

def srcHash():
    h = hashlib.sha1()
    root = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'src')
    for d, _, fs in sorted(os.walk(root)):
        for f in sorted(fs):
            if f.endswith('.js'):
                h.update(open(os.path.join(d, f), 'rb').read())
    return h.hexdigest()[:12]

SP = os.path.join(os.environ['TEMP'], 'claude', 'vbharness_%s.json' % a.slot)
if not os.path.exists(SP): sys.exit('no harness on slot %r - start it first' % a.slot)
cdp.PORT = json.load(open(SP))['dbg']
ws = cdp.WS(cdp.wait_target())
ws.call('Runtime.enable')

# ── SETTLE ── gotoBiome teleports, so the ring is mid-refill for a while afterwards.
# Sampling through that would score the arrival transient as a fault.
if a.biome:
    cdp.ev(ws, '__vb.gotoBiome(%r)' % a.biome)
    time.sleep(3.0)

# ── THE IN-PAGE SAMPLER ── one rAF loop that both DRIVES the flight and records the
# ring, because the two must agree frame for frame: polling from python samples at
# whatever rate the socket allows and would alias the per-frame drops we are counting.
FLIGHT = r'''
(() => {
  const N = %d, SPD = %f, HDG = %f, ALT = %f;
  const P = __vb.P, s = [];
  P.fly = true; P.y = ALT; P.vy = 0;
  const dx = Math.cos(HDG), dz = Math.sin(HDG);
  let i = 0;
  const step = () => {
    P.x += dx * SPD; P.z += dz * SPD; P.y = ALT; P.vy = 0;
    const r = __vb.ring(), o = r.own || {};
    s.push([r.filled|0, r.evicted|0, r.adopted|0, r.overflow|0, r.poolUsed|0,
            o.stale|0, o.zero|0, o.badTiles|0,
            r.adoptClear === undefined ? -1 : (r.adoptClear|0), r.tiles|0,
            r.squash === undefined ? -1 : (r.squash|0),
            r.free === undefined ? -1 : (r.free|0),
            r.abandoned === undefined ? -1 : (r.abandoned|0)]);
    if (++i < N) requestAnimationFrame(step);
    else { window.__RF = { s, slots: r.poolSlots|0, exposed: r.adoptClear !== undefined }; }
  };
  requestAnimationFrame(step);
  return N;
})()
''' % (a.frames, a.speed, a.heading, a.alt)

H0 = srcHash()
cdp.ev(ws, 'window.__RF = null; ' + FLIGHT)
deadline = time.time() + max(30.0, a.frames / 20.0 + 20.0)
while time.time() < deadline:
    time.sleep(1.0)
    if cdp.ev(ws, '!!window.__RF') is True: break
else:
    sys.exit('flight did not finish - is the sim ticking? (canvas focus / ?cdp)')

R = cdp.ev(ws, 'JSON.stringify(window.__RF)')
R = json.loads(R) if isinstance(R, str) else R
S, slots, exposed = R['s'], R['slots'], R['exposed']
if len(S) < 2: sys.exit('no samples')

fill = [r[0] for r in S]
drops = [fill[i-1] - fill[i] for i in range(1, len(fill)) if fill[i] < fill[i-1]]
d_ev, d_ad = S[-1][1] - S[0][1], S[-1][2] - S[0][2]
d_ov = S[-1][3] - S[0][3]
d_ac = (S[-1][8] - S[0][8]) if exposed else None
secs = len(S) / 60.0

onset = next((i for i in range(1, len(S)) if S[i][3] > S[i-1][3]), None)
_wdi = max(range(1, len(S)), key=lambda i: max(0, S[i-1][0] - S[i][0]))
_wdf = S[_wdi - 1][0]   # where the plane sat the frame BEFORE the worst drop
out = {
  'frames': len(S),
  'overflowOnsetFrame': onset,
  'tilesAtOnset': S[onset][9] if onset is not None else None,
  'overflowLastFrame': next((i for i in range(len(S) - 1, 0, -1) if S[i][3] > S[i-1][3]), None),
  'filledAtOnset': S[onset][0] if onset is not None else None,
  'tilesMax': max(r[9] for r in S),
  'squashMax': max(r[10] for r in S),
  'squashEnd': S[-1][10],
  'srcHash': H0, 'biome': a.biome,
  'filled': {'first': fill[0], 'last': fill[-1], 'min': min(fill), 'max': max(fill)},
  'dropFrames': len(drops),
  'worstDrop': max(drops) if drops else 0,
  'worstDropAtFilled': _wdf,
  'worstDropPct': round(100.0 * (max(drops) if drops else 0) / max(1, _wdf), 2),
  'meanDrop': round(sum(drops) / len(drops), 2) if drops else 0.0,
  'evicted': d_ev, 'adopted': d_ad,
  'evictPerSec': round(d_ev / secs, 1), 'adoptPerSec': round(d_ad / secs, 1),
  'overflow': d_ov,
  'poolHighWaterPct': round(100.0 * max(r[4] for r in S) / (slots or 1), 1),
  'poolHighWaterMinPct': round(100.0 * min(r[4] for r in S) / (slots or 1), 1),
  'liveOccPeakPct': (round(100.0 * max(r[4] - r[11] for r in S) / (slots or 1), 1)
                     if S[0][11] >= 0 else None),
  'liveOccMinPct': (round(100.0 * min(r[4] - r[11] for r in S) / (slots or 1), 1)
                    if S[0][11] >= 0 else None),
  'framesUnder86': (sum(1 for r in S if 100.0 * (r[4] - r[11]) / (slots or 1) < 86.0)
                    if S[0][11] >= 0 else None),
  'abandoned': (S[-1][12] - S[0][12]) if S[0][12] >= 0 else None,
  'abandonedLastFrame': next((i for i in range(len(S) - 1, 0, -1) if S[i][12] > S[i-1][12]), None),
  'ownStale': max(r[5] for r in S), 'ownZero': max(r[6] for r in S),
  'ownBad': max(r[7] for r in S),
  'adoptClear': d_ac,
}
H1 = srcHash()
if H1 != H0:
    out['srcHash'] = '%s->%s CHANGED MID-FLIGHT' % (H0, H1)
print(json.dumps(out, indent=2))

# ── VERDICT ── against the baselines measured by hand on a healthy build. These are
# the numbers to argue with when one of them moves, not thresholds to trust blindly.
bad = []
if H1 != H0: bad.append('src/ CHANGED DURING THE FLIGHT (%s -> %s) - this run measured two builds, discard it' % (H0, H1))
if out['worstDrop'] > 20:
    _sq = out['squashMax'] > 0 and out['worstDrop'] <= 40
    bad.append('worstDrop %d - %s' % (out['worstDrop'],
        'one squash step (RING_TILE >> 2 = 32) - the radius yielding at its rate limit' if _sq
        else ('materially larger than one squash step (32) - either steps are stacking within a '
              'frame or filled does not respond linearly to ringSquash' if out['squashMax'] > 0
              else 'healthy decays smoothly (~2); this is pumping')))
if out['ownStale'] or out['ownBad']: bad.append('ring ownership broken: stale=%d bad=%d (healthy 0/0)' % (out['ownStale'], out['ownBad']))
_olf = out['overflowLastFrame']
if out['overflow'] and _olf is not None and _olf > len(S) * 0.5:
    bad.append('pool overflow %d STILL REFUSING at frame %d of %d - this outlives the fill phase, '
               'so it is a real capacity ceiling' % (out['overflow'], _olf, len(S)))
elif out['overflow']:
    print('note: overflow %d confined to frames %s-%s of %d (fill transient, not a capacity ceiling)'
          % (out['overflow'], out['overflowOnsetFrame'], _olf, len(S)))
if out['squashEnd'] > 0 and out['squashEnd'] >= out['squashMax']:
    bad.append('squash may be RATCHETED: end %d == max %d - '
               'reach never came back' % (out['squashEnd'], out['squashMax']))
if d_ac is None:
    bad.append('adoptClear NOT EXPOSED - ringStats() omits ringAdoptClear, so the primary '
               'stale-descriptor signal cannot be read. Add `adoptClear: ringAdoptClear,` to ringStats0.')
elif d_ac == 0 and out['adopted'] > 50:
    bad.append('adoptClear 0 while %d tiles were adopted - the reclaim sweep is not firing' % out['adopted'])
print('\nVERDICT: ' + ('OK' if not bad else 'ATTENTION\n  - ' + '\n  - '.join(bad)))
sys.exit(1 if bad else 0)
