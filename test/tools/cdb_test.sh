#!/bin/bash
# The SCSI disk's conformance test: `alphabox cdbtest` drives file, RAM and
# CD-ROM disks on a bus of their own, with no guest and no controller
# model, and checks status, sense, data and lengths against SCSI-2, SPC-3,
# SBC-3 and MMC-3 (src/devices/storage/DiskCdbTest.cpp).
#
# usage: cdb_test.sh <alphabox-binary> [-v]
#   Runs in $ALPHABOX_WORK/cdbtest (ALPHABOX_WORK defaults to <repo>/lab);
#   the images it makes there (about 11 MB) are removed at the end.
#   -v lists every check, not only the failures.
#
# Prints a FAIL line per failed check and "PASS" or "FAIL" last. Exit
# status: 0 when every check passed.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 1 ] || { sed -n '2,13p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/cdbtest
[ -x "$BIN" ] || { echo "cdb_test: $BIN is not executable"; exit 2; }
mkdir -p "$D" || exit 2
"$BIN" cdbtest "$D" ${2:-} > "$D/cdbtest.out" 2>&1
rc=$?
grep -a -E '^  (PASS|FAIL) |^cdbtest: [0-9]+ passed|^(PASS|FAIL)$' "$D/cdbtest.out"
[ $rc -gt 1 ] && echo "cdb_test: the test stopped (status $rc); see $D/cdbtest.out"
exit $rc
