@echo off
REM ?????? Local web server for the voxelbit website (needed for WebGPU; file:// won't work) ??????
REM Double-click this file to start the server, then open the URL below in Chrome or Edge.
cd /d "%~dp0"

echo.
echo  Serving %CD%
echo  Open:  http://localhost:8000/engine.html
echo  (Press Ctrl+C or close this window to stop.)
echo.

REM Prefer the "py" launcher, fall back to "python".
where py >nul 2>nul && (py -m http.server 8000) || (python -m http.server 8000)

REM If the window closed instantly, Python isn't installed/on PATH.
pause

