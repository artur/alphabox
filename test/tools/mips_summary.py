#!/usr/bin/env python3
"""Summarize JIT_STATS throughput lines from one or more emulator logs.

usage: mips_summary.py <label=log> [<label=log> ...] [--skip N]

Reads "[JIT][STATS][CPUn] throughput X MIPS (...)" lines and prints, per log
and CPU, the sample count and the p25/p50/p75/max MIPS. --skip drops each
CPU's first N samples (boot/firmware warm-up)."""
import re
import sys

PAT = re.compile(rb'\[JIT\]\[STATS\]\[CPU(\d+)\] throughput (\d+) MIPS')


def pct(v, p):
    if not v:
        return 0
    s = sorted(v)
    return s[min(len(s) - 1, int(p * (len(s) - 1) + 0.5))]


def main():
    args = sys.argv[1:]
    skip = 0
    if '--skip' in args:
        i = args.index('--skip')
        skip = int(args[i + 1])
        del args[i:i + 2]
    for arg in args:
        label, _, path = arg.partition('=')
        cpus = {}
        with open(path, 'rb') as f:
            for line in f:
                m = PAT.search(line)
                if m:
                    cpus.setdefault(int(m.group(1)), []).append(int(m.group(2)))
        if not cpus:
            print(f'{label:>10}: no throughput samples')
            continue
        for c in sorted(cpus):
            v = cpus[c][skip:]
            print(f'{label:>10} cpu{c}: n={len(v):4d}  p25 {pct(v, .25):5d}  '
                  f'p50 {pct(v, .5):5d}  p75 {pct(v, .75):5d}  max {max(v) if v else 0:5d} MIPS')


if __name__ == '__main__':
    main()
