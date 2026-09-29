#!/bin/bash
# USB storage throughput in a Windows 2000 guest, by the guest's own clock.
#
# usage: usb_bench.sh <alphabox-binary> <label> [VAR=value ...]
#   Builds (once) $ALPHABOX_WORK/usbbench/usbbench.img: 64 MB, an MBR and one
#   FAT16 partition holding BIG.BIN (16 MB of random bytes) and T.BAT. Boots a
#   clone of $ALPHABOX_WORK/win2k-installed with that image as a USB disk on
#   root hub port 2 (and the USB tablet on port 1), runs F:\T.BAT from
#   Start > Run, and prints the guest's times: A..B is BIG.BIN read from USB
#   to C:, B..C is the copy written back (Windows' write cache: not a USB
#   figure). Checks the written-back copy byte for byte.
#   VAR=value pairs go to the emulator's environment, e.g.
#   ALPHABOX_USB_ASYNC_US=250 (answer like hardware behind libusb).
# Needs mtools (mformat, mcopy, mtype). Stop only this script's emulator.
set -u
[ $# -ge 2 ] || { sed -n 2,16p "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); L=$2; shift 2
T=$(cd "$(dirname "$0")" && pwd)
R0=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R0/lab}
D=$WORK/usbbench
SRC=$WORK/win2k-installed
mkdir -p "$D"
if [ ! -s "$D/usbbench.img" ]; then
  python3 - "$D/usbbench.img" <<'PY'
import struct, sys
size, start = 64 << 20, 63
mbr = bytearray(512)
mbr[446:462] = struct.pack('<BBBBBBBBII', 0x80, 1, 1, 0, 0x06, 0xfe, 0xff, 0xff,
                           start, size // 512 - start)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'wb') as f:
    f.write(mbr); f.truncate(size)
PY
  mformat -i "$D/usbbench.img@@32256" -v USBBENCH -T $((64 * 2048 - 63)) -h 255 -s 63 -H 63 :: || exit 2
  head -c 16777216 /dev/urandom > "$D/BIG.BIN"
  printf '@echo off\r\necho A %%TIME%% > c:\\t.txt\r\ncopy f:\\big.bin c:\\big.bin\r\necho B %%TIME%% >> c:\\t.txt\r\ncopy c:\\big.bin f:\\big2.bin\r\necho C %%TIME%% >> c:\\t.txt\r\ncopy c:\\t.txt f:\\t.txt\r\necho DONE >> f:\\t.txt\r\n' > "$D/T.BAT"
  mcopy -i "$D/usbbench.img@@32256" "$D/BIG.BIN" "$D/T.BAT" :: || exit 2
fi
sed -e 's|  pci0.19 = ali_usb|&|' "$SRC/es40-window.cfg" | python3 -c "
import sys
s = sys.stdin.read()
s = s.replace('pci0.19 = ali_usb\n  {\n  }', '''pci0.19 = ali_usb
  {
    port1 = \"tablet\";
    disk2.0 = file
    {
      file = \"$D/run.img\";
    }
  }''')
print(s, end='')" > "$D/bench.cfg"
grep -q 'run.img' "$D/bench.cfg" || { echo "usb_bench: could not add the USB disk to $SRC/es40-window.cfg"; exit 2; }
R=$WORK/runs/usbbench-$L
rm -rf "$R"; mkdir -p "$R"; cp "$D/usbbench.img" "$D/run.img"
for f in "$SRC"/*; do case "$(basename "$f")" in fb|run*.log|*.dmp|alphabox-run|memory_*|peek.img|*backup*|*pre-smp*) continue;; esac; cp -c -R "$f" "$R/" 2>/dev/null || cp -R "$f" "$R/"; done
cd "$R" || exit 2
mkdir fb; : > keys.txt
env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys.txt "$BIN" run "$D/bench.cfg" > run.log 2>&1 &
PID=$!
t=0; last=""; still=0
while [ $t -lt 600 ]; do
  kill -0 $PID 2>/dev/null || { echo "usb_bench: the emulator exited"; tail -3 run.log; exit 1; }
  f=$(ls -t fb | head -1); h=$([ -n "$f" ] && shasum "fb/$f" | cut -c1-16)
  if [ -n "$h" ] && [ "$h" = "$last" ]; then still=$((still+5)); else still=0; last=$h; fi
  [ $t -ge 90 ] && [ $still -ge 20 ] && break
  sleep 5; t=$((t+5))
done
echo "$L: desktop after $t s"
echo "win-r" >> keys.txt; sleep 3
python3 -c "import sys; sys.path.insert(0,'$T'); import keys_for; print(' '.join(keys_for.tokens('f:/t.bat')))" >> keys.txt
sleep 4; echo "enter" >> keys.txt
s=$(date +%s)
while [ $(( $(date +%s) - s )) -lt 900 ]; do
  cp -c "$D/run.img" peek.img 2>/dev/null || cp "$D/run.img" peek.img
  mtype -i peek.img@@32256 ::/T.TXT 2>/dev/null | grep -q DONE && break
  sleep 5
done
rm -f peek.img
kill $PID; wait $PID 2>/dev/null
mtype -i "$D/run.img@@32256" ::/T.TXT | tr -d '\r'
python3 - "$(mtype -i "$D/run.img@@32256" ::/T.TXT | tr -d '\r')" <<'PY'
import sys, re
t = {k: v for k, v in re.findall(r'([ABC]) +(\d+:\d+:\d+\.\d+)', sys.argv[1])}
def sec(x):
    h, m, s = x.split(':'); return int(h) * 3600 + int(m) * 60 + float(s)
if 'A' in t and 'B' in t:
    d = (sec(t['B']) - sec(t['A'])) % 86400
    print(f"read 16 MB from USB: {d:.2f} s ({16 / d if d else 0:.1f} MB/s)")
PY
a=$(mtype -i "$D/run.img@@32256" ::/BIG2.BIN 2>/dev/null | shasum | cut -c1-16)
b=$(shasum "$D/BIG.BIN" | cut -c1-16)
[ "$a" = "$b" ] && echo "written-back copy: identical" || { echo "written-back copy: DIFFERS ($a vs $b)"; exit 1; }
cd "$WORK" && rm -rf "$R" "$D/run.img"
