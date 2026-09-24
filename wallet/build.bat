@echo off
rem Build the wallet and put the exe beside this file. The copy in target\
rem is where cargo leaves it; this one is the one to double-click, and it is
rem refreshed on every build so it is never older than the code.
setlocal
set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"
cd /d "%~dp0"

rem An OPEN wallet holds its exe, and neither cargo nor copy can overwrite a
rem running exe ("Access is denied" -- it stopped the build on 2026-09-23).
rem Windows does allow RENAMING one, so each is moved aside first: the open
rem window keeps running the old build and the next launch gets the new one.
rem Stale .old files are removed here once nothing is running them.
del /q "target\release\*.old" "*.old" 2>nul
if exist "target\release\wallet.exe" ren "target\release\wallet.exe" "wallet.%RANDOM%.old"
cargo build --release || exit /b 1
if exist "wallet.exe" ren "wallet.exe" "wallet.%RANDOM%.old"
copy /y "target\release\wallet.exe" "wallet.exe" >nul || exit /b 1
echo built %~dp0wallet.exe
