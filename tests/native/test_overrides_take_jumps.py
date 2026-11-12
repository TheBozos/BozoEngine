"""--overrides-take-jumps: a tail jump to an override goes through bozo_tail_call; without it, to the guest code."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = sys.argv.pop(1)
BASE = 0x100000


def image():
    names = b'\0.text\0.strtab\0.symtab\0.shstrtab\0'
    jump = 0x08000000 | ((BASE + 8) >> 2)
    code = struct.pack('<4I', jump, 0, 0x03e00008, 0)
    strings = b'\0caller\0callee\0'
    symbols = bytes(16) + struct.pack('<IIIBBH', 1, BASE, 8, 0x12, 0, 1)
    symbols += struct.pack('<IIIBBH', 8, BASE + 8, 8, 0x12, 0, 1)
    body = code + strings + symbols + names
    section_at = (0x100 + len(body) + 3) & ~3
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, 8, 1,
                         BASE, 52, section_at, 0, 52, 32, 1, 40, 5, 4)
    ph = struct.pack('<8I', 1, 0x100, BASE, BASE, len(code), len(code), 5, 16)
    out = header + ph + bytes(0x100 - 84) + body
    out += bytes(section_at - len(out))
    text_at = 0x100 + len(code)
    sections = [[0] * 10, [1, 1, 6, BASE, 0x100, len(code), 0, 0, 4, 0],
                [7, 3, 0, 0, text_at, len(strings), 0, 0, 1, 0],
                [15, 2, 0, 0, text_at + len(strings), len(symbols), 2, 1, 4, 16],
                [23, 3, 0, 0, text_at + len(strings) + len(symbols), len(names), 0, 0, 1, 0]]
    return out + b''.join(struct.pack('<10I', *s) for s in sections)


class OverridesTakeJumpsTests(unittest.TestCase):
    def test_tail_jump_to_an_override(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            elf = root / 'fixture.elf'
            elf.write_bytes(image())
            externals = root / 'externals.txt'
            externals.write_text(f'override 0x{BASE + 8:x}\n')

            def generated(name, *flags):
                out = root / name
                done = subprocess.run([TOOL, str(elf), str(out), '--files', '1', '--externals', str(externals), *flags],
                                      capture_output=True, text=True)
                self.assertEqual(done.returncode, 0, done.stderr)
                return ''.join(p.read_text() for p in sorted(out.glob('*_000.cpp')))

            tail = f'bozo_tail_call(rdram, ctx, rt, 0x{BASE + 8:x}u)'
            self.assertNotIn(tail, generated('reference'))
            self.assertIn(tail, generated('jumps', '--overrides-take-jumps'))


if __name__ == '__main__':
    unittest.main()
