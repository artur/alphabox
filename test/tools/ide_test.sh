#!/bin/bash
# The IDE controller's register-level test: `alphabox idetest` drives the
# ALi M1543C's IDE function through its task file, control block and bus
# master registers (and the floppy controller and ISA DMA through theirs),
# with no guest and no processor, and checks the answers against ATA/ATAPI-6,
# SFF-8038i, the Intel 82077AA and 8237A datasheets
# (src/devices/pci/IdeControllerTest.cpp). A command that stops the machine
# or leaves a channel busy fails its check.
#
# usage: ide_test.sh <alphabox-binary> [-v]
#   Runs in $ALPHABOX_WORK/idetest (ALPHABOX_WORK defaults to <repo>/lab);
#   the images it makes there (a sparse 200 GB one among them) are removed at
#   the end. IDETEST_ONLY=group,group runs only the groups named.
#   -v lists every check, not only the failures.
#
# Prints a FAIL line per failed check and "PASS" or "FAIL" last. Exit
# status: 0 when every check passed.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 1 ] || { sed -n '2,15p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/idetest
[ -x "$BIN" ] || { echo "ide_test: $BIN is not executable"; exit 2; }
mkdir -p "$D" || exit 2
"$BIN" idetest "$D" ${2:-} > "$D/idetest.out" 2>&1
rc=$?
grep -a -E '^  (PASS|FAIL) |^idetest: [0-9]+ passed|^(PASS|FAIL)$' "$D/idetest.out"
[ $rc -gt 1 ] && echo "ide_test: the test stopped (status $rc); see $D/idetest.out"
exit $rc
