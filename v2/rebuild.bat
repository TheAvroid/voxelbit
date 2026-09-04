@echo off
REM ---------------------------------------------------------------------------
REM rebuild.bat -- apply a bake.
REM
REM "Bake as default" in the settings menu (Y) writes src/core/defaults.h. That
REM is SOURCE, so it does not take effect until v2 is rebuilt -- this is the
REM rebuild, so applying a bake is a double-click rather than a shell
REM incantation you have to remember.
REM
REM This used to shell out to MSYS2 to run build.sh. It no longer needs to:
REM the host build is MSVC now, so this is just build.bat.
REM ---------------------------------------------------------------------------
setlocal

echo Rebuilding v2 with the baked defaults ...
echo.
call "%~dp0build.bat"
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo Done. The baked settings are now what v2 opens with.
) else (
  echo Build failed with code %RC%.
)
pause
exit /b %RC%
