#!/bin/bash
# Direct3D conformance on a Windows 2000 guest: does the display card's HAL
# draw what Microsoft's software rasteriser draws?
#
# usage: d3d_check.sh <alphabox-binary> <guest-dir> [cfg]
#   <guest-dir> is an installed guest (under $ALPHABOX_WORK unless absolute)
#   whose display driver has Direct3D and whose desktop is in 16-bit colour
#   (e.g. lab/rpro-win, chip = "ragepro"); cfg defaults to es40-window.cfg.
#
# The guest is cloned (copy-on-write) into $ALPHABOX_WORK/runs/d3dcheck, so
# the original is never booted. d3dcheck/d3dcheck.c is built with nada
# ($NADA, default ~/Documents/proj/nada) and staged as C:\D3D before the
# boot; the clone boots headless, the program is started through Start >
# Run, and its output is read from a copy of the disk until it says EXIT.
# Then the emulator this script started -- and only that one -- is stopped.
#
# Prints the program's RESULT lines. Per scene, results/<scene>.png holds
# the HAL image on the left, the software one on the right, and their
# difference (bright where they differ) below. Exit status: 0 when every
# scene matched, 1 when some differ, 2 when the run itself failed.
set -u
export LC_ALL=C MTOOLS_SKIP_CHECK=1
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
NADA=${NADA:-$HOME/Documents/proj/nada}
[ $# -ge 2 ] || { sed -n '2,22p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
case "$2" in /*) SRC=$2 ;; *) SRC=$WORK/$2 ;; esac
CFG=${3:-es40-window.cfg}
RUN=$WORK/runs/d3dcheck
[ -x "$BIN" ] && [ -f "$SRC/$CFG" ] && [ -f "$SRC/disk0.img" ] ||
  { echo "d3d_check: bad binary, guest or cfg"; exit 2; }
[ -x "$NADA/nada" ] || { echo "d3d_check: no nada at $NADA"; exit 2; }
[ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" != 4 ] ||
  { echo "d3d_check: memory pressure critical"; exit 2; }

# Build.
B=$(mktemp -d) || exit 2
{ cat "$NADA/lib/windows/kernel32.def"; printf '    LoadLibraryA\n    GetProcAddress\n'; } > "$B/kernel32.def"
"$NADA/nada" -t alpha-windows -o "$B/D3DCHECK.EXE" -I "$NADA/include" \
  -I "$NADA/include/windows" "$T/d3dcheck/d3dcheck.c" "$NADA/lib/windows.c" \
  "$NADA/lib/printf.c" "$NADA/lib/stdio.c" "$NADA/lib/string.c" \
  "$NADA/lib/stdlib.c" "$NADA/lib/ctype.c" "$NADA/lib/math.c" \
  "$NADA/lib/fltconv.c" "$B/kernel32.def" || { echo "d3d_check: build failed"; exit 2; }
printf '@echo off\r\nC:\\D3D\\D3DCHECK.EXE C:\\D3D > C:\\D3D\\OUT.TXT 2>&1\r\necho EXIT %%ERRORLEVEL%% >> C:\\D3D\\OUT.TXT\r\n' > "$B/RUN.BAT"

# Clone the guest and stage C:\D3D.
rm -rf "$RUN"; mkdir -p "$RUN"
for f in "$SRC"/*; do
  case "$(basename "$f")" in fb|run*.log|*.dmp|alphabox-run|memory_*) continue ;; esac
  cp -c -R "$f" "$RUN/" 2>/dev/null || cp -R "$f" "$RUN/"
done
IMG="$RUN/disk0.img@@16384"
mdeltree -i "$IMG" ::/D3D > /dev/null 2>&1
mmd -i "$IMG" ::/D3D &&
  mcopy -i "$IMG" "$B/D3DCHECK.EXE" "$B/RUN.BAT" ::/D3D/ ||
  { echo "d3d_check: staging failed"; exit 2; }
rm -rf "$B"

# Boot headless.
cd "$RUN" || exit 2
mkdir -p fb; : > keys.txt
SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb ALPHABOX_KEYPIPE=keys.txt \
  "$BIN" run "$CFG" > run.log 2>&1 &
PID=$!
echo "d3d_check: guest pid $PID"
stop() { kill "$PID" 2>/dev/null; for _ in $(seq 1 30); do kill -0 "$PID" 2>/dev/null || return 0; sleep 2; done; kill -9 "$PID" 2>/dev/null; }
trap 'stop; exit 2' INT TERM

# The desktop: the screen stops changing for 20 s, at least 90 s in. The
# dumper writes a frame every ~2 s whether or not anything changed, so it
# is the newest frame's content that has to hold still.
t=0; last=""; still=0
while [ $t -lt 600 ]; do
  kill -0 "$PID" 2>/dev/null || { echo "d3d_check: emulator exited"; tail -5 run.log; exit 2; }
  f=$(ls -t fb | head -1)
  h=$([ -n "$f" ] && shasum "fb/$f" | cut -c1-16)
  if [ -n "$h" ] && [ "$h" = "$last" ]; then still=$((still + 5)); else still=0; last=$h; fi
  [ $t -ge 90 ] && [ $still -ge 20 ] && break
  sleep 5; t=$((t + 5))
done
[ $t -lt 600 ] || { echo "d3d_check: no desktop after 600 s"; stop; exit 2; }
echo "d3d_check: desktop after ${t} s, starting the program"
echo "win-r $(python3 "$T/keys_for.py" 'C:\D3D\RUN.BAT') enter" >> keys.txt

# Wait for EXIT in OUT.TXT on a copy of the disk. What the guest writes
# reaches the image only when Windows' lazy writer flushes it (within a
# minute here), so the copy lags the program; once EXIT is seen, give the
# image files a little longer before stopping.
t=0
while [ $t -lt 900 ]; do
  sleep 10; t=$((t + 10))
  kill -0 "$PID" 2>/dev/null || { echo "d3d_check: emulator exited"; break; }
  cp -c disk0.img peek.img 2>/dev/null || cp disk0.img peek.img
  mtype -i "peek.img@@16384" ::/D3D/OUT.TXT 2>/dev/null | tr -d '\r' | grep -q '^EXIT' &&
    { echo "d3d_check: finished after ${t} s"; sleep 30; break; }
done
[ $t -lt 900 ] || echo "d3d_check: no EXIT after 900 s"
stop
wait "$PID" 2>/dev/null
mkdir -p results
cp -c disk0.img peek.img 2>/dev/null || cp disk0.img peek.img
mcopy -o -i "peek.img@@16384" "::/D3D/*" results/ > /dev/null 2>&1
rm -f peek.img results/D3DCHECK.EXE* results/RUN.BAT
[ -f results/OUT.TXT ] || { echo "d3d_check: no output (see $RUN/run.log)"; exit 2; }
# (not out.txt: macOS's file system would take that for OUT.TXT itself)
tr -d '\r' < results/OUT.TXT > results/result.txt && rm -f results/OUT.TXT
grep -q '^EXIT' results/result.txt || { echo "d3d_check: the program did not finish:"; tail -8 results/result.txt; exit 2; }

# Side-by-side images: HAL | software, and their difference below.
python3 - results <<'PY'
import glob, os, sys
d = sys.argv[1]
def load(p):
    raw = open(p, 'rb').read().split(b'\n', 3)
    w, h = map(int, raw[1].split())
    return w, h, raw[3]
for hal in sorted(glob.glob(os.path.join(d, '*-hal.ppm'))):
    name = os.path.basename(hal)[:-8]
    rgb = os.path.join(d, name + '-rgb.ppm')
    if not os.path.exists(rgb):
        continue
    w, h, a = load(hal)
    _, _, b = load(rgb)
    W, H = 2 * w + 8, 2 * h + 8
    out = bytearray(b'\x40' * (W * H * 3))
    for y in range(h):
        for x in range(w):
            s = (y * w + x) * 3
            for img, ox, oy in ((a, 0, 0), (b, w + 8, 0)):
                o = ((oy + y) * W + ox + x) * 3
                out[o:o + 3] = img[s:s + 3]
            dv = max(abs(a[s + i] - b[s + i]) for i in range(3))
            o = ((h + 8 + y) * W + x) * 3
            out[o:o + 3] = bytes((min(255, dv * 4),) * 3)
    ppm = os.path.join(d, name + '.ppm')
    open(ppm, 'wb').write(b'P6\n%d %d\n255\n' % (W, H) + bytes(out))
PY
for p in results/*.ppm; do
  case "$p" in *-hal.ppm|*-rgb.ppm) continue ;; esac
  python3 "$T/ppm2png.py" "$p" "${p%.ppm}.png" && rm -f "$p"
done
grep -E '^(RESULT|DONE|EXIT)|failed' results/result.txt
echo "d3d_check: images in $RUN/results"
grep -q '^EXIT 0' results/result.txt && exit 0
exit 1
