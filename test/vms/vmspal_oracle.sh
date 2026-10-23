#!/bin/bash
# The vmspal differential oracle: every native OpenVMS PALcode routine of the
# interpreter (src/cpu/AlphaCPU_vmspal.cpp: the CALL_PAL routines, the
# interrupt entries, the TB-miss and fault entries) compared with the
# console's own PALcode doing the same work, on a real OpenVMS 8.4 boot.
#
# usage: vmspal_oracle.sh [label] [vms_dcl.py options ...]
#
# It copies this tree's tracked files (as they are in the working tree, so
# an uncommitted fix is tested too) to $ALPHABOX_WORK/vmspal-oracle-build,
# applies test/vms/vmspal_oracle.patch there (src/ itself is never changed),
# builds a headless interpreter, and boots OpenVMS with vms_dcl.py
# (--workload full --shutdown by default; further options are passed on,
# e.g. --memory-bits 29 on a host short of memory). The report is
# $ALPHABOX_WORK/runs/<label>-report.txt; its table is printed at the end.
#
# How it compares: the native routine runs first with its effects recorded
# (memory stores, TB and TB-shadow changes, the lock flag; the chipset
# acknowledges and CSR writes are held back and the PIC's acknowledge cycle
# is peeked, so that nothing a second run would repeat happens twice). Its
# result is kept and undone, and the PALcode then runs from the same state.
# At the first instruction boundary where the PALcode reaches the native
# result's PC (or returns to native mode) the two are compared: PC,
# R0-R30, PS (p22), p21, mode, ASNs, FEN, IER, AST and software interrupt
# state, what a STx_C would do (the lock flag and HRM D.19's rule), a missed
# interrupt signal, both TBs (as sets), and every quadword either run
# stored. The guest goes on with the PALcode's result, so a difference does
# not propagate. Classified apart: the registers the ARM leaves
# UNPREDICTABLE after an MxPR (13.1), clock readings, events during which
# an interrupt line was raised (a race, not a difference), the PALcode's
# scratch temporaries (by offset), and nested double TB misses. The report
# also counts the PALcode's instructions per routine, and their share of
# all instructions executed.
#
# A patch, not code in src/: the hooks sit in the interpreter's hottest
# paths. When src/ moves on and the patch stops applying, refresh it from
# a patched copy (diff -ruN <pristine>/src <patched>/src).
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
LABEL=${1:-vmspal-oracle}
[ $# -gt 0 ] && shift
D=$WORK/vmspal-oracle-build
mkdir -p "$D" "$WORK/runs" || exit 2
rm -rf "$D/src"
mkdir -p "$D/src"
( cd "$R" && git ls-files -z -- CMakeLists.txt cmake src test/unit test/fp |
    tar --null -cf - -T - ) | tar -x -C "$D/src" || exit 2
if ! ( cd "$D/src" && patch -p1 -s -N < "$T/vmspal_oracle.patch" ); then
  echo "vmspal_oracle: the patch does not apply to this tree; refresh test/vms/vmspal_oracle.patch"
  exit 2
fi
cmake -S "$D/src" -B "$D/build" -DCMAKE_BUILD_TYPE=Release -DDISABLE_SDL=yes \
  > "$D/configure.log" 2>&1 || { echo "vmspal_oracle: configure failed, see $D/configure.log"; exit 2; }
cmake --build "$D/build" --target alphabox -j"$(sysctl -n hw.ncpu 2>/dev/null || nproc)" \
  > "$D/build.log" 2>&1 || { echo "vmspal_oracle: build failed, see $D/build.log"; exit 2; }
REPORT=$WORK/runs/$LABEL-report.txt
rm -f "$REPORT"
[ $# -gt 0 ] || set -- --workload full --shutdown
ALPHABOX_WORK=$WORK python3 -I "$T/vms_dcl.py" "$D/build/alphabox" "$LABEL" \
  --env "ALPHABOX_VMSPAL_ORACLE=$REPORT" "$@"
rc=$?
[ -f "$REPORT" ] && sed -n '1,/^$/p' "$REPORT"
echo "report: $REPORT"
exit $rc
