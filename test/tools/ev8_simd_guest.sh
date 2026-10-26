#!/bin/bash
# The 21464's 1C short-vector SIMD set and 07 paired single under Linux:
# test/tools/ev8_simd.c, compiled by the guest's own gcc in the linux-es47
# guest of guests_check.sh on the `ev8` row (GUESTS_EV7_CPU=ev8), its kernels
# checked against scalar C. Stage S5/S6 of docs/platforms/ev8.md.
#
# usage: ev8_simd_guest.sh <alphabox-binary> <label>
#
# The source goes in through the serial console, base64 in lines of 300
# characters (GUESTS_EXTRA_CMDS, whose commands are separated by ';', which
# base64 never contains), then base64 -d, gcc -O1 and a run. The guest boots
# on a clone of its disk, as every guests_check.sh guest does. PASS when the
# guest passes and the program prints "EV8SIMD: PASS". Environment as for
# guests_check.sh (ALPHABOX_WORK, GUESTS_PORT_BASE, GUESTS_BOOT_TMO).
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -eq 2 ] || { echo "usage: $0 <alphabox-binary> <label>"; exit 2; }
b64=$(base64 < "$T/ev8_simd.c" | tr -d '\n')
cmds="rm -f /tmp/e.b64 /tmp/e.c /tmp/e"
n=${#b64}
i=0
while [ $i -lt $n ]; do
  cmds="$cmds;echo ${b64:$i:300} >> /tmp/e.b64"
  i=$((i + 300))
done
cmds="$cmds;base64 -d /tmp/e.b64 > /tmp/e.c;gcc -O1 -o /tmp/e /tmp/e.c;/tmp/e"
GUESTS_EV7_CPU=ev8 GUESTS_EXTRA_CMDS="$cmds" "$T/guests_check.sh" "$1" "$2" linux-es47
rc=$?
log=${ALPHABOX_WORK:-$R/lab}/runs/guests-$2/linux-es47.log
# the console's lines, the shell's terminal escapes taken out
lines=$(LC_ALL=C tr -d '\r' < "$log" | LC_ALL=C sed -e $'s/\033\\[[?0-9;]*[a-zA-Z]//g' |
  grep -a 'EV8SIMD' | grep -av 'echo')
echo "$lines"
if [ $rc -eq 0 ] && echo "$lines" | grep -q '^EV8SIMD: PASS'; then
  echo "PASS ev8_simd: the guest's program agrees with scalar C"
  exit 0
fi
echo "FAIL ev8_simd (guest rc=$rc; see $log)"
exit 1
