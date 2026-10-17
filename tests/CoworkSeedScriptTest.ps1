param(
    [Parameter(Mandatory)][string]$ScriptPath
)

# tools\seed-cowork-hooks.ps1 is the manual fallback for the Claude-Desktop
# per-session seeding, and -- since 1.0.1 -- the uninstaller's way of taking those
# entries back out again. That makes two promises worth pinning at the script level
# (the in-process seeder's own merge rules are covered by CoworkHookSeederTest):
# it is a guest in someone else's config file, and "-Remove" really leaves nothing
# behind, so "uninstalling leaves no trace" stays true.
$ErrorActionPreference = 'Stop'

$root = Join-Path ([IO.Path]::GetTempPath()) ('pet-cowork-seed-' + [guid]::NewGuid().ToString('N'))
$acct = Join-Path $root 'Claude-3p\local-agent-mode-sessions\acct\00000000'
$s1 = Join-Path $acct 's1\.claude'          # brand new conversation: no settings yet
$s2 = Join-Path $acct 's2\.claude'          # another tool's config we must not disturb
$s3 = Join-Path $acct 's3\.claude'          # a file we cannot parse: hands off
$helper = Join-Path $root 'fake\DesktopCompanionHook.exe'
$originalLocalAppData = $env:LOCALAPPDATA
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# Another tool's hooks live in the same file, including one group in a shared event
# ("Stop") and one in an event we do not manage at all ("PreToolUse"); the pet
# entry is a STALE path, as a re-install would find it.
$otherTool = '{"permissions":{"allow":["Bash(ls:*)"]},"env":{"FOO":"1"},' +
    '"hooks":{"PreToolUse":[{"hooks":[{"type":"command","command":"C:\\other\\guard.exe"}]}],' +
    '"Stop":[{"hooks":[{"type":"command","command":"C:\\other\\notify.exe"}]},' +
    '{"hooks":[{"type":"command","command":"\"C:\\old\\DesktopCompanionHook.exe\""}]}]}}'

function CountPetEntries([string]$Path) {
    (Select-String -LiteralPath $Path -Pattern 'DesktopCompanionHook' -AllMatches).Matches.Count
}
function Assert([bool]$condition, [string]$message) {
    if (-not $condition) { throw $message }
}

try {
    New-Item -ItemType Directory -Path (Split-Path $helper) -Force | Out-Null
    New-Item -ItemType File -Path $helper | Out-Null
    New-Item -ItemType Directory -Path $s1, $s2, $s3 -Force | Out-Null
    [IO.File]::WriteAllText((Join-Path $s2 'settings.json'), $otherTool, $utf8NoBom)
    [IO.File]::WriteAllText((Join-Path $s3 'settings.json'), '{ not json', $utf8NoBom)

    # Redirect the script's scan root at the throwaway tree for the whole run, so a
    # test never writes into the real session configs.
    $env:LOCALAPPDATA = $root

    & $ScriptPath -HelperPath $helper

    $settings1 = Join-Path $s1 'settings.json'
    $settings2 = Join-Path $s2 'settings.json'
    Assert (Test-Path -LiteralPath $settings1) 'a new conversation must get a settings.json'
    Assert ((CountPetEntries $settings1) -eq 3) 'each managed event must get exactly one pet entry'

    $bytes2 = [IO.File]::ReadAllBytes($settings2)
    Assert (-not ($bytes2[0] -eq 0xEF -and $bytes2[1] -eq 0xBB -and $bytes2[2] -eq 0xBF)) `
        'settings.json must not carry a UTF-8 BOM'
    Assert ((CountPetEntries $settings2) -eq 3) 'a stale pet entry must be replaced, not stacked'
    $seeded = [IO.File]::ReadAllText($settings2, [Text.Encoding]::UTF8)
    Assert ($seeded -match 'Bash\(ls:\*\)') 'permissions must survive seeding'
    Assert ($seeded -match '"FOO"') 'env must survive seeding'
    Assert ($seeded -match 'guard\.exe') 'an event we do not manage must survive seeding'
    Assert ($seeded -match 'notify\.exe') 'another tool''s group in a shared event must survive'
    Assert ($seeded -notmatch 'C:\\\\old\\\\DesktopCompanionHook') 'the stale helper path must be gone'
    Assert (([IO.File]::ReadAllText((Join-Path $s3 'settings.json'), [Text.Encoding]::UTF8)) -eq '{ not json') `
        'a config we cannot parse must not be rewritten'

    & $ScriptPath -Remove

    $removed1 = [IO.File]::ReadAllText($settings1, [Text.Encoding]::UTF8)
    Assert ((CountPetEntries $settings1) -eq 0) 'Remove must take our entries out of a seeded file'
    Assert ($removed1 -notmatch '"hooks"') 'an emptied hooks object must go too, not linger as {}'
    Assert ($null -ne ($removed1 | ConvertFrom-Json)) 'the file must stay valid JSON'

    $removed2 = [IO.File]::ReadAllText($settings2, [Text.Encoding]::UTF8)
    Assert ((CountPetEntries $settings2) -eq 0) 'Remove must take our entries out of a shared file'
    Assert ($removed2 -match 'Bash\(ls:\*\)' -and $removed2 -match '"FOO"') 'Remove must keep every other key'
    Assert ($removed2 -match 'guard\.exe' -and $removed2 -match 'notify\.exe') 'Remove must keep other tools'' hooks'
    Assert ($removed2 -notmatch '"Stop"\s*:\s*\[\s*\]') 'an emptied event must be dropped, not left as an empty array'
    Assert ($null -ne ($removed2 | ConvertFrom-Json)) 'the file must stay valid JSON'
    Assert (([IO.File]::ReadAllText((Join-Path $s3 'settings.json'), [Text.Encoding]::UTF8)) -eq '{ not json') `
        'Remove must not rewrite a config we cannot parse'

    # The uninstaller may run more than once (a repair, a manual re-run): the second
    # pass must not touch a file it has nothing to do in.
    $before = (Get-FileHash -LiteralPath $settings2 -Algorithm SHA256).Hash
    & $ScriptPath -Remove
    Assert ((Get-FileHash -LiteralPath $settings2 -Algorithm SHA256).Hash -eq $before) `
        'a second Remove must leave the file byte-for-byte alone'

    # And the same script has to survive the host the installer actually uses:
    # Windows PowerShell 5.1, whose ConvertTo-Json and file encoding differ from
    # the pwsh this test usually runs under.
    $legacyShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $legacyShell) {
        $legacyArgs = @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $ScriptPath)
        & $legacyShell @legacyArgs '-HelperPath' $helper
        $legacyBytes = [IO.File]::ReadAllBytes($settings2)
        Assert (-not ($legacyBytes[0] -eq 0xEF -and $legacyBytes[1] -eq 0xBB -and $legacyBytes[2] -eq 0xBF)) `
            'Windows PowerShell 5.1 wrote settings.json with a UTF-8 BOM'
        Assert ((CountPetEntries $settings2) -eq 3) '5.1 must seed the managed events'
        & $legacyShell @legacyArgs '-Remove'
        Assert ((CountPetEntries $settings2) -eq 0) '5.1 must be able to remove them again'
        Assert ((CountPetEntries $settings1) -eq 0) '5.1 must be able to remove them again (new conversation)'
    } else {
        Write-Warning 'Windows PowerShell 5.1 not present; skipped the installer-host pass'
    }

    Write-Host 'seed/remove round trip: preserved other tools, dropped our own, no BOM'
} finally {
    $env:LOCALAPPDATA = $originalLocalAppData
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
