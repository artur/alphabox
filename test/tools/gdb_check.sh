#!/bin/bash
# The guest debugger against the SRM console (docs/debugging.md): a scripted
# gdb attaches at reset, stops at a breakpoint, reads registers and memory,
# single-steps, runs the monitor commands, lets the console reach P00>>>,
# attaches again at the prompt to break inside a loop that is running (and,
# in a JIT build, compiled), interrupts a running machine with Ctrl-C, and
# detaches. Prints one PASS/FAIL line per check and PASS or FAIL overall.
#
# usage: PORT=<serial port> GDB_PORT=<gdb port> gdb_check.sh <alphabox> <label> [cpus]
#   Runs in $ALPHABOX_WORK/runs/gdb-<label> (ALPHABOX_WORK defaults to
#   <repo>/lab). GDB defaults to /opt/homebrew/bin/gdb, or the gdb on PATH.
#   GDB_HOST is where gdb connects (default localhost; 127.0.0.1, or
#   [::1] for IPv6: the stub listens on both).
#   Exit status 0 when every check passed.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || {
  echo "usage: PORT=<port> GDB_PORT=<port> $0 <alphabox-binary> <label> [cpus]"
  exit 2
}
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
CPUS=${3:-1}
PORT=${PORT:-21000}
GDB_PORT=${GDB_PORT:-$((PORT + 1))}
GDB=${GDB:-/opt/homebrew/bin/gdb}
GDB_HOST=${GDB_HOST:-localhost}
[ -x "$GDB" ] || GDB=$(command -v gdb)
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/gdb-$LABEL
[ -x "$BIN" ] || { echo "gdb_check: $BIN is not executable"; exit 2; }
[ -n "$GDB" ] || { echo "gdb_check: no gdb"; exit 2; }

rm -rf "$D" && mkdir -p "$D" || exit 2
cp "$R/test/rom/cl67srmrom.exe" "$D/"
python3 "$T/srm_cfg.py" --out "$D/es40.cfg" --port "$PORT" --cpus "$CPUS" \
  --gdb "$GDB_PORT" || exit 2
cd "$D" || exit 2

# A command with a deadline: macOS has no timeout(1).
deadline() { perl -e 'alarm shift; exec @ARGV' "$@"; }
wait_for() { # <file> <pattern> <seconds>
  local i
  for ((i = 0; i < $3 * 10; i++)); do
    grep -aq "$2" "$1" 2>/dev/null && return 0
    sleep 0.1
  done
  return 1
}

"$BIN" run > alphabox.out 2>&1 &
PID=$!
python3 "$T/srm_console.py" --port "$PORT" --log axp.log --timeout 900 \
  > console.status 2>&1 &
CPID=$!
cleanup() {
  kill "$CPID" 2>/dev/null
  if kill -0 "$PID" 2>/dev/null; then
    kill -TERM "$PID" 2>/dev/null # our own emulator only
    for ((i = 0; i < 300 && $(kill -0 "$PID" 2>/dev/null && echo 1 || echo 0); i++)); do
      sleep 0.1
    done
    kill -9 "$PID" 2>/dev/null
  fi
}
trap cleanup EXIT

wait_for alphabox.out GDB-I-LISTEN 120 || {
  echo "FAIL: the stub never listened"
  tail -5 alphabox.out
  exit 1
}

# Session 1, at reset: the console's first instruction is the PALcode's
# entry at 0x8000 (PALmode). 0x13880 is the console routine its second
# instruction calls.
cat > s1.gdb <<EOF
set pagination off
set confirm off
set heuristic-fence-post 64
target remote $GDB_HOST:$GDB_PORT
echo @@ threads\n
info threads
echo @@ pc-at-reset\n
printf "PC=%#lx\n", \$pc
x/2i \$pc
break *0x13880
continue
echo @@ hit\n
printf "PC=%#lx\n", \$pc
info registers
x/8i \$pc
echo @@ stepi\n
stepi
printf "STEP1=%#lx\n", \$pc
stepi
printf "STEP2=%#lx\n", \$pc
stepi
printf "STEP3=%#lx\n", \$pc
echo @@ memory\n
x/4gx 0x8000
monitor pr 0x8000 4
set \$r = \$t0
set \$t0 = 0x1234abcd
printf "T0=%#lx\n", \$t0
set \$t0 = \$r
set {int}0x8010 = 0x5a5aa5a5
printf "W=%#x\n", {int}0x8010
set {int}0x8010 = 0
echo @@ monitor\n
monitor cpu
monitor ipr
monitor irq
monitor tlb
monitor xlate 0x8000
monitor jit
echo @@ smp\n
info threads
thread $CPUS
printf "THREAD$CPUS-PC=%#lx\n", \$pc
thread 1
delete
echo @@ detach\n
detach
EOF
deadline 120 "$GDB" --batch -nx -x s1.gdb > s1.log 2>&1

# The console runs on to its prompt with nobody attached.
wait_for console.status status=prompt 300

# Session 2, at the prompt: attaching stops the machine wherever it is.
# 0x5c0fc-0x5c104 is the loop where the console's scheduler walks its
# process queue, which it keeps doing at the prompt; in a JIT build it is
# compiled by now, and 0x5c104 is inside a compiled block. The breakpoint
# must stop it exactly there. Then the cycle counter across a stop, then
# stepping from the head of that compiled block: gdb steps an Alpha by
# planting a breakpoint on the next instruction (alpha-tdep's software
# single step), which lands inside the block; "maint packet" sends the
# stub's own step once. Then Ctrl-C: gdb is sent SIGINT while it waits in
# "continue", as a terminal would.
cat > s2.gdb <<EOF
set pagination off
set confirm off
set heuristic-fence-post 64
set breakpoint always-inserted on
target remote $GDB_HOST:$GDB_PORT
echo @@ attached\n
printf "PC=%#lx\n", \$pc
info threads
set \$bp = 0x5c104
echo @@ jit-before\n
eval "monitor jit %#lx", \$bp
break *\$bp
continue
echo @@ loop-hit\n
printf "PC=%#lx BP=%#lx\n", \$pc, \$bp
echo @@ jit-after\n
eval "monitor jit %#lx", \$bp
echo @@ clock\n
thread 1
monitor ipr
shell sleep 2
continue
thread 1
monitor ipr
delete
echo @@ step-compiled\n
break *0x5c0fc
continue
delete
printf "HEAD=%#lx THREAD=%d\n", \$pc, \$_thread
eval "monitor jit %#lx", \$pc + 4
monitor cpu
stepi
printf "CSTEP1=%#lx\n", \$pc
monitor cpu
stepi
printf "CSTEP2=%#lx\n", \$pc
monitor cpu
eval "maint packet vCont;s:%x", \$_thread
maint flush register-cache
printf "HWSTEP=%#lx\n", \$pc
echo @@ interrupt\n
shell (sleep 1; kill -INT \$PPID) > /dev/null 2>&1 &
continue
printf "INT-PC=%#lx\n", \$pc
thread $CPUS
printf "THREAD$CPUS-PC=%#lx\n", \$pc
thread 1
detach
EOF
deadline 120 "$GDB" --batch -nx -x s2.gdb > s2.log 2>&1

# Session 3, two or more processors: every breakpoint hit is reported. The
# interrupt entry (0x8680) is where all the processors go on the interval
# timer's tick, and they tick together, so they often hit it at the same
# moment; whichever the stub reports, the others' hits must come back as
# stops of their own. The stub counts the hits it records for each
# processor ("breakpoint hits" in "monitor cpu"); gdb's own trace of the
# protocol counts the stop replies that carry one (a continue from a
# breakpoint is a step over it, with a breakpoint of gdb's on the next
# instruction, so there are two replies per stop gdb shows).
if [ "$CPUS" -gt 1 ]; then
  cat > s3.gdb <<EOF
set pagination off
set confirm off
set breakpoint always-inserted on
target remote $GDB_HOST:$GDB_PORT
break *0x8680
python
import gdb, re
def hits():
    out = gdb.execute("monitor cpu", to_string=True)
    return [int(x) for x in re.findall(r"breakpoint hits ([0-9]+)", out)]
before = hits()
gdb.execute("set debug remote 1")
seen = {}
for i in range(1000):
    gdb.execute("continue")
    t = gdb.selected_thread().num
    seen[t] = seen.get(t, 0) + 1
gdb.execute("set debug remote 0")
after = hits()
print("@@ twohits")
for i in range(len(after)):
    print("CPU%d SHOWN=%d RECORDED=%d" % (i, seen.get(i + 1, 0), after[i] - before[i]))
end
delete
detach
EOF
  deadline 240 "$GDB" --batch -nx -x s3.gdb > s3.log 2>&1
fi

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
# The lines of a gdb log between "@@ <name>" and the next "@@".
sect() { awk -v s="@@ $2" '$0==s{f=1;next} /^@@ /{f=0} f' "$1"; }
in_sect() { sect "$1" "$2" | grep -aqE "$3"; }

nthreads=$(sect s1.log threads | grep -acE '^[* ] +[0-9]+ +Thread')
check "attach at reset: $CPUS thread(s) (saw $nthreads)" [ "$nthreads" -eq "$CPUS" ]
check "stopped at the console's first instruction (0x8000)" in_log s1.log '^PC=0x8000$'
check "breakpoint 0x13880 reached" in_sect s1.log hit '^PC=0x13880$'
check "info registers lists the Alpha registers" in_log s1.log '^(sp|ra|fpcr) +0x'
check "x/8i disassembles at the PC" in_log s1.log '^=> 0x13880:'
s1=$(grep -a '^STEP1=' s1.log | cut -d= -f2)
s2=$(grep -a '^STEP2=' s1.log | cut -d= -f2)
s3=$(grep -a '^STEP3=' s1.log | cut -d= -f2)
check "stepi moves the PC (0x13880 -> $s1 -> $s2 -> $s3)" \
  [ -n "$s1" -a -n "$s3" -a "$s1" != 0x13880 -a "$s2" != "$s1" -a "$s3" != "$s2" ]
check "memory read by address" in_log s1.log '^0x8000:[[:space:]]+0x'
check "monitor pr reads physical memory" in_log s1.log '^0000000000008000: [0-9a-f]{16} [0-9a-f]{16}'
check "register write" in_log s1.log '^T0=0x1234abcd$'
check "memory write" in_log s1.log '^W=0x5a5aa5a5$'
check "monitor cpu" in_log s1.log '^cpu0: (stopped|running)'
check "monitor ipr" in_log s1.log '^  PAL_BASE +0000000000008000'
check "monitor irq" in_log s1.log '^cpu0: external'
check "monitor tlb" in_log s1.log '^ITB \(current ASN'
check "monitor xlate" in_log s1.log '^0000000000008000(: not mapped| -> )'
check "monitor jit" in_log s1.log '(cpu0 JIT: epoch|this build has no JIT)'
check "thread $CPUS readable" in_log s1.log "^THREAD$CPUS-PC=0x"
check "SRM reached P00>>> after the detach" grep -aq '^status=prompt' console.status
check "attach at the prompt" in_log s2.log '^PC=0x'
hit=$(grep -a '^PC=.* BP=' s2.log | head -1)
bp=$(echo "$hit" | sed 's/.*BP=//')
pc=$(echo "$hit" | sed 's/^PC=//; s/ BP.*//')
check "breakpoint inside the running prompt loop hit (PC $pc, breakpoint $bp)" \
  [ -n "$bp" -a "$pc" = "$bp" ]
if in_sect s2.log jit-before 'cpu0 JIT'; then
  check "the loop was compiled before the breakpoint went in" \
    in_sect s2.log jit-before 'compiled prefix'
  check "and runs interpreted while it is there" \
    in_sect s2.log jit-after 'a breakpoint is in it: runs interpreted'
fi
# Two seconds stopped, then the loop once more: cpu0's cycle counter must
# have moved by the time it ran, not by the time it was stopped (it gets
# at most a second at a time, so an unfrozen clock shows 800 million).
cc=($(sect s2.log clock | awk '$1 == "CC" {print $2}'))
dcc=$(( ${#cc[@]} == 2 ? 0x${cc[1]} - 0x${cc[0]} : -1 ))
check "guest time stood still while stopped (CC moved $dcc cycles)" \
  [ "$dcc" -gt 0 -a "$dcc" -lt 400000000 ]
check "stepi from the head of a compiled block stops on each instruction" \
  bash -c "grep -aq '^HEAD=0x5c0fc ' s2.log && grep -aq '^CSTEP1=0x5c100$' s2.log && grep -aq '^CSTEP2=0x5c104$' s2.log"
# Each step ran one instruction: a compiled block that ran past the
# breakpoint gdb plants on the next instruction would come back to it only
# after another trip round the loop.
# (On two processors either may be the one in the loop: the counts are
# that one's.)
st=$(grep -a '^HEAD=' s2.log | sed 's/.*THREAD=//')
ic=($(sect s2.log step-compiled | awk -v c="cpu$((${st:-1} - 1)):" '$1 == c {
  for (i = 1; i < NF; i++) if ($i == "instructions") print $(i + 1) }'))
steps="${#ic[@]}:$(( ${#ic[@]} == 3 ? ic[1] - ic[0] : -1 )),$(( ${#ic[@]} == 3 ? ic[2] - ic[1] : -1 ))"
check "each of those steps ran one instruction (counts $steps)" [ "$steps" = "3:1,1" ]
check "and the stub's own single step (vCont;s) takes one instruction" \
  in_log s2.log '^HWSTEP=0x5c10[8c]$|^HWSTEP=0x5c160$'
check "Ctrl-C stops a running machine" in_log s2.log 'received signal SIGINT'
check "and it stopped somewhere" in_log s2.log '^INT-PC=0x[0-9a-f]+'
check "$CPUS thread(s) at the prompt" \
  [ "$(sect s2.log attached | grep -acE '^[* ] +[0-9]+ +Thread')" -eq "$CPUS" ]
check "the machine runs after the last detach" in_log alphabox.out 'GDB-I-DETACH'
if [ "$CPUS" -gt 1 ]; then
  check "thread $CPUS runs (started by the console)" \
    in_log s2.log "^THREAD$CPUS-PC=0x[0-9a-f]{2,}"
fi

if [ "$CPUS" -gt 1 ]; then
  lost=0
  both=1
  while read -r name shown rec; do
    shown=${shown#SHOWN=}
    rec=${rec#RECORDED=}
    n=${name#CPU}
    sent=$(grep -ac "Packet received: T05thread:$((n + 1));swbreak" s3.log)
    [ "$shown" -gt 0 ] || both=0
    lost=$((lost + rec - sent))
  done < <(sect s3.log twohits | grep -a '^CPU')
  check "every breakpoint hit by $CPUS processors is reported ($lost lost)" \
    [ "$lost" -eq 0 ]
  check "and each of the processors hit it, so that was tested" [ "$both" -eq 1 ]
fi

# The console's output is what it is without a debugger: guest time stood
# still while it was stopped.
norm() { tr -d '\000' < "$1" | sed 's/CPU [0-9] speed is.*//'; }
if [ "$CPUS" -eq 1 ]; then
  check "console log matches test/rom/axp_correct.log" \
    diff -q <(norm "$R/test/rom/axp_correct.log") <(norm axp.log)
fi
mm=$(grep -ac MISMATCH alphabox.out)
check "JIT_VERIFY mismatches: $mm" [ "$mm" -eq 0 ]

cleanup
trap - EXIT
if [ $fails -eq 0 ]; then
  echo "PASS ($D)"
  exit 0
fi
echo "FAIL: $fails check(s) ($D: s1.log, s2.log, alphabox.out)"
exit 1
