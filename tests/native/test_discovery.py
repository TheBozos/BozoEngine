"""Function discovery on an ELF without symbols, on a synthetic program with what a compiler leaves behind."""
import pathlib
import re
import struct
import subprocess
import sys
import tempfile
import unittest

EMIT = len(sys.argv) > 1 and sys.argv[1] == '--emit'
TOOL = None if EMIT else sys.argv.pop(1)
SCORE = None if EMIT else sys.argv.pop(1)
BASE = 0x100000
DATA = 0x110000
V0, A0, T0, T1, T9, SP, RA = 2, 4, 8, 9, 25, 29, 31


def i_type(op, rs, rt, imm):
    return (op << 26) | (rs << 21) | (rt << 16) | (imm & 0xFFFF)


def addiu(rt, rs, imm): return i_type(9, rs, rt, imm)
def sltiu(rt, rs, imm): return i_type(11, rs, rt, imm)
def lui(rt, imm): return i_type(15, 0, rt, imm)
def lw(rt, off, rs): return i_type(35, rs, rt, off)
def ld(rt, off, rs): return i_type(55, rs, rt, off)
def sd(rt, off, rs): return i_type(63, rs, rt, off)
def sll(rd, rt, sa): return (rt << 16) | (rd << 11) | (sa << 6)
def addu(rd, rs, rt): return (rs << 21) | (rt << 16) | (rd << 11) | 33
def jr(rs): return (rs << 21) | 8
def hi(a): return (a + 0x8000) >> 16


NOP = 0
JR_RA = jr(RA)


def beq(rs, rt, label): return lambda at, L: i_type(4, rs, rt, (L[label] - at - 4) // 4)
def b(label): return beq(0, 0, label)
def j(label): return lambda at, L: (2 << 26) | (L[label] >> 2)
def jal(label): return lambda at, L: (3 << 26) | (L[label] >> 2)
def lui_of(rt, label): return lambda at, L: lui(rt, hi(L[label]))
def addiu_of(rt, rs, label): return lambda at, L: addiu(rt, rs, L[label])
def lui_data(rt, address): return lui(rt, hi(address))
def addiu_data(rt, rs, address): return addiu(rt, rs, address)


TABLE = DATA
SOURCE = [
    'main', addiu(SP, SP, -16), sd(RA, 0, SP), jal('switcher'), NOP, ld(RA, 0, SP), JR_RA, addiu(SP, SP, 16),
    'padding', NOP,
    'switcher', sltiu(T0, A0, 3), beq(T0, 0, 'default'), sll(T0, A0, 2), lui_data(T1, TABLE), addiu_data(T1, T1, TABLE),
    addu(T0, T0, T1), lw(T0, 0, T0), 'switcher_jr', jr(T0), NOP,
    'case0', j('end'), addiu(V0, 0, 10),
    'case1', j('end'), addiu(V0, 0, 11),
    'case2', addiu(V0, 0, 12), b('end'), NOP,
    'default', addiu(V0, 0, -1),
    'end', JR_RA, NOP,
    'by_pointer', JR_RA, addiu(V0, 0, 7),
    'early_return', beq(A0, 0, 'late'), NOP, JR_RA, addiu(V0, 0, 1),
    'late', JR_RA, addiu(V0, 0, 2),
    'backward', b('middle'), NOP,
    'top', JR_RA, addiu(V0, 0, 3),
    'middle', b('top'), NOP,
    'no_return', addiu(SP, SP, -16), sd(RA, 0, SP), jal('switcher'), NOP,
    'after_no_return', addiu(SP, SP, -32), sd(RA, 0, SP), ld(RA, 0, SP), JR_RA, addiu(SP, SP, 32),
    'tail_to_next', j('next'), NOP,
    'next', JR_RA, addiu(V0, 0, 5),
    'through_register', lw(T9, 0, A0), 'through_register_jr', jr(T9), NOP,
    'slots', lui_of(T1, 'slot0'), addiu_of(T1, T1, 'slot0'), sll(T0, A0, 3), addu(T1, T1, T0), 'slots_jr', jr(T1), NOP,
    'slot0', b('slots_end'), addiu(V0, 0, 20),
    'slot1', b('slots_end'), addiu(V0, 0, 21),
    'slots_end', JR_RA, NOP,
]
FUNCTIONS = ['main', 'switcher', 'by_pointer', 'early_return', 'backward', 'no_return', 'after_no_return',
             'tail_to_next', 'next', 'through_register', 'slots']


def assemble():
    labels, at = {}, BASE
    for item in SOURCE:
        if isinstance(item, str):
            labels[item] = at
        else:
            at += 4
    words, at = [], BASE
    for item in SOURCE:
        if isinstance(item, str):
            continue
        words.append(item(at, labels) if callable(item) else item)
        at += 4
    return words, labels, at


WORDS, L, CODE_END = assemble()
POINTER_WORD = DATA + 12
DATA_WORDS = [L['case0'], L['case1'], L['case2'], L['by_pointer'], L['switcher'] + 4, L['switcher'] + 8, L['padding']]


def image(symbols=()):
    names = b'\0.text\0.data\0.strtab\0.symtab\0.shstrtab\0'
    code = struct.pack('<%dI' % len(WORDS), *WORDS)
    data = struct.pack('<%dI' % len(DATA_WORDS), *DATA_WORDS)
    strings, table = b'\0', bytes(16)
    for name, address, size in symbols:
        table += struct.pack('<IIIBBH', len(strings), address, size, 0x12, 0, 1)
        strings += name.encode() + b'\0'
    code_at, data_at = 0x100, 0x100 + len(code)
    strings_at = data_at + len(data)
    table_at = strings_at + len(strings)
    names_at = table_at + len(table)
    section_at = (names_at + len(names) + 3) & ~3
    header = struct.pack('<16sHHIIIIIHHHHHH', b'\x7fELF\x01\x01\x01' + bytes(9), 2, 8, 1,
                         BASE, 52, section_at, 0, 52, 32, 2, 40, 6, 5)
    ph = struct.pack('<8I', 1, code_at, BASE, BASE, len(code), len(code), 5, 16)
    ph += struct.pack('<8I', 1, data_at, DATA, DATA, len(data), len(data), 6, 16)
    out = header + ph + bytes(0x100 - 52 - 64) + code + data + strings + table + names
    out += bytes(section_at - len(out))
    sections = [[0] * 10,
                [1, 1, 6, BASE, code_at, len(code), 0, 0, 4, 0],
                [7, 1, 3, DATA, data_at, len(data), 0, 0, 4, 0],
                [13, 3, 0, 0, strings_at, len(strings), 0, 0, 1, 0],
                [21, 2, 0, 0, table_at, len(table), 3, 1, 4, 16],
                [29, 3, 0, 0, names_at, len(names), 0, 0, 1, 0]]
    return out + b''.join(struct.pack('<10I', *s) for s in sections)


def all_symbols():
    ordered = sorted(L[f] for f in FUNCTIONS) + [CODE_END]
    return [(f, L[f], ordered[ordered.index(L[f]) + 1] - L[f]) for f in FUNCTIONS]


class Run:
    def __init__(self, elf_bytes, *options, map_text=None):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            (root / 'case.elf').write_bytes(elf_bytes)
            command = [TOOL, str(root / 'case.elf'), str(root / 'out'), '--files', '1', '--no-comments',
                       '--report', str(root / 'report.txt'), *options]
            if map_text:
                (root / 'map.txt').write_text(map_text)
                command += ['--symbols', str(root / 'map.txt')]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            self.stdout = result.stdout
            self.code = ''.join(p.read_text() for p in (root / 'out').glob('*.cpp'))
            self.report_text = (root / 'report.txt').read_text()
            self.score = subprocess.run([sys.executable, '-I', SCORE, str(root / 'case.elf'), str(root / 'report.txt')],
                                        capture_output=True, text=True).stdout
        self.rows = [line.split() for line in self.report_text.splitlines()]

    def rows_of(self, kind):
        return [r for r in self.rows if r[0] == kind]

    def starts(self):
        return {int(r[1], 16) for r in self.rows_of('function')}


class DiscoveryTests(unittest.TestCase):
    def test_functions_and_nothing_else(self):
        run = Run(image())
        self.assertEqual(run.rows[0], ['mode', 'code'])
        self.assertEqual(run.starts(), {L[f] for f in FUNCTIONS})

    def test_switch_is_a_goto_inside_the_function(self):
        run = Run(image())
        tables = {int(r[1], 16): r for r in run.rows_of('table')}
        row = tables[L['switcher_jr']]
        self.assertEqual(row[3], 'goto')
        self.assertEqual([int(x, 16) for x in row[4:]], [L['case0'], L['case1'], L['case2']])
        self.assertRegex(run.code, r'switch \(target\) \{ case \w+: goto L_%x; ' % L['case0'])

    def test_jump_to_code_slots(self):
        run = Run(image())
        tables = {int(r[1], 16): r for r in run.rows_of('table')}
        targets = [int(x, 16) for x in tables[L['slots_jr']][4:]]
        self.assertIn(L['slot0'], targets)
        self.assertIn(L['slot1'], targets)

    def test_register_jump_is_listed_and_dispatched(self):
        run = Run(image())
        self.assertEqual([int(r[1], 16) for r in run.rows_of('unresolved')], [L['through_register_jr']])
        self.assertRegex(run.code, r'if \(target == entryRa\).*bozo_call_indirect\(rdram, ctx, rt, target\)')

    def test_data_pointers_are_filtered(self):
        run = Run(image())
        entries = {int(r[1], 16) for r in run.rows_of('entry')}
        self.assertIn(L['switcher'] + 4, entries)
        self.assertNotIn(L['switcher'] + 8, entries)
        self.assertNotIn(L['padding'], entries)
        self.assertNotIn(L['case0'], entries)
        self.assertEqual(re.findall(r'warning: .*', run.stdout), [])

    def test_symbols_keep_their_boundaries(self):
        symbols = all_symbols()
        with_symbols = Run(image(symbols))
        self.assertEqual(with_symbols.rows[0], ['mode', 'symbols'])
        self.assertEqual({(int(r[1], 16), int(r[2], 16)) for r in with_symbols.rows_of('function')},
                         {(a, a + s) for _, a, s in symbols})
        self.assertNotIn('discovery:', with_symbols.stdout)

    def test_hidden_symbols_score_against_the_symbols(self):
        run = Run(image(all_symbols()), '--no-elf-symbols')
        self.assertEqual(run.rows[0], ['mode', 'code'])
        self.assertEqual(run.starts(), {L[f] for f in FUNCTIONS})
        self.assertIn('all: 11 symbols, 11 found, 11 match: precision 1.0000, recall 1.0000', run.score)
        self.assertRegex(run.score, r'jump tables 2: 2 inside a symbol, 2 of those')

    def test_few_symbols_do_not_stop_discovery(self):
        run = Run(image([('next', L['next'], 8)]))
        self.assertEqual(run.rows[0], ['mode', 'code'])
        self.assertEqual(run.starts(), {L[f] for f in FUNCTIONS})
        self.assertIn(['function', hex(L['next']), hex(L['next'] + 8), 'next'], run.rows)

    def test_map_name_is_a_start(self):
        run = Run(image(), map_text='named 0x%x\n' % L['top'])
        self.assertIn(L['top'], run.starts())
        self.assertIn('named_0x%x' % L['top'], run.code)


if __name__ == '__main__':
    if EMIT:
        root = pathlib.Path(sys.argv[2])
        root.mkdir(parents=True, exist_ok=True)
        (root / 'fixture.elf').write_bytes(image())
        with open(root / 'addresses.h', 'w') as out:
            for name, address in L.items():
                out.write('constexpr uint32_t k_%s = 0x%x;\n' % (name, address))
            out.write('constexpr uint32_t kCodeEnd = 0x%x;\nconstexpr uint32_t kPointerWord = 0x%x;\n' % (CODE_END, POINTER_WORD))
            out.write('constexpr uint32_t kData = 0x%x;\nconstexpr uint32_t kDataWords = %d;\n' % (DATA, len(DATA_WORDS)))
            out.write('constexpr uint32_t kFileCode = 0x100;\n')
    else:
        unittest.main()
