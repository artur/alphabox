#!/bin/bash
# How much guest code does a Windows boot get through, and how long until the
# desktop? Two arms of one binary, interleaved, on throwaway clones.
#
# usage: boot_rate.sh <binary> <install-dir> <cfg> <seconds> <rounds> \
#                     <label> <base env> <head env>
#   e.g.  boot_rate.sh build-jit/alphabox win2k-installed es40-window.cfg \
#                      60 2 nopflush ALPHABOX_JIT_NOPFLUSH=0 ALPHABOX_X=1
#
# A boot is the workload the compute benchmark cannot see: it is the guest's
# kernel, its drivers and the console firmware, which is where instruction
# cache flushes come from. Reports instructions executed in the window and
# when the idle loop was first recognized (the desktop, as far as the
# emulator can tell).
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 8 ] || { sed -n '2,12p' "$0"; exit 2; }
BIN=$1; INST=$2; CFG=$3; SECS=$4; ROUNDS=$5; LABEL=$6; ENV_BASE=$7; ENV_HEAD=$8
WORK=${ALPHABOX_WORK:-$R/lab}
SRC=$WORK/$INST

one() { # <arm> <round>; prints "<instructions> <idle-loop seen>"
  local arm=$1
  local round=$2
  local d=$WORK/bootrate-$LABEL-$arm-$round
  local env_arm
  [ "$arm" = base ] && env_arm=$ENV_BASE || env_arm=$ENV_HEAD
  rm -rf "$d"
  cp -c -R "$SRC" "$d" 2>/dev/null || cp -R "$SRC" "$d" || exit 2
  cd "$d" || exit 2
  env SDL_VIDEO_DRIVER=dummy ALPHABOX_RATE=5 "$env_arm" "$BIN" run "$CFG" \
      > boot.log 2>&1 &
  local pid=$! t0 now
  t0=$(date +%s)
  while [ $(($(date +%s) - t0)) -lt "$SECS" ]; do
    kill -0 $pid 2>/dev/null || break
    sleep 2
  done
  kill -TERM $pid 2>/dev/null
  for _ in $(seq 1 20); do kill -0 $pid 2>/dev/null || break; sleep 1; done
  kill -0 $pid 2>/dev/null && kill -9 $pid
  wait $pid 2>/dev/null
  # Instructions the processor reported over the whole window, and how far
  # into it the guest reached its idle loop -- to the nearest rate report,
  # which is every 5 seconds.
  local instr desktop
  instr=$(grep -a "CPU0-I-RATE" boot.log |
          sed -n 's/.*(\([0-9]*\) instructions.*/\1/p' |
          awk '{s+=$1} END {print s+0}')
  desktop=$(grep -a "CPU0-I-RATE\|idle loop recognized" boot.log |
            awk '/idle loop/ {print n * 5 "s"; found=1; exit}
                 /RATE/ {n++}
                 END {if (!found) print "-"}')
  echo "$instr $desktop"
}

echo "== $LABEL: $SECS s of boot, $ROUNDS interleaved rounds"
echo "   base: $ENV_BASE"
echo "   head: $ENV_HEAD"
for r in $(seq 1 "$ROUNDS"); do
  b=$(one base "$r"); h=$(one head "$r")
  printf "   round %d   base %13s instr, desktop %-5s   head %13s instr, desktop %-5s\n" \
         "$r" "${b% *}" "${b#* }" "${h% *}" "${h#* }"
done
