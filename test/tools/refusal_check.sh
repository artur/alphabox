#!/bin/bash
# Check that the emulator refuses what it must refuse, at once and with a
# message, instead of starting a machine that is not the one asked for.
#
# usage: refusal_check.sh <alphabox-binary> <label>
#
# Each case writes a small configuration, starts the emulator and expects it
# to end by itself within REFUSAL_TMO seconds with a non-zero exit status and
# the case's message in its output. A case whose emulator is still running
# then is stopped (only the PID started here) and fails: it accepted what it
# should have refused.
#
#   restore-garbage     ALPHABOX_RESTORE names a file that is no state file
#   restore-missing     ... a file that does not exist
#   restore-version     ... a state file of a version this build does not read
#                       Until 2026-11-11 each of these printed a line and
#                       cold-booted, so a test or a measurement resumed
#                       nothing and did not know.
#   ds20e-hose          a card on pci2.7 of a DS20E: the board has hoses 0
#   ds20l-hose          and 1, and the device used to be accepted and then
#                       sit where the console never looks
#   permedia2-memory    permedia2 with memory = 6: the frame buffer's
#                       addresses wrap by a mask, which needs a power of two
#                       (skipped on a build without SDL3, where no video
#                       card is accepted)
#
# No firmware is needed: every refusal comes before the console is loaded,
# except the restores, which use the merge gate's console (test/rom).
#
# Output: one line per case, `PASS|FAIL <case> <reason>`, and the runs in
# $ALPHABOX_WORK/runs/refusal-<label>/ (ALPHABOX_WORK defaults to <repo>/lab).
# Exit 0 only if every case passed.
#
# Environment:
#   REFUSAL_TMO    seconds a case may take (default 300: the restores load
#                  and decompress the console first, a minute on a busy host)
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
TMO=${REFUSAL_TMO:-300}

[ $# -eq 2 ] || { sed -n '2,5p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
[ -x "$BIN" ] || { echo "refusal_check: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/refusal-$2
rm -rf "$OUT"; mkdir -p "$OUT"
rc=0

# cfg <dir> <platform> <devices>: a machine with one processor
cfg() {
  {
    echo 'sys0 = tsunami'
    echo '{'
    echo "  platform = \"$2\";"
    echo '  memory.bits = 26;'
    echo '  rom.srm = "cl67srmrom.exe";'
    echo '  rom.flash = "flash.rom";'
    echo '  rom.dpr = "dpr.rom";'
    echo '  cpu0 = ev68cb { speed = 1000M; }'
    # nothing connects: a port that waited for a terminal would never
    # reach the restore
    echo '  serial0 = serial { null_attach = true; }'
    echo '  serial1 = serial { null_attach = true; }'
    echo "$3"
    echo '}'
  } > "$1/es40.cfg"
}

# expect <case> <message>: run the emulator in the case's directory (the
# environment as the caller set it) and check how it ends
expect() {
  local d=$OUT/$1 n=0 st
  ( cd "$d" && exec "$BIN" run es40.cfg ) > "$d/alphabox.out" 2>&1 &
  local pid=$!
  while kill -0 $pid 2>/dev/null && [ $n -lt $((TMO * 5)) ]; do
    sleep 0.2; n=$((n + 1))
  done
  if kill -0 $pid 2>/dev/null; then
    kill $pid 2>/dev/null; sleep 2; kill -9 $pid 2>/dev/null
    wait $pid 2>/dev/null
    echo "FAIL $1 still running after $TMO s: not refused"; rc=1; return
  fi
  wait $pid; st=$?
  if [ $st -eq 0 ]; then
    echo "FAIL $1 exit status 0"; rc=1
  elif ! grep -aq -- "$2" "$d/alphabox.out"; then
    echo "FAIL $1 exit status $st without \"$2\""; rc=1
  else
    echo "PASS $1 exit status $st, \"$2\""
  fi
}

es40_dev='  pci0.7 = ali { vga_console = false; }
  pci0.15 = ali_ide { }
  pci0.19 = ali_usb { }'

# --- restores that cannot be resumed
for c in restore-garbage restore-missing restore-version; do
  mkdir -p "$OUT/$c"; cfg "$OUT/$c" es40 "$es40_dev"
  cp "$R/test/rom/cl67srmrom.exe" "$OUT/$c/"
done
head -c 4096 /dev/urandom > "$OUT/restore-garbage/snap.axp"
# the magic number, then version 2.1
printf '\x40\xe5\xfa\xa1\x01\x00\x02\x00' > "$OUT/restore-version/snap.axp"
head -c 4096 /dev/zero >> "$OUT/restore-version/snap.axp"
for c in restore-garbage restore-missing restore-version; do
  ALPHABOX_RESTORE=snap.axp expect $c "could not be resumed"
  rm -f "$OUT/$c/cl67srmrom.exe" "$OUT/$c/decompressed.rom"
done

# --- cards where the board has no hose
mkdir -p "$OUT/ds20e-hose" "$OUT/ds20l-hose"
cfg "$OUT/ds20e-hose" ds20e '  pci0.5 = ali { vga_console = false; }
  pci0.6 = ali_ide { }
  pci2.7 = sym53c810 { }'
expect ds20e-hose "this machine has PCI hoses 0 and 1"
cfg "$OUT/ds20l-hose" ds20l "$es40_dev
  pci3.4 = sym53c810 { }"
expect ds20l-hose "this machine has PCI hoses 0 and 1"

# --- a frame buffer size the card's model cannot address
if "$BIN" --version | grep -q 'Features:.* SDL3'; then
  mkdir -p "$OUT/permedia2-memory"
  cfg "$OUT/permedia2-memory" es40 "$es40_dev
  pci0.2 = permedia2 { memory = 6; }"
  printf 'gui = sdl\n{\n}\n' >> "$OUT/permedia2-memory/es40.cfg"
  SDL_VIDEODRIVER=dummy expect permedia2-memory "memory must be 2, 4 or 8"
else
  echo "SKIP permedia2-memory this build has no SDL3"
fi

exit $rc
