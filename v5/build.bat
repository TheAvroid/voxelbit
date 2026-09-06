@echo off
REM ---------------------------------------------------------------------------
REM build.bat -- build v5.
REM
REM v5 is a Rust program, so this is mostly `cargo build --release`. What it
REM adds is the one step cargo cannot do: copying the NGX runtime DLLs next to
REM the exe.
REM
REM THAT STEP IS NOT OPTIONAL AND ITS ABSENCE IS SILENT. dlss_wgpu links the
REM NGX static library at build time, but the DLSS models themselves load at
REM RUNTIME out of nvngx_dlss.dll and nvngx_dlssd.dll, and NGX looks for them
REM beside the executable. Without them everything still builds, links and
REM runs -- and reports "DLSS is not supported on this system" on a machine
REM where it is perfectly well supported, because the check it fails is
REM "did the DLL load", not "is there an RTX card here".
REM
REM The three environment variables the build needs (DLSS_SDK, VULKAN_SDK,
REM LIBCLANG_PATH) are in .cargo/config.toml, so nothing has to be set up in
REM the shell first. They are build-time only; the exe needs none of them.
REM
REM Usage:  build.bat          normal build
REM         build.bat clean    discard target/
REM ---------------------------------------------------------------------------
setlocal

set "HERE=%~dp0"
if "%HERE:~-1%"=="\" set "HERE=%HERE:~0,-1%"

if /i "%~1"=="clean" (
  echo cleaning %HERE%\target
  if exist "%HERE%\target" rmdir /s /q "%HERE%\target"
  echo done.
  exit /b 0
)

REM cargo is installed per-user by rustup and is not always on a fresh PATH.
where cargo >nul 2>nul
if errorlevel 1 (
  if exist "%USERPROFILE%\.cargo\bin\cargo.exe" (
    set "PATH=%USERPROFILE%\.cargo\bin;%PATH%"
  ) else (
    echo build: cargo not found. Install Rust from https://rustup.rs
    exit /b 1
  )
)

if "%DLSS_SDK%"=="" set "DLSS_SDK=C:\Users\mrwbh\DLSS"

pushd "%HERE%"
cargo build --release
if errorlevel 1 (
  popd
  echo build: cargo failed.
  exit /b 1
)
popd

REM --- the NGX runtime, beside the exe ----------------------------------------
REM The `rel` build, not `dev`: the dev DLLs carry an on-screen watermark.
set "NGX=%DLSS_SDK%\lib\Windows_x86_64\rel"
for %%F in (nvngx_dlss.dll nvngx_dlssd.dll) do (
  if exist "%NGX%\%%F" (
    copy /y "%NGX%\%%F" "%HERE%\target\release\%%F" >nul
  ) else (
    echo build: WARNING -- %NGX%\%%F is missing.
    echo        v5 will run, but DLSS will report itself unsupported.
  )
)

echo built %HERE%\target\release\v5.exe
exit /b 0
