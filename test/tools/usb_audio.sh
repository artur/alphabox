#!/bin/bash
# The USB speaker in a Windows 2000 guest: what it plays, captured and
# compared with the file it played.
#
# usage: usb_audio.sh <alphabox-binary> <label> <card> [VAR=value ...]
#   card: ali  -- the speaker on the ALi's OHCI (port 3, tablet on port 1)
#         ehci -- on the EHCI card, served by its companion (port 3)
#   Boots a clone of $ALPHABOX_WORK/win2k-installed with the speaker, plays
#   C:\WINNT\MEDIA\<SOUND> (default CHORD.WAV) with sndrec32 from Start > Run,
#   records the stream with ALPHABOX_USBAUDIO_WAV, and compares the capture
#   with the source (test/tools/wav_compare.py). VAR=value pairs go to the
#   emulator's environment, e.g. ALPHABOX_USB_ASYNC_US=500: the speaker
#   behind the async shim, isochronous packets moving with libusb's timing.
#   WIZ=<n>: press Enter n times after the boot (Found New Hardware wizards
#   of a card the clone has not seen). Late isochronous TDs and packets
#   the shim could not queue are counted from the USBT trace.
# Run directory: $RUNDIR, default $ALPHABOX_WORK/runs/usbaudio-<label>
# (removed unless KEEP=1). Stops only its own emulator.
set -u
[ $# -ge 3 ] || { sed -n 2,18p "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); L=$2; CARD=$3; shift 3
T=$(cd "$(dirname "$0")" && pwd)
R0=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R0/lab}
SRC=$WORK/win2k-installed
R=${RUNDIR:-$WORK/runs/usbaudio-$L}
SOUND=${SOUND:-CHORD.WAV}
export MTOOLS_SKIP_CHECK=1
lvl=$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)
[ "$lvl" = 4 ] && { echo "usb_audio: memory pressure critical"; exit 2; }
rm -rf "$R"; mkdir -p "$R/fb"
for f in "$SRC"/*; do case "$(basename "$f")" in fb|run*.log|*.dmp|alphabox-run|memory_*|peek.img|*backup*|*pre-smp*) continue;; esac; cp -c -R "$f" "$R/" 2>/dev/null || cp -R "$f" "$R/"; done
cd "$R" || exit 2
mcopy -o -n -i "disk0.img@@16384" "::/WINNT/MEDIA/$SOUND" source.wav || exit 2
python3 - es40-window.cfg "$CARD" > audio.cfg <<'PY' || exit 2
import sys
s, card = open(sys.argv[1]).read(), sys.argv[2]
devs = '    port1 = "tablet";\n    port3 = "audio";\n'
if card == 'ali':
    assert 'pci0.19 = ali_usb\n  {\n  }' in s
    s = s.replace('pci0.19 = ali_usb\n  {\n  }', 'pci0.19 = ali_usb\n  {\n' + devs + '  }')
elif card == 'ehci':
    s = s.rstrip()
    s = s[:-1].rstrip() + '\n\n  pci0.3 = ehci\n  {\n' + devs + '  }\n}\n'
else:
    raise SystemExit('usb_audio: card is ali or ehci')
print(s, end='')
PY
: > keys.txt
env "$@" SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb \
    ALPHABOX_KEYPIPE=keys.txt ALPHABOX_USBAUDIO_WAV="$R/audio.wav" ALPHABOX_USBTRACE=1 \
    "$BIN" run audio.cfg > run.log 2>&1 &
PID=$!
settle() {
  local t=0 last="" still=0 f h
  while [ $t -lt 600 ]; do
    kill -0 $PID 2>/dev/null || { echo "usb_audio: the emulator exited"; tail -5 run.log; exit 1; }
    f=$(ls -t fb | head -1); h=$([ -n "$f" ] && shasum "fb/$f" | cut -c1-16)
    if [ -n "$h" ] && [ "$h" = "$last" ]; then still=$((still+5)); else still=0; last=$h; fi
    [ $t -ge "$1" ] && [ $still -ge 20 ] && break
    sleep 5; t=$((t+5))
  done
}
settle 90
if [ "${WIZ:-0}" -gt 0 ]; then
  for i in $(seq 1 "$WIZ"); do echo "enter" >> keys.txt; sleep 5; done
  settle 20
fi
echo "win-r" >> keys.txt; sleep 3
python3 -c "import sys; sys.path.insert(0,'$T'); import keys_for; print(' '.join(keys_for.tokens('sndrec32 /play /close c:/winnt/media/$SOUND')))" >> keys.txt
sleep 4; echo "enter" >> keys.txt
sleep "${PLAY_SECS:-15}"
kill $PID; wait $PID 2>/dev/null
echo "== $L ($CARD${*:+, $*})"
ok=1
if [ -s audio.wav ]; then
  python3 "$T/wav_compare.py" source.wav audio.wav | sed 's/^/   /'
  c=$(python3 "$T/wav_compare.py" source.wav audio.wav | sed -n 's/^envelope correlation \([0-9.-]*\).*/\1/p')
  python3 -c "import sys; sys.exit(0 if float('$c') >= 0.9 else 1)" || ok=0
else
  echo "   nothing captured"; ok=0
fi
ls audio-*.wav >/dev/null 2>&1 && echo "   (the stream changed rate part way: $(ls audio-*.wav | tr '\n' ' '))"
echo "   late isochronous TDs: $(grep -a -c 'late:' run.log); packets not moved in time: $(grep -a -c 'iso packet' run.log)"
[ $ok = 1 ] && echo "$L: PASS" || echo "$L: FAIL"
[ $ok = 1 ] && [ "${KEEP:-0}" != 1 ] && { cd "$WORK" && rm -rf "$R"; }
exit $((1 - ok))
