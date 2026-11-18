@echo off
rem Builds the Custom Amp plugin and its test program in Release mode,
rem using the CMake and Ninja that come with Visual Studio.

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto noVisualStudio

set "VS="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS goto noVisualStudio

cd /d "%~dp0"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

set "VSCMAKE=%VS%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build build %* || exit /b 1

echo.
echo Plugin: %~dp0build\CustomAmp_artefacts\Release\VST3\Custom Amp.vst3
exit /b 0

:noVisualStudio
echo Visual Studio with C++ was not found.
echo Install "Build Tools for Visual Studio" with the "Desktop development with C++" workload.
exit /b 1
