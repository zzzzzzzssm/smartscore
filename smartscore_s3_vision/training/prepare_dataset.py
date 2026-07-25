from __future__ import annotations

import argparse
import csv
import hashlib
from collections import Counter, defaultdict, deque
from pathlib import Path

import mediapipe as mp
import numpy as np
from PIL import Image, ImageDraw, ImageOps

from training.common import CLASS_NAMES, IMAGE_SIZE, iter_source_images, write_json


LANDMARK_INDEX_MCP = 5
LANDMARK_INDEX_TIP = 8


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Audit and crop SmartScore pointing images.")
    parser.add_argument("--input", type=Path, required=True, help="Directory containing P001/... class folders")
    parser.add_argument("--output", type=Path, required=True, help="Generated processed dataset directory")
    parser.add_argument("--padding", type=float, default=0.28, help="Square crop padding relative to hand size")
    parser.add_argument("--processed-size", type=int, default=192, help="Saved crop size before training jitter")
    parser.add_argument(
        "--near-duplicate-mae",
        type=float,
        default=1.25,
        help="Exclude consecutive crops whose 16x16 gray mean absolute error is at or below this value",
    )
    parser.add_argument("--min-detection-confidence", type=float, default=0.30)
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def image_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def square_crop_box(points: np.ndarray, width: int, height: int, padding: float) -> tuple[int, int, int, int]:
    x_min, y_min = points.min(axis=0)
    x_max, y_max = points.max(axis=0)
    center_x = (x_min + x_max) * 0.5
    center_y = (y_min + y_max) * 0.5
    side = max(x_max - x_min, y_max - y_min) * (1.0 + 2.0 * padding)
    side = max(side, 32.0)

    left = int(np.floor(center_x - side * 0.5))
    top = int(np.floor(center_y - side * 0.5))
    right = int(np.ceil(center_x + side * 0.5))
    bottom = int(np.ceil(center_y + side * 0.5))
    return left, top, right, bottom


def crop_with_padding(
    image: Image.Image, box: tuple[int, int, int, int], output_size: int = IMAGE_SIZE
) -> Image.Image:
    left, top, right, bottom = box
    pad_left = max(0, -left)
    pad_top = max(0, -top)
    pad_right = max(0, right - image.width)
    pad_bottom = max(0, bottom - image.height)
    if any((pad_left, pad_top, pad_right, pad_bottom)):
        image = ImageOps.expand(image, border=(pad_left, pad_top, pad_right, pad_bottom), fill=(127, 127, 127))
        left += pad_left
        right += pad_left
        top += pad_top
        bottom += pad_top
    return image.crop((left, top, right, bottom)).resize((output_size, output_size), Image.Resampling.BILINEAR)


def gray_fingerprint(image: Image.Image) -> np.ndarray:
    return np.asarray(image.convert("L").resize((16, 16), Image.Resampling.BILINEAR), dtype=np.float32)


def direction_warning(label: str, points: np.ndarray) -> str:
    if label not in {"point_left", "point_right"}:
        return ""
    index_mcp_x = points[LANDMARK_INDEX_MCP, 0]
    index_tip_x = points[LANDMARK_INDEX_TIP, 0]
    hand_width = max(float(points[:, 0].max() - points[:, 0].min()), 1.0)
    direction = (index_tip_x - index_mcp_x) / hand_width
    if label == "point_left" and direction > 0.12:
        return "landmark_direction_disagrees"
    if label == "point_right" and direction < -0.12:
        return "landmark_direction_disagrees"
    return ""


def make_contact_sheet(paths: list[Path], destination: Path, title: str, columns: int = 8, limit: int = 64) -> None:
    selected = paths[:limit]
    if not selected:
        return
    tile = 144
    title_height = 28
    rows = (len(selected) + columns - 1) // columns
    canvas = Image.new("RGB", (columns * tile, title_height + rows * tile), "white")
    draw = ImageDraw.Draw(canvas)
    draw.text((8, 6), title, fill="black")
    for index, path in enumerate(selected):
        with Image.open(path) as image:
            thumbnail = ImageOps.fit(image.convert("RGB"), (IMAGE_SIZE, IMAGE_SIZE))
        x = (index % columns) * tile + 8
        y = title_height + (index // columns) * tile
        canvas.paste(thumbnail, (x, y))
        draw.text((x, y + IMAGE_SIZE), path.stem[-12:], fill="black")
    destination.parent.mkdir(parents=True, exist_ok=True)
    canvas.save(destination, quality=90)


def main() -> None:
    args = parse_args()
    input_root = args.input.resolve()
    output_root = args.output.resolve()
    manifest_path = output_root / "manifest.csv"
    if manifest_path.exists() and not args.overwrite:
        raise FileExistsError(f"output already exists; pass --overwrite to rebuild: {manifest_path}")

    samples_root = output_root / "samples"
    samples_root.mkdir(parents=True, exist_ok=True)
    manifest_rows: list[dict[str, object]] = []
    review_rows: list[dict[str, object]] = []
    contact_paths: dict[tuple[str, str], list[Path]] = defaultdict(list)
    stats = Counter()
    seen_hashes: dict[str, Path] = {}
    recent_fingerprints: dict[tuple[str, str], deque[tuple[np.ndarray, Path]]] = defaultdict(
        lambda: deque(maxlen=12)
    )

    hands_api = mp.solutions.hands
    with hands_api.Hands(
        static_image_mode=True,
        max_num_hands=2,
        model_complexity=0,
        min_detection_confidence=args.min_detection_confidence,
        min_tracking_confidence=0.3,
    ) as hands:
        for person, label, source_path in iter_source_images(input_root):
            stats["source_total"] += 1
            try:
                with Image.open(source_path) as loaded:
                    image = loaded.convert("RGB")
                    image.load()
            except Exception as error:
                stats["decode_failed"] += 1
                review_rows.append(
                    {"person": person, "label": label, "source_path": str(source_path), "reason": "decode_failed", "detail": str(error)}
                )
                continue

            digest = image_sha256(source_path)
            duplicate = seen_hashes.get(digest)
            if duplicate is not None:
                stats["exact_duplicate"] += 1
                review_rows.append(
                    {"person": person, "label": label, "source_path": str(source_path), "reason": "exact_duplicate", "detail": str(duplicate)}
                )
                continue
            seen_hashes[digest] = source_path

            rgb = np.asarray(image)
            result = hands.process(rgb)
            landmarks = list(result.multi_hand_landmarks or [])
            if not landmarks:
                stats["no_hand"] += 1
                review_rows.append(
                    {"person": person, "label": label, "source_path": str(source_path), "reason": "no_hand", "detail": ""}
                )
                continue
            if len(landmarks) != 1:
                stats["multi_hand"] += 1
                review_rows.append(
                    {"person": person, "label": label, "source_path": str(source_path), "reason": "multi_hand", "detail": str(len(landmarks))}
                )
                continue

            points = np.asarray(
                [(landmark.x * image.width, landmark.y * image.height) for landmark in landmarks[0].landmark],
                dtype=np.float32,
            )
            box = square_crop_box(points, image.width, image.height, args.padding)
            crop = crop_with_padding(image, box, args.processed_size)
            fingerprint = gray_fingerprint(crop)
            recent = recent_fingerprints[(person, label)]
            closest: tuple[float, Path] | None = None
            for previous_fingerprint, previous_path in recent:
                distance = float(np.mean(np.abs(fingerprint - previous_fingerprint)))
                if closest is None or distance < closest[0]:
                    closest = (distance, previous_path)
            if closest is not None and closest[0] <= args.near_duplicate_mae:
                stats["near_duplicate"] += 1
                review_rows.append(
                    {
                        "person": person,
                        "label": label,
                        "source_path": str(source_path),
                        "reason": "near_duplicate",
                        "detail": f"mae={closest[0]:.3f}; previous={closest[1]}",
                    }
                )
                continue
            destination = samples_root / person / label / f"{source_path.stem}.jpg"
            destination.parent.mkdir(parents=True, exist_ok=True)
            crop.save(destination, format="JPEG", quality=95, subsampling=0)
            recent.append((fingerprint, source_path))

            warning = direction_warning(label, points)
            if warning:
                stats[warning] += 1
                review_rows.append(
                    {"person": person, "label": label, "source_path": str(source_path), "reason": warning, "detail": "sample_kept"}
                )
            stats["processed"] += 1
            stats[f"processed_{label}"] += 1
            manifest_rows.append(
                {
                    "person": person,
                    "label": label,
                    "source_path": str(source_path),
                    "processed_path": str(destination),
                    "sha256": digest,
                    "warning": warning,
                    "box_left": box[0],
                    "box_top": box[1],
                    "box_right": box[2],
                    "box_bottom": box[3],
                }
            )
            contact_paths[(person, label)].append(destination)

            if stats["source_total"] % 100 == 0:
                print(f"processed {stats['source_total']} source images", flush=True)

    output_root.mkdir(parents=True, exist_ok=True)
    manifest_fields = [
        "person", "label", "source_path", "processed_path", "sha256", "warning",
        "box_left", "box_top", "box_right", "box_bottom",
    ]
    with manifest_path.open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=manifest_fields)
        writer.writeheader()
        writer.writerows(manifest_rows)

    with (output_root / "review.csv").open("w", encoding="utf-8-sig", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["person", "label", "source_path", "reason", "detail"])
        writer.writeheader()
        writer.writerows(review_rows)

    for (person, label), paths in sorted(contact_paths.items()):
        make_contact_sheet(paths, output_root / "contact_sheets" / f"{person}_{label}.jpg", f"{person} / {label}")

    summary = {
        "input": str(input_root),
        "output": str(output_root),
        "image_size": IMAGE_SIZE,
        "processed_size": args.processed_size,
        "near_duplicate_mae": args.near_duplicate_mae,
        "class_names": CLASS_NAMES,
        "stats": dict(sorted(stats.items())),
    }
    write_json(output_root / "summary.json", summary)
    print(f"manifest: {manifest_path}")
    print(f"review: {output_root / 'review.csv'}")
    print(summary["stats"])


if __name__ == "__main__":
    main()
