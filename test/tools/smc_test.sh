#!/bin/bash
# Does a guest that rewrites its own compiled code still run the right code?
#
# usage: smc_test.sh <alphabox-binary> [label]
#
# The emulator skips an instruction-cache flush when nothing has been written
# to memory any block was compiled from. This boots a boot block that runs a
# loop until the JIT has compiled it, stores a different instruction into the
# middle of that compiled block, executes CALL_PAL IMB and re-enters the loop
# (smc_image.py). The new instruction branches to a halt, so:
#
#     the emulator halts   = the store was noticed and the block re-formed
#     the emulator hangs   = the stale compiled block ran; the old branch
#                            takes the guest round the loop and back, forever
#
# Four arms, and the last two are the point of the exercise:
#
#   NOPFLUSH=0   the unconditional flush: must halt (the image itself works)
#   NOPFLUSH=1   the flush we skip when the map says nothing was written:
#                must halt (the store was tracked)
#   NOPFLUSH=1 + BREAK   the processor's stores deliberately not reported:
#                must HANG (so arm 2 passing means something)
#   NOPFLUSH=2 + BREAK   the audit: flush anyway, but say so when a block's
#                source changed while the map claimed nothing was written.
#                Must halt AND report -- that is the instrument to run a
#                real guest under, and this shows it is not asleep.
#
# Exit status 0 only if all four behave as they must.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 1 ] || { sed -n '2,10p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=${2:-smc}
PORT=${PORT:-21740}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/smc-$LABEL
HANG_S=${HANG_S:-70}   # long enough for a working arm; a hang never ends

[ -x "$BIN" ] || { echo "smc_test: $BIN is not executable"; exit 2; }
rm -rf "$D"; mkdir -p "$D"
IMG=$D/smc.img
python3 "$T/smc_image.py" "$IMG" > "$D/image.txt" || exit 2
sed 's/^/  /' "$D/image.txt"

# One arm: boot the image and say whether the guest halted.
arm() {
  local tag=$1 port=$2; shift 2
  local start end secs
  start=$(python3 -c 'import time; print(time.time())')
  env "$@" PORT=$port ALPHABOX_WORK=$WORK FLOPPY="$IMG" EXIT_ON_HALT=1 \
    CMDS="boot dva0" AFTER=wait-exit CMD_TIMEOUT=$HANG_S \
    bash "$T/srm_probe.sh" "$BIN" "$LABEL-$tag" $((HANG_S + 60)) \
    > "$D/$tag.log" 2>&1
  end=$(python3 -c 'import time; print(time.time())')
  secs=$(python3 -c "print('%.0f' % ($end - $start))")
  if grep -aq "halted CPU 0\|HALT instruction executed\|HALT invoked" "$D/$tag.log"
  then echo "halted after ${secs}s"
  else echo "did NOT halt (${secs}s)"
  fi
}

ok=0
r1=$(arm plain   $((PORT + 0)) ALPHABOX_JIT_NOPFLUSH=0)
r2=$(arm skipped $((PORT + 1)) ALPHABOX_JIT_NOPFLUSH=1)
r3=$(arm broken  $((PORT + 2)) ALPHABOX_JIT_NOPFLUSH=1 ALPHABOX_JIT_NOPFLUSH_BREAK=1)
r4=$(arm audit   $((PORT + 3)) ALPHABOX_JIT_NOPFLUSH=2 ALPHABOX_JIT_NOPFLUSH_BREAK=1)
# srm_probe keeps the emulator's own output in its run directory.
n_audit=$(grep -ac "NOPFLUSH AUDIT" "$WORK/runs/probe-$LABEL-audit/alphabox.out" 2>/dev/null || echo 0)

check() { # <what must happen> <result> <description>
  case "$2" in
    halted*) got=halt ;;
    *)       got=hang ;;
  esac
  if [ "$got" = "$1" ]; then printf "  %-34s %-22s ok\n" "$3" "$2"
  else printf "  %-34s %-22s *** FAIL (expected %s) ***\n" "$3" "$2" "$1"; ok=1
  fi
}

echo "== $LABEL: a guest rewriting its own compiled code"
check halt "$r1" "unconditional flush"
check halt "$r2" "flush skipped when map is clean"
check hang "$r3" "stores deliberately unreported"
check halt "$r4" "the audit, with stores unreported"
if [ "$n_audit" -gt 0 ]; then
  printf "  %-34s %-22s ok\n" "... and it reported it" "$n_audit time(s)"
else
  printf "  %-34s %-22s *** FAIL (expected a report) ***\n" \
         "... and it reported it" "silent"; ok=1
fi
[ $ok = 0 ] && echo "  SMC: PASS" || echo "  SMC: FAIL"
exit $ok
