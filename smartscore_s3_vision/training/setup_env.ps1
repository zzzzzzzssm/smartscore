param(
    [switch]$CpuOnly
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$VenvPath = Join-Path $ProjectRoot ".venv-train-v2"
$PythonPath = Join-Path $VenvPath "Scripts\python.exe"

if (-not (Test-Path -LiteralPath $PythonPath)) {
    $PyLauncher = Get-Command py -ErrorAction SilentlyContinue
    if ($null -ne $PyLauncher) {
        & $PyLauncher.Source -3.11 -m venv $VenvPath
    } else {
        $IdfPython = Join-Path $env:IDF_TOOLS_PATH "tools\idf-python\3.11.2\python.exe"
        if (-not (Test-Path -LiteralPath $IdfPython)) {
            throw "Python 3.11 was not found. Install it or export an ESP-IDF environment first."
        }
        & $IdfPython -m venv $VenvPath
    }
}

& $PythonPath -m pip install --upgrade pip setuptools wheel
if ($CpuOnly) {
    & $PythonPath -m pip install torch==2.10.0 torchvision==0.25.0 --index-url https://download.pytorch.org/whl/cpu
} else {
    & $PythonPath -m pip install torch==2.10.0 torchvision==0.25.0 --index-url https://download.pytorch.org/whl/cu126
}
& $PythonPath -m pip install -r (Join-Path $PSScriptRoot "requirements.txt")

& $PythonPath -c "import torch; print('torch=', torch.__version__); print('cuda=', torch.cuda.is_available()); print('device=', torch.cuda.get_device_name(0) if torch.cuda.is_available() else 'CPU')"
Write-Host "Training environment ready: $PythonPath"
