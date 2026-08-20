import array
import tempfile
import unittest
from pathlib import Path

from build_salamander_bank import ZoneData, verify_bank, write_bank


class PianoBankFormatTest(unittest.TestCase):
    def make_zones(self) -> list[ZoneData]:
        pcm = array.array("h", ((index % 200) - 100 for index in range(1024)))
        samples = pcm.tobytes()
        return [
            ZoneData(60, 1, 50, samples, 200, 900),
            ZoneData(60, 51, 95, samples, 200, 900),
            ZoneData(60, 96, 127, samples, 200, 900),
        ]

    def test_round_trip_and_payload_crc(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "piano.pbank"
            write_bank(self.make_zones(), path)
            verify_bank(path)

            corrupted = bytearray(path.read_bytes())
            corrupted[-1] ^= 0x40
            path.write_bytes(corrupted)
            with self.assertRaisesRegex(ValueError, "sample CRC mismatch"):
                verify_bank(path)


if __name__ == "__main__":
    unittest.main()
