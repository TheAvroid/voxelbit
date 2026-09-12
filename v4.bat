@echo off
REM ---------------------------------------------------------------------------
REM v4.bat -- run the v4 engine: voxels as HARDWARE-ACCELERATED DXR PROCEDURAL
REM AABBs, under the template engine's lighting stack, unchanged.
REM
REM WHAT IS DIFFERENT ABOUT THIS ENGINE.
REM
REM v3 stores the world as an OpenVDB tree, flattens it to NanoVDB and MARCHES
REM it in software: the shader walks the tree itself, node by node, and the ray
REM tracing cores are not involved in finding the voxel at all.
REM
REM v4 gives the traversal to the hardware. Every 8x8x8 brick of voxels is one
REM AABB in a bottom-level acceleration structure; a 25.6 m chunk is one
REM structure; the whole world is one top-level structure of them. A ray is an
REM inline RayQuery, and the BVH descent that finds which bricks it crosses runs
REM in the RT cores. The shader is handed one 0.8 m box at a time and does the
REM only part silicon cannot do -- a twenty-two step DDA over a 512-bit
REM occupancy mask -- and commits the hit.
REM
REM Neither half does the other's job. That is the whole point of the design,
REM and it is what makes this comparable against v3 on an IDENTICAL renderer:
REM same ReSTIR, same SHaRC, same neural cache, same DDGI, same fog, clouds,
REM atmosphere, post and DLSS Ray Reconstruction. Only the store is new.
REM
REM ---------------------------------------------------------------------------
REM THE FIRST FRAME TAKES A FEW SECONDS AND THAT IS THE WORLD BEING MADE.
REM
REM The default 192 m square is about 93 million voxels. Generating it takes
REM roughly three seconds, packing it a fifth of one, and the structure build a
REM moment more; the startup line says how long each part took. Pass
REM "--world 96" for a quarter of the ground and a quarter of the wait.
REM
REM ---------------------------------------------------------------------------
REM Double-click this, or run it from a prompt. Arguments pass straight through:
REM
REM   v4.bat                          fly over the wood
REM   v4.bat --demo                   the acceptance scene: a floor, a stepped
REM                                   wall across a chunk seam, an ARCH and a
REM                                   pillar with a cave through it. The arch is
REM                                   the test -- no heightfield can hold one
REM   v4.bat --store-debug 1          paint every brick its own colour, which is
REM                                   the acceleration structure made visible
REM   v4.bat --store-debug 2          paint how many candidate bricks each ray
REM                                   was handed -- the number the design exists
REM                                   to keep small
REM   v4.bat --no-models              the generated wood instead of the authored
REM                                   one -- the A/B the assets are worth
REM   v4.bat --no-grass               no strands. Counter-intuitive: HAVING
REM                                   grass measured faster than not, because
REM                                   blades stop long grazing rays a few
REM                                   metres out instead of letting them run
REM                                   to the horizon
REM   v4.bat --water 4.5              pour a lake to that line
REM   v4.bat --world 320 --skin 4     a bigger square, stored deeper
REM   v4.bat --out shot.png --spp 64  render one frame offline and exit
REM   v4.bat --background             open minimised, never take focus
REM   v4.bat --profile                per-pass GPU timings
REM   v4.bat --help                   every option
REM
REM controls:  W A S D fly, shift is faster, space up, ctrl down.
REM            Y opens the settings menu, left click captures the mouse,
REM            right-drag looks, scroll zooms, P screenshots,
REM            arrow keys scrub the day/night clock, ESC releases the mouse --
REM            ESC again quits.
REM
REM EVERYTHING FAILS SOFT. Every optional piece asks the device whether it can
REM run before it runs, and any that cannot is skipped with a line at startup
REM saying why. The store is not optional and does not gate the render: a world
REM with nothing in it simply misses every ray and you get the sky.
REM
REM KEEP THIS FILE ASCII AND LF. cmd re-seeks a batch file by BYTE offset
REM between commands, so a non-ASCII character anywhere above desynchronises the
REM parser and it starts executing fragments of these comments as commands.
REM ---------------------------------------------------------------------------
setlocal

REM KEEP THE CONSOLE OUT OF THE WAY. Double-clicking a .bat ALWAYS creates a
REM console -- Windows makes the window before cmd reads a line of this file --
REM so it can only be RELOCATED, never prevented. This relaunches once,
REM minimised, and lets the original window exit. START and not a bare call: a
REM child that inherits this console is killed when the console closes, which is
REM exactly what the next line does. V4_CONSOLE=1 stops the recursion; set it
REM beforehand to keep a visible window.
if "%V4_CONSOLE%"=="" (
  set "V4_CONSOLE=1"
  start "v4" /min cmd /c ""%~f0" %*"
  exit /b 0
)

if "%V4_HOME%"=="" set "V4_HOME=%~dp0v4"
set "EXE=%V4_HOME%\build\bin\Release\v4.exe"

if not exist "%EXE%" (
  echo v4: %EXE% not found.
  echo.
  echo     Build it first:  %V4_HOME%\build.bat
  echo     Type-check without building: %V4_HOME%\check.bat
  echo     Check the STORE without a GPU at all:
  echo         g++ -O2 -std=c++17 -I src tests\brick_test.cpp -o brick_test
  echo.
  echo     Dependencies are NOT inside this tree. They are read by path from
  echo     C:\voxelbit\v3\external -- override with DEPS before building.
  echo.
  pause
  exit /b 1
)

REM Falcor loads shaders from disk beside the exe rather than embedding them, so
REM v4's have to be there. Missing them is the one failure that would otherwise
REM surface as an unhelpful error deep in startup.
if not exist "%V4_HOME%\build\bin\Release\shaders\v4\shaders\stores\Aabb.slang" (
  echo v4: the shaders are missing from build\bin\Release\shaders\v4.
  echo     Rebuild with %V4_HOME%\build.bat
  echo.
  pause
  exit /b 1
)

REM ---------------------------------------------------------------------------
REM THE ASSETS. The SAME files v2 and v3 draw, out of the same folder, so a seed
REM means the same wood in every engine:
REM
REM   foilage\pine9      pine_1..9.vox -- the nine authored conifers
REM   decoration          rock.vox, mushroom.vox, flowers.vox, and the 26
REM                       boulders under decoration\rocks in three sizes
REM
REM The palette is built FROM these. The ground's greens and browns are read off
REM the pines' own needles and bark, and the stone the terrain is made of is
REM read off the boulders' own greys -- so the floor is made of the same colours
REM as the things standing on it. That is why the models load before a single
REM voxel of terrain is written.
REM
REM A MISSING FOLDER IS NOT FATAL. Without them the engine generates a plainer
REM conifer and a noise-dented boulder, says "generated" on the models line at
REM startup, and renders. --no-models forces that path, which is the honest A/B
REM of authored content against generated.
REM ---------------------------------------------------------------------------
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
REM mineral.vox is not in the decoration folder -- it is still in the source
REM tree where it was authored. Six ores, seamed through the stone in three
REM depth bands: coal and iron near the top, emerald and ruby in the middle,
REM gold and diamond at the bottom. Almost all of it is sealed in rock and
REM costs the device nothing; what shows is the seams that meet open air.
set "MINERALS=%~dp0source\wip\foilage\mineral.vox"
if not exist "%MINERALS%" set "MINERALS="
if not exist "%PINES%\pine_1.vox" (
  echo v4: no pine_1.vox under %PINES% -- generating the wood instead.
  set "PINES="
)

REM Run from THIS folder, not the build folder, so screenshots and any --out
REM image land next to the launcher where they can be found.
REM
REM ANYTHING YOU PASS OVERRIDES THESE, because your arguments come last and the
REM parser takes the last value it sees.
if "%PINES%"=="" (
  "%EXE%" %*
) else (
  if "%MINERALS%"=="" (
    "%EXE%" --pines "%PINES%" --decor "%DECOR%" %*
  ) else (
    "%EXE%" --pines "%PINES%" --decor "%DECOR%" --minerals "%MINERALS%" %*
  )
)
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo.
  echo v4 exited with code %RC%.
  pause
)
exit /b %RC%
