<#
.SYNOPSIS
  Seed (or remove) the Desktop-Companion pet hooks in every Claude-Desktop
  ("3p"/Cowork) session config dir. Manual fallback -- the pet normally does this
  itself.

.DESCRIPTION
  The Claude Desktop app launches its embedded Claude Code with
  `--setting-sources=user`, and points that "user" layer at a PER-SESSION config
  dir:  %LOCALAPPDATA%\Claude-3p\local-agent-mode-sessions\<acct>\<x>\<session>\.claude

  So `~/.claude/settings.json` is never read there, and neither are project
  `.claude` files. The ONLY place that works is that per-session
  `.claude\settings.json`, and every new conversation is a fresh dir.

  THE PET DOES THIS BY ITSELF NOW (src/CoworkHookSeeder.cpp: seeds at startup and
  every couple of minutes, in-process, no console window). This script is kept for
  the cases the pet cannot cover -- chiefly when the pet runs under a sandbox that
  blocks writes outside its own folder, where its seeding fails with "access
  denied" and the trace says so. Run it from a normal shell in that situation.

  -Remove takes the pet's entries back out again; the installer's uninstaller runs
  it, so uninstalling really does leave no trace (the same promise the Codex /
  WorkBuddy / Claude-CLI hooks already keep).

  Only entries whose command references DesktopCompanionHook are touched; every
  other key, event and tool's hook group is preserved, and the merge matches the
  pet's (see CoworkHookSeederTest).
#>
param(
    [string]$HelperPath = (Join-Path $PSScriptRoot '..\build\DesktopCompanionHook.exe'),
    [switch]$Remove
)

$ErrorActionPreference = 'Stop'

# Write JSON as BOM-less UTF-8. `Set-Content -Encoding utf8` under PowerShell
# 5.1 stamps a UTF-8 BOM, which strict JSON readers (Codex) reject outright.
# Same contract as the helper in install-hooks.ps1 -- keep the two in step.
function Write-JsonFile {
    param([string]$Path, [string]$Content)
    $full = if ([IO.Path]::IsPathRooted($Path)) { $Path }
            else { Join-Path (Get-Location).Path $Path }
    $utf8NoBom = New-Object System.Text.UTF8Encoding($false)
    [IO.File]::WriteAllText($full, $Content + [Environment]::NewLine, $utf8NoBom)
}

# A hook group belongs to the pet when any of its commands references the helper.
function Test-PetGroup($Group) {
    foreach ($hook in @($Group.hooks)) {
        if ($hook.command -match 'DesktopCompanionHook') { return $true }
    }
    return $false
}

# Claude Desktop runs hook commands through the Windows shell; a quoted absolute
# path is what the desktop config already uses successfully (see .workbuddy).
# Removal matches on the file NAME, so it must not depend on the helper still being
# there -- at uninstall time it may already be on its way out.
$command = ''
if (-not $Remove) {
    $helper = (Resolve-Path -LiteralPath $HelperPath).Path
    $command = '"' + $helper + '"'
}
$events = @('UserPromptSubmit', 'Stop', 'SessionEnd')

$root = Join-Path $env:LOCALAPPDATA 'Claude-3p\local-agent-mode-sessions'
if (-not (Test-Path -LiteralPath $root)) { Write-Warning "No Claude Desktop session root at $root"; return }

$seeded = 0
$cleaned = 0
$skipped = 0
Get-ChildItem -LiteralPath $root -Directory -Recurse -Depth 3 -Filter '.claude' -ErrorAction SilentlyContinue |
    ForEach-Object {
        $settings = Join-Path $_.FullName 'settings.json'

        # Removing only ever touches files that exist, and never a config we cannot
        # parse: a config we do not understand is a config we must not rewrite.
        if (-not (Test-Path -LiteralPath $settings)) {
            if ($Remove) { return }
            $existing = $null
        } else {
            $raw = Get-Content -LiteralPath $settings -Raw
            if ([string]::IsNullOrWhiteSpace($raw)) { $existing = $null }
            else {
                try { $existing = $raw | ConvertFrom-Json -ErrorAction Stop }
                catch {
                    Write-Warning "Not JSON, leaving alone: $settings"
                    $script:skipped++
                    return
                }
            }
            if ($Remove -and $existing -and
                -not ($existing.PSObject.Properties.Name -contains 'hooks')) { return }
        }

        $config = [ordered]@{}
        $hooks = [ordered]@{}
        if ($existing) {
            $existing.PSObject.Properties |
                ForEach-Object { if ($_.Name -ne 'hooks') { $config[$_.Name] = $_.Value } }
            if ($existing.hooks) {
                # Every event we do not manage survives untouched.
                $existing.hooks.PSObject.Properties |
                    ForEach-Object { if ($events -notcontains $_.Name) { $hooks[$_.Name] = $_.Value } }
            }
        }

        $changed = $false
        foreach ($event in $events) {
            # Other tools' groups in a shared event survive; ours come out.
            $kept = @()
            if ($existing -and $existing.hooks -and $existing.hooks.$event) {
                $kept = @($existing.hooks.$event | Where-Object { -not (Test-PetGroup $_) })
            }
            if ($kept.Count -ne @($existing.hooks.$event).Count) { $changed = $true }
            if ($Remove) {
                # An event we emptied stops existing, rather than becoming "[]".
                if ($kept.Count -gt 0) { $hooks[$event] = $kept }
            } else {
                $kept += @{ hooks = @( @{ type = 'command'; command = $command; timeout = 5 } ) }
                $hooks[$event] = $kept
            }
        }

        if ($Remove) {
            # Never in this file -> leave it byte-for-byte as we found it.
            if (-not $changed) { return }
            # Whatever events still hold other tools' groups survive; if ours were
            # the only thing in `hooks`, the key goes with them.
            if (@($hooks.Keys).Count -gt 0) { $config['hooks'] = $hooks }
            Write-JsonFile -Path $settings -Content ($config | ConvertTo-Json -Depth 20)
            Write-Host "cleaned  $settings"
            $script:cleaned++
            return
        }

        $config['hooks'] = $hooks
        Write-JsonFile -Path $settings -Content ($config | ConvertTo-Json -Depth 20)
        Write-Host "seeded   $settings"
        $script:seeded++
    }

if ($Remove) {
    Write-Host "Removed pet hooks from $cleaned session config dir(s) (skipped $skipped unreadable)."
} else {
    Write-Host "Seeded $seeded session config dir(s)."
}
