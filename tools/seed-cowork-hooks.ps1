<#
.SYNOPSIS
  Seed the Desktop-Companion pet hooks into every Claude-Desktop ("3p"/Cowork)
  session config dir.

.DESCRIPTION
  The Claude Desktop app launches its embedded Claude Code with
  `--setting-sources=user`, and points that "user" layer at a PER-SESSION config
  dir:  %LOCALAPPDATA%\Claude-3p\local-agent-mode-sessions\<acct>\<x>\<session>\.claude

  So `~/.claude/settings.json` is never read there, and neither are project
  `.claude` files. The ONLY place that works is that per-session
  `.claude\settings.json`. This script writes the pet hooks into every existing
  session dir (idempotent; keeps unrelated keys). Re-run it (or schedule it) to
  cover new conversations, which get fresh session dirs.

  The token-exact "DesktopCompanionHook" proxy is untouched: only the pet's own
  hook entries are managed here.
#>
param(
    [string]$HelperPath = (Join-Path $PSScriptRoot '..\build\DesktopCompanionHook.exe')
)

$ErrorActionPreference = 'Stop'
$helper = (Resolve-Path -LiteralPath $HelperPath).Path

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
# Claude Desktop runs hook commands through the Windows shell; a quoted absolute
# path is what the desktop config already uses successfully (see .workbuddy).
$command = '"' + $helper + '"'

$hookGroups = [ordered]@{}
foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionEnd')) {
    $hookGroups[$event] = @( @{ hooks = @( @{ type = 'command'; command = $command; timeout = 5 } ) } )
}

$root = Join-Path $env:LOCALAPPDATA 'Claude-3p\local-agent-mode-sessions'
if (-not (Test-Path -LiteralPath $root)) { Write-Warning "No Claude Desktop session root at $root"; return }

$seeded = 0
Get-ChildItem -LiteralPath $root -Directory -Recurse -Depth 3 -Filter '.claude' -ErrorAction SilentlyContinue |
    ForEach-Object {
        $settings = Join-Path $_.FullName 'settings.json'
        $config = [ordered]@{}
        if (Test-Path -LiteralPath $settings) {
            (Get-Content -LiteralPath $settings -Raw | ConvertFrom-Json).PSObject.Properties |
                ForEach-Object { if ($_.Name -ne 'hooks') { $config[$_.Name] = $_.Value } }
        }
        $config['hooks'] = $hookGroups
        Write-JsonFile -Path $settings -Content ($config | ConvertTo-Json -Depth 20)
        Write-Host "seeded $settings"
        $script:seeded++
    }
Write-Host "Seeded $seeded session config dir(s)."
