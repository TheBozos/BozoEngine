"""Synthetic ELF CLI regression for opt-in offline callable entries."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

TOOL = sys.argv.pop(1)
BASE = 0x100000


def image(code_size=24):
    names = b'\0.text\0.strtab\0.symtab\0.shstrtab\0'
    code = struct.pack('<6I', 0x24020007, 0x03e00008, 0, 0x24420001, 0x03e00008, 0)
    strings = b'\0primary\0interior\0'
    symbols = bytes(16) + struct.pack('<IIIBBH', 1, BASE, 24, 0x12, 0, 1)
    symbols += struct.pack('<IIIBBH', 9, BASE+12, 0, 0x10, 0, 1)
    body = code + strings + symbols + names
    section_at = (0x100+len(body)+3)&~3
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01'+bytes(9), 2, 8, 1,
                         BASE, 52, section_at, 0, 52, 32, 1, 40, 5, 4)
    ph = struct.pack('<8I', 1, 0x100, BASE, BASE, 24, 24, 5, 16)
    out = header+ph+bytes(0x100-84)+body
    out += bytes(section_at-len(out))
    sections = [[0]*10, [1,1,6,BASE,0x100,code_size,0,0,4,0],
                [7,3,0,0,0x118,len(strings),0,0,1,0],
                [15,2,0,0,0x118+len(strings),len(symbols),2,1,4,16],
                [23,3,0,0,0x118+len(strings)+len(symbols),len(names),0,0,1,0]]
    return out+b''.join(struct.pack('<10I', *s) for s in sections)


class EntriesTests(unittest.TestCase):
    def test_opt_in_and_rejection(self):
        with tempfile.TemporaryDirectory() as temp:
            root=pathlib.Path(temp); elf=root/'fixture.elf'; elf.write_bytes(image())
            def run(name, entries=None):
                command=[TOOL, str(elf), str(root/name), '--files', '1']
                if entries is not None:
                    path=root/'entries'; path.write_text(entries)
                    command += ['--entries', str(path)]
                return subprocess.run(command, capture_output=True, text=True)
            plain=run('plain')
            self.assertEqual(plain.returncode, 0, plain.stderr)
            self.assertIn('1 table entries', plain.stdout)
            explicit=run('explicit', '# explicit offline label\n0x10000c\n')
            self.assertEqual(explicit.returncode, 0, explicit.stderr)
            self.assertIn('2 table entries', explicit.stdout)
            for i, bad in enumerate(['0x100001', '0x100018', '-4', '4294967296',
                                     '0x10000c garbage', '0x10000c\n0x10000c',
                                     '0x10000c\nnot-an-address']):
                name=f'bad{i}'
                self.assertNotEqual(run(name, bad).returncode, 0, bad)
                self.assertFalse((root/name).exists(), bad)
            missing=subprocess.run([TOOL,str(elf),str(root/'missing'),'--entries',str(root/'absent')],capture_output=True)
            self.assertNotEqual(missing.returncode,0)
            self.assertFalse((root/'missing').exists())
            # The loaded image still backs four bytes, but only 1..3 of them
            # belong to executable code. Checking the first byte alone is wrong.
            for code_size in (21,22,23):
                elf.write_bytes(image(code_size))
                self.assertNotEqual(run(f'partial{code_size}','0x100014').returncode,0)
                self.assertFalse((root/f'partial{code_size}').exists())


if __name__ == '__main__': unittest.main()
