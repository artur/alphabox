#!/bin/bash
# How fast does the emulator execute Alpha code?
#
# usage: [BODY=n] [ITER=n] [REPEAT=n] cpu_bench.sh <alphabox-binary> <label>
#   BODY    instructions per loop iteration (default 32). A long body
#           measures the translated code; a short one (try BODY=4) measures
#           what it costs to leave one block and enter the next.
#   ITER    iterations of the smaller of the two runs (default 300000000).
#           Keep it large: while the console sits at its prompt it polls at
#           over a hundred MIPS, so a loop that finishes in a fraction of a
#           second is lost in the noise of when the prompt appeared. At the
#           default the two runs take roughly five and ten seconds of pure
#           loop, which is well clear of it.
#   MIX     "alu" (default) or "mem": integer operates only, or a mix with
#           a load and a store in it, which is what guest code really looks
#           like and what the address path costs.
#   REGS    "pinned" (default) or "spilled": whether the loop uses guest
#           registers the JIT keeps in host registers. The difference is
#           what the fixed pin set is worth.
#   REPEAT  measurements per size (default 3); the best of each is used,
#           because a slower run only ever means the host was busy.
#
# The guest is a boot block that runs a known number of instructions and
# halts (bench_image.py), so nothing else is being measured -- no operating
# system, no idle loop, no devices. The same image is run at two sizes and
# the *difference* is reported, which subtracts everything constant: the
# console's own boot, the telnet handshake, the exit.
#
# It needs no JIT_STATS build, and should not use one: the counters that
# print the throughput are themselves part of what is being measured. This
# measures the emulator people actually run.
#
# Prints instructions per second as MIPS. For scale, the fastest Alpha ever
# built, the EV7z at 1.30 GHz, was rated at about 10300 MIPS.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { sed -n '2,20p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
BODY=${BODY:-32}
ITER=${ITER:-300000000}
REPEAT=${REPEAT:-3}
REGS=${REGS:-pinned}
MIX=${MIX:-alu}
PORT=${PORT:-21200}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/cpubench-$LABEL

[ -x "$BIN" ] || { echo "cpu_bench: $BIN is not executable"; exit 2; }
rm -rf "$D"; mkdir -p "$D"

# One run: boot the image, wait for the halt, report the seconds it took.
run_once() {
  local image=$1 port=$2 start end
  start=$(python3 -c 'import time; print(time.time())')
  # The loop runs for as long as it runs: srm_probe's per-command timeout is
  # a minute by default, which a memory-heavy image passes straight through.
  PORT=$port ALPHABOX_WORK=$WORK FLOPPY="$image" EXIT_ON_HALT=1 \
    CMDS="boot dva0" AFTER=wait-exit CMD_TIMEOUT=600 \
    bash "$T/srm_probe.sh" "$BIN" "$LABEL-$3" 300 > "$D/$3.log" 2>&1
  end=$(python3 -c 'import time; print(time.time())')
  # A run that did not reach the halt measured nothing, and a number derived
  # from it is worse than no number: a mistyped path or a port already in use
  # ends the run in a second and turns into six-figure MIPS.
  if ! grep -aq "halted CPU 0\|HALT instruction executed\|HALT invoked" \
       "$D/$3.log"; then
    echo "  $3: the image never halted -- see $D/$3.log" >&2
    exit 3
  fi
  python3 -c "print('%.3f' % ($end - $start))"
}

best_of() {
  local image=$1 tag=$2 port=$3 i t best=
  for i in $(seq 1 "$REPEAT"); do
    t=$(run_once "$image" "$port" "$tag-$i")
    echo "    $tag run $i: ${t}s" >&2
    best=$(python3 -c "
best = '$best'
print('$t' if not best or float('$t') < float(best) else best)")
  done
  echo "$best"
}

small=$D/small.img
large=$D/large.img
python3 "$T/bench_image.py" "$small" --iterations "$ITER" --body "$BODY" --regs "$REGS" --mix "$MIX" > /dev/null
python3 "$T/bench_image.py" "$large" --iterations $((ITER * 2)) --body "$BODY" --regs "$REGS" --mix "$MIX" > /dev/null
n_small=$(python3 "$T/bench_image.py" --count --iterations "$ITER" --body "$BODY")
n_large=$(python3 "$T/bench_image.py" --count --iterations $((ITER * 2)) --body "$BODY")

echo "== $LABEL: $BODY instructions per iteration, $MIX mix, $REGS registers, $ITER and $((ITER * 2)) iterations"
t_small=$(best_of "$small" small "$PORT")
t_large=$(best_of "$large" large "$((PORT + 1))")

python3 - "$n_small" "$n_large" "$t_small" "$t_large" "$BODY" <<'PY'
import sys
n1, n2 = int(sys.argv[1]), int(sys.argv[2])
t1, t2 = float(sys.argv[3]), float(sys.argv[4])
body = int(sys.argv[5])
dt = t2 - t1
if dt <= 0.2:
    sys.exit("  the two runs took the same time (%.3fs apart): raise ITER, or\n"
             "  something made both runs stop early -- check the logs" % dt)
mips = (n2 - n1) / dt / 1e6
print("  %.0f MIPS  (%d instructions in %.3fs, body %d)"
      % (mips, n2 - n1, dt, body))
print("  EV7z 1.30 GHz was about 10300 MIPS: %.1f%% of it" % (mips / 10300 * 100))
PY
