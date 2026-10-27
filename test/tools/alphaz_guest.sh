#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build AlphaZ (guest/alphaz) and run it in emulated guests, one at a time,
# each on a clone of its disk: its values, read back as text, checked
# against the configuration the guest ran with, and on Windows one PNG per
# tab of its window.
#
# usage: alphaz_guest.sh <alphabox-binary> <label> <guest>...
#        alphaz_guest.sh <alphabox-binary> <label> --recheck <guest>...
#          (a finished run's Linux console logs read and checked again)
#   w2k-ev68cb  Windows 2000 on the ES40 (lab/win2k-installed,
#               es40-window-smp.cfg): its two ev68cb processors
#   w2k-ev8     the same with two `ev8` processors, w2k-ev7 and w2k-ev7z
#               with `ev7` and `ev7z`: ALPHABOX_CPU_ANY_BOARD=1, a test
#               hook that lets the ES40 take another family's row
#   xp64        Windows XP 64-bit (lab/xp64-installed, es40-window.cfg; its
#               Windows volume is D:, the FAT at byte 209731584, C: being the
#               ARC system partition), with ALPHAZ64.EXE: Whistler 2210
#               loads no 32-bit Alpha program (PE32, 0x184)
#   linux       guests_check.sh's `linux` (Gentoo on the ES40, one ev68cb)
#   es47-ev7, es47-ev7z, es47-ev8, es47-ev8t4
#               guests_check.sh's `linux-es47` with GUESTS_EV7_CPU ev7,
#               ev7z or ev8, and ev8 with GUESTS_EV8_TPUS=4
#
# Windows: \NADA on the Windows volume (C:, D: on xp64) is staged on the clone with mtools (nt_bench.sh's
# method): ALPHAZ.EXE, the guest's SHUTDOWN.EXE (ntbench/shutdown.c), and a
# RUN.BAT hooked from the All Users Startup folder, which runs
# `ALPHAZ.EXE /report C:\NADA\OUT.TXT`, then the window, then shuts
# Windows down, which flushes FAT. While the window is up this script
# takes a frame per tab: Ctrl+Tab through ALPHABOX_KEYPIPE moves to the
# next, PgDn shows the rest of the CPU and System pages, Esc closes it.
# alphaz_fb.py finds the window by its band's colour and waits for each
# page to finish drawing.
# Linux: alphaz-linux, gzip'd and in base64 lines of 2000 characters,
# through the serial console as guests_check.sh's GUESTS_EXTRA_CMDS, then
# run; the report is read back from the console log.
#
# Output in $ALPHABOX_WORK/runs/alphaz-<label>/ (ALPHABOX_WORK defaults to
# <repo>/lab): build/ (build.sh's, NADA-COMMIT.txt), and for each guest
# <guest>/report.txt, check.txt (each value against the configuration),
# tab-N-<name>.png, the guest's log and the build's NADA-COMMIT.txt;
# summary.txt, a PASS or FAIL line a guest, each run's lines added. Exit 0
# only if every guest passed.
#
# Environment: NADA (default ~/Documents/proj/nada), KEEP=1 to keep the
# clones, BOOT_TMO (seconds to the window, default 1200), AZ_PORT_BASE
# (default 24600: guests_check.sh's ports are the base plus the guest's
# place in its list). Before each boot the host must be at memory pressure
# level 1 and have 5 GB free; the script waits up to 30 minutes for that.
set -u
export LC_ALL=C MTOOLS_SKIP_CHECK=1
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
NADA=${NADA:-$HOME/Documents/proj/nada}
BOOT_TMO=${BOOT_TMO:-1200}
[ $# -ge 3 ] || { sed -n '2,40p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
shift 2
[ -x "$BIN" ] || { echo "alphaz_guest: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/alphaz-$LABEL
mkdir -p "$OUT" || exit 2
echo "# $(date "+%Y-%m-%d %H:%M") $*" >> "$OUT/summary.txt" # a run adds its lines
FB="python3 -I $T/alphaz_fb.py"
PID=

stop_emu() {
  [ -n "$PID" ] || return 0
  kill "$PID" 2>/dev/null # our own emulator only
  local i
  for i in $(seq 1 15); do kill -0 "$PID" 2>/dev/null || break; sleep 1; done
  kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null
  PID=
}
trap stop_emu EXIT
trap 'exit 130' INT TERM

# the host's load averages, for reading a clock that disagrees: a guest's
# own timers fall behind the host's time when the host is starved
host_load() { uptime | sed 's/.*load averages*: //'; }

report() { # report <guest> <PASS|FAIL> <what>
  echo "$2 $1 $3" | tee -a "$OUT/summary.txt"
}

# the host must be quiet enough: memory pressure 1 and 5 GB free
host_ready() {
  local i=0 lvl free_kb
  while [ $i -lt 60 ]; do
    lvl=$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)
    free_kb=$(df -k "$WORK" | awk 'NR==2 {print $4}')
    [ "$lvl" = 1 ] && [ "${free_kb:-0}" -ge $((5 * 1048576)) ] && return 0
    [ $i = 0 ] && echo "   waiting for the host: pressure level $lvl, $((free_kb / 1048576)) GB free"
    sleep 30
    i=$((i + 1))
  done
  return 1
}

# --- build -------------------------------------------------------------------
B=$OUT/build
build() {
  OUT=$B NADA=$NADA "$R/guest/alphaz/build.sh" > "$OUT/build.log" 2>&1 ||
    { cat "$OUT/build.log"; echo "alphaz_guest: build failed"; return 1; }
  tail -2 "$OUT/build.log"
  local L=$NADA/lib
  local t exe
  # SHUTDOWN64.EXE for xp64, which loads no 32-bit Alpha program
  for t in alpha-windows alpha64-windows; do
    exe=SHUTDOWN.EXE; [ $t = alpha64-windows ] && exe=SHUTDOWN64.EXE
    "$NADA/nada" -t $t -o "$B/$exe" -I "$NADA/include" \
      "$T/ntbench/shutdown.c" "$T/ntbench/user32.def" "$T/ntbench/advapi32.def" \
      "$L/windows.c" "$L/printf.c" "$L/stdio.c" "$L/scanf.c" "$L/string.c" "$L/stdlib.c" \
      "$L/ctype.c" "$L/math.c" "$L/fltconv.c" "$L/time.c" "$L/setjmp.c" "$L/windows/kernel32.def" ||
      { echo "alphaz_guest: $exe failed"; return 1; }
  done
}

# the configuration's values for check: expect <cfg> <os> [ev8 cases]
expect() {
  local cfg=$1 os=$2 cpu n speed bits row
  cpu=$(sed -n 's/^ *cpu0 *= *\([a-z0-9]*\).*/\1/p' "$cfg" | head -1)
  n=$(grep -cE '^ *cpu[0-9]+ *= *[a-z]' "$cfg")
  speed=$(grep -o 'speed *= *[0-9]*M' "$cfg" | head -1 | grep -o '[0-9]*')
  bits=$(sed -n 's/^ *memory\.bits *= *\([0-9]*\).*/\1/p' "$cfg" | head -1)
  tpus=$(grep -o 'tpus *= *[0-9]*' "$cfg" | head -1 | grep -o '[0-9]*$')
  # on the EV7 boards memory.bits is each processor chip's memory
  # (docs/configuration.md, "Memory"); elsewhere the machine's
  local mem=$(( (1 << bits) / 1024 ))
  grep -qE 'platform *= *"(es47|es80|gs1280)"' "$cfg" && mem=$((mem * n))
  [ -n "$tpus" ] && n=$((n * tpus))
  row=$($FB row "$R/src" "$cpu") # implver amask type minor
  set -- $row
  echo "os=$os implver=$1 amask=$2 cpus=$n speed=$((speed * 1000000)) mem_kb=$mem"
}

# --- Windows -------------------------------------------------------------------
# focus_click <frame> <keypipe>: the PS/2 mouse to the middle of the band
# AlphaZ's frame shows, and a left click there
focus_click() {
  local frame=$1 keys=$2 box x y h dx dy
  box=$($FB band "$frame" | sed -n 's/^band \([0-9]*\),\([0-9]*\)-\([0-9]*\),\([0-9]*\).*/\1 \2 \3 \4/p')
  [ -n "$box" ] || return 1
  set -- $box
  x=$(( ($1 + $3) / 2 )); y=$(( ($2 + $4) / 2 ))
  h=$(sed -n 's/.*-[0-9]*x\([0-9]*\)\.ppm$/\1/p' <<< "$frame")
  echo "mouse:-3000:3000" >> "$keys"
  sleep 8
  dx=$x; dy=$(( ${h:-480} - 1 - y ))
  while [ $dx -gt 0 ] || [ $dy -gt 0 ]; do
    echo "mouse:$(( dx > 4 ? 4 : dx )):-$(( dy > 4 ? 4 : dy ))" >> "$keys"
    dx=$(( dx > 4 ? dx - 4 : 0 )); dy=$(( dy > 4 ? dy - 4 : 0 ))
  done
  echo "mouse:0:0:1 mouse:0:0:0" >> "$keys"
  sleep 25
}

# win_guest <guest> <cpu row> [install dir] [cfg] [the Windows volume's
# byte offset] [its drive letter] [64]: the installed Windows in
# $WORK/<install dir> (default win2k-installed, es40-window-smp.cfg, C: at
# 16384), its ev68cb rows made <cpu row>; with 64, the AXP64 builds
# (ALPHAZ64.EXE, SHUTDOWN64.EXE) go on the guest under the usual names
win_guest() {
  local g=$1 cpu=$2 inst=${3:-win2k-installed} cfg=${4:-es40-window-smp.cfg} off=${5:-16384}
  local bits=${7:-}
  local W="${6:-C}:\NADA" # where the guest sees \NADA (%s keeps the backslash)
  local D=$OUT/$1 run=$WORK/runs/alphaz-$LABEL-$1-run
  local IMG any= f ref i n name names done_at start rows load0
  rm -rf "$D" "$run"; mkdir -p "$D"
  cp "$B/NADA-COMMIT.txt" "$D/" # the build this guest ran
  cp -c -R "$WORK/$inst" "$run" || { report "$g" FAIL "no $WORK/$inst"; return 1; }
  rm -rf "$run/fb" "$run"/fb-*.ppm "$run"/peek.img
  rows=$(grep -c "= ev68cb\$" "$run/$cfg")
  if [ "$cpu" != ev68cb ]; then
    sed -i '' "s/= ev68cb\$/= $cpu/" "$run/$cfg"
    [ "$(grep -c "= $cpu\$" "$run/$cfg")" = "$rows" ] || { report "$g" FAIL "cpu rows not swapped"; return 1; }
    any=1
  fi
  cp "$run/$cfg" "$D/$cfg"
  IMG="$run/disk0.img@@$off"
  local S=$run/stage
  mkdir -p "$S"
  cp "$B/ALPHAZ$bits.EXE" "$S/ALPHAZ.EXE" && cp "$B/SHUTDOWN$bits.EXE" "$S/SHUTDOWN.EXE" ||
    { report "$g" FAIL "no ALPHAZ$bits.EXE or SHUTDOWN$bits.EXE in $B"; return 1; }
  {
    printf '%s\r\n' '@echo off' \
      "if exist $W\\RAN goto over" \
      "echo ran > $W\\RAN" \
      "start /wait $W\\ALPHAZ.EXE /report $W\\OUT.TXT" \
      "start /wait $W\\ALPHAZ.EXE /close 900 /keylog $W\\KEYS.TXT" \
      "echo DONE >> $W\\OUT.TXT" \
      ':over' \
      "$W\\SHUTDOWN.EXE"
  } > "$S/RUN.BAT"
  printf '%s\r\n' '@echo off' "if exist $W\\RUN.BAT call $W\\RUN.BAT" > "$S/NADARUN.BAT"
  mmd -i "$IMG" ::/NADA 2>/dev/null
  for f in "$S"/*; do mcopy -o -i "$IMG" "$f" "::/NADA/$(basename "$f")" || { report "$g" FAIL "mcopy $f"; return 1; }; done
  local hooked=0 p
  for p in "::/DOCUME~1/ALLUSE~1/STARTM~1/PROGRAMS/STARTUP" "::/DOCUME~1/ALLUSE~1.WIN/STARTM~1/PROGRAMS/STARTUP"; do
    mdir -i "$IMG" "$p" >/dev/null 2>&1 && mcopy -o -i "$IMG" "$S/NADARUN.BAT" "$p/NADARUN.BAT" && hooked=$((hooked + 1))
  done
  [ "$hooked" -gt 0 ] || { report "$g" FAIL "no Startup folder on C:"; return 1; }
  host_ready || { report "$g" FAIL "the host stayed busy (memory pressure or disk)"; return 1; }

  echo "== $g: $inst, $rows $cpu, startup hooks $hooked"
  ( cd "$run" && mkdir -p fb && : > keys.txt &&
    exec env ${any:+ALPHABOX_CPU_ANY_BOARD=1} SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb \
      ALPHABOX_KEYPIPE=keys.txt "$BIN" run "$cfg" ) > "$run/run.log" 2>&1 &
  PID=$!
  load0=$(host_load)
  start=$(date +%s)
  ref=$($FB window "$run/fb" "$BOOT_TMO" "$PID")
  if [ "$ref" = none ]; then
    f=$(ls -t "$run/fb" 2>/dev/null | grep -m1 ppm)
    [ -n "$f" ] && python3 -I "$T/ppm2png.py" "$run/fb/$f" "$D/last.png"
    stop_emu; cp "$run/run.log" "$D/run.log"
    report "$g" FAIL "no AlphaZ window within ${BOOT_TMO}s (last frame: $D/last.png)"
    return 1
  fi
  echo "   window up after $(( $(date +%s) - start ))s"
  # let the first page settle, then a frame a tab
  f=$($FB change "$run/fb" "$ref" 6 "$PID"); [ "$f" != same ] && ref=$f
  names="CPU Caches System Bench About"
  [ "$cpu" = ev8 ] || [ "$cpu" = ev8n ] && names="CPU Caches System EV8 Bench About"
  i=0
  for name in $names; do
    python3 -I "$T/ppm2png.py" "$ref" "$D/tab-$i-$name.png"
    echo "   tab $i $name: $($FB band "$ref")"
    if [ "$name" = CPU ] || [ "$name" = System ]; then
      echo pgdn >> "$run/keys.txt"
      f=$($FB change "$run/fb" "$ref" 12 "$PID")
      if [ "$f" != same ]; then python3 -I "$T/ppm2png.py" "$f" "$D/tab-$i-$name-2.png"; ref=$f; fi
    fi
    i=$((i + 1))
    [ "$i" -lt "$(echo $names | wc -w | tr -d " ")" ] || break
    echo ctrl-tab >> "$run/keys.txt"
    f=$($FB change "$run/fb" "$ref" 20 "$PID")
    if [ "$f" = same ] && [ "$i" = 1 ]; then
      # the keys went elsewhere: click the window's band to give it the
      # focus, from the bottom left corner in steps small enough that
      # Windows' pointer acceleration leaves them 1:1, and try again
      echo "   Ctrl+Tab changed nothing: clicking the window"
      focus_click "$ref" "$run/keys.txt"
      echo ctrl-tab >> "$run/keys.txt"
      f=$($FB change "$run/fb" "$ref" 20 "$PID")
    fi
    [ "$f" = same ] && { echo "   Ctrl+Tab changed nothing"; break; }
    ref=$f
  done
  n=$i
  echo esc >> "$run/keys.txt"
  $FB gone "$run/fb" 30 "$PID" > /dev/null
  # DONE in OUT.TXT, read off a clone of the live disk, or SRM's 720x400
  # frame after the reset that follows the shutdown
  done_at=0
  while kill -0 "$PID" 2>/dev/null && [ $(( $(date +%s) - start )) -lt $((BOOT_TMO + 600)) ]; do
    rm -f "$run/peek.img"
    if cp -c "$run/disk0.img" "$run/peek.img" 2>/dev/null &&
       mtype -i "$run/peek.img@@$off" ::/NADA/OUT.TXT 2>/dev/null | grep -q DONE; then
      done_at=$(( $(date +%s) - start )); break
    fi
    case "$(ls -t "$run/fb" | grep -m1 ppm)" in *720x400*) done_at=$(( $(date +%s) - start )); break ;; esac
    sleep 5
  done
  rm -f "$run/peek.img"
  stop_emu
  cp "$run/run.log" "$D/run.log"
  # the guest writes its ANSI code page, Windows-1252
  mtype -i "$IMG" ::/NADA/OUT.TXT 2>/dev/null | tr -d '\r' | iconv -f CP1252 -t UTF-8 > "$D/report.txt"
  mtype -i "$IMG" ::/NADA/KEYS.TXT 2>/dev/null | tr -d '\r' > "$D/keys-seen.txt"
  grep -a "ANYBOARD\|ANY_BOARD" "$run/run.log" | head -2
  [ "${KEEP:-0}" = 1 ] || rm -rf "$run"
  if ! grep -q "^== CPU" "$D/report.txt"; then
    report "$g" FAIL "no report in $W\\OUT.TXT ($n tabs shot)"
    return 1
  fi
  $FB check "$D/report.txt" $(expect "$D/$cfg" windows) ${ev8_cases:+ev8=$ev8_cases} > "$D/check.txt"
  local rc=$?
  echo "info host load averages   at the boot $load0, at the end $(host_load)" >> "$D/check.txt"
  cat "$D/check.txt" | sed 's/^/   /'
  if [ $rc = 0 ] && [ "$n" -ge "$(echo $names | wc -w | tr -d " ")" ]; then
    report "$g" PASS "$n tabs shot, every value as configured (finished after ${done_at}s)"
  else
    report "$g" FAIL "$n of $(echo $names | wc -w | tr -d " ") tabs shot, $(grep -c '^DIFF' "$D/check.txt") values differ"; return 1
  fi
}

# --- Linux ---------------------------------------------------------------------
linux_guest() { # linux_guest <guest> <guests_check guest> [cpu] [tpus]
  local g=$1 gc=$2 cpu=${3:-} tpus=${4:-} D=$OUT/$1 b64 cmds n i rc
  rm -rf "$D"; mkdir -p "$D"
  cp "$B/NADA-COMMIT.txt" "$D/" # the build this guest ran
  gzip -9n < "$B/alphaz-linux" > "$D/alphaz.gz"
  b64=$(base64 < "$D/alphaz.gz" | tr -d '\n')
  cmds="rm -f /tmp/az.b64 /tmp/alphaz"
  n=${#b64}; i=0
  while [ $i -lt $n ]; do cmds="$cmds;echo ${b64:$i:2000} >> /tmp/az.b64"; i=$((i + 2000)); done
  cmds="$cmds;base64 -d /tmp/az.b64 | gunzip > /tmp/alphaz;sha256sum /tmp/alphaz;chmod +x /tmp/alphaz;/tmp/alphaz"
  host_ready || { report "$g" FAIL "the host stayed busy (memory pressure or disk)"; return 1; }
  echo "== $g: guests_check.sh $gc${cpu:+, $cpu}${tpus:+, tpus $tpus}, $(( n / 2000 + 1 )) lines of base64"
  # the firmware is git-ignored: a worktree has none of its own
  local roms=${ALPHABOX_ROMS:-$R/roms}
  [ -d "$roms" ] || roms=$(cd "$(git -C "$R" rev-parse --git-common-dir)/.." && pwd)/roms
  # env, so that the optional NAME=value words count as assignments
  env ALPHABOX_ROMS="$roms" GUESTS_EXTRA_CMDS="$cmds" GUESTS_PORT_BASE="${AZ_PORT_BASE:-24600}" GUESTS_OUT="$D/gc" \
    GUESTS_KEEP=1 ${cpu:+GUESTS_EV7_CPU=$cpu} ${tpus:+GUESTS_EV8_TPUS=$tpus} \
    "$T/guests_check.sh" "$BIN" "alphaz-$LABEL-$g" "$gc" > "$D/guests_check.out" 2>&1 &
  local gpid=$! load0
  load0=$(host_load)
  wait $gpid
  rc=$?
  tail -1 "$D/guests_check.out" | sed 's/^/   guests_check: /'
  echo "boot $load0" > "$D/host-load.txt"
  cp "$D/gc/$gc/es40.cfg" "$D/es40.cfg" 2>/dev/null
  rm -f "$D/gc/$gc/disk.img" "$D/gc/$gc"/*.rom "$D/gc/$gc"/*.exe
  linux_check "$g" "$gc" "$rc"
}

# linux_check <guest> <guests_check guest> <its rc>: the report out of the
# console log, checked; also run alone by --recheck on a finished run
linux_check() {
  local g=$1 gc=$2 rc=$3 D=$OUT/$1 ok_sha=no exp model=EV7 crc load0
  load0=$(sed -n 's/^boot //p' "$D/host-load.txt" 2>/dev/null)
  # the shell's terminal escapes (bracketed paste) out, then the report
  tr -d '\r' < "$D/gc/$gc.log" | sed -e $'s/\033\\[[?0-9;]*[a-zA-Z]//g' > "$D/console.txt"
  sed -n '/^ALPHAZ-REPORT-BEGIN/,/^ALPHAZ-REPORT-END/p' "$D/console.txt" | sed '1d;$d' > "$D/report.txt"
  grep -a -m1 -E '^[0-9a-f]{64}  /tmp/alphaz' "$D/console.txt" > "$D/sha256-guest.txt"
  if ! grep -q "^== CPU" "$D/report.txt"; then
    report "$g" FAIL "no report in the console log (guests_check rc $rc)"
    return 1
  fi
  [ "$(cut -d' ' -f1 "$D/sha256-guest.txt")" = "$(shasum -a 256 "$B/alphaz-linux" | cut -d' ' -f1)" ] && ok_sha=yes
  exp=$(expect "$D/es40.cfg" linux)
  case "$exp" in *implver=2*) model=EV68CB ;; esac
  $FB check "$D/report.txt" $exp model=$model ${ev8_cases:+ev8=$ev8_cases} > "$D/check.txt"
  crc=$?
  echo "info host load averages   at the boot ${load0:-not recorded}, at the check $(host_load)" >> "$D/check.txt"
  sed 's/^/   /' "$D/check.txt"
  if [ $crc = 0 ] && [ $ok_sha = yes ]; then
    report "$g" PASS "every value as configured, binary sha256 matches (guests_check rc $rc)"
  else
    report "$g" FAIL "$(grep -c '^DIFF' "$D/check.txt") values differ, sha256 match $ok_sha (guests_check rc $rc)"; return 1
  fi
}

# Everything runs from main, called on the file's last line, so bash has
# read the whole script before the first guest boots: an edit to it while
# guests run cannot change what they do.
main() {
  local g rc_all=0 ev8_n
  # the EV8 page's cases, from az_ev8.c
  ev8_n=$(grep -cE '^ +add_case\(' "$R/guest/alphaz/az_ev8.c")
  if [ "${1:-}" = --recheck ]; then
    # a finished run's Linux logs read and checked again, nothing booted
    shift
    for g in "$@"; do
      ev8_cases=
      case $g in es47-ev8*) ev8_cases=$ev8_n ;; esac
      case $g in
      linux) linux_check "$g" linux "-" ;;
      es47-*) linux_check "$g" linux-es47 "-" ;;
      *) report "$g" FAIL "--recheck reads Linux guests only"; false ;;
      esac || rc_all=1
    done
    return $rc_all
  fi
  build || return 2
  for g in "$@"; do
    ev8_cases=
    case $g in
    w2k-ev68cb) win_guest "$g" ev68cb ;;
    w2k-ev7) win_guest "$g" ev7 ;;
    w2k-ev7z) win_guest "$g" ev7z ;;
    w2k-ev8) ev8_cases=$ev8_n; win_guest "$g" ev8 ;;
    xp64) win_guest "$g" ev68cb xp64-installed es40-window.cfg 209731584 D 64 ;;
    linux) linux_guest "$g" linux ;;
    es47-ev7) linux_guest "$g" linux-es47 ev7 ;;
    es47-ev7z) linux_guest "$g" linux-es47 ev7z ;;
    es47-ev8) ev8_cases=$ev8_n; linux_guest "$g" linux-es47 ev8 ;;
    es47-ev8t4) ev8_cases=$ev8_n; linux_guest "$g" linux-es47 ev8 4 ;;
    *) report "$g" FAIL "unknown guest"; false ;;
    esac || rc_all=1
  done
  return $rc_all
}

main "$@"; exit $?
