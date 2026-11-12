#!/bin/bash
# The Mach64's, the Permedia 2's and the ViRGE's self-tests
# (ALPHABOX_VIDEO_SELFTEST, docs/headless.md): each card checks its engine at
# the end of its init, before any machine starts, and the emulator ends with
# the result. No guest, no console firmware and no card BIOS: the option ROM
# each card wants is an empty one made here (55 AA, zeros, and four bytes at
# its end for the check of a read across it).
#
# usage: video_selftest.sh <alphabox-binary> [-v]
#   -v lists every check, not only the failures.
#
# One run per card, in $ALPHABOX_WORK/runs/video-selftest/<card>
# (ALPHABOX_WORK defaults to <repo>/lab). A run that has not ended after
# VIDEO_SELFTEST_TMO seconds (default 120) is stopped (only the emulator this
# script started) and fails: an endless walk is what several checks are for.
#
# Prints the FAILED checks and one line per card, `PASS|FAIL <card> <checks>`.
# Exit status: 0 when every card passed, 2 when the test could not run (a
# build without SDL3 accepts no video card).
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 1 ] || { sed -n '2,8p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
VERBOSE=${2:-}
WORK=${ALPHABOX_WORK:-$R/lab}
TMO=${VIDEO_SELFTEST_TMO:-120}
[ -x "$BIN" ] || { echo "video_selftest: $BIN is not executable"; exit 2; }
"$BIN" --version | grep -q 'Features:.* SDL3' ||
  { echo "video_selftest: this build has no SDL3, and a card needs a GUI"; exit 2; }
OUT=$WORK/runs/video-selftest
rm -rf "$OUT"; mkdir -p "$OUT" || exit 2
rc=0

for card in mach64 permedia2 s3virge; do
  d=$OUT/$card; mkdir -p "$d"
  cat > "$d/es40.cfg" <<EOF
gui = sdl
{
}
sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "cl67srmrom.exe";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  cpu0 = ev68cb { speed = 800M; }
  serial0 = serial { null_attach = true; }
  serial1 = serial { null_attach = true; }
  pci0.2 = $card { rom = "selftest.rom"; }
  pci0.7 = ali { vga_console = false; }
}
EOF
  { printf '\x55\xaa'; head -c 32762 /dev/zero; printf '\x11\x22\x33\x44'; } > "$d/selftest.rom"
  ( cd "$d" && exec env SDL_VIDEO_DRIVER=dummy ALPHABOX_VIDEO_SELFTEST=exit \
      "$BIN" run es40.cfg ) > "$d/alphabox.out" 2>&1 &
  pid=$!; n=0
  while kill -0 $pid 2>/dev/null && [ $n -lt $((TMO * 5)) ]; do
    sleep 0.2; n=$((n + 1))
  done
  hung=0
  if kill -0 $pid 2>/dev/null; then
    hung=1; kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
  fi
  wait $pid 2>/dev/null; st=$?
  [ "$VERBOSE" = -v ] && grep -a -- '-I-SELFTEST:' "$d/alphabox.out" | sed 's/^/  /'
  [ "$VERBOSE" = -v ] || grep -a -- '-I-SELFTEST:.*FAILED' "$d/alphabox.out" | sed 's/^/  /'
  ok=$(grep -ac -- '-I-SELFTEST:.* ok$' "$d/alphabox.out")
  if [ $hung = 1 ]; then
    echo "FAIL $card still running after $TMO s ($ok checks done)"; rc=1
  elif [ $st -eq 0 ] && grep -aq -- '-I-SELFTEST: PASS' "$d/alphabox.out"; then
    echo "PASS $card $ok checks"
  else
    echo "FAIL $card exit status $st ($ok ok)"; rc=1
  fi
done
exit $rc
