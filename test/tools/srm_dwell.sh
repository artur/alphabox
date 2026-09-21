#!/bin/bash
# Boot a lane to P00>>>, sit at the prompt for N seconds, stop it, and leave
# what the emulator said about itself in the run directory.
#
# usage: PORT=<port> srm_dwell.sh <binary> <label> <dwell_s> [VAR=val ...]
#
# srm_run.sh stops the moment the prompt appears, which is the right thing
# for a regression but measures nothing: the console at its prompt is a busy
# polling loop, and what that loop costs is a real number (see the
# instruction-cache flush entries in docs/performance.md). ALPHABOX_RATE=10
# in the environment prints it every ten seconds.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
BIN=$1; LABEL=$2; DWELL=$3; shift 3
PORT=${PORT:-21993}
D=${ALPHABOX_WORK:-$R/lab}/runs/dwell-$LABEL
rm -rf "$D"; mkdir -p "$D"
cp "$R/test/rom/cl67srmrom.exe" "$D/"
python3 "$T/srm_cfg.py" --out "$D/es40.cfg" --port "$PORT"
cd "$D" || exit 2
env "$@" "$BIN" run > alphabox.out 2>&1 &
PID=$!
python3 - "$PORT" "$DWELL" "$PID" <<'PY'
import socket, sys, time, os, signal
port, dwell, pid = int(sys.argv[1]), float(sys.argv[2]), int(sys.argv[3])
s = None
for _ in range(60):
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=2); break
    except OSError: time.sleep(1)
if s is None: print("status=noconnect"); sys.exit(1)
buf = b""; t0 = time.time()
s.settimeout(1)
while time.time() - t0 < 300:
    try: d = s.recv(4096)
    except socket.timeout: continue
    if not d: break
    buf += d
    if b"P00>>>" in buf: break
print("status=%s after %.1fs" % ("prompt" if b"P00>>>" in buf else "no-prompt", time.time() - t0))
t1 = time.time()
while time.time() - t1 < dwell:          # sit at the prompt, draining output
    try: s.recv(4096)
    except socket.timeout: pass
print("dwelled %.0fs at the prompt" % (time.time() - t1))
os.kill(pid, signal.SIGTERM)
PY
for _ in $(seq 1 30); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && kill -9 $PID
wait $PID 2>/dev/null
echo "log: $D/alphabox.out"
