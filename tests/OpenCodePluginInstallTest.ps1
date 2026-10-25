param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [string]$UnifiedInstallerPath = (Join-Path (Split-Path -Parent $InstallerPath) 'install-hooks.ps1'),
    [switch]$LegacyPass
)
$ErrorActionPreference = 'Stop'
$root = Join-Path ([IO.Path]::GetTempPath()) ('pet-opencode-install-' + [guid]::NewGuid().ToString('N'))
$encoding = New-Object Text.UTF8Encoding($false)
$cjk = [string][char]0x9cb8 + [string][char]0x9c7c + [string][char]0x5a18
function Assert([bool]$Value, [string]$Message) { if (-not $Value) { throw $Message } }
function ReadJson([string]$Path) { return ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($Path, $encoding)) }
function Fingerprint([string]$Directory) {
    if (-not (Test-Path -LiteralPath $Directory)) { return '<absent>' }
    return (@(Get-ChildItem -LiteralPath $Directory -File -Force -Recurse | Sort-Object FullName | ForEach-Object {
        $_.FullName + '|' + (Get-FileHash -LiteralPath $_.FullName).Hash + '|' + $_.LastWriteTimeUtc.Ticks
    }) -join "`n")
}
function MustFail([scriptblock]$Operation, [string]$Message) {
    $failed = $false
    try { & $Operation | Out-Null } catch { $failed = $true }
    Assert $failed $Message
}
try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $package = Join-Path $root ('portable author''s ' + $cjk)
    $tools = Join-Path $package 'tools'
    $sources = Join-Path $tools 'opencode-plugin'
    New-Item -ItemType Directory -Path $sources -Force | Out-Null
    $common = Join-Path (Split-Path -Parent $InstallerPath) 'hook-config-common.ps1'
    Copy-Item -LiteralPath $InstallerPath, $UnifiedInstallerPath, $common -Destination $tools
    $installer = Join-Path $tools 'install-opencode-plugin.ps1'
    $unified = Join-Path $tools 'install-hooks.ps1'
    foreach ($name in @('index.ts', 'bridge.mjs', 'package.json')) {
        [IO.File]::WriteAllText((Join-Path $sources $name), 'fixture ' + $name, $encoding)
    }
    $helper = Join-Path $package 'DesktopCompanionHook.exe'
    [IO.File]::WriteAllText($helper, 'not executable; installer fixture only', $encoding)
    $target = Join-Path $root ('global config ' + $cjk + '\plugins\desktop-companion')
    & $installer -TargetPath $target -DryRun | Out-Null
    Assert (-not (Test-Path -LiteralPath $target)) 'dry run created plugin directory'
    # Packaged defaults find the helper in parent/, without parent/build/.
    & $installer -TargetPath $target | Out-Null
    $configPath = Join-Path $target 'config.json'
    Assert ((ReadJson $configPath).helperPath -ceq $helper) 'portable helper path or Unicode path changed'
    foreach ($name in @('index.ts', 'bridge.mjs', 'package.json')) {
        Assert ((Get-FileHash -LiteralPath (Join-Path $target $name)).Hash -ceq
                (Get-FileHash -LiteralPath (Join-Path $sources $name)).Hash) "source bytes changed: $name"
    }
    $configBytes = [IO.File]::ReadAllBytes($configPath)
    Assert (-not ($configBytes[0] -eq 239 -and $configBytes[1] -eq 187 -and $configBytes[2] -eq 191)) 'config has UTF-8 BOM'
    $before = Fingerprint $target
    & $unified -Clients opencode -TargetPath $target -HelperPath $helper | Out-Null
    Assert ((Fingerprint $target) -ceq $before) 'unified/repeated install changed bytes, times or backups'
    $other = Join-Path $target 'user-note.txt'
    [IO.File]::WriteAllText($other, 'preserve me', $encoding)
    $oldIndex = [IO.File]::ReadAllBytes((Join-Path $target 'index.ts'))
    [IO.File]::WriteAllText((Join-Path $sources 'index.ts'), 'fixture upgrade', $encoding)
    & $installer -TargetPath $target -HelperPath $helper | Out-Null
    Assert ([IO.File]::ReadAllText((Join-Path $target 'index.ts')) -ceq 'fixture upgrade') 'upgrade did not replace owned plugin'
    $backups = @(Get-ChildItem -LiteralPath $target -Filter 'index.ts.backup-*')
    Assert ($backups.Count -eq 1) 'upgrade backup missing'
    Assert ([Convert]::ToBase64String([IO.File]::ReadAllBytes($backups[0].FullName)) -ceq
            [Convert]::ToBase64String($oldIndex)) 'upgrade backup bytes differ'
    $before = Fingerprint $target
    & $installer -TargetPath $target -HelperPath $helper | Out-Null
    Assert ((Fingerprint $target) -ceq $before) 'repeated upgrade rotated backups'
    # Preflight every file before any mutation, even when removal has no helper.
    $bridge = Join-Path $target 'bridge.mjs'
    $originalBridge = [IO.File]::ReadAllBytes($bridge)
    [IO.File]::WriteAllText($bridge, 'user-edited bridge', $encoding)
    $before = Fingerprint $target
    MustFail { & $installer -TargetPath $target -HelperPath $helper } 'modified owned file was overwritten'
    MustFail { & $installer -TargetPath $target -HelperPath 'C:\gone\DesktopCompanionHook.exe' -Remove } 'modified owned file was removed'
    Assert ((Fingerprint $target) -ceq $before) 'rejected mutation changed plugin files'
    [IO.File]::WriteAllBytes($bridge, $originalBridge)
    Remove-Item -LiteralPath $helper
    & $unified -Clients opencode -TargetPath $target -HelperPath $helper -Remove | Out-Null
    foreach ($name in @('index.ts', 'bridge.mjs', 'package.json', 'config.json')) {
        Assert (-not (Test-Path -LiteralPath (Join-Path $target $name))) "uninstall left an active managed file: $name"
    }
    Assert ([IO.File]::ReadAllText($other) -ceq 'preserve me') 'uninstall removed unrelated file'
    Assert (Test-Path -LiteralPath $backups[0].FullName) 'uninstall removed backup'
    $before = Fingerprint $target
    & $installer -TargetPath $target -Remove | Out-Null
    Assert ((Fingerprint $target) -ceq $before) 'repeated remove changed retained files'
    [IO.File]::WriteAllText($helper, 'fixture restored helper', $encoding)
    & $installer -TargetPath $target -HelperPath $helper | Out-Null
    Assert (Test-Path -LiteralPath (Join-Path $target 'index.ts')) 'reinstall could not restore removed plugin'
    Assert ([IO.File]::ReadAllText($other) -ceq 'preserve me') 'reinstall changed user file'

    $unowned = Join-Path $root 'unowned'
    New-Item -ItemType Directory -Path $unowned | Out-Null
    [IO.File]::WriteAllText((Join-Path $unowned 'index.ts'), 'other plugin', $encoding)
    $before = Fingerprint $unowned
    MustFail { & $unified -Clients opencode -TargetPath $unowned -HelperPath $helper } 'unified installer adopted unrelated folder'
    Assert ((Fingerprint $unowned) -ceq $before) 'unified failure altered unrelated folder'
    & $installer -TargetPath $unowned -Remove | Out-Null
    Assert ((Fingerprint $unowned) -ceq $before) 'remove without ownership changed unrelated folder'
    $invalid = Join-Path $root 'invalid-manifest'
    New-Item -ItemType Directory -Path $invalid | Out-Null
    [IO.File]::WriteAllText((Join-Path $invalid '.desktop-companion-plugin.json'), '{"owner":"Other","version":1,"files":{}}', $encoding)
    $before = Fingerprint $invalid
    MustFail { & $installer -TargetPath $invalid -HelperPath $helper } 'invalid ownership was accepted'
    Assert ((Fingerprint $invalid) -ceq $before) 'invalid ownership rejection changed files'
    MustFail { & $installer -TargetPath ([IO.Path]::GetPathRoot($root)) -HelperPath $helper } 'drive-root target accepted'
    Write-Host "OpenCode plugin install passed ($($PSVersionTable.PSVersion)): Unicode, portable layout, dry run, upgrade backups, idempotence, ownership, tamper protection, cleanup and reinstall."
} finally {
    $resolved = [IO.Path]::GetFullPath($root)
    $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw "Unsafe fixture cleanup path: $resolved" }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}
if (-not $LegacyPass) {
    $legacy = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $legacy) {
        & $legacy -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $PSCommandPath -InstallerPath $InstallerPath -UnifiedInstallerPath $UnifiedInstallerPath -LegacyPass
        if ($LASTEXITCODE -ne 0) { throw 'Windows PowerShell 5.1 OpenCode install pass failed' }
    }
}
