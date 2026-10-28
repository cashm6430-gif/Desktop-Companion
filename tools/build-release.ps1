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
# layout even when the linked implementation was rebuilt. Fail loudly before
# an invalid binary is used; -Clean regenerates the dependency records.
$ninjaEntry = Select-String -LiteralPath $cache -Pattern '^CMAKE_MAKE_PROGRAM:FILEPATH=(.+)$' | Select-Object -First 1
if ($ninjaEntry -and $ninjaEntry.Matches[0].Groups[1].Value -match 'ninja(\.exe)?$') {
    $ninja = $ninjaEntry.Matches[0].Groups[1].Value
    $objects = @('CMakeFiles/DesktopCompanion.dir/src/main.cpp.obj',
        'CMakeFiles/HookLifecycleTest.dir/tests/HookLifecycleTest.cpp.obj',
        'CMakeFiles/ToonEventBridgeTest.dir/tests/ToonEventBridgeTest.cpp.obj')
    Push-Location $build
    try {
        foreach ($object in $objects) {
            if (-not (Test-Path -LiteralPath $object -PathType Leaf)) { continue }
            $deps = (& $ninja -t deps $object) -join "`n"
            if ($LASTEXITCODE -ne 0 -or $deps -notmatch '#deps\s+[1-9][0-9]*' -or
                $deps -notmatch '(?m)PetController\.h\s*$' -or $deps -match '\(STALE\)') {
                throw "Missing or stale Ninja header dependencies for $object; rebuild with -Clean."
            }
        }
    } finally { Pop-Location }
}
if ($Test) {
    $ctest = Join-Path (Split-Path -Parent $CMake) 'ctest.exe'
    & $ctest --test-dir $build --output-on-failure
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}
Write-Host '[build] Release build and recorded header dependencies verified.'
