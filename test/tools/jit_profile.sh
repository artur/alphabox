#!/bin/bash
# Profile compiled code on the desktop snapshot: which blocks the time goes
# to, and which instructions in them.
#
#   jit_profile.sh <disasm-lane-binary> <workload> <section> <scale> [delay_s] [seconds]
#
# Resumes the snapshot (nt_snap.sh run) with the JIT_DISASM lane's binary
# (build-jit-disasm: the same code as production, plus a listing of every
# block with its host address, compile time and bytes), waits delay_s
# (default 18: the workload has started), samples the emulator with macOS
# `sample` for <seconds> (default 6) at 1 ms, and maps the samples to blocks
# and instructions with jit_profile.py. Output: the report on stdout; the raw
# sample and the listing are removed with the run's clone.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 4 ] || { sed -n '2,16p' "$0"; exit 2; }
BIN=$1 W=$2 SEC=$3 SC=$4 DELAY=${5:-18} DUR=${6:-6}
WORK=${ALPHABOX_WORK:-$R/lab}
L=prof-$$
TMP=$(mktemp -d)
KEEP=1 bash "$T/nt_snap.sh" run "$L" "$BIN" "$W" "$SEC" "$SC" > "$TMP/run.txt" 2>&1 &
NP=$!
sleep 3
P=$(pgrep -f "$(basename "$(dirname "$BIN")")/alphabox run" | head -1)
[ -n "$P" ] || { echo "jit_profile: the emulator did not start"; wait $NP; exit 1; }
sleep $((DELAY - 3))
sample "$P" "$DUR" 1 -mayDie -file "$TMP/sample.txt" > /dev/null 2>&1
T_END=$(python3 -c 'import time; print(int(time.time() * 1000))')
wait $NP
python3 "$T/jit_profile.py" "$TMP/sample.txt" "$WORK/ntsnap-$L/jit_disasm_cpu0.txt" "$T_END"
rm -rf "$WORK/ntsnap-$L" "$TMP"
