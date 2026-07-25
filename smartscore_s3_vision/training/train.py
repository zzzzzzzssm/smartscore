from __future__ import annotations

import argparse
import csv
import math
import platform
import statistics
import sys
import time
from collections import Counter
from pathlib import Path
from typing import Sequence

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import sklearn
import timm
import torch
import torchvision
from PIL import Image, ImageOps
from sklearn.metrics import classification_report, confusion_matrix, f1_score, recall_score
from torch import nn
from torch.utils.data import DataLoader, Dataset
from torchvision import transforms

from training.common import (
    CLASS_NAMES,
    CLASS_TO_INDEX,
    IMAGE_SIZE,
    IMAGENET_MEAN,
    IMAGENET_STD,
    SEED,
    Sample,
    count_by_person_and_class,
    horizontally_flipped_target,
    load_manifest,
    set_seed,
    write_json,
)
from training.modeling import MODEL_NAME, create_model, freeze_backbone, unfreeze_all


class GestureDataset(Dataset):
    def __init__(self, samples: Sequence[Sample], *, training: bool) -> None:
        self.samples = list(samples)
        self.training = training
        self.train_transform = transforms.Compose(
            [
                transforms.RandomAffine(degrees=9, translate=(0.08, 0.08), scale=(0.90, 1.10), fill=127),
                transforms.ColorJitter(brightness=0.25, contrast=0.20, saturation=0.18, hue=0.03),
                transforms.Resize((IMAGE_SIZE, IMAGE_SIZE), antialias=True),
                transforms.ToTensor(),
                transforms.Normalize(IMAGENET_MEAN, IMAGENET_STD),
            ]
        )
        self.eval_transform = transforms.Compose(
            [
                transforms.Resize((IMAGE_SIZE, IMAGE_SIZE), antialias=True),
                transforms.ToTensor(),
                transforms.Normalize(IMAGENET_MEAN, IMAGENET_STD),
            ]
        )

    def __len__(self) -> int:
        return len(self.samples)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, int, int]:
        sample = self.samples[index]
        with Image.open(sample.path) as loaded:
            image = loaded.convert("RGB")
        target = sample.target
        if self.training and torch.rand(1).item() < 0.5:
            image = ImageOps.mirror(image)
            target = horizontally_flipped_target(target)
        transform = self.train_transform if self.training else self.eval_transform
        return transform(image), target, index


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train the SmartScore direction classifier.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("cv", "final", "all"), default="all")
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--stage1-epochs", type=int, default=4)
    parser.add_argument("--stage2-epochs", type=int, default=12)
    parser.add_argument("--stage1-lr", type=float, default=1.0e-3)
    parser.add_argument("--stage2-lr", type=float, default=2.0e-4)
    parser.add_argument("--weight-decay", type=float, default=1.0e-4)
    parser.add_argument("--device", choices=("auto", "cuda", "cpu"), default="auto")
    return parser.parse_args()


def select_device(requested: str) -> torch.device:
    if requested == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is not available")
    if requested == "cpu":
        return torch.device("cpu")
    return torch.device("cuda" if torch.cuda.is_available() else "cpu")


def make_loader(samples: Sequence[Sample], *, training: bool, args: argparse.Namespace, device: torch.device) -> DataLoader:
    generator = torch.Generator()
    generator.manual_seed(SEED)
    return DataLoader(
        GestureDataset(samples, training=training),
        batch_size=args.batch_size,
        shuffle=training,
        num_workers=args.workers,
        pin_memory=device.type == "cuda",
        persistent_workers=args.workers > 0,
        generator=generator,
    )


def run_epoch(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    criterion: nn.Module,
    optimizer: torch.optim.Optimizer | None,
    scaler: torch.amp.GradScaler | None,
) -> tuple[float, np.ndarray, np.ndarray, np.ndarray]:
    training = optimizer is not None
    model.train(training)
    total_loss = 0.0
    total_samples = 0
    all_targets: list[np.ndarray] = []
    all_probabilities: list[np.ndarray] = []
    all_indices: list[np.ndarray] = []

    for images, targets, indices in loader:
        images = images.to(device, non_blocking=True)
        targets = targets.to(device, non_blocking=True)
        if training:
            optimizer.zero_grad(set_to_none=True)
        with torch.set_grad_enabled(training):
            with torch.amp.autocast(device_type=device.type, enabled=device.type == "cuda"):
                logits = model(images)
                loss = criterion(logits, targets)
            if training:
                assert scaler is not None
                scaler.scale(loss).backward()
                scaler.step(optimizer)
                scaler.update()

        probabilities = torch.softmax(logits.detach(), dim=1)
        batch_size = targets.shape[0]
        total_loss += float(loss.detach()) * batch_size
        total_samples += batch_size
        all_targets.append(targets.detach().cpu().numpy())
        all_probabilities.append(probabilities.cpu().numpy())
        all_indices.append(indices.numpy())

    return (
        total_loss / max(total_samples, 1),
        np.concatenate(all_targets),
        np.concatenate(all_probabilities),
        np.concatenate(all_indices),
    )


def save_confusion(targets: np.ndarray, predictions: np.ndarray, path: Path, title: str) -> None:
    matrix = confusion_matrix(targets, predictions, labels=range(len(CLASS_NAMES)))
    figure, axis = plt.subplots(figsize=(6.5, 5.5))
    image = axis.imshow(matrix, cmap="Blues")
    axis.set_title(title)
    axis.set_xlabel("Predicted")
    axis.set_ylabel("Actual")
    axis.set_xticks(range(len(CLASS_NAMES)), CLASS_NAMES, rotation=20, ha="right")
    axis.set_yticks(range(len(CLASS_NAMES)), CLASS_NAMES)
    for row in range(matrix.shape[0]):
        for column in range(matrix.shape[1]):
            axis.text(column, row, str(matrix[row, column]), ha="center", va="center")
    figure.colorbar(image, ax=axis)
    figure.tight_layout()
    path.parent.mkdir(parents=True, exist_ok=True)
    figure.savefig(path, dpi=150)
    plt.close(figure)


def report_for(targets: np.ndarray, probabilities: np.ndarray) -> dict:
    predictions = probabilities.argmax(axis=1)
    report = classification_report(
        targets,
        predictions,
        labels=range(len(CLASS_NAMES)),
        target_names=CLASS_NAMES,
        output_dict=True,
        zero_division=0,
    )
    report["confusion_matrix"] = confusion_matrix(
        targets, predictions, labels=range(len(CLASS_NAMES))
    ).tolist()
    return report


def checkpoint_payload(model: nn.Module, *, epoch: int, best_score: float, held_out_person: str | None) -> dict:
    return {
        "state_dict": {key: value.detach().cpu() for key, value in model.state_dict().items()},
        "model_name": MODEL_NAME,
        "class_names": CLASS_NAMES,
        "image_size": IMAGE_SIZE,
        "mean": IMAGENET_MEAN,
        "std": IMAGENET_STD,
        "epoch": epoch,
        "best_score": best_score,
        "held_out_person": held_out_person,
        "seed": SEED,
    }


def fit_fold(
    train_samples: Sequence[Sample],
    validation_samples: Sequence[Sample],
    destination: Path,
    held_out_person: str,
    args: argparse.Namespace,
    device: torch.device,
) -> tuple[Path, int, np.ndarray, np.ndarray, np.ndarray]:
    set_seed()
    model = create_model(pretrained=True).to(device)
    criterion = nn.CrossEntropyLoss(label_smoothing=0.04)
    train_loader = make_loader(train_samples, training=True, args=args, device=device)
    validation_loader = make_loader(validation_samples, training=False, args=args, device=device)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    destination.mkdir(parents=True, exist_ok=True)
    checkpoint_path = destination / "best.pth"
    history: list[dict[str, float | int | str]] = []
    best_score = -math.inf
    best_epoch = 0
    global_epoch = 0

    stages = (
        ("head", args.stage1_epochs, args.stage1_lr, freeze_backbone),
        ("finetune", args.stage2_epochs, args.stage2_lr, unfreeze_all),
    )
    for stage_name, epoch_count, learning_rate, configure in stages:
        configure(model)
        optimizer = torch.optim.AdamW(
            (parameter for parameter in model.parameters() if parameter.requires_grad),
            lr=learning_rate,
            weight_decay=args.weight_decay,
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(epoch_count, 1))
        for _ in range(epoch_count):
            global_epoch += 1
            started = time.perf_counter()
            train_loss, train_targets, train_probabilities, _ = run_epoch(
                model, train_loader, device, criterion, optimizer, scaler
            )
            validation_loss, validation_targets, validation_probabilities, _ = run_epoch(
                model, validation_loader, device, criterion, None, None
            )
            train_predictions = train_probabilities.argmax(axis=1)
            validation_predictions = validation_probabilities.argmax(axis=1)
            train_f1 = f1_score(train_targets, train_predictions, average="macro", zero_division=0)
            validation_f1 = f1_score(
                validation_targets, validation_predictions, average="macro", zero_division=0
            )
            history.append(
                {
                    "epoch": global_epoch,
                    "stage": stage_name,
                    "train_loss": train_loss,
                    "validation_loss": validation_loss,
                    "train_macro_f1": train_f1,
                    "validation_macro_f1": validation_f1,
                    "learning_rate": optimizer.param_groups[0]["lr"],
                    "seconds": time.perf_counter() - started,
                }
            )
            print(
                f"[{held_out_person}] epoch={global_epoch:02d} stage={stage_name} "
                f"train_loss={train_loss:.4f} val_loss={validation_loss:.4f} "
                f"train_f1={train_f1:.4f} val_f1={validation_f1:.4f}",
                flush=True,
            )
            if validation_f1 > best_score:
                best_score = validation_f1
                best_epoch = global_epoch
                torch.save(
                    checkpoint_payload(
                        model, epoch=global_epoch, best_score=best_score, held_out_person=held_out_person
                    ),
                    checkpoint_path,
                )
            scheduler.step()

    write_json(destination / "history.json", history)
    checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=True)
    model.load_state_dict(checkpoint["state_dict"])
    _, targets, probabilities, indices = run_epoch(model, validation_loader, device, criterion, None, None)
    report = report_for(targets, probabilities)
    report["held_out_person"] = held_out_person
    report["best_epoch"] = best_epoch
    report["train_counts"] = count_by_person_and_class(train_samples)
    report["validation_counts"] = count_by_person_and_class(validation_samples)
    write_json(destination / "metrics.json", report)
    save_confusion(targets, probabilities.argmax(axis=1), destination / "confusion_matrix.png", held_out_person)
    return checkpoint_path, best_epoch, targets, probabilities, indices


def choose_action_threshold(targets: np.ndarray, probabilities: np.ndarray) -> dict[str, float]:
    other_index = CLASS_TO_INDEX["other"]
    candidates: list[dict[str, float]] = []
    top_indices = probabilities.argmax(axis=1)
    top_scores = probabilities.max(axis=1)
    for threshold in np.arange(0.50, 1.00, 0.01):
        actions = (top_indices != other_index) & (top_scores >= threshold)
        other_mask = targets == other_index
        false_action_rate = float(actions[other_mask].mean()) if other_mask.any() else 0.0
        pointing_mask = targets != other_index
        correct_actions = actions & (top_indices == targets)
        pointing_recall = float(correct_actions[pointing_mask].mean()) if pointing_mask.any() else 0.0
        candidates.append(
            {
                "threshold": round(float(threshold), 2),
                "other_false_action_rate": false_action_rate,
                "pointing_recall": pointing_recall,
            }
        )
    acceptable = [item for item in candidates if item["other_false_action_rate"] <= 0.02]
    if acceptable:
        selected = acceptable[0]
    else:
        selected = min(candidates, key=lambda item: (item["other_false_action_rate"], -item["pointing_recall"]))
    return {**selected, "target_other_false_action_rate": 0.02}


def save_cv_predictions(
    samples: Sequence[Sample],
    records: Sequence[tuple[np.ndarray, np.ndarray, np.ndarray, str]],
    path: Path,
) -> tuple[np.ndarray, np.ndarray]:
    all_targets: list[np.ndarray] = []
    all_probabilities: list[np.ndarray] = []
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        fields = ["held_out_person", "source_path", "processed_path", "target", *[f"p_{name}" for name in CLASS_NAMES]]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for targets, probabilities, indices, held_out_person in records:
            for target, probability, index in zip(targets, probabilities, indices, strict=True):
                sample = samples[int(index)]
                row = {
                    "held_out_person": held_out_person,
                    "source_path": str(sample.source_path or ""),
                    "processed_path": str(sample.path),
                    "target": CLASS_NAMES[int(target)],
                }
                row.update({f"p_{name}": float(probability[class_index]) for class_index, name in enumerate(CLASS_NAMES)})
                writer.writerow(row)
            all_targets.append(targets)
            all_probabilities.append(probabilities)
    return np.concatenate(all_targets), np.concatenate(all_probabilities)


def run_cross_validation(
    samples: Sequence[Sample], args: argparse.Namespace, device: torch.device
) -> tuple[list[int], dict[str, float]]:
    people = sorted({sample.person for sample in samples})
    if len(people) < 2:
        raise ValueError("cross validation requires at least two people")
    records: list[tuple[np.ndarray, np.ndarray, np.ndarray, str]] = []
    best_epochs: list[int] = []
    cv_root = args.output / "cross_validation"
    for held_out_person in people:
        train_samples = [sample for sample in samples if sample.person != held_out_person]
        validation_samples = [sample for sample in samples if sample.person == held_out_person]
        _, best_epoch, targets, probabilities, local_indices = fit_fold(
            train_samples,
            validation_samples,
            cv_root / held_out_person,
            held_out_person,
            args,
            device,
        )
        global_indices = np.asarray([samples.index(validation_samples[int(index)]) for index in local_indices])
        records.append((targets, probabilities, global_indices, held_out_person))
        best_epochs.append(best_epoch)

    targets, probabilities = save_cv_predictions(samples, records, cv_root / "predictions.csv")
    threshold = choose_action_threshold(targets, probabilities)
    report = report_for(targets, probabilities)
    report["selected_action_threshold"] = threshold
    report["best_epochs"] = best_epochs
    write_json(cv_root / "summary.json", report)
    save_confusion(targets, probabilities.argmax(axis=1), cv_root / "confusion_matrix_all.png", "All held-out people")
    print(f"selected action threshold: {threshold}")
    return best_epochs, threshold


def train_final(
    samples: Sequence[Sample],
    args: argparse.Namespace,
    device: torch.device,
    best_epochs: Sequence[int] | None,
    threshold: dict[str, float] | None,
) -> Path:
    set_seed()
    destination = args.output / "final"
    destination.mkdir(parents=True, exist_ok=True)
    model = create_model(pretrained=True).to(device)
    criterion = nn.CrossEntropyLoss(label_smoothing=0.04)
    loader = make_loader(samples, training=True, args=args, device=device)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    if best_epochs:
        total_epochs = max(1, int(round(statistics.median(best_epochs))))
        stage1_epochs = min(args.stage1_epochs, total_epochs)
        stage2_epochs = max(0, total_epochs - stage1_epochs)
    else:
        stage1_epochs = args.stage1_epochs
        stage2_epochs = args.stage2_epochs
    history: list[dict[str, float | int | str]] = []
    global_epoch = 0
    for stage_name, epoch_count, learning_rate, configure in (
        ("head", stage1_epochs, args.stage1_lr, freeze_backbone),
        ("finetune", stage2_epochs, args.stage2_lr, unfreeze_all),
    ):
        if epoch_count <= 0:
            continue
        configure(model)
        optimizer = torch.optim.AdamW(
            (parameter for parameter in model.parameters() if parameter.requires_grad),
            lr=learning_rate,
            weight_decay=args.weight_decay,
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epoch_count)
        for _ in range(epoch_count):
            global_epoch += 1
            started = time.perf_counter()
            loss, targets, probabilities, _ = run_epoch(model, loader, device, criterion, optimizer, scaler)
            macro_f1 = f1_score(targets, probabilities.argmax(axis=1), average="macro", zero_division=0)
            history.append(
                {
                    "epoch": global_epoch,
                    "stage": stage_name,
                    "train_loss": loss,
                    "train_macro_f1": macro_f1,
                    "learning_rate": optimizer.param_groups[0]["lr"],
                    "seconds": time.perf_counter() - started,
                }
            )
            print(
                f"[final] epoch={global_epoch:02d} stage={stage_name} loss={loss:.4f} macro_f1={macro_f1:.4f}",
                flush=True,
            )
            scheduler.step()

    checkpoint_path = destination / "point_direction.pth"
    payload = checkpoint_payload(model, epoch=global_epoch, best_score=float("nan"), held_out_person=None)
    payload["action_threshold"] = None if threshold is None else threshold["threshold"]
    torch.save(payload, checkpoint_path)
    write_json(destination / "history.json", history)
    write_json(
        destination / "labels.json",
        {"class_names": CLASS_NAMES, "class_to_index": CLASS_TO_INDEX, "actions": {"point_right": 1, "point_left": 2, "other": 0}},
    )
    write_json(
        destination / "preprocess.json",
        {"image_size": IMAGE_SIZE, "pixel_order": "RGB", "mean_0_1": IMAGENET_MEAN, "std_0_1": IMAGENET_STD},
    )
    write_json(
        destination / "training_summary.json",
        {
            "model_name": MODEL_NAME,
            "epochs": global_epoch,
            "sample_counts": count_by_person_and_class(samples),
            "action_threshold": threshold,
        },
    )
    return checkpoint_path


def main() -> None:
    args = parse_args()
    args.manifest = args.manifest.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    set_seed()
    device = select_device(args.device)
    samples = load_manifest(args.manifest)
    print(f"device={device}; samples={len(samples)}; people={sorted({sample.person for sample in samples})}")
    if device.type == "cuda":
        print(f"gpu={torch.cuda.get_device_name(0)}")
    write_json(
        args.output / "environment.json",
        {
            "python": sys.version,
            "platform": platform.platform(),
            "torch": torch.__version__,
            "torchvision": torchvision.__version__,
            "timm": timm.__version__,
            "sklearn": sklearn.__version__,
            "device": str(device),
            "gpu": torch.cuda.get_device_name(0) if device.type == "cuda" else None,
            "arguments": vars(args) | {"manifest": str(args.manifest), "output": str(args.output)},
        },
    )

    best_epochs: list[int] | None = None
    threshold: dict[str, float] | None = None
    if args.mode in {"cv", "all"}:
        best_epochs, threshold = run_cross_validation(samples, args, device)
    if args.mode in {"final", "all"}:
        if args.mode == "final":
            cv_summary_path = args.output / "cross_validation" / "summary.json"
            if cv_summary_path.is_file():
                import json

                cv_summary = json.loads(cv_summary_path.read_text(encoding="utf-8"))
                best_epochs = [int(value) for value in cv_summary.get("best_epochs", [])]
                threshold = cv_summary.get("selected_action_threshold")
        checkpoint = train_final(samples, args, device, best_epochs, threshold)
        print(f"final checkpoint: {checkpoint}")


if __name__ == "__main__":
    main()
