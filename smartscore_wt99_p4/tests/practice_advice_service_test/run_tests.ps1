$ErrorActionPreference = 'Stop'

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($cmakeCommand) {
    $cmake = $cmakeCommand.Source
} else {
    $cmake = 'D:\Espressif\tools\cmake\3.30.2\bin\cmake.exe'
}
if (-not (Test-Path $cmake)) {
    throw 'CMake was not found.'
}

$buildDirectory = Join-Path $PSScriptRoot 'build'
& $cmake -S $PSScriptRoot -B $buildDirectory `
    -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$testExecutable = Join-Path $buildDirectory 'Release\practice_advice_state_host_tests.exe'
& $testExecutable
exit $LASTEXITCODE
