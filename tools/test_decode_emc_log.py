import importlib.util
import struct
import unittest
from pathlib import Path

MODULE_PATH = Path(__file__).with_name("decode_emc_log.py")
SPEC = importlib.util.spec_from_file_location("decode_emc_log", MODULE_PATH)
decoder = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(decoder)


class DecoderTests(unittest.TestCase):
    def test_size_is_strict(self):
        with self.assertRaises(ValueError):
            decoder.decode(b"\0" * 2019)

    def test_known_record_and_bitfields(self):
        data = bytearray(decoder.LOG_SIZE)
        struct.pack_into("<I", data, 0, 0x07E00000)
        struct.pack_into("<8I", data, 4, 0xC0020303, 0x1BD51ECC,
                         0x920000E5, 0x10010000, 0x0036217B,
                         0x1A3827DF, 0xFFFFFFFF, 0xFFFFFFFF)
        report = decoder.decode(bytes(data))
        row = report["records"][0]
        self.assertEqual(row["sequence"], "217B")
        self.assertEqual(row["socTemperatureC"], 39.87)
        self.assertIn("Main SoC", row["upCauseKnown"])
        self.assertEqual(row["codeLabel"],
                         "Main SoC/APU SB-TSI I2C access failure")


if __name__ == "__main__":
    unittest.main()
