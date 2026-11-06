"""Synthetic ELF CLI regression: a sized data object inside an executable section is data, not a function."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = sys.argv.pop(1)
BASE = 0x100000


def image(object_type):
    names = b'\0.text\0.strtab\0.symtab\0.shstrtab\0'
    code = struct.pack('<3I', 0x24020007, 0x03e00008, 0)
    code += struct.pack('<2I', BASE + 20, 0x03e00008)
    code += struct.pack('<3I', 0x24420001, 0x03e00008, 0)
    strings = b'\0first\0table\0second\0'
    symbols = bytes(16)
    symbols += struct.pack('<IIIBBH', 1, BASE, 12, 0x12, 0, 1)
    symbols += struct.pack('<IIIBBH', 7, BASE + 12, 8, 0x10 | object_type, 0, 1)
    symbols += struct.pack('<IIIBBH', 13, BASE + 20, 12, 0x12, 0, 1)
    body = code + strings + symbols + names
    section_at = (0x100 + len(body) + 3) & ~3
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, 8, 1,
                         BASE, 52, section_at, 0, 52, 32, 1, 40, 5, 4)
    ph = struct.pack('<8I', 1, 0x100, BASE, BASE, len(code), len(code), 7, 16)
    out = header + ph + bytes(0x100 - 84) + body
    out += bytes(section_at - len(out))
    strings_at = 0x100 + len(code)
    sections = [[0] * 10, [1, 1, 7, BASE, 0x100, len(code), 0, 0, 4, 0],
                [7, 3, 0, 0, strings_at, len(strings), 0, 0, 1, 0],
                [15, 2, 0, 0, strings_at + len(strings), len(symbols), 2, 1, 4, 16],
                [23, 3, 0, 0, strings_at + len(strings) + len(symbols), len(names), 0, 0, 1, 0]]
    return out + b''.join(struct.pack('<10I', *s) for s in sections)


class DataObjectTests(unittest.TestCase):
    def run_tool(self, root, name, object_type):
        elf = root / (name + '.elf')
        elf.write_bytes(image(object_type))
        result = subprocess.run([TOOL, str(elf), str(root / name), '--files', '1'], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout, (root / name)

    def test_object_in_code_is_not_a_function(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            out, gen = self.run_tool(root, 'object', 1)
            self.assertIn('2 table entries', out)
            generated = ''.join(p.read_text() for p in gen.glob('*.cpp'))
            self.assertNotIn('{0x10000cu,', generated)

    def test_untyped_symbol_still_seeds_code(self):
        with tempfile.TemporaryDirectory() as temp:
            out, _ = self.run_tool(pathlib.Path(temp), 'notype', 0)
            self.assertIn('3 table entries', out)


if __name__ == '__main__':
    unittest.main()
