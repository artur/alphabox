#!/bin/bash
# The guest's instruction rate on one benchmark section, in MIPS.
#
#   sect_mips.sh <label> <alphabox-binary> <workload> <section> <scale> [K=V ...]
#
# Resumes the desktop snapshot (nt_snap.sh run) with ALPHABOX_RATE=0.25, runs
# one workload section, and prints instructions / time over the section's
# plateau: the longest run of consecutive rate windows within 25% of each
# other. That leaves out the idle desktop around the run and the warm-up at
# its start, while hot code is compiled and the pin set adapts. K=V pairs
# are passed to the emulator's environment (e.g. ALPHABOX_JIT_CHAIN=0).
#
# The snapshot must have one processor (made from es40-window.cfg; a
# separate ALPHABOX_WORK keeps it apart from perf_ab's), and the scale must
# make the section the longest busy stretch: about 3 s or more of guest
# time. At scale 1 on 2026-10-23 some sections took 0.1-0.3 s.
#
# A rate, not a timing: for an A/B that goes in the ledger use perf_ab.py.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 5 ] || { sed -n '2,15p' "$0"; exit 2; }
L=$1 B=$2 W=$3 S=$4 N=$5
shift 5
WORK=${ALPHABOX_WORK:-$R/lab}
env "$@" ALPHABOX_RATE=0.25 KEEP=1 bash "$T/nt_snap.sh" run "mips-$L" "$B" "$W" \
    "$S" "$N" > /dev/null 2>&1
python3 - "$WORK/ntsnap-mips-$L/run.log" "$L" <<'PY'
import re, sys
log = open(sys.argv[1], errors='replace').read()
# One processor only: on two the guest moves the benchmark between them in
# the middle of a section, and the second keeps busy after it, so CPU0's
# longest plateau is not the section.
if 'CPU1-I-RATE' in log:
    print(f"{sys.argv[2]:28} needs a one-processor snapshot "
          "(nt_snap.sh make ... es40-window.cfg)")
    sys.exit(1)
rows = [(float(a), int(b), float(c)) for a, b, c in re.findall(
    r'CPU0-I-RATE: ([\d.]+) MIPS \((\d+) instructions in ([\d.]+) s\)', log)]
best, run = [], []
for r in rows:
    if r[0] > 100 and run and all(0.8 <= r[0] / x[0] <= 1.25 for x in run[-1:]) \
            and 0.75 <= r[0] / (sum(x[0] for x in run) / len(run)) <= 1.33:
        run.append(r)
    else:
        run = [r] if r[0] > 100 else []
    if len(run) > len(best):
        best = list(run)
if not best:
    print(f"{sys.argv[2]:28} no busy windows")
    sys.exit(1)
ins = sum(r[1] for r in best); t = sum(r[2] for r in best)
print(f"{sys.argv[2]:28} {ins / t / 1e6:8.0f} MIPS  ({len(best)} windows, {t:.1f} s)")
PY
rc=$?
rm -rf "$WORK/ntsnap-mips-$L"
exit $rc
