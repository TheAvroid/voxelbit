@echo off
REM ---------------------------------------------------------------------------
REM v6.bat -- run the v6 engine: an endless voxel pine forest on NVIDIA Falcor,
REM           through a four-phase ray tracing pipeline.
REM
REM Same wood as v2 and v4 -- same seed, same terrain, same nine pines, same
REM person walking through it. What is new is everything between the ray and
REM the pixel:
REM
REM   PHASE A  SHADING       ReSTIR direct lighting (NVIDIA RTXDI) over the sun
REM                          and sky, and multi-bounce indirect from irradiance
REM                          probes (NVIDIA RTXGI 1.3 DDGI).
REM   PHASE B  DEMODULATION  the surface texture is divided OUT of the lighting
REM                          before anything filters it, so a denoiser never
REM                          gets the chance to average a pine needle away.
REM   PHASE C  RECONSTRUCT   DLSS Ray Reconstruction. NRD runs only on signals
REM                          RR does not take -- ambient occlusion. No buffer is
REM                          ever denoised twice.
REM   PHASE D  UPSCALE       DLSS, then the texture is multiplied back over the
REM                          clean lighting at output resolution.
REM
REM Double-click this, or run it from a prompt. Any arguments are passed
REM straight through to v6.exe:
REM
REM   v6.bat                              walk around in the wood
REM   v6.bat --seed 7 --density 0.8       a different, thicker wood
REM   v6.bat --no-ddgi --no-restir        the v4 pipeline, for comparison
REM   v6.bat --pipeline                   print which phases came up, then exit
REM   v6.bat --check-demod                prove the demodulation is lossless
REM   v6.bat --fg off                     turn frame generation OFF
REM   v6.bat --fg 3x                      3x/4x -- RTX 50-series only
REM   v6.bat --out shot.png --spp 256     render one frame offline and exit
REM   v6.bat --background                 open minimised, never take focus
REM   v6.bat --help                       every option
REM
REM FRAME GENERATION IS ON (2x) BY DEFAULT, AND SETS ITSELF UP. This script puts
REM the Streamline interposer under slang-gfx first, because DLSS-G can only
REM insert a frame into a swapchain Streamline owns -- and Falcor builds its
REM swapchain through a prebuilt slang-gfx that loads d3d12 and dxgi by name.
REM The setup rewrites those two names so it loads Streamline instead.
REM
REM It is idempotent, it keeps a byte-for-byte backup of gfx.dll, and it is
REM undone with:
REM
REM     python v6\patch_gfx_interposer.py --restore
REM
REM Frame generation is RTX 40-series and up, and 3x/4x need a 50-series.
REM On anything older v6 says so at startup and runs without it.
REM --pipeline prints exactly what came up on this machine.
REM
REM controls:  W A S D walk, shift sprints, space jumps, F toggles fly.
REM            Y opens the SETTINGS MENU -- every phase above can be switched
REM            on and off in it while you watch. Its last row bakes the current
REM            settings as the new defaults; v6\rebuild.bat then applies them.
REM            left click captures the mouse, right-drag looks, scroll zooms,
REM            P screenshots, F1 prints the controls,
REM            arrow keys scrub the day/night clock (up/down = fast),
REM            X + scroll wheel sets the cycle speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS,
REM            ESC releases the mouse -- ESC again quits.
REM
REM EVERY PHASE FAILS SOFT. Each of the four asks the driver whether it can
REM actually run before it runs, and any that cannot is skipped with a line
REM saying why -- the engine still opens, still walks, still draws the wood.
REM A missing SDK at BUILD time does the same thing: v6 compiles without it.
REM
REM The engine lives in this repo, so the path is relative to this file rather
REM than hardcoded. Override it by setting V6_HOME first.
REM ---------------------------------------------------------------------------
setlocal

if "%V6_HOME%"=="" set "V6_HOME=%~dp0v6"
set "EXE=%V6_HOME%\build\bin\Release\v6.exe"

if not exist "%EXE%" (
  echo v6: %EXE% not found.
  echo.
  echo     Build it first:  double-click %V6_HOME%\rebuild.bat
  echo     ^(needs Visual Studio with "Desktop development with C++", and a
  echo      Falcor checkout -- see %V6_HOME%\README.md^)
  echo.
  pause
  exit /b 1
)

REM Falcor loads its shaders from disk beside the exe rather than embedding
REM them, so v6's have to be there. Missing them is the one failure that would
REM otherwise surface as an unhelpful error deep in startup.
if not exist "%V6_HOME%\build\bin\Release\shaders\v6\shaders\Trace.cs.slang" (
  echo v6: the shaders are missing from build\bin\Release\shaders\v6.
  echo     Rebuild with %V6_HOME%\rebuild.bat
  echo.
  pause
  exit /b 1
)

REM The pine and decoration models -- the SAME assets v2 and v4 draw, read from
REM the same folder, so a seed means the same wood in all three engines. The
REM engine defaults to these paths already; passing them explicitly means a
REM v6.exe built elsewhere still finds them.
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
if not exist "%PINES%\pine_1.vox" (
  echo v6: no pine_1.vox under %PINES% -- falling back to the engine default.
  set "PINES="
)

REM Nothing has to be added to PATH: Falcor.dll, slang, the D3D12 Agility
REM runtime, dxcompiler (which compiles RTXGI's probe shaders at startup) and
REM nvngx_dlssd.dll all sit beside the exe, and the exe is launched by full path.

REM --- frame generation setup ----------------------------------------------
REM
REM FRAME GENERATION IS ON BY DEFAULT NOW, so this can no longer wait for
REM --fg to appear on the command line. It still runs at most once, and the
REM guard is the PRESENCE OF THE SHIMS rather than a remembered flag --
REM Falcor re-deploys gfx.dll on every build and silently reverts the patch,
REM so "it was set up last time" is not evidence that it is set up now.
set "FGOFF="
echo %* | findstr /I /C:"--fg off" >nul && set "FGOFF=1"
if not defined FGOFF (
  if not exist "%V6_HOME%\build\bin\Release\slgi.dll" (
    echo v6: setting up frame generation ^(one off^) ...
    REM The forwarder needs MSVC, so it is only built if it is missing.
    if not exist "%V6_HOME%\build\slgi\slgi.dll" call "%V6_HOME%\build_slgi_forwarder.bat"
    python "%V6_HOME%\patch_gfx_interposer.py"
    if errorlevel 1 (
      echo.
      echo v6: could not set up frame generation. Everything else still runs;
      echo     --fg will simply report itself unavailable.
      echo.
    )
  )
)

REM Run from THIS folder, not the build folder, so screenshots (v6_shot_NNN.png)
REM and any --out image land next to the launcher where they can be found.
if "%PINES%"=="" (
  "%EXE%" %*
) else (
  "%EXE%" --pines "%PINES%" --decor "%DECOR%" %*
)
set "RC=%ERRORLEVEL%"

REM Only hold the window open on failure -- a clean exit should just close.
if not "%RC%"=="0" (
  echo.
  echo v6 exited with code %RC%.
  pause
)
exit /b %RC%
