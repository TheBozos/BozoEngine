#!/usr/bin/env python3
# irx_disasm.py: disassembles a PS2 IOP module (IRX: an ELF32 MIPS R3000 relocatable, type 0xff80) for study.
#
# Copyright (c) The Bozos. MIT licence (see LICENSE at the repository root). Standard library only.
#
#   irx_disasm.py <module.irx> sections            the sections, the module's name and its import and export tables
#   irx_disasm.py <module.irx> list [from] [to]    the code from `from` to `to` (byte offsets into .text, hex)
#   irx_disasm.py <module.irx> calls               every call target with the calls made to it, and every import used
#   irx_disasm.py <module.irx> refs <addr>         the instructions that reference a data address (lui/addiu pairs,
#                                                  base+offset loads and stores through a lui-set register)
#   irx_disasm.py <module.irx> strings             the strings of .rodata and .data
#
# The module's code and data stay where they are: the tool reads the file it is given and prints to the terminal.
# Do not commit a module or a listing of it (the game's data).
"""Disassembler for PS2 IOP modules (MIPS I, little-endian)."""

import struct
import sys

REG = ['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7',
       's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1', 'gp', 'sp', 'fp', 'ra']


class Irx:
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        if d[:4] != b'\x7fELF':
            raise SystemExit('%s: not an ELF file' % path)
        (self.type, self.machine, _, self.entry, _, shoff, _, _, _, _, shentsize, shnum, shstrndx) = \
            struct.unpack_from('<HHIIIIIHHHHHH', d, 16)
        self.sections = []
        for i in range(shnum):
            name, typ, flags, addr, off, size, link, info, align, es = struct.unpack_from('<10I', d, shoff + i * shentsize)
            self.sections.append(dict(name=name, type=typ, addr=addr, off=off, size=size, info=info))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            n = d[strtab['off'] + s['name']:]
            s['name'] = n[:n.index(b'\0')].decode('latin-1')
        self.mem = bytearray(max(s['addr'] + s['size'] for s in self.sections if s['name'] in ('.text', '.rodata', '.data', '.bss', '.sbss')))
        for s in self.sections:
            if s['name'] in ('.text', '.rodata', '.data'):
                self.mem[s['addr']:s['addr'] + s['size']] = d[s['off']:s['off'] + s['size']]
        self.text = next(s for s in self.sections if s['name'] == '.text')
        self.imports = {}  # stub address -> 'library:index'
        self.exports = {}  # function address -> 'library:index'
        self._tables()

    def word(self, a):
        return struct.unpack_from('<I', self.mem, a)[0]

    def _tables(self):
        t = self.text
        for a in range(t['addr'], t['addr'] + t['size'] - 20, 4):
            w = self.word(a)
            if w == 0x41E00000 and self.word(a + 4) == 0:  # import table
                lib = bytes(self.mem[a + 12:a + 20]).split(b'\0')[0].decode('latin-1')
                p = a + 20
                while p + 8 <= len(self.mem) and self.word(p) == 0x03E00008:
                    nxt = self.word(p + 4)
                    if nxt >> 16 != 0x2400:
                        break
                    self.imports[p] = '%s:%d' % (lib, nxt & 0xFFFF)
                    p += 8
            elif w == 0x41C00000 and self.word(a + 4) == 0:  # export table
                lib = bytes(self.mem[a + 12:a + 20]).split(b'\0')[0].decode('latin-1')
                p, i = a + 20, 0
                while p + 4 <= len(self.mem) and self.word(p) != 0:
                    self.exports[self.word(p)] = '%s:%d' % (lib, i)
                    p += 4
                    i += 1


def disasm(w, pc):
    op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31; sh = (w >> 6) & 31; fn = w & 63
    imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
    R = REG
    if w == 0:
        return 'nop', None
    if op == 0:
        names = {0: 'sll', 2: 'srl', 3: 'sra', 4: 'sllv', 6: 'srlv', 7: 'srav', 8: 'jr', 9: 'jalr', 12: 'syscall',
                 13: 'break', 16: 'mfhi', 17: 'mthi', 18: 'mflo', 19: 'mtlo', 24: 'mult', 25: 'multu', 26: 'div',
                 27: 'divu', 32: 'add', 33: 'addu', 34: 'sub', 35: 'subu', 36: 'and', 37: 'or', 38: 'xor', 39: 'nor',
                 42: 'slt', 43: 'sltu'}
        n = names.get(fn, 'special%d' % fn)
        if fn in (0, 2, 3): return '%s %s, %s, %d' % (n, R[rd], R[rt], sh), None
        if fn in (4, 6, 7): return '%s %s, %s, %s' % (n, R[rd], R[rt], R[rs]), None
        if fn == 8: return 'jr %s' % R[rs], None
        if fn == 9: return 'jalr %s, %s' % (R[rd], R[rs]), None
        if fn in (12, 13): return n, None
        if fn in (16, 18): return '%s %s' % (n, R[rd]), None
        if fn in (17, 19): return '%s %s' % (n, R[rs]), None
        if fn in (24, 25, 26, 27): return '%s %s, %s' % (n, R[rs], R[rt]), None
        return '%s %s, %s, %s' % (n, R[rd], R[rs], R[rt]), None
    if op == 1:
        n = {0: 'bltz', 1: 'bgez', 16: 'bltzal', 17: 'bgezal'}.get(rt, 'regimm%d' % rt)
        return '%s %s, 0x%x' % (n, R[rs], pc + 4 + (simm << 2)), pc + 4 + (simm << 2)
    if op in (2, 3):
        t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        return '%s 0x%x' % ('j' if op == 2 else 'jal', t), t
    if op in (4, 5):
        t = pc + 4 + (simm << 2)
        return '%s %s, %s, 0x%x' % ('beq' if op == 4 else 'bne', R[rs], R[rt], t), t
    if op in (6, 7):
        t = pc + 4 + (simm << 2)
        return '%s %s, 0x%x' % ('blez' if op == 6 else 'bgtz', R[rs], t), t
    names = {8: 'addi', 9: 'addiu', 10: 'slti', 11: 'sltiu', 12: 'andi', 13: 'ori', 14: 'xori'}
    if op in names:
        v = simm if op in (8, 9, 10, 11) else imm
        return '%s %s, %s, %s' % (names[op], R[rt], R[rs], hex(v) if op >= 12 else str(v)), None
    if op == 15: return 'lui %s, 0x%x' % (R[rt], imm), None
    if op == 16:
        if rs == 0: return 'mfc0 %s, $%d' % (R[rt], rd), None
        if rs == 4: return 'mtc0 %s, $%d' % (R[rt], rd), None
        if rs == 16 and fn == 16: return 'rfe', None
        return 'cop0 0x%x' % w, None
    if op == 18: return 'cop2 0x%x' % w, None
    mem = {32: 'lb', 33: 'lh', 34: 'lwl', 35: 'lw', 36: 'lbu', 37: 'lhu', 38: 'lwr', 40: 'sb', 41: 'sh', 42: 'swl',
           43: 'sw', 46: 'swr', 50: 'lwc2', 58: 'swc2'}
    if op in mem: return '%s %s, %d(%s)' % (mem[op], R[rt], simm, R[rs]), None
    return '.word 0x%08x' % w, None


def listing(irx, lo, hi, out=sys.stdout):
    hi_reg = {}  # register -> value set by lui (cleared at labels and branches)
    for pc in range(lo, hi, 4):
        w = irx.word(pc)
        text, target = disasm(w, pc)
        note = ''
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31
        imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 15:
            hi_reg[rt] = imm << 16
        elif op in (9, 13) and rs in hi_reg:
            v = (hi_reg[rs] + simm) if op == 9 else (hi_reg[rs] | imm)
            note = '= 0x%x' % (v & 0xFFFFFFFF)
            hi_reg[rt] = v & 0xFFFFFFFF
        elif op >= 32 and rs in hi_reg:
            note = '[0x%x]' % ((hi_reg[rs] + simm) & 0xFFFFFFFF)
            if op < 40: hi_reg.pop(rt, None)
        elif op == 0 and ((w >> 11) & 31) in hi_reg:
            hi_reg.pop((w >> 11) & 31, None)
        elif op in (8, 10, 11, 12, 14) or (op >= 32 and op < 40):
            hi_reg.pop(rt, None)
        if target is not None:
            if target in irx.imports: note = irx.imports[target]
            elif target in irx.exports: note = 'export ' + irx.exports[target]
        name = ''
        if pc in irx.exports: name = '  <export %s>' % irx.exports[pc]
        if pc in irx.imports: name = '  <import %s>' % irx.imports[pc]
        if name: out.write('%s\n' % name.strip())
        out.write('%06x: %08x  %-34s %s\n' % (pc, w, text, note))


def main(argv):
    if len(argv) < 3:
        print(__doc__ or '', 'usage: irx_disasm.py <module.irx> sections|list|calls|refs|strings ...')
        return 2
    irx = Irx(argv[1])
    cmd = argv[2]
    t = irx.text
    if cmd == 'sections':
        for s in irx.sections:
            print('%-10s addr 0x%06x size 0x%06x' % (s['name'], s['addr'], s['size']))
        print('entry 0x%x' % irx.entry)
        for a, n in sorted(irx.imports.items()): print('import 0x%06x %s' % (a, n))
        for a, n in sorted(irx.exports.items()): print('export 0x%06x %s' % (a, n))
    elif cmd == 'list':
        lo = int(argv[3], 16) if len(argv) > 3 else t['addr']
        hi = int(argv[4], 16) if len(argv) > 4 else t['addr'] + t['size']
        listing(irx, lo, hi)
    elif cmd == 'calls':
        calls = {}
        for pc in range(t['addr'], t['addr'] + t['size'], 4):
            w = irx.word(pc)
            if w >> 26 == 3:
                tg = ((w & 0x3FFFFFF) << 2)
                calls.setdefault(tg, []).append(pc)
        for tg in sorted(calls):
            print('0x%06x %-16s %d calls: %s' % (tg, irx.imports.get(tg, irx.exports.get(tg, '')), len(calls[tg]),
                                                  ' '.join('%x' % c for c in calls[tg][:12])))
    elif cmd == 'refs':
        want = int(argv[3], 16)
        hi_reg = {}
        for pc in range(t['addr'], t['addr'] + t['size'], 4):
            w = irx.word(pc); op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31
            imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
            if op == 15: hi_reg[rt] = imm << 16; continue
            if rs in hi_reg and (op == 9 or op >= 32):
                if (hi_reg[rs] + simm) & 0xFFFFFFFF == want:
                    print('%06x: %s' % (pc, disasm(w, pc)[0]))
    elif cmd == 'strings':
        for s in irx.sections:
            if s['name'] in ('.rodata', '.data'):
                b = irx.mem[s['addr']:s['addr'] + s['size']]; cur = b''
                for i, c in enumerate(b):
                    if 32 <= c < 127: cur += bytes([c])
                    else:
                        if len(cur) >= 4: print('0x%06x %s' % (s['addr'] + i - len(cur), cur.decode()))
                        cur = b''
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
