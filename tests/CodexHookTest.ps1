param(
    [Parameter(Mandatory)][string]$HelperPath,
    [Parameter(Mandatory)][string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
$configPath = Join-Path ([IO.Path]::GetTempPath()) ("pet-hooks-test-" + [guid]::NewGuid() + '.json')
try {
    & $InstallerPath -TargetPath $configPath -HelperPath $HelperPath
    # Byte-level guard: a UTF-8 BOM (EF BB BF) makes Codex refuse the whole
    # hooks.json, and `Get-Content | ConvertFrom-Json` below would NOT notice it
    # (it tolerates a BOM), so the regression has to be caught here.
    $bytes = [IO.File]::ReadAllBytes($configPath)
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 0xEF -and $bytes[1] -eq 0xBB -and $bytes[2] -eq 0xBF) {
        throw 'hooks.json was written with a UTF-8 BOM; Codex cannot parse it'
    }
    # The regression only shows up under Windows PowerShell 5.1: its
    # `Set-Content -Encoding utf8` stamps a BOM, pwsh 7's does not -- and CI runs
    # this test on pwsh, so the host that actually broke has to be driven here.
    # Product usage is `powershell -File tools\install-hooks.ps1` (setup.exe and
    # the README), i.e. 5.1.
    $legacyShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    if (Test-Path -LiteralPath $legacyShell) {
        $legacyConfig = Join-Path ([IO.Path]::GetTempPath()) ("pet-hooks-test-ps51-" + [guid]::NewGuid() + '.json')
        try {
            $legacyArgs = @('-NoLogo', '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass',
                '-File', $InstallerPath, '-TargetPath', $legacyConfig, '-HelperPath', $HelperPath)
            & $legacyShell @legacyArgs | Out-Null
            $legacyBytes = [IO.File]::ReadAllBytes($legacyConfig)
            $hasBom = ($legacyBytes.Length -ge 3) -and ($legacyBytes[0] -eq 0xEF) -and ($legacyBytes[1] -eq 0xBB) -and ($legacyBytes[2] -eq 0xBF)
            if ($hasBom) { throw 'Windows PowerShell 5.1 wrote hooks.json with a UTF-8 BOM; Codex cannot parse it' }
        } finally {
            if (Test-Path -LiteralPath $legacyConfig) { Remove-Item -LiteralPath $legacyConfig }
        }
    }
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    foreach ($event in @('UserPromptSubmit', 'Stop', 'Interrupt', 'SessionEnd')) {
        $command = $config.hooks.$event[0].hooks[0].command
        if ($command -notmatch '^& ') { throw "$event must execute the helper in PowerShell" }
        $taskProcess = [Diagnostics.Process]::new()
        try {
            $taskProcess.StartInfo.FileName = (Get-Process -Id $PID).Path
            # Keep checking the installer's exact production command below,
            # but explicitly isolate these JSON-response executions from any
            # already running pet's default QLocalServer endpoint.
            $isolatedCommand = $command + ' --dry-run-hook'
            foreach ($arg in @('-NoLogo', '-NoProfile', '-NonInteractive', '-Command', $isolatedCommand)) {
                $taskProcess.StartInfo.ArgumentList.Add($arg)
            }
            $taskProcess.StartInfo.UseShellExecute = $false
            $taskProcess.StartInfo.CreateNoWindow = $true
            $taskProcess.StartInfo.RedirectStandardInput = $true
            $taskProcess.StartInfo.RedirectStandardOutput = $true
            $taskProcess.StartInfo.RedirectStandardError = $true
            [void]$taskProcess.Start()
            $payload = @{ hook_event_name = $event; session_id = 'hook-protocol-test'; turn_id = 'test' }
            $taskProcess.StandardInput.WriteLine(($payload | ConvertTo-Json -Compress))
            $taskProcess.StandardInput.Close()
            if (-not $taskProcess.WaitForExit(3000)) {
                $taskProcess.Kill($true)
                throw "$event exceeded the installed hook timeout"
            }
            $stdout = $taskProcess.StandardOutput.ReadToEnd()
            $stderr = $taskProcess.StandardError.ReadToEnd()
            if ($taskProcess.ExitCode -ne 0 -or $stderr.Trim()) {
                throw "$event failed: exit=$($taskProcess.ExitCode), stderr=$stderr"
            }
            # A single empty object is a successful observational hook response.
            if ($stdout.Trim() -ne '{}') { throw "$event emitted unexpected stdout: $stdout" }
            [void]($stdout | ConvertFrom-Json -ErrorAction Stop)
            Write-Host "$event : exit 0, valid JSON"
        } finally {
            $taskProcess.Dispose()
        }
    }
    # Check the configured executable too, rather than only the command prefix.
    $resolvedHelper = (Resolve-Path -LiteralPath $HelperPath).Path
    $expected = "& '" + $resolvedHelper.Replace("'", "''") + "' --client codex"
    if ($config.hooks.Stop[0].hooks[0].command -ne $expected) { throw 'Wrong hook executable' }

    # All writes stay in throwaway fixtures. New source metadata is generated
    # for each supported client; legacy commands still work without metadata.
    $multiClientInstaller = Join-Path (Split-Path -Parent $InstallerPath) 'install-hooks.ps1'
    foreach ($client in @('workbuddy', 'claude')) {
        $clientConfig = Join-Path ([IO.Path]::GetTempPath()) ("pet-hooks-test-$client-" + [guid]::NewGuid() + '.json')
        try {
            & $multiClientInstaller -Clients $client -TargetPath $clientConfig -HelperPath $HelperPath | Out-Null
            $clientBytes = [IO.File]::ReadAllBytes($clientConfig)
            if ($clientBytes.Length -ge 3 -and $clientBytes[0] -eq 0xEF -and $clientBytes[1] -eq 0xBB -and $clientBytes[2] -eq 0xBF) {
                throw "$client config has a UTF-8 BOM"
            }
            $clientHooks = Get-Content -LiteralPath $clientConfig -Raw | ConvertFrom-Json
            $expectedClientCommand = '"' + $resolvedHelper + '" --client ' + $client
            foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionEnd')) {
                if ($clientHooks.hooks.$event[0].hooks[0].command -ne $expectedClientCommand) {
                    throw "$client $event has wrong executable or source metadata"
                }
            }
        } finally {
            if (Test-Path -LiteralPath $clientConfig) { Remove-Item -LiteralPath $clientConfig }
        }
    }

    # Exercise the optional flags in both orders, including production's
    # `--client codex --dry-run-hook` tail, without touching the live endpoint.
    $argumentCases = @(
        @('--dry-run-hook'),
        @('--client', 'codex', '--dry-run-hook'),
        @('--dry-run-hook', '--client', 'workbuddy'),
        @('--client', 'claude', '--dry-run-hook'),
        @('--client', 'unrecognized', '--dry-run-hook')
    )
    foreach ($arguments in $argumentCases) {
        $probe = [Diagnostics.Process]::new()
        try {
            $probe.StartInfo.FileName = $resolvedHelper
            foreach ($argument in $arguments) { $probe.StartInfo.ArgumentList.Add($argument) }
            $probe.StartInfo.UseShellExecute = $false
            $probe.StartInfo.CreateNoWindow = $true
            $probe.StartInfo.RedirectStandardInput = $true
            $probe.StartInfo.RedirectStandardOutput = $true
            $probe.StartInfo.RedirectStandardError = $true
            [void]$probe.Start()
            $probe.StandardInput.WriteLine('{"hook_event_name":"UserPromptSubmit","session_id":"protocol-test"}')
            $probe.StandardInput.Close()
            if (-not $probe.WaitForExit(3000)) { $probe.Kill($true); throw 'dry-run flag combination timed out' }
            $stdout = $probe.StandardOutput.ReadToEnd()
            $stderr = $probe.StandardError.ReadToEnd()
            if ($probe.ExitCode -ne 0 -or $stderr.Trim() -or $stdout.Trim() -ne '{}') {
                throw "dry-run flag combination failed: exit=$($probe.ExitCode), stdout=$stdout, stderr=$stderr"
            }
            [void]($stdout | ConvertFrom-Json -ErrorAction Stop)
        } finally { $probe.Dispose() }
    }
} finally {
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath }
}
