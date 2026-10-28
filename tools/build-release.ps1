<#
.SYNOPSIS
  Build the configured Windows Release tree and check Ninja's header dependencies.
.DESCRIPTION
  Run from PowerShell, for example: pwsh -File tools/build-release.ps1 -Test
  Use -Clean after a compiler/environment change or a failed dependency check.
  Paths default to the local development installation and can be overridden.
  Signing is separate because the development certificate is machine-specific.
#>
param(
    [switch]$Clean,
    [switch]$Test,
    [string]$MsvcRoot = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231',
    [string]$SdkRoot = 'C:/Program Files (x86)/Windows Kits/10',
    [string]$SdkVersion = '10.0.26100.0',
    [string]$QtRuntime = 'E:/Qt/release/bin/bin',
    [string]$CMake = 'D:/tool/Cmake/bin/cmake.exe'
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$build = Join-Path $repo 'build'
$cache = Join-Path $build 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cache -PathType Leaf)) { throw 'Configure the build directory with CMake first.' }
$buildType = Select-String -LiteralPath $cache -Pattern '^CMAKE_BUILD_TYPE:STRING=Release$'
if (-not $buildType) { throw 'The configured build directory must use the Release configuration.' }
foreach ($path in @($MsvcRoot, $SdkRoot, $QtRuntime, $CMake)) {
    if (-not (Test-Path -LiteralPath $path)) { throw "Required build path does not exist: $path" }
}
$env:WindowsSdkVersion = $SdkVersion + '\'
$env:INCLUDE = "$MsvcRoot/include;$SdkRoot/Include/$SdkVersion/ucrt;$SdkRoot/Include/$SdkVersion/shared;$SdkRoot/Include/$SdkVersion/um;$SdkRoot/Include/$SdkVersion/winrt;$SdkRoot/Include/$SdkVersion/cppwinrt"
$env:LIB = "$MsvcRoot/lib/x64;$SdkRoot/Lib/$SdkVersion/ucrt/x64;$SdkRoot/Lib/$SdkVersion/um/x64"
$env:PATH = "$MsvcRoot/bin/Hostx64/x64;$SdkRoot/bin/$SdkVersion/x64;$QtRuntime;" + $env:PATH
$buildArgs = @('--build', $build, '-j', '4')
if ($Clean) { $buildArgs += '--clean-first' }
& $CMake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

# A missing header dependency can leave a caller compiled with an old class
# layout. Check every project source object, not just a few known casualties.
# Generated Qt resource and empty moc sources can legitimately have no headers.
. (Join-Path $PSScriptRoot 'ninja-dependency-check.ps1')
$ninjaEntry = Select-String -LiteralPath $cache -Pattern '^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$' | Select-Object -First 1
if (-not $ninjaEntry -or $ninjaEntry.Matches[0].Groups[1].Value -notmatch 'ninja(\.exe)?$') {
    throw 'This build entry point requires the Ninja generator for dependency verification.'
}
$ninja = $ninjaEntry.Matches[0].Groups[1].Value
$requiredHeaders = @{
    'CMakeFiles/DesktopCompanion.dir/src/main.cpp.obj' = 'PetController.h'
    'CMakeFiles/HookLifecycleTest.dir/tests/HookLifecycleTest.cpp.obj' = 'PetController.h'
    'CMakeFiles/ToonEventBridgeTest.dir/tests/ToonEventBridgeTest.cpp.obj' = 'PetController.h'
}
Push-Location $build
try {
    $targetLines = @(& $ninja -t targets all)
    if ($LASTEXITCODE -ne 0) { throw 'Could not enumerate Ninja targets.' }
    $objects = @($targetLines | ForEach-Object {
        if ($_ -match '^(CMakeFiles/[^:]+\.dir/(?:src|tests|tools)/[^:]+\.cpp\.obj): CXX_COMPILER') { $Matches[1] }
    })
    if ($objects.Count -eq 0 -or $objects -notcontains 'CMakeFiles/DesktopCompanion.dir/src/main.cpp.obj') {
        throw 'No expected project C++ objects were found in the Ninja build.'
    }
    foreach ($object in $objects) {
        if (-not (Test-Path -LiteralPath $object -PathType Leaf)) { throw "Missing built object: $object" }
    }
    # Chunk arguments to avoid Windows command-line limits as the project grows.
    for ($offset = 0; $offset -lt $objects.Count; $offset += 24) {
        $last = [Math]::Min($offset + 23, $objects.Count - 1)
        $chunk = @($objects[$offset..$last])
        $deps = @(& $ninja -t deps @chunk)
        if ($LASTEXITCODE -ne 0) { throw 'Could not read Ninja dependency records.' }
        Assert-NinjaDependencyRecords -Objects $chunk -Lines $deps -RequiredHeaders $requiredHeaders
    }
    Write-Host "[build] Verified Ninja dependencies for $($objects.Count) project C++ objects."
} finally { Pop-Location }
if ($Test) {
    $ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
    & $ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Host '[build] Release build and recorded header dependencies verified.'
