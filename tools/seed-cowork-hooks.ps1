<#
.SYNOPSIS
  Seed or remove the desktop pet's hooks in Claude Desktop per-session configs.
.DESCRIPTION
  The app normally seeds these itself. This script is the manual fallback and
  the installed/portable uninstaller's cleanup path. Only commands executing
  DesktopCompanionHook(.exe) are changed; shared groups retain their other hooks.
#>
param(
    [string]$HelperPath = '',
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hook-config-common.ps1')
if (-not $HelperPath) { $HelperPath = Get-DefaultPetHelperPath -ScriptDirectory $PSScriptRoot }
$command = ''
if (-not $Remove) {
    $helper = (Resolve-Path -LiteralPath $HelperPath).Path
    $command = '"' + $helper + '" --client claude'
}
$events = @('UserPromptSubmit', 'Stop', 'SessionEnd')
$root = Join-Path $env:LOCALAPPDATA 'Claude-3p\local-agent-mode-sessions'
if (-not (Test-Path -LiteralPath $root)) { Write-Warning "No Claude Desktop session root at $root"; return }

$written = 0
$skipped = 0
Get-ChildItem -LiteralPath $root -Directory -Recurse -Depth 3 -Filter '.claude' -ErrorAction SilentlyContinue |
    ForEach-Object {
        $settings = Join-Path $_.FullName 'settings.json'
        if (-not (Test-Path -LiteralPath $settings)) {
            if ($Remove) { return }
            $config = New-Object psobject
        } else {
            try { $config = Read-HookConfig -Path $settings -Events $events }
            catch { Write-Warning "Leaving config alone: $settings ($($_.Exception.Message))"; $script:skipped++; return }
        }
        $hadHooks = $config.PSObject.Properties.Name -contains 'hooks'
        if ($Remove -and -not $hadHooks) { return }
        if (-not $hadHooks) { $config | Add-Member -NotePropertyName hooks -NotePropertyValue (New-Object psobject) }
        $changed = $false
        foreach ($event in $events) {
            $existing = @()
            if ($config.hooks.PSObject.Properties.Name -contains $event) { $existing = @($config.hooks.$event) }
            $kept = Remove-PetHookCommands -Groups $existing -Changed ([ref]$changed)
            if (-not $Remove) {
                $kept += [ordered]@{ hooks = @([ordered]@{ type = 'command'; command = $command; timeout = 5 }) }
            }
            if ($Remove -and $kept.Count -eq 0) { $config.hooks.PSObject.Properties.Remove($event) }
            else { $config.hooks | Add-Member -NotePropertyName $event -NotePropertyValue @($kept) -Force }
        }
        if ($Remove -and -not $changed) { return }
        if ($Remove -and @($config.hooks.PSObject.Properties).Count -eq 0) { $config.PSObject.Properties.Remove('hooks') }
        if (Write-HookConfig -Path $settings -Config $config) { $script:written++ }
    }
Write-Host "Updated $written Claude session config(s), skipped $skipped invalid config(s)."
