@echo off
REM ---------------------------------------------------------------------------
REM v6-framegen-debug.bat -- DOUBLE-CLICK THIS when frame generation says
REM                          "none generated".
REM
REM Runs v6 with Streamline's own logging turned on, then prints the handful of
REM lines that actually explain what DLSS-G did. Every other API in this
REM integration reports success whether or not a frame was generated -- this log
REM is the only source that does not, and it is what found the last three bugs.
REM
REM WHAT TO DO: let the window open, CLICK IT so it has focus, walk around for
REM ten seconds or so, then close it with ESC ESC. The summary is printed here
REM afterwards; copy the whole thing.
REM
REM Frame generation needs a VISIBLE, FOCUSED window. A minimised or covered
REM window is not really presented, so there is nothing to insert a frame into
REM and it will legitimately report zero.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "LOGDIR=%HERE%\v6\build\bin\Release\sl-log"

if exist "%LOGDIR%" rmdir /s /q "%LOGDIR%" 2>nul
mkdir "%LOGDIR%" 2>nul

set "V6_SL_LOG=1"
echo.
echo   Starting v6 with frame generation and Streamline logging.
echo   CLICK THE WINDOW so it has focus, walk around a few seconds,
echo   then press ESC twice to quit. The summary appears here.
echo.
call "%HERE%\v6.bat" --fg 2x %*

echo.
echo ===========================================================================
echo   STREAMLINE SUMMARY -- copy everything below this line
echo ===========================================================================
if not exist "%LOGDIR%\sl.log" (
  echo   No log was written. Streamline did not initialise at all, which
  echo   usually means the interposer patch is not in place -- run
  echo   v6-framegen.bat once first.
  goto :done
)

echo.
echo --- did the plugins initialise BEFORE the swap chain was created? ---------
echo   This is THE test. DLSS-G wraps the swap chain from a hook on its
echo   creation, and hooks are dead until the plugins have a device. If
echo   "Initializing plugins" comes AFTER "Upgraded IDXGISwapChain", DLSS-G
echo   missed the swap chain and will pass every frame straight through
echo   without a single error to say so.
echo.
findstr /C:"Initializing plugins" /C:"Upgraded IDXGISwapChain" /C:"without device being created" "%LOGDIR%\sl.log"
echo.
echo --- errors (NOTHING filtered) --------------------------------------------
echo   The old version of this script hid every line mentioning
echo   getNGXFeatureRequirements. That was a mistake: it is the requirements
echo   checker, so it is exactly where a failed OS/driver/HWS requirement is
echo   reported. Nothing is filtered now.
echo.
findstr /C:"[error]" "%LOGDIR%\sl.log"
echo.
echo --- frame generation -----------------------------------------------------
findstr /I /C:"dlss_g" "%LOGDIR%\sl.log" | findstr /I /C:"present" /C:"generat" /C:"support" /C:"error" /C:"warn"
echo.
echo --- swapchain ------------------------------------------------------------
findstr /I /C:"swapchain" "%LOGDIR%\sl.log" | findstr /I /C:"proxy" /C:"error"
echo.
echo --- reflex ---------------------------------------------------------------
findstr /I /C:"reflex" "%LOGDIR%\sl.log" | findstr /I /C:"error" /C:"warn" /C:"mode"
echo.
echo   (full log: %LOGDIR%\sl.log)

:done
echo ===========================================================================
echo.
pause
exit /b 0
