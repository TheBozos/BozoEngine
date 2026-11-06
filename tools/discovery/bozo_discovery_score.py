#!/usr/bin/env python3
"""Scores function discovery on an executable that keeps its symbols (docs/specs/discovery.md).

    bozo-recomp game.elf out --no-elf-symbols --report hidden.txt
    bozo_discovery_score.py game.elf hidden.txt
"""
import bisect
import struct
import sys


def read_elf(path):
    data = open(path, 'rb').read()
    shoff = struct.unpack_from('<I', data, 32)[0]
    entsize, count, names_index = struct.unpack_from('<HHH', data, 46)
    sections = [struct.unpack_from('<10I', data, shoff + i * entsize) for i in range(count)]
    text = lambda base, at: data[base + at:data.index(b'\0', base + at)].decode('latin-1')
    names = sections[names_index][4]
    code, truth, objects = [], {}, []
    for s in sections:
        name = text(names, s[0])
        if s[1] == 1 and s[2] & 2 and s[2] & 4 and name != '.vutext' and not name.startswith('.DVP'):
            code.append((s[3], s[3] + s[5], name))
    for s in sections:
        if s[1] != 2:
            continue
        strings = sections[s[6]][4]
        for k in range(s[5] // 16):
            name, value, size, info, _, _ = struct.unpack_from('<IIIBBH', data, s[4] + k * 16)
            kind = info & 15
            if kind == 2 and size and not value & 3 and any(a <= value < b for a, b, _ in code):
                truth.setdefault(value, (value + size, text(strings, name)))
            elif kind == 1 and size:
                objects.append((value, value + size))
    return code, truth, sorted(objects)


def read_report(path):
    functions, tables, unresolved = {}, [], []
    for line in open(path):
        p = line.split()
        if p[0] == 'function':
            functions[int(p[1], 16)] = int(p[2], 16)
        elif p[0] == 'table':
            tables.append((int(p[1], 16), [int(x, 16) for x in p[4:]]))
        elif p[0] == 'unresolved':
            unresolved.append(int(p[1], 16))
    return functions, tables, unresolved


def ratio(a, b):
    return a / b if b else 1.0


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    code, truth, objects = read_elf(sys.argv[1])
    functions, tables, unresolved = read_report(sys.argv[2])
    starts = sorted(truth)

    def holder(address):
        i = bisect.bisect_right(starts, address) - 1
        return starts[i] if i >= 0 and address < truth[starts[i]][0] else None

    def in_object(address):
        i = bisect.bisect_right(objects, (address, 1 << 40)) - 1
        return i >= 0 and address < objects[i][1]

    found = set(functions)
    for low, high, name in code + [(0, 1 << 32, 'all')]:
        t = {a for a in truth if low <= a < high}
        f = {a for a in found if low <= a < high}
        hit = t & f
        print(f'{name}: {len(t)} symbols, {len(f)} found, {len(hit)} match: '
              f'precision {ratio(len(hit), len(f)):.4f}, recall {ratio(len(hit), len(t)):.4f}')
    false_starts = found - set(truth)
    inside = [a for a in false_starts if holder(a) is not None]
    print(f'false starts {len(false_starts)}: {len(inside)} inside a symbol, '
          f'{sum(1 for a in false_starts if in_object(a))} in a data object, the rest in code no function symbol covers')
    hit = found & set(truth)
    ends = sum(1 for a in hit if 0 <= functions[a] - truth[a][0] < 64)
    print(f'matching starts with the right end (padding allowed): {ends} of {len(hit)}')
    in_function = [(jr, targets) for jr, targets in tables if holder(jr) is not None]
    whole = sum(1 for jr, targets in in_function if all(holder(t) == holder(jr) for t in targets))
    print(f'jump tables {len(tables)}: {len(in_function)} inside a symbol, {whole} of those with every target in it')
    print(f'unresolved jr {len(unresolved)}: {sum(1 for a in unresolved if holder(a) is not None)} inside a symbol')


if __name__ == '__main__':
    main()
