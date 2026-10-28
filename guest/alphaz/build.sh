#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# build.sh: build AlphaZ with nada, both programs from the same core:
#   ALPHAZ.EXE    Windows on the Alpha, the window (-t alpha-windows)
#   ALPHAZ64.EXE  the same window for 64-bit Windows on the Alpha (AXP64,
#                 Whistler build 2210, -t alpha64-windows), which loads no
#                 32-bit Alpha program
#   alphaz-linux  Linux on the Alpha, the text report, static (-t alpha)
# into OUT, and write OUT/NADA-COMMIT.txt: the nada commit, the commands,
# and the sha256 of every input and output.
#
# usage: guest/alphaz/build.sh
# Environment: NADA=<nada checkout> (default ~/Documents/proj/nada),
#   OUT=<dir> (default $ALPHABOX_WORK/runs/alphaz-build, ALPHABOX_WORK being
#   the repository's lab/ unless set).
#
# nada must be at commit b8db5784 or later: -mwindows (a PE of Subsystem 2),
# the tab control, the other Win32 names and Linux's clock_gettime and
# sched_setaffinity in its headers, and `.arch ev8` inside an asm text. It
# is used read-only: its ./nada binary, include/ and lib/, and the commit
# git gives for its tree.
#
# The .def files name the DLL calls AlphaZ makes. nada's headers declare
# all of them now, but its lib/windows/ has only kernel32.def (the names
# lib/windows.c calls), so this directory keeps the rest: user32.def,
# gdi32.def, comctl32.def and advapi32.def, since nada has no .def for
# those DLLs, and kernel32x.def, the kernel32 names AlphaZ calls that
# nada's kernel32.def lacks (build.sh links it less the names nada's file
# has, a name in two .def files being an error).
#
# All the programs are built for the 21264 (-mcpu=ev6): BWX, FIX and MVI,
# which every processor Alphabox emulates has. The EV8 instructions of
# az_ev8.c are mnemonics after `.arch ev8`, which raises the assembler for
# that asm text only; they run only after AMASK reports bit 11. Each one's
# word, written beside it, is checked here against every program and
# against the word nada's assembler writes for its mnemonic at -mcpu=ev8.
# Copyright (C) 2026 Artur Goulão
set -u
export LC_ALL=C
HERE=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$HERE/../.." && pwd)
NADA=${NADA:-$HOME/Documents/proj/nada}
OUT=${OUT:-${ALPHABOX_WORK:-$R/lab}/runs/alphaz-build}
mkdir -p "$OUT" || exit 2
OUT=$(cd "$OUT" && pwd)

# nada's session rebuilds its binary now and then: wait for it, a while
i=0
while [ ! -x "$NADA/nada" ] && [ $i -lt 30 ]; do sleep 10; i=$((i + 1)); done
[ -x "$NADA/nada" ] || { echo "build.sh: no nada at $NADA"; exit 2; }

NADA_NEEDED=b8db5784
if ! git -C "$NADA" merge-base --is-ancestor "$NADA_NEEDED" HEAD 2>/dev/null; then
  echo "build.sh: nada at $NADA is older than commit $NADA_NEEDED (or is not a git checkout);"
  echo "          AlphaZ needs -mwindows, the tab control, clock_gettime and .arch ev8 from it"
  exit 2
fi
COMMIT=$(git -C "$NADA" rev-parse --short=8 HEAD 2>/dev/null || echo unknown)
DIRTY=0
[ -n "$(git -C "$NADA" status --porcelain -- src lib include 2>/dev/null)" ] && DIRTY=1
{
  echo "/* written by guest/alphaz/build.sh */"
  echo "#define AZ_NADA_COMMIT \"$COMMIT\""
  echo "#define AZ_NADA_DIRTY $DIRTY"
} > "$OUT/az_build.h"

# kernel32x.def less the names nada's kernel32.def has
{
  sed -n '1,/^EXPORTS/p' "$HERE/kernel32x.def"
  sed -n '/^EXPORTS/,$p' "$HERE/kernel32x.def" | grep '^ ' | while read -r name; do
    grep -qx "    $name" "$NADA/lib/windows/kernel32.def" || echo "    $name"
  done
} > "$OUT/kernel32x.def"

python3 -I "$HERE/mkicon.py" "$OUT/alphaz.ico" || exit 1
cp "$HERE/alphaz.rc" "$OUT/alphaz.rc" || exit 1

L=$NADA/lib
LIBC="$L/ctype.c $L/string.c $L/stdlib.c $L/fltconv.c $L/stdio.c $L/printf.c $L/scanf.c $L/time.c $L/setjmp.c"
CORE="$HERE/az_core.c $HERE/az_ev8.c"
WIN_CMD="$NADA/nada -t alpha-windows -mwindows -mcpu=ev6 -I $NADA/include -I $NADA/include/windows -I $OUT -I $HERE \
-o $OUT/ALPHAZ.EXE $HERE/alphaz_win.c $CORE $L/windows.c $LIBC $L/windows/kernel32.def $OUT/kernel32x.def \
$HERE/user32.def $HERE/gdi32.def $HERE/comctl32.def $HERE/advapi32.def $OUT/alphaz.rc"
WIN64_CMD=$(echo "$WIN_CMD" | sed "s| -t alpha-windows | -t alpha64-windows |; s|$OUT/ALPHAZ.EXE |$OUT/ALPHAZ64.EXE |")
LINUX_CMD="$NADA/nada -t alpha -mcpu=ev6 -I $NADA/include -I $OUT -I $HERE \
-o $OUT/alphaz-linux $HERE/alphaz_linux.c $CORE $L/linux.c $LIBC"

rm -f "$OUT/ALPHAZ.EXE" "$OUT/ALPHAZ64.EXE" "$OUT/alphaz-linux"
$WIN_CMD || { echo "build.sh: ALPHAZ.EXE failed"; exit 1; }
$WIN64_CMD || { echo "build.sh: ALPHAZ64.EXE failed"; exit 1; }
$LINUX_CMD || { echo "build.sh: alphaz-linux failed"; exit 1; }

# each PE is what the loader needs: the Alpha's Machine, PE32 or PE32+, and
# Subsystem 2 (WINDOWS_GUI, from -mwindows), so no console opens behind the
# window. The Subsystem field is at the same offset in PE32 and PE32+.
python3 -I - "$OUT/ALPHAZ.EXE" 0x184 0x10B "$OUT/ALPHAZ64.EXE" 0x284 0x20B <<'PY' || exit 1
import struct, sys
a = sys.argv[1:]
for p, want_machine, want_magic in zip(a[0::3], a[1::3], a[2::3]):
    b = open(p, "rb").read()
    pe = struct.unpack_from("<I", b, 0x3C)[0]
    assert b[pe:pe + 4] == b"PE\0\0", "no PE signature"
    machine = struct.unpack_from("<H", b, pe + 4)[0]
    opt = pe + 24
    magic, sub = struct.unpack_from("<H", b, opt)[0], struct.unpack_from("<H", b, opt + 68)[0]
    assert (machine, magic, sub) == (int(want_machine, 16), int(want_magic, 16), 2), (p, hex(machine), hex(magic), sub)
    print("%s: machine 0x%X, magic 0x%X, subsystem 2 (GUI)" % (p.rsplit("/", 1)[-1], machine, magic))
PY

# --- the EV8 words against nada's assembler ---------------------------------
CHK=$OUT/ev8check
rm -rf "$CHK" && mkdir -p "$CHK"
ev8_one() { # ev8_one <file> <text>
  {
    echo 'typedef unsigned long long u64;'
    echo "static u64 __attribute__((naked)) f(u64 a, u64 b) { __asm__(\"    $2\\n    ret\\n\"); }"
    echo 'int main(int argc, char **argv) { (void)argv; return argc > 9 ? (int)f(argc, argc) : 0; }'
  } > "$CHK/$1.c"
  "$NADA/nada" -t alpha -mcpu=ev8 -I "$NADA/include" -o "$CHK/$1" "$CHK/$1.c" "$L/linux.c" $LIBC
}
ev8_one base nop || { echo "build.sh: the EV8 check's base did not build"; exit 1; }
n=0
grep -E '^AZ_EV8_(INT|PAIR)\(' "$HERE/az_ev8.c" | sed -E 's/^AZ_EV8_[A-Z]+\(([a-z0-9_]+), "0x([0-9a-f]+)", "([^"]+)"\)$/\1 \2 \3/' > "$CHK/words.txt"
while read -r name word mnem; do
  ev8_one "$name" "$mnem" || { echo "build.sh: nada did not assemble '$mnem'"; exit 1; }
  n=$((n + 1))
done < "$CHK/words.txt"
python3 -I - "$CHK" "$OUT/ALPHAZ.EXE" "$OUT/ALPHAZ64.EXE" "$OUT/alphaz-linux" <<'PY' || exit 1
import os, struct, sys
chk = sys.argv[1]
base = open(os.path.join(chk, "base"), "rb").read()
progs = [open(p, "rb").read() for p in sys.argv[2:]]
bad = 0
for line in open(os.path.join(chk, "words.txt")):
    name, word, mnem = line.split(" ", 2)
    w = struct.pack("<I", int(word, 16))
    b = open(os.path.join(chk, name), "rb").read()
    ok = w in b and w not in base and all(w in p for p in progs)
    print("  %-4s 0x%s  %s" % ("ok" if ok else "BAD", word, mnem.strip()))
    bad += not ok
sys.exit(1 if bad else 0)
PY
[ $? = 0 ] || { echo "build.sh: an EV8 word is not nada's"; exit 1; }
echo "EV8 words: $n, each the word nada's assembler writes at -mcpu=ev8"

sha() { shasum -a 256 "$@" | sed "s|  $HERE/|  guest/alphaz/|; s|  $OUT/|  |"; }
{
  echo "AlphaZ, built $(date -u '+%Y-%m-%d %H:%M UTC') by guest/alphaz/build.sh"
  echo "nada: $NADA at commit $COMMIT$([ $DIRTY = 1 ] && echo ' (src/, lib/ or include/ had uncommitted changes)')"
  echo
  echo "Commands:"
  echo "  $WIN_CMD" | sed "s|$NADA|\$NADA|g; s|$HERE|guest/alphaz|g; s|$OUT|\$OUT|g"
  echo "  $WIN64_CMD" | sed "s|$NADA|\$NADA|g; s|$HERE|guest/alphaz|g; s|$OUT|\$OUT|g"
  echo "  $LINUX_CMD" | sed "s|$NADA|\$NADA|g; s|$HERE|guest/alphaz|g; s|$OUT|\$OUT|g"
  echo
  echo "sha256:"
  sha "$OUT/ALPHAZ.EXE" "$OUT/ALPHAZ64.EXE" "$OUT/alphaz-linux"
  sha "$HERE"/*.c "$HERE"/*.h "$HERE"/*.def "$HERE"/*.rc "$HERE"/mkicon.py "$OUT/alphaz.ico"
} > "$OUT/NADA-COMMIT.txt"
ls -l "$OUT/ALPHAZ.EXE" "$OUT/ALPHAZ64.EXE" "$OUT/alphaz-linux" | awk '{print $5, $9}'
echo "build.sh: done, $OUT/NADA-COMMIT.txt"
