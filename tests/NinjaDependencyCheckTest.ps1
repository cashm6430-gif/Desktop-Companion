$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../tools/ninja-dependency-check.ps1')
$first = 'CMakeFiles/DesktopCompanion.dir/src/main.cpp.obj'
$second = 'CMakeFiles/Example.dir/src/Example.cpp.obj'
$required = @{ $first = 'PetController.h' }
function Check-Fails([string]$Name, [string[]]$Lines) {
    $failed = $false
    try { Assert-NinjaDependencyRecords -Objects @($first, $second) -Lines $Lines -RequiredHeaders $required }
    catch { $failed = $true }
    if (-not $failed) { throw "Expected failure: $Name" }
}
$valid = @(
    "${first}: #deps 2, deps mtime 10 (VALID)",
    '    ../src/PetController.h',
    '    ../src/PetWindow.h',
    "${second}: #deps 1, deps mtime 11 (VALID)",
    '    ../src/Example.h'
)
Assert-NinjaDependencyRecords -Objects @($first, $second) -Lines $valid -RequiredHeaders $required
Check-Fails 'missing record' $valid[0..2]
Check-Fails 'missing critical header' @($valid[0], $valid[2], $valid[3], $valid[4])
Check-Fails 'zero dependencies' @($valid[0], $valid[1], $valid[2], "${second}: #deps 0, deps mtime 11 (VALID)")
Check-Fails 'stale record' @($valid[0], $valid[1], $valid[2], "${second}: #deps 1, deps mtime 11 (STALE)", $valid[4])
Check-Fails 'truncated record' @($valid[0], $valid[1], $valid[3], $valid[4])
Check-Fails 'duplicate record' ($valid + $valid[3..4])
Write-Host 'Ninja dependency validation: 7 cases passed.'
