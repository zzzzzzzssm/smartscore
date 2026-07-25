$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.4'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\python_env\idf5.4_py3.12_env'
$env:IDF_SKIP_CHECK_SUBMODULES = '1'
$env:PATH = "D:\Espressif\python_env\idf5.4_py3.12_env\Scripts;$env:PATH"

. 'D:\Espressif\frameworks\esp-idf-v5.4\export.ps1'
if ($LASTEXITCODE -ne 0) {
    Write-Error "ESP-IDF 5.4 environment activation failed"
    return
}

# The current Espressif installation also contains an EIM `idf.py.exe`
# launcher. Pin this shell to IDF 5.4's Python entry point so `idf.py`
# cannot resolve to the launcher from another setup.
function global:idf.py {
    & 'D:\Espressif\python_env\idf5.4_py3.12_env\Scripts\python.exe' `
      'D:\Espressif\frameworks\esp-idf-v5.4\tools\idf.py' @args
}

Set-Location 'D:\qianrushi\smartscore_wt99_p4'
Write-Host ''
Write-Host 'SmartScore WT99 ESP-IDF 5.4 terminal is ready.' -ForegroundColor Green
Write-Host 'Try: idf.py --version' -ForegroundColor Cyan
