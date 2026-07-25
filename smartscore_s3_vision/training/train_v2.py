from __future__ import annotations

import argparse
import copy
import csv
import json
import math
import platform
import statistics
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path
from typing import Sequence

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np
import sklearn
import timm
import torch
import torch.nn.functional as F
import torchvision
from PIL import Image, ImageOps
from sklearn.metrics import accuracy_score, classification_report, confusion_matrix, f1_score
from torch import nn
from torch.utils.data import DataLoader, Dataset, WeightedRandomSampler
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
from training.modeling import (
    SUPPORTED_MODEL_NAMES,
    TINY_MODEL_NAME,
    create_model,
    estimate_macs,
    freeze_backbone,
    has_pretrained_weights,
    load_checkpoint,
    unfreeze_all,
)


DEFAULT_CANDIDATES = (
    TINY_MODEL_NAME,
    "mobilenetv3_small_050.lamb_in1k",
    "mobilenetv3_small_075.lamb_in1k",
)


class GestureDatasetV2(Dataset[tuple[torch.Tensor, int, int]]):
    def __init__(self, samples: Sequence[Sample], *, training: bool, image_size: int = IMAGE_SIZE) -> None:
        self.samples = list(samples)
        self.training = training
        self.image_size = image_size
        self.train_transform = transforms.Compose(
            [
                transforms.RandomResizedCrop(
                    (image_size, image_size), scale=(0.72, 1.0), ratio=(0.88, 1.12), antialias=True
                ),
                transforms.RandomAffine(
                    degrees=10, translate=(0.055, 0.055), scale=(0.94, 1.06), fill=127
                ),
                transforms.ColorJitter(brightness=0.30, contrast=0.24, saturation=0.20, hue=0.035),
                transforms.RandomApply([transforms.GaussianBlur(kernel_size=3, sigma=(0.1, 1.0))], p=0.12),
                transforms.ToTensor(),
                transforms.RandomErasing(p=0.08, scale=(0.015, 0.06), ratio=(0.5, 2.0), value="random"),
                transforms.Normalize(IMAGENET_MEAN, IMAGENET_STD),
            ]
        )
        self.eval_transform = transforms.Compose(
            [
                transforms.Resize((image_size, image_size), antialias=True),
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


class ModelEma:
    def __init__(self, model: nn.Module, decay: float) -> None:
        self.module = copy.deepcopy(model).eval()
        self.decay = decay
        for parameter in self.module.parameters():
            parameter.requires_grad_(False)

    @torch.no_grad()
    def update(self, model: nn.Module) -> None:
        source = model.state_dict()
        for name, value in self.module.state_dict().items():
            source_value = source[name].detach()
            if value.dtype.is_floating_point:
                value.mul_(self.decay).add_(source_value, alpha=1.0 - self.decay)
            else:
                value.copy_(source_value)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train and compare SmartScore direction candidates.")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("cv", "final", "all"), default="all")
    parser.add_argument("--candidates", nargs="+", default=list(DEFAULT_CANDIDATES))
    parser.add_argument(
        "--folds",
        nargs="*",
        default=[],
        help="Optional held-out people used for a smoke run; the default evaluates every person",
    )
    parser.add_argument("--winner", default="")
    parser.add_argument("--batch-size", type=int, default=96)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--head-epochs", type=int, default=3)
    parser.add_argument("--finetune-epochs", type=int, default=18)
    parser.add_argument("--head-lr", type=float, default=1.0e-3)
    parser.add_argument("--finetune-lr", type=float, default=3.0e-4)
    parser.add_argument("--weight-decay", type=float, default=2.0e-4)
    parser.add_argument("--label-smoothing", type=float, default=0.03)
    parser.add_argument("--ema-decay", type=float, default=0.995)
    parser.add_argument("--control-fraction", type=float, default=0.125)
    parser.add_argument("--early-stop-patience", type=int, default=5)
    parser.add_argument("--device", choices=("auto", "cuda", "cpu"), default="auto")
    parser.add_argument("--resume", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--image-size", type=int, default=96)
    parser.add_argument("--tiny-lr", type=float, default=1.0e-3)
    parser.add_argument("--teacher-root", type=Path, default=Path("training_artifacts_v2"))
    parser.add_argument("--distill-alpha", type=float, default=0.35)
    parser.add_argument("--distill-temperature", type=float, default=2.0)
    parser.add_argument(
        "--exclude-direction-warnings",
        action=argparse.BooleanOptionalAction,
        default=True,
    )
    return parser.parse_args()


def safe_model_name(model_name: str) -> str:
    return model_name.replace(".", "_").replace("/", "_")


def select_device(requested: str) -> torch.device:
    if requested == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA was requested but is unavailable")
    if requested == "cpu":
        return torch.device("cpu")
    return torch.device("cuda" if torch.cuda.is_available() else "cpu")


def split_control(samples: Sequence[Sample], fraction: float) -> tuple[list[Sample], list[Sample]]:
    if not 0.05 <= fraction <= 0.30:
        raise ValueError("control fraction must be between 0.05 and 0.30")
    groups: dict[tuple[str, str], list[Sample]] = defaultdict(list)
    for sample in samples:
        groups[(sample.person, sample.label)].append(sample)
    training: list[Sample] = []
    control: list[Sample] = []
    for key, group in sorted(groups.items()):
        ordered = sorted(group, key=lambda sample: str(sample.source_path or sample.path))
        rng = np.random.default_rng(SEED + sum(ord(char) for char in "|".join(key)))
        indices = np.arange(len(ordered))
        rng.shuffle(indices)
        control_count = max(1, int(round(len(ordered) * fraction)))
        control_indices = set(indices[:control_count].tolist())
        for index, sample in enumerate(ordered):
            (control if index in control_indices else training).append(sample)
    return training, control


def make_loader(
    samples: Sequence[Sample],
    *,
    training: bool,
    args: argparse.Namespace,
    device: torch.device,
    seed_offset: int = 0,
) -> DataLoader:
    generator = torch.Generator().manual_seed(SEED + seed_offset)
    workers = args.workers if training else 0
    sampler = None
    shuffle = False
    if training:
        counts = Counter(sample.label for sample in samples)
        desired = {"point_right": 0.30, "point_left": 0.30, "other": 0.40}
        weights = [desired[sample.label] / counts[sample.label] for sample in samples]
        sampler = WeightedRandomSampler(weights, num_samples=len(samples), replacement=True, generator=generator)
    return DataLoader(
        GestureDatasetV2(samples, training=training, image_size=args.image_size),
        batch_size=args.batch_size,
        shuffle=shuffle,
        sampler=sampler,
        num_workers=workers,
        pin_memory=device.type == "cuda",
        persistent_workers=workers > 0,
        generator=generator,
    )


def run_epoch(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
    criterion: nn.Module,
    optimizer: torch.optim.Optimizer | None = None,
    scaler: torch.amp.GradScaler | None = None,
    ema: ModelEma | None = None,
    teacher: nn.Module | None = None,
    distill_alpha: float = 0.0,
    distill_temperature: float = 1.0,
) -> tuple[float, np.ndarray, np.ndarray, np.ndarray]:
    training = optimizer is not None
    model.train(training)
    losses = 0.0
    count = 0
    targets_all: list[np.ndarray] = []
    probabilities_all: list[np.ndarray] = []
    indices_all: list[np.ndarray] = []
    for images, targets, indices in loader:
        images = images.to(device, non_blocking=True)
        targets = targets.to(device, non_blocking=True)
        if training:
            optimizer.zero_grad(set_to_none=True)
        with torch.set_grad_enabled(training):
            with torch.amp.autocast(device_type=device.type, enabled=device.type == "cuda"):
                logits = model(images)
                loss = criterion(logits, targets)
                if training and teacher is not None and distill_alpha > 0.0:
                    with torch.no_grad():
                        teacher_logits = teacher(images)
                    temperature = distill_temperature
                    distillation = F.kl_div(
                        F.log_softmax(logits / temperature, dim=1),
                        F.softmax(teacher_logits / temperature, dim=1),
                        reduction="batchmean",
                    ) * (temperature * temperature)
                    loss = (1.0 - distill_alpha) * loss + distill_alpha * distillation
            if training:
                assert scaler is not None
                scaler.scale(loss).backward()
                scaler.step(optimizer)
                scaler.update()
                if ema is not None:
                    ema.update(model)
        batch = targets.shape[0]
        losses += float(loss.detach()) * batch
        count += batch
        targets_all.append(targets.detach().cpu().numpy())
        probabilities_all.append(torch.softmax(logits.detach(), dim=1).cpu().numpy())
        indices_all.append(indices.numpy())
    return (
        losses / max(count, 1),
        np.concatenate(targets_all),
        np.concatenate(probabilities_all),
        np.concatenate(indices_all),
    )


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
    report["accuracy"] = float(accuracy_score(targets, predictions))
    other = targets == CLASS_TO_INDEX["other"]
    report["other_argmax_false_action_rate"] = float((predictions[other] != CLASS_TO_INDEX["other"]).mean())
    return report


def choose_action_threshold(targets: np.ndarray, probabilities: np.ndarray) -> dict[str, float]:
    other_index = CLASS_TO_INDEX["other"]
    top_indices = probabilities.argmax(axis=1)
    top_scores = probabilities.max(axis=1)
    candidates: list[dict[str, float]] = []
    for threshold in np.arange(0.34, 0.96, 0.01):
        actions = (top_indices != other_index) & (top_scores >= threshold)
        other_mask = targets == other_index
        pointing_mask = targets != other_index
        correct_actions = actions & (top_indices == targets)
        candidates.append(
            {
                "threshold": round(float(threshold), 2),
                "other_false_action_rate": float(actions[other_mask].mean()),
                "pointing_recall": float(correct_actions[pointing_mask].mean()),
            }
        )
    acceptable = [item for item in candidates if item["other_false_action_rate"] <= 0.02]
    selected = max(acceptable, key=lambda item: item["pointing_recall"]) if acceptable else min(
        candidates, key=lambda item: (item["other_false_action_rate"], -item["pointing_recall"])
    )
    return {**selected, "target_other_false_action_rate": 0.02}


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


def checkpoint_payload(
    model: nn.Module,
    *,
    model_name: str,
    epoch: int,
    best_score: float,
    held_out_person: str | None,
    image_size: int,
) -> dict:
    return {
        "state_dict": {key: value.detach().cpu() for key, value in model.state_dict().items()},
        "model_name": model_name,
        "class_names": CLASS_NAMES,
        "image_size": image_size,
        "mean": IMAGENET_MEAN,
        "std": IMAGENET_STD,
        "epoch": epoch,
        "best_score": best_score,
        "held_out_person": held_out_person,
        "seed": SEED,
    }


def load_distillation_teacher(
    model_name: str,
    args: argparse.Namespace,
    device: torch.device,
    held_out_person: str | None,
) -> nn.Module | None:
    if model_name != TINY_MODEL_NAME:
        return None
    if held_out_person is None:
        checkpoint_path = args.teacher_root / "final" / "point_direction.pth"
    else:
        checkpoint_path = (
            args.teacher_root
            / "candidates"
            / safe_model_name("mobilenetv3_small_100.lamb_in1k")
            / "folds"
            / held_out_person
            / "best.pth"
        )
    if not checkpoint_path.is_file():
        raise FileNotFoundError(f"leak-free distillation teacher not found: {checkpoint_path}")
    teacher, _ = load_checkpoint(checkpoint_path, device)
    teacher.eval()
    for parameter in teacher.parameters():
        parameter.requires_grad_(False)
    return teacher


def training_stages(
    model_name: str,
    args: argparse.Namespace,
    *,
    head_epochs: int | None = None,
    finetune_epochs: int | None = None,
) -> tuple[tuple[str, int, float, object], ...]:
    head = args.head_epochs if head_epochs is None else head_epochs
    finetune = args.finetune_epochs if finetune_epochs is None else finetune_epochs
    if not has_pretrained_weights(model_name):
        return (("finetune", head + finetune, args.tiny_lr, unfreeze_all),)
    return (
        ("head", head, args.head_lr, freeze_backbone),
        ("finetune", finetune, args.finetune_lr, unfreeze_all),
    )


def fit_fold(
    all_samples: Sequence[Sample],
    held_out_person: str,
    model_name: str,
    destination: Path,
    args: argparse.Namespace,
    device: torch.device,
) -> tuple[int, np.ndarray, np.ndarray, np.ndarray, dict]:
    prediction_path = destination / "predictions.npz"
    metrics_path = destination / "metrics.json"
    if args.resume and prediction_path.is_file() and metrics_path.is_file():
        saved = np.load(prediction_path)
        metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
        print(f"[{safe_model_name(model_name)}/{held_out_person}] resumed", flush=True)
        return (
            int(metrics["best_epoch"]),
            saved["targets"],
            saved["probabilities"],
            saved["global_indices"],
            metrics,
        )

    set_seed(SEED + sum(ord(char) for char in held_out_person + model_name))
    held_out_indices = [index for index, sample in enumerate(all_samples) if sample.person == held_out_person]
    held_out_samples = [all_samples[index] for index in held_out_indices]
    available = [sample for sample in all_samples if sample.person != held_out_person]
    train_samples, control_samples = split_control(available, args.control_fraction)
    train_loader = make_loader(train_samples, training=True, args=args, device=device)
    control_loader = make_loader(control_samples, training=False, args=args, device=device)
    held_out_loader = make_loader(held_out_samples, training=False, args=args, device=device)
    model = create_model(pretrained=has_pretrained_weights(model_name), model_name=model_name).to(device)
    teacher = load_distillation_teacher(model_name, args, device, held_out_person)
    ema_decay = min(args.ema_decay, 0.95) if model_name == TINY_MODEL_NAME else args.ema_decay
    ema = ModelEma(model, ema_decay)
    criterion = nn.CrossEntropyLoss(label_smoothing=args.label_smoothing)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    destination.mkdir(parents=True, exist_ok=True)
    checkpoint_path = destination / "best.pth"
    best_score = -math.inf
    best_epoch = 0
    global_epoch = 0
    no_improvement = 0
    history: list[dict[str, float | int | str]] = []

    for stage, epochs, learning_rate, configure in training_stages(model_name, args):
        configure(model)
        optimizer = torch.optim.AdamW(
            (parameter for parameter in model.parameters() if parameter.requires_grad),
            lr=learning_rate,
            weight_decay=args.weight_decay,
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=max(epochs, 1))
        for stage_epoch in range(epochs):
            global_epoch += 1
            started = time.perf_counter()
            train_loss, train_targets, train_probabilities, _ = run_epoch(
                model,
                train_loader,
                device,
                criterion,
                optimizer,
                scaler,
                ema,
                teacher,
                args.distill_alpha,
                args.distill_temperature,
            )
            control_loss, control_targets, control_probabilities, _ = run_epoch(
                ema.module, control_loader, device, criterion
            )
            train_f1 = f1_score(
                train_targets, train_probabilities.argmax(axis=1), average="macro", zero_division=0
            )
            control_f1 = f1_score(
                control_targets, control_probabilities.argmax(axis=1), average="macro", zero_division=0
            )
            history.append(
                {
                    "epoch": global_epoch,
                    "stage": stage,
                    "train_loss": train_loss,
                    "control_loss": control_loss,
                    "train_macro_f1": train_f1,
                    "control_macro_f1": control_f1,
                    "learning_rate": optimizer.param_groups[0]["lr"],
                    "seconds": time.perf_counter() - started,
                }
            )
            print(
                f"[{safe_model_name(model_name)}/{held_out_person}] epoch={global_epoch:02d} "
                f"stage={stage} train_f1={train_f1:.4f} control_f1={control_f1:.4f}",
                flush=True,
            )
            if control_f1 > best_score + 1.0e-4:
                best_score = control_f1
                best_epoch = global_epoch
                no_improvement = 0
                torch.save(
                    checkpoint_payload(
                        ema.module,
                        model_name=model_name,
                        epoch=global_epoch,
                        best_score=best_score,
                        held_out_person=held_out_person,
                        image_size=args.image_size,
                    ),
                    checkpoint_path,
                )
            elif stage == "finetune":
                no_improvement += 1
            scheduler.step()
            if (
                stage == "finetune"
                and stage_epoch + 1 >= 8
                and no_improvement >= args.early_stop_patience
            ):
                print(f"[{safe_model_name(model_name)}/{held_out_person}] early stop", flush=True)
                break

    write_json(destination / "history.json", history)
    checkpoint = torch.load(checkpoint_path, map_location=device, weights_only=True)
    model.load_state_dict(checkpoint["state_dict"])
    _, targets, probabilities, local_indices = run_epoch(model, held_out_loader, device, criterion)
    global_indices = np.asarray([held_out_indices[int(index)] for index in local_indices], dtype=np.int64)
    metrics = report_for(targets, probabilities)
    metrics.update(
        {
            "held_out_person": held_out_person,
            "model_name": model_name,
            "best_epoch": best_epoch,
            "best_control_macro_f1": best_score,
            "train_counts": count_by_person_and_class(train_samples),
            "control_counts": count_by_person_and_class(control_samples),
            "validation_counts": count_by_person_and_class(held_out_samples),
        }
    )
    write_json(metrics_path, metrics)
    np.savez_compressed(
        prediction_path,
        targets=targets,
        probabilities=probabilities,
        global_indices=global_indices,
    )
    save_confusion(
        targets,
        probabilities.argmax(axis=1),
        destination / "confusion_matrix.png",
        f"{model_name} / {held_out_person}",
    )
    del model, ema, teacher
    if device.type == "cuda":
        torch.cuda.empty_cache()
    return best_epoch, targets, probabilities, global_indices, metrics


def save_predictions(
    samples: Sequence[Sample],
    records: Sequence[tuple[str, np.ndarray, np.ndarray, np.ndarray]],
    path: Path,
) -> tuple[np.ndarray, np.ndarray]:
    targets_all: list[np.ndarray] = []
    probabilities_all: list[np.ndarray] = []
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8-sig", newline="") as handle:
        fields = ["held_out_person", "source_path", "processed_path", "target"] + [
            f"p_{name}" for name in CLASS_NAMES
        ]
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        for person, targets, probabilities, indices in records:
            for target, probability, index in zip(targets, probabilities, indices, strict=True):
                sample = samples[int(index)]
                row = {
                    "held_out_person": person,
                    "source_path": str(sample.source_path or ""),
                    "processed_path": str(sample.path),
                    "target": CLASS_NAMES[int(target)],
                }
                row.update(
                    {
                        f"p_{name}": float(probability[class_index])
                        for class_index, name in enumerate(CLASS_NAMES)
                    }
                )
                writer.writerow(row)
            targets_all.append(targets)
            probabilities_all.append(probabilities)
    return np.concatenate(targets_all), np.concatenate(probabilities_all)


def run_candidate_cv(
    samples: Sequence[Sample],
    model_name: str,
    args: argparse.Namespace,
    device: torch.device,
) -> dict:
    people = list(args.folds) if args.folds else sorted({sample.person for sample in samples})
    root = args.output / "candidates" / safe_model_name(model_name)
    records: list[tuple[str, np.ndarray, np.ndarray, np.ndarray]] = []
    best_epochs: list[int] = []
    per_person: dict[str, dict] = {}
    for person in people:
        best_epoch, targets, probabilities, indices, metrics = fit_fold(
            samples, person, model_name, root / "folds" / person, args, device
        )
        records.append((person, targets, probabilities, indices))
        best_epochs.append(best_epoch)
        per_person[person] = {
            "accuracy": metrics["accuracy"],
            "macro_f1": metrics["macro avg"]["f1-score"],
            "other_argmax_false_action_rate": metrics["other_argmax_false_action_rate"],
        }
    targets, probabilities = save_predictions(samples, records, root / "predictions.csv")
    summary = report_for(targets, probabilities)
    threshold = choose_action_threshold(targets, probabilities)
    profile_model = create_model(pretrained=False, model_name=model_name)
    summary.update(
        {
            "model_name": model_name,
            "image_size": args.image_size,
            "parameters": sum(parameter.numel() for parameter in profile_model.parameters()),
            "macs": estimate_macs(profile_model, args.image_size),
            "people": people,
            "per_person": per_person,
            "worst_person_accuracy": min(value["accuracy"] for value in per_person.values()),
            "worst_person_macro_f1": min(value["macro_f1"] for value in per_person.values()),
            "best_epochs": best_epochs,
            "median_best_epoch": float(statistics.median(best_epochs)),
            "selected_action_threshold": threshold,
        }
    )
    del profile_model
    write_json(root / "summary.json", summary)
    save_confusion(
        targets,
        probabilities.argmax(axis=1),
        root / "confusion_matrix_all.png",
        f"{model_name} / all held-out people",
    )
    return summary


def candidate_is_eligible(summary: dict) -> bool:
    threshold = summary["selected_action_threshold"]
    return (
        threshold["other_false_action_rate"] <= 0.02
        and summary["accuracy"] >= 0.90
        and summary["macro avg"]["f1-score"] >= 0.90
        and summary["worst_person_accuracy"] >= 0.8064516129032258
    )


def candidate_rank(summary: dict) -> tuple[float, int, int, float, float, float]:
    threshold = summary["selected_action_threshold"]
    return (
        1.0 if candidate_is_eligible(summary) else 0.0,
        -int(summary.get("macs", summary["parameters"])),
        -int(summary["parameters"]),
        float(summary["macro avg"]["f1-score"]),
        float(summary["worst_person_accuracy"]),
        float(threshold["pointing_recall"]),
    )


def choose_winner(summaries: Sequence[dict], output: Path) -> dict:
    eligible = [summary for summary in summaries if candidate_is_eligible(summary)]
    winner = max(eligible, key=candidate_rank) if eligible else None
    selection = {
        "winner": winner["model_name"] if winner is not None else None,
        "ranking_rule": [
            "all_accuracy_and_safety_gates",
            "smaller_estimated_macs",
            "smaller_parameter_count",
            "overall_macro_f1",
            "worst_person_accuracy",
            "pointing_recall_at_selected_threshold",
        ],
        "acceptance_gates": {
            "accuracy_min": 0.90,
            "macro_f1_min": 0.90,
            "other_false_action_rate_max": 0.02,
            "worst_person_accuracy_min": 0.8064516129032258,
        },
        "ranking": [
            {
                "model_name": summary["model_name"],
                "rank_key": candidate_rank(summary),
                "accuracy": summary["accuracy"],
                "macro_f1": summary["macro avg"]["f1-score"],
                "worst_person_accuracy": summary["worst_person_accuracy"],
                "threshold": summary["selected_action_threshold"],
                "parameters": summary["parameters"],
                "macs": summary.get("macs"),
                "eligible": candidate_is_eligible(summary),
            }
            for summary in sorted(summaries, key=candidate_rank, reverse=True)
        ],
    }
    write_json(output / "candidate_selection.json", selection)
    if winner is None:
        raise RuntimeError("no smaller candidate met all cross-validation acceptance gates")
    return winner


def train_final(
    samples: Sequence[Sample],
    model_name: str,
    cv_summary: dict,
    args: argparse.Namespace,
    device: torch.device,
) -> Path:
    set_seed()
    destination = args.output / "final"
    destination.mkdir(parents=True, exist_ok=True)
    total_epochs = max(args.head_epochs, int(round(cv_summary["median_best_epoch"])))
    head_epochs = min(args.head_epochs, total_epochs)
    finetune_epochs = max(0, total_epochs - head_epochs)
    model = create_model(pretrained=has_pretrained_weights(model_name), model_name=model_name).to(device)
    teacher = load_distillation_teacher(model_name, args, device, None)
    ema_decay = min(args.ema_decay, 0.95) if model_name == TINY_MODEL_NAME else args.ema_decay
    ema = ModelEma(model, ema_decay)
    criterion = nn.CrossEntropyLoss(label_smoothing=args.label_smoothing)
    loader = make_loader(samples, training=True, args=args, device=device, seed_offset=999)
    scaler = torch.amp.GradScaler(device.type, enabled=device.type == "cuda")
    history: list[dict[str, float | int | str]] = []
    global_epoch = 0
    for stage, epochs, learning_rate, configure in training_stages(
        model_name,
        args,
        head_epochs=head_epochs,
        finetune_epochs=finetune_epochs,
    ):
        if epochs <= 0:
            continue
        configure(model)
        optimizer = torch.optim.AdamW(
            (parameter for parameter in model.parameters() if parameter.requires_grad),
            lr=learning_rate,
            weight_decay=args.weight_decay,
        )
        scheduler = torch.optim.lr_scheduler.CosineAnnealingLR(optimizer, T_max=epochs)
        for _ in range(epochs):
            global_epoch += 1
            started = time.perf_counter()
            loss, targets, probabilities, _ = run_epoch(
                model,
                loader,
                device,
                criterion,
                optimizer,
                scaler,
                ema,
                teacher,
                args.distill_alpha,
                args.distill_temperature,
            )
            macro_f1 = f1_score(
                targets, probabilities.argmax(axis=1), average="macro", zero_division=0
            )
            history.append(
                {
                    "epoch": global_epoch,
                    "stage": stage,
                    "loss": loss,
                    "sampled_train_macro_f1": macro_f1,
                    "learning_rate": optimizer.param_groups[0]["lr"],
                    "seconds": time.perf_counter() - started,
                }
            )
            print(
                f"[final/{safe_model_name(model_name)}] epoch={global_epoch:02d} "
                f"stage={stage} loss={loss:.4f} sampled_f1={macro_f1:.4f}",
                flush=True,
            )
            scheduler.step()
    checkpoint_path = destination / "point_direction.pth"
    payload = checkpoint_payload(
        ema.module,
        model_name=model_name,
        epoch=global_epoch,
        best_score=float("nan"),
        held_out_person=None,
        image_size=args.image_size,
    )
    payload["action_threshold"] = cv_summary["selected_action_threshold"]["threshold"]
    torch.save(payload, checkpoint_path)
    write_json(destination / "history.json", history)
    write_json(
        destination / "labels.json",
        {
            "class_names": CLASS_NAMES,
            "class_to_index": CLASS_TO_INDEX,
            "actions": {"point_right": 1, "point_left": 2, "other": 0},
        },
    )
    write_json(
        destination / "preprocess.json",
        {
            "image_size": args.image_size,
            "pixel_order": "RGB",
            "mean_0_1": IMAGENET_MEAN,
            "std_0_1": IMAGENET_STD,
        },
    )
    write_json(
        destination / "training_summary.json",
        {
            "model_name": model_name,
            "epochs": global_epoch,
            "sample_counts": count_by_person_and_class(samples),
            "action_threshold": cv_summary["selected_action_threshold"],
            "cross_validation_accuracy": cv_summary["accuracy"],
            "cross_validation_macro_f1": cv_summary["macro avg"]["f1-score"],
            "worst_person_accuracy": cv_summary["worst_person_accuracy"],
            "parameters": cv_summary["parameters"],
            "macs": cv_summary["macs"],
            "excluded_direction_warning_samples": args.excluded_direction_warning_samples,
        },
    )
    del teacher
    return checkpoint_path


def load_candidate_summary(args: argparse.Namespace, model_name: str) -> dict:
    path = args.output / "candidates" / safe_model_name(model_name) / "summary.json"
    if not path.is_file():
        raise FileNotFoundError(f"candidate summary not found: {path}")
    return json.loads(path.read_text(encoding="utf-8"))


def main() -> None:
    args = parse_args()
    args.manifest = args.manifest.resolve()
    args.output = args.output.resolve()
    args.teacher_root = args.teacher_root.resolve()
    if args.image_size < 64 or args.image_size > 160 or args.image_size % 16 != 0:
        raise ValueError("image size must be a multiple of 16 between 64 and 160")
    if not 0.0 <= args.distill_alpha < 1.0:
        raise ValueError("distill alpha must be in [0, 1)")
    if args.distill_temperature <= 0.0:
        raise ValueError("distill temperature must be positive")
    args.output.mkdir(parents=True, exist_ok=True)
    invalid = [name for name in args.candidates if name not in SUPPORTED_MODEL_NAMES]
    if invalid:
        raise ValueError(f"unsupported candidates: {invalid}")
    set_seed()
    device = select_device(args.device)
    samples = load_manifest(args.manifest)
    original_sample_count = len(samples)
    if args.exclude_direction_warnings:
        samples = [sample for sample in samples if sample.warning != "landmark_direction_disagrees"]
    args.excluded_direction_warning_samples = original_sample_count - len(samples)
    people = sorted({sample.person for sample in samples})
    unknown_folds = sorted(set(args.folds) - set(people))
    if unknown_folds:
        raise ValueError(f"unknown held-out people: {unknown_folds}; available={people}")
    print(
        f"device={device}; samples={len(samples)}; excluded_direction_warnings="
        f"{args.excluded_direction_warning_samples}; image_size={args.image_size}; "
        f"people={people}; candidates={args.candidates}"
    )
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
            "arguments": vars(args)
            | {
                "manifest": str(args.manifest),
                "output": str(args.output),
                "teacher_root": str(args.teacher_root),
            },
        },
    )

    summaries: list[dict] = []
    if args.mode in {"cv", "all"}:
        for model_name in args.candidates:
            summaries.append(run_candidate_cv(samples, model_name, args, device))
        winner_summary = choose_winner(summaries, args.output)
    else:
        winner_name = args.winner
        if not winner_name:
            selection_path = args.output / "candidate_selection.json"
            if not selection_path.is_file():
                raise FileNotFoundError("--winner is required when candidate_selection.json is absent")
            winner_name = json.loads(selection_path.read_text(encoding="utf-8"))["winner"]
        winner_summary = load_candidate_summary(args, winner_name)

    if args.mode in {"final", "all"}:
        checkpoint = train_final(
            samples, winner_summary["model_name"], winner_summary, args, device
        )
        print(f"winner={winner_summary['model_name']}; final checkpoint={checkpoint}", flush=True)


if __name__ == "__main__":
    main()
