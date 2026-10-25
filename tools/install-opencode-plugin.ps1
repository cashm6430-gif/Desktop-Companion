<#
.SYNOPSIS
  Install or remove the Desktop Companion OpenCode V2 server plugin.
.DESCRIPTION
  Uses OpenCode's global plugin discovery; never rewrites opencode.json(c).
  Only manifest-owned, unmodified files may be replaced or removed. Other
  files are retained. OpenCode reloads its watched plugin directory itself.
#>
param(
    [string]$HelperPath = '',
    [string]$TargetPath = (Join-Path $env:USERPROFILE '.config\opencode\plugins\desktop-companion'),
    [switch]$Remove,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hook-config-common.ps1')
if (-not $HelperPath) { $HelperPath = Get-DefaultPetHelperPath -ScriptDirectory $PSScriptRoot }
$fullTarget = [IO.Path]::GetFullPath($TargetPath)
if ($fullTarget -eq [IO.Path]::GetPathRoot($fullTarget)) { throw 'The plugin target cannot be a drive root.' }
$target = $fullTarget.TrimEnd('\', '/')
$markerName = '.desktop-companion-plugin.json'
$markerPath = Join-Path $target $markerName
$fileNames = @('index.ts', 'bridge.mjs', 'package.json', 'config.json')
$utf8 = New-Object Text.UTF8Encoding($false, $true)

function Assert-PlainPath([string]$Path, [bool]$Directory = $false) {
    if (-not (Test-Path -LiteralPath $Path)) { return }
    $item = Get-Item -LiteralPath $Path -Force
    if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0 -or
        $item.PSIsContainer -ne $Directory) { throw "Refusing non-regular plugin path: $Path" }
}
function Hash-Bytes([byte[]]$Bytes) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($Bytes))).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose() }
}
function Write-PluginBytes([string]$Path, [byte[]]$Bytes) {
    if ((Test-Path -LiteralPath $Path) -and
        (Hash-Bytes ([IO.File]::ReadAllBytes($Path))) -ceq (Hash-Bytes $Bytes)) { return }
    $temporary = Join-Path $target ('.pet-plugin-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllBytes($temporary, $Bytes)
        if (Test-Path -LiteralPath $Path) {
            # The suffix is neither .ts nor .js, so backups are never entrypoints.
            $backup = $Path + '.backup-' + [guid]::NewGuid().ToString('N')
            [IO.File]::Replace($temporary, $Path, $backup)
        } else { [IO.File]::Move($temporary, $Path) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
}

Assert-PlainPath $target $true
Assert-PlainPath $markerPath
$manifest = $null
if (Test-Path -LiteralPath $markerPath) {
    $raw = [IO.File]::ReadAllText($markerPath, $utf8)
    if (-not $raw.TrimStart().StartsWith('{')) { throw "Invalid plugin ownership manifest: $markerPath" }
    $manifest = ConvertFrom-Json -InputObject $raw -ErrorAction Stop
    if ($manifest.owner -cne 'DesktopCompanion' -or $manifest.version -ne 1 -or
        $manifest.files -isnot [pscustomobject] -or
        @($manifest.files.PSObject.Properties).Count -ne $fileNames.Count) {
        throw "Invalid plugin ownership manifest: $markerPath"
    }
    foreach ($name in $fileNames) {
        $entry = $manifest.files.PSObject.Properties[$name]
        if ($null -eq $entry -or $entry.Value -isnot [string] -or $entry.Value -cnotmatch '^[a-f0-9]{64}$') {
            throw "Invalid managed plugin entry: $name"
        }
        $path = Join-Path $target $name
        Assert-PlainPath $path
        if ((Test-Path -LiteralPath $path) -and
            (Hash-Bytes ([IO.File]::ReadAllBytes($path))) -cne $entry.Value) {
            throw "Managed plugin file was modified; preserving it: $path"
        }
    }
} elseif (-not $Remove -and (Test-Path -LiteralPath $target)) {
    if (@(Get-ChildItem -LiteralPath $target -Force).Count -gt 0) {
        throw "Plugin directory is not owned by DesktopCompanion: $target"
    }
}

if ($Remove) {
    if ($null -eq $manifest) { Write-Host "OpenCode: no owned plugin at $target"; return }
    if ($DryRun) { Write-Host "Remove (dry-run) OpenCode plugin -> $target"; return }
    foreach ($name in $fileNames) {
        $path = Join-Path $target $name
        if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
    }
    # Never recursively remove the directory: user files and backups survive.
    # Retain the inert ownership marker alongside them, so reinstall can safely
    # restore the missing managed files without adopting an unrelated folder.
    if (@(Get-ChildItem -LiteralPath $target -Force | Where-Object Name -ne $markerName).Count -eq 0) {
        Remove-Item -LiteralPath $markerPath -Force
        [IO.Directory]::Delete($target)
    }
    Write-Host "Removed OpenCode plugin from $target"
    return
}

$helper = (Resolve-Path -LiteralPath $HelperPath).Path
Assert-PlainPath $helper
$source = Join-Path $PSScriptRoot 'opencode-plugin'
$contents = [ordered]@{}
foreach ($name in @('index.ts', 'bridge.mjs', 'package.json')) {
    $sourcePath = Join-Path $source $name
    Assert-PlainPath $sourcePath
    $contents[$name] = [IO.File]::ReadAllBytes($sourcePath)
}
$contents['config.json'] = $utf8.GetBytes((([ordered]@{ helperPath = $helper } | ConvertTo-Json) + [Environment]::NewLine))
$hashes = [ordered]@{}
foreach ($name in $fileNames) { $hashes[$name] = Hash-Bytes $contents[$name] }
$newManifest = [ordered]@{ owner = 'DesktopCompanion'; version = 1; files = $hashes }
$contents[$markerName] = $utf8.GetBytes(($newManifest | ConvertTo-Json -Depth 5) + [Environment]::NewLine)
if ($DryRun) { Write-Host "Install (dry-run) OpenCode plugin -> $target; helper $helper"; return }
if (-not (Test-Path -LiteralPath $target)) { New-Item -ItemType Directory -Path $target -Force | Out-Null }
# OpenCode watches this directory. Publish the entrypoint only after its local
# dependencies and configuration exist, so first discovery cannot import an
# incomplete plugin. Each replacement is atomic; the directory is not a single
# filesystem transaction, and changed owned files retain their exact backups.
foreach ($name in @('config.json', 'package.json', 'bridge.mjs', $markerName, 'index.ts')) {
    Write-PluginBytes (Join-Path $target $name) $contents[$name]
}
Write-Host "Installed OpenCode V2 plugin in $target"
