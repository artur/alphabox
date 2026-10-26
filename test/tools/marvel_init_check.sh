#!/bin/bash
# Check that a Marvel console restarts every processor at `init`.
#
# usage: marvel_init_check.sh <alphabox-binary> <label> [board[:cpus] ...]
#
# Boards (default es47:2 es80:4 gs1280:4): each is booted to P00>>> with that
# many processors (ev7, or BOARDS_EV7_CPU) and boards_check.sh's other EV7
# settings (512 MB per processor, the CMD 649 and the USS-344 at pci2.2 and
# pci2.3), and `init` is typed MARVEL_INIT_INITS times. At `init` the console halts its secondaries
# and starts each again through its RBOX_SCRATCH1 (docs/platforms/marvel.md,
# "The start hand-off"); a secondary that does not answer in time is left out
# of the machine the operating system is given. A run passes when, after each
# `init`, the console printed "Starting secondary CPU" once per secondary and
# none of "failed to take jump address", "not accessible" or "No memory
# available"; and when, after the last one, the HWRPB's processor slots hold
# exactly that many processors with flags 0x1cc set (available, present,
# PALcode valid, memory valid and loaded: what Linux counts, setup_smp in
# arch/alpha/kernel/smp.c) and `show cpu` lists that many. The emulator must
# then exit with status 0 on SIGTERM.
#
# Output: one line per board and run, `PASS|FAIL <board>-init <seconds>
# <reason>`, also in $ALPHABOX_WORK/runs/boards-<label>/init-summary.txt
# (ALPHABOX_WORK defaults to <repo>/lab), with each run's console session as
# <board>-init[-<run>].log and its run directory beside it. boards_check.sh
# runs this for the EV7 boards it checks. Exit 0 only if every run passed.
#
# Environment:
#   MARVEL_INIT_INITS      `init`s per run (default 2)
#   MARVEL_INIT_RUNS       runs per board (default 1)
#   BOARDS_PORT_BASE       as boards_check.sh (default 21600): a board's port
#                          is the base plus 12 plus its place in es47 es80
#                          gs1280, past boards_check.sh's twelve
#   BOARDS_PROMPT_TMO      seconds to wait for each prompt (default 600)
#   BOARDS_MIN_FREE_GB     stop if the data volume has less free (default 3)
#   BOARDS_EV7_CPU         the processor class (default ev7; ev7z, or ev8,
#                          the 21464 in EV7-compatible mode)
#   BOARDS_SPEED           every processor's `speed` (default 1000M)
#   BOARDS_EV8_TPUS        with ev8: each chip's TPUs, 1 to 4, written as its
#                          `tpus` (unset: no key, the row's one processor
#                          per chip). Each TPU is a processor
#                          (docs/platforms/ev8.md, S4): a board of n chips
#                          is checked for n * TPUs processors, and [:cpus]
#                          counts chips.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
ROMS=${ALPHABOX_ROMS:-$R/roms}
SRM=$ROMS/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE
BASE=${BOARDS_PORT_BASE:-21600}
PTMO=${BOARDS_PROMPT_TMO:-600}
MINFREE=${BOARDS_MIN_FREE_GB:-3}
INITS=${MARVEL_INIT_INITS:-2}
RUNS=${MARVEL_INIT_RUNS:-1}
CPU=${BOARDS_EV7_CPU:-ev7}
SPEED=${BOARDS_SPEED:-1000M}
TPUS=1; CPUOPT=
if [ "$CPU" = ev8 ] && [ -n "${BOARDS_EV8_TPUS:-}" ]; then
  TPUS=$BOARDS_EV8_TPUS; CPUOPT=" tpus = $TPUS;"
fi

[ $# -ge 2 ] || { sed -n '2,4p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2; shift 2
BOARDS=${*:-es47:2 es80:4 gs1280:4}
[ -x "$BIN" ] || { echo "marvel_init_check: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/boards-$LABEL
mkdir -p "$OUT" || exit 2
: > "$OUT/init-summary.txt"
PID=

cleanup() {
  [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && { kill "$PID" 2>/dev/null; kill -9 "$PID" 2>/dev/null; }
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# The console session: the prompt, the `init`s, `show cpu`, the HWRPB's
# processor slots. Prints `ok <reason>` or `bad <reason>`.
drive() { # drive <port> <log> <cpus> <inits>
  python3 -I - "$@" "$PTMO" <<'PY'
import re, socket, sys, time
port, log, ncpu, inits, tmo = (int(sys.argv[1]), sys.argv[2], int(sys.argv[3]),
                               int(sys.argv[4]), float(sys.argv[5]))
s = None
for _ in range(800):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2)
        break
    except OSError:
        time.sleep(0.05)
if s is None:
    print("bad no connection"); sys.exit(1)
s.settimeout(1)
out = open(log, "wb")
def prompt():
    buf = b""
    end = time.time() + tmo
    while time.time() < end:
        try:
            d = s.recv(4096)
        except socket.timeout:
            continue
        except OSError:
            break
        if not d:
            break
        d = re.sub(rb"\xff[\xfb-\xfe].", b"", d).replace(b"\x00", b"")
        out.write(d); out.flush(); buf += d
        if b"P00>>>" in buf[-64:]:
            return buf.decode("latin1").replace("\r", "")
    return None
def cmd(c):
    s.sendall(c.encode() + b"\r")
    r = prompt()
    if r is None:
        print("bad no prompt after %s" % c); sys.exit(1)
    return r
if prompt() is None:
    print("bad no prompt"); sys.exit(1)
bad = []
for k in range(1, inits + 1):
    r = cmd("init")
    started = len(re.findall(r"Starting secondary CPU", r))
    for m in re.findall(r"CPU \d+ failed to take jump address[^\n]*|"
                        r"CPU \d+ not accessible|"
                        r"No memory available to start secondary CPU \d+", r):
        bad.append("init %d: %s" % (k, m.strip()))
    if started != ncpu - 1:
        bad.append("init %d: %d secondaries started, not %d" % (k, started, ncpu - 1))
listed = len(re.findall(r"(?m)^CPU \d+ ", cmd("show cpu")))
m = re.search(r"HWRPB is at ([0-9a-fA-F]+)", cmd("show hwrpb"))
if not m:
    print("bad no HWRPB address"); sys.exit(1)
hwrpb = int(m.group(1), 16)
def q(addr, n=0):
    r = cmd("e -q %spmem:%x" % ("-n %d " % n if n else "", addr))
    return [int(v, 16) for v in re.findall(r"(?m)^pmem:\s*[0-9a-fA-F]+\s+([0-9a-fA-F]{16})", r)]
hdr = q(hwrpb + 0x90, 2)  # nr_processors, processor_size, processor_offset
if len(hdr) != 3:
    print("bad HWRPB header unread"); sys.exit(1)
nr, size, off = hdr
def flags(i):  # the slot's flags: hwpcb[16], then flags
    v = q(hwrpb + off + i * size + 0x80)
    return v[0] if v else 0
valid = [i for i in range(min(nr, 256)) if (flags(i) & 0x1cc) == 0x1cc]
if len(valid) != ncpu:
    bad.append("HWRPB: %d processors valid (%s), not %d" %
               (len(valid), " ".join(map(str, valid)), ncpu))
if listed != ncpu:
    bad.append("show cpu lists %d" % listed)
if bad:
    print("bad " + "; ".join(bad)); sys.exit(1)
print("ok %d processors after each of %d init%s; HWRPB slots %s" %
      (ncpu, inits, "s" if inits > 1 else "", " ".join(map(str, valid))))
PY
}

rc_all=0
for spec in $BOARDS; do
  b=${spec%%:*}; n=${spec#*:}
  case $b in es47) idx=0; dn=2 ;; es80) idx=1; dn=4 ;; gs1280) idx=2; dn=4 ;;
  *) echo "FAIL $b-init 0 not a Marvel board" | tee -a "$OUT/init-summary.txt"; rc_all=1; continue ;;
  esac
  [ "$n" = "$spec" ] && n=$dn
  PORT=$((BASE + 12 + idx))
  for ((run = 1; run <= RUNS; run++)); do
    name=$b-init; [ "$RUNS" -gt 1 ] && name=$b-init-$run
    D=$OUT/$name
    t0=$(date +%s)
    report() { # report <PASS|FAIL> <reason>
      echo "$1 $b-init $(( $(date +%s) - t0 )) $2" | tee -a "$OUT/init-summary.txt"
      [ "$1" = PASS ] || rc_all=1
    }
    free_kb=$(df -k "$WORK" | awk 'NR==2 {print $4}')
    if [ "${free_kb:-0}" -lt $((MINFREE * 1048576)) ]; then
      report FAIL "less than ${MINFREE} GB free on the data volume"; continue
    fi
    if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
      report FAIL "memory pressure critical, not starting"; continue
    fi
    [ -f "$SRM" ] || { report FAIL "firmware missing: $SRM"; continue; }
    rm -rf "$D" "$D.log"; mkdir -p "$D"
    cp "$SRM" "$D/" && chmod u+w "$D/SRM_V7_3.EXE"
    {
      echo 'sys0 = tsunami'
      echo '{'
      echo "  platform = \"$b\";"
      echo '  memory.bits = 29;'
      echo '  rom.srm = "SRM_V7_3.EXE";'
      echo '  rom.nvram = "cmm_nvram.bin";'
      for ((i = 0; i < n; i++)); do echo "  cpu$i = $CPU { speed = $SPEED;$CPUOPT }"; done
      echo "  serial0 = serial { address = \"127.0.0.1\"; port = $PORT; }"
      echo '  serial1 = serial { null_attach = true; }'
      echo '  pci2.2 = cmd649 { }'
      echo '  pci2.3 = uss344 { port1 = "tablet"; port2 = "keyboard"; }'
      echo '}'
    } > "$D/es40.cfg"
    ( cd "$D" && exec "$BIN" run es40.cfg ) \
      > >(head -c 30000000 > "$D/alphabox.out") 2>&1 &
    PID=$!
    st=$(drive "$PORT" "$D/console.log" $((n * TPUS)) "$INITS" 2>&1 | tail -1)
    if kill -0 "$PID" 2>/dev/null; then
      kill "$PID" 2>/dev/null
      k=0
      while kill -0 "$PID" 2>/dev/null && [ $k -lt 300 ]; do sleep 0.2; k=$((k + 1)); done
      kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
    fi
    wait "$PID" 2>/dev/null; erc=$?
    PID=
    tr -d '\r\000' < "$D/console.log" > "$D.log" 2>/dev/null
    rm -f "$D/SRM_V7_3.EXE" "$D"/*decompressed.rom
    case $st in
    ok\ *) if [ "$erc" = 0 ]; then report PASS "${st#ok }"
           else report FAIL "${st#ok }; exit status $erc"; fi ;;
    *) report FAIL "${st#bad }" ;;
    esac
  done
done
exit $rc_all
