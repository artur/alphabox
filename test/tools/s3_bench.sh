#!/bin/bash
# Graphics workload for an installed Windows guest: boot to the desktop, open
# a command prompt, start a long directory listing and let it scroll for a
# fixed time. Scrolling a console window is drawing-engine work -- a screen
# full of text moved up a line, over and over -- so it is what the guest feels
# as a slow or fast card.
#
# usage: s3_bench.sh <label> <alphabox-binary> <install-dir> <cfg> \
#            [work_seconds] [VAR=value ...]
#   install-dir  guest install directory; relative paths are under
#                $ALPHABOX_WORK (default <repo>/lab). It is never modified:
#                the run uses a clone in $ALPHABOX_WORK/s3bench-<label>.
#   work_seconds how long to wait for the listing to finish (default 120)
#   VAR=value    extra environment for the emulator
#   DIR=<path>   what to list (default c:\winnt); a bigger tree makes the
#                measurement finer, since the screen is only sampled every
#                two seconds
#
# With ALPHABOX_BLIT_STATS=1 the emulator reports the drawing engine's pixels
# as it goes, and this prints how many of them were drawn during the timed
# window -- more pixels in the same time is a faster card. Also prints the
# distinct frames drawn in that window and the last screenshot.
set -u
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
[ $# -ge 4 ] || { sed -n '2,19p' "$0"; exit 2; }
LABEL=$1 BIN=$2 INST=$3 CFG=$4
SECS=${5:-120}
shift $(($# < 5 ? 4 : 5))
case "$INST" in /*) SRC=$INST ;; *) SRC=$WORK/$INST ;; esac
BIN=$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")
[ -x "$BIN" ] || { echo "$LABEL: binary missing"; exit 2; }
[ -f "$SRC/$CFG" ] || { echo "$LABEL: $SRC/$CFG missing"; exit 2; }
if [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" = 4 ]; then
  echo "$LABEL: memory pressure critical, not starting"
  exit 2
fi

D=$WORK/s3bench-$LABEL
rm -rf "$D"
cp -c -R "$SRC" "$D" 2>/dev/null || cp -R "$SRC" "$D" || exit 2
cd "$D" || exit 2
rm -rf fb && mkdir fb && : > keys

env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys \
  "$BIN" run "$CFG" > run.log 2>&1 &
P=$!
# Our own emulator's pid, written down: this host may be running others, and
# a profiler pointed at the wrong one measures an idle machine.
echo $P > emulator.pid
trap 'kill $P 2>/dev/null' EXIT

frame_hash() { md5 -q "fb/$(ls fb | tail -1)" 2>/dev/null; }

# The screen going quiet is the only sign the guest is ready for a keystroke.
settle() { # settle <deadline_s> <quiet_rounds>
  local deadline=$((SECONDS + $1)) quiet=0 last=""
  while [ $SECONDS -lt $deadline ]; do
    sleep 2
    kill -0 $P 2>/dev/null || return 1
    local h; h=$(frame_hash)
    if [ -n "$h" ] && [ "$h" = "$last" ]; then
      quiet=$((quiet + 1))
      [ $quiet -ge "$2" ] && return 0
    else
      quiet=0
    fi
    last=$h
  done
  return 1
}

key_takes() { # key_takes <token> [deadline_s]
  local before try
  for try in 1 2; do
    before=$(frame_hash)
    echo "$1" >> keys
    local deadline=$((SECONDS + ${2:-20}))
    while [ $SECONDS -lt $deadline ]; do
      sleep 2
      kill -0 $P 2>/dev/null || return 1
      [ "$(frame_hash)" != "$before" ] && return 0
    done
  done
  return 1
}

# Cumulative pixels from the emulator's own periodic report.
pixels_now() { grep -a "8514 blit:" run.log | tail -1 | sed 's/.*blit: \([0-9]*\) .*/\1/'; }

for i in $(seq 1 240); do
  kill -0 $P 2>/dev/null || break
  [ "$(ls fb | wc -l)" -ge 75 ] && break
  sleep 2
done
kill -0 $P 2>/dev/null || { echo "$LABEL: emulator exited before the desktop"; exit 1; }
settle 300 3 || echo "$LABEL: desktop never settled, trying anyway"

# Start menu, then R for Run...: Win+R does not raise the dialog in this guest.
if ! key_takes "ctrl-esc" || ! key_takes "r"; then
  echo "$LABEL: the Start menu never opened"
  exit 1
fi
settle 30 2
echo "c m d enter" >> keys
settle 60 2

# A listing long enough to outlast the window, on a directory every install
# has, so two runs scroll the same text.
python3 "$T/keys_for.py" --enter "dir /s ${DIR:-c:\\winnt}" >> keys

# How long the listing takes is the number that matters: the screen going
# quiet again is the prompt coming back.
start_px=$(pixels_now); start_frames=$(ls fb | wc -l); t0=$SECONDS
if settle "$SECS" 3; then
  # the settle rounds are part of the elapsed time; take them back off
  elapsed=$((SECONDS - t0 - 6))
else
  elapsed=0
fi
end_px=$(pixels_now); end_frames=$(ls fb | wc -l)

kill $P 2>/dev/null # our own emulator only
for i in $(seq 1 40); do kill -0 $P 2>/dev/null || break; sleep 0.5; done
kill -0 $P 2>/dev/null && kill -9 $P
wait $P 2>/dev/null
trap - EXIT

last=$(ls fb | tail -1)
[ -n "$last" ] && python3 "$T/ppm2png.py" "fb/$last" "$D/last.png" > /dev/null 2>&1

if [ "$elapsed" -gt 0 ]; then
  echo "$LABEL: listing finished in ${elapsed}s"
else
  echo "$LABEL: listing did not finish within ${SECS}s"
fi
if [ -n "${start_px:-}" ] && [ -n "${end_px:-}" ]; then
  echo "$LABEL: $((end_px - start_px)) pixels drawn"
else
  echo "$LABEL: no blit stats (run with ALPHABOX_BLIT_STATS=1)"
fi
echo "$LABEL: $((end_frames - start_frames)) frames, screenshot $D/last.png"
echo "$LABEL: emulator pid was in $D/emulator.pid"
