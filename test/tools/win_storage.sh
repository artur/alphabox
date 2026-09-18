#!/bin/bash
# Storage-controller check with an installed Windows guest: add a controller
# with a FAT16 test disk, boot, have Windows copy DATA.BIN to COPY.BIN on
# that disk, stop, and compare the two files on the host.
#
# usage: CTRL=<config class> win_storage.sh <label> <alphabox-binary> \
#            <install-dir> <cfg> [boots] [VAR=value ...]
#   CTRL      controller class, e.g. sym53c810 (placed at pci0.3).
#             CTRL=ali_ide instead hangs the test disk off the IDE
#             controller the machine already has, as drive IDE_DEV
#             (default disk0.1, the master's companion on the first
#             channel).
#   CTRL_OPTS extra lines for the controller section, e.g. 'chip = "x";'
#   BRIDGE    put the controller behind a PCI-PCI bridge of this class
#             (at pci0.3), as device BRIDGE_DEV (default 2) on its bus
#   boots     how many boots to try (default 2): Windows installs the driver
#             for new hardware on the first boot and may only use it on the
#             next one.
#   The install directory is never modified: the run works on a clone in
#   $ALPHABOX_WORK/storage-<label> (kept, for inspection; delete it after).
#
# Prints, per boot, whether COPY.BIN appeared and matches DATA.BIN, and the
# path of the last screenshot. Exit status 0 when a boot produced a match.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
[ $# -ge 4 ] || { sed -n '2,21p' "$0"; exit 2; }
LABEL=$1 BIN=$2 INST=$3 CFG=$4 BOOTS=${5:-2}
shift $(($# < 5 ? 4 : 5))
: "${CTRL:?set CTRL to the controller class}"
case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "$LABEL: binary missing"; exit 2; }
[ -f "$SRC/$CFG" ] || { echo "$LABEL: $SRC/$CFG missing"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "$LABEL: memory pressure critical, not starting"
  exit 2
fi

D=$WORK/storage-$LABEL
rm -rf "$D"
cp -c -R "$SRC" "$D" 2>/dev/null || cp -R "$SRC" "$D" || exit 2
cd "$D" || exit 2
python3 "$T/fat_disk.py" make test0.img --size-mb 64 --data-kb 2048 > /dev/null || exit 2
python3 - "$CFG" "$CTRL" "${CTRL_OPTS:-}" "${BRIDGE:-}" "${BRIDGE_DEV:-2}" \
  "${IDE_DEV:-disk0.1}" > storage.cfg <<'PY' || exit 2
import sys
cfg, ctrl, opts, bridge, bridge_dev, ide_dev = sys.argv[1:7]
t = open(cfg).read()
anchor = "  pci0.15 = ali_ide"
assert anchor in t, "no ali_ide section to anchor on"
def drive(name):
    return "    %s = file\n    {\n      file = \"test0.img\";\n    }\n" % name
if ctrl == "ali_ide":
    # the machine already has this controller: add the disk to it
    head = anchor + "\n  {\n"
    assert head in t, "ali_ide section does not open the way we expect"
    print(t.replace(head, head + drive(ide_dev), 1), end="")
    sys.exit()
sec = "  pci0.3 = %s\n  {\n%s%s  }\n\n" % (
    ctrl, ("    " + opts + "\n") if opts else "", drive("disk0.0"))
if bridge:
    inner = sec.replace("  pci0.3 = ", "  pci.%s = " % bridge_dev, 1)
    inner = "".join("  " + l if l.strip() else l for l in inner.splitlines(True))
    sec = "  pci0.3 = %s\n  {\n%s  }\n\n" % (bridge, inner.rstrip("\n") + "\n")
print(t.replace(anchor, sec + anchor, 1), end="")
PY

ok=1
for boot in $(seq 1 "$BOOTS"); do
  rm -rf fb && mkdir fb && : > keys
  env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys \
    "$BIN" run storage.cfg > boot$boot.log 2>&1 &
  P=$!
  # desktop after ~2.5 min (one frame every ~2 s)
  for i in $(seq 1 240); do
    kill -0 $P 2>/dev/null || break
    [ "$(ls fb | wc -l)" -ge 75 ] && break
    sleep 2
  done
  if kill -0 $P 2>/dev/null; then
    echo "win-r" >> keys
    sleep 4
    echo "c m d enter" >> keys
    sleep 6
    python3 "$T/keys_for.py" --enter \
      'for %d in (d e f g h i) do if exist %d:\data.bin copy %d:\data.bin %d:\copy.bin' >> keys
    sleep 45
    kill $P # our own emulator only
    for i in $(seq 1 40); do kill -0 $P 2>/dev/null || break; sleep 0.5; done
    kill -0 $P 2>/dev/null && kill -9 $P
  else
    echo "  boot $boot: emulator exited early"
  fi
  wait $P 2>/dev/null
  last=$(ls fb | tail -1)
  [ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" boot$boot.png
  printf '  boot %d: ' "$boot"
  if python3 "$T/fat_disk.py" check test0.img COPY.BIN DATA.BIN; then
    ok=0
    break
  fi
done
grep -aE 'Emulator Failure|SYM:|%IDE-' boot*.log | sort | uniq -c | sort -rn | head -8 | sed 's/^/  /'
echo "  screens: $D/boot*.png"
exit $ok
