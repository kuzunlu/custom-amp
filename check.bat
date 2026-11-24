@echo off
rem Runs every check: the C++ build, the engine checks (theory, settings.json, every preset against its reference
rem recording) and the plugin test (about 5 minutes).
rem Run it before pushing: GitHub only builds release tags (to save Actions minutes).
rem Full paths everywhere: some shells (NoDefaultCurrentDirectoryInExePath) do not run programs from the current folder.
setlocal
cd /d "%~dp0"

echo === Build
call "%~dp0build.bat" || exit /b 1

echo === Engine checks
"%~dp0build\EngineTest_artefacts\Release\EngineTest.exe" check "%~dp0." || exit /b 1

echo === Plugin test
"%~dp0build\PluginTest_artefacts\Release\PluginTest.exe" || exit /b 1

echo === All checks passed
