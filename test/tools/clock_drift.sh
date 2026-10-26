#!/bin/bash
# How well a guest's clock keeps time against the host's.
#
# usage: clock_drift.sh <alphabox-binary> <label> <guest> [minutes]
#
# Cold boots one guest of test/tools/guests_check.sh on a clone of its disk
# (its configuration, boot and login code are sourced from there), reads the
# guest's clock against the host's just after login, a minute later and
# <minutes> later (default 20), and prints the drift in seconds per hour,
# PASS or FAIL against a bound (default 2 s per hour).
#
# The guests, and how their clock is read:
#   vms-es40   OpenVMS 8.4 on the ES40 (one EV68 at 833 MHz): WRITE
#              SYS$OUTPUT F$TIME() on the serial console, the time SHOW TIME
#              prints, with hundredths (SHOW TIME itself is logged beside it)
#   linux      Gentoo on the ES40 (one EV68 at 833 MHz): date +%s.%N on the
#              serial console, nanoseconds. Also the cycle and timer
#              frequencies of /proc/cpuinfo (the HWRPB's), the kernel's
#              clocksource and what it said of its cycle counter, and the
#              timer's count in /proc/interrupts at each reading: the rate
#              of the interval timer the guest sees, over the first minute
#              and over the run
#   w2k        Windows 2000 RC2 (two EV68s): `echo %TIME% > COM1` typed into
#              a command window through the key pipe, hundredths; serial0 is
#              put on a telnet port for it, and the host's time of a reading
#              is when its line arrives there. If nothing arrives, the
#              command window is read from the screen instead (tesseract):
#              `echo %TIME%`, the host's time then being when its Enter key
#              was handed to the key pipe, good to about 0.15 s a reading.
#              NOT WORKING YET (2026-10-26): the RC2 image's COM1 gave
#              nothing, and tesseract misreads the console's raster font
#              (18:13:44.21 as "18 213=44_.21"); the time is on the screen,
#              so the next step is matching its glyphs exactly, as
#              guests_check.sh reads the taskbar clock
#
# A reading is DRIFT_READS clock reads in a row (default 5). For each read the
# offset is the guest's clock minus the host's when the line arrived; a
# reading is their mean, and its spread (largest minus smallest) is the
# resolution actually seen. drift = (the last reading's offset - the first's)
# / the host time between them, per hour. The host's load average is noted
# at every reading. The host's monotonic clock is kept beside its wall
# clock: if they part by more than a second over the run, the host slept or
# its clock was set, and the run ends RERUN instead of PASS or FAIL; a
# reading more than a minute late says so too.
#
# Output: one line `PASS|FAIL|RERUN <guest> <drift> s/h ...`, and in
# $ALPHABOX_WORK/runs/clock-<label>/ (ALPHABOX_WORK defaults to <repo>/lab):
#   <guest>.txt    the result: offsets, spreads, loads, rates
#   <guest>.chat   the readings as the dialogue recorded them
#   <guest>.log    the console (serial session, or the emulator's output)
#   <guest>.png    the last frame (w2k)
# Exit 0 for PASS, 1 for FAIL, 3 for RERUN, 2 when the run did not happen.
#
# Environment:
#   DRIFT_MINUTES        minutes between the first and last readings (20)
#   DRIFT_BOUND          the PASS bound in seconds per hour (2)
#   DRIFT_READS          clock reads per reading (5)
#   GUESTS_PORT_BASE     as guests_check.sh: the guest's telnet port is the
#                        base plus its place in guests_check.sh's list
#   GUESTS_BOOT_TMO, GUESTS_KEEP, ALPHABOX_WORK   as guests_check.sh
#   DRIFT_DUTY=<run_ms>:<stop_ms>  from the first reading on, stop and
#                        continue the emulator (SIGSTOP, SIGCONT) so that it
#                        runs run_ms out of every run_ms + stop_ms: a host
#                        that gives it that share of its time, made the same
#                        every run, without loading anyone else's work
#   DRIFT_SKIP_HOSTCHECK=1  start even if the host is short of memory or
#                        disk; otherwise the run needs memory pressure level
#                        1, under 17.5 GB of swap in use and over 4 GB free
#                        on the data volume (checked where sysctl has them)
set -u
export LC_ALL=C
TD=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 3 ] || { sed -n '2,4p' "$0"; exit 2; }
DRIFT_BIN=$1 DRIFT_LABEL=$2 GUEST=$3
MIN=${4:-${DRIFT_MINUTES:-20}}
BOUND=${DRIFT_BOUND:-2}
NREAD=${DRIFT_READS:-5}
case $GUEST in
vms-es40 | linux | w2k) ;;
*) echo "clock_drift: unknown guest '$GUEST' (vms-es40, linux, w2k)"; exit 2 ;;
esac
case $MIN in '' | *[!0-9]*) echo "clock_drift: minutes must be a whole number"; exit 2 ;; esac
[ "$MIN" -ge 2 ] || { echo "clock_drift: at least 2 minutes"; exit 2; }

host_ok() {
  [ "${DRIFT_SKIP_HOSTCHECK:-0}" = 1 ] && return 0
  local lvl swap free
  lvl=$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null)
  if [ -n "$lvl" ] && [ "$lvl" != 1 ]; then
    echo "memory pressure level $lvl, not 1"; return 1
  fi
  swap=$(sysctl vm.swapusage 2>/dev/null | sed -n 's/.*used = \([0-9.]*\)M.*/\1/p')
  if [ -n "$swap" ] && ! awk -v s="$swap" 'BEGIN {exit !(s < 17920)}'; then
    echo "${swap} MB of swap in use, not under 17.5 GB"; return 1
  fi
  free=$(df -k "${ALPHABOX_WORK:-$TD/../../lab}" 2>/dev/null | awk 'NR==2 {print $4}')
  if [ -n "$free" ] && [ "$free" -le $((4 * 1048576)) ]; then
    echo "$((free / 1048576)) GB free on the data volume, not over 4"; return 1
  fi
}
why=$(host_ok) || { echo "SKIP $GUEST: $why"; exit 2; }

# The library: the helper, the guests' configurations, boot and login.
export GUESTS_OUT=${ALPHABOX_WORK:-$(cd "$TD/../.." && pwd)/lab}/runs/clock-$DRIFT_LABEL
set -- "$DRIFT_BIN" "clock-$DRIFT_LABEL"
# shellcheck source=guests_check.sh
. "$TD/guests_check.sh"
idx=0
for x in $ALL $EXTRA; do [ "$x" = "$GUEST" ] && break; idx=$((idx + 1)); done
PORT=$((BASE + idx))
D=$OUT/$GUEST
CHAT=$OUT/$GUEST.chat
TAPPID= DUTYPID=
DUTY=${DRIFT_DUTY:-}
case $DUTY in '' | [0-9]*:[0-9]*) ;; *) echo "clock_drift: DRIFT_DUTY is <run_ms>:<stop_ms>"; exit 2 ;; esac
t0=$(date +%s)
secs() { echo $(($(date +%s) - t0)); }
endrun() { # endrun <rc>: stop what this run started, keep the logs
  duty_end
  stop_emu
  [ -n "$TAPPID" ] && kill "$TAPPID" 2>/dev/null
  [ -d "$D" ] && lastframe "$D" "$OUT/$GUEST.png" 2>/dev/null
  [ -f "$D/console.log" ] && tr -d '\r\000' < "$D/console.log" | tail -c 400000 > "$OUT/$GUEST.log"
  [ -f "$D/alphabox.out" ] && tr -d '\r\000' < "$D/alphabox.out" | tail -c 200000 > "$OUT/$GUEST.emu.log"
  [ "${GUESTS_KEEP:-0}" = 1 ] || rm -rf "$D"
  exit "$1"
}
die() { echo "FAIL $GUEST $(secs) $1" | tee -a "$OUT/summary.txt"; endrun 2; }

# --- DRIFT_DUTY: the emulator gets run_ms of every run_ms + stop_ms ------------
cat > "$OUT/.duty.py" << 'PY'
import os, re, signal, sys, time

pid, run_ms, stop_ms = int(sys.argv[1]), float(sys.argv[2]), float(sys.argv[3])
logf, start_rx, stop_rx = sys.argv[4], sys.argv[5], sys.argv[6]


def alive():
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def end(*_):
    if alive():
        os.kill(pid, signal.SIGCONT)
    print("duty: %d cycles, stopped %.1f s of %.1f s" % (cycles, stopped, time.time() - t0))
    sys.exit(0)


cycles, stopped, t0 = 0, 0.0, time.time()
signal.signal(signal.SIGTERM, end)
pos, txt = 0, ""


def log_has(rx):
    """Whether the console log has rx in what was written since the last look."""
    global pos, txt
    try:
        with open(logf, "rb") as f:
            f.seek(pos)
            d = f.read()
    except OSError:
        return False
    pos += len(d)
    txt = (txt + d.decode("latin1"))[-4096:]
    return re.search(rx, txt) is not None


while start_rx and not log_has(start_rx):
    if not alive():
        end()
    time.sleep(0.5)
txt, t0, last = "", time.time(), 0.0
while alive():
    if stop_rx and time.time() - last > 1:
        last = time.time()
        if log_has(stop_rx):
            break
    time.sleep(run_ms / 1000)
    os.kill(pid, signal.SIGSTOP)
    a = time.time()
    time.sleep(stop_ms / 1000)
    os.kill(pid, signal.SIGCONT)
    stopped += time.time() - a
    cycles += 1
end()
PY
duty_start() { # duty_start [start-regex stop-regex]: on $PID, from that output
  [ -n "$DUTY" ] || return 0
  python3 -I "$OUT/.duty.py" "$PID" "${DUTY%%:*}" "${DUTY#*:}" "$D/console.log" "${1:-}" "${2:-}" \
    >> "$OUT/$GUEST.duty" 2>&1 &
  DUTYPID=$!
}
duty_end() {
  [ -n "$DUTYPID" ] || return 0
  kill "$DUTYPID" 2>/dev/null
  wait "$DUTYPID" 2>/dev/null
  DUTYPID=
}
serial_started() { duty_start 'GCT=[0-9]' 'SYS\$SYSTEM:SHUTDOWN|sync; poweroff'; }
[ -n "$DUTY" ] && SERIAL_GUEST_STARTED=serial_started

# --- the dialogue for the serial guests --------------------------------------
STAMP_VMS='GCT=(\d+-[A-Z]{3}-\d{4} \d\d:\d\d:\d\d\.\d\d)'
STAMP_LINUX='GCT=(\d+\.\d+)'
STAMP_IRQ='GCI= *(\d+: *\d+[^\n]*)'
read_steps() { # read_steps <name>: one reading, DRIFT_READS reads in a row
  local i
  steps+=("load=$1")
  for ((i = 0; i < NREAD; i++)); do
    if [ "$GUEST" = vms-es40 ]; then
      steps+=("send=WRITE SYS\$OUTPUT \"GCT=\"+F\$TIME()" "stamp=$1:$STAMP_VMS@60" "expect=\\$ \$@60")
    else
      steps+=("send=date +GCT=%s.%N" "stamp=$1:$STAMP_LINUX@60" "expect=# \$@60")
    fi
  done
  if [ "$GUEST" = vms-es40 ]; then
    steps+=("send=SHOW TIME" "expect=\\$ \$@60")
  else
    steps+=("send=(grep -i timer /proc/interrupts || grep -i rtc /proc/interrupts) | head -1 | sed 's/^/GCI=/'"
            "stamp=$1:$STAMP_IRQ@60" "expect=# \$@60")
  fi
}

serial_drift() {
  local l rest=$((MIN * 60 - 60))
  steps=()
  if [ "$GUEST" = vms-es40 ]; then
    vms_es40_setup || die "$SETUP_WHY"
    while IFS= read -r l; do steps+=("$l"); done < <(vms_steps dqa0 "$BTMO")
  else
    linux_setup || die "$SETUP_WHY"
    while IFS= read -r l; do steps+=("$l"); done < <(linux_login_steps)
    steps+=("send=grep -i -E 'cycle freq|timer freq|BogoMIPS|cpus active' /proc/cpuinfo; echo GC-DONE-\$?"
            "expect=GC-DONE-[0-9]@60"
            "send=cat /sys/devices/system/clocksource/clocksource0/current_clocksource; echo GC-DONE-\$?"
            "expect=GC-DONE-[0-9]@60"
            "send=dmesg | grep -i -E 'cycle|hwrpb|clocksource|rtc|timer' | head -20; echo GC-DONE-\$?"
            "expect=GC-DONE-[0-9]@60")
  fi
  read_steps first
  steps+=("sleep=60")
  read_steps minute
  steps+=("sleep=$rest")
  read_steps last
  if [ "$GUEST" = vms-es40 ]; then
    steps+=("send=@SYS\$SYSTEM:SHUTDOWN" "reply=(\\]\\??:?|\\?) ?\$>" "expect=SHUTDOWN COMPLETE|halted CPU|P00>>>@300")
  else
    steps+=("send=sync; poweroff" "expect=halted CPU|P00>>>@180")
  fi
  serial_guest "$GUEST" "$PORT" "${steps[@]}" || die "console dialogue: $SG_RESULT"
  cp "$D/chat.txt" "$CHAT"
}

# --- Windows 2000 --------------------------------------------------------------
w2k_serial() { # the copied configuration: serial0 on the telnet port
  awk -v port="$PORT" '/serial0 *= *serial/ {s = 1}
    s && /null_attach *= *true/ {sub(/null_attach *= *true;/, "address = \"127.0.0.1\"; port = " port ";"); s = 0}
    {print}' "$1" > "$1.drift" && mv "$1.drift" "$1" && grep -q "port = $PORT;" "$1"
}
type_text() { keys "$(python3 -I "$TD/keys_for.py" "$1")"; }
stamp_line() { # stamp_line stamp|load <name>: the host's clocks now (and load)
  python3 -I -c 'import os, sys, time
f = [sys.argv[1], sys.argv[2], "%.6f" % time.time(), "%.6f" % time.monotonic()]
if sys.argv[1] == "load":
    f += ["%.2f" % v for v in os.getloadavg()]
print(" ".join(f))' "$@"
}
W2K_MODE=com1
w2k_read() { # w2k_read <name>: one reading of DRIFT_READS reads
  local i n0 k v f t
  stamp_line load "$1" >> "$CHAT"
  keys shift; sleep 1; keys shift; sleep 1 # wake a screen saver, if one came
  for ((i = 0; i < NREAD; i++)); do
    if [ "$W2K_MODE" = com1 ]; then
      n0=$(wc -l < "$OUT/w2k.com1")
      type_text 'echo %TIME% > COM1'; keys enter
      k=0
      while [ "$(wc -l < "$OUT/w2k.com1")" -le "$n0" ] && [ $k -lt 60 ]; do sleep 0.5; k=$((k + 1)); done
      if [ "$(wc -l < "$OUT/w2k.com1")" -gt "$n0" ]; then
        sed -n "$((n0 + 1))p" "$OUT/w2k.com1" | sed "s/^stamp com1 /stamp $1 /" >> "$CHAT"
        sleep 1
        continue
      fi
      [ "$1" = first ] && [ "$i" = 0 ] || die "no COM1 line within 30 s at reading $1"
      W2K_MODE=screen
      echo "note COM1 gave nothing: the screen is read instead" >> "$CHAT"
      # a write that waits on the port is given up; dark text on white
      # for the reader
      keys ctrl-c; sleep 2
      type_text 'color f0'; keys enter; sleep 2
    fi
    # From the screen: a clear window, the command, its Enter alone.
    type_text 'cls'; keys enter; sleep 2
    type_text 'echo %TIME%'; sleep 4
    t=$(stamp_line stamp "$1")
    keys enter
    sleep 5
    f=$(ls "$D"/fb/*.ppm 2>/dev/null | tail -1)
    v=$(H ocr "$f" 0,0,-1,-1 3 6 | grep -o -E '[0-9]{1,2}:[0-9]{2}:[0-9]{2}[.,][0-9]{2}' | tail -1)
    [ -n "$v" ] || { echo "note $1 read $i: no time on the screen" >> "$CHAT"; continue; }
    echo "$t $v" >> "$CHAT"
  done
}

w2k_drift() {
  local r
  : > "$OUT/w2k.com1"
  # the listener connects once the emulator is up, and keeps every line
  python3 -I "$HELPER" tap "$PORT" "$OUT/w2k.com1.log" "$OUT/w2k.com1" 900 > "$OUT/w2k.tap.out" 2>&1 &
  TAPPID=$!
  WIN_GUEST_PREP=w2k_serial win_guest w2k win2k-installed es40-window-smp.cfg 600 1 || die "$W_WHAT"
  echo "note $W_WHAT" >> "$CHAT"
  # a command window: Start, Run, cmd
  keys ctrl-esc; sleep 3; keys r; sleep 4
  type_text cmd; keys enter; sleep 8
  duty_start
  w2k_read first
  sleep 50
  w2k_read minute
  sleep $((MIN * 60 - 60 - 20))
  w2k_read last
  duty_end
  r=$(win_shutdown 240)
  echo "note $r" >> "$CHAT"
}

: > "$CHAT"
rm -f "$OUT/$GUEST.duty"
if [ "$GUEST" = w2k ]; then w2k_drift; else serial_drift; fi
duty_end
[ -n "$DUTY" ] && echo "note DRIFT_DUTY=$DUTY: $(tail -1 "$OUT/$GUEST.duty" 2>/dev/null)" >> "$CHAT"
echo "note guest $GUEST, binary $BIN ($("$BIN" --version 2>/dev/null | head -1)), $MIN minutes, $NREAD reads a reading, port $PORT" >> "$CHAT"

# --- the result ------------------------------------------------------------------
cat > "$OUT/.drift.py" << 'PY'
import datetime, re, sys

guest, chatf, logf, minutes, bound = sys.argv[1:6]
minutes, bound = int(minutes), float(bound)
reads, loads, irq, notes = {}, {}, {}, []


def guest_offset(v, wall):
    """The guest's clock minus the host's, in seconds."""
    host = datetime.datetime.fromtimestamp(wall)
    if guest == "linux":
        return float(v) - wall
    if guest == "vms-es40":
        g = datetime.datetime.strptime(v, "%d-%b-%Y %H:%M:%S.%f")
        return (g - host).total_seconds()
    h, m, s = v.replace(",", ".").split(":")
    d = int(h) * 3600 + int(m) * 60 + float(s) - (
        host.hour * 3600 + host.minute * 60 + host.second + host.microsecond / 1e6)
    return (d + 43200) % 86400 - 43200


for line in open(chatf, errors="replace"):
    f = line.split()
    if len(f) >= 5 and f[0] == "stamp":
        name, wall, mono, v = f[1], float(f[2]), float(f[3]), " ".join(f[4:])
        if guest == "linux" and v[:1].isdigit() and ":" in v:
            m = re.match(r"(\d+):\s*(\d+)", v)
            irq.setdefault(name, (wall, int(m.group(2))))
            continue
        reads.setdefault(name, []).append((wall, mono, guest_offset(v, wall)))
    elif len(f) >= 7 and f[0] == "load":
        loads[f[1]] = (float(f[2]), float(f[3]), f[4], f[5], f[6])
    elif f and f[0] == "note":
        notes.append(line[5:].strip())

out = []
for n in notes:
    out.append("note: " + n)


def mean(xs):
    return sum(xs) / len(xs)


pts = {}
for name in ("first", "minute", "last"):
    r = reads.get(name, [])
    if not r:
        continue
    w, mo, off = mean([x[0] for x in r]), mean([x[1] for x in r]), mean([x[2] for x in r])
    spread = max(x[2] for x in r) - min(x[2] for x in r)
    pts[name] = (w, mo, off)
    ld = loads.get(name)
    out.append("reading %-6s at %s: %d reads, offset %+.3f s, spread %.3f s, load %s" % (
        name, datetime.datetime.fromtimestamp(w).strftime("%H:%M:%S"), len(r), off, spread,
        " ".join(ld[2:]) if ld else "?"))

verdict, drift = "RERUN", None
if "first" in pts and "last" in pts:
    (w0, m0, o0), (w1, m1, o1) = pts["first"], pts["last"]
    drift = (o1 - o0) / (w1 - w0) * 3600
    jump = (w1 - w0) - (m1 - m0)
    late = (w1 - w0) - minutes * 60
    out.append("drift %+.3f s/h over %.1f min (host wall %.1f s, monotonic %.1f s)" % (
        drift, (w1 - w0) / 60, w1 - w0, m1 - m0))
    if "minute" in pts:
        wm, _, om = pts["minute"]
        out.append("first minute: %+.3f s/h" % ((om - o0) / (wm - w0) * 3600))
    if abs(jump) > 1:
        out.append("RERUN: the host's wall and monotonic clocks parted by %.1f s (a sleep, or the clock was set)" % jump)
    elif abs(late) > 60:
        out.append("RERUN: the last reading came %.0f s off its time" % late)
    else:
        verdict = "PASS" if abs(drift) <= bound else "FAIL"
if guest == "linux" and irq:
    names = [n for n in ("first", "minute", "last") if n in irq]
    for a, b in zip(names, names[1:]):
        (wa, ca), (wb, cb) = irq[a], irq[b]
        out.append("timer interrupts %s..%s: %d in %.2f s, %.3f Hz" % (a, b, cb - ca, wb - wa, (cb - ca) / (wb - wa)))
    if "first" in irq and "last" in irq:
        (wa, ca), (wb, cb) = irq["first"], irq["last"]
        out.append("timer rate over the run: %.3f Hz" % ((cb - ca) / (wb - wa)))
    # the guest's output, without the commands it echoed
    txt = "\n".join(l for l in open(logf, errors="replace").read().split("\n")
                    if "GC-DONE" not in l and "GCI=" not in l)
    for rx in (r"cycle frequency[^\n]*", r"timer frequency[^\n]*", r"BogoMIPS[^\n]*",
               r"HWRPB[^\n]*", r"[Ee]stimated[^\n]*", r"clocksource[^\n]*",
               r"^(rpcc|jiffies|qemu)\s*$"):
        for m in re.finditer(rx, txt, re.M):
            out.append("guest: " + m.group(0).strip())
print("\n".join(out))
print("RESULT %s %s" % (verdict, "%+.3f" % drift if drift is not None else "-"))
PY
tr -d '\r\000' < "$D/console.log" > "$OUT/$GUEST.log" 2>/dev/null || : > "$OUT/$GUEST.log"
python3 -I "$OUT/.drift.py" "$GUEST" "$CHAT" "$OUT/$GUEST.log" "$MIN" "$BOUND" > "$OUT/$GUEST.txt"
res=$(sed -n 's/^RESULT //p' "$OUT/$GUEST.txt")
verdict=${res%% *} drift=${res#* }
sed '/^RESULT /d' "$OUT/$GUEST.txt" | sed 's/^/    /'
loads=$(sed -n 's/.* load \([0-9.]* [0-9.]* [0-9.]*\)$/\1/p' "$OUT/$GUEST.txt" | awk '{printf "%s%s", (NR > 1 ? ", " : ""), $1}')
echo "$verdict $GUEST $drift s/h over $MIN min (bound $BOUND s/h, load ${loads:-?}, $(secs) s)" | tee -a "$OUT/summary.txt"
case $verdict in PASS) endrun 0 ;; FAIL) endrun 1 ;; *) endrun 3 ;; esac
