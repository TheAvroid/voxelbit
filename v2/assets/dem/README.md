# Terrain data

The `.vbdem` (elevation) and `.vbcov` (land cover) files are **not in git**. They
are derived, and they are large -- the 60 km Front Range window is 130 MB, which
is past GitHub's 100 MB per-file hard limit. They rebuild from source in minutes.

## What v2 expects by default

    rmnp50.vbdem    50 km on Rocky Mountain National Park (Options::demPath)
    rmnp50.vbcov    the land cover for it

## THE COVER'S BLOCKS OVERLAP AND FADE, AND THAT MATTERS

`exportImage` caps a request at 2048 px and RESAMPLES per request, so the blocks
this fetches come back about two levels apart -- and the classifier turns two
levels into seventeen points of class change, correlated along the whole row.
Measured on the old rmnp50: **48.6% of the columns change class across row
4095/4096 against 18% at an ordinary row**, which in the game is a dead straight
shoreline running the width of the world.

Measured, fetching the same 512 m as two requests and as one:

    separate requests, across the join   39.5% of pixels change class
    ONE request, across the same join    22.5%   <- the natural variation
    a +2 DN shift on r, g, b              4.0%

So: the blocks are 2048 px but STEP 1856, every join has 192 px of ground both
blocks saw, and the pixels are cross-faded across it -- a straight
discontinuity becomes a gradient a kilometre wide, over ground that already
varies 22% row to row. Each block is also mapped onto one low-resolution
reference fetch of the whole window first, with a single `bytes.translate`
table, so the two sides start within a level of each other.

**A single coarse request was tried first and reverted**: it has no seam and
gives 24.4 m a sample, which at shrink 6 is four world metres of flat colour --
reported as "the data appears as squares that are visible".

**`front60`, `elbert40` and `cheesman30` were built with the old block code and
still carry those seams.** Rebuild them with the command above if you use one.

## THE DEM GETS THE LAST WORD ON WATER, BOTH WAYS

`naip2cov.py` runs two passes over the finished classification, and between
them they are the answer to "detect and fix the terrain artifacts". Neither is
a heuristic on colour; both ask the elevation a question the photograph cannot
answer.

**USGS hydro-flattens water.** Every posting inside a mapped water body is
written at one elevation, to the bit -- which is a thing that does not
otherwise happen in a measured surface, where even a car park wanders by
centimetres. So:

* **Flat means water.** Flood the DEM for connected regions of exactly equal
  float, keep the ones of 60 postings or more, and mark them water whatever the
  imagery said. On rmnp50: **225 bodies, 323,846 samples, 3,429 ha**, of which
  **136,367 samples** had been called something else. The largest is Lake
  Granby at 178,647 samples, and the photograph had only 75.8% of it.
* **Not flat means not water.** Flood the *water mask* into connected bodies
  and drop any body that is not anchored to hydro-flattened ground. On rmnp50:
  **336 bodies kept, 20,225 dropped (78,968 samples, 17.7% of all the water)**.
  Those were cloud shadow, wet talus and above all snow in shadow, which sits
  where deep water sits in colour space. Found by standing at 4,103 m asl --
  above every lake in Colorado -- in an ocean with waves on it.

Two things worth knowing if you touch this.

**The drain test is about DEPTH, not count.** The first cut refused any flat
region with a lower neighbour, on the reasoning that water sits in a hollow,
and it threw away every lake in the window. A reservoir's drawdown zone is
exposed lake bed, so 43% of Granby's 38,468 rim cells read *below* full pool --
by a centimetre or two. Measured across all 225 regions the worst drop on any
rim is under 5 cm, while a bench cut into a slope falls metres within one
posting. So the test is a drop threshold, and on this window it rejects nothing
at all, which is the point.

**The anchor test is anchoring, not slope.** A slope threshold eats the shore
of every real lake, because a rim cell's central difference straddles the bank.
Anchoring separates cleanly instead: the 336 kept bodies are 70-97% flat cells
each, the 20,225 dropped ones are *zero*, at a median of four samples a body.
Nothing was a close call.

## TERRACES ARE A QUANTISING ARTEFACT, SO THE QUANTISING IS DITHERED

Stepped contour rings on bare ground are not the data and not the
interpolation. Quantising any smooth ramp onto 0.1 m voxels steps a whole voxel
at a time, so a constant grade gives treads of constant width lying along the
contours: the snowfield at world (1050, 1275) is a 1.83% grade, which is a step
every 5.5 world metres, and it photographed as concentric rings.

**Smoothstep in `DemField::heightM` was tried and reverted.** Making the cell
joins C1 is a real improvement to the surface and does nothing at all to this --
built, rendered, rings unchanged -- because the quantising happens afterwards
either way. It is also slightly worse where it matters, since zero slope at
each posting puts the widest tread of all right on the sample.

`VoxelWorld::heightM` dithers instead, which is the standard answer to banding:
half a voxel of hash per column, gated on the grade so level ground and steep
ground both get nothing. Amplitude is **0.55 voxels peak to peak** and that
number was set by eye -- at a full voxel every column can flip and the slope
becomes speckle; at 0.55 only a column already near a voxel boundary moves, so
the tread stays flat and its edge frays. Every column still lands within half a
voxel of the measured surface, which is strictly closer than the terracing it
replaces.

## Could the data be better? Yes -- and the projection blocker is now gone

3DEP publishes **1 m lidar-derived bare earth** over both windows. At
`--dem-scale 6` a 10.29 m posting is 1.7 world metres, or seventeen voxels, so
everything between postings is interpolation; at **`--dem-scale 1`, which is
what Acadia and Ouachita both run, it is 103 voxels**. That is the number worth
keeping in mind: the two newest worlds are the most interpolated ones there are.

    posting     voxel columns invented between two measurements, at scale 1
    10.29 m     103
     1 m         10
     0.35 m       3.5   (QL1 lidar's own point spacing -- not a raster)
     0.1 m        1     (nothing with coverage reaches this)

**`tools/dem2raw.cpp` now reads the 1 m product.** Two things had to change and
both are done:

* **Projection.** The 1 m tiles are UTM in metres; the 1/3" tiles are geographic
  in degrees. A tile now carries its own CRS, read from the GeoTIFF key
  directory, and every lookup converts lon/lat into that tile's space first.
  Geographic and UTM are handled; anything else is refused by name.
* **Naming.** 1 m tiles are named for the project that flew them
  (`USGS_one_meter_x46y382_AR_Ouachita_B5_2016.tif`), not on a clean degree
  grid, so the directory is INDEXED BY WHAT EACH FILE SAYS IT COVERS rather
  than by a name. `--index` forces that; it is also chosen automatically when
  the name-derived tile for the window centre does not exist.

**`--step-m` sets the output posting.** Without it the tool is byte-for-byte
what it was -- verified by rebuilding `ouachita12.vbdem` and comparing.

    tools/dem2raw.exe --dir C:/geo/dem/ouachita/1m \
                      --center -93.3000 34.6600 --km 12 --step-m 1 \
                      --out assets/dem/ouachita12_1m.vbdem

### It was checked against the product it replaces, not just run

A wrong zone, a transposed tie point or a datum muddle all produce a `.vbdem`
that opens cleanly, has the right extent in its header, and is full of
plausible elevations. Nothing downstream can tell. Two checks:

* `tools/dem2raw.exe --utm-test` -- the easting on a central meridian is exactly
  500000 by construction, a round trip over CONUS zones 10-19 is under a
  millimetre, and Mount Elbert lands on its published grid reference.
* `python tools/demcmp.py a.vbdem b.vbdem` -- asks a SECOND, independently
  georeferenced product what the ground is doing at the same place. Over a 2 km
  window at Ouachita, 10.29 m against 1 m:

      mean   +0.00 m     <- no datum or tie-point offset
      rms     0.23 m     <- resolution, which is what it should be
      worst  +-2.2 m     <- on the steepest ground, where a 10 m cell averages

  A mean over a couple of metres means the datum. An rms in the tens of metres
  means the projection, not the resolution.

### Two ways a 1 m window looks empty when the data is right there

Both cost a rebuild here, and both present identically: a clean band of holes
and a bare total like `holes 52522508 samples with no data`. **`dem2raw` now
prints a per-tile sample count and the window's own lon/lat bounds**, which
turns each of these into something you can read off in one line.

**1. A TILE IS NAMED FOR ITS NORTH-WEST CORNER** -- the same convention as the
1/3" tiles, and easy to assume otherwise because the name looks like a grid
index:

    x46y385  ->  easting  459994..470006    (x names the WEST edge)
                 northing 3839994..3850006  (y names the NORTH edge)

Read `y` as the south edge and you fetch a row of tiles one step too far south.
The window loses its whole top -- 36.5% of it, and the relief comes out 116 m
instead of 339 m, which reads as a flat world rather than a missing one. The
give-away in the new report is a tile with **0 samples**:

    1m\USGS_one_meter_x46y382_AR_Ouachita_B5_2016.tif         0 samples
    1m\USGS_one_meter_x47y384_AR_Ouachita_B5_2016.tif  85213709 samples

**2. A TILE THAT COVERS A POINT IS NOT A TILE THAT HAS IT.** The 1 m product is
published per ACQUISITION LOT, and a lot's tiles are full rectangles with
nodata wherever that lot did not fly. So two lots publish tiles at the SAME
grid reference, each holding part of the ground: `AR_Ouachita_B5_2016` and
`_B6_2016` both have `x47y384`, B5's footprint stopping around northing
3837000 and B6's carrying on north. Fetch only B5 and a 2.7 km band across the
middle of the window is empty -- 25% of it.

`Mosaic::sample` therefore **falls through nodata to the next covering tile**,
so overlapping lots merge with no ordering rule and no preference, and only a
point that *every* covering tile calls nodata is a hole.

**Pick one campaign if you can.** `AR_Eastern_D23` also publishes `y385` over
this window and was not used: mixing acquisitions puts a point-density and date
seam through the middle of the world, which is the one cost of 1 m data that
the seamless 1/3" product does not have.

### What it costs

    window        posting    samples      .vbdem
    12 km         10.29 m    1166^2         5 MB
    12 km          1 m      12000^2       576 MB
    50 km          1 m      50000^2        10 GB   <- does not fit DemField

**The scale-1 worlds are the small ones, which is the whole reason this is
practical.** Acadia at 10 km and Ouachita at 12 km are 400 MB and 576 MB, which
v2 can hold beside its usual 6-9 GB. rmnp50 at 1 m is 10 GB and also needs it
least, since at shrink 6 its posting is already seventeen voxels. The
high-resolution inset idea in the note this section replaces is still the answer
for a 50 km window; it is not needed for a 12 km one.

**The cover does NOT have to match.** `CoverField` indexes by world position
through its own header, so a 1 m `.vbdem` pairs with the existing 10.29 m
`.vbcov`. They are built on the same grid by default and nothing requires it.

## THE WATERLINE IS A SIGNED CONTOUR, NOT A RASTER EDGE

A shoreline that steps in rectangles is not the class mask being read
nearest-neighbour -- `waterHere` has read an interpolated plane for a while.
It is the plane being **one-sided**. `naip2cov.py` bakes distance-to-shore as
zero on every land cell and metres offshore in the water, so across the bank it
reads 0.00 and then 35 to 90 m one cell later. Asking for the `kShoreEdgeM`
contour of that picks a level one part in fifty up a cliff, which pins the
waterline to the raster boundary -- and at a 10.29 m posting and `--dem-scale
6` that boundary is a staircase with **17-voxel treads**.

`CoverField::buildShoreField` runs the chamfer transform **both ways** at load
and stores the difference, biased by 128: negative on land, positive in water,
zero at the line. The contour is then a real crossing between two cells and can
sit anywhere inside one. Measured on Lake Granby, longest straight run of
waterline:

| shore | before | after |
| --- | --- | --- |
| south | 17.6 m | 3.0 m |
| east | 3.9 m | 1.7 m |

It is built at load rather than baked on purpose: the engine already owns the
transform (`despeckleWater` has to re-run it after it cleans the mask), it
costs **132 ms** on the 4,859^2 window, and it keeps all five baked `.vbcov`
files valid. Bumping the format would have meant a NAIP refetch for each.

On top of that the lookup position is **warped** by about 0.7 of a cell of
value noise, faded out within two cells of the line. Signing removes the steps
but leaves the raster's corners rounded over one cell, which from above still
reads as a grid; the warp decides which way the edge wanders, which is the one
thing about a coastline that does not have to be measured to be right. The
fade matters -- the same field is the bed's depth ramp, so warping it in open
water would put lumps across the bottom.

## Rebuilding

Needs the USGS 3DEP tiles at `C:/geo/dem/colorado/13arcsec_10m` (1/3 arc-second,
28 one-degree tiles covering the state; the download URL is in dem2raw.cpp).

    cd v2
    g++ -O2 -std=c++17 -o tools/dem2raw.exe tools/dem2raw.cpp -lz
    tools/dem2raw.exe --center -105.1565 39.0219 --km 60 \
                      --out assets/dem/front60.vbdem
    python tools/naip2cov.py assets/dem/front60.vbdem assets/dem/front60.vbcov

The DEM takes about five seconds. The cover pulls USGS NAIP aerial imagery over
the same window and classifies it, which takes about four minutes for 34 M
samples and needs no credentials.

## If they are missing

v2 prints `[dem] FAILED to load ...` and falls back to the procedural landform,
which still works -- `--no-dem` selects it deliberately. Nothing crashes.

## Other windows

    --center -106.4453 39.1178 --km 40    Mount Elbert, the Sawatch
    --center -105.2706 39.2028 --km 30    Cheesman Lake alone

Point v2 at one with `--dem <file> --cover <file>`.

## Acadia National Park -- `--acadia`

    acadia10.vbdem   10 km on -68.2700, 44.3550 -- Mount Desert Island's ridge:
                     Cadillac (465.6 m), Sargent, Penobscot, Eagle Lake and
                     Jordan Pond, and 7.8% open sea at the edges
    acadia10.vbcov   used for the WATER ONLY -- see below

Its tile is a different state, so it is a different download:

    mkdir -p C:/geo/dem/acadia/13arcsec_10m
    curl -L -o C:/geo/dem/acadia/13arcsec_10m/USGS_13_n45w069.tif       https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/current/n45w069/USGS_13_n45w069.tif
    tools/dem2raw.exe --dir C:/geo/dem/acadia/13arcsec_10m --prefix USGS_13                       --center -68.2700 44.3550 --km 10 --out assets/dem/acadia10.vbdem
    python tools/naip2cov.py assets/dem/acadia10.vbdem assets/dem/acadia10.vbcov

One tile covers the whole park: USGS names a tile by its NORTHWEST corner, so
`n45w069` is lat 43.998..45.002, lon -69.002..-67.998.

`--acadia` runs it at **true scale, birch-pinned, with the imagery believed
about the water and not about the ground** (`--cover-water`). The reasoning for
each is written out over the flag in `main.cpp`; the measurements behind it:

  * the stand table floors at 120 stems/ha below 1800 m, and dividing that by a
    shrink of 6 would thin the island to a seventh of a wood -- so scale 1;
  * the classifier is tuned on Colorado and calls **41.9% of the island bare
    rock at a median 64 m**, where the granite domes are all above 250 m.
    Rendered with it trusted: 2,867 trees on a lavender waste, against 9,162
    with it ignored;
  * its WATER is right, though -- 13.0% of the window, half under 3 m (the
    sea), the rest clustered at 83 m where Eagle Lake and Jordan Pond are, mean
    grade 5%. And on a DEM world the imagery is the ONLY source of water
    (`waterAt` returns kNoWater), so dropping the cover entirely costs an island
    its coast and every water animal with it.

`--cover-water` works on any window whose ground the classifier gets wrong:

    v2.exe --dem <file> --cover <file> --cover-water

and `--acadia --cover-ground` puts the classifier's ground back, which is what
the 2,867-tree render above was. Last flag wins.

## Lake Ouachita, Arkansas -- `--ouachita`

    ouachita12.vbdem  12 km on -93.3000, 34.6600 -- the Ouachita National
                      Forest north of Lake Ouachita: 339.1 m of relief
                      (173.6 .. 512.7 m asl), no holes
    ouachita12.vbcov  79.1% forest, 14.3% water, 3.6% rock, 2.9% meadow

The oak wood's own ground, the way `--acadia` is the birch's. One tile:

    mkdir -p C:/geo/dem/ouachita/13arcsec_10m
    curl -L -o C:/geo/dem/ouachita/13arcsec_10m/USGS_13_n35w094.tif https://prd-tnm.s3.amazonaws.com/StagedProducts/Elevation/13/TIFF/current/n35w094/USGS_13_n35w094.tif
    tools/dem2raw.exe --dir C:/geo/dem/ouachita/13arcsec_10m --prefix USGS_13 --center -93.3000 34.6600 --km 12 --out assets/dem/ouachita12.vbdem
    python tools/naip2cov.py assets/dem/ouachita12.vbdem assets/dem/ouachita12.vbcov

`n35w094` is named for its NORTHWEST corner, so it covers lat 33.998..35.002,
lon -94.002..-92.998.

It runs at **true scale, oak-pinned, with the imagery believed about the ground
as well as the water**. Scale 1 for the same reason Acadia takes it -- the
stand table floors at 120 stems/ha below 1800 m and this window tops out at
513 m, so a shrink of 6 would thin the wood to a seventh for nothing.

**`coverGround` stays ON here, unlike Acadia.** That flag is off on Mount
Desert Island because the Colorado-tuned classifier calls 41.9% of it bare
rock. This window is 3.6% rock, and the forest fraction going up the ridges is
77.0%, 98.4%, 99.4%, 99.5% -- the classifier has not smeared rock over the high
ground, so there is nothing to protect the terrain from.

### The same window at 1 m -- `ouachita12_1m.vbdem`

    bash C:/geo/dem/ouachita/1m/fetch.sh          # 8 tiles, ~2.9 GB
    tools/dem2raw.exe --dir C:/geo/dem/ouachita/1m \
                      --center -93.3000 34.6600 --km 12 --step-m 1 \
                      --out assets/dem/ouachita12_1m.vbdem

    12000 x 12000 samples, 549 MB, 173.6..513.0 m, relief 339.4 m, ZERO holes

Against the 10.29 m product over the whole window (`tools/demcmp.py`), 338,724
samples compared:

    mean   +0.00 m    rms 0.15 m    p05/p95 -0.20/+0.22    worst +-2.9 m

which is resolution and nothing else -- no datum offset, no projection error.
It is AR_Ouachita_B5_2016 and _B6_2016 merged; read the coverage traps above
before refetching.

**It is not what `--ouachita` loads.** That flag still points at the 10.29 m
`.vbdem`, because switching it would take the world from 5 MB of terrain to
549 MB without anyone asking. To stand in it:

    v2.exe --ouachita --dem assets/dem/ouachita12_1m.vbdem

The existing 10.29 m `ouachita12.vbcov` pairs with it unchanged -- `CoverField`
indexes by world position through its own header, so the two grids do not have
to match.

**And the roughness follows it down on its own.** `--dem-rough 1` measured over
the same window, as relief added in world metres:

    posting    rock     forest   meadow
    10.29 m    0.1488   0.0769   0.0251
     1 m       0.0263   0.0134   0.0044     <- 17.7% of the above

The design factor is `(1/10.29)^0.75` = 17.4%. Nothing was retuned to get that;
it is what the measurement leaving less unknown looks like.

### Why not the Ozarks, which is the purer oak

The first cut was the upper Buffalo River in the Boston Mountains -- 12 km on
-93.4000, 36.1000, off tile `n37w094`. On paper it wins:

| | relief | forest | water |
| --- | --- | --- | --- |
| Buffalo River | **446.4 m** | 77.7% | 0.01% (1 ha) |
| Lake Ouachita | 339.1 m | **79.1%** | **14.3%** |

It was built, and it has no water at all: one flat body, one hectare. The
Boston Mountains are an upland with no lakes, and the Buffalo is a free-flowing
river too narrow for a 10.29 m posting to hold. On a DEM world the imagery is
the only thing that makes water (`waterAt` returns `kNoWater` once a DEM is
loaded), so that window is a wood with no fish, ducks, lily pads, dragonflies
or frogs.

The species difference decides nothing, which is what settles it: the cover
only ever says forest, meadow or rock, and the trees planted in it are this
engine's own oak models. Oak-hickory versus oak-hickory-pine changes nothing
that gets drawn. So the trade is 107 m of relief against the whole water half
of the wood -- and 339 m at scale 1 is still more world relief than rmnp50 has
after its shrink of 6.

Table Rock Lake was measured too, at -93.4500 36.5800: a lake, but only 134.9 m
of relief.

### What the bed test says about it

`lakebed_test` reports FAIL on this window, at 12.53% of water on ground
steeper than 2% and 52,771 columns standing above the lake surface. Both are
the shore false positive that note in `naip2cov.py` describes: Lake Ouachita is
a drowned dendritic valley whose shoreline runs up every hollow, so an enormous
share of its water is within one posting of a steep bank, and a central
difference there reads the bank. The "islands breaking the surface" are in
large part real -- the lake has about two hundred of them.

rmnp50 passes the same test at 5.32% and Acadia fails it at 28.17%, so read
this number as a shoreline-convolution measure, not a verdict.

## Imagery from a directory instead of the service -- `--imagery-dir`

`naip2cov.py` can read a directory of georeferenced tiles and skip the whole
block/fade path above:

    python tools/naip2cov.py <in.vbdem> <out.vbcov> --imagery-dir <dir>

**Everything the fade machinery exists to hide is absent by construction.**
There is no join, because nothing was requested in pieces; no exposure drift,
because nothing was resampled; and no 2048 px cap, so the resolution is the
supplier's. The seam essay above stays because the service path stays.

**The supersample is deliberately 2x2 and not the whole cell.** At 15 cm imagery
and a 1 m posting there are 44 source pixels inside one cell. Averaging all 44
would BLUR exactly the boundaries this tool exists to find -- a shoreline is by
definition where neighbouring pixels disagree -- so four samples at the quarter
points is the compromise: a real use of the resolution, still four reads.

**Where 15 cm imagery comes from, free:**

    Maine       MEGIS GeoLibrary wide-area ortho, statewide 15 cm
    Arkansas    ADOP 6-inch statewide, 4-band RGB+NIR; 3-inch in some counties

**NOT VERIFIED AGAINST A LIVE BULK ENDPOINT.** The Arkansas bucket named in
public write-ups (`geostor-imagery`) returns NoSuchBucket, and the state's
download page now redirects to `geodata.gis.arkansas.gov`. The reader and its
test are done; the fetch for a given supplier is not, and the tiles must be
8-bit and either geographic or UTM. **State Plane is common in supplier
deliveries and is refused by name** rather than guessed at -- reproject first.

`tools/geoimg.py` is the reader; `tools/geoimg_test.py` proves it puts pixels in
the right place, by painting tiles whose content is a known function of POSITION
(a water band on a known easting, forest north of a known northing), reading
them back at independently computed lon/lat, and checking the right ground came
out. It covers raw, deflate and deflate+predictor, 3-band and 4-band, and checks
that JPEG-in-TIFF and State Plane are refused with a message that names the
problem.

## Roughness that knows what it is standing on -- `--dem-rough`

    v2.exe --ouachita --dem-rough 1

**THE PROBLEM IT SOLVES IS NOT TERRACING.** `terraceBreakM` deals with that. A
posting is the distance between two things that were MEASURED, and at scale 1
that leaves 103 voxel columns per measurement as a curve somebody chose. No
source with coverage reaches 0.1 m -- the best lidar in the country is ~0.35 m
between returns -- so the bottom of the scale is either synthesised or it is a
smooth ramp. It is currently a smooth ramp.

**AND IT IS NOT THE NOISE THAT WAS REMOVED.** `--dem-detail` is one amplitude
over the whole world and is off because of "remove that noise from all terrain":
the same speckle on a talus slope and on a flat sand bank. Three rules make this
a different thing rather than a quieter version of the same one:

1. **Per class.** Rock is rough, meadow is nearly flat, **water is exactly
   zero**, and the amplitude fades to nothing within 8 m of a waterline --
   the beach is where the complaint came from.
2. **It shrinks as the data improves.** Relief below wavelength L goes as L^H
   with H about 0.75, so the table is quoted at the 10.29 m posting and scaled
   by `(posting/10.29)^0.75`. **A 1 m source invents 17% of what a 10.29 m one
   does, with no constant retuned.**
3. **Real metres, divided by the shrink**, so `--dem-scale` changes what it
   means on the ground and not how big it looks.

Measured over Ouachita at scale 1, `--dem-rough 1`, as relief added in world
metres (`build/dem_rough_test.exe`):

    rock     0.1488 rms     forest  0.0769     meadow 0.0251
    snow     0.0160         water   0.0000  (exactly)

    distance from the waterline   0-2 m  0.0015   <- 2.9% of the plateau
                                  2-4 m  0.0084
                                  4-8 m  0.0357
                                 8-16 m  0.0556
                                16-64 m  0.0508   <- the plateau

**IT IS OFF BY DEFAULT** (`--dem-rough 0`), because every world so far was tuned
without it and none of them should move under anyone. The test's first claim is
that off is off, over 176,400 columns, bit-identical.

### The bug this test caught on its first run

`CoverField::shoreDistance` is **signed** -- negative on land, positive in
water, zero at the line. Read as a distance it makes every land column -127,
and through a `t*t` fade that is a gain of **252**, not a fade to nothing:
35.96 m rms on rock against an expected 0.15, and 100 m of invented relief on a
hillside. A screenshot would have shown "the terrain looks wrong" with no way to
say why. Anything reading that field wants its MAGNITUDE.

### What would make the table honest

The amplitudes are terrain intuition, not a fit -- there is no high-resolution
exemplar in the tree to fit against. The honest version is one patch of real
3-5 cm ground per class (UAV SfM, from OpenTopography's community datasets) with
its roughness spectrum measured and the table set from it. That is the one job
UAV photogrammetry is actually right for here: as a statistical exemplar, not as
coverage -- it cannot see through a canopy and it fails on water, and every
world here is forest and lake.
