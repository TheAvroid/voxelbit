@echo off
REM ────── Local web server for the voxelbit website (needed for WebGPU; file:// won't work) ──────
REM Double-click this file: it starts the no-cache server and opens engine.html in your browser.
cd /d "%~dp0"

echo.
echo  Serving %CD%
echo  Open:  http://localhost:8080/engine.html
echo  (Press Ctrl+C or close this window to stop.)
echo.

REM Open the engine once the server is up (detached 1 s delay so the first load doesn't race the server).
start "" cmd /c "timeout /t 1 >nul & start "" http://localhost:8080/engine.html"

REM No-cache server (serve-nocache.py, port 8080) so edits show up on refresh.
REM Prefer the "py" launcher, fall back to "python". If the port is already in use,
REM a server is already running — the browser tab just opened against it, all good.
where py >nul 2>nul && (py serve-nocache.py) || (python serve-nocache.py)

REM If the window closed instantly, Python isn't installed/on PATH.
pause
