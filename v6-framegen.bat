@echo off
REM ---------------------------------------------------------------------------
REM v6-framegen.bat -- DOUBLE-CLICK THIS to run v6 with frame generation on.
REM
REM Exactly the same engine as v6.bat, launched with --fg 2x. It exists so that
REM turning frame generation on does not require a command prompt.
REM
REM The first run sets frame generation up (a few seconds): it builds a small
REM shim, points slang-gfx at the Streamline interposer, and installs it. After
REM that it just launches.
REM
REM WHAT TO LOOK FOR once the window opens: the frame counter in the corner.
REM
REM   "N fps drawn -> M presented (2x)"   working. Note that N, the DRAWN rate,
REM                                       will be LOWER than without frame
REM                                       generation -- making a frame costs GPU
REM                                       time. M is the number that matters.
REM
REM   "N fps drawn, none generated"       not inserting frames. Press Y and read
REM                                       the line under "Frame generation"; it
REM                                       reports what the SDK actually said.
REM
REM 2x is the most an RTX 40-series can do. 3x and 4x are DLSS 4 multi-frame
REM generation and need a 50-series card; the menu hides them when the GPU
REM cannot do them.
REM
REM To undo the setup entirely and go back to a stock Falcor:
REM
REM     python v6\patch_gfx_interposer.py --restore
REM ---------------------------------------------------------------------------
setlocal

call "%~dp0v6.bat" --fg 2x %*
set "RC=%ERRORLEVEL%"

REM Frame generation is the one feature here that needs a visible, focused
REM window to do anything at all -- a minimised or occluded window is not
REM really presented, so there is nothing for it to insert a frame into. Worth
REM saying, because "it did nothing" and "it was never given a chance" look the
REM same from the outside.
if not "%RC%"=="0" (
  echo.
  echo v6 exited with code %RC%.
  pause
)
exit /b %RC%
