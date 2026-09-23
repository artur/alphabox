#!/bin/bash
# VGA render check: boot SRM with its console on a VGA card (vga_console),
# window-less (SDL dummy driver), dumping frames; report the settled screen.
#
# usage: [CARD=s3|cirrus|mach64] [CHIP=gd5430|gd5434|ct|vt2|rage2p|ragepro] [ROM=<bios>] \
#          vga_boot.sh <alphabox-binary> <label> [seconds]
#   Runs in $ALPHABOX_WORK/runs/vga-<label> (ALPHABOX_WORK defaults to <repo>/lab).
#   Needs an SDL lane. CARD defaults to s3, CHIP to gd5434 (cirrus) or ct
#   (mach64).
#   ROM defaults to test/arc/86c764x1.bin (s3), or for cirrus to the 86Box
#   ROM set (not in git): roms/video/cirruslogic/gd5434.BIN (gd5434) or
#   pci.bin (gd5430); for mach64 to the 86Box set in roms/video/mach64/:
#   the Mach64 CT PCI BIOS (ct) or the 264VT2 PCI BIOS (vt2).
#
# The screen settles on two frames (text cursor on/off). With SRM V7.3-1 the
# settled sets are:
#   s3             (86c764x1.bin)          58b2a3f795 b962153c15
#   cirrus gd5434  (GD543x PCI BIOS 1.10B) b866caa6ba ccc23de79a
#   cirrus gd5430  (86Box pci.bin)         c3f64950a2 d1fb4d6f9c
#   mach64 ct      (Mach64 CT PCI BIOS)    3e98e7f5a5 81a4da0cd7
#   mach64 vt2     (264VT2 PCI BIOS)       323cbfa3fa d4363d6f19
# The set is the last ten frames, one every ~2 s, so the cursor phase can
# alias: on a loaded host one of the two may be missing. Run it alone.
# A behaviour-preserving change must reproduce them. last.png in the run
# directory is the final frame.
#
# A whole different pair, on every frame, usually means the console printed
# one line more or fewer than the run the set was taken from and the screen
# is scrolled by a line: SRM's own "entering idle loop" lands on the screen
# in some runs and not others (one of eight CT boots here). Diff the last
# frame against the recorded run before believing a change caused it.
#
# Exit status: 0 when frames were captured and the emulator did not fail.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
[ $# -ge 2 ] || { echo "usage: [CARD=s3|cirrus|mach64] [ROM=<bios>] $0 <alphabox-binary> <label> [seconds]"; exit 2; }
[ -x "$1" ] || { echo "vga_boot: $1 is not executable"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2
SECS=${3:-60}
CARD=${CARD:-s3}
EXTRA=""
case $CARD in
s3) ROM=${ROM:-$R/test/arc/86c764x1.bin} ;;
cirrus)
  CHIP=${CHIP:-gd5434}
  case $CHIP in
  gd5434) ROM=${ROM:-$R/roms/video/cirruslogic/gd5434.BIN} ;;
  gd5430) ROM=${ROM:-$R/roms/video/cirruslogic/pci.bin} ;;
  *) echo "vga_boot: CHIP must be gd5430 or gd5434"; exit 2 ;;
  esac
  EXTRA="chip = \"$CHIP\";"
  ;;
mach64)
  CHIP=${CHIP:-ct}
  case $CHIP in
  ct) ROM=${ROM:-$R/roms/video/mach64/mach64-68b110b8cddfd546595673.bin} ;;
  vt2) ROM=${ROM:-$R/roms/video/mach64/atimach64vt2pci.bin} ;;
  rage2p) ROM=${ROM:-$R/roms/video/atirageii/rageii-pci.bin} ;;
  ragepro) ROM=${ROM:-$R/roms/video/atiragepro/rage2pr-bga-40212-103-mx27c512.bin} ;;
  *) echo "vga_boot: CHIP must be ct, vt2, rage2p or ragepro"; exit 2 ;;
  esac
  EXTRA="chip = \"$CHIP\";"
  ;;
*) echo "vga_boot: CARD must be s3, cirrus or mach64"; exit 2 ;;
esac
[ -f "$ROM" ] || { echo "vga_boot: VGA BIOS $ROM not found"; exit 2; }
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/vga-$LABEL

rm -rf "$D" && mkdir -p "$D/fb" || exit 2
cp "$R/test/rom/cl67srmrom.exe" "$D/" || exit 2
cp "$ROM" "$D/vgabios.bin" || exit 2
cat > "$D/es40.cfg" <<CFG
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

  pci0.2 = $CARD
  {
    rom = "vgabios.bin";
    $EXTRA
  }

  pci0.15 = ali_ide
  {
  }

  pci0.7 = ali
  {
    vga_console = true;
  }

  pci0.19 = ali_usb
  {
  }
}
CFG
cd "$D" || exit 2
SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb "$BIN" run > run.out 2>&1 &
P=$!
sleep "$SECS"
kill "$P" 2>/dev/null # our own emulator only
for i in $(seq 1 40); do kill -0 "$P" 2>/dev/null || break; sleep 0.5; done
kill -0 "$P" 2>/dev/null && kill -9 "$P" 2>/dev/null
wait "$P" 2>/dev/null

ok=0
python3 - "$D/fb" <<'PY' || ok=1
import glob, hashlib, sys, os
files = sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm")))
if not files:
    print("  NO FRAMES"); sys.exit(1)
h = [hashlib.md5(open(f, "rb").read()).hexdigest()[:10] for f in files]
print("  frames: %d" % len(h))
print("  final frame: %s  %s" % (os.path.basename(files[-1]), h[-1]))
print("  settled set (last 10 frames): %s" % " ".join(sorted(set(h[-10:]))))
PY
ls fb/*.ppm >/dev/null 2>&1 && python3 "$T/ppm2png.py" "$(ls fb/*.ppm | tail -1)" last.png
if grep -aE "Emulator Failure|Exception" run.out | head -3 | grep -q .; then
  grep -aE "Emulator Failure|Exception" run.out | head -3 | sed 's/^/  /'
  ok=1
fi
grep -a '\[JIT\]\[VERIFY\]' run.out | tail -1 | sed 's/^/  /'
exit $ok
