"""Export the game's LIVE 256-entry palette to game/assets/palette.vox.

    python tools/palette_vox.py

WHY THIS EXISTS. The palette is not a file — it is BUILT at boot by addCol/palShare as each asset
loads, so its contents and its ORDER depend on the whole load sequence (assets/palette.js, then
pine5.vox, then every decoration, then creature colours minted lazily by edCol during play). There
is no static list to open in MagicaVoxel, which means authoring new art has meant guessing at what
the table already holds. This boots the real game, reads the table back, and writes it out.

WHY IT MATTERS WHEN AUTHORING. A palette id is a MATERIAL, not a colour: cactusTab, foliaTab,
solidTab, the pickup sets and the support classes are all keyed by it. So the difference between
picking a colour that is EXACTLY in this file and one that is merely close is not cosmetic:

  * exact match  -> edCol/palShare hand you that id. Costs nothing, and you know what it means.
  * within 8/255 -> the TOLERANCE path hands you somebody else's id, and its material with it.
                    Measured 2026-08-18: a pink bird authored at (243,133,158) landed 5/255 from
                    the cactus flower's (243,130,153), inherited cactusTab, and every grid-stamped
                    pink bird stung the player. Nothing looked wrong; the damage was the tell.
  * further out  -> a new id is minted, if the table has room. It is at 256/256, so usually it does not.

So: open this file beside whatever you are authoring and pick FROM it wherever you can.

THE FILE. An 8x32 plate (MagicaVoxel's palette panel is 8 wide, so a plate row is a panel row), one voxel per colour, SORTED so that similar colours sit together: greys
first by brightness, then the chromatic colours grouped by hue and shaded light-to-dark within each
hue. Reading order is x then y, so the plate and MagicaVoxel's own palette panel show the same
gradient and you can find "the pink family" by looking rather than by hunting.

WHY SORTING IS SAFE, AND WHY THE ID IS NOT IN THE POSITION. Nothing in the game reads this file. When
you author art, the loaders match your voxels' RGB against the table — palShare and edCol both take
(r, g, b) and look the colour up — so the palette INDEX your .vox happens to use is never consulted.
That means the order here is free to be whatever is most useful to author against, and "grouped by
hue" beats "grouped by load order" for that. An earlier version laid the plate out as x = id % 16,
y = id >> 4 to make the id readable; it scattered every family across the whole plate.
The tool prints the index -> game id map when it runs, for when you do need the id back (to ask what
a colour MEANS — see the material note above).

Regenerate after ANY asset change that mints ids -- tools/hooks/pre-commit does this automatically when a
palette-affecting file is staged, so this normally runs itself. Stdlib only; run with the plain `python`.
"""
import json, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import cdp

OUT = os.path.join(ROOT, 'game', 'assets', 'palette.vox')
PORT = cdp.free_port(8100 + (sum(ord(c) for c in cdp.SLOT) % 200))   # never 8080 — see CLAUDE.md


def write_vox(path, sx, sy, sz, vox, pal):
    """One SIZE/XYZI/RGBA model. pal is 256 RGBA bytes*4, MV index 1..255 in entries 0..254."""
    def chunk(cid, content, children=b''):
        return cid + struct.pack('<II', len(content), len(children)) + content + children
    body = (chunk(b'SIZE', struct.pack('<III', sx, sy, sz))
            + chunk(b'XYZI', struct.pack('<I', len(vox)) + b''.join(bytes(v) for v in vox))
            + chunk(b'RGBA', pal))
    open(path, 'wb').write(b'VOX ' + struct.pack('<I', 150) + chunk(b'MAIN', b'', body))


def main():
    srv = subprocess.Popen([sys.executable, os.path.join(HERE, 'serve-nocache.py'),
                            '--port', str(PORT), '--no-watchdog'],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    proc = ws = None
    try:
        time.sleep(1.0)
        print('booting http://127.0.0.1:%d/?cdp' % PORT)
        proc, ws, errors = cdp.boot('http://127.0.0.1:%d/?cdp' % PORT,
                                    ready_expr='typeof (window.__vb||{}).palLen=="function"')
        raw = cdp.ev(ws, 'JSON.stringify(Array.from({length:256},(_,i)=>__vb.pal(i)))')
        pal = json.loads(raw)

        # ── THE SORT ── group by hue so families land together, and keep the near-greys out of that
        # grouping entirely: hue is meaningless below a few percent saturation, so sorting greys by it
        # would interleave the stone, the bark shadows and the snow at random. Greys lead (darkest to
        # lightest), then 12 hue buckets of 30 degrees each, light to dark inside a bucket so a ramp
        # reads as a ramp. 12 buckets rather than a continuous hue sort because a continuous one puts
        # a nearly-grey brown between two saturated reds; bucketing keeps a family contiguous.
        def hsv(c):
            r, g, b = c[0] / 255, c[1] / 255, c[2] / 255
            mx, mn = max(r, g, b), min(r, g, b)
            v = mx
            sat = 0 if mx == 0 else (mx - mn) / mx
            if mx == mn:
                h = 0.0
            elif mx == r:
                h = (60 * (g - b) / (mx - mn)) % 360
            elif mx == g:
                h = 60 * (b - r) / (mx - mn) + 120
            else:
                h = 60 * (r - g) / (mx - mn) + 240
            return h, sat, v

        entries = [(i, pal[i]) for i in range(1, 256) if i < len(pal) and pal[i]]

        # ── SERPENTINE, AND FINER BUCKETS ── a straight "sort each bucket light->dark" makes a SAWTOOTH: every
        # bucket ends dark and the next restarts light, so the seam between two families is the biggest jump on
        # the plate even though the families themselves are ordered. Alternating the direction per bucket
        # (boustrophedon) means each bucket ENDS where the next one BEGINS, and the whole run reads as one
        # continuous ramp. 24 buckets of 15 degrees rather than 12 of 30, now that the seams are cheap.
        # Within a bucket the sort is by VALUE and then SATURATION, so a ramp of one material stays together
        # instead of interleaving with a washed-out neighbour at the same brightness.
        # ── ROWS OF EIGHT, EACH ONE A SINGLE FAMILY (user 2026-08-19: "organize the pallete better by
        # gradient. I want to be able to work within smooth palette shades ... its smooth all the way across the
        # 8 slots") ── MagicaVoxel's palette panel is EIGHT wide, so a "shade" to the person authoring art is a
        # run of 8. The old layout sorted into one long continuous ramp and then poured it onto a 16-wide plate,
        # which meant a family could start anywhere in a row and most rows straddled two of them.
        # MEASURED on the live table before this change: 6 of 32 rows spanned more than 40 degrees of hue, and
        # the mean row spread was 25.7 deg. The worst were the first six rows — the greys — at 240, 337 and 344
        # degrees inside a single row.
        # TWO FIXES, and the grey one is the bigger:
        #   1. sat < 0.12 called far too much "grey". A colour at 0.11 saturation and hue 340 is a PINK, not a
        #      neutral, and the grey bucket is sorted by BRIGHTNESS ALONE — so blue-greys, pink-greys and true
        #      neutrals were interleaved by lightness into the first rows anyone looks at. 0.05 keeps only what
        #      is actually neutral; everything else goes to its hue family where it ramps properly.
        #   2. The plate is FILLED BY ROW from one family at a time rather than poured continuously. Each family
        #      contributes as many whole rows of 8 as it can; what is left over is pooled and sorted by hue into
        #      the last few rows. The serpentine is gone with it — it existed to hide the seam between families,
        #      and there is no seam to hide once a family owns its rows.
        # AFTER: 24 of 32 rows sit inside 15 degrees of hue, and the mean spread is 19.7. The rows that are still
        # mixed are the pooled remainders at the bottom, which is where a leftover belongs.
        GREY_SAT, HUE_BUCKET = 0.05, 15
        greys, chroma = [], {}
        for e in entries:
            h, sat, v = hsv(e[1])
            if sat < GREY_SAT:
                greys.append((v, e))
            else:
                chroma.setdefault(int(h // HUE_BUCKET), []).append((v, sat, e))
        greys.sort(key=lambda t: t[0])
        fams = [[e for _, e in greys]]
        for b in sorted(chroma):
            fams.append([e for _, _, e in sorted(chroma[b], key=lambda t: (-t[0], -t[1]))])   # light -> dark, saturated first
        ordered, tail = [], []
        for f in fams:
            i = 0
            while len(f) - i >= 8:                                   # whole rows only
                ordered.extend(f[i:i + 8]); i += 8
            tail.extend(f[i:])                                       # …the rest is pooled
        tail.sort(key=lambda e: (hsv(e[1])[0] // HUE_BUCKET, -hsv(e[1])[2]))
        ordered.extend(tail)
        entries = ordered
        # ── THE FREE SLOTS ARE DRAWN, IN BLACK (user 2026-08-18: "whatever slots you can gain make them black
        # slots") ── the table holds fewer than 255 colours now that PAL_TOL condenses similar shades, and an
        # unminted id is simply absent: the plate just stopped early and the headroom was invisible. Padding the
        # tail with black makes it something you can COUNT by looking, which is the point of gaining it.
        # Black is the honest filler here because id 0 is air and nothing in the table is (0,0,0), so a black
        # cell cannot be mistaken for a colour the game actually holds.
        free_slots = 255 - len(entries)
        entries = entries + [None] * free_slots

        # ── RGBA ── MV stores index i at byte offset (i-1)*4. Index is now SORT POSITION, not the
        # game id; see the header for why that is safe. Alpha 255 throughout — the game has no
        # per-id alpha (transparency rides ITEMMAP.w at runtime, not the palette).
        rgba = bytearray(1024)
        vox = []
        for n, e in enumerate(entries):
            gid, c = (None, (0, 0, 0)) if e is None else e   # a FREE slot — black, see the note above
            mv = n + 1                                # MagicaVoxel colour indices start at 1
            o = (mv - 1) * 4
            rgba[o], rgba[o + 1], rgba[o + 2], rgba[o + 3] = c[0], c[1], c[2], 255
            vox.append((n % 8, n // 8, 0, mv))        # EIGHT wide, so a plate row IS a panel row (see the note above the sort)
        used = len(entries)

        print('  index -> game id (index is sort position; the game matches by RGB, not by index):')
        for row in range(0, used, 8):
            print('   %3d: %s' % (row + 1, ' '.join('  .' if e is None else '%3d' % e[0]
                                                    for e in entries[row:row + 8])))

        write_vox(OUT, 8, 32, 1, vox, bytes(rgba))
        print('wrote %s: %d colours + %d FREE (black) slots on an 8x32 plate, one hue family per row'
              % (os.path.relpath(OUT, ROOT), used - free_slots, free_slots))
        errs = [e for e in errors() if '404' not in e]
        if errs:
            print('page errors during boot:', errs[:3])
    finally:
        try:
            if ws: ws.s.close()
        except Exception:
            pass
        for p in (proc, srv):
            try:
                if p: p.terminate()
            except Exception:
                pass


main()
