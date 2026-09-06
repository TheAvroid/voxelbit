@echo off
REM ---------------------------------------------------------------------------
REM build_physx.bat -- build NVIDIA PhysX 5 for v7, once.
REM
REM WHY THIS EXISTS RATHER THAN physx\generate_projects.bat ON ITS OWN.
REM
REM PhysX ships presets up to vc17, which selects the CMake generator
REM "Visual Studio 17 2022". Visual Studio 2022 is not installed on this
REM machine -- v7 builds with 2026 (v18) -- and that generator does not fall
REM back, it simply fails to find a toolset.
REM
REM The generator script does however understand a `generator="ninja"` attribute
REM on a preset, and NINJA DOES NOT CARE WHICH VISUAL STUDIO IT IS: it compiles
REM with whatever cl.exe the environment provides. So v7 ships its own preset --
REM buildtools\presets\public\vc17win64-ninja.xml, a copy of the cpu-only one
REM with that attribute added -- and this script arranges the environment first.
REM
REM CPU ONLY, DELIBERATELY. PhysX's GPU build pulls in its own CUDA toolchain
REM and a long list of architectures, and what v7 wants from PhysX is a
REM character controller and rigid bodies against voxel geometry -- work that
REM is nowhere near needing GPU rigid body solving. The GPU build can come later
REM if anything ever justifies it.
REM
REM Snippets and the PVD runtime are off for the same reason: they are samples
REM and a debugger connection, and they are most of the build time.
REM
REM Run once. v7's own build.bat picks up the result if it is there and builds
REM without physics if it is not.
REM ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "PHYSX=%HERE%\external\physx\physx"

if not exist "%PHYSX%\generate_projects.bat" (
  echo build_physx: no PhysX checkout at %PHYSX%
  echo     git clone --depth 1 https://github.com/NVIDIA-Omniverse/PhysX.git "%HERE%\external\physx"
  exit /b 1
)

REM --- the compiler environment, before anything asks for cl -------------------
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (
  echo build_physx: no Visual Studio with the C++ tools found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if errorlevel 1 ( echo build_physx: vcvars64 failed. & exit /b 1 )

echo   generating (ninja, cpu only)
REM Called by FULL PATH. "call generate_projects.bat" after a pushd is not
REM reliable here -- cmd resolved it as not recognized -- and the failure then
REM has to be checked on its own line, because "( popd & exit /b 1 )" does not
REM propagate the code out of the parenthesised block.
pushd "%PHYSX%"
call "%PHYSX%\generate_projects.bat" vc17win64-ninja
set "GENRC=%ERRORLEVEL%"
popd
if not "%GENRC%"=="0" (
  echo build_physx: generate failed with %GENRC%.
  exit /b 1
)

REM --- and build --------------------------------------------------------------
REM The generator puts a Ninja Multi-Config tree under compiler\vc17win64-ninja.
set "BUILDDIR=%PHYSX%\compiler\vc17win64-ninja"
if not exist "%BUILDDIR%\build.ninja" (
  echo build_physx: no build.ninja under %BUILDDIR% -- generation did not complete.
  exit /b 1
)

echo   ninja release
cmake --build "%BUILDDIR%" --config release
if errorlevel 1 ( echo build_physx: compile failed. & exit /b 1 )

echo.
echo built. v7's build.bat will find PhysX under
echo     %PHYSX%\bin
exit /b 0
