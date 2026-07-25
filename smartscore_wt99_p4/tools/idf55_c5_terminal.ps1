$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.5.3'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\python_env\idf5.5_py3.11_env'
$env:IDF_SKIP_CHECK_SUBMODULES = '1'
$env:PATH = "D:\Espressif\python_env\idf5.5_py3.11_env\Scripts;$env:PATH"

. 'D:\Espressif\frameworks\esp-idf-v5.5.3\export.ps1'
if (-not $?) {
    Write-Error "ESP-IDF 5.5.3 environment activation failed"
    return
}

# Pin idf.py to the ESP-IDF 5.5.3 Python entry point. This prevents an
# inherited ESP-IDF 5.1/5.4 environment or the EIM launcher taking priority.
function global:idf.py {
    & 'D:\Espressif\python_env\idf5.5_py3.11_env\Scripts\python.exe' `
      'D:\Espressif\frameworks\esp-idf-v5.5.3\tools\idf.py' @args
}

Set-Location 'D:\qianrushi\smartscore_wt99_p4\c5_hosted_slave'
Write-Host ''
Write-Host 'SmartScore WT99 ESP32-C5 ESP-IDF 5.5.3 terminal is ready.' -ForegroundColor Green
Write-Host 'Target project: c5_hosted_slave' -ForegroundColor Cyan
Write-Host 'Check: idf.py --version' -ForegroundColor Cyan
Write-Host 'Flash C5 on COM8: idf.py -p COM8 -b 115200 flash' -ForegroundColor Yellow
