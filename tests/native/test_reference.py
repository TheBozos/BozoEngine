#!/usr/bin/env python3
# Synthetic PS2XREF1 records: no disc or generated game code required.
import importlib.util
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = pathlib.Path(__file__).resolve().parents[2] / "tools/capture/ref_compare.py"
SPEC = importlib.util.spec_from_file_location("ref_compare", TOOL)
READER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(READER)


class ReferenceReader(unittest.TestCase):
    # Wire-format fixture: frame/thread, seven 64-bit values, four 32-bit counters.
    RECORD = bytes.fromhex(
        "11000000 f9ffffff "
        "0807060504030201 1817161514131211 2827262524232221 "
        "3837363534333231 4847464544434241 5857565554535251 "
        "6867666564636261 78777675 88878685 98979695 a8a7a6a5"
    )
    EXPECTED = {
        "frame": 17, "thread": -7, "ram": 0x0102030405060708,
        "scratchpad": 0x1112131415161718, "calls": 0x2122232425262728,
        "draws": 0x3132333435363738, "sound": 0x4142434445464748,
        "threads": 0x5152535455565758, "eeCycle": 0x6162636465666768,
        "callCount": 0x75767778, "drawBatches": 0x85868788,
        "drawVertices": 0x95969798, "soundSamples": 0xa5a6a7a8,
    }

    def write(self, path, second=None):
        header = bytearray(240)
        header[:8] = b"PS2XREF1"
        struct.pack_into("<6I", header, 8, 1, 240, 80, 1, 0x2000000, 0x4000)
        header[48:55] = b"fixture"
        path.write_bytes(header + self.RECORD + (self.RECORD if second is None else second))

    def test_all_fields_and_record_stride(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "synthetic.ref"
            self.write(path, b"\x12\0\0\0" + self.RECORD[4:])
            header, frames = READER.read(path)
            self.assertEqual(header["run"], "fixture")
            self.assertEqual(frames, [self.EXPECTED, dict(self.EXPECTED, frame=18)])

    def test_cli_detects_cycle_high_word_change(self):
        with tempfile.TemporaryDirectory() as folder:
            a, b = (pathlib.Path(folder) / name for name in ("a.ref", "b.ref"))
            self.write(a)
            second = bytearray(self.RECORD)
            second[60] ^= 1  # high word of the second frame's 64-bit EE cycle count
            self.write(b, second)
            result = subprocess.run([sys.executable, "-I", str(TOOL), str(a), str(b)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            self.assertIn("eeCycle differs first at frame 1", result.stdout)


if __name__ == "__main__":
    unittest.main()
