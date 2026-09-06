@echo off
REM ---------------------------------------------------------------------------
REM build.bat -- build v7 against NVIDIA Falcor.
REM
REM ONE CMAKE INVOCATION, and the arrangement is worth explaining because it is
REM not the obvious one. Falcor is configured as the SOURCE tree and this
REM directory is pulled in through its FALCOR_EXTERNAL_APP_DIR hook, while the
REM BINARY directory is pointed back here. So:
REM
REM   * Falcor stays the top-level project, which it has to -- several of its
REM     rules copy data/ and scripts/ relative to CMAKE_SOURCE_DIR, and making
REM     v7 the top level breaks them.
REM   * Everything BUILT lands under v7\build: v7.exe, Falcor.dll, slang, the
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

REM --- the preview D3D12 stack, opted into ------------------------------------
REM
REM "build.bat preview" asks for the 1.721.2-preview Agility runtime and the DXC
REM 1.10 preview. Together they give Shader Model 6.10 and therefore cooperative
REM vectors -- neural shading on D3D12 ALONGSIDE DLSS, which is the only
REM configuration where both work at once.
REM
REM IT NEEDS WINDOWS DEVELOPER MODE, AND A REBOOT AFTER TURNING THAT ON. The
REM D3D12 runtime latches the setting at boot; without it D3D12CreateDevice
REM returns DXGI_ERROR_SDK_COMPONENT_MISSING (0x887E0003) and v7 cannot open a
REM device at all -- which Falcor reports only as "Failed to create device".
set "PREVIEW=OFF"
if /i "%~1"=="preview" set "PREVIEW=ON"
if /i "%~2"=="preview" set "PREVIEW=ON"

REM A CHANGED FLAG MUST DISCARD THE CACHE. The configure below runs only when
REM there is no cache, so without this "build.bat preview" on an existing build
REM tree would silently do nothing at all -- the worst way for a flag to behave.
set "WAS="
if exist "%OUT%\CMakeCache.txt" for /f "tokens=2 delims==" %%V in ('findstr /b /c:"V7_D3D12_PREVIEW:" "%OUT%\CMakeCache.txt" 2^>nul') do set "WAS=%%V"
if defined WAS if not "%WAS%"=="%PREVIEW%" (
  echo   preview stack %PREVIEW% ^(was %WAS%^) -- discarding the cache
  rmdir /s /q "%OUT%" 2>nul
)

REM --- refuse to start if the game is holding its own exe ----------------------
REM A running v7.exe cannot be overwritten, and the failure arrives at the very
REM END of the build as a bare "LNK1104: cannot open file v7.exe", which says
REM nothing about why. That is a long wait for a message you cannot act on --
REM and it is exactly what happens after a "Bake as default", because the
REM natural thing to do next is rebuild with the game still open.
REM
REM So it is checked first, and said plainly.
tasklist /FI "IMAGENAME eq v7.exe" 2>nul | find /I "v7.exe" >nul
if not errorlevel 1 (
  echo.
  echo v7: v7.exe is still running, and the linker cannot overwrite a running exe.
  echo     Quit it first -- ESC twice in the window -- then run this again.
  echo.
  echo     Nothing has been changed. The build you have still works.
  exit /b 1
)

REM --- Falcor, and why v7 has its own copy -------------------------------------
REM
REM v6 builds against the shared checkout at C:\Users\mrwbh\Falcor. v7 does NOT,
REM and the reason is one line of C++.
REM
REM v7 needs Slang 2026.13 for cooperative vectors (see the configure block
REM below). Between 2024.1.34 and 2026.13.1 exactly one slang-gfx signature that
REM Falcor uses changed -- gfxEnableDebugLayer gained an enable flag -- and
REM Falcor 8.0 calls it the old way. Fixing that in the shared tree would edit a
REM file the FINISHED v6 engine compiles, to buy a feature v6 does not use.
REM
REM So the source is forked instead: 75 MB, which is what the tree costs without
REM its build directory. The eighteen packman junctions underneath it still
REM point at the SHARED C:\packman-repo -- those are read-only binary
REM dependencies and nothing here writes to them, so copying them would buy
REM nothing. Only the source is v7's own, and only one line of it differs.
REM
REM Set FALCOR_DIR to override, e.g. to build v7 against upstream again.
if "%FALCOR_DIR%"=="" set "FALCOR_DIR=%HERE%\external\falcor"
if not exist "%FALCOR_DIR%\CMakeLists.txt" (
  echo v7: no Falcor checkout at %FALCOR_DIR%.
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
  echo v7: Falcor's dependencies are not fetched yet.
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
  echo v7: no MSVC x64 toolchain found.
  echo     Install "Desktop development with C++" from the Visual Studio
  echo     Installer, or set VCVARS to your vcvars64.bat.
  exit /b 1
)
call "!VCVARS!" >nul 2>nul
if errorlevel 1 ( echo v7: vcvars64 failed. & exit /b 1 )

REM --- configure --------------------------------------------------------------
REM Only when there is no cache: reconfiguring every build costs twenty seconds
REM and CMake re-runs itself anyway whenever a CMakeLists.txt changes.
if not exist "%OUT%\CMakeCache.txt" (
  echo   cmake configure ^(Falcor + v7^)
  REM SLANG IS v7's OWN, AND THAT IS THE WHOLE REASON THIS ENGINE EXISTS.
  REM
  REM Falcor's packman package is Slang 2024.1.34, which has no cooperative
  REM vectors in it at all -- no CoopVec type, no coopVecMatMul, nothing. In-
  REM shader neural inference is not a feature you switch on in that compiler,
  REM it is a language construct it has never heard of. RTX Neural Shading wants
  REM 2026.10 or newer.
  REM
  REM THE OBVIOUS FIX WOULD BREAK v6. Falcor's checkout is SHARED -- v6 builds
  REM against the same tree -- and its Slang arrives through a packman junction
  REM into external/packman/slang. Upgrading that upgrades the shader compiler
  REM underneath an engine that is finished and working, to buy a feature it
  REM does not use.
  REM
  REM So the override is made HERE instead, in v7's own configure line. Falcor
  REM has a first-class hook for it -- FALCOR_LOCAL_SLANG and friends are CACHE
  REM variables, and a cache belongs to a BUILD TREE, not to the source. v7
  REM builds into v7uild with v7's cache; v6 builds into v6uild with its
  REM own. Nothing is written into the Falcor checkout by either. See
  REM external/slang, staged from the Vulkan SDK's 2026.13.1.
  REM
  REM :STRING ON THE BUILD_DIR IS LOAD-BEARING, not a style choice. That
  REM variable is declared CACHE PATH by Falcor, and CMake rewrites a -D value
  REM of type PATH into an ABSOLUTE one -- so a plain "." became the build
  REM directory, was pasted onto the end of the slang directory, and ninja went
  REM looking for "external/slang/c:/voxelbit/v7/lib". Typing it STRING seeds
  REM the cache entry before Falcor'"'"'s set(... CACHE PATH) runs, and a cache
  REM entry that already exists keeps the type it was given.
  REM
  REM WHY THIS IS SAFE RATHER THAN A PORT: slang-gfx, which Falcor links, still
  REM ships in 2026.13.1, and its header gained five types between the two
  REM versions and lost none. The legacy slang.h names Falcor uses moved into
  REM slang-deprecated.h, which slang.h includes unconditionally.
  REM
  REM USD is off: it is the single largest thing in the Falcor build, it pulls
  REM in boost and openvdb, and v7 loads .vox files and nothing else.
  REM The system version is pinned because Falcor's presets ask for a Windows
  REM 10 SDK that is no longer what gets installed.
  "%CMAKE%" -S "%FALCOR_DIR%" -B "%OUT%" -G "Ninja Multi-Config" ^
    -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
    -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
    -DFALCOR_EXTERNAL_APP_DIR="%HERE:\=/%" ^
    -DFALCOR_ENABLE_USD=OFF ^
    -DFALCOR_LOCAL_SLANG=ON ^
    -DFALCOR_LOCAL_SLANG_DIR="%HERE:\=/%/external/slang" ^
    -DFALCOR_LOCAL_SLANG_BUILD_DIR:STRING=. ^
    -DV7_D3D12_PREVIEW=%PREVIEW%
  if errorlevel 1 ( echo v7: configure failed. & exit /b 1 )
)

REM --- build ------------------------------------------------------------------
echo   ninja %CONFIG%
"%CMAKE%" --build "%OUT%" --config %CONFIG% --target v7
if errorlevel 1 (
  echo.
  echo v7: build failed -- nothing was changed. The previous build still runs.
  echo     If the link failed, close any running v7.exe and build again.
  exit /b 1
)

echo built %OUT%\bin\%CONFIG%\v7.exe
exit /b 0
