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
    [string]$HelperPath = (Join-Path $PSScriptRoot '..\build\DesktopCompanionHook.exe'),
    [string]$TargetPath,
    [switch]$Remove,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

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
               Command = "& '" + $Helper.Replace("'", "''") + "'" }
        }
        'workbuddy' {
            @{ Name = 'workbuddy'
               Root = Join-Path $env:USERPROFILE '.workbuddy'
               Config = Join-Path $env:USERPROFILE '.workbuddy\settings.json'
               Events = @('UserPromptSubmit', 'Stop', 'SessionEnd')
               Timeout = 5
               Matcher = ''
               Command = $quoted }
        }
        'claude' {
            @{ Name = 'claude'
               Root = Join-Path $env:USERPROFILE '.claude'
               Config = Join-Path $env:USERPROFILE '.claude\settings.json'
               Events = @('UserPromptSubmit', 'Stop', 'SessionEnd')
               Timeout = 5
               Matcher = ''
               Command = $quoted }
        }
        default { throw "Unknown client '$Name'" }
    }
}

# Write JSON as BOM-less UTF-8. `Set-Content -Encoding utf8` under PowerShell
# 5.1 stamps a UTF-8 BOM (EF BB BF) on the file, and Codex's config reader
# rejects the whole hooks.json when it starts with one -- the pet silently stops
# reacting. Keep this the ONLY way these scripts write a config file.
function Write-JsonFile {
    param([string]$Path, [string]$Content)
    $full = if ([IO.Path]::IsPathRooted($Path)) { $Path }
            else { Join-Path (Get-Location).Path $Path }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($full, $Content + [Environment]::NewLine, $utf8NoBom)
}

# A hook group belongs to the pet when any of its commands references the helper.
function Test-PetGroup {
    param($Group)
    foreach ($hook in @($Group.hooks)) {
        if ($hook.command -match 'DesktopCompanionHook') { return $true }
    }
    return $false
}

$helper = (Resolve-Path -LiteralPath $HelperPath).Path

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
        $config = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
        $hadHooks = @($config.PSObject.Properties.Name) -contains 'hooks'
    }

    if (-not $hadHooks) { $config | Add-Member -NotePropertyName hooks -NotePropertyValue (New-Object psobject) -Force }

    foreach ($event in $spec.Events) {
        $existing = @()
        if (@($config.hooks.PSObject.Properties.Name) -contains $event) { $existing = @($config.hooks.$event) }
        $kept = @($existing | Where-Object { -not (Test-PetGroup $_) })
        if (-not $Remove) {
            $group = @{ hooks = @(@{ type = 'command'; command = $spec.Command; timeout = $spec.Timeout }) }
            if ($null -ne $spec.Matcher) { $group['matcher'] = $spec.Matcher }
            $kept += $group
        }
        $config.hooks | Add-Member -NotePropertyName $event -NotePropertyValue $kept -Force
    }

    # Drop an empty hooks object we created and never populated.
    if (-not $Remove -and -not $spec.Events) { continue }

    $action = if ($Remove) { 'Removed' } else { 'Installed' }
    if ($DryRun) {
        Write-Host "$action (dry-run) pet hooks for $name -> $path"
        Write-Host ($config.hooks | ConvertTo-Json -Depth 20)
        continue
    }

    if (Test-Path -LiteralPath $path) {
        $backup = "$path.backup-$(Get-Date -Format yyyyMMdd-HHmmss)"
        Copy-Item -LiteralPath $path -Destination $backup
        Write-Host "Backup: $backup"
    } else {
        $parent = Split-Path -Parent $path
        if ($parent -and -not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    }
    Write-JsonFile -Path $path -Content ($config | ConvertTo-Json -Depth 20)
    Write-Host "$action pet hooks for $name in $path ($($spec.Events -join ', '))"
}
