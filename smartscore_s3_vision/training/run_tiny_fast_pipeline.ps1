param(
    [ValidateSet("train", "export", "quantize", "validate", "deploy", "all")]
    [string]$Step = "all"
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Python = Join-Path $ProjectRoot ".venv-train-v2\Scripts\python.exe"
$Manifest = Join-Path $ProjectRoot "training_data\processed_v2\manifest.csv"
$TeacherRoot = Join-Path $ProjectRoot "training_artifacts_v2"
$Artifacts = Join-Path $ProjectRoot "training_artifacts_tiny_fast\run_96"
$Final = Join-Path $Artifacts "final"

if (-not (Test-Path -LiteralPath $Python)) {
    throw "Training environment is missing: $Python"
}
if (-not (Test-Path -LiteralPath $Manifest)) {
    throw "Processed training manifest is missing: $Manifest"
}

Push-Location $ProjectRoot
try {
    $env:MPLCONFIGDIR = Join-Path $PSScriptRoot ".mplcache"
    if ($Step -in @("train", "all")) {
        & $Python -m unittest training.test_common training.test_train_v2
        & $Python -m training.train_v2 `
            --manifest $Manifest `
            --output $Artifacts `
            --mode all `
            --candidates tiny_point_cnn mobilenetv3_small_050.lamb_in1k mobilenetv3_small_075.lamb_in1k `
            --image-size 96 `
            --teacher-root $TeacherRoot `
            --device auto `
            --resume
    }
    if ($Step -in @("export", "all")) {
        & $Python -m training.export_onnx `
            --checkpoint (Join-Path $Final "point_direction.pth") `
            --output (Join-Path $Final "point_direction.onnx")
    }
    if ($Step -in @("quantize", "all")) {
        $Int8Model = Join-Path $Final "point_direction_int8.espdl"
        & $Python -m training.quantize_espdl `
            --onnx (Join-Path $Final "point_direction.onnx") `
            --manifest $Manifest `
            --output $Int8Model `
            --calibration-samples 256 `
            --evaluation-samples 1200 `
            --device auto `
            --bits 8
    }
    if ($Step -in @("validate", "all")) {
        $Selection = Get-Content -Raw (Join-Path $Artifacts "candidate_selection.json") | ConvertFrom-Json
        if (-not $Selection.winner) {
            throw "No candidate passed the cross-validation gates."
        }
        $WinnerDirectory = $Selection.winner.Replace(".", "_").Replace("/", "_")
        $CvSummary = Join-Path $Artifacts "candidates\$WinnerDirectory\summary.json"
        & $Python -m training.validate_deployment `
            --cv-summary $CvSummary `
            --quant-summary (Join-Path $Final "point_direction_int8.espdl.json") `
            --model (Join-Path $Final "point_direction_int8.espdl") `
            --output (Join-Path $Final "deployment_gate.json")
    }
    if ($Step -in @("deploy", "all")) {
        $Gate = Get-Content -Raw (Join-Path $Final "deployment_gate.json") | ConvertFrom-Json
        if (-not $Gate.passed) {
            throw "Deployment gate did not pass; firmware model was not changed."
        }
        Copy-Item `
            -LiteralPath (Join-Path $Final "point_direction_int8.espdl") `
            -Destination (Join-Path $ProjectRoot "main\models\point_direction.espdl") `
            -Force
        Write-Host "Validated TinyPointNet model deployed to firmware."
    }
} finally {
    Pop-Location
}
