from __future__ import annotations

import argparse
import hashlib
from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import torch

from training.common import CLASS_NAMES, SEED, write_json
from training.modeling import load_checkpoint


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Export and verify the SmartScore direction ONNX model.")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    args = parse_args()
    checkpoint_path = args.checkpoint.resolve()
    output_path = args.output.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    device = torch.device("cpu")
    model, checkpoint = load_checkpoint(checkpoint_path, device)
    image_size = int(checkpoint["image_size"])
    torch.manual_seed(SEED)
    example = torch.randn(1, 3, image_size, image_size, dtype=torch.float32)
    with torch.inference_mode():
        torch_output = model(example).numpy()

    torch.onnx.export(
        model,
        example,
        output_path,
        export_params=True,
        opset_version=13,
        do_constant_folding=True,
        input_names=["input"],
        output_names=["logits"],
        dynamic_axes=None,
        dynamo=False,
    )
    onnx_model = onnx.load(output_path)
    onnx.checker.check_model(onnx_model)
    session = ort.InferenceSession(str(output_path), providers=["CPUExecutionProvider"])
    onnx_output = session.run(["logits"], {"input": example.numpy()})[0]
    maximum_absolute_error = float(np.max(np.abs(torch_output - onnx_output)))
    if maximum_absolute_error > 1.0e-4:
        raise RuntimeError(f"ONNX parity check failed: max_abs_error={maximum_absolute_error}")

    metadata = {
        "checkpoint": str(checkpoint_path),
        "onnx": str(output_path),
        "sha256": sha256(output_path),
        "opset": 13,
        "input_name": "input",
        "input_shape": [1, 3, image_size, image_size],
        "output_name": "logits",
        "class_names": CLASS_NAMES,
        "action_threshold": checkpoint.get("action_threshold"),
        "maximum_absolute_error": maximum_absolute_error,
    }
    write_json(output_path.with_suffix(".onnx.json"), metadata)
    print(metadata)


if __name__ == "__main__":
    main()
