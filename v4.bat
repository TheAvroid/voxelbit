@echo off
REM ---------------------------------------------------------------------------
REM v4.bat -- run the v4 engine: an endless voxel pine forest on NVIDIA Falcor.
REM
REM v1 is the browser engine, launched by start.bat. v2 is the OptiX one. This
REM is the same wood again, path traced through Falcor with DXR inline ray
REM tracing -- one compute shader instead of a ray tracing pipeline.
REM
REM Double-click this, or run it from a prompt. Any arguments are passed
REM straight through to v4.exe:
REM
REM   v4.bat                              walk around in the wood
REM   v4.bat --seed 7 --density 0.8       a different, thicker wood
REM   v4.bat --view 8                     a smaller resident ring
REM   v4.bat --out shot.png --spp 256     render one frame offline and exit
REM   v4.bat --background                 open minimised, never take focus
REM   v4.bat --dlss dlaa                  denoise at full res, no upscaling
REM   v4.bat --no-dlss                    the plain accumulator, for comparison
REM   v4.bat --help                       every option
REM
REM controls:  W A S D walk, shift sprints, space jumps, F toggles fly.
REM            Y opens the SETTINGS MENU. Its last row bakes the current
REM            settings as the new defaults; v4\rebuild.bat then applies them.
REM            left click captures the mouse, right-drag looks, scroll zooms,
REM            P screenshots, F1 prints the controls,
REM            arrow keys scrub the day/night clock (up/down = fast),
REM            X + scroll wheel sets the cycle speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS,
REM            ESC releases the mouse -- ESC again quits.
REM
REM DLSS Ray Reconstruction is ON by default: the tracer draws one sample a
REM frame and the reconstruction carries the history, so walking looks like
REM standing still rather than like television static. If the driver or the card
REM cannot do it, v4 says so at startup and falls back to accumulating.
REM
REM The engine lives in this repo, so the path is relative to this file rather
REM than hardcoded. Override it by setting V4_HOME first.
REM ---------------------------------------------------------------------------
setlocal

if "%V4_HOME%"=="" set "V4_HOME=%~dp0v4"
set "EXE=%V4_HOME%\build\bin\Release\v4.exe"

if not exist "%EXE%" (
  echo v4: %EXE% not found.
  echo.
  echo     Build it first:  double-click %V4_HOME%\rebuild.bat
  echo     ^(needs Visual Studio with "Desktop development with C++", and a
  echo      Falcor checkout -- see %V4_HOME%\README.md^)
  echo.
  pause
  exit /b 1
)

REM Falcor loads its shaders from disk beside the exe rather than embedding
REM them, so v4's have to be there. Missing them is the one failure that would
REM otherwise surface as an unhelpful error deep in startup.
if not exist "%V4_HOME%\build\bin\Release\shaders\v4\shaders\Trace.cs.slang" (
  echo v4: the shaders are missing from build\bin\Release\shaders\v4.
  echo     Rebuild with %V4_HOME%\rebuild.bat
  echo.
  pause
  exit /b 1
)

REM The pine and decoration models. The engine defaults to these paths already;
REM passing them explicitly means a v4.exe built elsewhere still finds them.
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
if not exist "%PINES%\pine_1.vox" (
  echo v4: no pine_1.vox under %PINES% -- falling back to the engine default.
  set "PINES="
)

REM Nothing has to be added to PATH: Falcor.dll, slang and the D3D12 Agility
REM runtime all sit beside the exe, and the exe is launched by full path.

REM Run from THIS folder, not the build folder, so screenshots (v4_shot_NNN.png)
REM and any --out image land next to the launcher where they can be found.
if "%PINES%"=="" (
  "%EXE%" %*
) else (
  "%EXE%" --pines "%PINES%" --decor "%DECOR%" %*
)
set "RC=%ERRORLEVEL%"

REM Only hold the window open on failure -- a clean exit should just close.
if not "%RC%"=="0" (
  echo.
  echo v4 exited with code %RC%.
  pause
)
exit /b %RC%
