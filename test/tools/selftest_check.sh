#!/bin/bash
# The OHCI and virtio-blk self-tests (docs/headless.md), each on the test
# machine at its console prompt: drivers inside the emulator that drive the
# device through its registers and print PASS or FAIL.
#
# usage: selftest_check.sh <alphabox-binary> <label>
#
#   ohci     ALPHABOX_OHCI_SELFTEST=1, a tablet on the ALi function's port 1
#   virtio   ALPHABOX_VIRTIO_SELFTEST=1, a virtio_blk on pci0.3 with a 1 MB
#            image made here (sector 0 ends in 55 AA, as the test expects)
#
# Each is one srm_probe.sh run (the merge gate's console, test/rom), so the
# runs are in $ALPHABOX_WORK/runs/probe-selftest-<label>-ohci and -virtio
# (ALPHABOX_WORK defaults to <repo>/lab). A test that printed neither PASS
# nor FAIL before the console's commands ended is a failure.
#
# Output: the FAILED checks and one line per test, `PASS|FAIL <test> <checks>`.
# Exit 0 only if both passed.
#
# Environment:
#   SELFTEST_PORT_BASE   telnet port of the first run (default 21980); the
#                        second uses the next
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
PB=${SELFTEST_PORT_BASE:-21980}
[ $# -eq 2 ] || { sed -n '2,6p' "$0"; exit 2; }
BIN=$1; L=selftest-$2
[ -x "$BIN" ] || { echo "selftest_check: $BIN is not executable"; exit 2; }
S=$WORK/runs/$L-in
rm -rf "$S"; mkdir -p "$S" || exit 2
rc=0

# verdict <test> <tag> <run directory>
verdict() {
  local out=$3/alphabox.out n
  n=$(grep -ac "%$2-I-SELFTEST:.* ok\$" "$out" 2>/dev/null)
  grep -a "%$2-I-SELFTEST:.*FAILED" "$out" 2>/dev/null | sed 's/^/  /'
  if grep -aq "%$2-I-SELFTEST:.*FAIL" "$out" 2>/dev/null; then
    echo "FAIL $1 a check failed ($n ok)"; rc=1
  elif grep -aq "%$2-I-SELFTEST:.*PASS" "$out" 2>/dev/null; then
    echo "PASS $1 $n checks"
  else
    echo "FAIL $1 the test did not finish ($n ok)"; rc=1
  fi
}

# the commands only keep the console busy while the test runs
CM='show config|show device|show config|show device'

echo 'port1 = "tablet";' > "$S/usb.cfg"
ALPHABOX_OHCI_SELFTEST=1 USB_CFG=$S/usb.cfg PORT=$PB CMDS=$CM \
  "$T/srm_probe.sh" "$BIN" "$L-ohci" > "$S/ohci.log" 2>&1
verdict ohci OHCI "$WORK/runs/probe-$L-ohci"

head -c 1048576 /dev/zero > "$S/vblk.img"
printf '\x55\xaa' | dd of="$S/vblk.img" bs=1 seek=510 conv=notrunc 2>/dev/null
echo "pci0.3 = virtio_blk { disk0.0 = file { file = \"$S/vblk.img\"; } }" > "$S/virtio.cfg"
ALPHABOX_VIRTIO_SELFTEST=1 EXTRA_CFG=$S/virtio.cfg PORT=$((PB + 1)) CMDS=$CM \
  "$T/srm_probe.sh" "$BIN" "$L-virtio" > "$S/virtio.log" 2>&1
verdict virtio VIRTIO "$WORK/runs/probe-$L-virtio"

[ $rc = 0 ] && rm -rf "$S" "$WORK/runs/probe-$L-ohci" "$WORK/runs/probe-$L-virtio"
exit $rc
