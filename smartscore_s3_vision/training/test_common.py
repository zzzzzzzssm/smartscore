from __future__ import annotations

import unittest

from training.common import CLASS_NAMES, CLASS_TO_INDEX, horizontally_flipped_target


class LabelMappingTests(unittest.TestCase):
    def test_class_order_is_firmware_order(self) -> None:
        self.assertEqual(CLASS_NAMES, ("point_right", "point_left", "other"))
        self.assertEqual(CLASS_TO_INDEX["point_right"], 0)
        self.assertEqual(CLASS_TO_INDEX["point_left"], 1)
        self.assertEqual(CLASS_TO_INDEX["other"], 2)

    def test_horizontal_flip_swaps_directions_only(self) -> None:
        self.assertEqual(
            horizontally_flipped_target(CLASS_TO_INDEX["point_right"]), CLASS_TO_INDEX["point_left"]
        )
        self.assertEqual(
            horizontally_flipped_target(CLASS_TO_INDEX["point_left"]), CLASS_TO_INDEX["point_right"]
        )
        self.assertEqual(horizontally_flipped_target(CLASS_TO_INDEX["other"]), CLASS_TO_INDEX["other"])


if __name__ == "__main__":
    unittest.main()
