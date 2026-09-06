@echo off
REM ---------------------------------------------------------------------------
REM build.bat -- build v6 against NVIDIA Falcor.
REM
REM ONE CMAKE INVOCATION, and the arrangement is worth explaining because it is
REM not the obvious one. Falcor is configured as the SOURCE tree and this
REM directory is pulled in through its FALCOR_EXTERNAL_APP_DIR hook, while the
REM BINARY directory is pointed back here. So:
REM
REM   * Falcor stays the top-level project, which it has to -- several of its
REM     rules copy data/ and scripts/ relative to CMAKE_SOURCE_DIR, and making
REM     v6 the top level breaks them.
REM   * Everything BUILT lands under v6\build: v6.exe, Falcor.dll, slang, the
REM     shaders, the lot. This tree is self-contained and nothing is written
REM     into the Falcor checkout.
REM
REM The first build compiles Falcor as well and takes a couple of minutes.
REM Every build after that is incremental and only recompiles what changed.
REM
REM Usage:  build.bat            normal build
REM         build.bat clean      discard the build directory
REM         build.bat debug      build the Debug configuration instead
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "OUT=%HERE%\build"

if /i "%~1"=="clean" (
  echo cleaning %OUT%
  if exist "%OUT%" rmdir /s /q "%OUT%"
  echo done.
  exit /b 0
)

set "CONFIG=Release"
if /i "%~1"=="debug" set "CONFIG=Debug"

REM --- refuse to start if the game is holding its own exe ----------------------
REM A running v6.exe cannot be overwritten, and the failure arrives at the very
REM END of the build as a bare "LNK1104: cannot open file v6.exe", which says
REM nothing about why. That is a long wait for a message you cannot act on --
REM and it is exactly what happens after a "Bake as default", because the
REM natural thing to do next is rebuild with the game still open.
REM
REM So it is checked first, and said plainly.
REM findstr, NOT find. When this batch file is invoked from a POSIX shell the
REM inherited PATH puts Git Bash's find.exe ahead of cmd's, and that one does
REM not understand /I -- so the guard errored out, the test passed by
REM accident, and the real failure arrived minutes later as a bare LNK1104
REM at the end of the link. findstr has no POSIX namesake to be shadowed by.
tasklist /FI "IMAGENAME eq v6.exe" 2>nul | findstr /I /C:"v6.exe" >nul
if not errorlevel 1 (
  echo.
  echo v6: v6.exe is still running, and the linker cannot overwrite a running exe.
  echo     Quit it first -- ESC twice in the window -- then run this again.
  echo.
  echo     Nothing has been changed. The build you have still works.
  exit /b 1
)

if "%FALCOR_DIR%"=="" set "FALCOR_DIR=C:\Users\mrwbh\Falcor"
if not exist "%FALCOR_DIR%\CMakeLists.txt" (
  echo v6: no Falcor checkout at %FALCOR_DIR%.
  echo     Set FALCOR_DIR, or clone https://github.com/NVIDIAGameWorks/Falcor there.
  exit /b 1
)

REM Falcor ships its own cmake and ninja through packman. They are used rather
REM than whatever is on PATH deliberately: Falcor 8 and several of its vendored
REM dependencies declare cmake_minimum_required below 3.5, which CMake 4 refuses
REM outright, so a modern system cmake cannot configure this tree at all.
set "CMAKE=%FALCOR_DIR%\tools\.packman\cmake\bin\cmake.exe"
set "NINJA=%FALCOR_DIR%\tools\.packman\ninja\ninja.exe"
if not exist "%CMAKE%" (
  echo v6: Falcor's dependencies are not fetched yet.
  echo     Run %FALCOR_DIR%\setup.bat first, then this again.
  exit /b 1
)

REM --- locate and enter the MSVC environment ----------------------------------
REM vswhere is the documented way, but it lives in a fixed place that is not on
REM PATH and is absent on some installs -- hence the explicit fallback sweep.
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
  echo v6: no MSVC x64 toolchain found.
  echo     Install "Desktop development with C++" from the Visual Studio
  echo     Installer, or set VCVARS to your vcvars64.bat.
  exit /b 1
)
call "!VCVARS!" >nul 2>nul
if errorlevel 1 ( echo v6: vcvars64 failed. & exit /b 1 )

REM --- configure --------------------------------------------------------------
REM Only when there is no cache: reconfiguring every build costs twenty seconds
REM and CMake re-runs itself anyway whenever a CMakeLists.txt changes.
if not exist "%OUT%\CMakeCache.txt" (
  echo   cmake configure ^(Falcor + v6^)
  REM USD is off: it is the single largest thing in the Falcor build, it pulls
  REM in boost and openvdb, and v6 loads .vox files and nothing else.
  REM The system version is pinned because Falcor's presets ask for a Windows
  REM 10 SDK that is no longer what gets installed.
  "%CMAKE%" -S "%FALCOR_DIR%" -B "%OUT%" -G "Ninja Multi-Config" ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DFALCOR_EXTERNAL_APP_DIR="%HERE:\=/%" ^
    -DFALCOR_ENABLE_USD=OFF
  if errorlevel 1 ( echo v6: configure failed. & exit /b 1 )
)

REM --- build ------------------------------------------------------------------
echo   ninja %CONFIG%
"%CMAKE%" --build "%OUT%" --config %CONFIG% --target v6
if errorlevel 1 (
  echo.
  echo v6: build failed -- nothing was changed. The previous build still runs.
  echo     If the link failed, close any running v6.exe and build again.
  exit /b 1
)

REM --- re-apply the Streamline interposer patch -------------------------------
REM
REM FALCOR RE-DEPLOYS gfx.dll ON EVERY BUILD and that silently undoes the
REM patch. build_scripts\deploycommon.bat line 30 is
REM
REM     robocopy %SlangDir%\bin %OutDir% *.dll
REM
REM which copies the pristine slang-gfx over the patched one. Nothing warns:
REM the engine still builds, still runs, and frame generation still reports
REM itself available -- it just never generates a frame, because slang-gfx is
REM back to loading the real d3d12 and dxgi and the swapchain is no longer a
REM Streamline proxy.
REM
REM That cost several rounds of debugging the wrong thing, so the patch is
REM re-applied here, after the deploy, every time. It is idempotent -- it
REM always patches from its own pristine backup -- and it is skipped entirely
REM when the shims were never installed.
if exist "%OUT%\bin\%CONFIG%\slp12.dll" (
  python "%HERE%\patch_gfx_interposer.py" >nul 2>&1
  if errorlevel 1 (
    echo   warning: could not re-apply the interposer patch -- frame generation
    echo            will be unavailable until patch_gfx_interposer.py is re-run.
  ) else (
    echo   interposer patch re-applied to gfx.dll
  )
)

echo built %OUT%\bin\%CONFIG%\v6.exe
exit /b 0
