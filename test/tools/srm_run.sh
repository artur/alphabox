#!/bin/bash
# SRM regression boot for one build lane: boot the no-VGA test machine to
# P00>>> over telnet, stop it gracefully, and diff the console log against
# test/rom/axp_correct.log (the host-dependent "CPU n speed is" line ignored).
#
# usage: PORT=<port> srm_run.sh <alphabox-binary> <label> [timeout_s]
#   Runs in $ALPHABOX_WORK/runs/srm-<label> (ALPHABOX_WORK defaults to <repo>/lab).
#   Give each lane its own PORT to run lanes in parallel.
#
# Prints the prompt status, "diff clean" (or the first diff lines) and the
# JIT_VERIFY mismatch count. Exit status: 0 for prompt + diff clean + no
# mismatches, 1 otherwise.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { echo "usage: PORT=<port> $0 <alphabox-binary> <label> [timeout_s]"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
TMO=${3:-300}
PORT=${PORT:-21000}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/srm-$LABEL

[ -x "$BIN" ] || { echo "srm_run: $BIN is not executable"; exit 2; }
rm -rf "$D" && mkdir -p "$D" || exit 2
cp "$R/test/rom/cl67srmrom.exe" "$D/"
python3 "$T/srm_cfg.py" --out "$D/es40.cfg" --port "$PORT" || exit 2
cd "$D" || exit 2

"$BIN" run > alphabox.out 2>&1 &
PID=$!
start=$(date +%s)
python3 "$T/srm_console.py" --port "$PORT" --log axp.log --timeout "$TMO" \
  --pid $PID --after sigterm --exit-timeout 30 > console.status
elapsed=$(($(date +%s) - start))
if kill -0 $PID 2>/dev/null; then
  kill -9 $PID 2>/dev/null # our own emulator only
fi
wait $PID 2>/dev/null
sed 's/^/  /' console.status
echo "  boot+stop took ${elapsed}s"

ok=0
norm() { tr -d '\000' < "$1" | sed 's/CPU [0-9] speed is.*//'; }
if grep -q '^status=prompt' console.status; then
  if diff -c <(norm "$R/test/rom/axp_correct.log") <(norm axp.log) > diff.txt; then
    echo "diff clean"
  else
    echo "DIFF (first 40 lines of $D/diff.txt):"
    head -40 diff.txt
    ok=1
  fi
else
  echo "FAIL: no SRM prompt"
  ok=1
fi
mm=$(grep -ac MISMATCH alphabox.out)
echo "mismatch lines: $mm"
grep -a '\[JIT\]\[VERIFY\]' alphabox.out | tail -1
[ "$mm" -eq 0 ] || ok=1
# A block the emitter cannot encode is not compiled and runs in the
# interpreter instead: the log still matches and verify still passes, only
# many times slower. So an emit error fails the run too.
ee=$(grep -ac 'EMIT-ERROR' alphabox.out)
[ "$ee" -eq 0 ] || { echo "FAIL: $ee emit errors, e.g.:"; grep -a -m3 'EMIT-ERROR' alphabox.out; ok=1; }
exit $ok
