@echo off
REM ---------------------------------------------------------------------------
REM v7.bat -- run the v7 engine: the v6 wood, with RTX neural shading under it.
REM
REM Same wood as v2, v4 and v6 -- same seed, same terrain, same nine pines. What
REM is new is that the shaders can run a NEURAL NETWORK, on the tensor path,
REM inside the trace.
REM
REM   NEURAL SHADERS   cooperative vectors: a matrix multiply issued as one
REM                    instruction across a subgroup. v7 ships its own Slang
REM                    2026.13 to get them -- Falcor's 2024.1.34 has no such
REM                    type -- and its own Falcor fork so that v6, which shares
REM                    the checkout, is untouched.
REM   RADIANCE CACHE   a small MLP that learns this wood's indirect light and
REM                    lets a path stop early and ask instead of tracing on.
REM                    Trained ONLINE, while you walk, from the renderer it is
REM                    accelerating. --nrc
REM   GOVERNOR         states a frame rate as a SETTING and spends quality to
REM                    hold it. In the settings menu, off by default.
REM
REM ---------------------------------------------------------------------------
REM YOU HAVE TO CHOOSE A BACKEND, AND IT IS A REAL TRADE. Read this once.
REM
REM   v7.bat              D3D12.  DLSS Ray Reconstruction, Frame Generation,
REM                       Reflex and the DDGI probes all work. NO neural
REM                       shaders: cooperative vectors need Shader Model 6.10
REM                       and this stack reports 6.8.
REM
REM   v7.bat --vulkan     Vulkan. Neural shaders and the radiance cache work --
REM                       the driver exposes VK_NV_cooperative_vector and slang
REM                       compiles straight to it. NO DLSS: the Ray
REM                       Reconstruction, Streamline and RTXGI integrations in
REM                       this engine are all written against D3D12.
REM
REM Without Ray Reconstruction the Vulkan picture is a ONE-SAMPLE PATH TRACE and
REM looks like it -- that is the missing denoiser, not a broken cache.
REM
REM BOTH AT ONCE is possible and is not the default. It needs the preview D3D12
REM stack -- Agility 1.721.2-preview and DXC 1.10 -- which v7 has staged but
REM does not use unless asked:
REM
REM     v7\build.bat preview
REM
REM That requires Windows Developer Mode AND A REBOOT AFTER TURNING IT ON: the
REM D3D12 runtime latches the setting at boot, and without it no D3D12 device
REM can be created at all. Plain "v7\build.bat" goes back to the stock runtime.
REM
REM ---------------------------------------------------------------------------
REM Double-click this, or run it from a prompt. Any arguments are passed
REM straight through to v7.exe:
REM
REM   v7.bat                              walk around in the wood
REM   v7.bat --vulkan --nrc               neural shading, with the radiance cache
REM   v7.bat --seed 7 --density 0.8       a different, thicker wood
REM   v7.bat --vulkan --nrc --rr 16 --depth 16
REM                                       where the cache actually PAYS -- see
REM                                       the note below
REM   v7.bat --out shot.png --spp 256     render one frame offline and exit
REM   v7.bat --background                 open minimised, never take focus
REM   v7.bat --help                       every option
REM
REM WHAT THE CACHE IS AND IS NOT WORTH, measured on this machine rather than
REM assumed. At the engine's defaults it is about 5% SLOWER: Russian roulette
REM already terminates most paths after two or three bounces, so there is no
REM tail left to replace and only the query cost remains. Give it paths that
REM genuinely run long -- "--rr 16 --depth 16" -- and it is 28% FASTER
REM (59.7 fps against 46.5). It is a real capability with a real precondition,
REM not a free speedup, and on this card the matmuls run the DP4a path rather
REM than Blackwell's in-shader tensor cores.
REM
REM controls:  W A S D walk, shift sprints, space jumps, F toggles fly.
REM            Y opens the SETTINGS MENU -- the cache, the governor and every
REM            other trade can be switched while you watch. Its last row bakes
REM            the current settings as the new defaults; v7\rebuild.bat applies
REM            them.
REM            left click captures the mouse, right-drag looks, scroll zooms,
REM            P screenshots, F1 prints the controls,
REM            arrow keys scrub the day/night clock (up/down = fast),
REM            X + scroll wheel sets the cycle speed -- scroll DOWN past the
REM            slowest notch to run time BACKWARDS,
REM            ESC releases the mouse -- ESC again quits.
REM
REM EVERYTHING FAILS SOFT. Every optional piece asks the device whether it can
REM actually run before it runs, and any that cannot is skipped with a line at
REM startup saying why. The engine still opens, still walks, still draws.
REM
REM The engine lives in this repo, so the path is relative to this file rather
REM than hardcoded. Override it by setting V7_HOME first.
REM ---------------------------------------------------------------------------
setlocal

REM ---------------------------------------------------------------------------
REM KEEP THE CONSOLE OUT OF THE WAY.
REM
REM Double-clicking a .bat ALWAYS creates a console -- Windows makes the window
REM before cmd has read a single line of this file -- so it cannot be prevented,
REM only put somewhere. This relaunches the script once, minimised, and lets the
REM original window exit: what is left is one minimised console holding the
REM engine's log, sitting in the taskbar instead of on top of the game.
REM
REM START, AND NOT A BARE CALL. A child process that inherits this console is
REM killed when the console closes, which is exactly what the next line does.
REM
REM Set V7_CONSOLE=1 before running to keep the old visible window. The
REM relaunched copy sets it for itself, and that is what stops this recursing.
REM
REM ON FAILURE THE WINDOW IS STILL THERE -- minimised, holding the pause at the
REM bottom of this file. A launch that dies is a taskbar button to click, not a
REM window that vanished.
REM
REM KEEP THIS FILE ASCII AND LF. cmd re-seeks a batch file by BYTE offset
REM between commands, so a non-ASCII character anywhere above desynchronises the
REM parser and it starts executing fragments of these comments as commands.
REM ---------------------------------------------------------------------------
if "%V7_CONSOLE%"=="" (
  set "V7_CONSOLE=1"
  start "v7" /min cmd /c ""%~f0" %*"
  exit /b 0
)

if "%V7_HOME%"=="" set "V7_HOME=%~dp0v7"
set "EXE=%V7_HOME%\build\bin\Release\v7.exe"

if not exist "%EXE%" (
  echo v7: %EXE% not found.
  echo.
  echo     Build it first:  double-click %V7_HOME%\rebuild.bat
  echo     ^(needs Visual Studio with "Desktop development with C++"; the
  echo      Falcor fork and Slang are already inside %V7_HOME%\external^)
  echo.
  pause
  exit /b 1
)

REM Falcor loads its shaders from disk beside the exe rather than embedding
REM them, so v7's have to be there. Missing them is the one failure that would
REM otherwise surface as an unhelpful error deep in startup.
if not exist "%V7_HOME%\build\bin\Release\shaders\v7\shaders\Trace.cs.slang" (
  echo v7: the shaders are missing from build\bin\Release\shaders\v7.
  echo     Rebuild with %V7_HOME%\rebuild.bat
  echo.
  pause
  exit /b 1
)

REM The pine and decoration models -- the SAME assets v2, v4 and v6 draw, read
REM from the same folder, so a seed means the same wood in every engine. The
REM engine defaults to these paths already; passing them explicitly means a
REM v7.exe built elsewhere still finds them.
set "PINES=%~dp0game\assets\foilage\pine9"
set "DECOR=%~dp0game\assets\decoration"
if not exist "%PINES%\pine_1.vox" (
  echo v7: no pine_1.vox under %PINES% -- falling back to the engine default.
  set "PINES="
)

REM Nothing has to be added to PATH: Falcor.dll, v7's own slang, the D3D12
REM Agility runtime, dxcompiler and nvngx_dlssd.dll all sit beside the exe, and
REM the exe is launched by full path.

REM Run from THIS folder, not the build folder, so screenshots (v7_shot_NNN.png)
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
  echo v7 exited with code %RC%.
  pause
)
exit /b %RC%
