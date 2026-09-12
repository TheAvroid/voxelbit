@echo off
REM ---------------------------------------------------------------------------
REM checkstore.bat -- verify the voxel store without a GPU and without a build.
REM
REM Two passes, both of which take seconds:
REM
REM   1. THE FORMAT AND THE ARITHMETIC, in C++. scene/bricks.h, models.h and
REM      generate.h include nothing but each other and the core headers, so g++
REM      can load the REAL .vox assets, build the whole world, pack it exactly
REM      as the uploader does, read every voxel back out of the packed buffers,
REM      and check the brick walk against a brute-force walk over the world's
REM      own voxels -- with no device and no shader compiler anywhere.
REM
REM      It also checks the two ways a .vox file lies: that the nine pines came
REM      back TALLER than 256 voxels (a .vox coordinate is a byte, so a tall
REM      model is split across pieces and taking only the first loses every
REM      treetop) and that flowers.vox came back as SIX pieces and not one --
REM      the same overlap rule, read the other way.
REM
REM      That catches the two failures a store like this has that are SILENT:
REM      a popcount mismatch in the compacted material run, which gives every
REM      voxel its neighbour's colour, and a DDA that steps wrong, which moves
REM      surfaces by a voxel. Neither crashes and neither draws black.
REM
REM   2. THE SHADER, with slangc. Falcor compiles Slang at LOAD, so a type error
REM      in the store does not appear until the engine is running -- and then it
REM      appears as a program that failed to create, several layers down. The
REM      Vulkan SDK ships the same 2026.13 compiler Falcor uses here, and
REM      shaders/StoreProbe.cs.slang is a scratch entry point that exists only to
REM      give it something to compile the seam into.
REM
REM      BOTH BACKENDS ARE CHECKED. The null store has to keep compiling: it is
REM      the zero every measurement of a real one is taken against, and a change
REM      to the lighting stack that stops building against it has reached through
REM      the seam for something a store is not obliged to provide.
REM
REM Use this after every store edit. check.bat is the one for everything else,
REM and build.bat is for when you want to SEE it.
REM ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

set "OUT=%TEMP%\v4_checkstore"
if not exist "%OUT%" mkdir "%OUT%"

echo === 1/2  the format and the arithmetic ===
where g++ >nul 2>nul
if errorlevel 1 (
  echo checkstore: no g++ on PATH -- skipping the C++ harness.
  echo             MSYS2's mingw64\bin is where it lives here.
) else (
  g++ -O2 -std=c++17 -I "%HERE%\src" "%HERE%\tests\brick_test.cpp" -o "%OUT%\brick_test.exe"
  if errorlevel 1 ( echo checkstore: the harness does not compile. & exit /b 1 )
  "%OUT%\brick_test.exe" 96
  if errorlevel 1 ( echo checkstore: the store FAILED its own tests. & exit /b 1 )
  "%OUT%\brick_test.exe" 96 demo
  if errorlevel 1 ( echo checkstore: the demo scene FAILED. & exit /b 1 )
  REM AND THE PATH WITH NO ASSETS, which is the one that rots. Everything in
  REM the generator has a procedural fallback so that a wrongly-pointed asset
  REM folder gives a plainer wood rather than a black frame -- and a fallback
  REM nothing ever exercises is a fallback that has stopped compiling.
  "%OUT%\brick_test.exe" 96 landscape none
  if errorlevel 1 ( echo checkstore: the NO-MODELS fallback FAILED. & exit /b 1 )
  REM AND ONE BIG WORLD, WHICH IS NOT THE SAME WORLD SCALED UP. Some of the
  REM store's rules only have anything to bite on past a certain size: the
  REM whole-brick cull dropping the inside of a lake needed water deeper than
  REM one brick, and at 96 m there is none -- so this harness was GREEN while
  REM the middle of every lake was missing from the build. Thirteen seconds.
  "%OUT%\brick_test.exe" 192
  if errorlevel 1 ( echo checkstore: the 192 m world FAILED. & exit /b 1 )
)

echo.
echo === 2/2  the shader ===
if "%SLANGC%"=="" set "SLANGC=C:\VulkanSDK\1.4.357.0\Bin\slangc.exe"
if not exist "%SLANGC%" (
  echo checkstore: no slangc at %SLANGC% -- set SLANGC to one.
  exit /b 1
)

pushd "%HERE%\shaders"
"%SLANGC%" -target dxil -profile sm_6_5 -stage compute -entry main ^
    -DV4_STORE_IMPL="\"stores/Aabb.slang\"" StoreProbe.cs.slang -o "%OUT%\aabb.dxil"
if errorlevel 1 ( popd & echo checkstore: stores\Aabb.slang does NOT compile. & exit /b 1 )
echo   stores\Aabb.slang compiles.

"%SLANGC%" -target dxil -profile sm_6_5 -stage compute -entry main ^
    StoreProbe.cs.slang -o "%OUT%\null.dxil"
if errorlevel 1 ( popd & echo checkstore: stores\Null.slang does NOT compile. & exit /b 1 )
echo   stores\Null.slang compiles.
popd

echo.
echo v4: the store checks out.
exit /b 0
