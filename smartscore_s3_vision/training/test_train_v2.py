from __future__ import annotations

import unittest
from pathlib import Path

import numpy as np
import torch

from training.common import CLASS_NAMES, CLASS_TO_INDEX, Sample
from training.modeling import TINY_MODEL_NAME, create_model, estimate_macs
from training.train_v2 import candidate_is_eligible, candidate_rank, choose_action_threshold, split_control


def make_samples() -> list[Sample]:
    samples: list[Sample] = []
    for person in ("P001", "P002"):
        for label in CLASS_NAMES:
            for index in range(20):
                path = Path(f"{person}/{label}/{index:03d}.jpg")
                samples.append(Sample(path=path, source_path=path, person=person, label=label))
    return samples


class TrainingV2Tests(unittest.TestCase):
    def test_tiny_model_has_fixed_three_class_output_and_small_compute(self) -> None:
        model = create_model(pretrained=False, model_name=TINY_MODEL_NAME)
        self.assertEqual(tuple(model(torch.zeros(1, 3, 96, 96)).shape), (1, len(CLASS_NAMES)))
        self.assertLess(sum(parameter.numel() for parameter in model.parameters()), 60_000)
        self.assertLess(estimate_macs(model, 96), 4_000_000)

    def test_control_split_is_reproducible_stratified_and_disjoint(self) -> None:
        samples = make_samples()
        train_a, control_a = split_control(samples, 0.15)
        train_b, control_b = split_control(samples, 0.15)
        self.assertEqual([sample.path for sample in train_a], [sample.path for sample in train_b])
        self.assertEqual([sample.path for sample in control_a], [sample.path for sample in control_b])
        self.assertFalse({sample.path for sample in train_a} & {sample.path for sample in control_a})
        for person in ("P001", "P002"):
            for label in CLASS_NAMES:
                self.assertEqual(
                    sum(sample.person == person and sample.label == label for sample in control_a), 3
                )

    def test_threshold_respects_false_action_target_when_possible(self) -> None:
        targets = np.asarray(
            [CLASS_TO_INDEX["other"]] * 100
            + [CLASS_TO_INDEX["point_right"]] * 20
            + [CLASS_TO_INDEX["point_left"]] * 20
        )
        probabilities = np.zeros((len(targets), len(CLASS_NAMES)), dtype=np.float32)
        probabilities[:98, CLASS_TO_INDEX["other"]] = 0.95
        probabilities[:98, CLASS_TO_INDEX["point_right"]] = 0.03
        probabilities[:98, CLASS_TO_INDEX["point_left"]] = 0.02
        probabilities[98:100, CLASS_TO_INDEX["point_right"]] = 0.60
        probabilities[98:100, CLASS_TO_INDEX["other"]] = 0.35
        probabilities[100:120, CLASS_TO_INDEX["point_right"]] = 0.90
        probabilities[100:120, CLASS_TO_INDEX["other"]] = 0.08
        probabilities[120:, CLASS_TO_INDEX["point_left"]] = 0.90
        probabilities[120:, CLASS_TO_INDEX["other"]] = 0.08
        result = choose_action_threshold(targets, probabilities)
        self.assertLessEqual(result["other_false_action_rate"], 0.02)
        self.assertGreaterEqual(result["pointing_recall"], 0.99)

    def test_ranking_prefers_worst_person_accuracy_after_safety(self) -> None:
        base = {
            "accuracy": 0.92,
            "selected_action_threshold": {
                "other_false_action_rate": 0.01,
                "pointing_recall": 0.90,
            },
            "macro avg": {"f1-score": 0.91},
            "parameters": 1000,
        }
        stronger_worst_person = base | {"worst_person_accuracy": 0.86}
        weaker_worst_person = base | {"worst_person_accuracy": 0.80}
        self.assertGreater(candidate_rank(stronger_worst_person), candidate_rank(weaker_worst_person))

    def test_ranking_requires_accuracy_target_before_worst_person_tiebreak(self) -> None:
        base = {
            "selected_action_threshold": {
                "other_false_action_rate": 0.019,
                "pointing_recall": 0.75,
            },
            "macro avg": {"f1-score": 0.93},
            "parameters": 1000,
        }
        target_met = base | {"accuracy": 0.934, "worst_person_accuracy": 0.807}
        target_missed = base | {"accuracy": 0.894, "worst_person_accuracy": 0.811}
        self.assertGreater(candidate_rank(target_met), candidate_rank(target_missed))

    def test_candidate_requires_every_accuracy_and_safety_gate(self) -> None:
        eligible = {
            "accuracy": 0.91,
            "macro avg": {"f1-score": 0.91},
            "worst_person_accuracy": 0.82,
            "selected_action_threshold": {
                "other_false_action_rate": 0.019,
                "pointing_recall": 0.70,
            },
            "parameters": 50_000,
            "macs": 3_000_000,
        }
        self.assertTrue(candidate_is_eligible(eligible))
        self.assertFalse(candidate_is_eligible(eligible | {"accuracy": 0.899}))
        self.assertFalse(
            candidate_is_eligible(
                eligible
                | {"selected_action_threshold": {"other_false_action_rate": 0.021, "pointing_recall": 0.70}}
            )
        )


if __name__ == "__main__":
    unittest.main()
