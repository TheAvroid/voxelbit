@echo off
REM ────── voxelbit local dev server (needed for WebGPU; file:// won't work) ──────
REM Double-click this file. It starts the no-cache server MINIMISED and opens the game.
REM This launcher window closes itself once the browser is opening - only the small
REM minimised "voxelbit server" window stays behind. Close that window to stop the server.
cd /d "%~dp0"

REM ── Stop any PREVIOUS server still holding port 8080 ──────────────────────────
REM Without this, an old instance keeps serving STALE code: your edits don't show up, and a
REM renamed/deleted file 404s while the browser happily connects to the outdated server. The
REM old behaviour was "port in use = already running, all good", which silently hid exactly that.
for /f "tokens=5" %%p in ('netstat -ano 2^>nul ^| findstr /c:":8080" ^| findstr /c:"LISTENING"') do (
    echo  Stopping previous server on port 8080, PID %%p ...
    taskkill /PID %%p /F >nul 2>nul
)

REM ── Start the server in its OWN minimised window ──────────────────────────────
REM tools\serve-nocache.py serves the game\ folder only, so tools\, source\ and docs\
REM are unreachable from the browser. The spawned window inherits this directory.
REM The trailing pause keeps the window readable if Python is missing or the port is
REM taken - otherwise the error would vanish with the window.
start "voxelbit server" /min cmd /c "where py >nul 2>nul && (py tools\serve-nocache.py) || (python tools\serve-nocache.py) & echo. & echo  Server stopped. If it closed instantly, Python is not installed or not on PATH. & pause >nul"

REM ── Open the game, then close this launcher window ────────────────────────────
REM 1 s so the first request doesn't race the server's startup.
timeout /t 1 >nul
start "" http://localhost:8080/
exit
