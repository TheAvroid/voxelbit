@echo off
REM ---------------------------------------------------------------------------
REM v2.bat -- run the v2 engine: an endless voxel pine forest on NVIDIA OptiX.
REM
REM v1 is the browser engine, launched by start.bat. This is the native one.
REM
REM Double-click this, or run it from a prompt. Any arguments are passed
REM straight through to v2.exe:
REM
REM   v2.bat                              walk around in the wood
REM   v2.bat --seed 7 --density 0.8       a different, thicker wood
REM   v2.bat --view 8                     a smaller resident ring
REM   v2.bat --out shot.png --spp 256     render one frame offline and exit
REM   v2.bat --help                       every option
REM
REM controls:  W A S D walk, shift sprints, space jumps, F toggles fly.
REM            Y opens the SETTINGS MENU -- it frees the mouse so you can click.
REM            Its last row bakes the current settings as the new defaults;
REM            v2\rebuild.bat then applies them.
REM            right-drag looks, scroll zooms, P screenshots, F1 help,
REM            arrow keys scrub the day/night clock (up/down = fast),
REM            X + scroll wheel sets the cycle speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS,
REM            ESC releases the mouse -- ESC again quits.
REM
REM The engine now lives in this repo, so the path is relative to this file
REM rather than hardcoded. Override it by setting V2_HOME first.
REM ---------------------------------------------------------------------------
setlocal

if "%V2_HOME%"=="" set "V2_HOME=%~dp0v2"
set "EXE=%V2_HOME%\build\v2.exe"

if not exist "%EXE%" (
  echo v2: %EXE% not found.
  echo.
  echo     Build it first:  double-click %V2_HOME%\rebuild.bat
  echo     or from an MSYS2 shell:   cd %V2_HOME%  ^&^&  bash build.sh
  echo.
  pause
  exit /b 1
)

REM v2 loads its GPU programs from disk at startup rather than embedding them,
REM so these two have to exist beside the exe. Missing them is the one failure
REM that would otherwise surface as an unhelpful "cannot open" deep in startup.
for %%F in (v2.optixir tonemap.ptx) do (
  if not exist "%V2_HOME%\build\%%F" (
    echo v2: %%F is missing from %V2_HOME%\build.
    echo     Rebuild with %V2_HOME%\rebuild.bat
    echo.
    pause
    exit /b 1
  )
)

REM The pine and decoration models. The engine defaults to these paths already;
REM passing them explicitly means a v2.exe built elsewhere still finds them.
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
if not exist "%PINES%\pine_1.vox" (
  echo v2: no pine_1.vox under %PINES% -- falling back to the engine default.
  set "PINES="
)

REM Nothing has to be added to PATH: OptiX is loaded out of the driver store at
REM runtime and the CUDA driver API lives in System32.

REM Run from THIS folder, not the build folder, so screenshots (v2_shot_NNN.png)
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
  echo v2 exited with code %RC%.
  pause
)
exit /b %RC%
