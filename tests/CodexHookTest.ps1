param(
    [Parameter(Mandatory)][string]$HelperPath,
    [Parameter(Mandatory)][string]$InstallerPath
)

$ErrorActionPreference = 'Stop'
$configPath = Join-Path ([IO.Path]::GetTempPath()) ("pet-hooks-test-" + [guid]::NewGuid() + '.json')
try {
    & $InstallerPath -TargetPath $configPath -HelperPath $HelperPath
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    foreach ($event in @('UserPromptSubmit', 'Stop', 'Interrupt', 'SessionEnd')) {
        $command = $config.hooks.$event[0].hooks[0].command
        if ($command -notmatch '^& ') { throw "$event must execute the helper in PowerShell" }
        $taskProcess = [Diagnostics.Process]::new()
        try {
            $taskProcess.StartInfo.FileName = (Get-Process -Id $PID).Path
            foreach ($arg in @('-NoLogo', '-NoProfile', '-NonInteractive', '-Command', $command)) {
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
    $expected = "& '" + (Resolve-Path -LiteralPath $HelperPath).Path.Replace("'", "''") + "'"
    if ($config.hooks.Stop[0].hooks[0].command -ne $expected) { throw 'Wrong hook executable' }
} finally {
    if (Test-Path -LiteralPath $configPath) { Remove-Item -LiteralPath $configPath }
}
