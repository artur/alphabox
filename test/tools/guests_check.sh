#!/bin/bash
# Boot every guest operating system Alphabox has media for, and check each one.
#
# usage: guests_check.sh <alphabox-binary> <label> [guest ...]
#
# With no guests: all of them, in this order:
#   w2k       Windows 2000 Professional (RC2, build 2128), 2 CPUs: the desktop,
#             no "installing new devices" dialog, and the taskbar clock reads
#             the host's time (read with tesseract when it is installed)
#   w2k-beta  Japanese Windows 2000 Server beta (build 1877), 2 CPUs (its HAL
#             sends IPIs to CPUs 0-1 only): the desktop
#   xp64      Windows XP 64-bit (Whistler 2210 AXP64), booted through
#             AlphaBIOS/ARC from the flash, auto-logon: the desktop
#   vms-es40  OpenVMS 8.4 on the ES40: log in to DCL, SHOW TIME (the host's
#             date) and SHOW SYSTEM
#   vms-es47  OpenVMS 8.4 on the ES47 (EV7): log in to DCL, SHOW TIME
#   linux     Gentoo on the ES40: log in, uname -a, date, compile and run a
#             tiny C program
#   arc       the AlphaBIOS/ARC console from a programmed flash: its menu
#   netbsd    the NetBSD/alpha install CD: the installer's menu
# Each guest boots on an APFS clone (cp -c) of its disk, deleted afterwards;
# the originals are never opened. A check waits for a marker (a frame with a
# taskbar and no dialog, a console prompt) with a timeout, never a fixed
# sleep. Windows guests are shut down from Start, Shut Down; the others from
# their own shutdown command.
#
# Output: one line per guest, `PASS|FAIL <guest> <seconds> <what was checked>`,
# and in $ALPHABOX_WORK/runs/guests-<label>/ (ALPHABOX_WORK defaults to
# <repo>/lab):
#   <guest>.png        the last frame (Windows, ARC), or the VGA frame if any
#   <guest>.log        the console: the serial session, or the emulator's own
#                      output for the guests that have no serial console
#   <guest>.chat       the serial session's step results (SRM and OS guests)
#   <guest>.warn       the %SYS-W-/%SYS-E-/FAILURE lines the emulator printed
#   summary.txt        the PASS/FAIL lines
# Exit 0 only if every guest passed.
#
# Environment:
#   ALPHABOX_WORK        output root (default <repo>/lab); the guests' images
#                        are under it: win2k-installed, win2k-jp-install,
#                        xp64-installed, vms84, platforms/marvel/m7, linux,
#                        netbsd (media: NetBSD-10.2-alpha.iso)
#   GUESTS_PORT_BASE     first telnet port (default 21950); a guest's port is
#                        the base plus its place in the list above
#   GUESTS_BOOT_TMO      seconds to wait for the guest to come up (default
#                        900; the Windows guests take 2 to 5 minutes)
#   GUESTS_MIN_FREE_GB   stop if the data volume has less free (default 3)
#   GUESTS_KEEP=1        keep the run directories (clones included)
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
ROMS=${ALPHABOX_ROMS:-$R/roms}
BASE=${GUESTS_PORT_BASE:-21950}
BTMO=${GUESTS_BOOT_TMO:-900}
MINFREE=${GUESTS_MIN_FREE_GB:-3}
ALL="w2k w2k-beta xp64 vms-es40 vms-es47 linux arc netbsd"

[ $# -ge 2 ] || { sed -n '2,3p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2; shift 2
GUESTS=${*:-$ALL}
[ -x "$BIN" ] || { echo "guests_check: $BIN is not executable"; exit 2; }
OUT=$WORK/runs/guests-$LABEL
mkdir -p "$OUT" || exit 2
: > "$OUT/summary.txt"
PID=

cleanup() {
  [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null && { kill "$PID" 2>/dev/null; sleep 1; kill -9 "$PID" 2>/dev/null; }
}
trap cleanup EXIT
trap 'exit 130' INT TERM

# ---------------------------------------------------------------------------
# The helper: frame checks, the clock, and the serial dialogue.
# ---------------------------------------------------------------------------
HELPER=$OUT/.helper.py
cat > "$HELPER" <<'PY'
import glob, os, re, shutil, socket, subprocess, sys, tempfile, time


def newest(d, n=1):
    f = sorted(glob.glob(os.path.join(d, "*.ppm")))
    return f[-n:] if f else []


def read_ppm(path):
    d = open(path, "rb").read()
    p = d.split(b"\n", 3)
    w, h = map(int, p[1].split())
    return w, h, p[3]


def px(img, x, y):
    w, h, b = img
    o = (y * w + x) * 3
    return b[o], b[o + 1], b[o + 2]


def white_share(path, box):
    """Share of nearly white pixels in box (x0, y0, x1, y1)."""
    img = read_ppm(path)
    x0, y0, x1, y1 = box
    n = t = 0
    for y in range(y0, y1, 3):
        for x in range(x0, x1, 3):
            t += 1
            if min(px(img, x, y)) >= 235:
                n += 1
    return n / t


def is_splash(path):
    """The Windows logon splash ("Press Ctrl+Alt+Del") or the "safe to turn
    off" screen: a white box in the middle of a plain screen, no taskbar."""
    w, h, _ = read_ppm(path)
    return h >= 400 and not has_taskbar_no_dialog(path) and \
        white_share(path, (w // 4, h * 3 // 8, w * 3 // 4, h * 5 // 8)) >= 0.35


def has_taskbar_no_dialog(path):
    """A Windows desktop: a gray taskbar along the bottom and a single colour
    in the middle of the screen (no dialog, no window, no logon box)."""
    img = read_ppm(path)
    w, h, _ = img
    if h < 400:
        return False
    cnt = {}
    for x in range(0, w, 2):
        c = px(img, x, h - 5)
        cnt[c] = cnt.get(c, 0) + 1
    c, n = max(cnt.items(), key=lambda kv: kv[1])
    if n < 0.55 * (w // 2) or max(c) - min(c) > 24 or min(c) < 160:
        return False
    cnt = {}
    tot = 0
    for y in range(h // 3, 2 * h // 3, 3):
        for x in range(w // 3, 2 * w // 3, 3):
            c = px(img, x, y)
            cnt[c] = cnt.get(c, 0) + 1
            tot += 1
    cc, cn = max(cnt.items(), key=lambda kv: kv[1])
    if cn < 0.97 * tot:
        return False
    # the same colour at the top right: a maximised window or a big dialog
    # is not the desktop
    hits = tot = 0
    for y in range(8, h // 5, 6):
        for x in range(w * 3 // 4, w - 8, 6):
            tot += 1
            hits += px(img, x, y) == cc
    return hits >= 0.98 * tot


def ocr(path, box, scale=6, psm="13", extra=(), pad=0):
    """Text in box (x0, y0, x1, y1; negative values count from the right and
    bottom edges) with tesseract, or None when it is not installed. pad adds
    a white margin of that many pixels around the enlarged text."""
    if not shutil.which("tesseract"):
        return None
    w, h, b = read_ppm(path)
    x0, y0, x1, y1 = [v + (w if i % 2 == 0 else h) if v < 0 else v
                      for i, v in enumerate(box)]
    width = (x1 - x0) * scale + 2 * pad
    rows = [b"\xff" * width] * pad
    for y in range(y0, y1):
        r = bytearray(b"\xff" * pad)
        for x in range(x0, x1):
            o = (y * w + x) * 3
            r += bytes([(b[o] * 30 + b[o + 1] * 59 + b[o + 2] * 11) // 100]) * scale
        r += b"\xff" * pad
        rows += [bytes(r)] * scale
    rows += [b"\xff" * width] * pad
    fd, pg = tempfile.mkstemp(suffix=".pgm")
    with os.fdopen(fd, "wb") as f:
        f.write(b"P5\n%d %d\n255\n" % (width, len(rows)))
        f.write(b"".join(rows))
    try:
        r = subprocess.run(["tesseract", pg, "-", "--psm", psm, *extra],
                           capture_output=True, text=True, timeout=60)
        return r.stdout.strip()
    finally:
        os.unlink(pg)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def fbwait(d, tmo, pid):
    """Wait for two successive frames that are a desktop."""
    end = time.time() + tmo
    good = 0
    last = None
    while time.time() < end:
        if not alive(pid):
            print("died")
            return 1
        f = newest(d)
        if f and f[-1] != last:
            last = f[-1]
            try:
                good = good + 1 if has_taskbar_no_dialog(last) else 0
            except (IndexError, ValueError):  # a frame still being written
                last = None
            if good >= 2:
                print("ok", os.path.basename(last))
                return 0
        time.sleep(2)
    print("timeout", os.path.basename(last) if last else "no-frame")
    return 1


def fbsplash(d, tmo, pid):
    """Wait for a frame that is the logon splash or the closing screen, and
    that has not changed for four frames (the boot splash has a progress
    bar that moves)."""
    end = time.time() + tmo
    last = None
    prev = None
    still = 0
    while time.time() < end:
        if not alive(pid):
            print("died")
            return 1
        f = newest(d)
        if f and f[-1] != last:
            last = f[-1]
            try:
                if is_splash(last):
                    cur = open(last, "rb").read()
                    still = still + 1 if cur == prev else 0
                    prev = cur
                    if still >= 3:
                        print("ok", os.path.basename(last))
                        return 0
                else:
                    prev = None
                    still = 0
            except (IndexError, ValueError):
                last = None
        time.sleep(2)
    print("timeout")
    return 1


def fbtext(d, tmo, pid, rx, box):
    """Wait until the text in box of the newest frame matches rx."""
    end = time.time() + tmo
    last = None
    while time.time() < end:
        if not alive(pid):
            print("died")
            return 1
        f = newest(d)
        if f and f[-1] != last:
            last = f[-1]
            t = ocr(last, box, scale=3, psm="6") or ""
            if re.search(rx, t, re.I):
                print("ok", os.path.basename(last))
                return 0
        time.sleep(2)
    print("timeout")
    return 1


def clock(frame):
    """The taskbar clock, as minutes since midnight, or None.

    The clock's 8-pixel font is small for tesseract, and each way of
    enlarging it misreads some time: without a margin "9:47" came back as
    "3:47", with one a colon goes missing. So three readings vote, and a
    time at least two agree on is the answer; "4M" is how "AM" often reads."""
    votes = []
    for scale, pad in ((6, 0), (6, 20), (8, 20)):
        t = ocr(frame, (-65, -20, -8, -3), scale=scale, pad=pad)
        if t is None:
            return None
        m = re.search(r"(\d{1,2}):?(\d\d)[.,]?\s*([AaPp4])M", t)
        if m and int(m.group(1)) in range(1, 13) and int(m.group(2)) < 60:
            pm = m.group(3) in "Pp"
            votes.append((int(m.group(1)) % 12 + (12 if pm else 0)) * 60 + int(m.group(2)))
    for v in votes:
        if votes.count(v) >= 2:
            return v
    return None


class Chat:
    """A scripted dialogue on a serial console over telnet."""

    def __init__(self, port, logf, deadline_s=120):
        self.log = open(logf, "wb", buffering=0)
        self.buf = ""
        self.replies = []
        s = None
        end = time.time() + deadline_s
        while time.time() < end:
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.5)
        if s is None:
            raise SystemExit("noconnect")
        s.settimeout(0.5)
        self.s = s

    def rd(self):
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return
        if not d:
            raise EOFError
        d = re.sub(rb"\xff[\xfb-\xfe].", b"", d).replace(b"\x00", b"")
        self.log.write(d)
        self.buf += d.decode("latin1").replace("\r", "")
        for rx, text in self.replies:
            m = re.search(rx, self.buf, re.M)
            if m:
                self.buf = self.buf[m.end():]
                self.send(text)

    def send(self, text):
        b = (text.replace("$DATE", time.strftime("%d-%b-%Y %H:%M", time.gmtime()).upper())
             ).encode() + b"\r"
        for i in range(0, len(b), 8):
            self.s.sendall(b[i:i + 8])
            time.sleep(0.02)

    def expect(self, rx, tmo):
        end = time.time() + tmo
        r = re.compile(rx, re.M)
        while time.time() < end:
            m = r.search(self.buf)
            if m:
                self.buf = self.buf[m.end():]
                return m
            self.rd()
        return None


def chat(port, logf, outf, steps):
    out = open(outf, "w")
    t0 = time.time()
    try:
        c = Chat(port, logf)
    except SystemExit as e:
        print("FAIL", e)
        return 1
    rc = 0
    last = None
    try:
        for st in steps:
            k, _, v = st.partition("=")
            if k == "expect":
                rx, _, secs = v.rpartition("@")
                m = c.expect(rx, float(secs))
                out.write("%s %.0fs expect %s\n" % ("ok" if m else "TIMEOUT", time.time() - t0, rx))
                out.flush()
                if not m:
                    print("FAIL no match for %r within %ss" % (rx, secs))
                    return 1
                last = m
            elif k == "send":
                c.send(v)
            elif k == "reply":
                rx, _, text = v.partition(">")
                c.replies.append((rx, text))
            elif k == "noreply":
                c.replies = []
            elif k == "sleep":
                end = time.time() + float(v)
                while time.time() < end:
                    c.rd()
            elif k == "cmd":
                # a command and its output: v = "text@secs"; the end marker
                # is printed by the shell/DCL after the command
                text, _, secs = v.rpartition("@")
                c.send(text)
            elif k == "grab":
                # grab=name:regex: the first group of the regex over the
                # text since the last expect, written to the result file
                name, _, rx = v.partition(":")
                m = re.search(rx, c.buf, re.M)
                out.write("grab %s %s\n" % (name, m.group(1).strip() if m else ""))
            elif k == "last":
                # last=name: the text the last expect consumed up to
                out.write("last %s %s\n" % (v, last.group(0).strip() if last else ""))
    except EOFError:
        print("FAIL console closed")
        return 1
    print("ok %.0f" % (time.time() - t0))
    return rc


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "fbwait":
        sys.exit(fbwait(a[1], float(a[2]), int(a[3])))
    if a[0] == "fbsplash":
        sys.exit(fbsplash(a[1], float(a[2]), int(a[3])))
    if a[0] == "fbtext":
        sys.exit(fbtext(a[1], float(a[2]), int(a[3]), a[4], tuple(int(v) for v in a[5].split(","))))
    if a[0] == "clock":
        c = clock(a[1])
        if c is None:
            print("unread")
            sys.exit(2)
        now = time.localtime()
        host = now.tm_hour * 60 + now.tm_min
        d = abs(c - host)
        d = min(d, 1440 - d)
        print("%02d:%02d" % (c // 60, c % 60), "host %02d:%02d" % (now.tm_hour, now.tm_min),
              "diff", d)
        sys.exit(0 if d <= 5 else 1)
    if a[0] == "ocr":
        print(ocr(a[1], tuple(int(v) for v in a[2].split(",")), scale=int(a[3]), psm=a[4]) or "")
        sys.exit(0)
    if a[0] == "chat":
        sys.exit(chat(int(a[1]), a[2], a[3], a[4:]))
PY

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
H() { python3 -I "$HELPER" "$@"; }

report() { # report <guest> <PASS|FAIL> <seconds> <what>
  echo "$2 $1 $3 $4" | tee -a "$OUT/summary.txt"
}

stop_emu() {
  [ -n "$PID" ] || return 0
  kill -0 "$PID" 2>/dev/null || { wait "$PID" 2>/dev/null; PID=; return 0; }
  kill "$PID" 2>/dev/null
  local n=0
  while kill -0 "$PID" 2>/dev/null && [ $n -lt 150 ]; do sleep 0.2; n=$((n + 1)); done
  kill -0 "$PID" 2>/dev/null && kill -9 "$PID" 2>/dev/null
  wait "$PID" 2>/dev/null
  PID=
}

warnings() { # warnings <guest> <dir>: what the emulator complained about
  grep -a -E '%SYS-[WE]-|FAILURE|Emulator Failure|OPCDEC' "$2/alphabox.out" 2>/dev/null |
    tr -d '\r\000' > "$OUT/$1.warn"
  if [ -s "$OUT/$1.warn" ]; then
    sort "$OUT/$1.warn" | uniq -c | sort -rn | head -8 | sed 's/^/    emu: /'
  else
    echo "    emu: no %SYS-W-/%SYS-E-/FAILURE lines"
  fi
}

# start_emu <dir> <cfg> [VAR=value ...]: the emulator in <dir>, PID in $PID.
# Frames go to <dir>/fb; keys are typed from <dir>/keys.txt.
start_emu() {
  local d=$1 cfg=$2; shift 2
  mkdir -p "$d/fb"; : > "$d/keys.txt"
  ( cd "$d" && exec env "$@" SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb/fb \
      ALPHABOX_KEYPIPE=keys.txt "$BIN" run "$cfg" ) \
    > >(head -c 100000000 > "$d/alphabox.out") 2>&1 &
  PID=$!
}

keys() { echo "$*" >> "$D/keys.txt"; }

lastframe() { # lastframe <dir> <out.png>
  local f; f=$(ls "$1"/fb/*.ppm 2>/dev/null | tail -1)
  [ -n "$f" ] && python3 -I "$T/ppm2png.py" "$f" "$2"
}

# Windows 2000/XP: Start, Shut Down, OK; then wait for the closing screen.
win_shutdown() { # win_shutdown <timeout> [jp]
  keys ctrl-esc; sleep 3; keys u; sleep 5
  # the Japanese beta offers Log off first: F4 opens the list, Up Up Enter
  # picks Shut down
  [ "${2:-}" = jp ] && { keys f4; sleep 3; keys up up enter; sleep 3; }
  keys enter
  sleep 5
  case $(H fbsplash "$D/fb" "$1" "$PID" 2>&1 | tail -1) in ok*) echo "shutdown" ;; *) echo "no-shutdown" ;; esac
}

# A Windows guest from an install directory.
#   win_guest <guest> <install-dir> <cfg> <desktop-timeout> <clock 0|1> [logon]
# With logon, the splash's Ctrl+Alt+Del and an empty Administrator password.
win_guest() {
  local g=$1 src=$WORK/$2 cfg=$3 tmo=$4 clk=$5 logon=${6:-} f r
  D=$OUT/$g; rm -rf "$D"; mkdir -p "$D"
  for f in "$src"/*; do
    case $(basename "$f") in
      fb|fb-*|*.png|*.log|*.txt|*backup*|*pre-*|disk0*.img) continue ;;
    esac
    if [ -L "$f" ]; then
      # a link to the media: made absolute, so it resolves from the run dir
      t=$(readlink "$f")
      case $t in /*) ;; *) t=$(cd "$src/$(dirname "$t")" 2>/dev/null && pwd)/$(basename "$t") ;; esac
      ln -s "$t" "$D/$(basename "$f")"
    elif [ -f "$f" ]; then
      cp -c "$f" "$D/" 2>/dev/null || cp "$f" "$D/"
    fi
  done
  cp -c "$src/disk0.img" "$D/disk0.img" || return 2
  start_emu "$D" "$cfg"
  if [ -n "$logon" ]; then
    r=$(H fbsplash "$D/fb" "$tmo" "$PID" 2>&1 | tail -1)
    case $r in ok*) sleep 5; keys ctrl-alt-del; sleep 6; keys enter ;; esac
  fi
  r=$(H fbwait "$D/fb" "$tmo" "$PID" 2>&1 | tail -1)
  lastframe "$D" "$OUT/$g.png"
  W_RESULT=$r
  case $r in
    ok*) ;;
    *) W_WHAT="no desktop ($r)"; return 1 ;;
  esac
  W_WHAT="desktop"
  if [ "$clk" = 1 ]; then
    local c; c=$(H clock "$(ls "$D"/fb/*.ppm | tail -1)")
    case $? in
      0) W_WHAT="$W_WHAT, clock $c" ;;
      2) W_WHAT="$W_WHAT, clock not read (no tesseract?)" ;;
      *) W_WHAT="$W_WHAT, clock WRONG ($c)"; return 1 ;;
    esac
  fi
  return 0
}

check_free() {
  local free_kb; free_kb=$(df -k "$WORK" | awk 'NR==2 {print $4}')
  [ "${free_kb:-0}" -ge $((MINFREE * 1048576)) ] || return 1
  [ "$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)" != 4 ] || return 2
}

# serial_cfg <dir> <port> <cpus> <bits> <disk> <cd>: an ES40 whose console is
# the telnet port, an ALi IDE with the disk (and a CD) on it.
serial_cfg() {
  local d=$1 port=$2 cpus=$3 bits=$4 disk=$5 cd=${6:-} i
  {
    echo 'sys0 = tsunami'; echo '{'
    echo "  memory.bits = $bits;"
    echo '  rom.srm = "cl67srmrom.exe";'
    echo '  rom.decompressed = "decompressed.rom";'
    echo '  rom.flash = "flash.rom";'
    echo '  rom.dpr = "dpr.rom";'
    for ((i = 0; i < cpus; i++)); do echo "  cpu$i = ev68cb { speed = 833M; }"; done
    echo "  serial0 = serial { address = \"127.0.0.1\"; port = $port; }"
    echo '  serial1 = serial { null_attach = true; }'
    echo '  pci0.7 = ali { vga_console = false; }'
    echo '  pci0.15 = ali_ide'; echo '  {'
    [ -n "$disk" ] && echo "    disk0.0 = file { file = \"$disk\"; }"
    [ -n "$cd" ] && echo "    disk1.0 = file { file = \"$cd\"; read_only = true; cdrom = true; }"
    echo '  }'
    echo '  pci0.19 = ali_usb { }'
    echo '}'
  } > "$d/es40.cfg"
}

# serial_guest <guest> <port> <steps...>: start the emulator on <D>/es40.cfg
# and run the dialogue; the console log goes to $OUT/<guest>.log.
SG_RESULT=
serial_guest() {
  local g=$1 port=$2; shift 2
  start_emu "$D" es40.cfg
  SG_RESULT=$(H chat "$port" "$D/console.log" "$D/chat.txt" "$@")
  cp "$D/chat.txt" "$OUT/$g.chat" 2>/dev/null
  tr -d '\r\000' < "$D/console.log" | tail -c 400000 > "$OUT/$g.log"
  case $SG_RESULT in ok*) return 0 ;; *) return 1 ;; esac
}

# the DCL dialogue shared by both OpenVMS guests
vms_steps() { # vms_steps <boot-device> <boot-timeout>
  echo "expect=P00>>>@240"
  echo "send=boot $1"
  echo "reply=enter date and time[^\n]*\n?>\$DATE"
  echo "expect=job terminated at@$2"
  echo "sleep=3"
  echo "send="
  echo "expect=Username: @120"
  echo "send=SYSTEM"
  echo "expect=Password: @60"
  echo "send=Alphabox84"
  echo "expect=\\$ \$@240"
  echo "send=SET TERMINAL/WIDTH=132/PAGE=0"
  echo "expect=\\$ \$@60"
}

rc_all=0
for g in $GUESTS; do
  idx=0; for x in $ALL; do [ "$x" = "$g" ] && break; idx=$((idx + 1)); done
  PORT=$((BASE + idx))
  t0=$(date +%s)
  secs() { echo $(( $(date +%s) - t0 )); }
  fail() { stop_emu; report "$g" FAIL "$(secs)" "$1"; rc_all=1
           [ -d "$D" ] && lastframe "$D" "$OUT/$g.png"
           [ -f "$D/alphabox.out" ] && [ ! -f "$OUT/$g.log" ] && tr -d '\r\000' < "$D/alphabox.out" | tail -c 100000 > "$OUT/$g.log"
           [ -d "$D" ] && warnings "$g" "$D"
           [ "${GUESTS_KEEP:-0}" = 1 ] || rm -rf "$D"; }
  D=$OUT/$g
  case $g in
  w2k|w2k-beta|xp64|vms-es40|vms-es47|linux|arc|netbsd) ;;
  *) echo "FAIL $g 0 unknown guest"; rc_all=1; continue ;;
  esac
  check_free; case $? in
    1) D=; fail "less than ${MINFREE} GB free on the data volume"; continue ;;
    2) D=; fail "memory pressure critical, not starting"; continue ;;
  esac

  case $g in
  w2k)
    if win_guest w2k win2k-installed es40-window-smp.cfg 600 1; then
      what=$W_WHAT
      sd=$(win_shutdown 240); what="$what, $sd"
      lastframe "$D" "$OUT/$g.png"
      report "$g" PASS "$(secs)" "$what"
    else fail "$W_WHAT"; continue; fi ;;
  w2k-beta)
    if win_guest w2k-beta win2k-jp-install es40-2cpu.cfg 900 0 logon; then
      what="$W_WHAT, 2 CPUs"
      sd=$(win_shutdown 240 jp); what="$what, $sd"
      lastframe "$D" "$OUT/$g.png"
      report "$g" PASS "$(secs)" "$what"
    else fail "$W_WHAT"; continue; fi ;;
  xp64)
    if win_guest xp64 xp64-installed es40-window.cfg 900 0; then
      what=$W_WHAT
      sd=$(win_shutdown 240); what="$what, $sd"
      lastframe "$D" "$OUT/$g.png"
      report "$g" PASS "$(secs)" "$what"
    else fail "$W_WHAT"; continue; fi ;;
  vms-es40)
    src=$WORK/vms84/disk-installed.img
    rm -rf "$D"; mkdir -p "$D"
    cp "$R/test/rom/cl67srmrom.exe" "$D/" && chmod u+w "$D/cl67srmrom.exe"
    cp -c "$src" "$D/disk.img" || { fail "no image $src"; continue; }
    serial_cfg "$D" "$PORT" 1 30 disk.img
    steps=(); while IFS= read -r l; do steps+=("$l"); done < <(vms_steps dqa0 "$BTMO")
    steps+=("send=SHOW TIME" "expect=\\$ \$@60" "send=SHOW SYSTEM" "expect=\\$ \$@60"
            "send=@SYS\$SYSTEM:SHUTDOWN" "reply=(\\]\\??:?|\\?) ?\$>" "expect=SHUTDOWN COMPLETE|halted CPU|P00>>>@300")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      # the emulated RTC keeps the host's local time (see linux below); the
      # day without its leading zero, as SHOW TIME prints it
      d=$(date +%e-%b-%Y | tr a-z A-Z | sed 's/^ //')
      if grep -aqE "(^|[^0-9])0?$d" "$D/console.log" && grep -aq 'Pid *Process Name' "$D/console.log"; then
        stop_emu; report "$g" PASS "$(secs)" "DCL login, SHOW TIME ($d), SHOW SYSTEM, SHUTDOWN"
      else fail "logged in, but SHOW TIME/SHOW SYSTEM output not as expected"; continue; fi
    else fail "console dialogue: $SG_RESULT"; continue; fi ;;
  vms-es47)
    src=$WORK/platforms/marvel/m7/disk-installed.img
    rm -rf "$D"; mkdir -p "$D"
    cp -c "$src" "$D/disk.img" || { fail "no image $src"; continue; }
    {
      echo 'sys0 = tsunami'; echo '{'
      echo '  platform = "es47";'; echo '  memory.bits = 30;'
      echo "  rom.srm = \"$ROMS/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE\";"
      echo '  rom.nvram = "cmm_nvram.bin";'
      echo '  cpu0 = ev7 { speed = 1000M; }'; echo '  cpu1 = ev7 { speed = 1000M; }'
      echo '  pci2.1 = sym53c895 { disk0.0 = file { file = "disk.img"; } }'
      echo '  pci0.1 = dec21143 { type = "null"; }'
      echo "  serial0 = serial { address = \"127.0.0.1\"; port = $PORT; }"
      echo '}'
    } > "$D/es40.cfg"
    steps=(); while IFS= read -r l; do steps+=("$l"); done < <(vms_steps dka0 "$BTMO")
    steps+=("send=SHOW TIME" "expect=\\$ \$@60" "send=SHOW CPU" "expect=\\$ \$@60"
            "send=@SYS\$SYSTEM:SHUTDOWN" "reply=(\\]\\??:?|\\?) ?\$>" "expect=SHUTDOWN COMPLETE|halted CPU|P00>>>@300")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      d=$(date +%e-%b-%Y | tr a-z A-Z | sed 's/^ //')
      if grep -aqE "(^|[^0-9])0?$d" "$D/console.log"; then
        stop_emu; report "$g" PASS "$(secs)" "DCL login, SHOW TIME ($d), SHOW CPU, SHUTDOWN"
      else fail "logged in, but SHOW TIME output not as expected"; continue; fi
    else fail "console dialogue: $SG_RESULT"; continue; fi ;;
  linux)
    src=$WORK/linux
    rm -rf "$D"; mkdir -p "$D"
    cp "$R/test/rom/cl67srmrom.exe" "$D/" && chmod u+w "$D/cl67srmrom.exe"
    # a flash with os_type UNIX: from the run that installed the system
    for f in flash.rom dpr.rom; do cp "$src/run-disk/$f" "$D/" || { fail "no $src/run-disk/$f"; continue 2; }; done
    cp -c "$src/disk/gentoo-installed.img" "$D/disk.img" || { fail "no image"; continue; }
    serial_cfg "$D" "$PORT" 1 30 disk.img
    steps=("expect=P00>>>@240" "send=boot dqa0 -flags 0" "expect=login: @$BTMO" "send=root"
           "expect=Password: @60" "send=alphabox" "expect=# \$@120"
           "send=uname -a; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@60"
           "send=date; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@60"
           "send=echo '#include <stdio.h>' > /tmp/t.c; echo 'int main(){printf(\"gc-sum=%d\\n\",6*7);return 0;}' >> /tmp/t.c; gcc -O1 -o /tmp/t /tmp/t.c && /tmp/t; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@300"
           "send=sync; poweroff" "expect=halted CPU|P00>>>@180")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      # the guest's `date` against the host's clock. The emulated RTC keeps
      # the host's local time, which a guest with its zone at UTC prints as
      # UTC, so the host is compared in local time.
      gt=$(grep -a -o "$(date '+%a %b %e') [0-9][0-9]:[0-9][0-9]:[0-9][0-9] [^ ]* $(date +%Y)" "$D/console.log" | head -1)
      gt=$(echo "$gt" | grep -o '[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' | head -1)
      gm=$(( 10#${gt:0:2} * 60 + 10#${gt:3:2} )); hm=$(( 10#$(date +%H) * 60 + 10#$(date +%M) ))
      dm=$(( gm > hm ? gm - hm : hm - gm )); [ $dm -gt 720 ] && dm=$((1440 - dm))
      if grep -aq 'Linux es40 [0-9]' "$D/console.log" && grep -aq "gc-sum=42" "$D/console.log" &&
         [ -n "$gt" ] && [ $dm -le 5 ]; then
        stop_emu; report "$g" PASS "$(secs)" "login, uname -a, date (${gt:0:5}, host local time within ${dm} min), gcc compile and run, poweroff"
      else fail "logged in, but uname/date/gcc output not as expected (date '$gt')"; continue; fi
    else fail "console dialogue: $SG_RESULT"; continue; fi ;;
  arc)
    src=$WORK/xp64-installed
    rm -rf "$D"; mkdir -p "$D"
    for f in flash.rom dpr.rom cl67srmrom.exe 86c764x1.bin; do cp "$src/$f" "$D/" || { fail "no $src/$f"; continue 2; }; done
    # nothing to boot: AlphaBIOS stops at its menu
    : > "$D/disk0.img"; truncate -s 64m "$D/disk0.img"
    cat > "$D/es40.cfg" <<EOF
gui = sdl { keyboard.use_mapping = false; }
sys0 = tsunami
{
  memory.bits = 28;
  rom.srm = "cl67srmrom.exe";
  rom.decompressed = "decompressed.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  arc_year_compat = true;
  cpu0 = ev68cb { speed = 800M; skip_memtest_hack = true; }
  serial0 = serial { null_attach = true; }
  pci0.2 = s3 { rom = "86c764x1.bin"; }
  pci0.15 = ali_ide { disk0.0 = file { file = "disk0.img"; } }
  pci0.7 = ali { vga_console = true; }
  pci0.19 = ali_usb { }
}
EOF
    start_emu "$D" es40.cfg
    r=$(H fbtext "$D/fb" "$BTMO" "$PID" 'AlphaBIOS' 0,0,640,120)
    lastframe "$D" "$OUT/$g.png"
    case $r in
      ok*) stop_emu; report "$g" PASS "$(secs)" "AlphaBIOS/ARC from flash, its menu came up"
           tr -d '\r\000' < "$D/alphabox.out" | tail -c 100000 > "$OUT/$g.log"; warnings "$g" "$D"
           [ "${GUESTS_KEEP:-0}" = 1 ] || rm -rf "$D"; continue ;;
      *) fail "no AlphaBIOS menu ($r)"; continue ;;
    esac ;;
  netbsd)
    iso=$WORK/netbsd/NetBSD-10.2-alpha.iso
    [ -f "$iso" ] || { D=; fail "no media $iso"; continue; }
    rm -rf "$D"; mkdir -p "$D"
    cp "$R/test/rom/cl67srmrom.exe" "$D/" && chmod u+w "$D/cl67srmrom.exe"
    serial_cfg "$D" "$PORT" 1 28 "" "$iso"
    steps=("expect=P00>>>@240" "send=boot dqb0" "expect=(Install System|Installation messages|Terminal type)@$BTMO"
           "send=" "expect=(Install System|Installation messages)@120")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      stop_emu; report "$g" PASS "$(secs)" "SRM boot dqb0 from the CD, the installer menu came up"
    else fail "console dialogue: $SG_RESULT"; continue; fi ;;
  esac
  warnings "$g" "$D"
  stop_emu
  [ -f "$D/alphabox.out" ] && [ ! -s "$OUT/$g.log" ] && tr -d '\r\000' < "$D/alphabox.out" | tail -c 100000 > "$OUT/$g.log"
  [ -f "$D/console.log" ] && [ ! -s "$OUT/$g.log" ] && tr -d '\r\000' < "$D/console.log" | tail -c 400000 > "$OUT/$g.log"
  [ "${GUESTS_KEEP:-0}" = 1 ] || rm -rf "$D"
done
exit $rc_all
