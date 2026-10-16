param(
    [string]$TargetPath = (Join-Path $env:USERPROFILE '.codex\hooks.json'),
    [string]$HelperPath = (Join-Path $PSScriptRoot '..\build\DesktopCompanionHook.exe')
)

$ErrorActionPreference = 'Stop'
# Thin wrapper kept for the documented Codex entry point and CodexHookTest.
# The multi-client logic (Codex / WorkBuddy / Claude) lives in install-hooks.ps1.
& (Join-Path $PSScriptRoot 'install-hooks.ps1') -Clients codex -TargetPath $TargetPath -HelperPath $HelperPath
