@echo off
REM ---------------------------------------------------------------------------
REM build.bat -- build v2 with MSVC. One toolchain now, not two.
REM
REM WHY THIS REPLACED THE MinGW BUILD (build.sh):
REM
REM   Nothing was wrong with MinGW. It built a working engine for months. But
REM   every NVIDIA denoising library -- NRD, RTXTS, DLSS-RR -- ships as MSVC
REM   C++ with an MSVC ABI, and MinGW's linker cannot consume those. Staying on
REM   MinGW meant permanently ruling out every denoiser NVIDIA actually makes.
REM
REM   So the host moved to cl.exe. The engine's own code needed NO changes for
REM   it -- it compiled clean on the first attempt. The only thing MinGW was
REM   really providing was msys64's libglfw3.a, a MinGW-ABI static library that
REM   cl.exe cannot link. That is why external/glfw exists: GLFW's Win32 subset,
REM   vendored and built here with cl. It is 21 .c files and needs no cmake --
REM   GLFW supports exactly this, which is why there is no build system here.
REM
REM   nvcc is unaffected. It always drove cl.exe as its host compiler; the
REM   difference is that cl is now on PATH from vcvars, so -ccbin is not needed.
REM
REM /MT rather than /MD deliberately: the exe carries the CRT and needs no
REM redistributable, which is what -static-libgcc -static-libstdc++ gave before.
REM
REM Usage:  build.bat          normal build
REM         build.bat clean    discard everything, including vendored GLFW
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

if "%CUDA_PATH%"=="" set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
if "%OPTIX_PATH%"=="" set "OPTIX_PATH=C:\Users\mrwbh\optix-sdk"

REM --- locate and enter the MSVC environment ----------------------------------
REM vswhere is the documented way, but it lives in a fixed place that is not on
REM PATH, and is absent on some installs -- hence the explicit fallback sweep.
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
  echo build: no MSVC x64 toolchain found.
  echo        Install "Desktop development with C++" from the Visual Studio
  echo        Installer, or set VCVARS to your vcvars64.bat.
  exit /b 1
)
call "!VCVARS!" >nul 2>nul
if errorlevel 1 ( echo build: vcvars64 failed. & exit /b 1 )

REM --- prerequisites ----------------------------------------------------------
for %%F in ("%OPTIX_PATH%\include\optix.h" ^
            "%CUDA_PATH%\bin\nvcc.exe" ^
            "%CUDA_PATH%\lib\x64\cuda.lib") do (
  if not exist "%%~F" ( echo build: missing %%~F & exit /b 1 )
)

if not exist "%OUT%" mkdir "%OUT%"

REM --- 0. GLFW ----------------------------------------------------------------
REM Vendored and unchanging, so this runs once and is skipped every build after.
REM Delete build\glfw3.lib to force it.
if not exist "%OUT%\glfw3.lib" (
  echo   cl    glfw ^(win32 subset, once^)
  if not exist "%OUT%\glfw" mkdir "%OUT%\glfw"
  pushd "%HERE%\external\glfw\src"
  cl /nologo /c /O2 /MT /W3 /D_GLFW_WIN32 /DUNICODE /D_UNICODE ^
     /D_CRT_SECURE_NO_WARNINGS /Fo"%OUT%\glfw\\" *.c >nul
  if errorlevel 1 ( popd & echo build: GLFW failed. & exit /b 1 )
  popd
  lib /nologo /OUT:"%OUT%\glfw3.lib" "%OUT%\glfw\*.obj" >nul
  if errorlevel 1 ( echo build: GLFW archive failed. & exit /b 1 )
)

REM --- 1. OptiX device programs -> OptiX-IR -----------------------------------
REM compute_75 rather than the 4070's own compute_89: OptiX re-specialises the
REM IR for whatever card it finds, so building for the oldest RTX part costs
REM nothing and keeps the artefact portable.
echo   nvcc  v2.cu -^> v2.optixir
"%CUDA_PATH%\bin\nvcc" -optix-ir -arch=compute_75 -std=c++17 --use_fast_math ^
  -diag-suppress 20044 ^
  -I"%OPTIX_PATH%\include" -I"%CUDA_PATH%\include" ^
  "%HERE%\src\optix\v2.cu" -o "%OUT%\v2.optixir"
if errorlevel 1 ( echo build: v2.cu failed. & exit /b 1 )

REM --- 2. the display kernel -> PTX -------------------------------------------
REM PTX rather than a cubin so the driver JITs it for the installed card.
echo   nvcc  tonemap.cu -^> tonemap.ptx
"%CUDA_PATH%\bin\nvcc" -ptx -arch=compute_75 -std=c++17 --use_fast_math ^
  -I"%CUDA_PATH%\include" ^
  "%HERE%\src\optix\tonemap.cu" -o "%OUT%\tonemap.ptx"
if errorlevel 1 ( echo build: tonemap.cu failed. & exit /b 1 )

REM --- 3. host ----------------------------------------------------------------
REM /arch:AVX2 stands in for -march=native. /fp:fast is MSVC's -ffast-math, and
REM unlike GCC's it does not assume finiteness, so -fno-finite-math-only has no
REM counterpart to carry over. V2_SOURCE_DIR is where "Bake as default" writes
REM defaults.h back to -- an absolute path, because the exe is launched from
REM C:\voxelbit and anything relative would land in the wrong tree and report
REM success having written nothing that gets compiled.
set "CXXFLAGS=/nologo /c /O2 /arch:AVX2 /fp:fast /std:c++17 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS"
set "INCS=/I"%OPTIX_PATH%\include" /I"%CUDA_PATH%\include" /I"%HERE%\external\glfw\include""

echo   cl    image.cpp
cl %CXXFLAGS% %INCS% /Fo"%OUT%\image.obj" "%HERE%\src\core\image.cpp" >nul
if errorlevel 1 ( echo build: image.cpp failed. & exit /b 1 )

echo   cl    main.cpp -^> v2.exe
cl %CXXFLAGS% %INCS% /DV2_SOURCE_DIR="\"%HERE:\=/%/src\"" ^
   /Fo"%OUT%\main.obj" "%HERE%\src\main.cpp"
if errorlevel 1 ( echo build: main.cpp failed. & exit /b 1 )

REM OptiX needs no library at link time: optix_stubs.h loads nvoptix.dll out of
REM the driver store at runtime; cfgmgr32 and advapi32 are what that search
REM uses -- it walks the config manager for the display device, then reads the
REM driver's path out of the registry. MinGW linked advapi32 by default, so
REM this one only became visible on the move to cl. cuda.lib
REM is the DRIVER API, not the runtime -- v2 never links cudart.
link /nologo /OUT:"%OUT%\v2.exe" "%OUT%\main.obj" "%OUT%\image.obj" ^
  "%CUDA_PATH%\lib\x64\cuda.lib" "%OUT%\glfw3.lib" ^
  cfgmgr32.lib advapi32.lib gdi32.lib opengl32.lib user32.lib shell32.lib
if errorlevel 1 ( echo build: link failed. & exit /b 1 )

echo built %OUT%\v2.exe
exit /b 0
