"""Render docs/end.mp4 -- the release card -- from the game's OWN wordmark and version drum.

Nothing here draws the logo. The card is the esc menu's `#lock h1` with its real CSS lifted out of
style-base.css, and the counting version is boot.js's verRoll/verSet lifted verbatim. Two things an
earlier pass got wrong, which this file exists to keep right:

  * The gold bevel is a text-shadow written in PIXELS -- 4px offsets against a 60px font. It only
    stays in proportion if the wordmark renders at its true 60px and is TRANSFORM-scaled up. Setting
    a big font-size instead leaves the bevel a thin scratch and the lettering stops matching the menu.
  * The card is not a still. The wordmark grows, linearly, 1.000 -> 1.299 across the whole 8 s about a
    fixed centre. Measured off the shipped v1.2 card at twelve samples.

Capture is frame-accurate because the page runs on a clock we step rather than on the wall. The drum
reads performance.now(), schedules on setTimeout and rolls on a CSS transition, so all three are moved
onto that clock -- see the __tick block. (Chrome's own virtual-time policy is the obvious tool here and
was tried first: Page.captureScreenshot deadlocks against it after ~50 frames.)

  python tools/endcard.py            # writes docs/end.mp4
  python tools/endcard.py --keep     # leave game/_endcard.html behind to look at
"""
import base64, io, os, re, shutil, subprocess, sys, tempfile, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
W, H, FPS, FRAMES, PORT = 3840, 2160, 24, 192, 8123

# -- MEASURED off the shipped v1.2 card (docs/end.mp4 @ c5ba170) so the new one is the same object --
ZOOM_TO   = 1.3004      # wordmark ink 899 -> 1169 px, LINEAR in t
INK_W0    = 899.0       # wordmark-only ink width on the first frame
TARGET_CX = 1857.5      # its ink centre stays put while it grows
TARGET_CY = 1081.0

PAGE = r"""<!doctype html><meta charset=utf-8><title>end</title>
<style>
@font-face { font-family: 'px3'; src: url('3x3-pixel.otf') format('opentype'); font-display: block; }
html, body { margin: 0; padding: 0; background: #000; width: 100%; height: 100%; overflow: hidden; }
body { display: flex; align-items: center; justify-content: center; }
#lock { display: flex; align-items: center; justify-content: center; }
</style>
<style>
__RULES__
</style>
<!-- BOTH ids ride ONE element: #lock h1 is the esc menu's wordmark, rule for rule, and #loadTitle is what
     the rolling drum's rules are written against. font-size is left ALONE at the esc menu's 60px -- the
     zoom is a transform, which is the only way the px-valued gold bevel scales along with the glyphs. -->
<div id="zoom"><div id="lock"><h1 id="loadTitle"><span class="tv" data-t="voxel">voxel</span><span
 class="tb" data-t="bit">bit</span><sup class="tver">v<span class="vd"><span class="vdr"><i>0</i></span></span>.<span
 class="vd"><span class="vdr"><i>0</i></span></span></sup></h1></div></div>
<script>
// -- A CLOCK WE CAN STEP -----------------------------------------------------------------------------
// The drum's timing lives in three places: performance.now() for its ramp, setTimeout for the cleanup
// that ends each roll, and a CSS transition for the roll itself. All three are moved onto VT here, so
// one __tick(dt) advances the whole animation by exactly one frame and nothing races the wall clock.
// The game's own code is untouched by this -- it just reads the clock it always read.
const nraf = window.requestAnimationFrame.bind(window);
let VT = 0; const rafQ = []; let toQ = [];
performance.now = () => VT;
window.requestAnimationFrame = (fn) => rafQ.push(fn);
window.setTimeout = (fn, ms) => { toQ.push({ t: VT + (+ms || 0), fn: fn }); return toQ.length; };
// a CSS transition runs on the document timeline, which we cannot move -- so pin each one by hand.
// t0 is stamped on the tick that created it, which IS the tick whose rAF set the style.
const pin = () => { for (const a of document.getAnimations()) {
  if (a.__t0 === undefined) a.__t0 = VT;
  a.pause(); try { a.currentTime = VT - a.__t0; } catch (e) {} } };
const fire = () => { const due = toQ.filter((e) => e.t <= VT); toQ = toQ.filter((e) => e.t > VT);
  due.sort((a, b) => a.t - b.t).forEach((e) => e.fn()); };
const frameStep = (h) => { VT += h; fire(); rafQ.splice(0, rafQ.length).forEach((fn) => fn(VT)); pin(); };
window.__now = () => VT;
// -- STEP AT 60 Hz, SHOOT AT 24 -- the drum is not fps-independent: verRoll defers its transform by one
// rAF and then holds a 95 ms busy gate, so the size of a frame decides how many ticks that gate eats. Run
// the page at 24 and the gate swallows whole numbers -- the drum counted 0.1 -> 0.3. 60 Hz is what the
// loading screen actually runs at, so that is the cadence the drum is captured on; the screenshot just
// lands wherever 1/24 s falls between those ticks.
window.__advance = (target) => {
  const H = 1000 / 60;
  while (VT + H <= target + 1e-9) frameStep(H);
  if (target - VT > 1e-9) { VT = target; fire(); pin(); }
};
window.__paint = () => new Promise((r) => nraf(() => nraf(() => r(1))));   // a REAL composited frame, to screenshot

const zoom = document.getElementById('zoom');
let ZK = 1, ZDX = 0, ZDY = 0, S0 = 1, ANIM = false;
const zapply = () => { zoom.style.transform = 'translate(' + ZDX + 'px,' + ZDY + 'px) scale(' + ZK + ')'; };
window.__setK = (k) => { ZK = k; zapply(); };
window.__setD = (dx, dy) => { ZDX = dx; ZDY = dy; zapply(); };
window.__origin = (ox, oy) => { const r = zoom.getBoundingClientRect();
  zoom.style.transformOrigin = (ox - r.left) + 'px ' + (oy - r.top) + 'px'; };

// -- THE GAME'S OWN DRUM -----------------------------------------------------------------------------
// verRoll and verSet below are lifted verbatim from src/core/boot.js, and the verSet line in step() is
// the one loadNumStep runs every frame. This does not imitate the loading screen; it IS it.
let loadVerT0 = 0; const LOAD_VER_MS = __LOAD_MS__, DUR = __DUR__, ZOOM_TO = __ZOOM_TO__;
const loadVerEl = document.querySelector('#loadTitle .tver');
const verCells = loadVerEl ? [...loadVerEl.querySelectorAll('.vd')] : [];
__VERROLL__
const VER_TO = __VER_TO__;
__VERSET__
const step = () => {
  if (ANIM) {
    const t = Math.min(1, Math.max(0, (performance.now() - loadVerT0) / DUR));
    ZK = S0 * (1 + (ZOOM_TO - 1) * t); zapply();
    verSet(Math.min(VER_TO, (performance.now() - loadVerT0) / LOAD_VER_MS * VER_TO).toFixed(1));
  }
  requestAnimationFrame(step);
};
window.__go = (s0) => { S0 = s0; ANIM = true; loadVerT0 = VT;
  fire(); rafQ.splice(0, rafQ.length).forEach((fn) => fn(VT)); pin(); };   // render frame 0
requestAnimationFrame(step);
window.__ready = () => document.fonts.ready.then(() => 1);
</script>
"""


def build_page():
    src = io.open(os.path.join(ROOT, 'src/core/boot.js'), encoding='utf-8').read()
    i = src.index('  const verRoll = (cell, ch) => {')
    ver_roll = src[i:src.index('  };   // the version tag counts', i) + 4]
    j = src.index('  const verSet = (vs) => {')
    ver_set = src[j:src.index('  };', src.index('verRoll(verCells[0], vs[0])', j)) + 4]
    ver_to = re.search(r'const VER_TO = ([\d.]+);', src).group(1)
    load_ms = re.search(r'LOAD_VER_MS = (\d+)', src).group(1)

    css = io.open(os.path.join(ROOT, 'src/ui/style-base.css'), encoding='utf-8').read().split('\n')
    rules = '\n'.join(ln for ln in css if re.search(r'#lock h1|#loadTitle', ln))
    if 'text-shadow: 0px -4px 0' not in rules:
        raise SystemExit('the gold bevel rule did not come across -- check the style-base selectors')

    page = (PAGE.replace('__RULES__', rules).replace('__VERROLL__', ver_roll)
                .replace('__VERSET__', ver_set).replace('__VER_TO__', ver_to)
                .replace('__LOAD_MS__', load_ms).replace('__ZOOM_TO__', str(ZOOM_TO))
                .replace('__DUR__', '%.4f' % ((FRAMES - 1) * 1000.0 / FPS)))
    io.open(os.path.join(ROOT, 'game/_endcard.html'), 'w', encoding='utf-8').write(page)
    return ver_to


def shot(ws):
    r = ws.call('Page.captureScreenshot', {'format': 'png', 'fromSurface': True,
                                           'captureBeyondViewport': False}, timeout=120)
    return base64.b64decode(r['result']['data'])


def frame(ws, t_ms):
    """Advance the page's clock to t_ms (in 60 Hz steps), let a real frame composite, screenshot it."""
    cdp.ev(ws, '__advance(%.6f)' % t_ms)
    cdp.ev(ws, '__paint()')
    return shot(ws)


def ink(png):
    """(x0, x1, y0, y1) of the WORDMARK's ink, with the version badge left out.

    Separated by COLOUR, not by a gap in the ink. The obvious test -- 'the badge is whatever sits past
    a wide horizontal gap' -- silently fails: at this size the gap is 24px, inside the range of an
    ordinary letter gap, so the badge got measured as part of the wordmark and the whole card came out
    0.87x too small. The wordmark is crimson, lime and gold; the badge is grey. That never blurs.
    """
    from PIL import Image
    im = Image.open(io.BytesIO(png)).convert('RGB'); px = im.load()
    def sat(x, y):
        r, g, b = px[x, y]; mx = max(r, g, b)
        return mx > 60 and (mx - min(r, g, b)) / mx > 0.30
    xs = [x for x in range(im.width) if any(sat(x, y) for y in range(0, im.height, 2))]
    ys = [y for y in range(im.height) if any(sat(x, y) for x in range(0, im.width, 2))]
    if not xs or not ys: return None
    return (min(xs), max(xs), min(ys), max(ys))


def main():
    ver_to = build_page()
    srv = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools/serve-nocache.py'),
                            '--port', str(PORT)], cwd=ROOT,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    out = tempfile.mkdtemp(prefix='endcard-')   # 192 4K PNGs, ~11 MB -- regenerable, so not in the repo
    # serve-nocache serves game/ AS the root, so the card is at /_endcard.html, not /game/...
    proc = cdp.launch('http://127.0.0.1:%d/_endcard.html' % PORT)
    time.sleep(3.0)
    try:
        ws = cdp.WS(cdp.wait_target()); ws.s.settimeout(180)
        ws.call('Page.enable'); ws.call('Runtime.enable')
        ws.call('Emulation.setDeviceMetricsOverride',
                {'width': W, 'height': H, 'deviceScaleFactor': 1, 'mobile': False})
        if cdp.ev(ws, 'window.__ready()') != 1:
            raise SystemExit('page never became ready')

        # -- PASS 1: measure the wordmark at its natural 60px, then solve the scale and the offset --
        cdp.ev(ws, '__setK(1)')
        b = ink(frame(ws, 0))   # nothing has started yet; this just composites the static wordmark
        if not b: raise SystemExit('no ink at scale 1 -- did the font load?')
        s0 = INK_W0 / (b[1] - b[0])
        cx, cy = (b[0] + b[1]) / 2.0, (b[2] + b[3]) / 2.0
        cdp.ev(ws, '__origin(%f,%f)' % (cx, cy))      # pin the ink centre so it cannot drift as it grows
        cdp.ev(ws, '__setD(%f,%f)' % (TARGET_CX - cx, TARGET_CY - cy))
        cdp.ev(ws, '__setK(%f)' % s0)
        c = ink(frame(ws, 0))
        print('natural ink w %d -> S0 %.4f | placed w %d cx %.1f cy %.1f (want %d / %.0f / %.0f)'
              % (b[1] - b[0], s0, c[1] - c[0], (c[0] + c[1]) / 2.0, (c[2] + c[3]) / 2.0,
                 INK_W0, TARGET_CX, TARGET_CY))

        # -- PASS 2: the real animation, one screenshot per 1/24 s of its own clock --
        cdp.ev(ws, '__go(%f)' % s0)
        for i in range(FRAMES):
            png = frame(ws, i * 1000.0 / FPS)
            io.open(os.path.join(out, 'f%04d.png' % i), 'wb').write(png)
            if i % 48 == 0: print('  frame %d/%d' % (i, FRAMES))
    finally:
        for k in (proc, srv):
            try: k.kill()
            except Exception: pass

    dst = os.path.join(ROOT, 'docs/end.mp4')
    subprocess.run(['ffmpeg', '-y', '-v', 'error', '-framerate', str(FPS),
                    '-i', os.path.join(out, 'f%04d.png'), '-c:v', 'libx264', '-preset', 'slow',
                    '-crf', '16', '-pix_fmt', 'yuv420p', '-movflags', '+faststart', dst], check=True)
    print('wrote %s  %d bytes  (counts up to v%s)' % (dst, os.path.getsize(dst), ver_to))
    shutil.rmtree(out, ignore_errors=True)
    if '--keep' not in sys.argv:
        try: os.remove(os.path.join(ROOT, 'game/_endcard.html'))
        except OSError: pass


main()
