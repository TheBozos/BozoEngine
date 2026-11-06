#!/usr/bin/env python3
"""Name the library functions of a stripped EE executable from reference executables that keep their symbols.

    bozo_symbolmap.py match <target.elf> --ref <reference.elf>... --out <map.txt> [--report <report.txt>]
                            [--want <names.txt>]... [--reviewed <names.txt>]... [--check]
    bozo_symbolmap.py engine-names <engine checkout> <names.txt>

The map is what bozo-recomp, bozo-externals and the free host read (docs/specs/symbol-map.md): one "name 0xADDR" line per
identified function. The report lists how each name was found, the --want names that were not, and every ambiguous
match with its candidates. engine-names writes the names the engine answers natively: the usual --want file.

Nothing is guessed. A name goes into the map only when the evidence picks one target function and one name:
  exact     the whole code equals one reference function's, relocated fields masked (jal/j targets, lui immediates, the
            low halves and offsets that use them, $gp offsets), no other reference name has that code and no other
            target function matches it; short code also needs its calls to go to the same functions;
  calls     an identified function calls it at the same place as the reference's calls the named function, and its code
            is close to the reference's (drift between SDK releases);
  string    it refers to the same string as the reference function, its code is close and no other function does both;
  layout    it sits between two identified functions of its library, in the reference's order, its code is close to the
            reference's, nothing there is as close (identical twins are told apart by that order);
  rpc       the same place and shape, and the same RPC function number passed to sceSifCallRpc;
  drift     the same place and shape, at least DRIFT_CALLS identified calls agree, DRIFT_LOADS constant loads agree;
  reviewed  a person read the code: a --reviewed line, whose comment carries the evidence.
A close function that loads another constant into an argument register is a sibling, never the same function. Anything
else with candidates (a same-shape candidate, for one) is reported as ambiguous and left out of the map.

--check matches a target that keeps its symbols as if it were stripped and scores the map against them: how the tiers
are measured (docs/specs/symbol-map.md).
"""

import argparse
import bisect
import difflib
import os
import re
import struct
import sys
from collections import Counter, defaultdict

JR_RA = 0x03E00008
SYSCALL = 0x0000000C

OP_J, OP_JAL, OP_LUI = 2, 3, 15
ALU_IMM = {8, 9, 10, 11, 12, 13, 14, 24, 25}
ADDRESS_ALU = {8, 9, 13, 24, 25}
GPR_LOADS = {26, 27, 30, 32, 33, 34, 35, 36, 37, 38, 39, 55}
FPR_LOADS = {49, 53}
STORES = {31, 40, 41, 42, 43, 44, 45, 46, 57, 61, 63}
GP = 28
ARGUMENTS = range(4, 12)

SIMILAR = 0.70
STRING_SIMILAR = 0.60
LAYOUT_SIMILAR = 0.75
LAYOUT_SIMILAR_SHORT = 0.90
LAYOUT_SHORT = 16
SHAPE_SIMILAR = 0.80
SHAPE_SIMILAR_SHORT = 0.92
SHAPE_MARGIN = 0.03
OBJECT_SIMILAR = 0.70
DRIFT_CONSTANTS = 0.60
DRIFT_CALLS = 3
DRIFT_LOADS = 2
DRIFT_SHAPE = 0.85
OBJECT_MARGIN = 0.05
OBJECT_SPAN = 0x8000
LEAF_WORDS = 12
CALLER_WORDS = 24
LAYOUT_MARGIN = 0.05
LAYOUT_PAIR = 0.50
LAYOUT_SPAN = 0x4000


class Elf:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:4] != b"\x7fELF" or d[4] != 1 or d[5] != 1:
            raise ValueError(f"{path}: not a 32-bit little-endian ELF")
        self.path = path
        self.entry = struct.unpack_from("<I", d, 24)[0]
        phoff, shoff = struct.unpack_from("<II", d, 28)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", d, 42)
        self.segments = []
        for i in range(phnum):
            ptype, off, vaddr, _, filesz, memsz, flags, _ = struct.unpack_from("<8I", d, phoff + i * phentsize)
            if ptype == 1:
                self.segments.append((vaddr, off, filesz, memsz, flags))
        self.sections = []
        for i in range(shnum):
            self.sections.append(struct.unpack_from("<10I", d, shoff + i * shentsize))
        names = self.sections[shstrndx] if shnum else None

        def sname(sh):
            if names is None:
                return ""
            start = names[4] + sh[0]
            return d[start:d.index(b"\0", start)].decode("latin-1")

        self.code = []
        self.data_ranges = []
        self.symbols = []
        for sh in self.sections:
            name, stype, flags, addr, _, size = sname(sh), sh[1], sh[2], sh[3], sh[4], sh[5]
            if stype == 1 and flags & 2 and size:
                if flags & 4:
                    if not name.startswith(".vu") and ".DVP" not in name:
                        self.code.append((addr, addr + size))
                else:
                    self.data_ranges.append((addr, addr + size))
            if stype == 2:
                strtab = self.sections[sh[6]]
                for k in range(size // 16):
                    st_name, value, ssize, info, _, shndx = struct.unpack_from("<IIIBBH", d, sh[4] + k * 16)
                    start = strtab[4] + st_name
                    n = d[start:d.index(b"\0", start)].decode("latin-1")
                    self.symbols.append((n, value, ssize, info & 15, shndx))
        if not self.code:
            self.code = [(v, v + fs) for v, _, fs, _, fl in self.segments if fl & 1]

    def in_code(self, a):
        return any(lo <= a < hi for lo, hi in self.code)

    def offset(self, a):
        for vaddr, off, filesz, _, _ in self.segments:
            if vaddr <= a < vaddr + filesz:
                return off + a - vaddr
        return None

    def word(self, a):
        o = self.offset(a)
        if o is None or o + 4 > len(self.data):
            return None
        return struct.unpack_from("<I", self.data, o)[0]

    def words(self, a, n):
        o = self.offset(a)
        if o is None:
            return []
        n = max(0, min(n, (len(self.data) - o) // 4))
        return list(struct.unpack_from(f"<{n}I", self.data, o))

    def cstring(self, a):
        o = self.offset(a)
        if o is None:
            return None
        end = self.data.find(b"\0", o, o + 256)
        if end - o < 4:
            return None
        s = self.data[o:end]
        if all(32 <= c < 127 or c in (9, 10, 13) for c in s):
            return s.decode("ascii")
        return None


class Body:
    """A function's code with its relocated fields masked, its calls and the strings it refers to."""

    __slots__ = ("start", "masked", "shape", "calls", "strings", "refs")

    def __init__(self, elf, start, words):
        self.start = start
        self.masked = []
        self.shape = [shape_token(w) for w in words]
        self.calls = []
        self.strings = set()
        self.refs = set()
        hi = {}
        for i, w in enumerate(words):
            pc = start + 4 * i
            op, rs, rt, rd = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31
            m = w
            if op in (OP_J, OP_JAL):
                m = w & 0xFC000000
                target = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if op == OP_JAL or not start <= target < start + 4 * len(words):
                    self.calls.append(target)
            elif op == OP_LUI:
                m = w & 0xFFFF0000
                if rt:
                    hi[rt] = (w & 0xFFFF) << 16
            elif op in ALU_IMM:
                if rs in hi or rs == GP:
                    m = w & 0xFFFF0000
                    if rs in hi and op in ADDRESS_ALU and rt:
                        self._ref(elf, (hi[rs] + _simm(w)) & 0xFFFFFFFF)
                        hi[rt] = hi[rs] + _simm(w) if op != 13 else hi[rs] | (w & 0xFFFF)
                    else:
                        hi.pop(rt, None)
                else:
                    hi.pop(rt, None)
            elif op in GPR_LOADS or op in FPR_LOADS or op in STORES:
                if rs in hi or rs == GP:
                    m = w & 0xFFFF0000
                    if rs in hi:
                        self._ref(elf, (hi[rs] + _simm(w)) & 0xFFFFFFFF)
                if op in GPR_LOADS:
                    hi.pop(rt, None)
            elif op == 0:
                funct = w & 63
                if funct in (33, 45, 37) and (rs in hi or rt in hi) and rd:
                    hi[rd] = hi.get(rs, hi.get(rt))
                elif funct not in (8, 12, 13, 15, 24, 25, 26, 27):
                    hi.pop(rd, None)
            self.masked.append(m)

    def _ref(self, elf, a):
        if elf is None:
            return
        if elf.in_code(a):
            self.refs.add(a)
            return
        s = elf.cstring(a)
        if s is not None and len(s) >= 6:
            self.strings.add(s)


def shape_token(w):
    """What an instruction does, without its registers and fields: what survives recompiling a library's source."""
    op = w >> 26
    if op == 0:
        return w & 63
    if op in (16, 17, 18, 19):
        return (op << 8) | ((w >> 21) & 31)
    if op == 28:
        return (op << 8) | (w & 63)
    return op << 8


def syscall_number(masked):
    """The EE kernel call a body makes (li v1,N just before syscall), or None."""
    for prev, w in zip(masked, masked[1:]):
        if w == SYSCALL and prev >> 16 == 0x2403:
            return _simm(prev)
    return None


def _simm(w):
    v = w & 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def constants(masked):
    """The immediates a function keeps across recompiling: small constants and fields, not relocated ones."""
    out = []
    for w in masked:
        op = w >> 26
        imm = w & 0xFFFF
        if not imm:
            continue
        if op in ALU_IMM and (w >> 21) & 31 != 29 or op in GPR_LOADS or op in STORES:
            out.append(imm)
    return out


def constant_overlap(a, b):
    ca, cb = Counter(constants(a)), Counter(constants(b))
    if not ca and not cb:
        return 0.0
    return sum((ca & cb).values()) / max(sum(ca.values()), sum(cb.values()))


def shared_constant_loads(a, b):
    """How many distinctive constants the two load into the same register (li r,N with N not 0, 1 or -1)."""
    def loads(masked):
        return Counter(w for w in masked if w >> 26 == 9 and (w >> 21) & 31 == 0
                       and _simm(w) not in (0, 1, -1) and (w >> 16) & 31 != 29)
    return sum((loads(a) & loads(b)).values())


def rpc_arguments(body, callees):
    """The function number each call to sceSifCallRpc passes (li a1,N in the few instructions up to its delay slot)."""
    out = []
    for i, w in enumerate(body.masked):
        if w >> 26 != OP_JAL:
            continue
        pc = body.start + 4 * i
        if not callees & {c for c in body.calls if ((pc + 4) & 0xF0000000) == (c & 0xF0000000)}:
            continue
        for k in range(i + 1, max(i - 8, -1), -1):
            if k < len(body.masked) and body.masked[k] >> 16 == 0x2405:
                out.append(_simm(body.masked[k]))
                break
    return sorted(set(out))


def constant_conflict(a, b):
    """Aligned code that loads different constants into the same argument register (li a1,N against li a1,M): sibling
    functions (an RPC or kernel call by number), not one function in two SDK releases."""
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag != "replace" or i2 - i1 != j2 - j1:
            continue
        for x, y in zip(a[i1:i2], b[j1:j2]):
            if x >> 26 in (9, 13) and (x >> 21) & 31 == 0 and x >> 16 == y >> 16 and x != y and (x >> 16) & 31 in ARGUMENTS:
                return True
    return False


def similarity(a, b):
    sm = difflib.SequenceMatcher(None, a, b, autojunk=False)
    if sm.real_quick_ratio() < SIMILAR * 0.75 or sm.quick_ratio() < STRING_SIMILAR:
        return sm.quick_ratio() * 0.5
    return sm.ratio()


class Reference:
    """Every sized function of a reference executable, by address, with its names."""

    def __init__(self, elf):
        self.elf = elf
        self.names = defaultdict(list)
        sizes = {}
        starts = sorted({v for _, v, _, t, sh in elf.symbols if t in (0, 2) and sh and elf.in_code(v)})
        nexts = dict(zip(starts, starts[1:] + [None]))
        for name, value, size, stype, shndx in elf.symbols:
            if stype != 2 or not name or value == 0 or not elf.in_code(value) or value & 3:
                continue
            if name.startswith(("gcc2_compiled", "__gnu_compiled")):
                continue
            if size == 0 and nexts.get(value):
                size = nexts[value] - value
                while size > 4 and elf.word(value + size - 4) == 0 and elf.word(value + size - 8) != JR_RA:
                    size -= 4
            if size < 4:
                continue
            self.names[value].append(name)
            sizes[value] = max(sizes.get(value, 0), size)
        self.bodies = {}
        for a, size in sizes.items():
            self.bodies[a] = Body(elf, a, elf.words(a, size // 4))
        self.order = sorted(self.bodies)

    def name(self, a):
        return preferred(self.names[a])


def preferred(names):
    return sorted(set(names), key=lambda n: (n.startswith("_"), len(n), n))[0]


class Target:
    """The stripped executable: where functions start, and their code up to the next start."""

    def __init__(self, elf):
        self.elf = elf
        strong, weak = set(), set()
        if elf.in_code(elf.entry):
            strong.add(elf.entry)
        for lo, hi in elf.code:
            words = elf.words(lo, (hi - lo) // 4)
            body = Body(elf, lo, words)
            strong.update(c for c in body.calls if elf.in_code(c))
            strong.update(r for r in body.refs if not r & 3)
            for i, w in enumerate(words):
                op = w >> 26
                if op == OP_J:
                    t = (((lo + 4 * i) + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                    if elf.in_code(t):
                        weak.add(t)
                if w == JR_RA:
                    k = i + 2
                    while k < len(words) and words[k] == 0:
                        k += 1
                    if k < len(words):
                        weak.add(lo + 4 * k)
        self.strong = strong
        self.starts = sorted(strong | weak)
        self.ends = {}
        for lo, hi in elf.code:
            seq = [s for s in self.starts if lo <= s < hi]
            for a, b in zip(seq, seq[1:] + [hi]):
                self.ends[a] = b
        self._bodies = {}

    def body(self, start, limit_words=None):
        key = (start, limit_words)
        if key not in self._bodies:
            end = self.ends.get(start)
            n = (end - start) // 4 if end else 0
            if limit_words is not None:
                n = limit_words
            words = self.elf.words(start, n)
            while words and words[-1] == 0 and len(words) > 1 and limit_words is None:
                words.pop()
            self._bodies[key] = Body(self.elf, start, words)
        return self._bodies[key]

    def whole(self, start):
        return self.body(start)

    def boundary_ok(self, start, n):
        after = start + 4 * n
        if not self.elf.in_code(after):
            return True
        if after in self.ends:
            return True
        w = self.elf.word(after)
        return w == 0


class ByLength:
    def __init__(self, items):
        self.items = sorted(items)
        self.keys = [k for k, _ in self.items]

    def get_range(self, lo, hi):
        i, j = bisect.bisect_left(self.keys, lo), bisect.bisect_right(self.keys, hi)
        return [n for _, n in self.items[i:j]]


class Matcher:
    def __init__(self, target, refs, min_words):
        self.t = target
        self.refs = refs
        self.min_words = min_words
        self.named = {}      # target address -> (name, how, detail)
        self.by_name = {}    # name -> target address
        self.ambiguous = {}  # name -> (reason, candidates)
        self.ref_of = {}     # name -> (reference, reference address)
        self.groups = []     # (names, target addresses, words): code shared by several names or places
        for ri, ref in enumerate(refs):
            for a in ref.bodies:
                n = ref.name(a)
                self.ref_of.setdefault(n, (ri, a))
        self.by_length = ByLength((len(self._ref_body(n)[1].masked), n) for n in self.ref_of)

    def rival(self, addr, name, score, words):
        """Another reference function whose code is at least as close to the target's: the name is not proved."""
        body = self.t.body(addr, words).masked
        lo, hi = int(words * 0.75), int(words * 1.34) + 1
        for n in self.by_length.get_range(lo, hi):
            if canonical(n) == canonical(name) or (n in self.by_name and self.by_name[n] != addr):
                continue
            _, rb = self._ref_body(n)
            sm = difflib.SequenceMatcher(None, rb.masked, body, autojunk=False)
            if sm.real_quick_ratio() >= score and sm.quick_ratio() >= score and sm.ratio() >= score:
                return n
        return None

    def accept(self, addr, name, how, detail="", score=None):
        if addr in self.named or name in self.by_name:
            return False
        if score is not None:
            _, rbody = self._ref_body(name)
            if constant_conflict(rbody.masked, self.t.body(addr, len(rbody.masked)).masked):
                self.ambiguous[name] = (f"{how}: {detail}, but it loads other constants", [addr])
                return False
            other = self.rival(addr, name, score, len(rbody.masked))
            if other:
                self.ambiguous[name] = (f"{how}: {detail}, but {other} is as alike", [addr])
                return False
        self.named[addr] = (name, how, detail)
        self.by_name[name] = addr
        self.ambiguous.pop(name, None)
        return True

    def specific(self, sig):
        """Code long enough that sharing it means being the same function: a kernel call wrapper (its call number),
        a leaf of LEAF_WORDS or more, a function with calls of CALLER_WORDS or more. Shorter code is named only when
        its calls go to the same functions as the reference's."""
        if syscall_number(sig) is not None:
            return len(sig) >= self.min_words
        calls = sum(1 for w in sig if w >> 26 in (OP_J, OP_JAL))
        return len(sig) >= (CALLER_WORDS if calls else LEAF_WORDS)

    def run_exact(self):
        prefix = 6
        index = defaultdict(list)
        for s in self.t.starts:
            for k in range(1, prefix + 1):
                index[(k, tuple(self.t.body(s, k).masked))].append(s)
        signatures = defaultdict(set)
        sources = defaultdict(list)
        for ri, ref in enumerate(self.refs):
            for a, body in ref.bodies.items():
                sig = tuple(body.masked)
                signatures[sig].add(ref.name(a))
                sources[sig].append((ri, a))
        for sig, names in signatures.items():
            if len(sig) < 1:
                continue
            k = min(len(sig), prefix)
            hits = []
            for s in index.get((k, sig[:k]), ()):
                if len(sig) > k and tuple(self.t.body(s, len(sig)).masked) != sig:
                    continue
                if self.t.boundary_ok(s, len(sig)):
                    hits.append(s)
            if not hits:
                continue
            if not self.specific(sig):
                for n in names:
                    if n not in self.by_name:
                        self.ambiguous.setdefault(n, ("too short to tell by its code alone", hits))
                self.groups.append((names, hits, len(sig)))
                continue
            if len(names) > 1 or len(hits) > 1:
                self.groups.append((names, hits, len(sig)))
                for n in names:
                    if len(names) > 1:
                        self.ambiguous.setdefault(n, ("the same code as " + ", ".join(sorted(names - {n})), hits))
                    else:
                        self.ambiguous.setdefault(n, ("the same code at several places", hits))
                continue
            name = next(iter(names))
            self.accept(hits[0], name, "exact", f"{len(sig)} words")

    def _ref_body(self, name):
        ri, a = self.ref_of[name]
        return self.refs[ri], self.refs[ri].bodies[a]

    def _paired_calls(self, ref, rbody, tbody):
        """(reference callee name, target callee) pairs, aligned on the calls both sides already agree on."""
        rc = [ref.name(c) if c in ref.bodies else None for c in rbody.calls]
        tc = [self.named[c][0] if c in self.named else None for c in tbody.calls]
        if len(rc) == len(tc):
            return list(zip(rc, tbody.calls))
        pairs = []
        sm = difflib.SequenceMatcher(None, [x or "?" for x in rc], [x or "?" for x in tc], autojunk=False)
        for tag, i1, i2, j1, j2 in sm.get_opcodes():
            if tag in ("equal", "replace") and i2 - i1 == j2 - j1:
                pairs.extend(zip(rc[i1:i2], tbody.calls[j1:j2]))
        return pairs

    def run_calls(self):
        changed = True
        while changed:
            changed = False
            votes = defaultdict(set)
            for taddr, (name, _, _) in list(self.named.items()):
                if name not in self.ref_of:
                    continue
                ref, rbody = self._ref_body(name)
                tbody = self.t.whole(taddr)
                if self.named[taddr][1] == "exact":
                    tbody = self.t.body(taddr, len(rbody.masked))
                for callee, tcallee in self._paired_calls(ref, rbody, tbody):
                    if callee and callee not in self.by_name and self.t.elf.in_code(tcallee):
                        votes[callee].add((tcallee, name))
            for callee, cands in votes.items():
                addrs = {a for a, _ in cands}
                if len(addrs) != 1:
                    self.ambiguous[callee] = ("identified callers call different functions here",
                                              sorted(addrs))
                    continue
                addr = next(iter(addrs))
                if addr in self.named:
                    continue
                _, rbody = self._ref_body(callee)
                score = similarity(rbody.masked, self.t.whole(addr).masked)
                callers = ", ".join(sorted({c for _, c in cands}))
                if score >= SIMILAR:
                    if self.accept(addr, callee, "calls", f"called from {callers}, code {score:.2f} alike",
                                   score=score):
                        changed = True
                else:
                    self.ambiguous.setdefault(callee, (f"called from {callers} but its code is only {score:.2f} alike",
                                                       [addr]))

    def absence(self, name):
        """Why a reference function has no match: where its identified neighbours put it in the target."""
        ri, a = self.ref_of[name]
        ref = self.refs[ri]
        i = bisect.bisect_left(ref.order, a)
        before = next((x for x in reversed(ref.order[:i]) if ref.name(x) in self.by_name), None)
        after = next((x for x in ref.order[i + 1:] if ref.name(x) in self.by_name), None)
        if before is None or after is None or ref.name(before) not in self.by_name:
            return "no identified neighbour in the target"
        lo, hi = self.by_name[ref.name(before)], self.by_name[ref.name(after)]
        if not 0 < hi - lo < LAYOUT_SPAN:
            return f"neighbours {ref.name(before)} and {ref.name(after)} are not together in the target"
        free = [s for s in self.t.starts if lo < s < hi and s not in self.named]
        if not free:
            return f"not linked: {ref.name(before)} and {ref.name(after)} are adjacent in the target"
        return f"{len(free)} unnamed functions between {ref.name(before)} and {ref.name(after)}, none alike"

    def _callee_names(self, ref, rbody):
        return [ref.name(c) if c in ref.bodies else None for c in rbody.calls]

    def run_groups(self):
        """Code several names or places share: the one candidate whose calls go where the name's calls go."""
        changed = False
        for names, hits, words in self.groups:
            agree = defaultdict(list)
            for name in names:
                if name in self.by_name or name not in self.ref_of:
                    continue
                ref, rbody = self._ref_body(name)
                want = self._callee_names(ref, rbody)
                for h in hits:
                    if h in self.named:
                        continue
                    got = [self.named[c][0] if c in self.named else None for c in self.t.body(h, words).calls]
                    if len(got) != len(want):
                        continue
                    known = [(w, g) for w, g in zip(want, got) if w and g]
                    if known and all(canonical(w) == canonical(g) for w, g in known):
                        agree[name].append(h)
            claimed = defaultdict(int)
            for hs in agree.values():
                for h in hs:
                    claimed[h] += 1
            for name, hs in agree.items():
                if len(hs) == 1 and claimed[hs[0]] == 1:
                    if self.accept(hs[0], name, "exact", f"{words} words, shared code told apart by its calls"):
                        changed = True
        return changed

    def run_layout(self):
        """Library objects keep their functions' order. Between two identified neighbours, the reference's functions
        and the target's are aligned in order (most similar code first); a pair is named when its code is close
        enough and neither side has a closer partner in that gap. Functions with identical code are told apart by
        that order."""
        changed = False
        for ri, ref in enumerate(self.refs):
            placed = [(a, self.by_name[ref.name(a)]) for a in ref.order
                      if ref.name(a) in self.by_name and self.ref_of.get(ref.name(a), (None,))[0] == ri]
            for (ra, ta), (rb, tb) in zip(placed, placed[1:]):
                if not 0 < tb - ta < LAYOUT_SPAN or rb - ra > LAYOUT_SPAN:
                    continue
                rs = [a for a in ref.order if ra < a < rb and ref.name(a) not in self.by_name
                      and self.ref_of.get(ref.name(a), (None,))[0] == ri]
                ts = [s for s in self.t.starts if ta < s < tb and s not in self.named]
                if not rs or not ts:
                    continue
                for i, j, score, twin in self._align(ref, rs, ts):
                    name = ref.name(rs[i])
                    detail = f"between {ref.name(ra)} and {ref.name(rb)}, code {score:.2f} alike"
                    if twin:
                        detail += f", the same code as {twin}, told apart by their order"
                    if self.accept(ts[j], name, "layout", detail, score=None if twin else score):
                        changed = True
                rs = [a for a in rs if ref.name(a) not in self.by_name]
                ts = [t for t in ts if t not in self.named]
                if not rs or not ts:
                    continue
                for i, j, score in self._align_shape(ref, rs, ts):
                    detail = f"between {ref.name(ra)} and {ref.name(rb)}, same shape {score:.2f}"
                    if self.accept_shape(ref, rs[i], ts[j], detail):
                        changed = True
        return changed

    def accept_shape(self, ref, ra, ta, detail):
        """The same shape is never enough for the map (checked on references: one in three is wrong). Listed as a
        candidate when a witness backs it (a call or a string the reference function has too) and nothing is against
        it (a call elsewhere, other constants)."""
        name = ref.name(ra)
        rbody = ref.bodies[ra]
        tbody = self.t.whole(ta)
        if self.calls_contradict(ref, ra, ta):
            self.ambiguous.setdefault(name, (f"{detail}, but its calls go elsewhere", [ta]))
            return False
        if constant_conflict(rbody.masked, self.t.body(ta, len(rbody.masked)).masked):
            self.ambiguous.setdefault(name, (f"{detail}, but it loads other constants", [ta]))
            return False
        wanted = {canonical(w) for w in self._callee_names(ref, rbody) if w}
        got = {canonical(self.named[c][0]) for c in tbody.calls if c in self.named}
        calls = wanted & got
        strings = rbody.strings & tbody.strings
        if not calls and not strings:
            self.ambiguous.setdefault(name, (f"{detail}, but no call or string confirms it", [ta]))
            return False
        witness = ", ".join(sorted(calls)[:3]) or repr(sorted(strings)[0][:30])
        same = constant_overlap(rbody.masked, tbody.masked)
        rpc_ref = self.rpc_numbers(ref, ra)
        rpc_target = self.rpc_numbers_target(ta)
        if rpc_ref and rpc_ref == rpc_target and same >= DRIFT_CONSTANTS:
            numbers = ",".join(str(n) for n in rpc_ref)
            return self.accept(ta, name, "rpc", f"{detail}, RPC function {numbers}, constants {same:.2f} alike")
        loads = shared_constant_loads(rbody.masked, tbody.masked)
        shape_score = similarity(rbody.shape, tbody.shape)
        if len(calls) >= DRIFT_CALLS and loads >= DRIFT_LOADS and shape_score >= DRIFT_SHAPE:
            return self.accept(ta, name, "drift", f"{detail}, {len(calls)} calls agree ({witness}), {loads} constant "
                                                  f"loads agree")
        self.ambiguous.setdefault(name, (f"candidate only: {detail}, with {witness}, constants {same:.2f} alike", [ta]))
        return False

    def rpc_numbers(self, ref, ra):
        target = {a for a in ref.bodies if canonical(ref.name(a)) == "sceSifCallRpc"}
        return rpc_arguments(ref.bodies[ra], target)

    def rpc_numbers_target(self, ta):
        target = {a for a, (n, _, _) in self.named.items() if canonical(n) == "sceSifCallRpc"}
        return rpc_arguments(self.t.whole(ta), target)

    def run_objects(self):
        """A library rebuilt between SDK releases can reorder its functions: within the span its identified functions
        occupy, the pairs that are each other's clear best by shape."""
        changed = False
        for ri, ref in enumerate(self.refs):
            libs = defaultdict(list)
            for a in ref.order:
                key = library_key(ref.name(a))
                if key:
                    libs[key].append(a)
            for key, members in libs.items():
                placed = [self.by_name[ref.name(a)] for a in members if ref.name(a) in self.by_name]
                if len(placed) < 2:
                    continue
                lo, hi = min(placed), max(placed)
                if hi - lo > OBJECT_SPAN:
                    continue
                rs = [a for a in members if ref.name(a) not in self.by_name
                      and self.ref_of.get(ref.name(a), (None,))[0] == ri]
                i = bisect.bisect_right(self.t.starts, hi)
                j = bisect.bisect_left(self.t.starts, lo)
                ts = [s for s in self.t.starts[j:i + 1] if s not in self.named]
                if not rs or not ts:
                    continue
                sims = {(r, t): similarity(ref.bodies[r].shape, self.t.whole(t).shape) for r in rs for t in ts}
                for r in rs:
                    t = max(ts, key=lambda x: sims[(r, x)])
                    s = sims[(r, t)]
                    if s < OBJECT_SIMILAR:
                        continue
                    if any(sims[(r, x)] > s - OBJECT_MARGIN for x in ts if x != t) or \
                            any(sims[(o, t)] > s - OBJECT_MARGIN for o in rs if o != r):
                        self.ambiguous.setdefault(ref.name(r), (f"in {key}'s functions, not a clear best by shape", [t]))
                        continue
                    if self.accept_shape(ref, r, t, f"among {key}'s functions, same shape {s:.2f}"):
                        changed = True
        return changed

    def calls_contradict(self, ref, ra, ta):
        want = self._callee_names(ref, ref.bodies[ra])
        got = [self.named[c][0] if c in self.named else None for c in self.t.whole(ta).calls]
        wanted = {canonical(w) for w in want if w}
        return any(g and canonical(g) not in wanted for g in got)

    def _align_shape(self, ref, rs, ts):
        sims = [[similarity(ref.bodies[r].shape, self.t.whole(t).shape) for t in ts] for r in rs]
        m, n = len(rs), len(ts)
        best = [[0.0] * (n + 1) for _ in range(m + 1)]
        for i in range(1, m + 1):
            for j in range(1, n + 1):
                pair = best[i - 1][j - 1] + sims[i - 1][j - 1] if sims[i - 1][j - 1] >= LAYOUT_PAIR else -1.0
                best[i][j] = max(best[i - 1][j], best[i][j - 1], pair)
        pairs = []
        i, j = m, n
        while i and j:
            s = sims[i - 1][j - 1]
            if s >= LAYOUT_PAIR and best[i][j] == best[i - 1][j - 1] + s:
                pairs.append((i - 1, j - 1))
                i, j = i - 1, j - 1
            elif best[i][j] == best[i - 1][j]:
                i -= 1
            else:
                j -= 1
        out = []
        for i, j in reversed(pairs):
            s = sims[i][j]
            need = SHAPE_SIMILAR if len(ref.bodies[rs[i]].shape) >= LAYOUT_SHORT else SHAPE_SIMILAR_SHORT
            if s < need:
                continue
            col = [sims[k][j] for k in range(m) if k != i]
            row = [sims[i][k] for k in range(n) if k != j]
            if (col and max(col) > s - SHAPE_MARGIN) or (row and max(row) > s - SHAPE_MARGIN):
                self.ambiguous.setdefault(ref.name(rs[i]), ("in its library's order, another function there has "
                                                            "the same shape", [ts[j]]))
                continue
            out.append((i, j, s))
        return out

    def _align(self, ref, rs, ts):
        sims = [[similarity(ref.bodies[r].masked, self.t.body(t, len(ref.bodies[r].masked)).masked) for t in ts]
                for r in rs]
        m, n = len(rs), len(ts)
        best = [[0.0] * (n + 1) for _ in range(m + 1)]
        for i in range(1, m + 1):
            for j in range(1, n + 1):
                pair = best[i - 1][j - 1] + sims[i - 1][j - 1] if sims[i - 1][j - 1] >= LAYOUT_PAIR else -1.0
                best[i][j] = max(best[i - 1][j], best[i][j - 1], pair)
        pairs = []
        i, j = m, n
        while i and j:
            s = sims[i - 1][j - 1]
            if s >= LAYOUT_PAIR and best[i][j] == best[i - 1][j - 1] + s:
                pairs.append((i - 1, j - 1))
                i, j = i - 1, j - 1
            elif best[i][j] == best[i - 1][j]:
                i -= 1
            else:
                j -= 1
        out = []
        for i, j in reversed(pairs):
            s = sims[i][j]
            need = LAYOUT_SIMILAR if len(ref.bodies[rs[i]].masked) >= LAYOUT_SHORT else LAYOUT_SIMILAR_SHORT
            if s < need:
                continue
            mine = ref.bodies[rs[i]].masked
            twins = [k for k in range(m) if k != i and ref.bodies[rs[k]].masked == mine and s == 1.0
                     and canonical(ref.name(rs[k])) != canonical(ref.name(rs[i]))]
            col = [sims[k][j] for k in range(m) if k != i and k not in twins]
            row = [sims[i][k] for k in range(n) if k != j]
            if twins:
                same = [k for k in range(n) if k != j and sims[i][k] >= s]
                if len(same) != len(twins):
                    continue
            elif (col and max(col) > s - LAYOUT_MARGIN) or (row and max(row) > s - LAYOUT_MARGIN):
                name = ref.name(rs[i])
                self.ambiguous.setdefault(name, ("in its library's order, another function there looks as alike",
                                                 [ts[j]]))
                continue
            out.append((i, j, s, ", ".join(ref.name(rs[k]) for k in twins)))
        return out

    def run_strings(self, wanted):
        users = defaultdict(set)
        for s in self.t.starts:
            for text in self.t.whole(s).strings:
                users[text].add(s)
        for name in sorted(wanted):
            if name in self.by_name or name not in self.ref_of:
                continue
            _, rbody = self._ref_body(name)
            cands = set()
            for text in rbody.strings:
                cands |= users.get(text, set())
            cands -= set(self.named)
            if not cands:
                continue
            scored = sorted(((similarity(rbody.masked, self.t.whole(c).masked), c) for c in cands), reverse=True)
            good = [(sc, c) for sc, c in scored if sc >= STRING_SIMILAR]
            if len(good) == 1:
                sc, c = good[0]
                self.accept(c, name, "string", f"same string, code {sc:.2f} alike", score=sc)
            elif good:
                self.ambiguous[name] = ("several functions use the same string", [c for _, c in good])


def library_key(name):
    m = re.match(r"(sce[A-Z][a-z0-9]+)", name)
    return m.group(1) if m else None


def load_names(paths):
    out = set()
    for p in paths:
        with open(p) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                if line:
                    out.add(line)
    return out


def engine_names(root):
    """What the dispatcher answers as a kernel call and what the native libraries implement, read from their sources."""
    names = set()
    with open(os.path.join(root, "runtime", "ee", "dispatcher.cpp")) as f:
        names.update(re.findall(r'\{"(\w+)", KernelCall::', f.read()))
    libs = os.path.join(root, "runtime", "libs")
    for file in sorted(os.listdir(libs)):
        if file.endswith(".cpp"):
            with open(os.path.join(libs, file)) as f:
                names.update(re.findall(r'\{\s*"(\w+)"\s*,\s*(?:k[A-Z]\w*|Library::\w+)\s*,', f.read()))
    return names


def load_reviewed(path):
    out = []
    with open(path) as f:
        for number, line in enumerate(f, 1):
            text, _, evidence = line.partition("#")
            words = text.split()
            if not words:
                continue
            if len(words) != 2:
                raise SystemExit(f'{path}:{number}: expected "name 0xADDR"')
            out.append((words[0], int(words[1], 16), evidence.strip()))
    return out


def canonical(name):
    n = name[4:] if name.startswith("ps2_") else name
    return n.lstrip("_")


def check(elf, mt):
    """The map against the target's own symbols (a reference matched as if it were stripped)."""
    truth = defaultdict(set)
    for name, value, _, stype, _ in elf.symbols:
        if stype == 2 and name:
            truth[value].add(canonical(name))
    right, wrong = defaultdict(int), defaultdict(list)
    for addr, (name, how, _) in sorted(mt.named.items()):
        if canonical(name) in truth.get(addr, ()):
            right[how] += 1
        else:
            wrong[how].append(f"0x{addr:06x} {name} (is {', '.join(sorted(truth.get(addr, ()))) or 'no symbol'})")
    lines = ["check against the target's symbols:"]
    for how in sorted(set(right) | set(wrong)):
        lines.append(f"  {how}: {right[how]} right, {len(wrong[how])} wrong")
        lines.extend(f"    wrong {w}" for w in wrong[how])
    return lines


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("match")
    m.add_argument("target")
    m.add_argument("--ref", action="append", required=True)
    m.add_argument("--out", required=True)
    m.add_argument("--report")
    m.add_argument("--want", action="append", default=[])
    m.add_argument("--min-words", type=int, default=4)
    m.add_argument("--reviewed", action="append", default=[],
                   help='"name 0xADDR" lines a person confirmed by reading the code ("#" comments carry the evidence)')
    m.add_argument("--check", action="store_true",
                   help="the target keeps its symbols: match without them, then score the map against them")
    e = sub.add_parser("engine-names")
    e.add_argument("engine")
    e.add_argument("out")
    a = ap.parse_args(argv)
    if a.cmd == "engine-names":
        names = engine_names(a.engine)
        with open(a.out, "w") as f:
            f.write("".join(n + "\n" for n in sorted(names)))
        print(f"{len(names)} names -> {a.out}")
        return 0

    target = Target(Elf(a.target))
    refs = [Reference(Elf(p)) for p in a.ref]
    wanted = load_names(a.want)
    mt = Matcher(target, refs, a.min_words)
    mt.run_exact()
    lib_names = {n for n in mt.ref_of if canonical(n) in {canonical(w) for w in wanted}} if wanted else set(mt.ref_of)
    while True:
        before = len(mt.named)
        mt.run_calls()
        mt.run_groups()
        mt.run_layout()
        mt.run_objects()
        mt.run_strings(lib_names)
        if len(mt.named) == before:
            break

    for path in a.reviewed:
        for name, addr, evidence in load_reviewed(path):
            if not target.elf.in_code(addr):
                raise SystemExit(f"{path}: {name} 0x{addr:x} is not in the target's code")
            taken = mt.named.get(addr)
            if taken and taken[0] != name:
                raise SystemExit(f"{path}: 0x{addr:x} is {taken[0]} by its {taken[1]} match, not {name}")
            if name in mt.by_name and mt.by_name[name] != addr:
                raise SystemExit(f"{path}: {name} is 0x{mt.by_name[name]:x} by its match, not 0x{addr:x}")
            if not taken:
                mt.named[addr] = (name, "reviewed", evidence)
                mt.by_name[name] = addr
                mt.ambiguous.pop(name, None)

    with open(a.out, "w") as f:
        f.write(f"# Symbol map of {a.target.replace(chr(92), '/').rsplit('/', 1)[-1]}: bozo_symbolmap.py match, "
                f"{len(mt.named)} functions.\n")
        for addr in sorted(mt.named):
            f.write(f"{mt.named[addr][0]} 0x{addr:x}\n")

    report = []
    hows = defaultdict(int)
    for name, how, _ in mt.named.values():
        hows[how] += 1
    report.append(f"identified: {len(mt.named)} ({', '.join(f'{k} {v}' for k, v in sorted(hows.items()))})")
    if wanted:
        canon_named = {canonical(n): (n, addr) for addr, (n, _, _) in mt.named.items()}
        canon_amb = {canonical(n): (n, v) for n, v in mt.ambiguous.items()}
        canon_ref = {canonical(n) for n in mt.ref_of}
        found = sorted(w for w in wanted if canonical(w) in canon_named)
        report.append(f"wanted: {len(wanted)}, identified {len(found)}")
        for w in found:
            n, addr = canon_named[canonical(w)]
            _, how, detail = mt.named[addr]
            report.append(f"  found      {w:40s} 0x{addr:06x} {how}: {detail}")
        for w in sorted(wanted):
            c = canonical(w)
            if c in canon_named:
                continue
            if c in canon_amb:
                reason, cands = canon_amb[c][1]
                report.append(f"  ambiguous  {w:40s} {reason}: " + " ".join(f"0x{x:06x}" for x in cands[:8]))
            elif c in canon_ref:
                why = mt.absence(next(n for n in mt.ref_of if canonical(n) == c))
                report.append(f"  not found  {w:40s} {why}")
            else:
                report.append(f"  no ref     {w:40s} in no reference executable")
    report.append(f"ambiguous (all names): {len(mt.ambiguous)}")
    for name in sorted(mt.ambiguous):
        reason, cands = mt.ambiguous[name]
        report.append(f"  {name:40s} {reason}: " + " ".join(f"0x{x:06x}" for x in cands[:8]))
    if a.check:
        report.extend(check(target.elf, mt))
    report.append("every identified function:")
    for addr in sorted(mt.named):
        name, how, detail = mt.named[addr]
        report.append(f"  0x{addr:06x} {name:40s} {how}: {detail}")
    text = "\n".join(report) + "\n"
    if a.report:
        with open(a.report, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
