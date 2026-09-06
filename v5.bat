@echo off
REM ---------------------------------------------------------------------------
REM v5.bat -- run the v5 engine: an endless voxel pine forest, ray traced by
REM           Bevy Solari and denoised by NVIDIA DLSS Ray Reconstruction.
REM
REM v1 is the browser engine, launched by start.bat. v2 is the native OptiX one.
REM v5 renders the same world through a real-time hybrid pipeline instead: the
REM lighting is raytraced with ReSTIR at about one ray per pixel and DLSS Ray
REM Reconstruction resolves it, rather than being converged by brute force.
REM
REM Double-click this, or run it from a prompt. Any arguments are passed
REM straight through to v5.exe:
REM
REM   v5.bat                              walk around in the wood
REM   v5.bat --seed 7 --density 0.8       a different, thicker wood
REM   v5.bat --view 6                     a bigger resident ring (costs fps)
REM   v5.bat --no-dlss                    the raw ReSTIR output, to compare
REM   v5.bat --time 19 --cycle 60         an evening, running fast
REM   v5.bat --shot shot.png              settle, write a png and exit
REM   v5.bat --help                       every option
REM
REM controls:  Y opens the SETTINGS MENU -- every render knob, live, with a
REM            row that bakes them as the new defaults. It frees the mouse so
REM            you can click; up/down or hover chooses, left/right or the wheel
REM            changes, 1-5 are presets.
REM            W A S D walk, shift sprints, space jumps, F toggles fly,
REM            Q or ctrl descends while flying.
REM            click to capture the mouse; right-drag looks without capturing.
REM            X + wheel sets the day/night speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS. The wheel alone does nothing.
REM            arrow keys scrub the clock (up/down are the fast ones),
REM            P pauses it, H toggles the fps counter, F1 prints the controls,
REM            ESC closes the menu, then releases the mouse, then quits.
REM
REM Override the engine location by setting V5_HOME first.
REM ---------------------------------------------------------------------------
setlocal

if "%V5_HOME%"=="" set "V5_HOME=%~dp0v5"
set "EXE=%V5_HOME%\target\release\v5.exe"

if not exist "%EXE%" (
  echo v5: %EXE% not found.
  echo.
  echo     Build it first:  %V5_HOME%\build.bat
  echo     ^(needs Rust from https://rustup.rs, plus the Vulkan SDK and the
  echo      DLSS SDK -- see %V5_HOME%\README.md^)
  echo.
  pause
  exit /b 1
)

REM DLSS loads its models from these at runtime, out of the exe's own folder.
REM Missing them is not fatal and not loud: v5 simply reports DLSS as
REM unsupported and shows the raw ReSTIR image, which is a puzzling thing to
REM see on an RTX card, so it is worth saying plainly here.
for %%F in (nvngx_dlss.dll nvngx_dlssd.dll) do (
  if not exist "%V5_HOME%\target\release\%%F" (
    echo v5: %%F is missing from the build folder -- DLSS will be off.
    echo     Re-run %V5_HOME%\build.bat to copy it from the DLSS SDK.
    echo.
  )
)

REM The pine and decoration models. The engine defaults to these paths already;
REM passing them explicitly means a v5.exe built elsewhere still finds them.
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
if not exist "%PINES%\pine_1.vox" (
  echo v5: no pine_1.vox under %PINES% -- falling back to the engine default.
  set "PINES="
)

REM The working directory is deliberately NOT changed, so a relative --shot path
REM resolves against wherever you ran this from -- which is what a command line
REM should do. Double-clicked, that is this folder anyway.
if "%PINES%"=="" (
  "%EXE%" %*
) else (
  "%EXE%" --pines "%PINES%" --decor "%DECOR%" %*
)
set "RC=%ERRORLEVEL%"

REM Only hold the window open on failure -- a clean exit should just close.
if not "%RC%"=="0" (
  echo.
  echo v5 exited with code %RC%.
  pause
)
exit /b %RC%
