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

## Could the data be better? Yes, and here is the number

3DEP publishes **1 m lidar** over both windows -- Acadia is 4 tiles of
`ME_MidCoast_2021_B21`, the Colorado window is 103 tiles at roughly 255 MB
each. At `--dem-scale 6` a 10.29 m posting is 1.7 world metres, or seventeen
voxels, so everything between postings is interpolation; at 1 m it would be
1.7 voxels and essentially every column would be measured.

What stops it being a drop-in:

* **Size.** The 50 km window at 1 m is 50,000^2 floats = **10 GB**. At 3 m it
  is 1.1 GB. Neither fits the current "load the whole grid" `DemField`.
* **Projection.** The 1 m tiles are UTM, not lon/lat, so `tools/dem2raw.cpp`
  would need a reprojection step it does not have today.

The shape that would work is a **high-resolution inset**: keep the 50 km window
at 10 m for the distance, and load a second `.vbdem` of 5 km at 1 m -- 100 MB --
around wherever the player actually is. Not implemented.

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
