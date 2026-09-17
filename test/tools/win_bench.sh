#!/bin/bash
# Boot an installed Windows guest headless on a throwaway APFS clone and report
# what it reached and how fast it ran.
#
# usage: win_bench.sh <label> <alphabox-binary> <install-dir> <cfg> <seconds> [VAR=value ...]
#   install-dir  guest install directory; relative paths are under
#                $ALPHABOX_WORK (default <repo>/lab). It is never modified:
#                the run uses a clone in $ALPHABOX_WORK/bench-<label>.
#   cfg          configuration file name inside the install directory
#   VAR=value    extra environment, e.g. ALPHABOX_NO_IDLE=1, ALPHABOX_IRQSTATS=1
#
# Prints: the last framebuffer as PNG (check it: desktop / logon screen), host
# CPU% samples near the end, per-CPU MIPS p25/p50/p75 (JIT_STATS builds only),
# idle-loop recognition and notable warnings.
#
# Benchmark caveat: with the guest idle at its desktop and ALPHABOX_NO_IDLE=1,
# the steady state is the guest's idle loop, so MIPS measures the idle loop,
# not real guest code.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
[ $# -ge 5 ] || { sed -n '2,20p' "$0"; exit 2; }
LABEL=$1 BIN=$2 INST=$3 CFG=$4 SECS=$5
shift 5
case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
[ -x "$BIN" ] || { echo "$LABEL: binary $BIN missing"; exit 2; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -f "$SRC/$CFG" ] || { echo "$LABEL: $SRC/$CFG missing"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "$LABEL: memory pressure critical, not starting"
  exit 2
fi
D=$WORK/bench-$LABEL
rm -rf "$D"
# APFS clone (instant, copy-on-write); plain copy elsewhere. Relative image
# links inside the install keep resolving.
cp -c -R "$SRC" "$D" 2>/dev/null || cp -R "$SRC" "$D" || exit 2
rm -rf "$D/fb" && mkdir -p "$D/fb"
cd "$D" || exit 2
env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb "$BIN" run "$CFG" > bench.log 2>&1 &
P=$!
echo "== $LABEL: pid $P, $(basename "$BIN") on $INST/$CFG for ${SECS}s $*"
for i in $(seq 1 "$SECS"); do
  kill -0 $P 2>/dev/null || { echo "$LABEL: emulator exited after ${i}s"; break; }
  if [ "$i" -eq $((SECS - 25)) ] && command -v top > /dev/null; then
    printf '  host CPU%%: '
    for k in 1 2 3 4 5 6; do top -l 2 -s 2 -pid $P -stats cpu 2>/dev/null | tail -1 | tr '\n' ' '; done
    echo
  fi
  sleep 1
done
last=$(ls -t fb 2>/dev/null | grep ppm | head -1)
[ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" last.png && echo "  last screen: $D/last.png"
kill $P 2>/dev/null # our own emulator only
for i in $(seq 1 20); do kill -0 $P 2>/dev/null || break; sleep 0.5; done
kill -0 $P 2>/dev/null && echo "  still running after SIGTERM!"
wait $P 2>/dev/null
python3 "$T/mips_summary.py" "$LABEL=bench.log" --skip 3 | sed 's/^/  /'
grep -aE 'CPU-I-IDLE:|CPU-I-PACING|SYS-W-UNKNOWNCFG|KBC-W-DROP|Emulator Failure|OPCDEC|CPU-W-IPR|Unknown TIG' bench.log |
  sort | uniq -c | head -14 | sed 's/^/  /'
a=$(grep -ac 'IDE-W-ATAPI' bench.log)
[ "$a" -gt 0 ] && echo "  %IDE-W-ATAPI lines: $a"
grep -a '%IRQ-I-STATS' bench.log | tail -1 | cut -c1-160 | sed 's/^/  /'
exit 0
