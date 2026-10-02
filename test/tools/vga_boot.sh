#!/bin/bash
# VGA render check: boot SRM with its console on a VGA card (vga_console),
# window-less (SDL dummy driver), dumping frames; report the settled screen.
#
# usage: [CARD=s3|cirrus|mach64|permedia2|s3virge|radeon|tga] [CHIP=gd5430|gd5434|ct|vt2|rage2p|ragepro|dx|virge|vx|gx2] [ROM=<bios>] \
#          vga_boot.sh <alphabox-binary> <label> [seconds]
#   Runs in $ALPHABOX_WORK/runs/vga-<label> (ALPHABOX_WORK defaults to <repo>/lab).
#   Needs an SDL lane. CARD defaults to s3, CHIP to gd5434 (cirrus) or ct
#   (mach64).
#   ROM defaults to test/arc/86c764x1.bin (s3), or for cirrus to the 86Box
#   ROM set (not in git): roms/video/cirruslogic/gd5434.BIN (gd5434) or
#   pci.bin (gd5430); for mach64 to the 86Box set in roms/video/mach64/:
#   the Mach64 CT PCI BIOS (ct) or the 264VT2 PCI BIOS (vt2); for radeon to
#   the Radeon 7500 AGP BIOS (RV200, 64 MB) in roms/video/radeon7500/; for permedia2
#   to the ELSA GLoria Synergy PCI BIOS 8.07.00 (roms/video/permedia2/).
#   That BIOS never runs under SRM V7.3-1, which calls option ROMs with
#   AX = 0 where the BIOS expects its own bus and device (see es40.cfg), so
#   permedia2 gives no frames here; AlphaBIOS runs it. s3virge takes CHIP
#   dx (the default), virge, vx or gx2, with the part's BIOS from
#   roms/video/s3virge/: the S3 reference BIOSes 86c375_4.bin (2.01.16, DX),
#   86c325.bin (1.00-10, ViRGE) and flagpoint.VBI (2.16.13, GX2), and for
#   the VX, which has no S3 reference image, Diamond's Stealth 3D 3000
#   diamondstealth3000.vbi. tga, the DEC ZLXp-E1, has no VGA BIOS and
#   takes no ROM. SRM V7.3-1 lists it (tga0) but has no console driver
#   for it and stays on the serial line, so it gives no frames on its own;
#   AlphaBIOS drives it. FLASH=<image> boots the console from a flash
#   image instead of the SRM file -- test/arc/flash.rom, whose nvram
#   script starts AlphaBIOS (the test-arc skill) -- with the ALi's
#   vga_console set as there.
#
# The screen settles on two frames (text cursor on/off). With SRM V7.3-1 the
# settled sets are:
#   s3             (86c764x1.bin)          58b2a3f795 b962153c15
#   cirrus gd5434  (GD543x PCI BIOS 1.10B) b866caa6ba ccc23de79a
#   cirrus gd5430  (86Box pci.bin)         c3f64950a2 d1fb4d6f9c
#   mach64 ct      (Mach64 CT PCI BIOS)    3e98e7f5a5 81a4da0cd7
#   mach64 vt2     (264VT2 PCI BIOS)       323cbfa3fa d4363d6f19
#   s3virge dx     (S3 BIOS 2.01.16)       1b6682c7dc e23c1c51f6
#   s3virge virge  (S3 BIOS 1.00-10)       1b6682c7dc e23c1c51f6
#   s3virge gx2    (S3 BIOS 2.16.13)       1b6682c7dc e23c1c51f6
#   s3virge vx     (Diamond S3D 3000 1.00) 107a595f1d 7a3a484498
#   s3virge vx     (STB Velocity 3D 1.10)  52d3ffb772 974d8129d5
#   s3virge gx2    (Diamond S3D 4000 1.01) 990a4adfa5 cccaa8c08a
#   radeon         (RV200 BIOS 2002/04/16) 7010dde2ad 79d0463f19
# and with FLASH=test/arc/flash.rom, AlphaBIOS V5.71's "No Operating System
# Selections Found" screen (one frame, no cursor blink; give it 90 s):
#   tga            (no ROM)                1082cc8409
# (the vendor BIOSes carry fonts of their own).
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
[ $# -ge 2 ] || { echo "usage: [CARD=s3|cirrus|mach64|permedia2|s3virge|radeon|tga] [ROM=<bios>] $0 <alphabox-binary> <label> [seconds]"; exit 2; }
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
permedia2) ROM=${ROM:-$R/roms/video/permedia2/SYN80700.PAN} ;;
radeon) ROM=${ROM:-$R/roms/video/radeon7500/ATI.7500.64.Hynix50_020416.rom} ;;
s3virge)
  CHIP=${CHIP:-dx}
  case $CHIP in
  dx | gx) ROM=${ROM:-$R/roms/video/s3virge/86c375_4.bin} ;;
  virge) ROM=${ROM:-$R/roms/video/s3virge/86c325.bin} ;;
  vx) ROM=${ROM:-$R/roms/video/s3virge/diamondstealth3000.vbi} ;;
  gx2) ROM=${ROM:-$R/roms/video/s3virge/flagpoint.VBI} ;;
  *) echo "vga_boot: CHIP must be dx, gx, virge, vx or gx2"; exit 2 ;;
  esac
  EXTRA="chip = \"$CHIP\";"
  ;;
tga)
  ROM=${ROM:-}
  [ -n "${MODEL:-}" ] && EXTRA="model = \"$MODEL\";"
  ;;
*) echo "vga_boot: CARD must be s3, cirrus, mach64, permedia2, s3virge, radeon or tga"; exit 2 ;;
esac
[ -z "$ROM" ] || [ -f "$ROM" ] || { echo "vga_boot: VGA BIOS $ROM not found"; exit 2; }
WORK=${ALPHABOX_WORK:-$R/lab}
D=$WORK/runs/vga-$LABEL

rm -rf "$D" && mkdir -p "$D/fb" || exit 2
cp "$R/test/rom/cl67srmrom.exe" "$D/" || exit 2
FLASHCFG=""
if [ -n "${FLASH:-}" ]; then
  cp "$FLASH" "$D/flash.rom" || exit 2
  FLASHCFG="arc_year_compat = true;"
fi
ROMLINE=""
if [ -n "$ROM" ]; then
  cp "$ROM" "$D/vgabios.bin" || exit 2
  ROMLINE='rom = "vgabios.bin";'
fi
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
  $FLASHCFG

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
    $ROMLINE
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
# KEYS=<keyscript> types at the console (docs/headless.md), e.g. a command.
[ -n "${KEYS:-}" ] && export ALPHABOX_KEYSCRIPT="$KEYS"
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
