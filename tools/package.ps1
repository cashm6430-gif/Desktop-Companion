<#
.SYNOPSIS
  Stage and package Desktop-Companion into a portable zip (and, when Inno Setup
  is installed, a Setup.exe).

.DESCRIPTION
  Stages a clean runtime folder from the already-deployed build tree -- the exes,
  the Qt DLLs and platform plugin windeployqt dropped there, the Cubism core and
  shaders, and the assets -- drops the self-contained hook installer beside it,
  then zips it. If ISCC.exe is found the Inno Setup script
  (installer\DesktopCompanion.iss) is compiled against the same folder.

  The staged folder is what both flavours ship: the zip is the portable build,
  and the .iss packages the same tree as the installer.
#>
param(
    [string]$Version = '0.1.0',
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$OutDir = '',
    [string]$SevenZip = 'D:\tool\7-Zip\7z.exe',
    [switch]$SkipZip,
    [switch]$SkipSetup
)

$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'build\package' }
$build = Join-Path $RepoRoot 'build'
$stage = Join-Path $OutDir 'DesktopCompanion'

$exe = Join-Path $build 'DesktopCompanion.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: $exe is missing." }

Write-Host "Staging into $stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# The build dir is already a runnable deployment (windeployqt ran during the
# build): take the two exes and the Qt/Cubism DLLs sitting next to them.
Copy-Item -LiteralPath (Join-Path $build 'DesktopCompanion.exe') -Destination $stage
Copy-Item -LiteralPath (Join-Path $build 'DesktopCompanionHook.exe') -Destination $stage
Get-ChildItem -LiteralPath $build -Filter '*.dll' |
    Where-Object { $_.Name -ne 'Qt6Test.dll' } |   # test-only, never shipped
    Copy-Item -Destination $stage
foreach ($dir in @('platforms', 'FrameworkShaders')) {
    $src = Join-Path $build $dir
    if (-not (Test-Path -LiteralPath $src)) { throw "Missing runtime folder: $src" }
    Copy-Item -LiteralPath $src -Destination $stage -Recurse
}
Copy-Item -LiteralPath (Join-Path $RepoRoot 'assets') -Destination $stage -Recurse

# Self-contained hook installers + one-click wrappers (portable flavour). The
# Cowork seeding script ships too: the pet does that seeding in-process, but this is
# the documented manual fallback, and the uninstaller needs it to undo the seeding.
New-Item -ItemType Directory -Path (Join-Path $stage 'tools') -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\install-hooks.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\seed-cowork-hooks.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'installer\portable\install-hooks.cmd') -Destination $stage
Copy-Item -LiteralPath (Join-Path $RepoRoot 'installer\portable\uninstall-hooks.cmd') -Destination $stage

if (-not $SkipZip) {
    if (-not (Test-Path -LiteralPath $SevenZip)) { Write-Warning "7-Zip not found ($SevenZip); skipping zip." }
    else {
        $zip = Join-Path $OutDir "DesktopCompanion-$Version-portable.zip"
        if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
        Push-Location $OutDir
        try { & $SevenZip a -tzip -mx=9 $zip 'DesktopCompanion' | Out-Null }
        finally { Pop-Location }
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed ($LASTEXITCODE)" }
        Write-Host "Portable zip: $zip"
    }
}

if (-not $SkipSetup) {
    $iscc = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { $_ -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (-not $iscc) {
        Write-Warning 'Inno Setup 6 (ISCC.exe) not found; skipping Setup.exe. Install it from jrsoftware.org and re-run.'
    } else {
        $iss = Join-Path $RepoRoot 'installer\DesktopCompanion.iss'
        & $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutDir=$OutDir" $iss
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
        Write-Host "Setup: $(Join-Path $OutDir "DesktopCompanion-$Version-setup.exe")"
    }
}

Write-Host "Done. Staged folder: $stage"
