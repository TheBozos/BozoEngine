#!/usr/bin/env python3
# calllog_read.py: reads and checks a call log (PS2XCLG1, docs/specs/calllog.md).
#
# Copyright (c) The Bozos. MIT licence (see LICENSE at the repository root). Standard library only.
#
#   calllog_read.py <log.clg>                                  summary: header, record counts, MEM by source, top calls
#   calllog_read.py <log> calls                                per call target: count, class, name, memory written
#   calllog_read.py <log> list [--frames A-B] [--types T,..] [--limit N]
#                                                              one line per record
#   calllog_read.py <log> show <index>                         one record in full (index: its place in the file, from 0)
#   calllog_read.py <log> check                                validates every chunk and record and the invariants of
#                                                              calllog.md section 8; exit status 1 on any error
#   calllog_read.py <log> compare <other.clg>                  the first record where two logs differ
"""Reads and checks a PS2XCLG1 call log (BozoEngine docs/specs/calllog.md)."""

import argparse
import collections
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from capture_read import (CTX_WORDS, MASK_WORDS, OUT_HEADER_BYTES, FLAT_BYTES, OUT_KINDS, CaptureError,  # noqa: E402
                          parse_outcall, read_runs, word_name)

MAGIC = b"PS2XCLG1"
HEADER_BYTES = 128
RECORD_HEADER_BYTES = 40

TYPES = {1: "CALL", 2: "MEM", 3: "CTX", 4: "INVOKE", 5: "INVOKE_END", 6: "SWITCH", 7: "FRAME", 8: "OP", 9: "INIT"}
SOURCES = {0: "guest-checkpoint", 1: "call", 2: "iop", 3: "iop-vblank", 4: "events", 5: "resume", 6: "handler-end", 7: "handler-start",
           8: "thread-end", 9: "dispatcher", 10: "task"}
OPS = {1: "queue", 2: "block", 3: "wake", 4: "irq", 5: "task", 6: "event", 7: "create", 8: "start", 9: "exit",
       10: "terminate", 11: "priority", 12: "rotate", 13: "suspend", 14: "resume", 15: "wakeup", 16: "cancel-wakeup",
       17: "release-wait", 18: "sema-create", 19: "sema-delete", 20: "signal", 21: "poll-sema", 22: "wait-sema",
       23: "flag-create", 24: "flag-delete", 25: "flag-set", 26: "flag-clear", 27: "flag-poll", 28: "flag-wait",
       29: "alarm-set", 30: "alarm-cancel", 31: "handler-add", 32: "handler-remove", 33: "handler-enable",
       34: "cause-enable", 35: "vsync-flag", 36: "gs-vsync-callback", 37: "field-rate", 38: "thread-setup",
       39: "transfer"}
INVOCATIONS = {1: "interrupt", 2: "alarm", 3: "gs-callback", 4: "rpc-callback", 5: "syscall-override",
               6: "exit-handler", 7: "hle-call", 8: "sif-command"}
SWITCH_REASONS = {1: "blocked", 2: "preempted", 3: "ended", 4: "suspended", 5: "deleted", 6: "start",
                  7: "handler-start", 8: "handler-end", 9: "other"}
WAIT_REASONS = {0: "none", 1: "sleep", 2: "semaphore", 3: "event-flag", 4: "vsync", 5: "external", 6: "mpeg"}
CLASSES = {1: "kernel", 2: "library", 3: "override"}
CALL_FLAGS = {1: "unwound", 2: "frame-inside", 4: "cut", 8: "context-lost", 16: "transfer"}
INSIDE_CALL = 0x80


class LogError(Exception):
    pass


class Record:
    __slots__ = ("index", "type", "flags", "sub", "size", "cycle", "seq", "frame", "thread", "checkpoints", "pc",
                 "buf", "pos")

    def __init__(self, index, buf, pos):
        self.index = index
        self.buf = buf
        self.pos = pos
        (self.type, self.flags, self.sub, self.size, self.cycle, self.seq, self.frame, self.thread,
         self.checkpoints, self.pc) = struct.unpack_from("<BBHIQQIiII", buf, pos)

    def body(self, offset, fmt):
        return struct.unpack_from("<" + fmt, self.buf, self.pos + offset)

    def raw(self):
        return bytes(self.buf[self.pos:self.pos + self.size])


class CallLog:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        if len(self.data) < HEADER_BYTES or self.data[:8] != MAGIC:
            raise LogError("%s: not a PS2XCLG1 call log" % path)
        (self.version, self.header_bytes, self.context_bytes, self.flat_bytes, self.ram_bytes, self.spr_bytes,
         self.vu0_bytes, self.page_bytes, self.frame_limit, self.flags) = struct.unpack_from("<10I", self.data, 8)
        self.elf_hash, self.ee_clock, self.record_header = struct.unpack_from("<QII", self.data, 48)
        self.run = self.data[64:128].split(b"\0")[0].decode("utf-8", "replace")
        if self.context_bytes != CTX_WORDS * 4 or self.flat_bytes != FLAT_BYTES:
            raise LogError("%s: context or flat space of another layout" % path)
        self.names = {}
        self.classes = {}
        self.overrides = []
        self.end = None
        self.chunk_counts = collections.Counter()
        self.chunk_errors = []

    def chunks(self):
        pos = self.header_bytes
        while pos + 16 <= len(self.data):
            tag, flags, stored, raw = struct.unpack_from("<4sIII", self.data, pos)
            payload = self.data[pos + 16:pos + 16 + stored]
            if len(payload) != stored:
                self.chunk_errors.append("chunk at %d cut short" % pos)
                return
            if flags & 1:
                try:
                    payload = zlib.decompress(payload)
                except zlib.error as e:
                    self.chunk_errors.append("chunk at %d: %s" % (pos, e))
                    return
            if len(payload) != raw:
                self.chunk_errors.append("chunk at %d: %d bytes once decompressed, header says %d" % (
                    pos, len(payload), raw))
            tag = tag.decode("latin-1")
            self.chunk_counts[tag] += 1
            yield tag, payload
            pos += 16 + stored
        if pos != len(self.data):
            self.chunk_errors.append("%d bytes after the last chunk" % (len(self.data) - pos))

    def records(self):
        """Every record in file order (NAME, OVRD and END chunks are read on the way)."""
        index = 0
        for tag, payload in self.chunks():
            if tag == "NAME":
                (count,) = struct.unpack_from("<I", payload, 0)
                p = 4
                for _ in range(count):
                    address, kind, length = struct.unpack_from("<III", payload, p)
                    name = payload[p + 12:p + 12 + length].decode("utf-8", "replace")
                    self.names[address] = name
                    self.classes[address] = kind
                    p += 12 + ((length + 3) & ~3)
            elif tag == "OVRD":
                (count,) = struct.unpack_from("<I", payload, 0)
                self.overrides = list(struct.unpack_from("<%dI" % count, payload, 4))
            elif tag == "END ":
                self.end = struct.unpack_from("<QQIIQ", payload, 0) if len(payload) >= 32 else None
            elif tag == "RECS":
                pos = 0
                while pos < len(payload):
                    if pos + RECORD_HEADER_BYTES > len(payload):
                        raise LogError("record %d: header past its chunk" % index)
                    r = Record(index, payload, pos)
                    if r.size < RECORD_HEADER_BYTES or r.size % 4 or pos + r.size > len(payload):
                        raise LogError("record %d: size %d" % (index, r.size))
                    yield r
                    index += 1
                    pos += r.size

    def call_name(self, kind, target):
        if kind in (1, 2):
            name = self.names.get(target & 0x1FFFFFF)
            if name:
                return name
            if kind == 2 or target in self.overrides:
                return "override 0x%x" % target
        if kind == 3:
            return "syscall"
        return ""


def parse_call(r, errors):
    where = "record %d (CALL)" % r.index
    ra, sp, t0, t1, t2, t3, f12, f13, after, spin_before, spin_after, depth, _ = r.body(40, "8IQIIII")
    out, end = parse_outcall(r.buf, r.pos + 96, r.pos + r.size, where, errors)
    out.update(ra=ra, sp=sp, t=(t0, t1, t2, t3), f12=f12, f13=f13, cycle_after=after, spin_before=spin_before,
               spin_after=spin_after, depth=depth)
    traces, end = read_traces(r.buf, end, r.pos + r.size, where, errors)
    out["traces"] = traces
    if end != r.pos + r.size:
        errors.append("%s: %d bytes after its traced calls" % (where, r.pos + r.size - end))
    return out


def read_traces(buf, pos, end, where, errors):
    """The traced calls (PS2XREF1's calls field) a record carries: count, then 32-byte entries."""
    if pos + 4 > end:
        errors.append("%s: no traced-call count" % where)
        return [], end
    (count,) = struct.unpack_from("<I", buf, pos)
    pos += 4
    if pos + 32 * count > end:
        errors.append("%s: %d traced calls past the end" % (where, count))
        return [], end
    traces = [struct.unpack_from("<8I", buf, pos + 32 * i) for i in range(count)]
    return traces, pos + 32 * count


def parse_frame(r, errors):
    ended, run_count, trace_count, _ = r.body(40, "4I")
    where = "record %d (FRAME)" % r.index
    runs, pos = read_runs(r.buf, r.pos + 56, run_count, r.pos + r.size, where, errors)
    if pos + 32 * trace_count != r.pos + r.size:
        errors.append("%s: size %d" % (where, r.size))
    traces = [struct.unpack_from("<8I", r.buf, pos + 32 * i) for i in range(trace_count)]
    return {"frame": ended, "runs": runs, "traces": traces}


def parse_mem(r, errors):
    count, _, origin = r.body(40, "IIQ")
    runs, end = read_runs(r.buf, r.pos + 56, count, r.pos + r.size, "record %d (MEM)" % r.index, errors)
    return {"runs": runs, "origin": origin}


def parse_init(r, errors):
    (count,) = r.body(40, "I")
    runs, end = read_runs(r.buf, r.pos + 48, count, r.pos + r.size, "record %d (INIT)" % r.index, errors)
    return {"runs": runs}


def parse_ctx(r, errors):
    thread, depth, count, _ = r.body(40, "iIII")
    mask = r.body(56, "%dI" % MASK_WORDS)
    changed = [i for i in range(CTX_WORDS) if mask[i // 32] >> (i % 32) & 1]
    if len(changed) != count:
        errors.append("record %d (CTX): mask has %d words, count says %d" % (r.index, len(changed), count))
    words = r.body(56 + 4 * MASK_WORDS, "%dI" % count) if count else ()
    if 56 + 4 * MASK_WORDS + 4 * count != r.size:
        errors.append("record %d (CTX): size %d" % (r.index, r.size))
    return {"thread": thread, "depth": depth, "changed": dict(zip(changed, words))}


def parse_invoke(r):
    thread, depth, pc, flags, tag, sequence, a0, a1, a2, a3, gp, sp, ra, _ = r.body(40, "iIIIQQ8I")
    return {"thread": thread, "depth": depth, "pc": pc, "onComplete": flags & 1, "tag": tag, "sequence": sequence,
            "a": (a0, a1, a2, a3), "gp": gp, "sp": sp, "ra": ra}


def parse_invoke_end(r):
    thread, depth, v0, v1, sequence = r.body(40, "iIIIQ")
    return {"thread": thread, "depth": depth, "v0": v0, "v1": v1, "sequence": sequence}


def parse_switch(r):
    frm, frm_depth, to, to_depth, wait_reason, wait_object, token = r.body(40, "iIiIIIQ")
    return {"from": frm, "fromDepth": frm_depth, "to": to, "toDepth": to_depth, "waitReason": wait_reason,
            "waitObject": wait_object, "token": token}


def parse_op(r):
    origin, count, depth, _, origin_seq, wide = r.body(40, "IIIIQQ")
    args = r.body(72, "%dI" % count) if count else ()
    return {"origin": origin, "depth": depth, "originSeq": origin_seq, "wide": wide, "args": args}


def describe(log, r, errors=None):
    errors = [] if errors is None else errors
    head = "#%-8d f%-5d c%-12d s%-8d t%-3d%s" % (r.index, r.frame, r.cycle, r.seq, r.thread,
                                                "^" if r.type != 1 and r.flags & INSIDE_CALL else " ")
    t = TYPES.get(r.type, "?%d" % r.type)
    if r.type == 1:
        c = parse_call(r, errors)
        flags = ",".join(n for b, n in CALL_FLAGS.items() if r.flags & b)
        name = log.call_name(c["kind"], c["target"])
        extra = ""
        if c["kind"] in (1, 2):
            extra = " a0-a3 %s" % " ".join("%x" % v for v in c["value"])
        elif c["kind"] == 3:
            extra = " v1 %d" % c["value"][0]
        elif c["kind"] in (5, 6):
            extra = " size %d value %x" % (c["size"], c["value"][0])
        return "%s CALL %-8s 0x%08x %-28s ra %x%s; %d words, %d runs, %d traced, +%d cycles%s" % (
            head, OUT_KINDS.get(c["kind"], "?"), c["target"], name, c["ra"], extra, len(c["changed"]), len(c["runs"]),
            len(c["traces"]), c["cycle_after"] - r.cycle, (" [" + flags + "]") if flags else "")
    if r.type == 2:
        m = parse_mem(r, errors)
        return "%s MEM %-12s %d runs, %d bytes%s" % (head, SOURCES.get(r.sub, r.sub), len(m["runs"]),
                                                    sum(x[2] for x in m["runs"]),
                                                    "" if m["origin"] == 2 ** 64 - 1 else
                                                    " (made by call %d)" % m["origin"])
    if r.type == 3:
        c = parse_ctx(r, errors)
        shown = " ".join("%s=%x" % (word_name(i), v) for i, v in list(c["changed"].items())[:6])
        return "%s CTX thread %d depth %d: %d words %s%s" % (head, c["thread"], c["depth"], len(c["changed"]), shown,
                                                             " ..." if len(c["changed"]) > 6 else "")
    if r.type == 4:
        i = parse_invoke(r)
        return "%s INVOKE %s on thread %d depth %d: pc %x a0-a3 %s gp %x sp %x%s" % (
            head, INVOCATIONS.get(r.sub, r.sub), i["thread"], i["depth"], i["pc"], " ".join("%x" % v for v in i["a"]),
            i["gp"], i["sp"], " (completion)" if i["onComplete"] else "")
    if r.type == 5:
        i = parse_invoke_end(r)
        return "%s INVOKE_END %s thread %d depth %d v0 %x" % (head, INVOCATIONS.get(r.sub, r.sub), i["thread"],
                                                              i["depth"], i["v0"])
    if r.type == 6:
        s = parse_switch(r)
        wait = ""
        if r.sub == 1:
            wait = " (%s %d %x)" % (WAIT_REASONS.get(s["waitReason"], s["waitReason"]), s["waitObject"], s["token"])
        return "%s SWITCH %d/%d -> %d/%d %s%s" % (head, s["from"], s["fromDepth"], s["to"], s["toDepth"],
                                                 SWITCH_REASONS.get(r.sub, r.sub), wait)
    if r.type == 7:
        f = parse_frame(r, errors)
        extra = ""
        if f["runs"] or f["traces"]:
            extra = " (inside the call: %d runs, %d traced calls by then)" % (len(f["runs"]), len(f["traces"]))
        return "%s FRAME %d ended%s" % (head, f["frame"], extra)
    if r.type == 8:
        o = parse_op(r)
        origin = SOURCES.get(o["origin"], o["origin"])
        if o["origin"] == 1:
            origin = "call %d" % o["originSeq"]
        return "%s OP %s%-9s by %-12s %s wide %x" % (head, "  " * o["depth"], OPS.get(r.sub, r.sub), origin,
                                                     " ".join("%x" % v for v in o["args"]), o["wide"])
    if r.type == 9:
        m = parse_init(r, errors)
        return "%s INIT %d runs, %d bytes" % (head, len(m["runs"]), sum(x[2] for x in m["runs"]))
    return "%s %s" % (head, t)


def frame_range(text):
    if not text:
        return 0, 2 ** 32
    a, _, b = text.partition("-")
    return int(a, 0), (int(b, 0) if b else int(a, 0) + 1)


def cmd_summary(log, args):
    types = collections.Counter()
    mem = collections.Counter()
    mem_bytes = collections.Counter()
    calls = collections.Counter()
    call_bytes = 0
    unwound = 0
    last = None
    errors = []
    for r in log.records():
        types[r.type] += 1
        if r.type == 1:
            c = parse_call(r, errors)
            calls[(c["kind"], c["target"])] += 1
            call_bytes += sum(x[2] for x in c["runs"])
            unwound += bool(r.flags & 1)
        elif r.type == 2:
            m = parse_mem(r, errors)
            mem[r.sub] += 1
            mem_bytes[r.sub] += sum(x[2] for x in m["runs"])
        last = r
    print("%s: run %s, version %d, frame limit %d%s, ELF XXH64 %016x, %d bytes" % (
        log.path, log.run, log.version, log.frame_limit, " (strict)" if log.flags & 1 else "", log.elf_hash,
        len(log.data)))
    print("chunks: " + ", ".join("%s %d" % kv for kv in sorted(log.chunk_counts.items())))
    if log.end:
        print("end: %d records, %d calls, frame %d, cycle %d" % (log.end[0], log.end[1], log.end[2], log.end[4]))
    else:
        print("no END chunk: the log was cut short")
    print("records: " + ", ".join("%s %d" % (TYPES.get(k, k), v) for k, v in sorted(types.items())))
    print("calls: %d (%d left through the dispatcher), %d bytes of memory written" % (types[1], unwound, call_bytes))
    for k in sorted(mem):
        print("  MEM %-13s %8d records %12d bytes" % (SOURCES.get(k, k), mem[k], mem_bytes[k]))
    print("top calls:")
    for (kind, target), n in calls.most_common(args.top if hasattr(args, "top") else 25):
        print("  %9d %-8s 0x%08x %s" % (n, OUT_KINDS.get(kind, kind), target, log.call_name(kind, target)))
    if last is not None:
        print("last record: frame %d, cycle %d" % (last.frame, last.cycle))
    for e in (log.chunk_errors + errors)[:20]:
        print("  error: " + e)
    return 1 if log.chunk_errors or errors else 0


def cmd_calls(log, args):
    stats = {}
    errors = []
    for r in log.records():
        if r.type != 1:
            continue
        c = parse_call(r, errors)
        key = (c["kind"], c["target"])
        s = stats.setdefault(key, [0, 0, 0, 0])
        s[0] += 1
        s[1] += sum(x[2] for x in c["runs"])
        s[2] += bool(r.flags & 1)
        s[3] += c["cycle_after"] - r.cycle
    print("%9s %-8s %-10s %-8s %12s %8s %12s  %s" % ("calls", "kind", "target", "class", "bytes", "unwound",
                                                     "cycles", "name"))
    for (kind, target), (n, nbytes, unwound, cycles) in sorted(stats.items(), key=lambda kv: -kv[1][0]):
        cls = CLASSES.get(log.classes.get(target & 0x1FFFFFF), "")
        if kind == 2 or target in log.overrides:
            cls = "override"
        if kind == 3:
            cls = "kernel"
        print("%9d %-8s 0x%08x %-8s %12d %8d %12d  %s" % (n, OUT_KINDS.get(kind, kind), target, cls, nbytes, unwound,
                                                         cycles, log.call_name(kind, target)))
    return 0


def cmd_list(log, args):
    lo, hi = frame_range(args.frames)
    wanted = None
    if args.types:
        names = {v: k for k, v in TYPES.items()}
        wanted = {names[t.strip().upper()] for t in args.types.split(",")}
    shown = 0
    for r in log.records():
        if r.frame < lo:
            continue
        if r.frame >= hi:
            break
        if wanted and r.type not in wanted:
            continue
        print(describe(log, r))
        shown += 1
        if args.limit and shown >= args.limit:
            break
    return 0


def cmd_show(log, args):
    for r in log.records():
        if r.index != args.index:
            continue
        errors = []
        print(describe(log, r, errors))
        print("  header: type %d flags %x sub %d size %d cycle %d seq %d frame %d thread %d checkpoints %d pc %x" % (
            r.type, r.flags, r.sub, r.size, r.cycle, r.seq, r.frame, r.thread, r.checkpoints, r.pc))
        if r.type == 1:
            c = parse_call(r, errors)
            print("  value %s ra %x sp %x t0-t3 %s f12 %x f13 %x depth %d cycles %d..%d spin %d..%d" % (
                " ".join("%x" % v for v in c["value"]), c["ra"], c["sp"], " ".join("%x" % v for v in c["t"]),
                c["f12"], c["f13"], c["depth"], r.cycle, c["cycle_after"], c["spin_before"], c["spin_after"]))
            for i, v in c["changed"].items():
                print("  %-10s = %08x" % (word_name(i), v))
            for address, off, length in c["runs"][:40]:
                print("  run 0x%07x %d bytes: %s%s" % (address, length, r.buf[off:off + min(length, 32)].hex(),
                                                      "..." if length > 32 else ""))
        elif r.type in (2, 9):
            m = parse_mem(r, errors) if r.type == 2 else parse_init(r, errors)
            for address, off, length in m["runs"][:60]:
                print("  run 0x%07x %d bytes: %s%s" % (address, length, r.buf[off:off + min(length, 32)].hex(),
                                                      "..." if length > 32 else ""))
        elif r.type == 3:
            for i, v in parse_ctx(r, errors)["changed"].items():
                print("  %-10s = %08x" % (word_name(i), v))
        for e in errors:
            print("  error: " + e)
        return 1 if errors else 0
    print("no record %d" % args.index, file=sys.stderr)
    return 1


def cmd_check(log, args):
    """calllog.md section 8: what a valid log satisfies."""
    errors = []
    counts = collections.Counter()
    calls = 0
    last_cycle = 0
    last_frame = 0
    frames_seen = 0
    first = True
    current = None  # (thread, depth) the dispatcher last switched to
    last_call_open = False  # the records since the last CALL were all made inside it
    depth = collections.defaultdict(int)
    for r in log.records():
        counts[r.type] += 1
        where = "record %d (%s)" % (r.index, TYPES.get(r.type, r.type))
        if r.type not in TYPES:
            errors.append("%s: unknown type" % where)
            continue
        if first and r.type != 9:
            errors.append("%s: the first record is not INIT" % where)
        first = False
        if r.cycle < last_cycle:
            errors.append("%s: cycle %d before the previous record's %d" % (where, r.cycle, last_cycle))
        last_cycle = r.cycle
        if r.frame < last_frame:
            errors.append("%s: frame %d before %d" % (where, r.frame, last_frame))
        last_frame = r.frame
        if r.type == 1:
            if r.seq != calls:
                errors.append("%s: seq %d, %d calls before it" % (where, r.seq, calls))
            c = parse_call(r, errors)
            if c["cycle_after"] < r.cycle:
                errors.append("%s: ends before it starts" % where)
            calls += 1
        elif r.flags & INSIDE_CALL:
            if r.seq != calls - 1 or not last_call_open:
                errors.append("%s: made inside call %d, which is not the CALL before it" % (where, r.seq))
        elif r.seq != calls:
            errors.append("%s: seq %d, %d calls before it" % (where, r.seq, calls))
        if r.type == 1:
            last_call_open = True
        elif not r.flags & INSIDE_CALL:
            last_call_open = False
        if r.type == 2:
            parse_mem(r, errors)
            if r.sub not in SOURCES or r.sub in (0, 1, 10):
                errors.append("%s: source %d" % (where, r.sub))
        elif r.type == 9:
            parse_init(r, errors)
        elif r.type == 3:
            parse_ctx(r, errors)
        elif r.type == 7:
            ended = parse_frame(r, errors)["frame"]
            if ended != frames_seen:
                errors.append("%s: frame %d ended, expected %d" % (where, ended, frames_seen))
            frames_seen += 1
        elif r.type == 6:
            s = parse_switch(r)
            if current is not None and (s["from"], s["fromDepth"]) != current and r.sub != 6:
                errors.append("%s: from %d/%d, the last switch went to %d/%d" % (
                    where, s["from"], s["fromDepth"], current[0], current[1]))
            current = (s["to"], s["toDepth"])
        elif r.type == 4:
            i = parse_invoke(r)
            if i["depth"] != depth[i["thread"]] + 1:
                errors.append("%s: depth %d on thread %d at depth %d" % (where, i["depth"], i["thread"],
                                                                        depth[i["thread"]]))
            depth[i["thread"]] = i["depth"]
        elif r.type == 5:
            i = parse_invoke_end(r)
            if i["depth"] != depth[i["thread"]] or i["depth"] == 0:
                errors.append("%s: depth %d on thread %d at depth %d" % (where, i["depth"], i["thread"],
                                                                        depth[i["thread"]]))
            depth[i["thread"]] = i["depth"] - 1
        elif r.type == 8:
            o = parse_op(r)
            if r.sub not in OPS:
                errors.append("%s: op %d" % (where, r.sub))
            if o["origin"] not in SOURCES:
                errors.append("%s: origin %d" % (where, o["origin"]))
        if len(errors) > 200:
            break
    if log.end is None:
        errors.append("no END chunk")
    elif log.end[0] != sum(counts.values()) or log.end[1] != calls:
        errors.append("END says %d records, %d calls; the file has %d, %d" % (log.end[0], log.end[1],
                                                                            sum(counts.values()), calls))
    errors = log.chunk_errors + errors
    print("%s: %d records (%s), %d calls, %d frames, %d errors" % (
        log.path, sum(counts.values()), ", ".join("%s %d" % (TYPES[k], v) for k, v in sorted(counts.items())), calls,
        frames_seen, len(errors)))
    for e in errors[:50]:
        print("  error: " + e)
    return 1 if errors else 0


def cmd_compare(log, args):
    other = CallLog(args.other)
    a_iter = log.records()
    b_iter = other.records()
    n = 0
    while True:
        a = next(a_iter, None)
        b = next(b_iter, None)
        if a is None and b is None:
            print("identical: %d records" % n)
            return 0
        if a is None or b is None or a.raw() != b.raw():
            print("first difference at record %d:" % n)
            print("  %s: %s" % (log.path, describe(log, a) if a else "(end)"))
            print("  %s: %s" % (other.path, describe(other, b) if b else "(end)"))
            return 1
        n += 1


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("log", help="the call log (.clg)")
    sub = p.add_subparsers(dest="command")
    sm = sub.add_parser("summary")
    sm.add_argument("--top", type=int, default=25)
    sub.add_parser("calls")
    ls = sub.add_parser("list")
    ls.add_argument("--frames")
    ls.add_argument("--types")
    ls.add_argument("--limit", type=int, default=0)
    sh = sub.add_parser("show")
    sh.add_argument("index", type=int)
    sub.add_parser("check")
    cp = sub.add_parser("compare")
    cp.add_argument("other")
    args = p.parse_args(argv)
    try:
        log = CallLog(args.log)
        if args.command is None:
            args.top = 25
        return {None: cmd_summary, "summary": cmd_summary, "calls": cmd_calls, "list": cmd_list, "show": cmd_show,
                "check": cmd_check, "compare": cmd_compare}[args.command](log, args)
    except (LogError, CaptureError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    except BrokenPipeError:
        return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
