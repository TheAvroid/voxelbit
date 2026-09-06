@echo off
REM ---------------------------------------------------------------------------
REM compile_ddgi_shaders.bat -- RTXGI's eight probe shaders, to DXIL.
REM
REM WHY AT BUILD TIME AND NOT AT STARTUP. RTXGI ships its probe shaders as HLSL
REM source and every integration NVIDIA publishes compiles them at runtime with
REM dxcompiler.dll. That costs a hundred-odd milliseconds of startup, drags a
REM second shader compiler into the process beside Slang, and turns a typo in a
REM define into a runtime failure on someone else's machine. Compiling them here
REM makes all three go away: the engine loads eight .cso files and hands the
REM bytes to the SDK, and a bad define is a BUILD error.
REM
REM THE DEFINES ARE NOT OPTIONAL AND NOT GUESSABLE. They have to match the
REM DDGIVolumeDesc the host fills in exactly -- the texel counts and the ray
REM count are compiled INTO the shaders as loop bounds and groupshared array
REM sizes. Disagree with the host and the SDK reads past the end of a
REM groupshared array. So both sides read the same numbers: these are mirrored
REM by the static_asserts in src/gpu/ddgi.h, which fail the build if they drift.
REM
REM UNMANAGED Resource Mode is why the REGS list below exists at all. v2 creates
REM the probe textures itself -- so that Falcor owns them and its barrier
REM tracking stays correct across v2 own Slang probe passes -- and the price is
REM that these shaders no longer inherit the SDK own registers and have to be
REM told where every binding lives. They must agree with the root signature
REM GetDDGIVolumeRootSignatureDesc() hands back; see ddgi.h.
REM ---------------------------------------------------------------------------
setlocal enabledelayedexpansion

if "%RTXGI_SRC%"=="" set "RTXGI_SRC=C:\Users\mrwbh\RTXGI-DDGI"
if "%FALCOR_DIR%"=="" set "FALCOR_DIR=C:\Users\mrwbh\Falcor"
set "DXC=%FALCOR_DIR%\external\packman\dxcompiler\bin\x64\dxc.exe"
set "SDK=%RTXGI_SRC%\rtxgi-sdk"
set "OUT=%~1"
if "%OUT%"=="" set "OUT=%~dp0build\bin\Release\shaders\v2\ddgi"

if not exist "%DXC%" ( echo compile_ddgi: no dxc.exe at %DXC% & exit /b 1 )
if not exist "%SDK%\shaders\ddgi\ProbeBlendingCS.hlsl" (
  echo compile_ddgi: no RTXGI DDGI shaders at %SDK%
  exit /b 1
)
if not exist "%OUT%" mkdir "%OUT%"

REM -- the volume's shape, mirrored by kProbeNumRays and friends in ddgi.h ------
set "NUM_RAYS=192"
set "IRR_TEXELS=8"
set "IRR_INTERIOR=6"
set "DIST_TEXELS=16"
set "DIST_INTERIOR=14"
set "WAVE_LANES=32"

REM RTXGI_COORDINATE_SYSTEM=2 is RIGHT HAND, Y-UP, and it is v2's world rather
REM than a preference: the terrain is a height field in Y and the camera basis
REM is (right, up, forward). Anything else silently transposes the probe grid.
set "COMMON=-D HLSL=1 -D RTXGI_DDGI_RESOURCE_MANAGEMENT=0 -D RTXGI_BINDLESS_TYPE=0 -D RTXGI_COORDINATE_SYSTEM=2 -D RTXGI_DDGI_SHADER_REFLECTION=0 -D RTXGI_DDGI_BINDLESS_RESOURCES=0 -D RTXGI_DDGI_DEBUG_PROBE_INDEXING=0 -D RTXGI_DDGI_DEBUG_OCTAHEDRAL_INDEXING=0 -D RTXGI_DDGI_DEBUG_BORDER_COPY_INDEXING=0"
REM UNMANAGED MODE means the shaders no longer inherit the SDK's own resource
REM registers -- they have to be told where every binding lives, and these have
REM to match the root signature GetDDGIVolumeRootSignatureDesc() hands back.
REM OUTPUT_REGISTER is the one that differs per shader (irradiance u1, distance
REM u2) and is set at the call site rather than here.
set "REGS=-D CONSTS_REGISTER=b0 -D CONSTS_SPACE=space1 -D VOLUME_CONSTS_REGISTER=t0 -D VOLUME_CONSTS_SPACE=space1 -D RAY_DATA_REGISTER=u0 -D RAY_DATA_SPACE=space1 -D OUTPUT_SPACE=space1 -D PROBE_DATA_REGISTER=u3 -D PROBE_DATA_SPACE=space1 -D PROBE_VARIABILITY_SPACE=space1 -D PROBE_VARIABILITY_REGISTER=u4 -D PROBE_VARIABILITY_AVERAGE_REGISTER=u5"
set "INCS=-I "%SDK%\include" -I "%SDK%\shaders""

REM Probe blending needs the ray count baked in because it stages the whole
REM probe's rays in groupshared memory -- that is what BLEND_SHARED_MEMORY buys,
REM and it is the single biggest win in the blend pass.
set "BLEND=-D RTXGI_DDGI_BLEND_SHARED_MEMORY=1 -D RTXGI_DDGI_BLEND_RAYS_PER_PROBE=%NUM_RAYS% -D RTXGI_DDGI_BLEND_SCROLL_SHARED_MEMORY=0"

call :one ProbeBlendingCS.hlsl DDGIProbeBlendingCS blend_irradiance.cso ^
     "%BLEND% -D RTXGI_DDGI_BLEND_RADIANCE=1 -D RTXGI_DDGI_PROBE_NUM_TEXELS=%IRR_TEXELS% -D RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=%IRR_INTERIOR% -D OUTPUT_REGISTER=u1"
if errorlevel 1 exit /b 1

call :one ProbeBlendingCS.hlsl DDGIProbeBlendingCS blend_distance.cso ^
     "%BLEND% -D RTXGI_DDGI_BLEND_RADIANCE=0 -D RTXGI_DDGI_PROBE_NUM_TEXELS=%DIST_TEXELS% -D RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=%DIST_INTERIOR% -D OUTPUT_REGISTER=u2"
if errorlevel 1 exit /b 1

call :one ProbeRelocationCS.hlsl DDGIProbeRelocationCS relocation_update.cso ""
if errorlevel 1 exit /b 1
call :one ProbeRelocationCS.hlsl DDGIProbeRelocationResetCS relocation_reset.cso ""
if errorlevel 1 exit /b 1

call :one ProbeClassificationCS.hlsl DDGIProbeClassificationCS classification_update.cso ""
if errorlevel 1 exit /b 1
call :one ProbeClassificationCS.hlsl DDGIProbeClassificationResetCS classification_reset.cso ""
if errorlevel 1 exit /b 1

REM The reduction is a wave-intrinsic tree, so it has to be told the lane width
REM it will run on. 32 on every NVIDIA part.
set "REDUCE=-D RTXGI_DDGI_PROBE_NUM_INTERIOR_TEXELS=%IRR_INTERIOR% -D RTXGI_DDGI_WAVE_LANE_COUNT=%WAVE_LANES%"
call :one ReductionCS.hlsl DDGIReductionCS reduction.cso "%REDUCE%"
if errorlevel 1 exit /b 1
call :one ReductionCS.hlsl DDGIExtraReductionCS reduction_extra.cso "%REDUCE%"
if errorlevel 1 exit /b 1

echo   ddgi     8 probe shaders -^> %OUT%
exit /b 0

:one
REM %1 file  %2 entry point  %3 output  %4 extra defines
"%DXC%" -T cs_6_6 -E %2 %COMMON% %REGS% %~4 %INCS% -Fo "%OUT%\%3" "%SDK%\shaders\ddgi\%1"
if errorlevel 1 (
  echo compile_ddgi: FAILED %1 :: %2
  exit /b 1
)
exit /b 0
