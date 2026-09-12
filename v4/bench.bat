@echo off
REM ---------------------------------------------------------------------------
REM bench.bat -- the store's cost, measured the same way every time.
REM
REM THE CAMERA IS PINNED AND THAT IS THE WHOLE POINT. This engine's spawn is a
REM command-line position, but an A/B taken from two different places in a wood
REM compares two views and not two stores -- a 54 % swing in trace time between
REM two spots in the same world is ordinary. Every number below is taken from
REM the SAME eye, the same direction and the same seed.
REM
REM Three views, because a voxel store's cost is not one number:
REM
REM   OPEN     looking across the wood from head height. Long grazing rays over
REM            the terrain shell -- the case that hands the shader the most
REM            candidate bricks per ray.
REM   CANOPY   inside a crown, looking up. Dense sparse foliage: the case tight
REM            brick bounds are supposed to fix.
REM   ABOVE    looking down from 40 m. Short rays, mostly first-hit, and the
REM            one that isolates the acceleration structure from the walk.
REM
REM Run it before and after a change and compare the `trace` column.
REM ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "EXE=%HERE%\build\bin\Release\v4.exe"
if not exist "%EXE%" ( echo bench: build it first. & exit /b 1 )

set "ASSETS=--pines %HERE%\..\game\assets\foilage\pine9 --decor %HERE%\..\game\assets\decoration"

REM AND THE SEED IS PINNED, WHICH IS LOAD-BEARING. The game rolls a new world
REM every launch; two runs of this file without this line would compare two
REM different woods, and the trace cost between two woods swings by more than
REM any change to the store has ever moved it. Same camera, same seed, or the
REM numbers mean nothing.
set "ASSETS=%ASSETS% --world-seed 20260911"

REM ...AND THE FIXED BOX, WHICH IS ALSO DELIBERATE. The game streams an endless
REM world: the window follows the player and rebuilds as it slides. That is the
REM right thing for playing and the wrong thing for measuring -- these three
REM cameras are pinned precisely so two runs photograph the SAME world, and a
REM world that rebuilds itself underneath them is not a benchmark.
set "ASSETS=%ASSETS% --no-stream"

REM --profile AND NOT --out, BECAUSE --out MEASURES THE WRONG THING.
REM
REM An offline render is dominated by shading: the three views below came out
REM within 3 Mpaths/s of each other on a store whose trace cost varies by far
REM more than that. --profile reports the TRACE PASS on its own, in
REM milliseconds, which is the number a change to the store actually moves.
REM
REM --background opens minimised and never takes focus; --shot-frame exits after
REM that many frames, so this is a headless benchmark and not a game you have to
REM close. 200 frames is past the point the timings settle.
set "COMMON=--background --profile --shot-frame 200 --width 1280 --height 720 --no-sound"
set "OUT=%TEMP%\v4_bench"
if not exist "%OUT%" mkdir "%OUT%"

echo === OPEN -- long grazing rays across the wood ===
"%EXE%" %ASSETS% %COMMON% --shot "%OUT%\open.png" --cam-x -6 --cam-z 34 --eye 2 --yaw 205 --pitch -3 | findstr /C:"store    " /C:"gpu    "
echo === CANOPY -- inside a crown, looking up ===
"%EXE%" %ASSETS% %COMMON% --shot "%OUT%\canopy.png" --cam-x 12 --cam-z -20 --eye 6 --yaw 60 --pitch 18 | findstr /C:"gpu    "
echo === ABOVE -- short rays, mostly first hit ===
"%EXE%" %ASSETS% %COMMON% --shot "%OUT%\above.png" --cam-x 0 --cam-z 0 --cam-y 45 --yaw 205 --pitch -35 | findstr /C:"gpu    "
exit /b 0
