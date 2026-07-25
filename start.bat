@echo off
REM ────── Local web server for the voxelbit website (needed for WebGPU; file:// won't work) ──────
REM Double-click this file: it starts the no-cache server and opens the game in your browser.
cd /d "%~dp0"

echo.
echo  Serving %CD%
echo  Open:  http://localhost:8080/
echo  (Press Ctrl+C or close this window to stop.)
echo.

REM ── Stop any PREVIOUS server still holding port 8080 ──────────────────────────
REM Without this, an old instance keeps serving STALE code: your edits don't show up, and a
REM renamed/deleted file 404s while the browser happily connects to the outdated server. The
REM old behaviour was "port in use = already running, all good", which silently hid exactly that.
for /f "tokens=5" %%p in ('netstat -ano 2^>nul ^| findstr /c:":8080" ^| findstr /c:"LISTENING"') do (
    echo  Stopping previous server on port 8080, PID %%p - so this one serves current files...
    taskkill /PID %%p /F >nul 2>nul
)

REM Open the game once the server is up (detached 1 s delay so the first load doesn't race the server).
start "" cmd /c "timeout /t 1 >nul & start "" http://localhost:8080/"

REM No-cache server (serve-nocache.py, port 8080) so edits show up on refresh.
REM Prefer the "py" launcher, fall back to "python".
where py >nul 2>nul && (py serve-nocache.py) || (python serve-nocache.py)

REM If the window closed instantly, Python isn't installed/on PATH.
pause
