from __future__ import annotations

import argparse
from pathlib import Path

import torch
from PIL import Image
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
from torch.utils.data import DataLoader, Dataset
from torchvision import transforms

from training.common import (
    CLASS_NAMES,
    IMAGENET_MEAN,
    IMAGENET_STD,
    Sample,
    count_by_person_and_class,
    load_manifest,
    write_json,
)
from training.modeling import load_checkpoint


class EvaluationDataset(Dataset[tuple[torch.Tensor, int]]):
    def __init__(self, samples: list[Sample], image_size: int) -> None:
        self.samples = samples
        self.transform = transforms.Compose(
            [
                transforms.Resize((image_size, image_size), antialias=True),
                transforms.ToTensor(),
                transforms.Normalize(IMAGENET_MEAN, IMAGENET_STD),
            ]
        )

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, int]:
        sample = self.samples[index]
        with Image.open(sample.path) as image:
            tensor = self.transform(image.convert("RGB"))
        return tensor, sample.target


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Evaluate the final model on an independent person dataset.")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--device", choices=("auto", "cpu", "cuda"), default="auto")
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    device_name = args.device
    if device_name == "auto":
        device_name = "cuda" if torch.cuda.is_available() else "cpu"
    if device_name == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is unavailable")
    device = torch.device(device_name)
    samples = load_manifest(args.manifest.resolve())
    model, checkpoint = load_checkpoint(args.checkpoint.resolve(), device)
    image_size = int(checkpoint["image_size"])
    loader = DataLoader(
        EvaluationDataset(samples, image_size),
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=0,
    )
    predictions: list[int] = []
    targets: list[int] = []
    with torch.inference_mode():
        for images, labels in loader:
            logits = model(images.to(device))
            predictions.extend(logits.argmax(dim=1).cpu().tolist())
            targets.extend(labels.tolist())

    report = classification_report(
        targets,
        predictions,
        labels=list(range(len(CLASS_NAMES))),
        target_names=CLASS_NAMES,
        output_dict=True,
        zero_division=0,
    )
    result = {
        "checkpoint": str(args.checkpoint.resolve()),
        "manifest": str(args.manifest.resolve()),
        "device": str(device),
        "samples": len(samples),
        "sample_counts": count_by_person_and_class(samples),
        "accuracy": float(accuracy_score(targets, predictions)),
        "macro_f1": float(f1_score(targets, predictions, average="macro")),
        "classification_report": report,
        "confusion_matrix": confusion_matrix(
            targets,
            predictions,
            labels=list(range(len(CLASS_NAMES))),
        ).tolist(),
        "class_names": CLASS_NAMES,
    }
    write_json(args.output.resolve(), result)
    print(result, flush=True)


if __name__ == "__main__":
    main()
