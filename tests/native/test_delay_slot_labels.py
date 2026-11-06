"""Synthetic ELF CLI regression: every label the emitter jumps to after a delay slot is defined, through a chain."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = sys.argv.pop(1)
BASE = 0x100000


def beq(rs, rt, at, target):
    return (4 << 26) | (rs << 21) | (rt << 16) | (((target - (at + 4)) // 4) & 0xFFFF)


def image(words=None, symbols_at=None):
    names = b'\0.text\0.strtab\0.symtab\0.shstrtab\0'
    if words is None:
        words = [beq(8, 9, 0x00, 0x08), beq(10, 11, 0x04, 0x20), beq(12, 13, 0x08, 0x20), 0, 0, 0x03e00008, 0, 0,
                 0x03e00008, 0]
    code = struct.pack('<%dI' % len(words), *words)
    strings = b'\0chain\0second\0'
    symbols = bytes(16)
    for name, at, size in symbols_at or [(1, 0, len(code))]:
        symbols += struct.pack('<IIIBBH', name, BASE + at, size, 0x12, 0, 1)
    body = code + strings + symbols + names
    section_at = (0x100 + len(body) + 3) & ~3
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, 8, 1,
                         BASE, 52, section_at, 0, 52, 32, 1, 40, 5, 4)
    ph = struct.pack('<8I', 1, 0x100, BASE, BASE, len(code), len(code), 5, 16)
    out = header + ph + bytes(0x100 - 84) + body
    out += bytes(section_at - len(out))
    strings_at = 0x100 + len(code)
    sections = [[0] * 10, [1, 1, 6, BASE, 0x100, len(code), 0, 0, 4, 0],
                [7, 3, 0, 0, strings_at, len(strings), 0, 0, 1, 0],
                [15, 2, 0, 0, strings_at + len(strings), len(symbols), 2, 1, 4, 16],
                [23, 3, 0, 0, strings_at + len(strings) + len(symbols), len(names), 0, 0, 1, 0]]
    return out + b''.join(struct.pack('<10I', *s) for s in sections)


def labels(elf_bytes):
    with tempfile.TemporaryDirectory() as temp:
        root = pathlib.Path(temp)
        elf = root / 'case.elf'
        elf.write_bytes(elf_bytes)
        result = subprocess.run([TOOL, str(elf), str(root / 'out'), '--files', '1'], capture_output=True, text=True)
        if result.returncode != 0:
            raise AssertionError(result.stderr)
        generated = ''.join(p.read_text() for p in (root / 'out').glob('*.cpp'))
    problems = []
    for body in re.split(r'\nstatic void body_', generated):
        used = set(re.findall(r'goto (L_[0-9a-f]+);', body))
        defined = re.findall(r'^(L_[0-9a-f]+):;', body, re.M)
        problems += [('undefined', u) for u in used - set(defined)]
        problems += [('twice', d) for d in set(defined) if defined.count(d) > 1]
    return set(re.findall(r'^(L_[0-9a-f]+):;', generated, re.M)), problems


class DelaySlotLabelTests(unittest.TestCase):
    def test_label_chain(self):
        defined, problems = labels(image())
        self.assertIn('L_10000c', defined)
        self.assertEqual(problems, [])

    def test_delay_slot_at_the_next_function(self):
        bgezal = (1 << 26) | (0x11 << 16) | ((0x18 - 4) // 4)
        words = [bgezal, beq(10, 11, 0x04, 0x14), beq(12, 13, 0x08, 0x14), 0x03e00008, 0, 0x03e00008, 0]
        _, problems = labels(image(words, [(1, 0, 0x0c), (7, 0x0c, 0x10)]))
        self.assertEqual(problems, [])


if __name__ == '__main__':
    unittest.main()
