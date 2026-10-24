#!/bin/bash
# Boot every machine Alphabox emulates to its SRM console prompt.
#
# usage: boards_check.sh <alphabox-binary> <label> [board ...]
#        boards_check.sh --diff <labelA> <labelB> [board ...]
#
# With no boards: every row of src/platforms/Platforms.cpp, in this order:
#   es40 ds20e ds20 ds10 ds20l es45 es45m1 ds25 ds15 es47 es80 gs1280
# For each board it writes a minimal configuration (the board's ALi places,
# its own console firmware from roms/), starts the emulator, waits for the
# console prompt (with a timeout, never a fixed sleep), runs `show config`
# (and `show fru` on the EV7 boards), and stops the emulator with SIGTERM.
# A board passes only if the emulator then exits with status 0. After the
# boards, each EV7 board checked is booted again with more processors (es47
# 2, es80 and gs1280 4) for marvel_init_check.sh: `init` twice, and every
# processor restarted each time. Its lines are `PASS|FAIL <board>-init ...`.
#
# Firmware (never downloaded; roms/ is the git-ignored copy of media you own):
#   es40                    test/rom/cl67srmrom.exe (the merge gate's console)
#   ds20e ds20 ds10         PC264SRM.ROM / DS10SRM.ROM, a console image
#   ds20l es45 es45m1 ds25 ds15
#                           the update utility (DS20L_V6_6.EXE, ES45_V7_3.EXE,
#                           ...). It is run first, as on a real machine
#                           (<return>, update, Y), the emulator stopped so the
#                           flash is saved, and then the board is booted from
#                           that flash with no firmware image named.
#   es47 es80 gs1280        SRM_V7_3.EXE (GS1280 directory); the console is on
#                           the CMM's virtual UART, which serial0's telnet
#                           port carries.
#
# Output: one line per board, `PASS|FAIL <board> <seconds> <reason>`, and per
# board in $ALPHABOX_WORK/runs/boards-<label>/ (ALPHABOX_WORK defaults to
# <repo>/lab):
#   <board>.log        the console session, normalised (timestamps, run paths,
#                      port numbers), so two runs can be diffed
#   <board>.cmds.log   the commands' output alone, normalised
#   <board>.warn       the %SYS-W-/%SYS-E-/FAILURE lines the emulator printed
#   <board>/           the run directory (config, alphabox.out, flash)
#   <board>-init.log, <board>-init/
#                      the EV7 boards' `init` check (marvel_init_check.sh)
#   summary.txt        the PASS/FAIL lines
# The emulator's %SYS-W-/%SYS-E-/FAILURE lines are also listed under each
# board (counted, at most 12 distinct). Exit 0 only if every board reached its
# prompt. `--diff` compares two labelled runs board by board.
#
# Environment:
#   ALPHABOX_WORK        output root (default <repo>/lab)
#   BOARDS_PORT_BASE     first telnet port (default 21600); a board's port is
#                        the base plus its place in the list above, so it does
#                        not depend on which boards you select. Run one board
#                        at a time, sequentially.
#   BOARDS_PROMPT_TMO    seconds to wait for the prompt (default 600)
#   BOARDS_CMD_TMO       seconds per console command (default 180)
#   BOARDS_MIN_FREE_GB   stop if the data volume has less free (default 3)
#   BOARDS_KEEP=1        keep the copied firmware and decompressed consoles
#   BOARDS_SYS_EXTRA     text added inside the machine block (e.g.
#                        'serial_number = "AY12345678";')
#   BOARDS_EXTRA_CMDS    more console commands, separated by ';' (e.g.
#                        'show sys_serial_num')
#   BOARDS_INIT=0        skip the EV7 boards' `init` check
#   BOARDS_EV7_CPU       the EV7 boards' processor class (default ev7; ev7z,
#                        or ev8, the 21464 in EV7-compatible mode), for the
#                        boards and their `init` check
#   BOARDS_AFTER         how the console run ends (srm_console.py --after;
#                        default sigterm). snap_roundtrip.sh sets sigusr1,
#                        with ALPHABOX_SNAPSHOT and ALPHABOX_SNAPSHOT_EXIT in
#                        the environment, to save the machine at the prompt.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
ROMS=${ALPHABOX_ROMS:-$R/roms}
FW=$ROMS/alpha-firmware-v7.3
BASE=${BOARDS_PORT_BASE:-21600}
PTMO=${BOARDS_PROMPT_TMO:-600}
CTMO=${BOARDS_CMD_TMO:-180}
MINFREE=${BOARDS_MIN_FREE_GB:-3}
ALL="es40 ds20e ds20 ds10 ds20l es45 es45m1 ds25 ds15 es47 es80 gs1280"

# Remove the run-specific parts of a log so two runs can be diffed.
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

if [ "${1:-}" = --diff ]; then
  [ $# -ge 3 ] || { sed -n '2,5p' "$0"; exit 2; }
  A=$WORK/runs/boards-$2; B=$WORK/runs/boards-$3; shift 3
  rc=0
  for b in ${@:-$ALL}; do
    [ -f "$A/$b.cmds.log" ] || [ -f "$B/$b.cmds.log" ] || continue
    if diff -a -u "$A/$b.cmds.log" "$B/$b.cmds.log" >/dev/null 2>&1; then
      echo "same $b"
    else
      echo "DIFF $b"; rc=1
      diff -a -u "$A/$b.cmds.log" "$B/$b.cmds.log" | head -60
    fi
  done
  exit $rc
fi

[ $# -ge 2 ] || { sed -n '2,5p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2; shift 2
BOARDS=${*:-$ALL}
[ -x "$BIN" ] || { echo "boards_check: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/boards-$LABEL
mkdir -p "$OUT" || exit 2
: > "$OUT/summary.txt"
PID=

cleanup() {
  [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && { kill "$PID" 2>/dev/null; kill -9 "$PID" 2>/dev/null; }
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# board_info <board>: sets B_PLAT B_SRC B_KIND (rom|lfu|ev7) B_BITS B_CPUS
# B_CPU B_DPR and B_DEV (the devices inside the machine block).
board_info() {
  B_PLAT=$1; B_KIND=rom; B_BITS=26; B_CPUS=1; B_CPU=ev68cb; B_DPR=1
  local ali='  pci0.7 = ali { vga_console = false; }'
  case $1 in
  es40) B_SRC=$R/test/rom/cl67srmrom.exe
    B_DEV="$ali
  pci0.15 = ali_ide { }
  pci0.19 = ali_usb { }" ;;
  ds20e|ds20) B_SRC=$FW/DS20/PC264SRM.ROM
    B_DEV='  pci0.5 = ali { vga_console = false; }
  pci0.6 = ali_ide { }' ;;
  ds10) B_SRC=$FW/DS10/DS10SRM.ROM
    B_DEV="$ali
  pci0.13 = ali_ide { }
  pci0.1 = ali_usb { }" ;;
  ds20l) B_SRC=$FW/DS20L/DS20L_V6_6.EXE; B_KIND=lfu
    B_DEV="$ali
  pci0.15 = ali_ide { }
  pci0.19 = ali_usb { }" ;;
  es45|es45m1) B_SRC=$FW/ES45/ES45_V7_3.EXE; B_KIND=lfu; B_DPR=0
    B_DEV="$ali
  pci0.16 = ali_ide { }" ;;
  ds25) B_SRC=$FW/DS25/DS25_V7_3.EXE; B_KIND=lfu; B_DPR=0
    B_DEV="$ali
  pci0.16 = ali_ide { }" ;;
  ds15) B_SRC=$FW/DS15/DS15_V7_3.EXE; B_KIND=lfu; B_DPR=0
    B_DEV="$ali
  pci0.13 = ali_ide { }" ;;
  es47|es80|gs1280) B_SRC=$FW/GS1280/SRM_V7_3.EXE; B_KIND=ev7; B_BITS=29
    B_CPUS=2; B_CPU=${BOARDS_EV7_CPU:-ev7}; B_DPR=0
    B_DEV='  pci2.2 = cmd649 { }
  pci2.3 = uss344 { port1 = "tablet"; port2 = "keyboard"; }' ;;
  *) return 1 ;;
  esac
}

# write_cfg <dir> <port> <with-rom 0|1>
write_cfg() {
  local d=$1 port=$2 withrom=$3 i
  {
    echo 'sys0 = tsunami'
    echo '{'
    echo "  platform = \"$B_PLAT\";"
    echo "  memory.bits = $B_BITS;"
    [ -n "${BOARDS_SYS_EXTRA:-}" ] && echo "  $BOARDS_SYS_EXTRA"
    [ "$withrom" = 1 ] && echo "  rom.srm = \"$(basename "$B_SRC")\";"
    if [ "$B_KIND" = ev7 ]; then echo '  rom.nvram = "cmm_nvram.bin";'
    else
      echo '  rom.flash = "flash.rom";'
      [ "$B_DPR" = 1 ] && echo '  rom.dpr = "dpr.rom";'
    fi
    for ((i = 0; i < B_CPUS; i++)); do
      echo "  cpu$i = $B_CPU { speed = 1000M; }"
    done
    echo "  serial0 = serial { address = \"127.0.0.1\"; port = $port; }"
    echo '  serial1 = serial { null_attach = true; }'
    echo "$B_DEV"
    echo '}'
  } > "$d/es40.cfg"
}

# start_emu <dir>: run the emulator in <dir>, PID in $PID
start_emu() {
  ( cd "$1" && exec "$BIN" run es40.cfg ) \
    > >(head -c 30000000 > "$1/alphabox.out") 2>&1 &
  PID=$!
}

# stop_emu: SIGTERM (graceful: the flash and DPR are saved), then wait for
# it, with a bound; kills only the PID this script started. EMU_RC is its
# exit status: 0 for a graceful exit.
EMU_RC=0
stop_emu() {
  [ -n "$PID" ] || return 0
  if kill -0 "$PID" 2>/dev/null; then
    kill "$PID" 2>/dev/null
    local n=0
    while kill -0 "$PID" 2>/dev/null && [ $n -lt 150 ]; do sleep 0.2; n=$((n + 1)); done
    kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
  fi
  wait "$PID" 2>/dev/null
  EMU_RC=$?
  PID=
}

# lfu_install <port> <log>: the update utility's dialog.
lfu_install() {
  python3 -I - "$1" "$2" "$PTMO" <<'PY'
import re, socket, sys, time
port, log, tmo = int(sys.argv[1]), sys.argv[2], float(sys.argv[3])
s = None
for _ in range(800):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        time.sleep(0.05)
if s is None:
    print("noconnect"); sys.exit(1)
s.settimeout(0.5)
out = open(log, "wb")
buf = b""
def wait(rx):
    global buf
    pat = re.compile(rx, re.S)
    end = time.time() + tmo
    while not pat.search(buf):
        if time.time() > end:
            return False
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            return False
        if not d:
            return False
        d = re.sub(rb"\xff[\xfb-\xfe].", b"", d).replace(b"\x00", b"")
        out.write(d); out.flush(); buf += d
    return True
def say(text):
    global buf
    s.sendall(text.encode() + b"\r")
    buf = b""
for rx, reply in ((rb"hit <return>", ""), (rb"UPD>", "update"),
                  (rb"\[Y/\(N\)\]", "Y")):
    if not wait(rx):
        print("lfu: no %s" % rx.decode()); sys.exit(1)
    say(reply)
if not wait(rb"UPD>\s*$"):
    print("lfu: update did not finish"); sys.exit(1)
ok = re.search(rb"(?i)SRM\s+Updating.*?Verifying[^\n]*PASSED", buf, re.S)
print("lfu: SRM %s" % ("PASSED" if ok else "not verified"))
sys.exit(0 if ok else 1)
PY
}

report() { # report <board> <PASS|FAIL> <seconds> <reason>
  local line="$2 $1 $3 $4"
  echo "$line" | tee -a "$OUT/summary.txt"
}

warnings() { # warnings <board> <dir>
  local b=$1 d=$2
  grep -a -E '%SYS-[WE]-|FAILURE' "$d/alphabox.out" 2>/dev/null | normalise "$d" > "$OUT/$b.warn"
  if [ -s "$OUT/$b.warn" ]; then
    sort "$OUT/$b.warn" | uniq -c | sort -rn | head -12 | sed 's/^/    emu: /'
  else
    echo "    emu: no %SYS-W-/%SYS-E-/FAILURE lines"
  fi
}

rc_all=0
for b in $BOARDS; do
  idx=0; for x in $ALL; do [ "$x" = "$b" ] && break; idx=$((idx + 1)); done
  if ! board_info "$b" || [ "$idx" -ge 12 ]; then
    echo "FAIL $b 0 unknown board"; rc_all=1; continue
  fi
  PORT=$((BASE + idx))
  D=$OUT/$b
  t0=$(date +%s)
  rm -rf "$D" "$OUT/$b".{log,cmds.log,warn}; mkdir -p "$D"
  fail() { # fail <reason>
    stop_emu
    report "$b" FAIL $(( $(date +%s) - t0 )) "$1"
    [ -f "$D/console.log" ] && normalise "$D" < "$D/console.log" | head -c 400000 > "$OUT/$b.log"
    [ -f "$D/cmds.txt" ] && normalise "$D" < "$D/cmds.txt" | head -c 400000 > "$OUT/$b.cmds.log"
    warnings "$b" "$D"
    rc_all=1
  }

  free_kb=$(df -k "$WORK" | awk 'NR==2 {print $4}')
  if [ "${free_kb:-0}" -lt $((MINFREE * 1048576)) ]; then
    fail "less than ${MINFREE} GB free on the data volume"; continue
  fi
  if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
    fail "memory pressure critical, not starting"; continue
  fi
  if [ ! -f "$B_SRC" ]; then fail "firmware missing: $B_SRC"; continue; fi
  cp "$B_SRC" "$D/" && chmod u+w "$D/$(basename "$B_SRC")"

  if [ "$B_KIND" = lfu ]; then
    # Phase 1: the update utility installs the console into the flash.
    write_cfg "$D" "$PORT" 1
    start_emu "$D"
    if ! lfu_install "$PORT" "$D/lfu.log" > "$D/lfu.status" 2>&1; then
      fail "update utility: $(tail -1 "$D/lfu.status")"; continue
    fi
    stop_emu
    if [ "$EMU_RC" != 0 ]; then
      fail "update utility run: exit status $EMU_RC"; continue
    fi
    rm -f "$D/$(basename "$B_SRC")"
    cp "$D/alphabox.out" "$D/alphabox-lfu.out"
  fi

  # The console: from the flash it was installed in (lfu), or the image.
  [ "$B_KIND" = lfu ] && withrom=0 || withrom=1
  write_cfg "$D" "$PORT" "$withrom"
  cmds=(--cmd "show config"); cmdnames="show config"
  [ "$B_KIND" = ev7 ] && { cmds+=(--cmd "show fru"); cmdnames="show config, show fru"; }
  if [ -n "${BOARDS_EXTRA_CMDS:-}" ]; then
    IFS=';' read -r -a extra_cmds <<< "$BOARDS_EXTRA_CMDS"
    for c in "${extra_cmds[@]}"; do cmds+=(--cmd "$c"); cmdnames="$cmdnames, $c"; done
  fi
  start_emu "$D"
  python3 -I "$T/srm_console.py" --port "$PORT" --log "$D/console.log" \
    --cmd-log "$D/cmds.txt" --timeout "$PTMO" --cmd-timeout "$CTMO" \
    --pid "$PID" --after "${BOARDS_AFTER:-sigterm}" --exit-timeout 60 "${cmds[@]}" \
    > "$D/console.status" 2>&1
  stop_emu
  st=$(tr '\n' ' ' < "$D/console.status")
  normalise "$D" < "$D/console.log" | head -c 400000 > "$OUT/$b.log"
  normalise "$D" < "$D/cmds.txt" | head -c 400000 > "$OUT/$b.cmds.log"
  secs=$(( $(date +%s) - t0 ))
  case $st in
  *status=prompt*no-prompt-after*) report "$b" FAIL $secs "a command did not return to the prompt: $st"; rc_all=1 ;;
  *status=prompt*)
    if [ "$EMU_RC" = 0 ]; then
      report "$b" PASS $secs "prompt, ${cmdnames}"
    else
      report "$b" FAIL $secs "prompt, ${cmdnames}; exit status $EMU_RC"; rc_all=1
    fi ;;
  *) report "$b" FAIL $secs "no prompt within ${PTMO}s: $st"; rc_all=1 ;;
  esac
  warnings "$b" "$D"
  if [ "${BOARDS_KEEP:-0}" != 1 ]; then
    rm -f "$D/$(basename "$B_SRC")" "$D"/*-decompressed.rom "$D"/decompressed.rom
  fi
done

# The Marvel consoles' `init`, which halts and starts the secondaries again:
# every processor must come back (marvel_init_check.sh).
if [ "${BOARDS_INIT:-1}" != 0 ]; then
  mv=
  for b in $BOARDS; do case $b in es47|es80|gs1280) mv="$mv $b" ;; esac; done
  if [ -n "$mv" ]; then
    BOARDS_PORT_BASE=$BASE BOARDS_PROMPT_TMO=$PTMO BOARDS_MIN_FREE_GB=$MINFREE \
      ALPHABOX_WORK=$WORK ALPHABOX_ROMS=$ROMS "$T/marvel_init_check.sh" "$BIN" "$LABEL" $mv
    [ $? = 0 ] || rc_all=1
    cat "$OUT/init-summary.txt" >> "$OUT/summary.txt"
  fi
fi
exit $rc_all
