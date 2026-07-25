param(
    [string]$Port = "COM3",
    [int]$Baud = 115200,
    [string]$Label = "unlabelled"
)

$ErrorActionPreference = "Stop"
$scriptPath = Join-Path $PSScriptRoot "capture_music_log.py"
$idfPython = $null
if ($env:IDF_PYTHON_ENV_PATH) {
    $candidate = Join-Path $env:IDF_PYTHON_ENV_PATH "Scripts\python.exe"
    if (Test-Path -LiteralPath $candidate) {
        $idfPython = $candidate
    }
}
if (-not $idfPython) {
    $pythonCommand = Get-Command python -ErrorAction SilentlyContinue
    if ($pythonCommand) {
        $idfPython = $pythonCommand.Source
    }
}
if (-not $idfPython) {
    throw "Python was not found. Run this script from the ESP-IDF PowerShell environment."
}

& $idfPython $scriptPath --port $Port --baud $Baud --label $Label
exit $LASTEXITCODE
