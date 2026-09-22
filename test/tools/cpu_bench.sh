#!/bin/bash
# How fast does the emulator execute Alpha code?
#
# usage: [BODY=n] [ITER=n] [REPEAT=n] [MIX=alu|mem] [REGS=pinned|spilled] \
#          cpu_bench.sh <alphabox-binary> <label>
#   BODY    instructions per loop iteration (default 32). A long body
#           measures the translated code; a short one (try BODY=4) measures
#           what it costs to leave one block and enter the next.
#   ITER    loop iterations. The default is whatever makes about 38 billion
#           instructions for the BODY asked for, up to the 2.1 billion
#           iterations the image can encode with one LDAH/LDA pair. A long
#           body then runs the loop for eight to twenty seconds and a
#           four-instruction one -- which runs at 7500 MIPS, being one
#           block chained to itself -- for barely one, so the sampling
#           below is fine enough for even that to be many windows.
#   MIX     "alu" (default) or "mem": integer operates only, or a mix with
#           a load and a store in it, which is what guest code really looks
#           like and what the address path costs.
#   REGS    "pinned" (default) or "spilled": whether the loop uses guest
#           registers the JIT keeps in host registers. The difference is
#           what the fixed pin set is worth.
#   REPEAT  runs (default 3). Each one is a measurement of its own now, so
#           the spread across them is a statement about the host, not about
#           the method; the median is reported.
#
# The guest is a boot block that runs a known number of instructions and
# halts (bench_image.py), so nothing else is being measured -- no operating
# system, no idle loop, no devices.
#
# WHAT IS TIMED, AND WHY IT IS NOT THE WALL CLOCK
#
# Until 2026-09-22 this ran the image at two sizes and reported the
# *difference* between the two wall-clock times, to subtract everything
# constant: the console's own boot, the telnet handshake, the exit. The
# trouble is that the constant is not constant and is far larger than the
# signal. A run is about twenty seconds, of which some seventeen are SRM
# booting to its prompt and only two to four are the loop; the console
# driver's connect retry used to quantise the rest to whole seconds. The
# same loop on the same binary measured 3812, 4730 and 5229 MIPS in three
# sittings -- a 37% spread in a tool whose whole purpose is to compare.
#
# So the loop is timed from inside instead. ALPHABOX_RATE makes each
# processor print how many instructions it executed in each window of wall
# time -- a twentieth of a second here -- and the loop is the run's last
# steady stretch of those windows. Measured that way, three runs of the
# same image agree to a few tenths of a per cent, and where they do not,
# the difference is real: a four-instruction loop runs at either 7640 or
# 6260 MIPS depending on where its compiled code lands.
#
# It needs no JIT_STATS build, and should not use one: those counters are
# themselves part of what is being measured. ALPHABOX_RATE is in every
# build, off unless asked for, and looks at the clock once every 256
# batches, which is why it can be read as a measurement.
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
# About 38 billion instructions, whatever the body length is -- but the
# image builds its iteration count with one LDAH/LDA pair, so 2.1 billion
# iterations is the ceiling, and a short body simply runs for less time.
# That is what the fine sampling period below is for.
ITER=${ITER:-$((38400000000 / BODY))}
[ "$ITER" -gt 2100000000 ] && ITER=2100000000
REPEAT=${REPEAT:-3}
REGS=${REGS:-pinned}
MIX=${MIX:-alu}
PORT=${PORT:-21200}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/cpubench-$LABEL

[ -x "$BIN" ] || { echo "cpu_bench: $BIN is not executable"; exit 2; }
rm -rf "$D"; mkdir -p "$D"

image=$D/loop.img
python3 "$T/bench_image.py" "$image" --iterations "$ITER" --body "$BODY" \
  --regs "$REGS" --mix "$MIX" > /dev/null
n_total=$(python3 "$T/bench_image.py" --count --iterations "$ITER" --body "$BODY")
# The prologue is a handful of instructions; the loop is the rest, and it is
# the loop that the windows below will be counting back through.
n_loop=$((ITER * BODY))

echo "== $LABEL: $BODY instructions per iteration, $MIX mix, $REGS registers, $ITER iterations ($n_total instructions)"

# One run: boot the image, wait for the halt, and report the rate the
# processor itself measured over the loop's own windows.
run_once() {
  local tag=$1
  PORT=$PORT ALPHABOX_WORK=$WORK FLOPPY="$image" EXIT_ON_HALT=1 \
    CMDS="boot dva0" AFTER=wait-exit CMD_TIMEOUT=600 ALPHABOX_RATE=0.05 \
    bash "$T/srm_probe.sh" "$BIN" "$LABEL-$tag" 300 > "$D/$tag.log" 2>&1
  # A run that did not reach the halt measured nothing, and a number derived
  # from it is worse than no number: a mistyped path or a port already in use
  # ends the run in a second and turns into six-figure MIPS.
  if ! grep -aq "halted CPU 0\|HALT instruction executed\|HALT invoked" \
       "$D/$tag.log"; then
    echo "  $tag: the image never halted -- see $D/$tag.log" >&2
    exit 3
  fi
  python3 - "$WORK/runs/probe-$LABEL-$tag/alphabox.out" "$n_loop" "$tag" <<'PY'
import re, sys
log, n_loop, tag = sys.argv[1], int(sys.argv[2]), sys.argv[3]
win = []
for line in open(log, errors="replace"):
    m = re.search(r"CPU0-I-RATE: [\d.]+ MIPS \((\d+) instructions in ([\d.]+) s\)",
                  line)
    if m:
        win.append((int(m.group(1)), float(m.group(2))))
# Cut the run into stretches of windows that agree with each other, and
# take the last long one. A run is the firmware (hundreds of windows at
# about 1600 MIPS), then the loop at its own rate, then a window or two of
# halting and exiting. Segmenting assumes nothing about which stretch is
# faster -- only that the loop is the last steady one of any length, which
# holds for a four-instruction loop at 7600 MIPS and would hold for one
# slower than the firmware.
#
# Two rules that look reasonable and are not, both learned here:
#
# Do not count backwards until the instructions add up to the loop's known
# length. The window that gets printed is the one that has just completed,
# so the loop's last fraction of a second never appears, the printed
# windows never add up to the whole loop, and the count walks back into
# the firmware: that read 3804 MIPS for a loop running at 4470.
#
# Do not anchor on the run's last window either. How many windows the halt
# and the console's exit take is not fixed -- sometimes one, sometimes two
# -- so anchoring there can put the entire loop a step away and leave a
# single window standing.
def rate(w):
    return w[0] / w[1]

segs, cur = [], []
for w in win:
    if w[1] <= 0:
        continue
    if cur:
        rates = sorted(rate(x) for x in cur)
        med = rates[len(rates) // 2]
        if med <= 0 or abs(rate(w) - med) / med > 0.10:
            segs.append(cur)
            cur = []
    cur.append(w)
if cur:
    segs.append(cur)
long_enough = [s for s in segs if len(s) >= 6]
used = long_enough[-1] if long_enough else []
# The first and last window of the stretch each share their time with
# whatever came before or after; with windows to spare, leave them out.
if len(used) >= 8:
    used = used[1:-1]
instr = sum(d for d, _ in used)
secs = sum(s for _, s in used)
if len(used) < 6 or secs <= 0:
    sys.exit("  %s: only %d clean window(s) of loop -- raise ITER"
             % (tag, len(used)))
# A window at either end of the stretch can hold a little of what came
# before or after, so a few per cent over the loop's length is the method
# working, not failing. An order of magnitude over means the stretch that
# was taken for the loop is some other part of the run.
if instr > n_loop * 1.05:
    sys.exit("  %s: %d loop instructions found but only %d were run -- what "
             "was taken for the loop cannot be it" % (tag, instr, n_loop))
print("%.0f %d %.3f" % (instr / secs / 1e6, len(used), secs))
PY
}

rates=""
for i in $(seq 1 "$REPEAT"); do
  # A run that measured nothing must stop the tool: carried forward, an
  # empty measurement becomes an empty median and a crash three lines down.
  out=$(run_once "run$i") || exit 3
  [ -n "$out" ] || { echo "  run$i: no measurement -- see $D/run$i.log"; exit 3; }
  read -r mips used secs <<< "$out"
  printf '    run %d: %s MIPS (%s windows, %ss of loop)\n' "$i" "$mips" "$used" "$secs"
  rates="$rates $mips"
done

python3 - "$BODY" $rates <<'PY'
import sys
body = int(sys.argv[1])
r = sorted(float(x) for x in sys.argv[2:])
med = r[len(r) // 2] if len(r) % 2 else (r[len(r) // 2 - 1] + r[len(r) // 2]) / 2
spread = (r[-1] - r[0]) / med * 100 if med else 0
print("  %.0f MIPS  (median of %d, spread %.1f%%, body %d)"
      % (med, len(r), spread, body))
print("  EV7z 1.30 GHz was about 10300 MIPS: %.1f%% of it" % (med / 10300 * 100))
PY
