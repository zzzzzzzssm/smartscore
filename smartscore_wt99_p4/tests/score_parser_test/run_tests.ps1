$ErrorActionPreference = 'Stop'

$idfPath = $env:IDF_PATH
if (-not $idfPath -or -not (Test-Path (Join-Path $idfPath 'components\json\cJSON\cJSON.c'))) {
    $idfPath = 'D:\Espressif\frameworks\esp-idf-v5.4'
}
if (-not (Test-Path (Join-Path $idfPath 'components\json\cJSON\cJSON.c'))) {
    throw 'ESP-IDF cJSON source was not found. Set IDF_PATH and retry.'
}

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
$jsonDirectory = (Join-Path $idfPath 'components\json\cJSON').Replace('\', '/')
& $cmake -S $PSScriptRoot -B $buildDirectory `
    -G 'Visual Studio 17 2022' -A x64 `
    "-DIDF_JSON_DIR=$jsonDirectory"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $cmake --build $buildDirectory --config Release
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$testExecutable = Join-Path $buildDirectory 'Release\compact_score_host_tests.exe'
& $testExecutable
exit $LASTEXITCODE
