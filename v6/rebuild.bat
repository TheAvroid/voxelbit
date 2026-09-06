@echo off
REM ---------------------------------------------------------------------------
REM rebuild.bat -- apply a bake. THIS is the one to double-click.
REM
REM "Bake as default" in the settings menu (Y) writes src/core/defaults.h. That
REM is SOURCE, so it does not take effect until v6 is rebuilt -- this is the
REM rebuild, so applying a bake is a double-click rather than a shell
REM incantation you have to remember.
REM
REM The only difference from build.bat beside it is what a double-click does:
REM build.bat exits the moment it finishes, so the window closes before you can
REM read it. That one is for a prompt or a script. This one is for you.
REM ---------------------------------------------------------------------------
setlocal

echo Rebuilding v6 with the baked defaults ...
echo.
call "%~dp0build.bat" %*
set "RC=%ERRORLEVEL%"

echo.
if "%RC%"=="0" (
  echo Done. The baked settings are now what v6 opens with.
) else (
  echo Build failed with code %RC% -- nothing was changed, so the build you
  echo already have still runs.
)
pause
exit /b %RC%
