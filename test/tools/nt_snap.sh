#!/bin/bash
# The NT benchmark without the boot: snapshot a logged-in desktop once, then
# resume it per run.
#
#   nt_snap.sh make <alphabox-binary> <install-dir> <cfg>
#       Clone the install, stage C:\NADA (the benchmark PEs and a RUN.BAT that
#       takes its workload as arguments), boot to the desktop, and take a
#       snapshot with the machine quiesced: SIGUSR1 makes the emulator stop
#       every thread, save state, and exit -- so the disk image it leaves
#       behind is exactly what the snapshot saw. Result: $ALPHABOX_WORK/ntsnap.
#   nt_snap.sh run <label> <alphabox-binary> [workload] [section] [scale]
#       Clone ntsnap, resume it (ALPHABOX_RESTORE), type the run command into
#       Start > Run, wait for the guest to write DONE and read the numbers off
#       the disk. Prints the same lines as nt_bench.sh, so perf_ab.py can
#       consume either.
#
# Why: a cold boot is ~120 s of the ~165 s a benchmark run costs, and an A/B
# is four of them. A restore is seconds. The snapshot is taken by whichever
# binary made it and resumed by whichever is under test; that is sound while
# the two share the saved-state layout, and RestoreState refuses loudly
# ("STRUCT SIZE does not match") when they do not.
#
# Nothing is written to C: after the snapshot except by the guest itself:
# writing to a FAT volume behind a resumed OS would corrupt its cached FAT.
# That is why the benchmark files are staged BEFORE the snapshot and the run
# command carries its arguments instead.
set -u
export LC_ALL=C MTOOLS_SKIP_CHECK=1
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
NADA=${NADA:-$HOME/Documents/proj/nada}
SNAP=$WORK/ntsnap
VERB=${1:-}; shift || true

nada_build() { # <out> <source-or-def>...
  local out=$1; shift
  "$NADA/nada" -t alpha-windows -o "$out" -I "$NADA/include" "$@" \
    "$NADA/lib/windows.c" "$NADA/lib/printf.c" "$NADA/lib/stdio.c" \
    "$NADA/lib/scanf.c" "$NADA/lib/string.c" "$NADA/lib/stdlib.c" \
    "$NADA/lib/ctype.c" "$NADA/lib/math.c" "$NADA/lib/fltconv.c" \
    "$NADA/lib/time.c" "$NADA/lib/setjmp.c" "$NADA/lib/windows/kernel32.def"
}
abs() { echo "$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"; }
pressure_ok() { [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" != 4 ]; }

case "$VERB" in
make)
  [ $# -ge 3 ] || { sed -n '2,20p' "$0"; exit 2; }
  BIN=$(abs "$1"); INST=$2; CFG=$3
  case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
  [ -x "$BIN" ] && [ -f "$SRC/$CFG" ] || { echo "nt_snap: bad binary or cfg"; exit 2; }
  [ -x "$NADA/nada" ] || { echo "nt_snap: no nada at $NADA"; exit 2; }
  pressure_ok || { echo "nt_snap: memory pressure critical"; exit 2; }
  rm -rf "$SNAP"; cp -c -R "$SRC" "$SNAP" 2>/dev/null || cp -R "$SRC" "$SNAP" || exit 2
  IMG="$SNAP/disk0.img@@16384"
  STAGE=$(mktemp -d) || exit 2
  nada_build "$STAGE/AXPBENCH.EXE" "$T/ntbench/axpbench.c" "$T/ntbench/kernel32x.def" || exit 2
  nada_build "$STAGE/SHUTDOWN.EXE" "$T/ntbench/shutdown.c" "$T/ntbench/user32.def" "$T/ntbench/advapi32.def" || exit 2
  cp "$T/ntbench/jsbench.js" "$STAGE/JSBENCH.JS"
  python3 - "$STAGE/CABIN.BIN" <<'PY'
import sys, random
random.seed(1234)
w = [bytes(random.randrange(33, 127) for _ in range(random.randrange(3, 12))) for _ in range(4096)]
o = bytearray()
while len(o) < 8 << 20: o += random.choice(w) + b' '
open(sys.argv[1], 'wb').write(bytes(o[:8 << 20]))
PY
  # RUN.BAT takes <workload> <section> <scale>; no Startup hook, no self-mark.
  {
    printf '@echo off\r\n'
    printf 'set W=%%1\r\nif "%%W%%"=="" set W=axp\r\n'
    printf 'set S=%%2\r\nif "%%S%%"=="" set S=all\r\n'
    printf 'set N=%%3\r\nif "%%N%%"=="" set N=1\r\n'
    printf 'echo alphabox nt_snap %%W%% %%S%% %%N%% > C:\\NADA\\OUT.TXT\r\n'
    printf 'echo START %%TIME%% >> C:\\NADA\\OUT.TXT\r\n'
    printf 'if "%%W%%"=="js" goto js\r\nif "%%W%%"=="cab" goto cab\r\n'
    printf 'C:\\NADA\\AXPBENCH.EXE %%S%% %%N%% >> C:\\NADA\\OUT.TXT 2>&1\r\ngoto done\r\n'
    printf ':js\r\ncscript //nologo C:\\NADA\\JSBENCH.JS %%N%% %%S%% >> C:\\NADA\\OUT.TXT 2>&1\r\ngoto done\r\n'
    printf ':cab\r\nmakecab /D CompressionType=LZX /D CompressionMemory=21 C:\\NADA\\CABIN.BIN C:\\NADA\\OUT.CAB >> C:\\NADA\\OUT.TXT 2>&1\r\n'
    printf ':done\r\necho END %%TIME%% >> C:\\NADA\\OUT.TXT\r\necho DONE >> C:\\NADA\\OUT.TXT\r\n'
    printf 'C:\\NADA\\SHUTDOWN.EXE\r\n'
  } > "$STAGE/RUN.BAT"
  mmd -i "$IMG" ::/NADA 2>/dev/null
  for f in "$STAGE"/*; do mcopy -o -i "$IMG" "$f" "::/NADA/$(basename "$f")" || exit 2; done
  rm -rf "$STAGE"
  echo "== nt_snap make: staged C:\\NADA, booting $(basename "$BIN") to the desktop"
  cd "$SNAP" || exit 2
  rm -rf fb && mkdir -p fb
  env SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_SNAPSHOT="$SNAP/snap.axp" \
      ALPHABOX_SNAPSHOT_EXIT=1 "$BIN" run "$CFG" > make.log 2>&1 &
  P=$!
  t0=$(date +%s)
  until ls fb 2>/dev/null | grep -q 640x480; do
    kill -0 $P 2>/dev/null || { echo "nt_snap: emulator died during boot"; tail -5 make.log; exit 1; }
    [ $(( $(date +%s) - t0 )) -gt 600 ] && { echo "nt_snap: no Windows frame in 600 s"; kill $P; exit 1; }
    sleep 5
  done
  SETTLE=${SETTLE:-90}
  echo "   Windows up after $(( $(date +%s) - t0 )) s; settling ${SETTLE} s for login and Startup"
  sleep "$SETTLE"
  echo "   snapshotting (SIGUSR1)"
  kill -USR1 $P
  for i in $(seq 1 60); do kill -0 $P 2>/dev/null || break; sleep 1; done
  kill -0 $P 2>/dev/null && { echo "nt_snap: emulator did not exit after the snapshot"; kill $P; exit 1; }
  wait $P 2>/dev/null
  [ -s "$SNAP/snap.axp" ] || { echo "nt_snap: no snapshot written"; tail -5 make.log; exit 1; }
  # A snapshot is of ONE machine: the cfg that built it must be the cfg that
  # resumes it, or RestoreState meets a component the file never held (a
  # second CPU, say) and refuses. Record it beside the snapshot.
  echo "$CFG" > "$SNAP/snap.cfgname"
  rm -rf fb
  ls -l "$SNAP/snap.axp" | awk '{print "   snapshot:", $5, "bytes"}'
  echo "== nt_snap make: done -> $SNAP"
  ;;

run)
  [ $# -ge 2 ] || { sed -n '2,20p' "$0"; exit 2; }
  LABEL=$1; BIN=$(abs "$2"); WORKLOAD=${3:-axp}; SECTION=${4:-all}; SCALE=${5:-1}
  TIMEOUT=${TIMEOUT:-600}
  [ -x "$BIN" ] || { echo "nt_snap: $BIN is not executable"; exit 2; }
  [ -s "$SNAP/snap.axp" ] || { echo "nt_snap: no snapshot at $SNAP -- run 'nt_snap.sh make' first"; exit 2; }
  pressure_ok || { echo "nt_snap: memory pressure critical"; exit 2; }
  D=$WORK/ntsnap-$LABEL
  rm -rf "$D"; cp -c -R "$SNAP" "$D" 2>/dev/null || cp -R "$SNAP" "$D" || exit 2
  # RUNLOG=<path>: keep the emulator's log (e.g. its ALPHABOX_RATE lines)
  # after the clone is removed.
  [ "${KEEP:-0}" = 1 ] || trap '[ -n "${RUNLOG:-}" ] && mkdir -p "$(dirname "$RUNLOG")" && cp "$D/run.log" "$RUNLOG"; rm -rf "$D"' EXIT
  IMG="$D/disk0.img@@16384"
  cd "$D" || exit 2
  CFG=$(cat snap.cfgname 2>/dev/null)
  [ -n "$CFG" ] && [ -f "$CFG" ] || { echo "nt_snap: snapshot has no recorded cfg (snap.cfgname); remake it"; exit 2; }
  rm -rf fb && mkdir -p fb && : > keys.txt
  env SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys.txt \
      ALPHABOX_RESTORE="$D/snap.axp" "$BIN" run "$CFG" > run.log 2>&1 &
  P=$!
  echo "== $LABEL: resumed snapshot with $(basename "$BIN"): $WORKLOAD $SECTION x$SCALE (pid $P)"
  sleep "${RESUME_SETTLE:-8}"
  kill -0 $P 2>/dev/null || { echo "nt_snap: emulator died on restore"; tail -8 run.log; exit 1; }
  # Type the command into Start > Run. Forward slashes: the key injector has
  # no backslash, and Run accepts them.
  echo "win-r" >> keys.txt; sleep 3
  python3 -c "import sys; sys.path.insert(0,'$T'); import keys_for; print(' '.join(keys_for.tokens('c:/nada/run.bat $WORKLOAD $SECTION $SCALE')))" >> keys.txt
  sleep 4; echo "enter" >> keys.txt
  start=$(date +%s)
  guest_done() { rm -f peek.img; cp -c "$D/disk0.img" peek.img 2>/dev/null || return 1
                 mtype -i "peek.img@@16384" ::/NADA/OUT.TXT 2>/dev/null | grep -q DONE; }
  while kill -0 $P 2>/dev/null; do
    [ $(( $(date +%s) - start )) -gt "$TIMEOUT" ] && { echo "   timeout after ${TIMEOUT}s"; break; }
    if guest_done; then echo "   guest wrote DONE after $(( $(date +%s) - start ))s"; break; fi
    sleep 3
  done
  rm -f peek.img
  kill $P 2>/dev/null
  for i in $(seq 1 10); do kill -0 $P 2>/dev/null || break; sleep 1; done
  kill -0 $P 2>/dev/null && kill -9 $P 2>/dev/null
  wait $P 2>/dev/null
  echo "--- C:\\NADA\\OUT.TXT ---"
  if ! mtype -i "$IMG" ::/NADA/OUT.TXT 2>/dev/null | tr -d '\r'; then
    echo "   (no OUT.TXT: the run command never fired)"
    last=$(ls -t fb 2>/dev/null | grep -m1 ppm)
    [ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" last.png && echo "   last screen: $D/last.png"
    exit 1
  fi
  mtype -i "$IMG" ::/NADA/OUT.TXT 2>/dev/null | grep -q DONE || { echo "   (incomplete)"; exit 1; }
  [ "$WORKLOAD" = cab ] && echo "cab-sha $(mtype -i "$IMG" ::/NADA/OUT.CAB 2>/dev/null | shasum | cut -c1-40)"
  ;;
*) sed -n '2,20p' "$0"; exit 2 ;;
esac
