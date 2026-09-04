@echo off
REM ---------------------------------------------------------------------------
REM rebuild.bat -- apply a bake.
REM
REM "Bake as default" in the settings menu (Y) writes src/core/defaults.h. That
REM is SOURCE, so it does not take effect until v2 is rebuilt -- this is the
REM rebuild, so that applying a bake is a double-click rather than a thing you
REM have to remember the shell incantation for.
REM
REM build.sh is a bash script and the toolchain is MSYS2's, so it has to run
REM under MSYS2's bash rather than cmd. -lc gives it a login shell, which is
REM what puts the MinGW toolchain on PATH.
REM ---------------------------------------------------------------------------
setlocal

if "%MSYS2_ROOT%"=="" set "MSYS2_ROOT=C:\msys64"
set "BASH=%MSYS2_ROOT%\usr\bin\bash.exe"
set "HERE=%~dp0"

if not exist "%BASH%" (
  echo rebuild: %BASH% not found.
  echo          Set MSYS2_ROOT to your MSYS2 install, or run "bash build.sh"
  echo          yourself from an MSYS2 shell.
  echo.
  pause
  exit /b 1
)

REM %~dp0 ends with a backslash and carries a drive letter; bash needs neither.
set "UNIXHERE=%HERE:\=/%"
set "UNIXHERE=%UNIXHERE:~0,-1%"

echo Rebuilding v2 with the baked defaults ...
echo.
"%BASH%" -lc "cd '%UNIXHERE%' && bash build.sh"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo Done. The baked settings are now what v2 opens with.
) else (
  echo Build failed with code %RC%.
)
pause
exit /b %RC%
