@echo off
REM ---------------------------------------------------------------------------
REM v5.bat -- run the pine forest from the engine folder.
REM
REM The launcher at C:\voxelbit\v5.bat is the one to double-click day to day;
REM this one sits next to the source and finds the build through %~dp0, so a
REM copy of the engine checked out anywhere still runs without configuration.
REM
REM   v5.bat                              walk around in the wood
REM   v5.bat --no-dlss                    the raw ReSTIR output, to compare
REM   v5.bat --seed 7 --density 0.8       a different, thicker wood
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
REM ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
set "EXE=%HERE%target\release\v5.exe"

if not exist "%EXE%" (
  echo v5: %EXE% not found.
  echo     Build it first:  build.bat
  echo.
  pause
  exit /b 1
)

REM DLSS loads its models from these at runtime, out of the exe's own folder.
for %%F in (nvngx_dlss.dll nvngx_dlssd.dll) do (
  if not exist "%HERE%target\release\%%F" (
    echo v5: %%F is missing from target\release -- DLSS will be off.
    echo     Re-run build.bat to copy it from the DLSS SDK.
    echo.
  )
)

REM The assets live one level up, in the repo this engine sits in.
set "PINES=%HERE%..\game\assets\foilage\pine9"
set "DECOR=%HERE%..\game\assets\decoration"

if not exist "%PINES%\pine_1.vox" (
  echo v5: no pine_1.vox under %PINES% -- falling back to the engine default.
  "%EXE%" %*
) else (
  "%EXE%" --pines "%PINES%" --decor "%DECOR%" %*
)
set "RC=%ERRORLEVEL%"

if not "%RC%"=="0" (
  echo.
  echo v5 exited with code %RC%.
  pause
)
exit /b %RC%
