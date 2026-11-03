#!/usr/bin/env python3
# capture_read.py: reads and checks a per-function capture (PS2XCAP1, docs/specs/capture.md).
#
# Copyright (c) The Bozos. MIT licence (see LICENSE at the repository root). Standard library only.
#
#   capture_read.py <capture.bin | folder>                 summary: header, totals, unreplayable reasons
#   capture_read.py <capture> functions                    per function: samples, unreplayable calls by reason
#   capture_read.py <capture> samples [--function 0xADDR]  one line per sample
#   capture_read.py <capture> show <index>                 one sample: header, key registers, its out-calls
#   capture_read.py <capture> state <index> <out.bin>      the flat memory at that sample's entry (0x2005000 bytes)
#   capture_read.py <capture> check                        validates every chunk; exit status 1 on any error
"""Reads and checks a PS2XCAP1 per-function capture (BozoEngine docs/specs/capture.md)."""

import argparse
import collections
import os
import struct
import sys
import zlib

MAGIC = b"PS2XCAP1"
HEADER_BYTES = 128
CTX_WORDS = 332
CTX_BYTES = CTX_WORDS * 4
MASK_WORDS = (CTX_WORDS + 31) // 32
SAMPLE_HEADER_BYTES = 64
OUT_HEADER_BYTES = 36
RAM_BYTES = 0x2000000
SPR_BYTES = 0x4000
VU0_BYTES = 0x1000
FLAT_BYTES = RAM_BYTES + SPR_BYTES + VU0_BYTES

OUT_KINDS = {1: "stub", 2: "override", 3: "syscall", 4: "vu0", 5: "io-load", 6: "io-store", 7: "exception"}
REASONS = ["threadSwitch", "interrupted", "unwound", "exception", "tooBig", "cop0Timer", "abandoned"]
GPR = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]


class CaptureError(Exception):
    pass


def word_name(i):
    if i < 128:
        return "%s.w%d" % (GPR[i // 4], i % 4)
    if i < 136:
        return "%s.w%d" % (["hi", "lo", "hi1", "lo1"][(i - 128) // 2], (i - 128) % 2)
    if i == 136:
        return "pc"
    if i == 137:
        return "sa"
    if i < 170:
        return "f%d" % (i - 138)
    if i == 170:
        return "fcr31"
    if i == 171:
        return "facc"
    if i < 300:
        return "vf%d.%s" % ((i - 172) // 4, "xyzw"[(i - 172) % 4])
    if i < 304:
        return "acc.%s" % "xyzw"[i - 300]
    if i < 320:
        return "vi%d" % (i - 304)
    return ["Q", "I", "P", "R", "vu0_status", "vu0_mac", "vu0_clip", "vu0_cmsar0", "cop0_status",
            "reserved", "reserved", "reserved"][i - 320]


def read_runs(buf, pos, count, end, where, errors):
    """Parses count runs from buf[pos:end]; returns (runs as (address, offset of data, length), new pos)."""
    runs = []
    for i in range(count):
        if pos + 8 > end:
            errors.append("%s: run %d past the end" % (where, i))
            return runs, end
        address, length = struct.unpack_from("<II", buf, pos)
        if length == 0 or address + length > FLAT_BYTES:
            errors.append("%s: run %d at 0x%x length %d outside the flat space" % (where, i, address, length))
        runs.append((address, pos + 8, length))
        pos += 8 + ((length + 3) & ~3)
    if pos > end:
        errors.append("%s: runs overrun their bytes" % where)
    return runs, pos


def parse_outcall(buf, pos, end, where, errors):
    if pos + OUT_HEADER_BYTES + MASK_WORDS * 4 > end:
        raise CaptureError("%s: out-call header past the end" % where)
    kind, size, flags, target = struct.unpack_from("<BBHI", buf, pos)
    value = struct.unpack_from("<4I", buf, pos + 8)
    ctx_count, run_count, _reserved = struct.unpack_from("<III", buf, pos + 24)
    mask = struct.unpack_from("<%dI" % MASK_WORDS, buf, pos + OUT_HEADER_BYTES)
    changed = [i for i in range(CTX_WORDS) if mask[i // 32] >> (i % 32) & 1]
    if len(changed) != ctx_count:
        errors.append("%s: mask has %d words, count says %d" % (where, len(changed), ctx_count))
    if any(m >> (CTX_WORDS - 32 * (MASK_WORDS - 1)) for m in mask[-1:]):
        errors.append("%s: mask bits past word %d" % (where, CTX_WORDS))
    if kind not in OUT_KINDS:
        errors.append("%s: unknown kind %d" % (where, kind))
    if kind in (5, 6) and size not in (1, 2, 4, 8, 16):
        errors.append("%s: io size %d" % (where, size))
    wpos = pos + OUT_HEADER_BYTES + MASK_WORDS * 4
    words = struct.unpack_from("<%dI" % ctx_count, buf, wpos) if ctx_count else ()
    runs, npos = read_runs(buf, wpos + 4 * ctx_count, run_count, end, where, errors)
    return {"kind": kind, "size": size, "flags": flags, "target": target, "value": value,
            "changed": dict(zip(changed, words)), "runs": runs}, npos


def parse_sample(buf, errors, deep=True):
    if len(buf) < SAMPLE_HEADER_BYTES + 2 * CTX_BYTES:
        raise CaptureError("sample shorter than its fixed parts")
    (function, index, frame, thread, call_index, slot, outcall_count, entry_run_count, entry_run_bytes,
     outcall_bytes, exit_run_count, exit_run_bytes, ra, sp, _reserved) = struct.unpack_from("<IIIiQ10I", buf, 0)
    s = {"function": function, "index": index, "frame": frame, "thread": thread, "call": call_index, "slot": slot,
         "outcall_count": outcall_count, "entry_run_count": entry_run_count, "exit_run_count": exit_run_count,
         "ra": ra, "sp": sp, "buf": buf}
    where = "sample %d (0x%06x)" % (index, function)
    expect = SAMPLE_HEADER_BYTES + 2 * CTX_BYTES + entry_run_bytes + outcall_bytes + exit_run_bytes
    if expect != len(buf):
        errors.append("%s: parts add up to %d bytes, the chunk has %d" % (where, expect, len(buf)))
        return s
    pos = SAMPLE_HEADER_BYTES
    s["entry_ctx"] = struct.unpack_from("<%dI" % CTX_WORDS, buf, pos)
    pos += CTX_BYTES
    entry_end = pos + entry_run_bytes
    s["entry_runs"], p2 = read_runs(buf, pos, entry_run_count, entry_end, where + " entry", errors)
    if p2 != entry_end:
        errors.append("%s: entry runs take %d bytes, the header says %d" % (where, p2 - pos, entry_run_bytes))
    pos = entry_end
    out_end = pos + outcall_bytes
    s["outcalls"] = []
    if deep:
        q = pos
        for i in range(outcall_count):
            oc, q = parse_outcall(buf, q, out_end, "%s out-call %d" % (where, i), errors)
            s["outcalls"].append(oc)
        if q != out_end:
            errors.append("%s: out-calls take %d bytes, the header says %d" % (where, q - pos, outcall_bytes))
    pos = out_end
    s["exit_ctx"] = struct.unpack_from("<%dI" % CTX_WORDS, buf, pos)
    pos += CTX_BYTES
    s["exit_runs"], p3 = read_runs(buf, pos, exit_run_count, pos + exit_run_bytes, where + " exit", errors)
    if p3 != pos + exit_run_bytes:
        errors.append("%s: exit runs take %d bytes, the header says %d" % (where, p3 - pos, exit_run_bytes))
    if s["entry_ctx"][136] != function:
        errors.append("%s: entry pc 0x%x is not the function" % (where, s["entry_ctx"][136]))
    if s["entry_ctx"][31 * 4] != ra:
        errors.append("%s: entry ra 0x%x, the header says 0x%x" % (where, s["entry_ctx"][124], ra))
    if s["exit_ctx"][136] != ra:
        errors.append("%s: exit pc 0x%x is not the return address 0x%x" % (where, s["exit_ctx"][136], ra))
    if s["exit_ctx"][29 * 4] != sp:
        errors.append("%s: exit sp 0x%x is not the entry sp 0x%x" % (where, s["exit_ctx"][116], sp))
    return s


class Capture:
    def __init__(self, path):
        if os.path.isdir(path):
            path = os.path.join(path, "capture.bin")
        self.path = path
        self.file = open(path, "rb")
        h = self.file.read(HEADER_BYTES)
        if len(h) != HEADER_BYTES or h[:8] != MAGIC:
            raise CaptureError("%s: not a PS2XCAP1 capture" % path)
        (self.version, self.header_bytes, self.ctx_bytes, self.flat_bytes, self.ram_bytes, self.spr_bytes,
         self.vu0_bytes, self.samples_per_function, self.frame_from, self.frame_to) = struct.unpack_from("<10I", h, 8)
        (self.elf_hash,) = struct.unpack_from("<Q", h, 48)
        (self.page_bytes,) = struct.unpack_from("<I", h, 56)
        self.run = h[64:128].split(b"\0", 1)[0].decode("utf-8", "replace")
        if self.version != 1 or self.ctx_bytes != CTX_BYTES or self.flat_bytes != FLAT_BYTES:
            raise CaptureError("%s: version %d, context %d bytes, flat %d bytes: not this reader's layout"
                               % (path, self.version, self.ctx_bytes, self.flat_bytes))
        self.file.seek(self.header_bytes)

    def chunks(self):
        """Yields (tag, payload, file offset); a truncated last chunk ends the iteration (tag None)."""
        self.file.seek(self.header_bytes)
        while True:
            offset = self.file.tell()
            head = self.file.read(16)
            if not head:
                return
            if len(head) < 16:
                yield None, b"", offset
                return
            tag, flags, stored, raw = struct.unpack("<4sIII", head)
            data = self.file.read(stored)
            if len(data) < stored:
                yield None, b"", offset
                return
            if flags & 1:
                try:
                    data = zlib.decompress(data)
                except zlib.error as e:
                    raise CaptureError("chunk at %d: %s" % (offset, e))
            if len(data) != raw:
                raise CaptureError("chunk at %d: %d bytes, the header says %d" % (offset, len(data), raw))
            yield tag.decode("ascii", "replace"), data, offset


def walk(cap, deep=False):
    """Totals, per-function counts, every sample's header, and the errors found."""
    errors = []
    funcs = collections.defaultdict(lambda: {"samples": 0, "skips": 0, "reasons": [0] * len(REASONS)})
    samples = []
    totals = collections.Counter()
    end = None
    overrides = []
    expected_index = 0
    for tag, data, offset in cap.chunks():
        if tag is None:
            errors.append("the file ends inside a chunk at %d (the run was cut short)" % offset)
            break
        totals[tag] += 1
        if tag == "SAMP":
            s = parse_sample(data, errors, deep)
            if s["index"] != expected_index:
                errors.append("sample at %d has index %d, expected %d" % (offset, s["index"], expected_index))
            expected_index += 1
            funcs[s["function"]]["samples"] += 1
            s.pop("buf", None)
            samples.append({k: s[k] for k in ("function", "index", "frame", "thread", "call", "slot",
                                              "outcall_count", "entry_run_count", "exit_run_count", "ra", "sp")})
        elif tag == "SKIP":
            function, frame, reasons, thread, call, outcalls, _r = struct.unpack_from("<IIIiQII", data, 0)
            f = funcs[function]
            f["skips"] += 1
            for i in range(len(REASONS)):
                if reasons >> i & 1:
                    f["reasons"][i] += 1
                    totals["reason:" + REASONS[i]] += 1
            if reasons == 0 or reasons >> len(REASONS):
                errors.append("SKIP at %d: reasons 0x%x" % (offset, reasons))
        elif tag == "OVRD":
            (n,) = struct.unpack_from("<I", data, 0)
            overrides = list(struct.unpack_from("<%dI" % n, data, 4))
        elif tag == "END ":
            end = struct.unpack_from("<%dI" % (len(data) // 4), data, 0)
        else:
            errors.append("unknown chunk %r at %d" % (tag, offset))
    if end is None:
        errors.append("no END chunk (the run was cut short)")
    elif end[0] != totals["SAMP"] or end[1] != totals["SKIP"]:
        errors.append("END says %d samples and %d skips, the file has %d and %d"
                      % (end[0], end[1], totals["SAMP"], totals["SKIP"]))
    return totals, funcs, samples, errors, end, overrides


def cmd_summary(cap, args):
    totals, funcs, samples, errors, end, overrides = walk(cap)
    size = os.path.getsize(cap.path)
    print("%s: run %r, %.1f MB, elf XXH64 %016x" % (cap.path, cap.run, size / 1048576.0, cap.elf_hash))
    print("  %d samples a function over frames %d-%d" % (cap.samples_per_function, cap.frame_from, cap.frame_to))
    covered = sum(1 for f in funcs.values() if f["samples"])
    called = (" of %d called" % end[4]) if end and len(end) > 4 else ""
    print("  %d samples of %d functions%s; %d calls not replayable (%d functions tried, %d with no sample)"
          % (totals["SAMP"], covered, called, totals["SKIP"], len(funcs), len(funcs) - covered))
    for r in REASONS:
        if totals["reason:" + r]:
            print("    %-12s %d" % (r, totals["reason:" + r]))
    print("  %d port overrides listed" % len(overrides))
    for e in errors[:20]:
        print("  error: " + e)
    return 1 if errors else 0


def cmd_functions(cap, args):
    totals, funcs, samples, errors, end, overrides = walk(cap)
    print("function  samples  skips  " + "  ".join(REASONS))
    for address in sorted(funcs):
        f = funcs[address]
        print("0x%06x  %7d  %5d  %s" % (address, f["samples"], f["skips"],
                                        "  ".join("%*d" % (len(r), n) for r, n in zip(REASONS, f["reasons"]))))
    return 0


def cmd_samples(cap, args):
    totals, funcs, samples, errors, end, overrides = walk(cap)
    want = int(args.function, 0) if args.function else None
    print("index  function  frame  thread  call  slot  outcalls  entryRuns  exitRuns  ra")
    for s in samples:
        if want is not None and s["function"] != want:
            continue
        print("%5d  0x%06x  %5d  %6d  %4d  %4d  %8d  %9d  %8d  0x%x" % (
            s["index"], s["function"], s["frame"], s["thread"], s["call"], s["slot"], s["outcall_count"],
            s["entry_run_count"], s["exit_run_count"], s["ra"]))
    return 0


def find_sample(cap, index):
    errors = []
    for tag, data, offset in cap.chunks():
        if tag == "SAMP" and struct.unpack_from("<I", data, 4)[0] == index:
            return parse_sample(data, errors, True), errors
    raise CaptureError("no sample %d" % index)


def cmd_show(cap, args):
    s, errors = find_sample(cap, args.index)
    print("sample %d: function 0x%06x, frame %d, thread %d, call %d, slot %d, ra 0x%x, sp 0x%x"
          % (s["index"], s["function"], s["frame"], s["thread"], s["call"], s["slot"], s["ra"], s["sp"]))
    e, x = s["entry_ctx"], s["exit_ctx"]
    for name, reg in (("a0", 4), ("a1", 5), ("a2", 6), ("a3", 7), ("v0", 2), ("v1", 3), ("sp", 29), ("ra", 31)):
        print("  %-3s entry %08x  exit %08x" % (name, e[reg * 4], x[reg * 4]))
    print("  context words changed by the call: %d" % sum(1 for i in range(CTX_WORDS) if e[i] != x[i]))
    print("  memory: %d entry runs (%d bytes), %d exit runs (%d bytes)" % (
        len(s["entry_runs"]), sum(r[2] for r in s["entry_runs"]), len(s["exit_runs"]),
        sum(r[2] for r in s["exit_runs"])))
    print("  out-calls: %d" % len(s["outcalls"]))
    for i, oc in enumerate(s["outcalls"]):
        changed = ", ".join("%s=%08x" % (word_name(k), v) for k, v in list(oc["changed"].items())[:6])
        print("    %3d %-9s 0x%08x size %2d value %s; %d context words%s; %d memory runs (%d bytes)" % (
            i, OUT_KINDS.get(oc["kind"], "?"), oc["target"], oc["size"], " ".join("%08x" % v for v in oc["value"]),
            len(oc["changed"]), (" (" + changed + ")") if changed else "", len(oc["runs"]),
            sum(r[2] for r in oc["runs"])))
    for e in errors:
        print("  error: " + e)
    return 1 if errors else 0


def cmd_state(cap, args):
    flat = bytearray(FLAT_BYTES)
    errors = []
    for tag, data, offset in cap.chunks():
        if tag != "SAMP":
            continue
        s = parse_sample(data, errors, False)
        buf = data
        for address, pos, length in s["entry_runs"]:
            flat[address:address + length] = buf[pos:pos + length]
        if s["index"] == args.index:
            with open(args.out, "wb") as f:
                f.write(flat)
            print("flat memory at the entry of sample %d (0x%06x) written to %s" % (args.index, s["function"], args.out))
            return 0
    raise CaptureError("no sample %d" % args.index)


def cmd_check(cap, args):
    totals, funcs, samples, errors, end, overrides = walk(cap, deep=True)
    print("%s: %d chunks, %d samples, %d unreplayable calls, %d errors" % (
        cap.path, sum(v for k, v in totals.items() if not k.startswith("reason:")), totals["SAMP"],
        totals["SKIP"], len(errors)))
    for e in errors[:50]:
        print("  error: " + e)
    return 1 if errors else 0


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("capture", help="capture.bin, or the folder holding it")
    sub = p.add_subparsers(dest="command")
    sub.add_parser("summary")
    sub.add_parser("functions")
    sp = sub.add_parser("samples")
    sp.add_argument("--function")
    sh = sub.add_parser("show")
    sh.add_argument("index", type=int)
    st = sub.add_parser("state")
    st.add_argument("index", type=int)
    st.add_argument("out")
    sub.add_parser("check")
    args = p.parse_args(argv)
    try:
        cap = Capture(args.capture)
        return {None: cmd_summary, "summary": cmd_summary, "functions": cmd_functions, "samples": cmd_samples,
                "show": cmd_show, "state": cmd_state, "check": cmd_check}[args.command](cap, args)
    except CaptureError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
