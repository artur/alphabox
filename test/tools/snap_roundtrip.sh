#!/bin/bash
# Check that every machine Alphabox emulates survives a snapshot: saved at its
# console prompt, restored, saved again, and resumed.
#
# usage: snap_roundtrip.sh <alphabox-binary> <label> [board ...]
#
# Boards (default all twelve), firmware and configurations are those of
# boards_check.sh. For each board:
#  1. boards_check.sh boots it to its prompt, runs `show config` (and
#     `show fru` on the EV7 boards) and saves the machine there: SIGUSR1 with
#     ALPHABOX_SNAPSHOT_EXIT=1, to snap1.axp.
#  2. The emulator restores snap1.axp and saves it again before anything runs
#     (ALPHABOX_RESTORE_SAVE), to snap2.axp. The two files must be the same
#     byte for byte, but for the fields that follow the host's clock, listed
#     in HOST_TIME below with why. Where they differ, ALPHABOX_SNAPSHOT_MAP's
#     map names the part of the file (memory, system, chipset or a component,
#     with its class) and the offset within it.
#  3. It restores snap1.axp again, wakes the console with a carriage return
#     and runs the same commands: their output must be the same as in 1, and
#     the emulator must exit with status 0 on SIGTERM.
#
# Output: one line per board, `PASS|FAIL|SKIP <board> <seconds> <reason>`, in
# $ALPHABOX_WORK/runs/snap-<label>/summary.txt (ALPHABOX_WORK defaults to
# <repo>/lab). Per board in that directory:
#   <board>.cmp        where the two state files differ, by part
#   <board>.resume.log the commands' output after the second restore,
#                      normalised as boards_check.sh's <board>.cmds.log
# boards_check.sh's run (step 1) is $ALPHABOX_WORK/runs/boards-snap-<label>/,
# and each board's run directory there holds the state files, maps and
# emulator output of all three steps. Exit 0 only if no board failed.
#
# Environment:
#   BOARDS_PORT_BASE     as boards_check.sh (default 21600)
#   BOARDS_PROMPT_TMO    as boards_check.sh; also the bound on steps 2 and 3
#   SNAP_KEEP=1          keep the state files and the firmware copies (they
#                        are removed after each board otherwise)
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
PTMO=${BOARDS_PROMPT_TMO:-600}
ALL="es40 ds20e ds20 ds10 ds20l es45 es45m1 ds25 ds15 es47 es80 gs1280"

[ $# -ge 2 ] || { sed -n '2,5p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2; shift 2
BOARDS=${*:-$ALL}
[ -x "$BIN" ] || { echo "snap_roundtrip: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/snap-$LABEL
BOUT=$WORK/runs/boards-snap-$LABEL
mkdir -p "$OUT" || exit 2
: > "$OUT/summary.txt"
PID=
CPID=

cleanup() {
  for p in $PID $CPID; do
    kill -0 "$p" 2>/dev/null && { kill "$p" 2>/dev/null; kill -9 "$p" 2>/dev/null; }
  done
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# As boards_check.sh's, so the outputs of steps 1 and 3 compare.
normalise() { # normalise <rundir>
  tr -d '\r\000' | sed -E \
    -e "s#$1#@RUN@#g" -e "s#$WORK#@WORK#g" -e "s#$R#@REPO@#g" \
    -e "s#127\\.0\\.0\\.1:[0-9]+#127.0.0.1:@PORT@#g" \
    -e "s#\\b216[0-4][0-9]\\b#@PORT@#g" \
    -e 's#[0-9]{4}-[0-9]{2}-[0-9]{2}[ T][0-9:.]+#@TIME@#g' \
    -e 's#[0-9]{1,2}-[A-Z][a-z]{2}-[0-9]{4} [0-9:.]+#@TIME@#g' \
    -e 's#\b[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?#@HMS@#g' \
    -e 's/[ \t]+$//'
}

# wait_pid <pid> <seconds>: wait for our own emulator to end, with a bound;
# sets RC to its exit status (137 when it had to be killed).
wait_pid() {
  local n=0
  while kill -0 "$1" 2>/dev/null && [ $n -lt $(($2 * 5)) ]; do sleep 0.2; n=$((n + 1)); done
  kill -0 "$1" 2>/dev/null && { kill "$1" 2>/dev/null; sleep 2; kill -9 "$1" 2>/dev/null; }
  wait "$1" 2>/dev/null
  RC=$?
}

# Where two state files differ: the parts of the first one's map, the bytes
# that differ in each and the first few offsets within it. Exit 0 when they
# are the same but for the fields in HOST_TIME.
compare() { # compare <snap1> <snap2> <map1> <map2>
  python3 -I - "$@" <<'PY'
import sys
# Fields that follow the host's clock, which runs on between the save and
# the restore although no guest instruction does: (the component's class,
# as the end of its name in the map; offset in its part; length; what).
HOST_TIME = [
    # The ALi's 8254: the PCI part (4112 bytes), its magic and size (12),
    # then SAli_state, whose SPit_state is at 272 (AliM1543C.hpp, I8254.hpp).
    # CI8254::save writes each counter's count as of the save, and the OUT
    # bit in its status: the counters count host time (I8254.cpp).
    ("CAliM1543C", 4124 + 272, 12, "8254 counts (SPit_state.counter[0..2])"),
    ("CAliM1543C", 4124 + 272 + 36, 3, "8254 OUT bits (SPit_state.status[0..2])"),
]
a, b = (open(p, "rb").read() for p in sys.argv[1:3])
maps = []
for p in sys.argv[3:5]:
    try:
        maps.append([(int(l.split(" ", 1)[0]), l.split(" ", 1)[1].strip())
                     for l in open(p) if l.strip()])
    except OSError:
        maps.append([])
if a == b:
    print("identical, %d bytes" % len(a))
    sys.exit(0)
if maps[0] != maps[1]:
    print("the maps differ: the parts are not in the same places")
parts = [(0, "header"), (8, "memory")] + maps[0]
def part(off):
    best = parts[0]
    for p in parts:
        if p[0] <= off:
            best = p
    return best
def host_time(name, rel):
    for suffix, at, n, what in HOST_TIME:
        if name.endswith(suffix) and at <= rel < at + n:
            return what
    return None
diffs, allowed = {}, set()
n = min(len(a), len(b))
for c in range(0, n, 4096):
    if a[c:c + 4096] == b[c:c + 4096]:
        continue
    for off in range(c, min(c + 4096, n)):
        if a[off] != b[off]:
            start, name = part(off)
            what = host_time(name, off - start)
            if what:
                allowed.add("%s %s" % (name, what))
            else:
                diffs.setdefault((start, name), []).append(off - start)
if len(a) != len(b):
    print("sizes differ: %d and %d bytes" % (len(a), len(b)))
elif not diffs and maps[0] == maps[1]:
    print("identical but for host time (%s), %d bytes" % (
        "; ".join(sorted(allowed)), len(a)))
    sys.exit(0)
for (start, name), offs in sorted(diffs.items()):
    print("%s (at %d): %d bytes differ, at +%s" % (
        name, start, len(offs), ", +".join(str(o) for o in offs[:16])
        + (", ..." if len(offs) > 16 else "")))
for what in sorted(allowed):
    print("host time, allowed: %s" % what)
sys.exit(1)
PY
}

report() { # report <board> <PASS|FAIL> <seconds> <reason>
  echo "$2 $1 $3 $4" | tee -a "$OUT/summary.txt"
}

rc_all=0
for b in $BOARDS; do
  t0=$(date +%s)
  D=$BOUT/$b
  rm -f "$OUT/$b".{cmp,resume.log}
  # 1. Boot, commands, snapshot at the prompt.
  ALPHABOX_SNAPSHOT=snap1.axp ALPHABOX_SNAPSHOT_EXIT=1 \
    ALPHABOX_SNAPSHOT_MAP=snap1.map BOARDS_AFTER=sigusr1 BOARDS_KEEP=1 \
    "$T/boards_check.sh" "$BIN" "snap-$LABEL" "$b" > "$OUT/$b.boards.txt" 2>&1
  if ! grep -q "^PASS $b " "$OUT/$b.boards.txt" || [ ! -f "$D/snap1.axp" ]; then
    report "$b" FAIL $(( $(date +%s) - t0 )) "no snapshot at the prompt: $(grep -m1 "$b" "$OUT/$b.boards.txt")"
    rc_all=1; continue
  fi
  port=$(sed -nE 's/.*serial0 = serial \{ address = "127.0.0.1"; port = ([0-9]+);.*/\1/p' "$D/es40.cfg")

  # 2. Restore and save again before anything runs. The serial port waits for
  # a telnet client before the machine is built: connect one that only reads.
  rm -f "$D/snap2.axp" "$D/snap2.map"
  ( cd "$D" && ALPHABOX_RESTORE=snap1.axp ALPHABOX_RESTORE_SAVE=snap2.axp \
      ALPHABOX_SNAPSHOT_MAP=snap2.map exec "$BIN" run es40.cfg ) \
    > "$D/restore-save.out" 2>&1 &
  PID=$!
  python3 -I - "$port" "$PTMO" > "$D/restore-save.console" 2>&1 <<'PY' &
import socket, sys, time
port, tmo = int(sys.argv[1]), float(sys.argv[2])
end = time.time() + tmo
while time.time() < end:
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        time.sleep(0.05)
else:
    sys.exit(1)
s.settimeout(1)
while time.time() < end:
    try:
        if not s.recv(4096):
            break
    except socket.timeout:
        continue
    except OSError:
        break
PY
  CPID=$!
  wait_pid "$PID" "$PTMO"; PID=
  rc2=$RC
  wait "$CPID" 2>/dev/null; CPID=
  if [ "$rc2" != 0 ] || [ ! -f "$D/snap2.axp" ]; then
    report "$b" FAIL $(( $(date +%s) - t0 )) "restore and save: exit status $rc2, $(grep -a -m1 -E 'SYS-F|SYS-I-VERSION|FAILURE' "$D/restore-save.out")"
    rc_all=1; continue
  fi
  compare "$D/snap1.axp" "$D/snap2.axp" "$D/snap1.map" "$D/snap2.map" > "$OUT/$b.cmp"
  same=$?

  # 3. Resume, wake the console, the same commands.
  cmds=(--cmd "show config")
  case $b in es47 | es80 | gs1280) cmds+=(--cmd "show fru") ;; esac
  ( cd "$D" && ALPHABOX_RESTORE=snap1.axp exec "$BIN" run es40.cfg ) \
    > "$D/resume.out" 2>&1 &
  PID=$!
  python3 -I "$T/srm_console.py" --port "$port" --wake --log "$D/resume-console.log" \
    --cmd-log "$D/resume-cmds.txt" --timeout "$PTMO" --cmd-timeout "$PTMO" \
    --pid "$PID" --after sigterm --exit-timeout 60 "${cmds[@]}" \
    > "$D/resume.status" 2>&1
  wait_pid "$PID" 60; PID=
  rc3=$RC
  normalise "$D" < "$D/resume-cmds.txt" > "$OUT/$b.resume.log" 2>/dev/null
  st=$(tr '\n' ' ' < "$D/resume.status")
  secs=$(( $(date +%s) - t0 ))
  why=
  [ "$same" = 0 ] || why="the state files differ ($(grep -c ' bytes differ' "$OUT/$b.cmp") parts, $OUT/$b.cmp)"
  case $st in
  *status=prompt*no-prompt-after*) why="${why:+$why; }after the restore a command did not return: $st" ;;
  *status=prompt*)
    diff -a -q "$BOUT/$b.cmds.log" "$OUT/$b.resume.log" > /dev/null ||
      why="${why:+$why; }the commands' output after the restore differs from before" ;;
  *) why="${why:+$why; }no prompt after the restore: $st" ;;
  esac
  [ "$rc3" = 0 ] || why="${why:+$why; }exit status $rc3 after the restore"
  if [ -z "$why" ]; then
    report "$b" PASS $secs "state files $(head -1 "$OUT/$b.cmp"); the console answers after the restore"
  else
    report "$b" FAIL $secs "$why"
    sed 's/^/    /' "$OUT/$b.cmp" | head -20
    rc_all=1
  fi
  if [ "${SNAP_KEEP:-0}" != 1 ]; then
    rm -f "$D"/snap1.axp "$D"/snap2.axp "$D"/*.tmp "$D"/*-decompressed.rom "$D"/decompressed.rom
    find "$D" -maxdepth 1 -type f \( -iname '*.exe' -o -iname '*.rom' -o -iname '*.ROM' \) \
      ! -name flash.rom ! -name dpr.rom -delete
  fi
done
exit $rc_all
