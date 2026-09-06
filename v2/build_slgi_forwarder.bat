@echo off
REM ---------------------------------------------------------------------------
REM build_slgi_forwarder.bat -- build the shim slang-gfx loads instead of
REM                            d3d12 / dxgi. Installed under BOTH names.
REM
REM See slgi_forwarder.c for the full reasoning. In short: slang-gfx has been
REM patched to ask for "slp12" and "slgi" where it used to ask for d3d12 and
REM dxgi, and this DLL answers to both. It calls through to sl.interposer.dll,
REM which keeps its own name so Streamline can still find its own plugins.
REM
REM It THUNKS rather than forwards, because the Windows loader splits an export
REM forward at the first dot and "sl.interposer.dll" contains two.
REM
REM Run after patch_gfx_interposer.py, which installs slp12.dll and a placeholder
REM slgi.dll that this then overwrites with the real forwarder.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"
set "OUT=%HERE%\external\slang\bin"

REM No prerequisite check. This DLL forwards to sl.interposer.dll by NAME --
REM the loader resolves that when something opens the forwarder, not now -- so
REM it can be built before the interposer is installed. The previous version
REM required slp12.dll to exist first, which meant a rebuild after --restore
REM silently skipped and left a STALE forwarder in place, still pointing at the
REM old target.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VCVARS="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * ^
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
      -property installationPath 2^>nul`) do set "VCVARS=%%i\VC\Auxiliary\Build\vcvars64.bat"
)
if not exist "!VCVARS!" ( echo build_slgi: no MSVC x64 toolchain found. & exit /b 1 )
call "!VCVARS!" >nul 2>nul

set "TMPDIR=%HERE%\build\slgi"
if not exist "%TMPDIR%" mkdir "%TMPDIR%"

REM The forwards are /export linker pragmas inside the .c -- see the comment at
REM the top of slgi_forwarder.c for why a .def file cannot express them.
cl /nologo /LD /O1 /MT "%HERE%\slgi_forwarder.c" /Fe:"%TMPDIR%\slgi.dll" /Fo:"%TMPDIR%\\"
if errorlevel 1 ( echo build_slgi: compile/link failed. & exit /b 1 )

REM INSTALLED UNDER BOTH NAMES, and forgetting the second one is a silent
REM no-op waiting to happen: slang-gfx resolves the DXGI entry points from
REM slgi.dll but D3D12CreateDevice from slp12.dll, and it is D3D12CreateDevice
REM that hands the device to Streamline early enough for DLSS-G to hook swap
REM chain creation. Rebuilding only slgi.dll leaves a stale slp12.dll in place
REM and every change to that path quietly does nothing.
copy /y "%TMPDIR%\slgi.dll" "%OUT%\slgi.dll" >nul
if errorlevel 1 ( echo build_slgi: could not install slgi.dll ^(is v2.exe running?^) & exit /b 1 )
copy /y "%TMPDIR%\slgi.dll" "%OUT%\slp12.dll" >nul
if errorlevel 1 ( echo build_slgi: could not install slp12.dll ^(is v2.exe running?^) & exit /b 1 )

echo   shim built: slgi.dll + slp12.dll thunk through to sl.interposer.dll
exit /b 0
