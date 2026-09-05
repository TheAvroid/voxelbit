@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
if "%CUDA_PATH%"=="" set "CUDA_PATH=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v13.3"
cd /d "%~dp0"
if not exist out mkdir out
cl /nologo /O2 /std:c++17 /EHsc /MT /D_CRT_SECURE_NO_WARNINGS ^
   /I"..\..\external\glfw\include" /I"%CUDA_PATH%\include" ^
   /Fo"out\\" /Fe"out\rows_test.exe" rows_test.cpp ^
   /link "..\..\build\glfw3.lib" gdi32.lib user32.lib shell32.lib opengl32.lib || exit /b 1
