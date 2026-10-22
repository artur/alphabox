#!/usr/bin/env python3
"""Compare two device access records (ALPHABOX_RECORD, src/system/AccessRecord.hpp).

usage: trace_diff.py [--device NAME] [--ignore REGEX] [--context N]
                     [--max-lines N] <base-record> <head-record>

Each record is split into streams, one per device and processor (the first
two fields of every line), and each stream of the base is compared with the
same stream of the head, in order. The order between streams is not
compared: it depends on how the processors and device threads interleave,
which no two runs repeat.

--device keeps only the streams of that device (repeatable; a name as the
recorder writes it, or its part before or inside the parentheses).
--ignore drops every line the regular expression matches (repeatable),
for accesses known to depend on timing: on the Tsunami, the interrupt
path's MISC acknowledge, DIR reads and DIM masking,
--ignore 'csr cpu\\d+ [rw] cchip (80|200|240|280|2c0|600|640|680|6c0) '.
What is left is collapsed again as the recorder collapses (a run that
repeats with a period of up to four lines is kept once): an interrupt in
the middle of a polling loop breaks the recorder's collapse there, and
taking its accesses out joins the loop again. Those registers are then not
compared at all: a "same" says nothing about which interrupt lines were
asserted when (the routing the console programs, in configuration space,
is still compared).

Prints, per stream, "same <n>" or "differs" with a unified diff (at most
--max-lines lines of it, default 40), then a summary line.
Exit status 0 when every stream is the same, 1 otherwise, 2 on usage errors.
"""
import argparse
import difflib
import re
import sys


def repeats(hist, r):
    """Whether r continues a loop of period k <= 4 (AccessRecord.cpp)."""
    for k in range(1, 5):
        if len(hist) < 2 * k - 1:
            break
        if hist[-k] == r and all(hist[-j] == hist[-j - k] for j in range(1, k)):
            return True
    return False


def collapse(lines):
    out, hist = [], []
    for r in lines:
        if not repeats(hist, r):
            out.append(r)
        hist = (hist + [r])[-8:]
    return out


def streams(path, devices, ignore):
    out = {}
    with open(path, errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if line.startswith("#"):
                continue
            fields = line.split(" ")
            if len(fields) < 7:
                continue
            dev, who = fields[0], fields[1]
            if devices and not selected(dev, devices):
                continue
            if any(r.search(line) for r in ignore):
                continue
            out.setdefault((dev, who), []).append(" ".join(fields[2:]))
    if ignore:
        out = {k: collapse(v) for k, v in out.items()}
    return out


def selected(dev, devices):
    names = {dev}
    m = re.fullmatch(r"([^(]*)\((.*)\)", dev)
    if m:
        names |= {m.group(1), m.group(2)}
    return bool(names & devices)


def main():
    ap = argparse.ArgumentParser(add_help=True)
    ap.add_argument("base")
    ap.add_argument("head")
    ap.add_argument("--device", action="append", default=[])
    ap.add_argument("--ignore", action="append", default=[])
    ap.add_argument("--context", type=int, default=2)
    ap.add_argument("--max-lines", type=int, default=40)
    a = ap.parse_args()
    try:
        ignore = [re.compile(r) for r in a.ignore]
    except re.error as e:
        print(f"bad --ignore: {e}", file=sys.stderr)
        return 2
    devices = set(a.device)
    try:
        base = streams(a.base, devices, ignore)
        head = streams(a.head, devices, ignore)
    except OSError as e:
        print(e, file=sys.stderr)
        return 2

    differ = 0
    for key in sorted(set(base) | set(head)):
        b, h = base.get(key, []), head.get(key, [])
        name = " ".join(key)
        if b == h:
            print(f"same {len(b):7d}  {name}")
            continue
        differ += 1
        print(f"differs     {name}  (base {len(b)}, head {len(h)} lines)")
        diff = difflib.unified_diff(b, h, "base", "head", lineterm="",
                                    n=a.context)
        for n, line in enumerate(diff):
            if n >= a.max_lines:
                print("    ...")
                break
            print("    " + line)
    total = len(set(base) | set(head))
    print(f"{total - differ} of {total} streams the same")
    return 1 if differ else 0


if __name__ == "__main__":
    sys.exit(main())
