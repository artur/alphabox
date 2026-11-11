#!/bin/bash
# The Marvel boards' TOY clock takes the time a guest sets.
#
# usage: marvel_toy_check.sh <alphabox-binary> <label>
#
# The CMM of an ES47, ES80 or GS1280 holds the TOY (MC146818 registers in
# its memory, written through the processor's byte window). OpenVMS sets it
# as it would the chip: SET in register B, the time, SET released. Until
# 2026-11-11 the model never saw register B written (it is the high byte of
# the word at 10, and only even addresses were looked at), so the time a
# guest set was overwritten by the host's at the next read.
#
# guests_check.sh boots OpenVMS 8.4 on the ES47 (the vms-es47 guest, on a
# clone of its disk), and after its own checks runs
#   SET TIME=02-JAN-2022:12:34:56
#   SHOW TIME
# The emulator reports the offset from the host's clock it took when SET
# was released (%MVL-I-CMM: the console set the TOY): it must be that of
# 2 January 2022 12:34:56, within a day (the TOY keeps the guest's local
# time, the offset is counted in UTC).
#
# Prints guests_check.sh's line and `PASS` or `FAIL <why>` last. Exit 0 on
# PASS. Output in $ALPHABOX_WORK/runs/guests-toy-<label>/ (ALPHABOX_WORK
# defaults to <repo>/lab); the run directory is removed after a PASS.
#
# Environment: as guests_check.sh (GUESTS_PORT_BASE, GUESTS_BOOT_TMO, ...).
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { sed -n '2,5p' "$0"; exit 2; }
BIN=$1; LABEL=toy-$2
WORK=${ALPHABOX_WORK:-$R/lab}
OUT=$WORK/runs/guests-$LABEL

GUESTS_KEEP=1 GUESTS_EXTRA_CMDS='SET TIME=02-JAN-2022:12:34:56;SHOW TIME' \
  bash "$T/guests_check.sh" "$BIN" "$LABEL" vms-es47
rc=$?
D=$OUT/vms-es47
finish() { # finish PASS|FAIL [why]
  [ "$1" = PASS ] && rm -rf "$D"
  echo "$1${2:+ $2}"
  [ "$1" = PASS ]
  exit $?
}
[ $rc = 0 ] || finish FAIL "the guest did not pass its own checks"
[ -f "$D/alphabox.out" ] || finish FAIL "no emulator output in $D"

# SHOW TIME after the SET TIME: the guest's own clock took it.
tr -d '\r\000' < "$D/console.log" | grep -aqE '(^| )2-JAN-2022 12:3[4-9]' ||
  finish FAIL "SHOW TIME does not show 2-JAN-2022 after SET TIME"

# The last offset the CMM took, against the one asked for.
off=$(tr -d '\r\000' < "$D/alphabox.out" |
  sed -n 's/.*the console set the TOY (offset \(-\{0,1\}[0-9]*\) s from the host).*/\1/p' | tail -1)
[ -n "$off" ] || finish FAIL "the CMM never saw the TOY set (no %MVL-I-CMM line)"
want=$(python3 -I -c 'import calendar, time
print(calendar.timegm((2022, 1, 2, 12, 34, 56)) - int(time.time()))')
d=$((off - want)); [ $d -lt 0 ] && d=$((-d))
[ $d -le 90000 ] ||
  finish FAIL "the TOY's offset is $off s, not within a day of $want"
finish PASS
