param(
    [string]$TargetPath = (Join-Path $env:USERPROFILE '.codex\hooks.json')
)

$ErrorActionPreference = 'Stop'
$helper = (Resolve-Path (Join-Path $PSScriptRoot '..\build\DesktopCompanionHook.exe')).Path
$command = '"' + $helper + '"'
$events = @('UserPromptSubmit', 'Stop', 'Interrupt', 'SessionEnd')

if (Test-Path -LiteralPath $TargetPath) {
    $config = Get-Content -LiteralPath $TargetPath -Raw | ConvertFrom-Json -AsHashtable
    $backup = "$TargetPath.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
    Copy-Item -LiteralPath $TargetPath -Destination $backup
    Write-Host "Backup: $backup"
} else {
    $config = @{}
}

if (-not $config.Contains('hooks') -or $null -eq $config.hooks) { $config.hooks = @{} }
foreach ($event in $events) {
    $groups = @()
    if ($config.hooks.Contains($event)) { $groups = @($config.hooks[$event]) }
    $groups = @($groups | Where-Object {
        -not (@($_.hooks) | Where-Object {
            $_.type -eq 'command' -and $_.command -match 'DesktopCompanionHook\.exe'
        })
    })
    $groups += @{ hooks = @(@{ type = 'command'; command = $command; timeout = 3 }) }
    $config.hooks[$event] = $groups
}

$parent = Split-Path -Parent $TargetPath
if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
$config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $TargetPath -Encoding utf8
Write-Host "Installed four pet hooks in $TargetPath"
Write-Host 'Review and trust the new hooks in Codex before they can run.'
