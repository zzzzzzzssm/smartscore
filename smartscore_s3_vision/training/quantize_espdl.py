from __future__ import annotations

import argparse
import hashlib
import math
import random
from pathlib import Path
from typing import Sequence

import numpy as np
import onnxruntime as ort
import torch
from PIL import Image
from sklearn.metrics import accuracy_score, f1_score
from torch.utils.data import DataLoader, Dataset
from torchvision import transforms

from esp_ppq.api import espdl_quantize_onnx
from esp_ppq.executor import TorchExecutor

from training.common import (
    CLASS_NAMES,
    IMAGENET_MEAN,
    IMAGENET_STD,
    SEED,
    Sample,
    load_manifest,
    set_seed,
    write_json,
)


class CalibrationDataset(Dataset[torch.Tensor]):
    def __init__(self, samples: Sequence[Sample], image_size: int) -> None:
        self.samples = list(samples)
        self.transform = transforms.Compose(
            [
                transforms.Resize((image_size, image_size), antialias=True),
                transforms.ToTensor(),
                transforms.Normalize(IMAGENET_MEAN, IMAGENET_STD),
            ]
        )

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> torch.Tensor:
        with Image.open(self.samples[index].path) as image:
            return self.transform(image.convert("RGB"))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Quantize the direction ONNX model for ESP32-S3.")
    parser.add_argument("--onnx", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--calibration-samples", type=int, default=128)
    parser.add_argument("--evaluation-samples", type=int, default=600)
    parser.add_argument("--bits", type=int, choices=(8, 16), default=16)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    parser.add_argument("--error-report", action="store_true")
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def choose_device(value: str) -> torch.device:
    if value == "auto":
        value = "cuda" if torch.cuda.is_available() else "cpu"
    if value == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is unavailable")
    return torch.device(value)


def balanced_sample(samples: Sequence[Sample], count: int, seed: int) -> list[Sample]:
    if count <= 0:
        raise ValueError("sample count must be positive")
    rng = random.Random(seed)
    buckets: dict[tuple[str, str], list[Sample]] = {}
    for sample in samples:
        buckets.setdefault((sample.person, sample.label), []).append(sample)
    for bucket in buckets.values():
        rng.shuffle(bucket)

    keys = sorted(buckets)
    selected: list[Sample] = []
    cursors = {key: 0 for key in keys}
    while len(selected) < min(count, len(samples)):
        made_progress = False
        for key in keys:
            cursor = cursors[key]
            if cursor < len(buckets[key]):
                selected.append(buckets[key][cursor])
                cursors[key] += 1
                made_progress = True
                if len(selected) >= count:
                    break
        if not made_progress:
            break
    return selected


def evaluate_quantized_graph(
    graph: object,
    onnx_path: Path,
    samples: Sequence[Sample],
    device: torch.device,
    image_size: int,
) -> dict[str, float | int]:
    dataset = CalibrationDataset(samples, image_size)
    float_session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    quant_executor = TorchExecutor(graph=graph, device=str(device))
    float_predictions: list[int] = []
    quant_predictions: list[int] = []
    targets: list[int] = []
    absolute_errors: list[float] = []

    with torch.inference_mode():
        for index, sample in enumerate(samples):
            tensor = dataset[index].unsqueeze(0)
            float_logits = float_session.run(["logits"], {"input": tensor.numpy()})[0]
            quant_logits = quant_executor.forward(tensor.to(device))[0].detach().cpu().numpy()
            float_predictions.append(int(np.argmax(float_logits, axis=1)[0]))
            quant_predictions.append(int(np.argmax(quant_logits, axis=1)[0]))
            targets.append(sample.target)
            absolute_errors.append(float(np.max(np.abs(float_logits - quant_logits))))

    return {
        "samples": len(samples),
        "float_accuracy": float(accuracy_score(targets, float_predictions)),
        "quantized_accuracy": float(accuracy_score(targets, quant_predictions)),
        "float_macro_f1": float(f1_score(targets, float_predictions, average="macro")),
        "quantized_macro_f1": float(f1_score(targets, quant_predictions, average="macro")),
        "prediction_agreement": float(np.mean(np.equal(float_predictions, quant_predictions))),
        "maximum_logit_absolute_error": max(absolute_errors, default=math.nan),
    }


def main() -> None:
    args = parse_args()
    set_seed(SEED)
    onnx_path = args.onnx.resolve()
    manifest_path = args.manifest.resolve()
    output_path = args.output.resolve()
    if not onnx_path.is_file():
        raise FileNotFoundError(f"ONNX model not found: {onnx_path}")
    onnx_session = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    input_shape = onnx_session.get_inputs()[0].shape
    if len(input_shape) != 4 or input_shape[0] != 1 or input_shape[1] != 3 or input_shape[2] != input_shape[3]:
        raise ValueError(f"expected fixed square NCHW input, got {input_shape}")
    image_size = int(input_shape[2])
    samples = [
        sample
        for sample in load_manifest(manifest_path)
        if sample.warning != "landmark_direction_disagrees"
    ]
    calibration_samples = balanced_sample(samples, args.calibration_samples, SEED)
    evaluation_samples = balanced_sample(samples, args.evaluation_samples, SEED + 1)
    device = choose_device(args.device)
    calibration_loader = DataLoader(
        CalibrationDataset(calibration_samples, image_size),
        batch_size=1,
        shuffle=False,
        num_workers=0,
    )
    output_path.parent.mkdir(parents=True, exist_ok=True)
    print(
        f"quantizing target=esp32s3 bits={args.bits} device={device} calibration={len(calibration_samples)}",
        flush=True,
    )
    graph = espdl_quantize_onnx(
        onnx_import_file=str(onnx_path),
        espdl_export_file=str(output_path),
        calib_dataloader=calibration_loader,
        calib_steps=len(calibration_samples),
        input_shape=[1, 3, image_size, image_size],
        target="esp32s3",
        num_of_bits=args.bits,
        device=str(device),
        error_report=args.error_report,
        export_config=True,
        export_test_values=True,
        metadata_props={
            "model": "smartscore_point_direction",
            "classes": ",".join(CLASS_NAMES),
            "input_layout": "NCHW",
            "input_color": "RGB",
        },
    )
    if not output_path.is_file():
        raise RuntimeError(f"ESP-DL exporter did not create {output_path}")
    evaluation = evaluate_quantized_graph(graph, onnx_path, evaluation_samples, device, image_size)
    metadata = {
        "onnx": str(onnx_path),
        "espdl": str(output_path),
        "sha256": sha256(output_path),
        "target": "esp32s3",
        "bits": args.bits,
        "input_shape": [1, 3, image_size, image_size],
        "input_layout": "NCHW",
        "input_color": "RGB",
        "normalization_mean": IMAGENET_MEAN,
        "normalization_std": IMAGENET_STD,
        "class_names": CLASS_NAMES,
        "calibration_samples": len(calibration_samples),
        "evaluation": evaluation,
    }
    write_json(output_path.with_suffix(".espdl.json"), metadata)
    print(metadata, flush=True)


if __name__ == "__main__":
    main()
