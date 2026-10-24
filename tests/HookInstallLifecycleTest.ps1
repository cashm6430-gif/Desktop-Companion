param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [Parameter(Mandatory)][string]$SeedScriptPath,
    [switch]$LegacyPass
)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('pet-hook-lifecycle-' + [guid]::NewGuid().ToString('N'))
$originalLocal = $env:LOCALAPPDATA
$encoding = New-Object System.Text.UTF8Encoding($false)
$cjk = [string][char]0x9cb8 + [string][char]0x9c7c + [string][char]0x5a18
function Assert([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function Fingerprint([string]$Path) {
    $file = Get-Item -LiteralPath $Path
    $backups = @(Get-ChildItem -LiteralPath $file.DirectoryName -Filter ($file.Name + '.backup-*') | Sort-Object Name | ForEach-Object Name)
    return (Get-FileHash -LiteralPath $Path).Hash + '|' + $file.LastWriteTimeUtc.Ticks + '|' + ($backups -join ',')
}
function ReadConfig([string]$Path) { return ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($Path, [Text.Encoding]::UTF8)) }
function PutConfig([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 100), $encoding) }
function RunInstall([string]$Client, [string]$Path, [string]$Helper, [bool]$Remove = $false) {
    & $InstallerPath -Clients $Client -TargetPath $Path -HelperPath $Helper -Remove:$Remove | Out-Null
}
try {
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    $newFolder = Join-Path $root ("new author's " + $cjk)
    New-Item -ItemType Directory -Path $newFolder | Out-Null
    $helper = Join-Path $newFolder 'DesktopCompanionHook.exe'
    [IO.File]::WriteAllText($helper, 'not executable; installer fixture only', $encoding)
    foreach ($client in @('codex', 'workbuddy', 'claude')) {
        $path = Join-Path $root ($client + '.json')
        $other = [ordered]@{ type = 'command'; command = '"C:\other\notify.exe" DesktopCompanionHook.exe' }
        $lookalike = [ordered]@{ type = 'command'; command = '"C:\other\DesktopCompanionHook.exe.bak"' }
        $prefix = [ordered]@{ type = 'command'; command = '"C:\other\MyDesktopCompanionHook.exe"' }
        $old = [ordered]@{ type = 'command'; command = '& ''C:\old path\DesktopCompanionHook.exe''' }
        $initial = [ordered]@{ theme = $cjk; hooks = [ordered]@{
            Stop = @([ordered]@{ matcher = 'Bash'; metadata = [ordered]@{ keep = 42 }; hooks = @($old, $other, $lookalike, $prefix) })
            PreToolUse = @([ordered]@{ hooks = @($other) })
        } }
        PutConfig $path $initial
        $initialBytes = [IO.File]::ReadAllBytes($path)
        RunInstall $client $path $helper
        $config = ReadConfig $path
        Assert ($config.theme -ceq $cjk) "$client lost UTF-8 text"
        Assert ($config.hooks.Stop.Count -eq 2) "$client must retain mixed group and append one pet group"
        Assert ($config.hooks.Stop[0].hooks.Count -eq 3) "$client removed unrelated commands"
        Assert ($config.hooks.Stop[0].matcher -eq 'Bash' -and $config.hooks.Stop[0].metadata.keep -eq 42) "$client lost group metadata"
        Assert ($config.hooks.Stop[0].hooks[0].command -ceq $other.command) "$client mistook an argument for executable"
        $expected = if ($client -eq 'codex') { "& '" + $helper.Replace("'", "''") + "' --client codex" }
                    else { '"' + $helper + '" --client ' + $client }
        Assert ($config.hooks.Stop[1].hooks[0].command -ceq $expected) "$client upgrade command is wrong"
        $bytes = [IO.File]::ReadAllBytes($path)
        Assert (-not ($bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF)) "$client wrote a BOM"
        $backup = @(Get-ChildItem -LiteralPath $root -Filter ($client + '.json.backup-*'))
        Assert ($backup.Count -eq 1) "$client must back up the old config once"
        Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backup[0].FullName)) -ceq [Convert]::ToBase64String($initialBytes)) "$client backup bytes differ"
        $before = Fingerprint $path
        RunInstall $client $path $helper
        Assert ((Fingerprint $path) -ceq $before) "$client repeated install changed bytes, time or backups"
        # Simulate portable folder loss: cleanup must not Resolve-Path the helper.
        RunInstall $client $path (Join-Path $root 'gone\DesktopCompanionHook.exe') $true
        $removed = ReadConfig $path
        Assert ($removed.hooks.Stop.Count -eq 1 -and $removed.hooks.Stop[0].hooks.Count -eq 3) "$client cleanup lost unrelated hooks"
        Assert ($removed.hooks.PSObject.Properties.Name -notcontains 'UserPromptSubmit') "$client left an empty managed event"
        Assert ($removed.hooks.PSObject.Properties.Name -notcontains 'SessionEnd') "$client left an empty managed event"
        Assert ($removed.hooks.PreToolUse.Count -eq 1) "$client touched an unmanaged event"
        $before = Fingerprint $path
        RunInstall $client $path $helper $true
        Assert ((Fingerprint $path) -ceq $before) "$client repeated remove changed bytes, time or backups"
        Assert (@(Get-ChildItem -LiteralPath $root -Filter ($client + '.json.backup-*')).Count -eq 2) "$client backup names collided"

        $empty = Join-Path $root ($client + '-empty.json')
        PutConfig $empty ([ordered]@{ theme = $cjk })
        $before = Fingerprint $empty
        RunInstall $client $empty $helper $true
        Assert ((Fingerprint $empty) -ceq $before) "$client removal touched a config without our hooks"
        RunInstall $client $empty $helper
        RunInstall $client $empty $helper $true
        Assert ((ReadConfig $empty).PSObject.Properties.Name -notcontains 'hooks') "$client left an empty hooks object"

        $bomPath = Join-Path $root ($client + '-bom.json')
        RunInstall $client $bomPath $helper
        $canonical = [IO.File]::ReadAllBytes($bomPath)
        [IO.File]::WriteAllBytes($bomPath, [byte[]](@(0xEF, 0xBB, 0xBF) + $canonical))
        RunInstall $client $bomPath $helper
        Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($bomPath)) -ceq [Convert]::ToBase64String($canonical)) "$client left a BOM on otherwise canonical JSON"
    }
    foreach ($invalid in @('{invalid', 'null', '[]', '[{}]', '{"hooks":[]}', '{"hooks":{"Stop":{}}}', '{"hooks":{"Stop":[{}]}}', '{"hooks":{"Stop":[{"hooks":[null]}]}}', '{"hooks":{"Stop":[{"hooks":[{"type":"command","command":7}]}]}}')) {
        $path = Join-Path $root 'invalid.json'
        [IO.File]::WriteAllText($path, $invalid, $encoding)
        $before = Fingerprint $path
        $failed = $false
        try { RunInstall 'codex' $path $helper } catch { $failed = $true }
        Assert $failed 'invalid config was silently accepted'
        Assert ((Fingerprint $path) -ceq $before) 'invalid config was rewritten or backed up'
    }
    $env:LOCALAPPDATA = Join-Path $root 'local-app-data'
    $session = Join-Path $env:LOCALAPPDATA 'Claude-3p\local-agent-mode-sessions\acct\bucket\session\.claude'
    New-Item -ItemType Directory -Path $session -Force | Out-Null
    $settings = Join-Path $session 'settings.json'
    PutConfig $settings $initial
    & $SeedScriptPath -HelperPath $helper | Out-Null
    $config = ReadConfig $settings
    Assert ($config.theme -ceq $cjk -and $config.hooks.Stop[0].hooks.Count -eq 3) 'session seeding lost UTF-8 or mixed hooks'
    $before = Fingerprint $settings
    & $SeedScriptPath -HelperPath $helper | Out-Null
    Assert ((Fingerprint $settings) -ceq $before) 'session repeated seed changed bytes, time or backups'
    & $SeedScriptPath -Remove | Out-Null
    $before = Fingerprint $settings
    & $SeedScriptPath -Remove | Out-Null
    Assert ((Fingerprint $settings) -ceq $before) 'session repeated removal changed bytes, time or backups'
    Assert ((ReadConfig $settings).hooks.Stop[0].hooks.Count -eq 3) 'session removal lost other commands'

    # The shipping tools directory has its helper in the parent, not parent/build.
    $packageRoot = Join-Path $root 'portable layout'
    $packageTools = Join-Path $packageRoot 'tools'
    New-Item -ItemType Directory -Path $packageTools -Force | Out-Null
    $commonPath = Join-Path (Split-Path -Parent $InstallerPath) 'hook-config-common.ps1'
    Copy-Item -LiteralPath $InstallerPath, $SeedScriptPath, $commonPath -Destination $packageTools
    $packageHelper = Join-Path $packageRoot 'DesktopCompanionHook.exe'
    [IO.File]::WriteAllText($packageHelper, 'not executable; portable fixture only', $encoding)
    $packageInstaller = Join-Path $packageTools 'install-hooks.ps1'
    $packageSeeder = Join-Path $packageTools 'seed-cowork-hooks.ps1'
    $packageConfig = Join-Path $root 'packaged-default.json'
    & $packageInstaller -Clients codex -TargetPath $packageConfig | Out-Null
    Assert ((ReadConfig $packageConfig).hooks.Stop[0].hooks[0].command -ceq ("& '" + $packageHelper.Replace("'", "''") + "' --client codex")) 'packaged installer default helper path is wrong'
    & $packageSeeder | Out-Null
    $packageSessionConfig = ReadConfig $settings
    Assert ($packageSessionConfig.hooks.Stop[1].hooks[0].command -ceq ('"' + $packageHelper + '" --client claude')) 'packaged session seeder default helper path is wrong'
    & $packageSeeder -Remove | Out-Null
    Remove-Item -LiteralPath $packageHelper
    & $packageInstaller -Clients codex -TargetPath $packageConfig -Remove | Out-Null
    Assert ((ReadConfig $packageConfig).PSObject.Properties.Name -notcontains 'hooks') 'packaged cleanup requires the helper executable'
    Write-Host "Hook lifecycle passed ($($PSVersionTable.PSVersion)): upgrade, exact ownership, mixed groups, UTF-8, backup, cleanup and idempotence."
} finally {
    $env:LOCALAPPDATA = $originalLocal
    $resolved = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe fixture cleanup path: $resolved" }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
if (-not $LegacyPass) {
    $legacy = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $legacy) {
        & $legacy -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -InstallerPath $InstallerPath -SeedScriptPath $SeedScriptPath -LegacyPass
        if ($LASTEXITCODE -ne 0) { throw 'Windows PowerShell 5.1 lifecycle pass failed' }
    }
}
