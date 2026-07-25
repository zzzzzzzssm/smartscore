param(
    [ValidateSet("prepare", "train", "export", "quantize", "deploy", "external", "all")]
    [string]$Step = "all",
    [switch]$RebuildCrops
)

$ErrorActionPreference = "Stop"
$ProjectRoot = Split-Path -Parent $PSScriptRoot
$Python = Join-Path $ProjectRoot ".venv-train-v2\Scripts\python.exe"
$Raw = Join-Path $ProjectRoot "training_data\point_dataset"
$Processed = Join-Path $ProjectRoot "training_data\processed_v2"
$Artifacts = Join-Path $ProjectRoot "training_artifacts_v2"

if (-not (Test-Path -LiteralPath $Python)) {
    throw "Training environment is missing. Run training\setup_env.ps1 first."
}

Push-Location $ProjectRoot
try {
    $env:MPLCONFIGDIR = Join-Path $PSScriptRoot ".mplcache"
    if ($Step -in @("prepare", "all")) {
        $Arguments = @(
            "-m", "training.prepare_dataset",
            "--input", $Raw,
            "--output", $Processed,
            "--processed-size", "192",
            "--near-duplicate-mae", "1.25"
        )
        if ($RebuildCrops) {
            $Arguments += "--overwrite"
        }
        if ($RebuildCrops -or -not (Test-Path -LiteralPath (Join-Path $Processed "manifest.csv"))) {
            & $Python @Arguments
        } else {
            Write-Host "Processed dataset already exists; reusing $Processed"
        }
    }
    if ($Step -in @("train", "all")) {
        & $Python -m unittest training.test_common training.test_train_v2
        & $Python -m training.train_v2 --manifest (Join-Path $Processed "manifest.csv") --output $Artifacts --mode all --device auto --resume
    }
    if ($Step -in @("export", "all")) {
        & $Python -m training.export_onnx --checkpoint (Join-Path $Artifacts "final\point_direction.pth") --output (Join-Path $Artifacts "final\point_direction.onnx")
    }
    if ($Step -in @("quantize", "all")) {
        & $Python -m training.quantize_espdl --onnx (Join-Path $Artifacts "final\point_direction.onnx") --manifest (Join-Path $Processed "manifest.csv") --output (Join-Path $Artifacts "final\point_direction_int16.espdl") --calibration-samples 256 --evaluation-samples 1200 --device auto --bits 16
    }
    if ($Step -in @("deploy", "all")) {
        $ModelSource = Join-Path $Artifacts "final\point_direction_int16.espdl"
        $ModelDestination = Join-Path $ProjectRoot "main\models\point_direction.espdl"
        if (-not (Test-Path -LiteralPath $ModelSource)) {
            throw "Quantized model is missing: $ModelSource"
        }
        Copy-Item -LiteralPath $ModelSource -Destination $ModelDestination -Force
        Write-Host "Firmware model updated: $ModelDestination"
    }
    if ($Step -eq "external") {
        $ExternalRaw = Join-Path $ProjectRoot "training_data\external_test"
        $ExternalProcessed = Join-Path $ProjectRoot "training_data\external_processed"
        & $Python -m training.prepare_dataset --input $ExternalRaw --output $ExternalProcessed --processed-size 192 --near-duplicate-mae 1.25 --overwrite
        & $Python -m training.evaluate --checkpoint (Join-Path $Artifacts "final\point_direction.pth") --manifest (Join-Path $ExternalProcessed "manifest.csv") --output (Join-Path $Artifacts "external_test\metrics.json") --device auto
    }
} finally {
    Pop-Location
}
