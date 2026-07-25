from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from training.common import write_json


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Validate a point model before firmware deployment.")
    parser.add_argument("--cv-summary", type=Path, required=True)
    parser.add_argument("--quant-summary", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--baseline-bytes", type=int, default=3_313_488)
    return parser.parse_args()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    args = parse_args()
    cv = json.loads(args.cv_summary.read_text(encoding="utf-8"))
    quant = json.loads(args.quant_summary.read_text(encoding="utf-8"))
    evaluation = quant["evaluation"]
    model_size = args.model.stat().st_size
    checks = {
        "cv_accuracy": cv["accuracy"] >= 0.90,
        "cv_macro_f1": cv["macro avg"]["f1-score"] >= 0.90,
        "cv_other_false_action_rate": cv["selected_action_threshold"]["other_false_action_rate"] <= 0.02,
        "cv_worst_person_accuracy": cv["worst_person_accuracy"] >= 0.8064516129032258,
        "quantized_accuracy": evaluation["quantized_accuracy"] >= 0.90,
        "quantized_macro_f1": evaluation["quantized_macro_f1"] >= 0.90,
        "quantized_macro_f1_drop":
            evaluation["float_macro_f1"] - evaluation["quantized_macro_f1"] <= 0.03,
        "prediction_agreement": evaluation["prediction_agreement"] >= 0.97,
        "smaller_than_baseline": model_size < args.baseline_bytes,
        "espdl_exists": args.model.is_file() and model_size > 0,
    }
    result = {
        "passed": all(checks.values()),
        "checks": checks,
        "model": str(args.model.resolve()),
        "model_bytes": model_size,
        "baseline_bytes": args.baseline_bytes,
        "sha256": sha256(args.model),
        "model_name": cv["model_name"],
        "image_size": cv["image_size"],
        "parameters": cv["parameters"],
        "macs": cv["macs"],
        "action_threshold": cv["selected_action_threshold"],
        "cross_validation": {
            "accuracy": cv["accuracy"],
            "macro_f1": cv["macro avg"]["f1-score"],
            "worst_person_accuracy": cv["worst_person_accuracy"],
        },
        "quantization": evaluation,
    }
    write_json(args.output, result)
    print(json.dumps(result, ensure_ascii=False, indent=2), flush=True)
    if not result["passed"]:
        failed = [name for name, passed in checks.items() if not passed]
        raise RuntimeError(f"deployment gate failed: {failed}")


if __name__ == "__main__":
    main()
