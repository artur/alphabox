#!/bin/bash
# USB across a saved and restored machine, in a Windows 2000 guest.
#
# usage: usb_snap.sh <alphabox-binary> <label> <card> [VAR=value ...]
#   card: ali    -- the ALi's OHCI: tablet on port 1, disk on 2, speaker on 3
#         ehci   -- the EHCI card with its companions, same devices (ports
#                   1-2 are companion 0's, 3 companion 1's)
#   Boots a clone of $ALPHABOX_WORK/win2k-installed with those devices, checks
#   the tablet puts the cursor where it is told (four spots, within a pixel),
#   then snapshots the running guest (SIGUSR1 + ALPHABOX_SNAPSHOT_EXIT) and
#   resumes it in a fresh process (ALPHABOX_RESTORE). After the restore: the
#   tablet again, then C:\USBT.BAT from Start > Run -- it copies a 4 MB file
#   to the USB disk, compares it there with fc /b, and plays chord.wav on the
#   USB speaker (captured with ALPHABOX_USBAUDIO_WAV). Checks the copy on
#   the disk image byte for byte and that the speaker got a stream.
#   VAR=value pairs go to the emulator's environment (both runs).
#   SETTLE=<s>: seconds to let the restored guest re-enumerate (default 25).
#   WIZ=<n>: press Enter n times after the boot, 5 s apart -- walks Found
#   New Hardware wizards to Finish (the ehci card, on a clone that has not
#   seen it: its EHCI function gets no driver, its speaker needs one).
#   BUSY=1: take the snapshot BUSY_AT (5) seconds into C:\BUSY.BAT, which
#   plays a sound on the speaker and copies the file to the disk four times:
#   the machine is saved with transfers under way.
# Run directory: $RUNDIR, default $ALPHABOX_WORK/runs/usbsnap-<label> (removed after a pass
# unless KEEP=1). Stops only its own emulator.
set -u
[ $# -ge 3 ] || { sed -n 2,20p "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); L=$2; CARD=$3; shift 3
T=$(cd "$(dirname "$0")" && pwd)
R0=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R0/lab}
SRC=$WORK/win2k-installed
R=${RUNDIR:-$WORK/runs/usbsnap-$L}
tab_ok=1
export MTOOLS_SKIP_CHECK=1
lvl=$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)
[ "$lvl" = 4 ] && { echo "usb_snap: memory pressure critical"; exit 2; }
rm -rf "$R"; mkdir -p "$R/fb"
for f in "$SRC"/*; do case "$(basename "$f")" in fb|run*.log|*.dmp|alphabox-run|memory_*|peek.img|*backup*|*pre-smp*) continue;; esac; cp -c -R "$f" "$R/" 2>/dev/null || cp -R "$f" "$R/"; done
cd "$R" || exit 2

# The USB disk: an MBR and one FAT16 partition with a marker file.
python3 - usb.img <<'PY'
import struct, sys
size, start = 64 << 20, 63
mbr = bytearray(512)
mbr[446:462] = struct.pack('<BBBBBBBBII', 0x80, 1, 1, 0, 0x06, 0xfe, 0xff, 0xff,
                           start, size // 512 - start)
mbr[510:512] = b'\x55\xaa'
with open(sys.argv[1], 'wb') as f:
    f.write(mbr); f.truncate(size)
PY
mformat -i "usb.img@@32256" -v USBSNAP -T $((64 * 2048 - 63)) -h 255 -s 63 -H 63 :: || exit 2
echo usbsnap > MARK.TXT; mcopy -i "usb.img@@32256" MARK.TXT :: || exit 2
# C:\USBSRC.BIN and C:\USBT.BAT, staged before the guest boots.
head -c 4194304 /dev/urandom > USBSRC.BIN
printf '@echo off\r\nset U=\r\nfor %%%%d in (E F G H I J K) do if exist %%%%d:\\MARK.TXT set U=%%%%d\r\necho drive %%U%% > C:\\USBT.TXT\r\ncopy C:\\USBSRC.BIN %%U%%:\\COPY.BIN >> C:\\USBT.TXT\r\nfc /b C:\\USBSRC.BIN %%U%%:\\COPY.BIN >> C:\\USBT.TXT\r\nsndrec32 /play /close C:\\WINNT\\MEDIA\\CHORD.WAV\r\necho DONE >> C:\\USBT.TXT\r\ncopy C:\\USBT.TXT %%U%%:\\USBT.TXT\r\n' > USBT.BAT
printf '@echo off\r\nset U=\r\nfor %%%%d in (E F G H I J K) do if exist %%%%d:\\MARK.TXT set U=%%%%d\r\nstart sndrec32 /play /close C:\\WINNT\\MEDIA\\WINDOW~2.WAV\r\nfor %%%%n in (1 2 3 4) do copy C:\\USBSRC.BIN %%U%%:\\BUSY%%%%n.BIN\r\n' > BUSY.BAT
mcopy -o -i "disk0.img@@16384" USBSRC.BIN USBT.BAT BUSY.BAT :: || exit 2
mdel -i "disk0.img@@16384" ::/USBT.TXT 2>/dev/null

case "$CARD" in
ali) DEVS='    port1 = "tablet";\n    disk2.0 = file\n    {\n      file = "usb.img";\n    }\n    port3 = "audio";\n'
     python3 - es40-window.cfg "$DEVS" > snap.cfg <<'PY' || exit 2
import sys
s = open(sys.argv[1]).read()
new = 'pci0.19 = ali_usb\n  {\n' + sys.argv[2].replace('\\n', '\n') + '  }'
assert 'pci0.19 = ali_usb\n  {\n  }' in s
print(s.replace('pci0.19 = ali_usb\n  {\n  }', new), end='')
PY
     ;;
ehci) DEVS='    port1 = "tablet";\n    disk2.0 = file\n    {\n      file = "usb.img";\n    }\n    port3 = "audio";\n'
     python3 - es40-window.cfg "$DEVS" > snap.cfg <<'PY' || exit 2
import sys
s = open(sys.argv[1]).read().rstrip()
assert s.endswith('}')
card = '\n  pci0.3 = ehci\n  {\n' + sys.argv[2].replace('\\n', '\n') + '  }\n}\n'
print(s[:-1].rstrip() + '\n' + card, end='')
PY
     ;;
*) echo "usb_snap: card is ali or ehci"; exit 2 ;;
esac

settle() { # wait for the screen to stop changing: 20 s still, after $1 s
  local t=0 last="" still=0 f h
  while [ $t -lt 600 ]; do
    kill -0 $PID 2>/dev/null || { echo "usb_snap: the emulator exited"; tail -5 run*.log; exit 1; }
    f=$(ls -t fb | head -1); h=$([ -n "$f" ] && shasum "fb/$f" | cut -c1-16)
    if [ -n "$h" ] && [ "$h" = "$last" ]; then still=$((still+5)); else still=0; last=$h; fi
    [ $t -ge "$1" ] && [ $still -ge 20 ] && break
    sleep 5; t=$((t+5))
  done
  echo "   settled after $t s"
}
# The tablet to four spots; the cursor must land at each (the pixel where
# its arrow starts), within one pixel.
tablet() { # <tag>
  local n=0 pos
  for pos in 0.9:0.1 0.1:0.9 0.5:0.25 0.75:0.75; do
    n=$((n+1)); echo "tablet:$pos" >> keys.txt; sleep 4
    cp "fb/$(ls -t fb | head -1)" "$1-pos$n.ppm"
  done
  echo "tablet:0.02:0.5" >> keys.txt
  python3 - "$1" <<'PY'
import sys
tag = sys.argv[1]
def ppm(p):
    d = open(p, 'rb').read(); parts = d.split(b'\n', 3)
    w, h = map(int, parts[1].split()); return w, h, parts[3]
W, H, _ = ppm(tag + '-pos1.ppm')
frames = [ppm('%s-pos%d.ppm' % (tag, i))[2] for i in range(1, 5)]
want = [(576, 48), (64, 432), (320, 120), (480, 360)]
ok = True
for i, f in enumerate(frames):
    others = [g for j, g in enumerate(frames) if j != i]; xs = []; ys = []
    for y in range(H - 40):
        for x in range(W):
            o = (y * W + x) * 3; px = f[o:o + 3]
            if px in (b'\x00\x00\x00', b'\xff\xff\xff') and all(px != g[o:o + 3] for g in others):
                xs.append(x); ys.append(y)
    got = (min(xs), min(ys)) if xs else None
    good = got is not None and abs(got[0] - want[i][0]) <= 1 and abs(got[1] - want[i][1]) <= 1
    ok &= good
    print('   %s pos%d: cursor at %s, want %s %s' % (tag, i + 1, got, want[i], 'ok' if good else 'WRONG'))
print('   %s tablet: %s' % (tag, 'PASS' if ok else 'FAIL'))
sys.exit(0 if ok else 1)
PY
  [ $? = 0 ] || tab_ok=0
}

: > keys.txt
echo "== $L ($CARD): boot"
env "$@" SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb \
    ALPHABOX_KEYPIPE=keys.txt ALPHABOX_SNAPSHOT="$R/snap.axp" ALPHABOX_SNAPSHOT_EXIT=1 \
    "$BIN" run snap.cfg > run1.log 2>&1 &
PID=$!
settle 90
if [ "${WIZ:-0}" -gt 0 ]; then
  # Found New Hardware wizards (a card the guest has not seen before): Enter
  # takes each through its default buttons to Finish.
  for i in $(seq 1 "$WIZ"); do echo "enter" >> keys.txt; sleep 5; done
  settle 20
fi
tablet before
if [ "${BUSY:-0}" = 1 ]; then
  # Snapshot in the middle of USB traffic: a copy to the disk and a sound
  # on the speaker, both under way.
  echo "win-r" >> keys.txt; sleep 3
  python3 -c "import sys; sys.path.insert(0,'$T'); import keys_for; print(' '.join(keys_for.tokens('c:/busy.bat')))" >> keys.txt
  sleep 4; echo "enter" >> keys.txt; sleep "${BUSY_AT:-5}"
fi
echo "== snapshot"
kill -USR1 $PID
for i in $(seq 1 60); do kill -0 $PID 2>/dev/null || break; sleep 1; done
kill -0 $PID 2>/dev/null && { echo "usb_snap: no exit after the snapshot"; kill $PID; exit 1; }
wait $PID 2>/dev/null
[ -s snap.axp ] || { echo "usb_snap: no snapshot"; tail -5 run1.log; exit 1; }
ls -l snap.axp | awk '{print "   snapshot:", $5, "bytes"}'

echo "== restore"
rm -f fb/*; : > keys.txt
env "$@" SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb \
    ALPHABOX_KEYPIPE=keys.txt ALPHABOX_RESTORE="$R/snap.axp" ALPHABOX_USBAUDIO_WAV="$R/audio.wav" \
    "$BIN" run snap.cfg > run2.log 2>&1 &
PID=$!
sleep "${SETTLE:-25}"
kill -0 $PID 2>/dev/null || { echo "usb_snap: died on restore"; tail -8 run2.log; exit 1; }
tablet after
echo "win-r" >> keys.txt; sleep 3
python3 -c "import sys; sys.path.insert(0,'$T'); import keys_for; print(' '.join(keys_for.tokens('c:/usbt.bat')))" >> keys.txt
sleep 4; echo "enter" >> keys.txt
s=$(date +%s); done=0
while [ $(( $(date +%s) - s )) -lt 300 ]; do
  kill -0 $PID 2>/dev/null || break
  cp -c usb.img peek.img 2>/dev/null || cp usb.img peek.img
  mtype -i peek.img@@32256 ::/USBT.TXT 2>/dev/null | grep -q DONE && { done=1; break; }
  sleep 5
done
rm -f peek.img
sleep 2
cp "fb/$(ls -t fb | head -1)" last.ppm 2>/dev/null
kill $PID; wait $PID 2>/dev/null
echo "== results"
mtype -i "usb.img@@32256" ::/USBT.TXT 2>/dev/null | tr -d '\r' | sed 's/^/   /'
ok=1
[ $done = 1 ] || { echo "   the batch did not finish"; ok=0; }
a=$(mtype -i "usb.img@@32256" ::/COPY.BIN 2>/dev/null | shasum | cut -c1-16)
b=$(shasum USBSRC.BIN | cut -c1-16)
[ "$a" = "$b" ] && echo "   copy on the USB disk: identical" || { echo "   copy on the USB disk: DIFFERS"; ok=0; }
if [ -s audio.wav ]; then
  echo "   speaker: $(stat -f %z audio.wav) bytes captured"
else
  echo "   speaker: nothing captured"; ok=0
fi
[ $tab_ok = 1 ] || { echo "   the tablet missed"; ok=0; }
grep -c 'late:' run2.log | sed 's/^/   late iso TDs in the trace: /'
[ $ok = 1 ] && echo "$L: PASS" || echo "$L: FAIL"
[ $ok = 1 ] && [ "${KEEP:-0}" != 1 ] && { cd "$WORK" && rm -rf "$R"; }
exit $((1 - ok))
