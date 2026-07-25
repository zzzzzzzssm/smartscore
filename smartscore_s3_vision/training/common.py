from __future__ import annotations

import csv
import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Sequence

import numpy as np


CLASS_NAMES: tuple[str, ...] = ("point_right", "point_left", "other")
CLASS_TO_INDEX = {name: index for index, name in enumerate(CLASS_NAMES)}
INDEX_TO_CLASS = {index: name for name, index in CLASS_TO_INDEX.items()}
IMAGE_EXTENSIONS = {".jpg", ".jpeg", ".png"}
IMAGE_SIZE = 128
IMAGENET_MEAN = (0.485, 0.456, 0.406)
IMAGENET_STD = (0.229, 0.224, 0.225)
SEED = 20260716


@dataclass(frozen=True)
class Sample:
    path: Path
    person: str
    label: str
    source_path: Path | None = None
    warning: str = ""
    box_left: int = 0
    box_top: int = 0
    box_right: int = 0
    box_bottom: int = 0

    @property
    def target(self) -> int:
        return CLASS_TO_INDEX[self.label]

    @property
    def box(self) -> tuple[int, int, int, int]:
        return self.box_left, self.box_top, self.box_right, self.box_bottom


def set_seed(seed: int = SEED) -> None:
    random.seed(seed)
    np.random.seed(seed)
    try:
        import torch

        torch.manual_seed(seed)
        if torch.cuda.is_available():
            torch.cuda.manual_seed_all(seed)
    except ImportError:
        pass


def horizontally_flipped_target(target: int) -> int:
    if target == CLASS_TO_INDEX["point_left"]:
        return CLASS_TO_INDEX["point_right"]
    if target == CLASS_TO_INDEX["point_right"]:
        return CLASS_TO_INDEX["point_left"]
    return target


def iter_source_images(dataset_root: Path) -> Iterable[tuple[str, str, Path]]:
    if not dataset_root.is_dir():
        raise FileNotFoundError(f"dataset directory does not exist: {dataset_root}")
    people = sorted(path for path in dataset_root.iterdir() if path.is_dir())
    if not people:
        raise ValueError(f"no person directories found under {dataset_root}")

    for person_dir in people:
        for label in CLASS_NAMES:
            class_dir = person_dir / label
            if not class_dir.is_dir():
                raise ValueError(f"missing class directory: {class_dir}")
            files = sorted(
                path for path in class_dir.iterdir() if path.is_file() and path.suffix.lower() in IMAGE_EXTENSIONS
            )
            if not files:
                raise ValueError(f"no images found in {class_dir}")
            for path in files:
                yield person_dir.name, label, path


def load_manifest(path: Path) -> list[Sample]:
    if not path.is_file():
        raise FileNotFoundError(f"manifest not found: {path}")
    samples: list[Sample] = []
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            label = row["label"]
            if label not in CLASS_TO_INDEX:
                raise ValueError(f"unknown label in manifest: {label}")
            samples.append(
                Sample(
                    path=Path(row["processed_path"]),
                    person=row["person"],
                    label=label,
                    source_path=Path(row["source_path"]),
                    warning=row.get("warning", ""),
                    box_left=int(row.get("box_left", 0) or 0),
                    box_top=int(row.get("box_top", 0) or 0),
                    box_right=int(row.get("box_right", 0) or 0),
                    box_bottom=int(row.get("box_bottom", 0) or 0),
                )
            )
    if not samples:
        raise ValueError(f"manifest contains no samples: {path}")
    return samples


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2), encoding="utf-8")
    temporary.replace(path)


def count_by_person_and_class(samples: Sequence[Sample]) -> dict[str, dict[str, int]]:
    counts: dict[str, dict[str, int]] = {}
    for sample in samples:
        person_counts = counts.setdefault(sample.person, {name: 0 for name in CLASS_NAMES})
        person_counts[sample.label] += 1
    return counts
