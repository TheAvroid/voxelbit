@echo off
REM ---------------------------------------------------------------------------
REM v2.bat -- run the pine forest from the engine folder.
REM
REM The launcher at C:\voxelbit\v2.bat is the one to double-click day to day;
REM this one sits next to the source and finds the build through %~dp0, so a
REM copy of the engine checked out anywhere still runs without configuration.
REM
REM   v2.bat                              walk around in the wood
REM   v2.bat --denoise upscale            trace a quarter of the pixels
REM   v2.bat --seed 7 --trees 1400        a different, thicker wood
REM   v2.bat --out shot.png --spp 256     render one frame offline and exit
REM   v2.bat --help                       every option
REM
REM controls:  Y opens the SETTINGS MENU -- it frees the mouse so you can click
REM            the settings. Its last row bakes them as the new defaults;
REM            rebuild.bat in the engine folder then applies them.
REM            W A S D / Q E fly, shift sprints, right-drag looks,
REM            scroll zooms, N cycles the denoiser, P screenshots, F1 help,
REM            arrow keys scrub the day/night clock (up/down = fast),
REM            X + scroll wheel sets the cycle speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS,
REM            ESC releases the mouse -- ESC again quits.
REM ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
set "EXE=%HERE%build\v2.exe"

if not exist "%EXE%" (
  echo v2: %EXE% not found.
  echo     Build it first from an MSYS2 shell:  bash build.sh
  echo.
  pause
  exit /b 1
)

REM Run from the build folder so v2.optixir and tonemap.ptx are found beside
REM the exe, and so screenshots land with them.
pushd "%HERE%build"
"%EXE%" %*
set "RC=%ERRORLEVEL%"
popd

REM Only hold the window open on failure -- a clean exit should just close.
if not "%RC%"=="0" (
  echo.
  echo v2 exited with code %RC%.
  pause
)
exit /b %RC%
