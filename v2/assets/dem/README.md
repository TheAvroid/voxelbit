# Terrain data

The `.vbdem` (elevation) and `.vbcov` (land cover) files are **not in git**. They
are derived, and they are large -- the 60 km Front Range window is 130 MB, which
is past GitHub's 100 MB per-file hard limit. They rebuild from source in minutes.

## What v2 expects by default

    front60.vbdem   60 km on -105.1565, 39.0219 -- Cheesman Lake AND Pikes Peak
    front60.vbcov   the land cover for it

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
