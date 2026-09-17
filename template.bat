@echo off
REM ---------------------------------------------------------------------------
REM template.bat -- run the template engine: the lighting, with nothing in it.
REM
REM This is a path tracer with ReSTIR, SHaRC, a neural radiance cache, DDGI,
REM volumetric fog, clouds, a physical atmosphere, post and DLSS Ray
REM Reconstruction -- and NO GEOMETRY AT ALL. Every ray misses. What you see is
REM the sky, the sun, the moon, the clouds and the fog, lit and denoised by the
REM full stack, with nothing standing in front of them.
REM
REM THAT IS NOT A BROKEN BUILD. It is the point. This engine exists so that
REM different ways of storing and tracing voxels can be tried against an
REM IDENTICAL renderer, and so that each one can be measured against a zero --
REM what a frame costs with the geometry query removed entirely.
REM
REM ---------------------------------------------------------------------------
REM WHERE A VOXEL STORE PLUGS IN. Two files, and no edits anywhere else:
REM
REM   template\shaders\stores\Null.slang   storeTrace() and storeOccluded().
REM                                        Copy it, implement the two, declare
REM                                        whatever buffers it needs at the top.
REM   template\src\gpu\store.h             build / shutdown / update / bind /
REM                                        addDefines. bind() puts EVERYTHING
REM                                        on one root var -- resources AND
REM                                        constants -- which is what keeps a
REM                                        new backend out of the tracer.
REM
REM Point TPL_STORE_IMPL at the new .slang from addDefines and the whole engine
REM -- camera pass, SHaRC update pass, DDGI probes and the fog's own shadow rays
REM -- is specialised on it.
REM
REM THE SECOND INTERFACE IS THE ONE THAT GETS FORGOTTEN. A shader-side store
REM answers rays; walking, collision, physics and any host-side pick need
REM solidAt / topAt / groundM / collidersNear on the CPU as well. There is no
REM walker here for exactly that reason -- the camera flies. See the note in
REM src\gpu\world.h before planning a backend.
REM
REM ---------------------------------------------------------------------------
REM Double-click this, or run it from a prompt. Arguments pass straight through:
REM
REM   template.bat                        fly around an empty sky
REM   template.bat --vulkan --nrc         neural shading, with the radiance cache
REM   template.bat --out shot.png --spp 64
REM                                       render one frame offline and exit
REM   template.bat --background           open minimised, never take focus
REM   template.bat --profile              per-pass GPU timings -- the zero every
REM                                       backend is measured against
REM   template.bat --help                 every option
REM
REM controls:  W A S D fly, shift is faster, space up, ctrl down.
REM            Y opens the settings menu, left click captures the mouse,
REM            right-drag looks, scroll zooms, P screenshots,
REM            arrow keys scrub the day/night clock, ESC releases the mouse --
REM            ESC again quits.
REM
REM EVERYTHING FAILS SOFT. Every optional piece asks the device whether it can
REM run before it runs, and any that cannot is skipped with a line at startup
REM saying why.
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
REM exactly what the next line does. TPL_CONSOLE=1 stops the recursion; set it
REM beforehand to keep a visible window.
if "%TPL_CONSOLE%"=="" (
  set "TPL_CONSOLE=1"
  start "template" /min cmd /c ""%~f0" %*"
  exit /b 0
)

if "%TPL_HOME%"=="" set "TPL_HOME=%~dp0template"
set "EXE=%TPL_HOME%\build\bin\Release\template.exe"

if not exist "%EXE%" (
  echo template: %EXE% not found.
  echo.
  echo     Build it first:  %TPL_HOME%\build.bat
  echo     Type-check without building: %TPL_HOME%\check.bat
  echo.
  echo     Dependencies are NOT inside this tree. They are read by path from
  echo     C:\voxelbit\v3\external -- override with DEPS before building.
  echo.
  pause
  exit /b 1
)

REM Falcor loads shaders from disk beside the exe rather than embedding them, so
REM the template's have to be there. Missing them is the one failure that would
REM otherwise surface as an unhelpful error deep in startup.
if not exist "%TPL_HOME%\build\bin\Release\shaders\template\shaders\Trace.cs.slang" (
  echo template: the shaders are missing from build\bin\Release\shaders\template.
  echo     Rebuild with %TPL_HOME%\build.bat
  echo.
  pause
  exit /b 1
)

REM NO ASSET PATHS. There are no models to load and no world to scatter them
REM into. A backend that authors content passes its own.

REM Run from THIS folder, not the build folder, so screenshots and any --out
REM image land next to the launcher where they can be found.
"%EXE%" %*
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo.
  echo template exited with code %RC%.
  pause
)
exit /b %RC%
