param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$IdfArguments
)

$env:IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.4'
$env:IDF_TOOLS_PATH = 'D:\Espressif'
$env:IDF_PYTHON_ENV_PATH = 'D:\Espressif\python_env\idf5.4_py3.12_env'
$env:IDF_SKIP_CHECK_SUBMODULES = '1'
$env:PATH = "D:\Espressif\python_env\idf5.4_py3.12_env\Scripts;$env:PATH"

. 'D:\Espressif\frameworks\esp-idf-v5.4\export.ps1'
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

idf.py @IdfArguments
exit $LASTEXITCODE
