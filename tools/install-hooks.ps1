<#
.SYNOPSIS
  Install (or remove) the Desktop-Companion pet hooks for every supported
  agent client: Codex, WorkBuddy and Claude.

.DESCRIPTION
  All three clients call the SAME helper (DesktopCompanionHook.exe) and pass the
  same stdin payload (hook_event_name / session_id), so the only per-client
  differences are where the hooks live and how the command string is written:

    Codex      ~/.codex/hooks.json        PowerShell '& ''<exe>''', events incl. Interrupt
    WorkBuddy  ~/.workbuddy/settings.json cmd-quoted "<exe>", {matcher,hooks[]} groups
    Claude     ~/.claude/settings.json    same shape as WorkBuddy, no Interrupt event

  Only entries whose command references DesktopCompanionHook(.exe) are replaced;
  every other hook is preserved. Each target file is backed up first, and an
  existing hooks section owned by another tool is never clobbered wholesale.

.PARAMETER Clients
  auto (default) installs for every client whose config root exists; codex /
  workbuddy / claude select one; all selects the three.

.PARAMETER TargetPath
  Override the config path for a single selected client (used by tests).

.PARAMETER Remove
  Remove the pet hooks instead of installing them.
#>
param(
    [ValidateSet('auto', 'codex', 'workbuddy', 'claude', 'all')]
    [string]$Clients = 'auto',
    [string]$HelperPath = '',
    [string]$TargetPath,
    [switch]$Remove,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'hook-config-common.ps1')
if (-not $HelperPath) { $HelperPath = Get-DefaultPetHelperPath -ScriptDirectory $PSScriptRoot }

function New-ClientSpec {
    param([string]$Name, [string]$Helper)
    $quoted = '"' + $Helper + '"'
    switch ($Name) {
        'codex' {
            @{ Name = 'codex'
               Root = Join-Path $env:USERPROFILE '.codex'
               Config = Join-Path $env:USERPROFILE '.codex\hooks.json'
               Events = @('UserPromptSubmit', 'Stop', 'Interrupt', 'SessionEnd')
               Timeout = 3
               Matcher = $null
               Command = "& '" + $Helper.Replace("'", "''") + "' --client codex" }
        }
        'workbuddy' {
            @{ Name = 'workbuddy'
               Root = Join-Path $env:USERPROFILE '.workbuddy'
               Config = Join-Path $env:USERPROFILE '.workbuddy\settings.json'
               Events = @('UserPromptSubmit', 'Stop', 'SessionEnd')
               Timeout = 5
               Matcher = ''
               Command = $quoted + ' --client workbuddy' }
        }
        'claude' {
            @{ Name = 'claude'
               Root = Join-Path $env:USERPROFILE '.claude'
               Config = Join-Path $env:USERPROFILE '.claude\settings.json'
               Events = @('UserPromptSubmit', 'Stop', 'SessionEnd')
               Timeout = 5
               Matcher = ''
               Command = $quoted + ' --client claude' }
        }
        default { throw "Unknown client '$Name'" }
    }
}

# Removal identifies commands, so it must work after the executable is gone.
$helper = if ($Remove) { [IO.Path]::GetFullPath($HelperPath) }
          else { (Resolve-Path -LiteralPath $HelperPath).Path }

if ($TargetPath -and ($Clients -eq 'auto' -or $Clients -eq 'all')) {
    throw '-TargetPath requires a single -Clients value (codex|workbuddy|claude).'
}

switch ($Clients) {
    'all' { $names = @('codex', 'workbuddy', 'claude') }
    'auto' { $names = @('codex', 'workbuddy', 'claude') | Where-Object {
                 Test-Path -LiteralPath (New-ClientSpec -Name $_ -Helper $helper).Root } }
    default { $names = @($Clients) }
}

if (-not $names) {
    Write-Host 'No agent clients detected under the user profile; nothing to do.'
    return
}

foreach ($name in $names) {
    $spec = New-ClientSpec -Name $name -Helper $helper
    $path = if ($TargetPath) { $TargetPath } else { $spec.Config }

    if (-not (Test-Path -LiteralPath $path)) {
        if ($Remove) { Write-Host "$name : no config at $path, nothing to remove"; continue }
        $config = New-Object psobject
        $hadHooks = $false
    } else {
        $config = Read-HookConfig -Path $path -Events $spec.Events
        $hadHooks = @($config.PSObject.Properties.Name) -contains 'hooks'
    }

    if ($Remove -and -not $hadHooks) { continue }
    if (-not $hadHooks) { $config | Add-Member -NotePropertyName hooks -NotePropertyValue (New-Object psobject) -Force }

    $changed = $false
    foreach ($event in $spec.Events) {
        $existing = @()
        if (@($config.hooks.PSObject.Properties.Name) -contains $event) { $existing = @($config.hooks.$event) }
        $kept = Remove-PetHookCommands -Groups $existing -Changed ([ref]$changed)
        if (-not $Remove) {
            $group = [ordered]@{ hooks = @([ordered]@{ type = 'command'; command = $spec.Command; timeout = $spec.Timeout }) }
            if ($null -ne $spec.Matcher) { $group['matcher'] = $spec.Matcher }
            $kept += $group
        }
        if ($Remove -and $kept.Count -eq 0) { $config.hooks.PSObject.Properties.Remove($event) }
        else { $config.hooks | Add-Member -NotePropertyName $event -NotePropertyValue @($kept) -Force }
    }

    if ($Remove -and -not $changed) { continue }
    if ($Remove -and @($config.hooks.PSObject.Properties).Count -eq 0) { $config.PSObject.Properties.Remove('hooks') }

    $action = if ($Remove) { 'Removed' } else { 'Installed' }
    if ($DryRun) {
        Write-Host "$action (dry-run) pet hooks for $name -> $path"
        Write-Host ($config.hooks | ConvertTo-Json -Depth 20)
        continue
    }

    $written = Write-HookConfig -Path $path -Config $config
    Write-Host "$action pet hooks for $name in $path ($($spec.Events -join ', '))"
}
