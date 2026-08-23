@echo off
REM ────── voxelbit local dev server (needed for WebGPU; file:// won't work) ──────
REM Double-click this file. It starts the no-cache server MINIMISED and opens the game.
REM This launcher window closes itself once the browser is opening, and the minimised
REM "voxelbit server" window closes itself when you close the game tab - so a normal
REM play session leaves nothing behind.
cd /d "%~dp0"

REM Re-entry point: the minimised window runs this same file with --server (see below).
if /i "%~1"=="--server" goto :server

REM ── Stop any PREVIOUS server still holding port 8080 ──────────────────────────
REM Without this, an old instance keeps serving STALE code: your edits don't show up, and a
REM renamed/deleted file 404s while the browser happily connects to the outdated server. The
REM old behaviour was "port in use = already running, all good", which silently hid exactly that.
for /f "tokens=5" %%p in ('netstat -ano 2^>nul ^| findstr /c:":8080" ^| findstr /c:"LISTENING"') do (
    echo  Stopping previous server on port 8080, PID %%p ...
    taskkill /PID %%p /F >nul 2>nul
)

REM ── Start the server in its OWN minimised window, then open the game and exit ──
start "voxelbit server" /min cmd /c call "%~f0" --server
timeout /t 1 >nul
start "" http://127.0.0.1:8080/
exit /b


:server
REM ── Runs inside the minimised window ──────────────────────────────────────────
REM tools\serve-nocache.py serves the game\ folder only, so tools\, source\ and docs\
REM are unreachable from the browser. It shuts itself down when the last game tab closes.
where py >nul 2>nul && (py tools\serve-nocache.py) || (python tools\serve-nocache.py)

REM Only hold the window open if something actually went WRONG (Python missing, port
REM busy, crash). A clean exit means the server shut down on purpose because the game
REM tab was closed - pausing there is what used to leave this window open forever.
if errorlevel 1 (
    echo.
    echo  Server stopped unexpectedly ^(exit code %errorlevel%^).
    echo  If this window appeared instantly, Python is not installed or not on PATH.
    echo.
    pause
)
exit /b
