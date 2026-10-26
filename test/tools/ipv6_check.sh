#!/bin/bash
# The emulator's own network endpoints over IPv6 (docs/configuration.md),
# each against the SRM console of the test machine, one emulator at a time:
#
#   serial6  serial0 on address "::1": a client over IPv6 reaches P00>>>
#            and runs a command;
#   dual     serial0 on "::" (dual stack): a client over IPv4 (127.0.0.1)
#            runs a command and leaves, then one over IPv6 (::1) comes back
#            to the prompt and runs another;
#   gdb6     the gdb stub (it listens on 127.0.0.1 and ::1): a minimal
#            remote protocol exchange over ::1, the stop reply and the
#            registers (g; the PC at reset is 0x8000), then a detach, after
#            which the console reaches its prompt;
#   udp6     a DE500 on the UDP back end with both ends on ::1 and
#            net_peer.py at the other: "boot ewa0 -protocols bootp" gets
#            the BOOTP reply and the TFTP image, and runs it to its HALT.
#
# usage: PORT=<base port> ipv6_check.sh <alphabox> <label> [section ...]
#   Uses ports PORT to PORT+6 (default 21860). Runs in
#   $ALPHABOX_WORK/runs/ipv6-<label>/<section> (ALPHABOX_WORK defaults to
#   <repo>/lab). Sections: the four above, all by default.
#
# Prints a PASS/FAIL line per check and PASS or FAIL last; exit status 0
# when every check passed. On a host without an IPv6 loopback it says so,
# with the commands that add one, and exits 2.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { sed -n '2,29p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
shift 2
SECTIONS=("$@")
[ ${#SECTIONS[@]} -gt 0 ] || SECTIONS=(serial6 dual gdb6 udp6)
PORT=${PORT:-21860}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/ipv6-$LABEL
[ -x "$BIN" ] || { echo "ipv6_check: $BIN is not executable"; exit 2; }

if ! python3 -I -c 'import socket; s = socket.socket(socket.AF_INET6); s.bind(("::1", 0))' 2>/dev/null; then
  echo "ipv6_check: this host has no IPv6 loopback (::1). To add one:"
  echo "  Linux: sudo sysctl -w net.ipv6.conf.lo.disable_ipv6=0 && sudo ip -6 addr add ::1/128 dev lo"
  echo "  macOS: sudo ifconfig lo0 inet6 ::1 prefixlen 128 alias"
  exit 2
fi
rm -rf "$D" && mkdir -p "$D" || exit 2

fails=0
check() { # <name> <condition...>
  local name=$1
  shift
  if "$@"; then
    echo "PASS: $name"
  else
    echo "FAIL: $name"
    fails=$((fails + 1))
  fi
}
in_log() { grep -aqE "$2" "$1"; }
wait_for() { # <file> <pattern> <seconds>
  local i
  for ((i = 0; i < $3 * 10; i++)); do
    grep -aqE "$2" "$1" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

PID=
start_emu() { # in the section's directory
  "$BIN" run > alphabox.out 2>&1 &
  PID=$!
}
stop_emu() { # our own emulator only: SIGTERM, then SIGKILL after 30 s
  [ -n "$PID" ] || return
  if kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null
    for ((i = 0; i < 300; i++)); do
      kill -0 "$PID" 2>/dev/null || break
      sleep 0.1
    done
    kill -9 "$PID" 2>/dev/null
  fi
  wait "$PID" 2>/dev/null
  PID=
}
PEER=
CON=
cleanup() {
  [ -n "$CON" ] && kill "$CON" 2>/dev/null
  [ -n "$PEER" ] && kill "$PEER" 2>/dev/null
  stop_emu
}
trap cleanup EXIT

section() { # <name> [srm_cfg.py options...]: a fresh directory and machine
  local name=$1
  shift
  echo "== $name"
  mkdir -p "$D/$name" && cd "$D/$name" || exit 2
  cp "$R/test/rom/cl67srmrom.exe" . || exit 2
  python3 -I "$T/srm_cfg.py" --out es40.cfg "$@" || exit 2
}
no_mismatch() {
  local mm
  mm=$(grep -ac MISMATCH alphabox.out)
  check "JIT_VERIFY mismatches: $mm" [ "$mm" -eq 0 ]
}

run_serial6() {
  local p=$PORT
  section serial6 --port $p --serial-address ::1
  start_emu
  python3 -I "$T/srm_console.py" --host ::1 --port $p --log axp.log \
    --cmd-log cmds.txt --timeout 300 --cmd "show version" --pid "$PID" \
    --after sigterm --exit-timeout 30 > console.status 2>&1
  stop_emu
  check "serial0 listens on [::1]:$p" in_log alphabox.out "Waiting for connection on \[::1\]:$p\.$"
  check "the client connected over IPv6" in_log console.status "^connected=\[::1\]:$p$"
  check "and is reported as [::1]" in_log alphabox.out "client \[::1\]:[0-9]+ connected"
  check "SRM reached P00>>> over IPv6" in_log console.status '^status=prompt'
  check "show version answered" in_log cmds.txt '^version[[:space:]]+V7\.3'
  no_mismatch
}

run_dual() {
  local p=$((PORT + 1))
  section dual --port $p --serial-address ::
  start_emu
  python3 -I "$T/srm_console.py" --host 127.0.0.1 --port $p --log axp4.log \
    --cmd-log cmds4.txt --timeout 300 --cmd "show version" > console4.status 2>&1
  python3 -I "$T/srm_console.py" --host ::1 --port $p --log axp6.log \
    --cmd-log cmds6.txt --timeout 60 --wake --cmd "show cpu" --pid "$PID" \
    --after sigterm --exit-timeout 30 > console6.status 2>&1
  stop_emu
  check "serial0 listens on [::]:$p, dual stack" in_log alphabox.out "Waiting for connection on \[::\]:$p \(IPv4 too\)\.$"
  check "an IPv4 client connected (127.0.0.1)" in_log console4.status "^connected=127\.0\.0\.1:$p$"
  check "and is reported as IPv4" in_log alphabox.out 'client 127\.0\.0\.1:[0-9]+ connected'
  check "it reached P00>>> and ran show version" bash -c "grep -aq '^status=prompt' console4.status && grep -aqE '^version[[:space:]]+V7\.3' cmds4.txt"
  check "the port took the next client after it left" in_log alphabox.out 'SRL-W-DISCONNECT'
  check "an IPv6 client connected next (::1)" in_log console6.status "^connected=\[::1\]:$p$"
  check "and is reported as [::1]" in_log alphabox.out 'client \[::1\]:[0-9]+ connected'
  check "it got the prompt and ran show cpu" bash -c "grep -aq '^status=prompt' console6.status && grep -aqE 'Primary CPU|CPU 0' cmds6.txt"
  no_mismatch
}

run_gdb6() {
  local p=$((PORT + 2)) g=$((PORT + 3))
  section gdb6 --port $p --gdb $g
  start_emu
  # The machine waits for its console client before the stub starts.
  python3 -I "$T/srm_console.py" --port $p --log axp.log --timeout 300 \
    > console.status 2>&1 &
  CON=$!
  if ! wait_for alphabox.out GDB-I-LISTEN 120; then
    check "the stub listened" false
    stop_emu
    return
  fi
  check "the stub listens on both loopbacks" in_log alphabox.out "GDB-I-LISTEN: gdb stub on 127\.0\.0\.1:$g and \[::1\]:$g;"
  # A minimal remote protocol session over ::1: ?, g, D.
  python3 -I - ::1 "$g" > rsp.log 2>&1 <<'EOF'
import socket
import sys

s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=20)
peer = s.getpeername()
print("connected=" + ("[%s]:%d" % peer[:2] if s.family == socket.AF_INET6
                      else "%s:%d" % peer[:2]))
buf = b""


def packet():
    """The next packet's body, acknowledged; '+' acks before it skipped."""
    global buf
    while True:
        i = buf.find(b"$")
        j = buf.find(b"#", i + 1) if i >= 0 else -1
        if j >= 0 and len(buf) >= j + 3:
            body, buf = buf[i + 1:j], buf[j + 3:]
            s.sendall(b"+")
            return body.decode()
        d = s.recv(65536)
        if not d:
            sys.exit("connection closed")
        buf += d


def command(c):
    s.sendall(b"$%s#%02x" % (c.encode(), sum(c.encode()) & 0xff))
    return packet()


print("stop=" + command("?"))
regs = command("g")
print("g=%d hex digits" % len(regs))
# gdb's Alpha numbering: r0-r31, f0-f31, pc (64), 64 bits each, target order.
print("pc=0x%x" % int.from_bytes(bytes.fromhex(regs[64 * 16:65 * 16]), "little"))
print("detach=" + command("D"))
EOF
  check "connected to the stub over IPv6" in_log rsp.log '^connected=\[::1\]:'
  check "the stub reports the peer" in_log alphabox.out 'GDB-I-ATTACH: gdb attached from \[::1\]:[0-9]+'
  check "stop reply" in_log rsp.log '^stop=T05'
  check "g returned the 67 registers" in_log rsp.log '^g=1072 hex digits$'
  check "the PC at reset is 0x8000" in_log rsp.log '^pc=0x8000$'
  check "detach answered OK" in_log rsp.log '^detach=OK$'
  wait "$CON"
  CON=
  check "SRM reached P00>>> after the detach" in_log console.status '^status=prompt'
  stop_emu
  no_mismatch
}

run_udp6() {
  local p=$((PORT + 4)) nic=$((PORT + 5)) peer=$((PORT + 6))
  section udp6 --port $p --nic dec21143 --nic-udp $nic:$peer --nic-udp-host ::1
  python3 -I "$T/net_peer.py" --host ::1 --listen $peer --nic $nic \
    --timeout 400 > peer.log 2>&1 &
  PEER=$!
  start_emu
  python3 -I "$T/srm_console.py" --port $p --log axp.log --cmd-log cmds.txt \
    --timeout 300 --cmd "boot ewa0 -protocols bootp" --cmd-timeout 120 \
    --pid "$PID" --after sigterm --exit-timeout 30 > console.status 2>&1
  stop_emu
  kill "$PEER" 2>/dev/null
  wait "$PEER" 2>/dev/null
  PEER=
  check "the NIC's UDP link is [::1]:$nic <-> [::1]:$peer" in_log alphabox.out "frames over UDP, \[::1\]:$nic <-> \[::1\]:$peer"
  check "net_peer.py on [::1]:$peer" in_log peer.log "listening on \[::1\]:$peer, the NIC at \[::1\]:$nic"
  check "BOOTP request crossed" in_log peer.log 'BOOTP request'
  check "the TFTP transfer completed" in_log peer.log 'TFTP transfer complete'
  check "the image ran to its HALT" in_log cmds.txt 'HALT instruction executed'
  no_mismatch
}

for s in "${SECTIONS[@]}"; do
  case $s in
  serial6) run_serial6 ;;
  dual) run_dual ;;
  gdb6) run_gdb6 ;;
  udp6) run_udp6 ;;
  *) echo "ipv6_check: no section $s"; exit 2 ;;
  esac
done

cleanup
trap - EXIT
if [ $fails -eq 0 ]; then
  echo "PASS ($D)"
  exit 0
fi
echo "FAIL: $fails check(s) ($D)"
exit 1
