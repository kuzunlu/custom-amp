# Makes a release on this PC, without GitHub Actions:
#
#   powershell -ExecutionPolicy Bypass -File release.ps1 -Version 0.2.0
#
# 1. Checks: main is clean, the same as GitHub's main, the tag is new, gh is logged in.
# 2. Tags v<Version> HERE first (the plugin shows the version git describe finds when it is built).
# 3. Runs check.bat (build, engine checks, plugin test). If anything fails, the tag is removed again.
# 4. Makes the installer (Inno Setup) and the two zips in build\release\.
# 5. Asks you, then pushes the tag and creates the GitHub release with the three files (gh).
#
# Needs Inno Setup 6 and the GitHub CLI (both installed with winget, pinned: JRSoftware.InnoSetup 6.7.3,
# GitHub.cli 2.106.0) and a one-time "gh auth login".

param([Parameter(Mandatory = $true)][string]$Version)

# Not "Stop": Windows PowerShell 5.1 would stop at the first line a program writes to stderr (gh and git do that
# even when all is well). Every step checks its exit code instead.
$ErrorActionPreference = "Continue"
Set-Location $PSScriptRoot
$tag = "v$Version"

function Stop-Release ([string]$why) {
    Write-Host "Not released: $why" -ForegroundColor Red
    exit 1
}

if ($Version -notmatch '^\d+\.\d+\.\d+$') { Stop-Release "the version must look like 0.2.0" }

$gh = (Get-Command gh -ErrorAction SilentlyContinue).Source
if (-not $gh) { $gh = "$env:ProgramFiles\GitHub CLI\gh.exe" }
if (-not (Test-Path $gh)) { Stop-Release "the GitHub CLI is not installed (winget install --id GitHub.cli --version 2.106.0 --exact)" }

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { Stop-Release "Inno Setup 6 is not installed (winget install --id JRSoftware.InnoSetup --version 6.7.3 --exact)" }

& $gh auth status *> $null
if ($LASTEXITCODE -ne 0) { Stop-Release "gh is not logged in: run 'gh auth login' once" }

if ((git rev-parse --abbrev-ref HEAD) -ne "main") { Stop-Release "you are not on main" }
if (git status --porcelain) { Stop-Release "there are uncommitted changes (git status)" }
git fetch --quiet origin
if ((git rev-parse HEAD) -ne (git rev-parse origin/main)) { Stop-Release "main is not the same as GitHub's main: push (or pull) first" }
if (git tag --list $tag) { Stop-Release "the tag $tag already exists here" }
if (git ls-remote --tags origin "refs/tags/$tag") { Stop-Release "the tag $tag already exists on GitHub" }

git tag $tag
Write-Host "Tagged $tag here (not pushed yet). Running every check..." -ForegroundColor Cyan
& cmd /c "`"$PSScriptRoot\check.bat`""
if ($LASTEXITCODE -ne 0) {
    git tag -d $tag | Out-Null
    Stop-Release "a check failed (the tag $tag was removed again)"
}

$out = Join-Path $PSScriptRoot "build\release"
if (Test-Path $out) { Remove-Item $out -Recurse -Force -ErrorAction Stop }
New-Item -ItemType Directory $out -ErrorAction Stop | Out-Null

& $iscc /Q "/DAppVersion=$Version" installer\CustomAmp.iss
if ($LASTEXITCODE -ne 0) { git tag -d $tag | Out-Null; Stop-Release "the installer did not build" }
$installer = Get-Item "build\installer\Custom-Amp-Setup-$Version.exe"
Copy-Item $installer.FullName $out
Compress-Archive -Path "build\CustomAmp_artefacts\Release\VST3\Custom Amp.vst3" -DestinationPath "$out\Custom-Amp-VST3-$tag.zip"
Compress-Archive -Path "build\CustomAmp_artefacts\Release\Standalone\Custom Amp.exe" -DestinationPath "$out\Custom-Amp-App-$tag.zip"
$files = Get-ChildItem $out | ForEach-Object { $_.FullName }

Write-Host ""
Write-Host "Ready to publish $tag with:" -ForegroundColor Cyan
Get-ChildItem $out | ForEach-Object { Write-Host ("  {0}  ({1:N1} MB)" -f $_.Name, ($_.Length / 1MB)) }
$answer = Read-Host "Push the tag and create the GitHub release now? (yes/no)"
if ($answer -ne "yes") {
    git tag -d $tag | Out-Null
    Stop-Release "you said no (the tag $tag was removed again; the files stay in build\release)"
}

$notes = @(
    "**Easiest:** run ``$($installer.Name)``, then rescan plugins in your DAW. It installs:"
    "- the VST3 plugin (Ableton lists it under **Kursat**, next to Stream Bridge),"
    "- the standalone app (Start menu: **Custom Amp**),"
    "- the presets as files you can edit, in ``Public Documents\Custom Amp\Presets``."
    ""
    "**Or by hand:** unzip ``Custom-Amp-VST3-$tag.zip`` and copy the ``Custom Amp.vst3`` folder to ``C:\Program Files\Common Files\VST3\``, then rescan plugins. ``Custom-Amp-App-$tag.zip`` has the standalone app."
    ""
    "Put **Custom Amp** on a guitar track. Set **Input** so hard strums peak around -6 dBFS. Needs 64-bit Windows."
) -join "`n"

git push origin $tag
if ($LASTEXITCODE -ne 0) { Stop-Release "the tag could not be pushed (it is still here: git tag -d $tag to remove it)" }
& $gh release create $tag @files --verify-tag --title "Custom Amp $tag" --notes $notes
if ($LASTEXITCODE -ne 0) { Stop-Release "gh could not create the release (the tag is on GitHub; try: gh release create $tag build\release\*)" }
Write-Host "Released $tag." -ForegroundColor Green
