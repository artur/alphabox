#!/bin/bash
# Run a real Windows NT application benchmark in a guest, unattended, and read
# the numbers back off its disk.
#
# usage: nt_bench.sh <label> <alphabox-binary> <install-dir> <cfg> [workload] [section] [scale]
#   workload  axp (default) | js | cab
#     axp   axpbench.exe, compiled here by nada for -t alpha-windows. Sections:
#           alu branch call ldst stride fp byte div sort, or all (default).
#           Each isolates one JIT datapath and times itself.
#     js    JScript under cscript: int fp str arr obj, or all -- Microsoft's
#           own optimized code (jscript.dll) with a real working set.
#     cab   makecab LZX over 8 MB: integer-heavy, large working set.
#   scale     multiplier on the section sizes (default 1)
# Environment: NADA=<dir> (default ~/Documents/proj/nada), TIMEOUT=<s>,
#   KEEP=1 to keep the clone, STAGE_ONLY=1 to build and stage C: and stop
#   before booting (for checking the harness without occupying the machine).
#
# Why this exists. The workload we had was `cmd /c for /l ... do @rem`: a real
# NT process, but integer-only with a handful of hot code pages, which runs at
# 100% native coverage and cannot show pressure on the FP path, the address
# path, the block cache or calls. And it was timed from outside, by watching a
# console window open and close, which resolves about 2 s. Here the guest times
# itself and writes the numbers to a file we read afterwards.
#
# How the guest is driven, with no keyboard injection at all: C: is FAT16 at
# byte 16384 of the disk, so mtools can write to a CLONE of the image while the
# machine is off. We drop the binaries and a RUN.BAT into C:\NADA, hook it from
# the All Users Startup folder, and let the login run it. It writes its output
# to C:\NADA\OUT.TXT and shuts Windows down, which is what flushes FAT. The
# machine then resets to SRM, whose console is 720x400 where Windows draws
# 640x480 -- a 720x400 frame after 640x480 ones is how we know it finished.
#
# The disk trick, the Startup hook, the shutdown helper and the geometry
# detection are all lifted from nada's tests/run-ntalpha.sh, which got there
# first; this is the benchmark-shaped version of it. The geometry rule is the
# fragile half, so it is backed by a DONE check against a clone of the running
# disk and by an overall timeout: a slow shutdown must not read as a hang.
set -u
export LC_ALL=C MTOOLS_SKIP_CHECK=1
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
[ $# -ge 4 ] || { sed -n '2,16p' "$0"; exit 2; }
LABEL=$1 BIN=$2 INST=$3 CFG=$4
WORKLOAD=${5:-axp} SECTION=${6:-all} SCALE=${7:-1}
NADA=${NADA:-$HOME/Documents/proj/nada}
TIMEOUT=${TIMEOUT:-1800}
case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
[ -x "$BIN" ] || { echo "nt_bench: $BIN is not executable"; exit 2; }
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -f "$SRC/$CFG" ] || { echo "nt_bench: $SRC/$CFG missing"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "nt_bench: memory pressure critical, not starting"; exit 2
fi

D=$WORK/ntbench-$LABEL
rm -rf "$D"
cp -c -R "$SRC" "$D" 2>/dev/null || cp -R "$SRC" "$D" || exit 2
IMG="$D/disk0.img@@16384"
[ "${KEEP:-0}" = 1 ] || trap 'rm -rf "$D"' EXIT

# --- build the guest-side programs -----------------------------------------
STAGE=$(mktemp -d) || exit 2
# The OS layer (lib/windows.c) carries the entry stub, and the portable half
# of the libc is separate files -- linking windows.c alone leaves $printf and
# $_start undefined. This is nada's own alpha-windows row from test.sh.
nada_build() { # <out> <source-or-def>...
  local out=$1; shift
  "$NADA/nada" -t alpha-windows -o "$out" -I "$NADA/include" "$@" \
    "$NADA/lib/windows.c" "$NADA/lib/printf.c" "$NADA/lib/stdio.c" \
    "$NADA/lib/scanf.c" "$NADA/lib/string.c" "$NADA/lib/stdlib.c" \
    "$NADA/lib/ctype.c" "$NADA/lib/math.c" "$NADA/lib/fltconv.c" \
    "$NADA/lib/time.c" "$NADA/lib/setjmp.c" "$NADA/lib/windows/kernel32.def"
}
if [ "$WORKLOAD" = axp ]; then
  [ -x "$NADA/nada" ] || { echo "nt_bench: no nada at $NADA"; exit 2; }
  nada_build "$STAGE/AXPBENCH.EXE" "$T/ntbench/axpbench.c" || exit 2
fi
# The shutdown helper is what flushes FAT: without it the results never reach
# the image. Vendored here (ntbench/shutdown.c) rather than referenced out of
# nada's tree, so neither repository depends on a file the other's tests are
# the ones exercising.
[ -x "$NADA/nada" ] || { echo "nt_bench: no nada at $NADA"; exit 2; }
nada_build "$STAGE/SHUTDOWN.EXE" "$T/ntbench/shutdown.c" \
  "$T/ntbench/user32.def" "$T/ntbench/advapi32.def" ||
  { echo "nt_bench: could not build SHUTDOWN.EXE"; exit 2; }

# --- stage C:\NADA ----------------------------------------------------------
case "$WORKLOAD" in
  axp) CMD="C:\\NADA\\AXPBENCH.EXE $SECTION $SCALE" ;;
  js)  cp "$T/ntbench/jsbench.js" "$STAGE/JSBENCH.JS"
       CMD="cscript //nologo C:\\NADA\\JSBENCH.JS $SCALE $SECTION" ;;
  cab) python3 - "$STAGE/CABIN.BIN" <<'PY'
import sys, random
random.seed(1234)                    # same bytes every run: the input is not
w = [bytes(random.randrange(33, 127) for _ in range(random.randrange(3, 12)))
     for _ in range(4096)]           # part of what is being measured
o = bytearray()
while len(o) < 8 << 20: o += random.choice(w) + b' '
open(sys.argv[1], 'wb').write(bytes(o[:8 << 20]))
PY
       CMD="makecab /D CompressionType=LZX /D CompressionMemory=21 C:\\NADA\\CABIN.BIN C:\\NADA\\OUT.CAB" ;;
  *) echo "nt_bench: unknown workload $WORKLOAD"; exit 2 ;;
esac

# RUN.BAT self-marks so a second boot does not run it again, times the whole
# thing in the guest's own clock, and shuts down however the workload ended.
{
  printf '@echo off\r\n'
  printf 'if exist C:\\NADA\\RAN goto over\r\n'
  printf 'echo ran > C:\\NADA\\RAN\r\n'
  printf 'echo alphabox nt_bench %s %s %s > C:\\NADA\\OUT.TXT\r\n' "$WORKLOAD" "$SECTION" "$SCALE"
  printf 'echo START %%TIME%% >> C:\\NADA\\OUT.TXT\r\n'
  printf '%s >> C:\\NADA\\OUT.TXT 2>&1\r\n' "$CMD"
  printf 'echo END %%TIME%% >> C:\\NADA\\OUT.TXT\r\n'
  printf 'echo DONE >> C:\\NADA\\OUT.TXT\r\n'
  printf ':over\r\n'
  printf 'C:\\NADA\\SHUTDOWN.EXE\r\n'
} > "$STAGE/RUN.BAT"
printf '@echo off\r\nif exist C:\\NADA\\RUN.BAT call C:\\NADA\\RUN.BAT\r\n' > "$STAGE/NADARUN.BAT"

mmd -i "$IMG" ::/NADA 2>/dev/null
for f in "$STAGE"/*; do
  mcopy -o -i "$IMG" "$f" "::/NADA/$(basename "$f")" || { echo "nt_bench: mcopy $f failed"; exit 2; }
done
# Windows 2000 may keep either All Users profile active; hook both.
hooked=0
for p in "::/DOCUME~1/ALLUSE~1/STARTM~1/PROGRAMS/STARTUP" \
         "::/DOCUME~1/ALLUSE~1.WIN/STARTM~1/PROGRAMS/STARTUP"; do
  if mdir -i "$IMG" "$p" >/dev/null 2>&1; then
    mcopy -o -i "$IMG" "$STAGE/NADARUN.BAT" "$p/NADARUN.BAT" && hooked=$((hooked + 1))
  fi
done
rm -rf "$STAGE"
[ "$hooked" -gt 0 ] || { echo "nt_bench: no Startup folder found on C:"; exit 2; }
echo "== $LABEL: $WORKLOAD $SECTION x$SCALE, startup hooks: $hooked"
if [ "${STAGE_ONLY:-0}" = 1 ]; then
  echo "   staged, not booting (STAGE_ONLY=1):"
  mdir -i "$IMG" ::/NADA | sed 's/^/     /'
  exit 0
fi

# --- boot, and wait for Windows to hand the machine back to SRM -------------
cd "$D" || exit 2
rm -rf fb && mkdir -p fb
env SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb "$BIN" run "$CFG" > run.log 2>&1 &
P=$!
echo "   pid $P, $(basename "$BIN")"
# Two independent end conditions, because neither alone is trustworthy. The
# geometry rule (SRM's console is 720x400, Windows draws 640x480, so a 720x400
# frame after 640x480 ones is the reset that follows shutdown) is specific to a
# machine that resets rather than powering off. The DONE check reads OUT.TXT
# off a COPY of the live image -- cp -c is a copy-on-write clone, so this costs
# nothing and never touches the file the emulator has open.
start=$(date +%s)
saw_win=0 done_at=0
guest_done() {
  rm -f peek.img
  cp -c "$D/disk0.img" peek.img 2>/dev/null || return 1
  mtype -i "peek.img@@16384" ::/NADA/OUT.TXT 2>/dev/null | grep -q DONE
}
while kill -0 $P 2>/dev/null; do
  now=$(date +%s)
  [ $((now - start)) -gt "$TIMEOUT" ] && { echo "   timeout after ${TIMEOUT}s"; break; }
  last=$(ls -t fb 2>/dev/null | grep -m1 ppm)
  case "$last" in
    *640x480*) saw_win=1 ;;
    *720x400*)
      if [ "$saw_win" = 1 ]; then done_at=$((now - start)); break; fi ;;
  esac
  # Independent of the screen: the guest says so itself. DONE reaching the
  # image IS the flush -- Windows only writes OUT.TXT out when it shuts down --
  # so there is nothing left to wait for and we stop immediately. That is worth
  # ~45 s a run: the whole SRM reset afterwards is time we used to sit through.
  if [ "$saw_win" = 1 ] && guest_done; then
    done_at=$((now - start))
    echo "   guest wrote DONE after ${done_at}s (results already flushed)"
    break
  fi
  sleep 3
done
rm -f peek.img
[ "$done_at" -gt 0 ] && echo "   guest finished and reset to SRM after ${done_at}s"
kill $P 2>/dev/null # our own emulator only
for i in $(seq 1 10); do kill -0 $P 2>/dev/null || break; sleep 1; done
kill -0 $P 2>/dev/null && { echo "   still up after SIGTERM, forcing"; kill -9 $P 2>/dev/null; }
wait $P 2>/dev/null

# --- results ----------------------------------------------------------------
echo "--- C:\\NADA\\OUT.TXT ---"
if mtype -i "$IMG" ::/NADA/OUT.TXT 2>/dev/null | tr -d '\r'; then :; else
  echo "   (no OUT.TXT: the guest never ran RUN.BAT)"
  last=$(ls -t fb 2>/dev/null | grep -m1 ppm)
  [ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" last.png &&
    echo "   last screen: $D/last.png"
  exit 1
fi
mtype -i "$IMG" ::/NADA/OUT.TXT 2>/dev/null | grep -q DONE || { echo "   (incomplete)"; exit 1; }
exit 0
