#!/bin/bash
# Disks that boot the Gentoo minimal CD's Linux on the ES47 (docs/linux.md,
# "Linux on the ES47").
#
# usage: es47_linux.sh bootdisk <iso> <out.img>
#        es47_linux.sh retarget <iso> <image>
#
# bootdisk  a 256 MB aboot disk (test/tools/aboot_disk.py): partition 1, ext2,
#           holds the CD's gentoo_nolsa kernel with one byte changed (below),
#           its initrd and etc/aboot.conf. Line 0 boots the live CD
#           (cdroot) with console=srm0 and epoch=2000, line 1 without the
#           epoch. Put it beside the CD and `boot` it; the CD is the root.
# retarget  changes an aboot disk installed on the ES40 (docs/linux.md,
#           "Installing to a disk") IN PLACE, so run it on a clone: the same
#           kernel into /boot/vmlinuz, aboot.conf line 0 with console=srm0,
#           and in /etc/inittab a getty on srm0 for the one on ttyS0.
#
# Why not the CD as it is:
#  - /boot/gentoo is built with ALPHA_LEGACY_START_ADDRESS (0x310000), inside
#    the memory the Marvel console keeps; aboot refuses it ("Busy
#    (Reserved)"), as the kernel's Kconfig help says it must on Marvel.
#    gentoo_nolsa is the CD's other kernel, built without it.
#  - Every one of the CD's kernels oopses in vgacon_startup on a Marvel
#    whose console is not a graphics one: marvel_ioremap returns NULL for the
#    legacy VGA window (0xb8000) while no console VGA hose is set
#    (core_marvel.c: marvel_find_console_vga_hose sets it only for a CTB of
#    TERM_TYPE 3), and vgacon_startup reads through that NULL. A real ES47
#    with a serial console does the same; Linux has no parameter to keep
#    vgacon off. The byte: vgacon_screen_info.orig_video_lines set to 0,
#    so vgacon_startup takes its "no VGA" path to the dummy console.
#  - The console is the SRM's (console=srm0: the live CD's fixinittab runs
#    agetty on the console's name, and the tty is srm0): Marvel has no
#    16550.
# Needs bsdtar, gzip, python3 and e2fsprogs (mke2fs, debugfs; Homebrew's
# e2fsprogs on macOS). An existing output file is never overwritten.
set -eu
T=$(cd "$(dirname "$0")" && pwd)
MODE=${1:-}
ISO=${2:-}
OUT=${3:-}
[ -n "$OUT" ] && [ -f "$ISO" ] || { sed -n '2,12p' "$0"; exit 2; }
E=
for d in /opt/homebrew/opt/e2fsprogs/sbin /usr/local/opt/e2fsprogs/sbin /sbin /usr/sbin; do
  [ -x "$d/debugfs" ] && { E=$d; break; }
done
[ -n "$E" ] || { echo "es47_linux: no debugfs (e2fsprogs)"; exit 2; }
W=$(mktemp -d "${TMPDIR:-/tmp}/es47_linux.XXXXXX")
trap 'rm -rf "$W"' EXIT

bsdtar -xf "$ISO" -C "$W" boot/gentoo_nolsa boot/gentoo_nolsa.igz \
  boot/System-gentoo_nolsa.map boot/bootlx
gzip -dc "$W/boot/gentoo_nolsa" > "$W/vmlinux"
python3 -I - "$W/vmlinux" "$W/boot/System-gentoo_nolsa.map" <<'PY'
import struct, sys
vm, smap = sys.argv[1], sys.argv[2]
sym = None
for line in open(smap):
    p = line.split()
    if len(p) == 3 and p[2] == "vgacon_screen_info":
        sym = int(p[0], 16)
d = bytearray(open(vm, "rb").read())
assert sym and d[:4] == b"\x7fELF", "no vgacon_screen_info or not an ELF"
phoff, = struct.unpack_from("<Q", d, 0x20)
phnum, = struct.unpack_from("<H", d, 0x38)
off = None
for i in range(phnum):
    t, _, o, va, _, fs, _, _ = struct.unpack_from("<IIQQQQQQ", d, phoff + i * 56)
    if t == 1 and va <= sym < va + fs:
        off = o + (sym - va)
assert off is not None, "vgacon_screen_info not in a loaded segment"
# struct screen_info: orig_video_lines at 0xe, orig_video_isVGA at 0xf
assert d[off + 0xe] == 25 and d[off + 0xf] == 1, "unexpected screen_info"
d[off + 0xe] = 0
open(vm, "wb").write(d)
print("patched vgacon_screen_info.orig_video_lines at file offset %#x" % (off + 0xe))
PY
gzip -9 -n -c "$W/vmlinux" > "$W/vmlinuz"

case $MODE in
bootdisk)
  [ -e "$OUT" ] && { echo "es47_linux: $OUT exists"; exit 1; }
  A="dokeymap looptype=squashfs loop=/image.squashfs cdroot"
  {
    echo "0:1/vmlinuz initrd=/initrd.igz $A console=srm0 epoch=2000"
    echo "1:1/vmlinuz initrd=/initrd.igz $A console=srm0"
  } > "$W/aboot.conf"
  python3 -I "$T/aboot_disk.py" "$OUT" 256 "$W/boot/bootlx" 128
  P="$OUT?offset=1048576"
  "$E/mke2fs" -q -t ext2 -b 1024 -I 128 -O none -L boot -E offset=1048576 "$OUT" 131072
  "$E/debugfs" -w -R "mkdir etc" "$P" 2>/dev/null
  "$E/debugfs" -w -R "write $W/vmlinuz vmlinuz" "$P" 2>/dev/null
  "$E/debugfs" -w -R "write $W/boot/gentoo_nolsa.igz initrd.igz" "$P" 2>/dev/null
  "$E/debugfs" -w -R "write $W/aboot.conf etc/aboot.conf" "$P" 2>/dev/null
  ;;
retarget)
  [ -f "$OUT" ] || { echo "es47_linux: no $OUT"; exit 1; }
  # partition 1 from sector 2048, partition 2 after its 128 MB (aboot_disk.py)
  P1="$OUT?offset=1048576"
  P2="$OUT?offset=$((1048576 + 128 * 1048576))"
  "$E/debugfs" -R "cat etc/aboot.conf" "$P1" 2>/dev/null > "$W/aboot.conf.old"
  grep -q '^0:1/vmlinuz' "$W/aboot.conf.old" || { echo "es47_linux: $OUT is not the ES40 install"; exit 1; }
  echo '0:1/vmlinuz root=/dev/sda2 console=srm0 epoch=2000' > "$W/aboot.conf"
  "$E/debugfs" -R "cat /etc/inittab" "$P2" 2>/dev/null > "$W/inittab"
  # no 16550 on Marvel: the ttyS0 getty would only respawn
  sed -i.bak -e 's/^s0:/#s0:/' "$W/inittab"
  grep -q '^m0:' "$W/inittab" || echo 'm0:12345:respawn:/sbin/agetty 9600 srm0 vt100' >> "$W/inittab"
  for f in vmlinuz etc/aboot.conf; do "$E/debugfs" -w -R "rm $f" "$P1" 2>/dev/null; done
  "$E/debugfs" -w -R "write $W/vmlinuz vmlinuz" "$P1" 2>/dev/null
  "$E/debugfs" -w -R "write $W/aboot.conf etc/aboot.conf" "$P1" 2>/dev/null
  "$E/debugfs" -w -R "rm /etc/inittab" "$P2" 2>/dev/null
  "$E/debugfs" -w -R "write $W/inittab /etc/inittab" "$P2" 2>/dev/null
  # debugfs gives a written file the host user's ids: root's, as before
  for f in vmlinuz etc/aboot.conf; do
    for k in uid gid; do "$E/debugfs" -w -R "sif $f $k 0" "$P1" 2>/dev/null; done
  done
  for k in uid gid; do "$E/debugfs" -w -R "sif /etc/inittab $k 0" "$P2" 2>/dev/null; done
  ;;
*) sed -n '2,12p' "$0"; exit 2 ;;
esac
"$E/debugfs" -R "ls -l" "$OUT?offset=1048576" 2>/dev/null | sed 's/^/  /'
echo "es47_linux: $MODE done: $OUT"
