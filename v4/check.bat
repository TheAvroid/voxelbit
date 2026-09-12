@echo off
REM ---------------------------------------------------------------------------
REM check.bat -- type-check every header in the engine without building it.
REM
REM main.cpp includes app.h, which includes the whole engine, so one /Zs syntax
REM pass over it checks all of it: the renderer, the store seam, the
REM camera, the audio, the recorder. It takes seconds, produces no object file
REM and no exe, and touches nothing the running game holds open.
REM
REM THAT LAST PART IS WHY THIS EXISTS. build.bat refuses to link while the exe
REM is running, and a full build is minutes and gigabytes. This is the gate to
REM use after every edit; the build is for when you want to SEE it.
REM
REM THE SIX INCLUDE ROOTS EACH FAIL ONE AT A TIME, several layers deep, and not
REM one of the messages names the dependency actually missing -- you get
REM Core/Error.h cannot open fstd/source_location.h, then fmt, then
REM Core/API/Handles.h cannot open slang.h, then imgui by way of
REM Core/Program/ShaderVar.h -> Utils/UI/Gui.h. Six rounds if you find them one
REM by one. They are all here.
REM
REM /D_USE_MATH_DEFINES IS REQUIRED, or Utils/Math/QuaternionMath.h fails on
REM M_PI -- which reads as a Falcor bug and is a missing define.
REM
REM DO NOT mark the vcpkg include as external (-external:I). It is then searched
REM after every -I, Falcor's own bundled NanoVDB wins, and a backend that uses
REM OpenVDB gets NanoVDB.h resolving old while CreateNanoGrid.h resolves new.
REM ---------------------------------------------------------------------------
setlocal
set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

REM Dependencies live in the v2 tree. They are NOT copied and NOT junctioned:
REM a junction here would mean a stray rmdir /s on this directory walks into it
REM and takes 680 MB of Falcor, Slang, PhysX and the Agility SDK with it.
if "%DEPS%"=="" set "DEPS=C:\voxelbit\v3\external"
if not exist "%DEPS%\falcor\Source" (
  echo v4: no dependencies at %DEPS%.
  echo           Set DEPS to a tree holding falcor\ and slang\.
  exit /b 1
)

if "%VCVARS%"=="" set "VCVARS=C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
call "%VCVARS%" >nul 2>nul


cl /nologo /std:c++20 /EHsc /MD /bigobj /Zc:__cplusplus ^
   /D_USE_MATH_DEFINES /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DOPENVDB_STATICLIB=1 ^
   /wd4275 /wd4251 /wd4146 ^
   /I "%HERE%\src" ^
   /I C:\vcpkg\installed\x64-windows-static-md\include ^
   /I "%DEPS%\falcor\Source" ^
   /I "%DEPS%\falcor\Source\Falcor" ^
   /I "%DEPS%\falcor\external\include" ^
   /I "%DEPS%\falcor\external\fmt\include" ^
   /I "%DEPS%\slang\include" ^
   /I "%DEPS%\falcor\external\imgui" ^
   /Zs "%HERE%\src\main.cpp"
if errorlevel 1 (
  echo.
  echo v4: headers do NOT type-check.
  exit /b 1
)
echo v4: headers type-check.
exit /b 0
