"""Re-texture the birch WOOD: a white trunk carrying dark horizontal lenticels. Leaves are never touched.

    "search the internet for birch trees. theres more white then there is black/grey.
     retexture the wood again on the birch trees."                              - user, 2026-08-23
    "the leaves are fine though"                                                - user, 2026-08-23

    python tools/birch_bark_white.py            measure only  (default)
    python tools/birch_bark_white.py --apply    rewrite the .vox files

WHAT A PAPER BIRCH ACTUALLY LOOKS LIKE (Morton Arboretum, Minnesota DNR, NPS): the bark is "very white" /
"pure white with age", marked by LENTICELS - black, raised, HORIZONTAL, elongated dashes - plus V-shaped dark
marks where branches meet the trunk. The dark is a MARKING ON a white field, not the field itself. Only the
oldest bark at the base thickens and goes near-black and scaly. So white must dominate by a wide margin, and
the black must read as dashes rather than as mottling.

THE FIRST ATTEMPT GOT THIS BACKWARDS and it is worth recording why. It mapped the three shades of the fitted
bake ramp onto white/grey BY LUMINANCE RANK - darkest to darkest - which sounds faithful and is not, because
the ramp's rank order is a fact about the SOURCE PHOTOGRAPH's lighting, not about the tree. The bake's most
populous bark tone was the shaded side of the trunk and the branch wood, so rank-mapping made 63% of all bark
the near-black and left 27% white. The tree came out grey with white bits.

SO THE SPLIT IS GEOMETRIC NOW, and the geometry says what the botany says:
  THICK bark (>= THICK_NB of its 26 neighbours are also bark) is a trunk or a limb - the white field.
  THIN bark is the fine twig network out in the crown, which really is dark on a real birch.
Measured on tree 7: 80% thick, 20% thin. The white field then takes its lenticels from a hash quantised to
(x>>3, y>>3, z) - CONSTANT across eight voxels horizontally and changing every voxel vertically, which is what
makes a dash lie flat instead of reading as noise. Net result is roughly 70% white to 30% dark, against 27/73
before, and on the trunk itself - which is all the player looks at - it is ~89% white.

Whites and greys are two shades each because a bark shade costs one GAME palette id and they cannot be shared
(assets/bow.js reserves them with palOwn - these ids are about to be told they are WOOD, and one shared with
stone would make the stone choppable). With the birches unloaded the palette is at 252/256, so four is all
there is.

IDEMPOTENT, unlike birch_deblack: every verdict comes from position and neighbour count, never from the colour
already there, so running it twice changes nothing. Safe to re-run after a bake.
Structure-preserving: only colour bytes and the first four palette entries move.

Stdlib only.
"""
import glob, math, os, struct, sys

DIR = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                   'game', 'assets', 'foilage', 'birch_trees')
APPLY = '--apply' in sys.argv[1:]
for a in sys.argv[1:]:
    if a != '--apply': sys.exit('unknown argument %s' % a)

WHITE_HI, WHITE_LO = (0xF4, 0xF3, 0xEE), (0xDF, 0xDE, 0xD6)
GREY_MID, GREY_LOW = (0x4A, 0x4A, 0x4F), (0x2B, 0x2B, 0x2F)
NBARK = 4
THICK_NB = 7                                           # >= this many bark neighbours (of 26) = trunk or limb. 10 left 55% of bark dark because the big trees carry far more twig than tree 7 does; 7 keeps every limb white and leaves only the genuinely hair-thin ends dark
# -- A COURSE EVERY BAND VOXELS, GUARANTEED, AND THE BAND TIGHTENS TOWARD THE GROUND -----------------
# (user 2026-08-24: "put more black stripes towards the bottom of the birch tree trunk: its plain white",
#  then "some trees are fine but others are plain white at the base")
# The old rule quantised z into bands of DASH_GAP and gave each band a ~50% COIN to carry a mark. That is
# the right average and the wrong distribution: a slim trunk is one (x>>3, y>>3) column, so it rolls the
# same coin all the way up, and a run of tails is a long stretch of blank white. MEASURED over the shipped
# models by height decile, tree 3 came out 0% grey across its bottom TWO deciles and 0% again at d6 and d8 -
# exactly the plain-white base the user photographed - while its neighbours looked fine.
# So the coin is gone. `phase` below is a monotone function of z whose slope is 1/band, and a course is drawn
# wherever it crosses an integer: EXACTLY one course per band of height, everywhere, with no run of luck to
# sit out. The per-column offset keeps two trunks side by side from banding at the same heights, which is all
# the randomness this ever needed.
# The band ramps with height, which is also what a paper birch does: the oldest bark at the base is roughest
# and most heavily marked, the youngest at the top is nearly clean white.
BAND_LO = 5                                            # one lenticel course per this many voxels at the BASE - 20% of it dark, and never two courses closer than 4 apart (the spacing asked for earlier on 2026-08-24)
BAND_HI = 13                                           # ...and this far apart at the crown. Mean grey share over the trunk is ln(HI/LO)/(HI-LO) ~= 12%
NB26 = [(a, b, c) for a in (-1, 0, 1) for b in (-1, 0, 1) for c in (-1, 0, 1) if a or b or c]


def chunks(d):
    out, i = [], 8
    while i < len(d) - 12:
        cid = d[i:i + 4]
        cs, ks = struct.unpack_from('<II', d, i + 4)
        i += 12
        if cid != b'MAIN': out.append((cid, i, cs))
        i += cs
    return out


def h3(x, y, z):
    n = (x * 73856093) ^ (y * 19349663) ^ (z * 83492791)
    n = (n ^ (n >> 13)) * 1274126177 & 0x7FFFFFFF
    return ((n ^ (n >> 16)) & 0xFFFF) / 65535.0


print('%-8s %8s %8s %8s %8s %8s   %s' % ('tree', 'bark', 'whiteHi', 'whiteLo', 'greyMid', 'greyLow', 'white share'))
tot = [0, 0, 0, 0]
# ONLY THE NUMBERED TREES. This used to glob every .vox in the folder, and the folder is not only
# trees: shrub_1.vox / shrub_2.vox were dropped in beside them and a bare --apply recoloured those
# too, rewriting palette entries that were never bark. A tool that edits art in place has no business
# guessing which files are its own - the numbering IS the contract (tools/birch_renumber.py).
for path in sorted([q for q in glob.glob(os.path.join(DIR, '*.vox')) if os.path.basename(q)[:-4].isdigit()],
                   key=lambda p: int(os.path.basename(p)[:-4]) if os.path.basename(p)[:-4].isdigit() else 1 << 30):
    d = bytearray(open(path, 'rb').read())
    ch = chunks(d)
    rgba_o = next(o for cid, o, cs in ch if cid == b'RGBA')
    xyzi = [(o, cs) for cid, o, cs in ch if cid == b'XYZI']
    occ = {}
    for o, cs in xyzi:
        n = struct.unpack_from('<I', d, o)[0]
        for k in range(n):
            b = o + 4 + k * 4
            occ[(d[b], d[b + 1], d[b + 2])] = d[b + 3]
    # The ramp is measured against THIS model's own bark, so a 140-voxel tree and a 240-voxel one both get a
    # marked base and a clean crown, rather than the ramp running out partway up the tall one.
    bz = [k[2] for k, v in occ.items() if v <= NBARK]
    bz0, bz1 = (min(bz), max(bz)) if bz else (0, 1)
    bspan = max(1, bz1 - bz0)
    K_PH = bspan / float(BAND_HI - BAND_LO)            # the integral of 1/band(z) for a band that ramps linearly with height

    def phase(z):
        bw = BAND_LO + (BAND_HI - BAND_LO) * min(1.0, max(0.0, (z - bz0) / float(bspan)))
        return K_PH * math.log(bw / float(BAND_LO))

    cnt = [0, 0, 0, 0]
    for o, cs in xyzi:
        n = struct.unpack_from('<I', d, o)[0]
        for k in range(n):
            b = o + 4 + k * 4
            if d[b + 3] > NBARK: continue              # a leaf — never a candidate
            x, y, z = d[b], d[b + 1], d[b + 2]
            nb = 0
            for dx, dy, dz in NB26:
                q = occ.get((x + dx, y + dy, z + dz))
                if q is not None and q <= NBARK: nb += 1
            # -- ONE RULE FOR ALL BARK: WHITE FIELD, LENTICEL DASHES (user 2026-08-24: "there seems to be too
            # much dark grey in [the branches] ... make the branches match the white/dark grey ratio of the
            # trunks") -- this used to split on thickness and paint THIN bark, the branch and twig network out
            # in the crown, entirely dark, reasoning that a real birch twig is dark. Measured over the shipped
            # models that came out as trunk 89% white / 11% grey against branch 0% / 100%, and from inside the
            # wood the crown read as a mass of dark sticks rather than as a birch.
            # A birch's LIMBS are white like its trunk; only the last hair-thin ends darken, and at 10 cm those
            # are thinner than a voxel anyway. So the field/dash rule applies to every bark voxel now and the
            # ratio is the same everywhere by construction: whatever DASH is, that is the grey share.
            # A LENTICEL BAND IS ONE COURSE AND THE NEXT IS AT LEAST DASH_GAP ABOVE IT. The dash used to be drawn
            # by hashing each course independently, so nothing stopped two or three landing on consecutive rows -
            # which reads as a thick smear rather than the fine ladder a birch actually wears.
            # So z is quantised into bands of DASH_GAP and only the band's FIRST course may darken; the per-column
            # offset keeps neighbouring trunks from banding at the same heights. The overall grey share is
            # unchanged - the band probability is DASH * DASH_GAP, which is the same ink spread further apart.
            off = h3(x >> 3, y >> 3, 7)                # per-column phase, so neighbouring trunks do not band together
            ph0, ph1 = phase(z - 1) + off, phase(z) + off
            isDash = int(ph0) != int(ph1)              # the phase crossed an integer between this course and the one below: draw the lenticel
            nc = (4 if h3(x >> 3, y >> 3, int(ph1)) < 0.45 else 3) if isDash \
                 else (2 if h3(x >> 2, y >> 2, z >> 1) < 0.28 else 1)
            cnt[nc - 1] += 1
            if APPLY: d[b + 3] = nc
    if APPLY:
        pal = bytearray(d[rgba_o:rgba_o + 1024])
        for i, c in enumerate((WHITE_HI, WHITE_LO, GREY_MID, GREY_LOW)):
            pal[i * 4:i * 4 + 3] = bytes(c); pal[i * 4 + 3] = 255
        d[rgba_o:rgba_o + 1024] = pal
        open(path, 'wb').write(bytes(d))
    for i in range(4): tot[i] += cnt[i]
    s = sum(cnt)
    print('%-8s %8d %8d %8d %8d %8d   %5.1f%%' % (os.path.basename(path)[:-4], s, cnt[0], cnt[1], cnt[2], cnt[3],
                                                  100.0 * (cnt[0] + cnt[1]) / max(1, s)))
s = sum(tot)
print('%-8s %8d %8d %8d %8d %8d   %5.1f%% white' % ('ALL', s, tot[0], tot[1], tot[2], tot[3],
                                                    100.0 * (tot[0] + tot[1]) / max(1, s)))
print('%s  whites %02x%02x%02x/%02x%02x%02x  greys %02x%02x%02x/%02x%02x%02x  thick>=%d dash=%.2f'
      % ('APPLIED' if APPLY else 'measured only - pass --apply', *WHITE_HI, *WHITE_LO, *GREY_MID, *GREY_LOW, THICK_NB, BAND_LO))
