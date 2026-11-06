#!/usr/bin/env python3
"""Symbol maps: bozo-externals and bozo-recomp read one like the ELF's symbols, and tools/symbolmap names a stripped
executable from a reference that keeps its symbols, leaving the ambiguous out. Synthetic ELFs only."""
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

EXTERNALS = sys.argv.pop(1)
RECOMP = sys.argv.pop(1)
MATCHER = sys.argv.pop(1)
BASE = 0x100000
TEXT = 0x1000


def elf(words, symbols=None):
    """A little-endian EE executable: its code at BASE, a symbol table when `symbols` (name, address, size) is given."""
    code = struct.pack(f"<{len(words)}I", *words)
    strtab = b"\0"
    entries = b"\0" * 16
    for name, address, size in symbols or []:
        entries += struct.pack("<IIIBBH", len(strtab), address, size, 0x12, 0, 1)
        strtab += name.encode() + b"\0"
    names = b"\0.text\0.shstrtab\0.symtab\0.strtab\0"
    at = TEXT + len(code)
    sections = [[0] * 10, [1, 1, 6, BASE, TEXT, len(code), 0, 0, 4, 0], [7, 3, 0, 0, at, len(names), 0, 0, 1, 0]]
    tail = names
    if symbols is not None:
        sym_at = at + len(tail)
        sections.append([17, 2, 0, 0, sym_at, len(entries), 4, 1, 4, 16])
        tail += entries
        sections.append([25, 3, 0, 0, sym_at + len(entries), len(strtab), 0, 0, 1, 0])
        tail += strtab
    shoff = at + len(tail)
    header = b"\x7fELF\x01\x01\x01" + bytes(9) + struct.pack(
        "<HHIIIIIHHHHHH", 2, 8, 1, BASE, 52, shoff, 0, 52, 32, 1, 40, len(sections), 2)
    program = struct.pack("<8I", 1, TEXT, BASE, BASE, len(code), len(code), 5, 4)
    out = header + program
    out += bytes(TEXT - len(out)) + code + tail
    return out + b"".join(struct.pack("<10I", *s) for s in sections)


def jal(target):
    return 0x0C000000 | (target >> 2)


def wrapper(number):
    return [0x24030000 | (number & 0xFFFF), 0x0000000C, 0x03E00008, 0]


def leaf(seed, count=14):
    return [0x24020000 | (seed + i) for i in range(count)] + [0x03E00008, 0]


def sibling(number):
    return [0x27BDFFF0, 0xFFBF0000, 0x24050000 | number] + [0x24060000 | (0x200 + i) for i in range(20)] + \
        [0xDFBF0000, 0x03E00008, 0x27BD0010]


def caller(target, padding=30):
    return [0x27BDFFF0, 0xFFBF0000] + [0x24040000 | (0x100 + i) for i in range(padding)] + \
        [jal(target), 0, 0xDFBF0000, 0x03E00008, 0x27BD0010]


class SymbolMapReaders(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.elf = self.root / "stripped.elf"
        self.elf.write_bytes(elf(wrapper(68) + wrapper(9) + leaf(0x40)))

    def tearDown(self):
        self.temp.cleanup()

    def externals(self, text=None):
        command = [EXTERNALS, str(self.elf), str(self.root / "externals.txt")]
        if text is not None:
            path = self.root / "map.txt"
            path.write_text(text)
            command += ["--symbols", str(path)]
        return subprocess.run(command, capture_output=True, text=True)

    def test_externals_binds_like_elf_symbols(self):
        plain = self.externals()
        self.assertEqual(plain.returncode, 0, plain.stderr)
        self.assertNotIn("stub", (self.root / "externals.txt").read_text())
        named = self.externals("# made by hand\nWaitSema 0x100000\nscePadRead 0x100010  # the pad\ngame 0x100020\n")
        self.assertEqual(named.returncode, 0, named.stderr)
        out = (self.root / "externals.txt").read_text()
        self.assertIn("stub 0x100000 # WaitSema", out)
        self.assertIn("stub 0x100010 # scePadRead", out)
        self.assertNotIn("game", out)

    def test_bad_maps_are_refused(self):
        for bad in ["scePadRead\n", "scePadRead 0x100000 extra\n", "scePadRead nonsense\n", "scePadRead -4\n",
                    "scePadRead 0x100002\n", "scePadRead 0x900000\n", "scePadRead 0x100000\nscePadRead 0x100010\n"]:
            with self.subTest(bad):
                self.assertNotEqual(self.externals(bad).returncode, 0)

    def test_recompiler_names_the_function(self):
        path = self.root / "map.txt"
        path.write_text("scePadRead 0x100010\n")
        out = self.root / "gen"
        result = subprocess.run([RECOMP, str(self.elf), str(out), "--files", "1", "--symbols", str(path)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        generated = "".join(p.read_text() for p in out.glob("*.cpp"))
        self.assertIn("scePadRead", generated)
        path.write_text("scePadRead 0x900000\n")
        self.assertNotEqual(subprocess.run([RECOMP, str(self.elf), str(out), "--symbols", str(path)],
                                           capture_output=True, text=True).returncode, 0)


class Matcher(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)

        # The reference's order: its names, its code.
        short = [0x24020007, 0x03E00008, 0]
        parts = [("twoPlaces", leaf(0x80)),
                 ("CreateThread", wrapper(32)),
                 ("AddHandler", wrapper(16)),
                 ("AddHandler2", wrapper(16)),
                 ("RemoveHandler", wrapper(17)),
                 ("libraryLeaf", leaf(0x40)),
                 ("rpcThree", sibling(3)),
                 ("shortCallee", short)]
        symbols, words = [], []
        for name, code in parts:
            symbols.append((name, BASE + 4 * len(words), 4 * len(code)))
            words += code
        short_at = symbols[-1][1]
        code = caller(short_at)
        symbols.append(("libraryCaller", BASE + 4 * len(words), 4 * len(code)))
        words += code
        (self.root / "ref.elf").write_bytes(elf(words, symbols))

        # The stripped target: game code first, so every address differs, and the same library after it.
        game = [0x24080000 | (0x500 + i) for i in range(20)] + [0x03E00008, 0]
        words = game + leaf(0x80)
        copy_at = BASE + 4 * len(game)
        at = {}
        for name, code in parts:
            at[name] = BASE + 4 * len(words)
            words += sibling(4) if name == "rpcThree" else code
        at["libraryCaller"] = BASE + 4 * len(words)
        words += caller(at["shortCallee"])
        at["gameCall"] = BASE + 4 * len(words)
        words += [jal(at["libraryLeaf"]), 0, jal(at["libraryCaller"]), 0, 0x03E00008, 0]
        self.at = at
        self.copy_at = copy_at
        (self.root / "target.elf").write_bytes(elf(words))

    def tearDown(self):
        self.temp.cleanup()

    def match(self, *extra):
        result = subprocess.run([sys.executable, "-I", MATCHER, "match", str(self.root / "target.elf"),
                                 "--ref", str(self.root / "ref.elf"), "--out", str(self.root / "map.txt"),
                                 "--report", str(self.root / "report.txt"), *extra], capture_output=True, text=True)
        names = {}
        if result.returncode == 0:
            for line in (self.root / "map.txt").read_text().splitlines():
                if not line.startswith("#"):
                    name, address = line.split()
                    names[name] = int(address, 16)
        return result, names

    def test_names_only_what_the_code_proves(self):
        result, names = self.match()
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ("CreateThread", "RemoveHandler", "libraryLeaf", "libraryCaller"):
            self.assertEqual(names.get(name), self.at[name], name)
        self.assertEqual(names.get("shortCallee"), self.at["shortCallee"], "named through its identified caller")
        self.assertEqual(names.get("AddHandler"), self.at["AddHandler"], "identical code told apart by its order")
        self.assertEqual(names.get("AddHandler2"), self.at["AddHandler2"], "identical code told apart by its order")
        self.assertNotIn("twoPlaces", names)
        self.assertNotIn("rpcThree", names, "another argument constant is a sibling, not this function")
        report = (self.root / "report.txt").read_text()
        self.assertIn("twoPlaces", report)
        self.assertIn(f"0x{self.at['twoPlaces']:06x}", report)
        self.assertIn(f"0x{self.copy_at:06x}", report)

    def test_reviewed_names_join_the_map_and_cannot_contradict_it(self):
        reviewed = self.root / "reviewed.txt"
        reviewed.write_text(f"twoPlaces 0x{self.at['twoPlaces']:x}  # read by hand\n")
        result, names = self.match("--reviewed", str(reviewed))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(names.get("twoPlaces"), self.at["twoPlaces"])
        reviewed.write_text(f"twoPlaces 0x{self.at['CreateThread']:x}\n")
        result, _ = self.match("--reviewed", str(reviewed))
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CreateThread", result.stderr + result.stdout)


if __name__ == "__main__":
    unittest.main()
