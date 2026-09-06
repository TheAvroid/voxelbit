@echo off
REM ---------------------------------------------------------------------------
REM build_rtxgi.bat -- build the RTXGI DDGI static library v2 links against.
REM
REM Run once. The result is a .lib plus the SDK headers, and v2's CMakeLists
REM finds both by RTXGI_DDGI_DIR.
REM
REM WHY 1.3 AND NOT THE RTXGI ON THIS MACHINE ALREADY. C:\Users\mrwbh\RTXGI is
REM RTXGI 2.7, and 2.x does not contain DDGI at all -- NVIDIA retired the probe
REM volumes after 1.3 and 2.x ships NRC and SHaRC instead. 1.3.7 is the last
REM release with the irradiance probes, so that is what this builds.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

if "%RTXGI_SRC%"=="" set "RTXGI_SRC=C:\Users\mrwbh\RTXGI-DDGI"
if not exist "%RTXGI_SRC%\rtxgi-sdk\CMakeLists.txt" (
  echo build_rtxgi: no RTXGI DDGI checkout at %RTXGI_SRC%.
  echo     git clone --branch v1.3.7 https://github.com/NVIDIAGameWorks/RTXGI-DDGI
  exit /b 1
)

if "%FALCOR_DIR%"=="" set "FALCOR_DIR=C:\Users\mrwbh\Falcor"
set "CMAKE=%FALCOR_DIR%\tools\.packman\cmake\bin\cmake.exe"
set "NINJA=%FALCOR_DIR%\tools\.packman\ninja\ninja.exe"
set "DXIL=%FALCOR_DIR%\external\packman\dxcompiler\bin\x64"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath 2^>nul`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "!VCVARS!" ( echo build_rtxgi: no MSVC found. & exit /b 1 )
call "!VCVARS!" >nul 2>nul

set "OUT=%~dp0build-rtxgi"

REM RIGHT HAND, Y-UP is not a preference -- it is v2's world. The terrain is a
REM height field in Y and the camera basis is (right, up, forward); telling the
REM SDK anything else silently transposes every probe in the volume.
REM
REM RESOURCE_MANAGEMENT=OFF puts the SDK in UNMANAGED mode: v2 creates the probe
REM textures itself and hands the SDK their native handles.
REM
REM That is the opposite of the obvious choice, and the reason is Falcor. In
REM Managed mode the SDK creates the probe textures, and v2's own Slang shaders
REM -- the probe trace that fills the ray data, and the tracer that samples the
REM irradiance -- would then be reading and writing resources Falcor knows
REM nothing about. Falcor tracks resource STATES itself and inserts barriers
REM from that model; a texture it did not create is absent from it, so the
REM barriers around every probe dispatch would be missing. That is the same
REM class of bug the NGX interop in dlss.h has to work around by hand, and here
REM it is avoidable outright: let Falcor own the textures, and pay for it with
REM a root signature and eight pipeline states of ordinary D3D12 boilerplate.
"%CMAKE%" -S "%RTXGI_SRC%\rtxgi-sdk" -B "%OUT%" -G "Ninja Multi-Config" ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DRTXGI_STATIC_LIB=ON ^
  -DRTXGI_API_D3D12_ENABLE=ON ^
  -DRTXGI_API_VULKAN_ENABLE=OFF ^
  -DRTXGI_API_D3D12_DXIL_PATH="%DXIL%" ^
  -DRTXGI_DDGI_RESOURCE_MANAGEMENT=OFF ^
  -DRTXGI_COORDINATE_SYSTEM="Right Hand, Y-Up" ^
  -DRTXGI_GFX_NAME_OBJECTS=ON
if errorlevel 1 ( echo build_rtxgi: configure failed. & exit /b 1 )

"%CMAKE%" --build "%OUT%" --config Release
if errorlevel 1 ( echo build_rtxgi: build failed. & exit /b 1 )

echo.
echo built the RTXGI DDGI static library under %OUT%
exit /b 0
