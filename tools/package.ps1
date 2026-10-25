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
    [ValidatePattern('^\d+\.\d+\.\d+(?:\.[0-9]+)?(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?$')]
    [string]$Version = '',
    [string]$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path,
    [string]$OutDir = '',
    [string]$SevenZip = 'D:\tool\7-Zip\7z.exe',
    [string]$VCRuntimeDir = '',
    [switch]$SkipZip,
    [switch]$SkipSetup
)

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path -LiteralPath $RepoRoot).Path
if (-not $OutDir) { $OutDir = Join-Path $RepoRoot 'build\package' }
$OutDir = [IO.Path]::GetFullPath($OutDir)
$build = Join-Path $RepoRoot 'build'
$stage = Join-Path $OutDir 'DesktopCompanion'

$exe = Join-Path $build 'DesktopCompanion.exe'
if (-not (Test-Path -LiteralPath $exe)) { throw "Build first: $exe is missing." }
$binaryVersion = (Get-Item -LiteralPath $exe).VersionInfo.ProductVersion
if (-not $Version) {
    if ($binaryVersion -notmatch '^\d+\.\d+\.\d+(?:\.\d+)?$') {
        throw 'No numeric executable ProductVersion; pass -Version explicitly.'
    }
    $Version = if ($binaryVersion.Split('.').Count -eq 4 -and $binaryVersion.EndsWith('.0')) {
        $binaryVersion.Substring(0, $binaryVersion.Length - 2)
    } else { $binaryVersion }
}

# Packaging never deletes an existing tree. A fresh output directory also keeps
# a failed candidate from being mistaken for a previously verified release.
if (Test-Path -LiteralPath $stage) { throw "Stage already exists; choose a fresh -OutDir: $stage" }
$zip = Join-Path $OutDir "DesktopCompanion-$Version-portable.zip"
$setup = Join-Path $OutDir "DesktopCompanion-$Version-setup.exe"
foreach ($target in @($zip, $setup)) {
    if (Test-Path -LiteralPath $target) { throw "Output already exists: $target" }
}

# An explicit runtime list prevents test executables and stray build DLLs from
# entering the package. Deployment and package contents are separate contracts.
$runtimeFiles = @(
    'DesktopCompanion.exe', 'DesktopCompanionHook.exe',
    'Qt6Core.dll', 'Qt6Gui.dll', 'Qt6Network.dll', 'Qt6OpenGL.dll',
    'Qt6OpenGLWidgets.dll', 'Qt6Svg.dll', 'Qt6Widgets.dll',
    'Live2DCubismCore.dll', 'd3dcompiler_47.dll', 'dxcompiler.dll', 'dxil.dll',
    'platforms\qwindows.dll'
)
foreach ($runtimeFile in $runtimeFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $runtimeFile) -PathType Leaf)) {
        throw "Missing runtime file: $runtimeFile"
    }
}
foreach ($required in @('FrameworkShaders', 'assets')) {
    $requiredRoot = if ($required -eq 'assets') { $RepoRoot } else { $build }
    if (-not (Test-Path -LiteralPath (Join-Path $requiredRoot $required) -PathType Container)) {
        throw "Missing runtime directory: $required"
    }
}

# windeployqt may leave the compiler runtime to the machine's global redist.
# Ship release x64 CRT DLLs app-local so the portable package also starts on a
# machine with no Visual Studio or preinstalled VC++ redistributable.
if (-not $VCRuntimeDir) {
    $compilerLine = Get-Content -LiteralPath (Join-Path $build 'CMakeCache.txt') |
        Where-Object { $_ -match '^CMAKE_CXX_COMPILER:FILEPATH=' } | Select-Object -First 1
    if ($compilerLine -match '^CMAKE_CXX_COMPILER:FILEPATH=(.+[/\\]VC)[/\\]Tools[/\\]MSVC[/\\]([^/\\]+)') {
        $redistRoot = Join-Path $Matches[1] ('Redist\MSVC\' + $Matches[2] + '\x64')
        $VCRuntimeDir = Get-ChildItem -LiteralPath $redistRoot -Directory -Filter 'Microsoft.VC*.CRT' |
            Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $VCRuntimeDir) { throw 'Cannot locate x64 MSVC release CRT; pass -VCRuntimeDir explicitly.' }
$VCRuntimeDir = (Resolve-Path -LiteralPath $VCRuntimeDir).Path
$crtFiles = @('concrt140.dll', 'msvcp140.dll', 'msvcp140_1.dll', 'msvcp140_2.dll',
    'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vccorlib140.dll',
    'vcruntime140.dll', 'vcruntime140_1.dll', 'vcruntime140_threads.dll')
foreach ($crtFile in $crtFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $VCRuntimeDir $crtFile) -PathType Leaf)) {
        throw "Missing release CRT: $crtFile in $VCRuntimeDir"
    }
}

Write-Host "Staging into $stage"
New-Item -ItemType Directory -Path $stage -Force | Out-Null

# The build dir is already a runnable deployment (windeployqt ran during the
# build): take the two exes and the Qt/Cubism DLLs sitting next to them.
foreach ($runtimeFile in $runtimeFiles) {
    $destination = Join-Path $stage $runtimeFile
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $build $runtimeFile) -Destination $destination
}
foreach ($crtFile in $crtFiles) {
    Copy-Item -LiteralPath (Join-Path $VCRuntimeDir $crtFile) -Destination $stage
}
foreach ($dir in @('FrameworkShaders')) {
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
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\hook-config-common.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\install-opencode-plugin.ps1') -Destination (Join-Path $stage 'tools')
Copy-Item -LiteralPath (Join-Path $RepoRoot 'tools\opencode-plugin') -Destination (Join-Path $stage 'tools') -Recurse
Copy-Item -LiteralPath (Join-Path $RepoRoot 'installer\portable\install-hooks.cmd') -Destination $stage
Copy-Item -LiteralPath (Join-Path $RepoRoot 'installer\portable\uninstall-hooks.cmd') -Destination $stage

# Hash the exact bytes that both distributions receive. The manifest excludes
# itself, generated archives and machine-specific absolute source paths.
$sourceHead = & git -C $RepoRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source revision.' }
$sourceDirty = [bool](& git -C $RepoRoot status --porcelain --untracked-files=normal)
if ($LASTEXITCODE -ne 0) { throw 'Cannot read source status.' }
$files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [ordered]@{
        path = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        bytes = $_.Length
        sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
})
$manifest = [ordered]@{
    schema_version = 1
    package_version = $Version
    binary_product_version = $binaryVersion
    created_utc = [DateTime]::UtcNow.ToString('o')
    source = [ordered]@{ head = "$sourceHead"; dirty = $sourceDirty }
    files = $files
}
$manifestPath = Join-Path $stage 'package-manifest.json'
[IO.File]::WriteAllText($manifestPath, ($manifest | ConvertTo-Json -Depth 8) + [Environment]::NewLine,
    (New-Object Text.UTF8Encoding($false)))

if (-not $SkipZip) {
    if (-not (Test-Path -LiteralPath $SevenZip)) { throw "7-Zip not found: $SevenZip. Use -SkipZip for stage-only verification." }
    else {
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
        throw 'Inno Setup 6 (ISCC.exe) not found. Use -SkipSetup for portable-only packaging.'
    } else {
        $iss = Join-Path $RepoRoot 'installer\DesktopCompanion.iss'
        & $iscc "/DAppVersion=$Version" "/DStageDir=$stage" "/DOutDir=$OutDir" $iss
        if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
        Write-Host "Setup: $(Join-Path $OutDir "DesktopCompanion-$Version-setup.exe")"
    }
}

Write-Host "Done. Staged folder: $stage"
