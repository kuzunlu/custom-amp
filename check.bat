@echo off
rem Runs every check: the Python tests, the C++ build, the plugin test and C++ vs Python (about 11 minutes).
rem Run it before pushing: GitHub only builds release tags (to save Actions minutes).
rem Full paths everywhere: some shells (NoDefaultCurrentDirectoryInExePath) do not run programs from the current folder.
setlocal
cd /d "%~dp0"
set PY=C:\Python314\python.exe
if not exist "%PY%" set PY=python

echo === Python tests
"%PY%" -m unittest discover -s tests -t . || exit /b 1

echo === Build
call "%~dp0build.bat" || exit /b 1

echo === Plugin test
"%~dp0build\PluginTest_artefacts\Release\PluginTest.exe" || exit /b 1

echo === C++ vs Python
"%PY%" tools\cpp_check.py || exit /b 1

echo === All checks passed
