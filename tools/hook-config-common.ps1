# Shared by the shipped hook installer and Claude Desktop session cleanup.
# Dot-source this file; it does not inspect or write any user configuration.

function Get-DefaultPetHelperPath {
    param([string]$ScriptDirectory)
    $deployed = Join-Path $ScriptDirectory '..\DesktopCompanionHook.exe'
    if (Test-Path -LiteralPath $deployed -PathType Leaf) { return [IO.Path]::GetFullPath($deployed) }
    return [IO.Path]::GetFullPath((Join-Path $ScriptDirectory '..\build\DesktopCompanionHook.exe'))
}

function Test-PetHookCommand {
    param($Command)
    if ($Command -isnot [string]) { return $false }
    $match = [regex]::Match($Command, '^\s*(?:&\s+)?(?:''((?:[^'']|'''')*)''|"([^"]+)"|([^\s''"]+))(?=\s|$)')
    if (-not $match.Success) { return $false }
    $executable = if ($match.Groups[1].Success) { $match.Groups[1].Value.Replace("''", "'") }
                  elseif ($match.Groups[2].Success) { $match.Groups[2].Value }
                  else { $match.Groups[3].Value }
    $leaf = ($executable -replace '/', '\').Split('\')[-1]
    return $leaf -ieq 'DesktopCompanionHook.exe' -or $leaf -ieq 'DesktopCompanionHook'
}

function Read-HookConfig {
    param([string]$Path, [string[]]$Events)
    $encoding = New-Object System.Text.UTF8Encoding($false, $true)
    $raw = [IO.File]::ReadAllText($Path, $encoding)
    # ConvertFrom-Json expands a one-element top-level array on some hosts.
    if (-not $raw.TrimStart().StartsWith('{')) { throw "Config is not a JSON object: $Path" }
    $config = ConvertFrom-Json -InputObject $raw -ErrorAction Stop
    if ($null -eq $config -or $config -isnot [pscustomobject]) { throw "Config is not a JSON object: $Path" }
    if ($config.PSObject.Properties.Name -contains 'hooks') {
        if ($config.hooks -isnot [pscustomobject]) { throw "Config hooks is not an object: $Path" }
        foreach ($event in $Events) {
            if ($config.hooks.PSObject.Properties.Name -notcontains $event) { continue }
            if ($config.hooks.$event -isnot [array]) { throw "Hook event $event is not an array: $Path" }
            foreach ($group in $config.hooks.$event) {
                if ($group -isnot [pscustomobject] -or $group.hooks -isnot [array]) {
                    throw "Hook event $event contains an invalid group: $Path"
                }
                foreach ($hook in $group.hooks) {
                    if ($hook -isnot [pscustomobject]) { throw "Hook event $event contains an invalid hook: $Path" }
                    if (($hook.PSObject.Properties.Name -contains 'command' -or $hook.type -eq 'command') -and
                        $hook.command -isnot [string]) { throw "Hook event $event contains an invalid command: $Path" }
                }
            }
        }
    }
    return $config
}

function Remove-PetHookCommands {
    param($Groups, [ref]$Changed)
    $kept = New-Object 'System.Collections.Generic.List[object]'
    foreach ($group in @($Groups)) {
        $hooks = @($group.hooks | Where-Object { -not (Test-PetHookCommand $_.command) })
        if ($hooks.Count -eq @($group.hooks).Count) { $kept.Add($group); continue }
        $Changed.Value = $true
        if ($hooks.Count -gt 0) {
            # Keep matcher and every other group property, changing only hooks.
            $group.hooks = $hooks
            $kept.Add($group)
        }
    }
    return ,($kept.ToArray())
}

function Write-HookConfig {
    param([string]$Path, $Config)
    $full = [IO.Path]::GetFullPath($Path)
    $content = ($Config | ConvertTo-Json -Depth 100) + [Environment]::NewLine
    $encoding = New-Object System.Text.UTF8Encoding($false)
    # Idempotent installs/removals do not rotate backups or timestamps.
    if ((Test-Path -LiteralPath $full) -and
        [Convert]::ToBase64String([IO.File]::ReadAllBytes($full)) -ceq [Convert]::ToBase64String($encoding.GetBytes($content))) { return $false }
    $parent = [IO.Path]::GetDirectoryName($full)
    if (-not (Test-Path -LiteralPath $parent)) { New-Item -ItemType Directory -Path $parent -Force | Out-Null }
    $temporary = Join-Path $parent ('.pet-hooks-' + [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::WriteAllText($temporary, $content, $encoding)
        if (Test-Path -LiteralPath $full) {
            $backup = $full + '.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fffffff') + '-' + [guid]::NewGuid().ToString('N')
            # Same-directory replacement is atomic; the backup is the exact old file.
            [IO.File]::Replace($temporary, $full, $backup)
        } else { [IO.File]::Move($temporary, $full) }
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
    }
    return $true
}
