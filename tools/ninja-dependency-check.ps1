# Functions shared by the build entry point and its regression test.
function Assert-NinjaDependencyRecords {
    param(
        [Parameter(Mandatory)][string[]]$Objects,
        [Parameter(Mandatory)][AllowEmptyCollection()][AllowEmptyString()][string[]]$Lines,
        [hashtable]$RequiredHeaders = @{}
    )
    $records = @{}
    $current = $null
    foreach ($line in $Lines) {
        if ($line -match '^(CMakeFiles/.*\.cpp\.obj): (.*)$') {
            $current = $Matches[1]
            if ($records.ContainsKey($current)) { throw "Duplicate Ninja dependency record: $current" }
            $records[$current] = @{ status = $Matches[2]; dependencies = [Collections.Generic.List[string]]::new() }
        } elseif ($current -and $line -match '^\s+(.+?)\s*$') {
            $records[$current].dependencies.Add($Matches[1])
        }
    }
    foreach ($object in $Objects) {
        if (-not $records.ContainsKey($object)) { throw "Missing Ninja dependency record for $object; rebuild with -Clean." }
        $record = $records[$object]
        if ($record.status -notmatch '^#deps\s+[1-9][0-9]*,.*\(VALID\)$') {
            throw "Missing or stale Ninja header dependencies for $object; rebuild with -Clean."
        }
        $reportedCount = [int]($record.status -replace '^#deps\s+([0-9]+),.*$', '$1')
        if ($record.dependencies.Count -ne $reportedCount) {
            throw "Incomplete Ninja dependency record for $object; rebuild with -Clean."
        }
        if ($RequiredHeaders.ContainsKey($object)) {
            $header = $RequiredHeaders[$object]
            $found = @($record.dependencies | Where-Object { [IO.Path]::GetFileName($_) -eq $header }).Count -gt 0
            if (-not $found) { throw "Missing $header dependency for $object; rebuild with -Clean." }
        }
    }
}
