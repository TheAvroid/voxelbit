@echo off
REM ---------------------------------------------------------------------------
REM launcher/build.bat -- compile the self-extracting launcher.
REM
REM DELIBERATELY NOT PART OF THE CMAKE BUILD. The engine's build tree is four
REM gigabytes and takes the better part of an hour from clean; this is one
REM translation unit that links against nothing but Windows. Tying it to the
REM engine would mean a full configure to fix a typo in a message box.
REM
REM Output: launcher\launcher.exe, which tools/package.py then appends the game
REM to. On its own it does nothing useful -- run with no payload it says so and
REM exits.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

REM --- locate and enter the MSVC environment ----------------------------------
REM Lifted verbatim from v1\build.bat, including the fallback sweep: vswhere is
REM the documented way, it is not on PATH, and it is absent on some installs.
REM Note the caret-escaped line continuations -- the parentheses in "Program
REM Files (x86)" close the enclosing if-block without them.
set "VCVARS="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath 2^>nul`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "!VCVARS!" (
  for %%R in ("%ProgramFiles%\Microsoft Visual Studio" "%ProgramFiles(x86)%\Microsoft Visual Studio") do (
    for %%V in (2022 2019 18 17) do (
      for %%E in (Enterprise Professional Community BuildTools) do (
        if not defined FOUNDVC if exist "%%~R\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat" (
          set "VCVARS=%%~R\%%V\%%E\VC\Auxiliary\Build\vcvars64.bat"
          set "FOUNDVC=1"
        )
      )
    )
  )
)
if not exist "!VCVARS!" (
  echo launcher: no MSVC x64 toolchain found.
  echo           Install "Desktop development with C++" from the Visual Studio
  echo           Installer, or set VCVARS to your vcvars64.bat.
  exit /b 1
)
call "!VCVARS!" >nul 2>nul
if errorlevel 1 ( echo launcher: vcvars64 failed. & exit /b 1 )

cd /d "%~dp0"

REM The icon, so the file in a Downloads folder looks like the game rather than
REM like a generic Windows executable. Optional: no icon is not a build failure.
set "RES="
if exist "%~dp0..\game\logo.ico" (
  > icon.rc echo 1 ICON "..\\game\\logo.ico"
  rc /nologo /fo icon.res icon.rc >nul 2>&1
  if exist icon.res set "RES=icon.res"
)

REM /MT so the exe carries its own CRT: a player who has never installed a
REM Visual Studio redistributable must still be able to double-click it. The
REM ENGINE beside it is /MD and ships its runtime in the payload, but the
REM launcher has to run before anything has been unpacked.
cl /nologo /std:c++17 /O2 /MT /EHsc /DUNICODE /D_UNICODE ^
   launcher.cpp %RES% ^
   /link /SUBSYSTEM:WINDOWS /OUT:launcher.exe
if errorlevel 1 ( echo launcher: build failed. & exit /b 1 )

del /q launcher.obj icon.rc 2>nul
echo launcher: built %~dp0launcher.exe
endlocal
