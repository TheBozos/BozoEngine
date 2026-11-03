#!/usr/bin/env python3
# ref_compare.py: compares two per-frame reference records (PS2XREF1, docs/specs/harness.md section 6).
#
# Copyright (c) The Bozos. MIT licence (see LICENSE at the repository root). Standard library only.
#
#   ref_compare.py <a.ref> <b.ref> [--ignore draws,sound] [--frames N]
#
# Prints both headers, the number of frames matching per field, the first frame each field differs at (with both
# values), and exits 1 if any compared field differs on any frame both records have.
"""Compares two PS2XREF1 per-frame reference records."""

import argparse
import struct
import sys

MAGIC = b"PS2XREF1"
FIELDS = ["thread", "ram", "scratchpad", "calls", "draws", "sound", "threads", "eeCycle", "callCount", "drawBatches",
          "drawVertices", "soundSamples"]


def read(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != MAGIC:
        raise SystemExit("%s: not a PS2XREF1 record" % path)
    version, header_bytes, record_bytes, hash_kind, ram_bytes, spr_bytes = struct.unpack_from("<6I", data, 8)
    elf_hash, flags, mask_count = struct.unpack_from("<QII", data, 32)
    text = lambda off: data[off:off + 64].split(b"\0")[0].decode("utf-8", "replace")
    header = {"version": version, "elfHash": elf_hash, "flags": flags, "run": text(48), "build": text(112),
              "elf": text(176)}
    frames = []
    for at in range(header_bytes, len(data) - record_bytes + 1, record_bytes):
        v = struct.unpack_from("<Ii6Q4I", data, at)
        frames.append(dict(zip(["frame"] + FIELDS, v)))
    return header, frames


def main(argv):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--ignore", default="", help="fields not compared (comma-separated), e.g. draws,sound")
    p.add_argument("--frames", type=int, default=0, help="compare only the first N frames")
    args = p.parse_args(argv)
    ignore = {x.strip() for x in args.ignore.split(",") if x.strip()}
    ha, fa = read(args.a)
    hb, fb = read(args.b)
    for name, h, f in (("a", ha, fa), ("b", hb, fb)):
        print("%s: run %s, build %s, ELF %s %016x, %d frames" % (name, h["run"], h["build"], h["elf"], h["elfHash"],
                                                                 len(f)))
    n = min(len(fa), len(fb))
    if args.frames:
        n = min(n, args.frames)
    compared = [x for x in ["ram", "scratchpad", "calls", "draws", "sound", "threads", "eeCycle"] if x not in ignore]
    same = {k: 0 for k in compared}
    first = {}
    for i in range(n):
        for k in compared:
            if fa[i][k] == fb[i][k]:
                same[k] += 1
            elif k not in first:
                first[k] = i
    print("%d frames compared: %s" % (n, ", ".join("%s %d" % (k, same[k]) for k in compared)))
    for k in compared:
        if k in first:
            i = first[k]
            extra = ""
            if k == "calls":
                extra = " (call counts %d and %d)" % (fa[i]["callCount"], fb[i]["callCount"])
            print("  %s differs first at frame %d: %016x vs %016x%s" % (k, i, fa[i][k], fb[i][k], extra))
    ok = not first and len(fa) == len(fb)
    if len(fa) != len(fb):
        print("frame counts differ: %d and %d" % (len(fa), len(fb)))
    print("identical" if ok else "DIFFERS")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
