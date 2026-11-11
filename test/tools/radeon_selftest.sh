#!/bin/bash
# The Radeon's self-test (ALPHABOX_RADEON_SELFTEST, docs/radeon.md "How it is
# checked"): the card's engines driven as a driver drives them, before any
# machine starts, each result against a reference of the test's own. Run
# twice: with the engine on its thread, and synchronous
# (ALPHABOX_RADEON_SYNC=1), which leaves the queue checks out.
#
# usage: radeon_selftest.sh <alphabox-binary> [-v]
#   Runs in $ALPHABOX_WORK/radeon-selftest (ALPHABOX_WORK defaults to
#   <repo>/lab), which must hold the card's BIOS image as vgabios.bin (it is
#   firmware, so it is not in the repository; docs/configuration.md says
#   which file); the configuration is written there when there is none. No
#   console firmware is needed: the emulator ends when the test has.
#   RADEON_SELFTEST_TMO: seconds a run may take (default 300). A check that
#   never ends, which is how a lock taken twice or an endless walk shows, is
#   a failure: the run is stopped (only the emulator this script started).
#   -v lists every check, not only the failures.
#
# Prints the FAILED checks and one line per run, `PASS|FAIL <mode> <checks>`.
# The frames go to $ALPHABOX_WORK/radeon-3d. Exit status: 0 when both runs
# passed, 2 when the test could not run.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 1 ] || { sed -n '2,21p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
VERBOSE=${2:-}
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/radeon-selftest
TMO=${RADEON_SELFTEST_TMO:-300}
[ -x "$BIN" ] || { echo "radeon_selftest: $BIN is not executable"; exit 2; }
"$BIN" --version | grep -q SDL3 ||
  { echo "radeon_selftest: this build has no SDL, and the card needs a GUI"; exit 2; }
mkdir -p "$D" "$WORK/radeon-3d" || exit 2
[ -f "$D/vgabios.bin" ] ||
  { echo "radeon_selftest: no $D/vgabios.bin (the card's BIOS image)"; exit 2; }
if [ ! -f "$D/es40.cfg" ]; then
  cat > "$D/es40.cfg" <<'EOF'
gui = sdl
{
  keyboard.use_mapping = false;
}

sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "cl67srmrom.exe";
  rom.decompressed = "decompressed.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  cpu0 = ev68cb
  {
    speed = 800M;
  }
  serial0 = serial
  {
    null_attach = true;
  }
  pci0.2 = radeon
  {
    rom = "vgabios.bin";
  }
  pci0.7 = ali
  {
    vga_console = true;
  }
}
EOF
fi

fails=0
run() { # <mode> [VAR=value]
  local mode=$1 out=$D/selftest-$1.out pid i
  ( cd "$D" && exec env SDL_VIDEO_DRIVER=dummy ALPHABOX_RADEON_SELFTEST=exit \
      ALPHABOX_RADEON_SELFTEST_DIR="$WORK/radeon-3d" ${2:-} \
      "$BIN" run es40.cfg ) > "$out" 2>&1 < /dev/null &
  pid=$!
  for ((i = 0; i < TMO * 2; i++)); do
    kill -0 $pid 2>/dev/null || break
    sleep 0.5
  done
  local stopped=""
  if kill -0 $pid 2>/dev/null; then
    kill -9 $pid
    stopped=" (stopped after ${TMO} s: a check did not end; the output it had not yet written is lost, run it by hand to see which)"
  fi
  wait $pid
  local rc=$?
  if [ -n "$VERBOSE" ]; then
    grep -a 'RADEON-I-SELFTEST' "$out"
  else
    grep -a 'RADEON-I-SELFTEST.* FAILED' "$out"
  fi
  local sum
  sum=$(grep -a -o '[0-9]* checks, [0-9]* failed' "$out" | tail -1)
  if [ $rc -eq 0 ] && [ -n "$sum" ]; then
    echo "PASS $mode $sum"
  else
    echo "FAIL $mode ${sum:-no result}, status $rc$stopped; see $out"
    fails=$((fails + 1))
  fi
}
run thread
run sync ALPHABOX_RADEON_SYNC=1
[ $fails -eq 0 ]
