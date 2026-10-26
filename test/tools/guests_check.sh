#!/bin/bash
# Boot every guest operating system Alphabox has media for, and check each one.
#
# usage: guests_check.sh <alphabox-binary> <label> [guest ...]
#
# With no guests: all of them, in this order:
#   w2k       Windows 2000 Professional (RC2, build 2128), 2 CPUs: the desktop,
#             no "installing new devices" dialog, and the taskbar clock reads
#             the host's time (read by matching the font's glyphs)
#   w2k-beta  Japanese Windows 2000 Server beta (build 1877), 2 CPUs (its HAL
#             sends IPIs to CPUs 0-1 only): the desktop
#   xp64      Windows XP 64-bit (Whistler 2210 AXP64), booted through
#             AlphaBIOS/ARC from the flash, auto-logon: the desktop
#   vms-es40  OpenVMS 8.4 on the ES40: log in to DCL, SHOW TIME (the host's
#             date) and SHOW SYSTEM
#   vms-es47  OpenVMS 8.4 on the ES47 (EV7): log in to DCL, SHOW TIME
#   linux     Gentoo on the ES40: log in, uname -a, date, compile and run a
#             tiny C program
#   linux-es47  the same install on the ES47's two EV7s, with the CD's kernel
#             for Marvel (linux/disk/gentoo-es47.img, made by
#             test/tools/es47_linux.sh retarget): log in, uname -a, the
#             processors and the machine Linux found, date, poweroff
#   arc       the AlphaBIOS/ARC console from a programmed flash: its menu
#   netbsd    the NetBSD/alpha install CD: the installer's menu
# Not in the default list (they add about 12 minutes), named when needed:
#   vms-ds20e, vms-ds20, vms-ds10, vms-ds20l
#             OpenVMS 8.4 (the disk installed on the ES40) on those boards,
#             each with its own console, IDE place and processor count (2, 2,
#             1, 2): boot dqa0, log in, SHOW CPU and the active CPU count,
#             F$GETSYI("HW_NAME"), a DMA exercise (BACKUP of SYS$BASE_IMAGE.EXE
#             and DIFFERENCES of the copy: no differences), SHOW ERROR (no
#             device with errors), SHUTDOWN. They guard what an operating
#             system reads from the chipset (Cchip revision, Pchip and TIG
#             values); the DS20L's console comes from the flash the update
#             utility wrote (lab/ds20l-lfu).
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
#   GUESTS_EV7_CPU       the processor class of vms-es47 and linux-es47
#                        (default ev7; ev7z, or ev8, the 21464 in
#                        EV7-compatible mode)
#   GUESTS_EV8_TPUS      with ev8: each chip's TPUs, 1 to 4, written as its
#                        `tpus` (unset: no key, one processor per chip);
#                        each TPU is a processor (docs/platforms/ev8.md, S4),
#                        and SHOW CPU's active set and Linux's "cpus active"
#                        must count 2 * TPUs
#   GUESTS_EXTRA_CMDS    more commands for vms-es47, linux and linux-es47,
#                        after their own and before the shutdown (the Linux
#                        guests: before their date, which is compared with
#                        the host's clock), separated by ';'
#                        (e.g. 'SHOW CPU/FULL' or 'cat /proc/cpuinfo')
#   GUESTS_OUT           the output directory (default
#                        $ALPHABOX_WORK/runs/guests-<label>)
#
# Sourced from another script (test/tools/clock_drift.sh) with the binary
# and a label as its positional parameters, this file defines the helper and
# the functions below and runs no guest.
set -u
export LC_ALL=C
T=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$T/../.." && pwd)
WORK=${ALPHABOX_WORK:-$R/lab}
ROMS=${ALPHABOX_ROMS:-$R/roms}
BASE=${GUESTS_PORT_BASE:-21950}
BTMO=${GUESTS_BOOT_TMO:-900}
MINFREE=${GUESTS_MIN_FREE_GB:-3}
ALL="w2k w2k-beta xp64 vms-es40 vms-es47 linux linux-es47 arc netbsd"
# guests that run only when named; they follow ALL in the port numbering
EXTRA="vms-ds20e vms-ds20 vms-ds10 vms-ds20l"

[ $# -ge 2 ] || { sed -n '2,3p' "$0"; exit 2; }
BIN=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
LABEL=$2; shift 2
GUESTS=${*:-$ALL}
[ -x "$BIN" ] || { echo "guests_check: $BIN is not executable"; exit 2; }
OUT=${GUESTS_OUT:-$WORK/runs/guests-$LABEL}
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


# The taskbar clock's font (Windows 2000, 8 pixels high): each glyph as its
# 8 rows of "#" (dark) and "." joined by "/". Add a missing one from the
# pattern the failure message prints.
CLOCK_FONT = {
    ".###./#...#/#...#/#...#/#...#/#...#/#...#/.###.": "0",
    ".#./##./.#./.#./.#./.#./.#./###": "1",
    ".###./#...#/....#/...#./..#../.#.../#..../#####": "2",
    ".###./#...#/....#/..##./....#/....#/#...#/.###.": "3",
    "...#./..##./.#.#./#..#./#####/...#./...#./...#.": "4",
    "#####/#..../#..../####./....#/....#/#...#/.###.": "5",
    "..##./.#.../#..../####./#...#/#...#/#...#/.###.": "6",
    "#####/....#/...#./...#./..#../..#../.#.../.#...": "7",
    ".###./#...#/#...#/.###./#...#/#...#/#...#/.###.": "8",
    ".###./#...#/#...#/#...#/.####/....#/...#./.##..": "9",
    "././#/#/././#/#": ":",
    "..##../..##../.#..#./.#..#./.#..#./######/#....#/#....#": "A",
    "####./#...#/#...#/#...#/####./#..../#..../#....": "P",
    "##...##/##...##/#.#.#.#/#.#.#.#/#..#..#/#..#..#/#.....#/#.....#": "M",
}


def clock_read(frame):
    """The taskbar clock, as (minutes since midnight, None), or (None, why).

    The clock is drawn in a fixed bitmap font, so each character is an exact
    pixel pattern: crop the tray, take dark pixels as text, split into glyphs
    at empty columns and look each one up in CLOCK_FONT."""
    w, h, _ = img = read_ppm(frame)
    if h < 400:
        return None, "frame is %dx%d, not a desktop" % (w, h)
    xs = range(w - 70, w - 6)
    ys = range(h - 24, h - 3)

    def dark(x, y):
        return sum(px(img, x, y)) < 384

    rows = [y for y in ys if any(dark(x, y) for x in xs)]
    if not rows:
        return None, "no text in the tray"
    band = range(rows[0], rows[0] + 8)
    spans, cur = [], None
    for x in xs:
        if any(dark(x, y) for y in band):
            cur = [x, x] if cur is None else [cur[0], x]
        elif cur:
            spans.append(cur)
            cur = None
    if cur:
        spans.append(cur)
    text, unknown = "", []
    for a, b in spans:
        g = "/".join("".join("#" if dark(x, y) else "." for x in range(a, b + 1)) for y in band)
        c = CLOCK_FONT.get(g)
        if c is None:
            unknown.append(g)
            c = "?"
        text += c
    if unknown:
        return None, "unknown glyph(s) in '%s': %s" % (text, " | ".join(unknown))
    m = re.fullmatch(r"(\d{1,2}):(\d\d)([AP])M", text)
    if not m or not 1 <= int(m.group(1)) <= 12 or int(m.group(2)) > 59:
        return None, "'%s' is not a time" % text
    return (int(m.group(1)) % 12 + (12 if m.group(3) == "P" else 0)) * 60 + int(m.group(2)), None


def clock(frame):
    """The taskbar clock as minutes since midnight, or None (see clock_read)."""
    return clock_read(frame)[0]


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
        self.t_rx = (time.time(), time.monotonic())

    def rd(self):
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return
        if not d:
            raise EOFError
        # when the newest data arrived: the host's wall clock and its
        # monotonic clock (a step whose two clocks disagree slept)
        self.t_rx = (time.time(), time.monotonic())
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
            elif k == "stamp":
                # stamp=name:regex@secs: expect the regex, and write its
                # first group with the host's wall and monotonic clocks
                # when the data that completed it arrived
                spec, _, secs = v.rpartition("@")
                name, _, rx = spec.partition(":")
                m = c.expect(rx, float(secs))
                if not m:
                    out.write("TIMEOUT %.0fs stamp %s\n" % (time.time() - t0, name))
                    print("FAIL no match for %r within %ss" % (rx, secs))
                    return 1
                out.write("stamp %s %.6f %.6f %s\n" % (name, c.t_rx[0], c.t_rx[1], m.group(1).strip()))
                out.flush()
                last = m
            elif k == "load":
                # load=name: the host's load averages, and its clocks
                out.write("load %s %.6f %.6f %.2f %.2f %.2f\n" % (
                    (v, time.time(), time.monotonic()) + os.getloadavg()))
                out.flush()
    except EOFError:
        print("FAIL console closed")
        return 1
    print("ok %.0f" % (time.time() - t0))
    return rc


def tap(port, logf, outf, connect_s):
    """Listen on a serial console until it closes: every line that is a
    time of day with hundredths (Windows' %TIME%) is written to outf with
    the host's wall and monotonic clocks when it arrived."""
    try:
        c = Chat(port, logf, deadline_s=connect_s)
    except SystemExit as e:
        print("FAIL", e)
        return 1
    out = open(outf, "w", buffering=1)
    rx = re.compile(r"(\d{1,2}:\d\d:\d\d[.,]\d\d)[ \t]*\n")
    try:
        while True:
            c.rd()
            m = rx.search(c.buf)
            while m:
                out.write("stamp com1 %.6f %.6f %s\n" % (c.t_rx[0], c.t_rx[1], m.group(1)))
                c.buf = c.buf[m.end():]
                m = rx.search(c.buf)
    except (EOFError, OSError):
        pass
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if a[0] == "fbwait":
        sys.exit(fbwait(a[1], float(a[2]), int(a[3])))
    if a[0] == "fbsplash":
        sys.exit(fbsplash(a[1], float(a[2]), int(a[3])))
    if a[0] == "fbtext":
        sys.exit(fbtext(a[1], float(a[2]), int(a[3]), a[4], tuple(int(v) for v in a[5].split(","))))
    if a[0] == "clock":
        c, why = clock_read(a[1])
        if c is None:
            print("unread:", why)
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
    if a[0] == "tap":
        sys.exit(tap(int(a[1]), a[2], a[3], float(a[4])))
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
  # Windows keeps local time in the TOY, and the first boot after a daylight
  # saving change moves it by the difference itself. A real machine's
  # battery clock would still keep the offset of the guest's last shutdown,
  # so the TOY starts at the host's offset when the image was last written
  # (an image from the summer, booted in the winter: local+60m).
  local tz=""
  if [ "$clk" = 1 ] && ! grep -q timezone "$D/$cfg"; then
    local a b m
    a=$(date -r "$src/disk0.img" +%z) b=$(date +%z)
    m=$(((${a:0:1}1 * (10#${a:1:2} * 60 + 10#${a:3:2})) - (${b:0:1}1 * (10#${b:1:2} * 60 + 10#${b:3:2}))))
    if [ "$m" != 0 ]; then
      [ "$m" -gt 0 ] && tz="local+${m}m" || tz="local${m}m"
      awk -v tz="$tz" '/^sys0 =/ {s = 1} s && /^\{/ {print; print "  timezone = \"" tz "\";"; s = 0; next} {print}' \
        "$D/$cfg" > "$D/$cfg.tz" && mv "$D/$cfg.tz" "$D/$cfg"
    fi
  fi
  # WIN_GUEST_PREP: a command run on the copied configuration before the
  # start (clock_drift.sh puts serial0 on a telnet port with it)
  if [ -n "${WIN_GUEST_PREP:-}" ]; then "$WIN_GUEST_PREP" "$D/$cfg" || return 2; fi
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
      0) W_WHAT="$W_WHAT, clock $c${tz:+ (TOY $tz, the offset when the image was written)}" ;;
      2) W_WHAT="$W_WHAT, clock not read (${c#unread: })" ;;
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
  # SERIAL_GUEST_STARTED: a command run once the emulator ($PID) is
  # started, before the dialogue (clock_drift.sh's DRIFT_DUTY)
  if [ -n "${SERIAL_GUEST_STARTED:-}" ]; then "$SERIAL_GUEST_STARTED"; fi
  SG_RESULT=$(H chat "$port" "$D/console.log" "$D/chat.txt" "$@")
  cp "$D/chat.txt" "$OUT/$g.chat" 2>/dev/null
  tr -d '\r\000' < "$D/console.log" | tail -c 400000 > "$OUT/$g.log"
  case $SG_RESULT in ok*) return 0 ;; *) return 1 ;; esac
}

# extra_steps vms|linux: GUESTS_EXTRA_CMDS as more steps of the dialogue
extra_steps() {
  local c cmds=()
  IFS=';' read -r -a cmds <<< "${GUESTS_EXTRA_CMDS:-}"
  for c in ${cmds[@]+"${cmds[@]}"}; do # bash 3.2: an empty array is unset
    c=${c#"${c%%[! ]*}"}
    [ -n "$c" ] || continue
    if [ "$1" = vms ]; then steps+=("send=$c" "expect=\\$ \$@120")
    else steps+=("send=$c; echo GC-DONE-\$?" "expect=GC-DONE-[0-9]@120"); fi
  done
}
EV7CPU=${GUESTS_EV7_CPU:-ev7}
# The ES47's two chips; with ev8 and GUESTS_EV8_TPUS each of that many TPUs,
# each a processor (docs/platforms/ev8.md, S4).
EV7OPT=; EV7N=2
if [ "$EV7CPU" = ev8 ] && [ -n "${GUESTS_EV8_TPUS:-}" ]; then
  EV7OPT=" tpus = $GUESTS_EV8_TPUS;"; EV7N=$((2 * GUESTS_EV8_TPUS))
fi
# How many processors an OpenVMS SHOW CPU's "Active" set holds ("0-7",
# "0,1,8,9", "0-3,8-11").
vms_active_count() { # vms_active_count <console log>
  tr -d '\r\000' < "$1" | sed -n 's/^ *Active  *\([0-9][0-9,-]*\) *$/\1/p' | tail -1 |
    tr ',' '\n' | awk -F- 'NF==2 {n += $2 - $1 + 1; next} NF==1 && $1 != "" {n++} END {print n + 0}'
}

# the DCL dialogue shared by both OpenVMS guests
vms_steps() { # vms_steps <boot-device> <boot-timeout> [prompt-regex]
  echo "expect=${3:-P00>>>}@240"   # the DS10 console's prompt has no P00
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

# board_cfg <dir> <port> <platform> <cpus> <rom> <ali> <ide> <usb>: a Tsunami
# family board (not the ES40) whose console is the telnet port, an ALi IDE
# with the disk at the board's own place. <rom> is a file name copied into
# <dir>, or - for a console already in the flash (the DS20L's, installed by
# its update utility); the places are the ones in test/tools/srm_cfg.py.
board_cfg() {
  local d=$1 port=$2 plat=$3 cpus=$4 rom=$5 ali=$6 ide=$7 usb=$8 i
  {
    echo 'sys0 = tsunami'; echo '{'
    echo "  platform = \"$plat\";"
    echo '  memory.bits = 30;'
    if [ "$rom" != - ]; then
      echo "  rom.srm = \"$rom\";"
      echo "  rom.decompressed = \"$rom-decompressed.rom\";"
    fi
    echo '  rom.flash = "flash.rom";'
    echo '  rom.dpr = "dpr.rom";'
    for ((i = 0; i < cpus; i++)); do echo "  cpu$i = ev68cb { speed = 833M; }"; done
    echo "  serial0 = serial { address = \"127.0.0.1\"; port = $port; }"
    echo '  serial1 = serial { null_attach = true; }'
    echo "  $ali = ali { vga_console = false; }"
    echo "  $ide = ali_ide"; echo '  {'
    echo '    disk0.0 = file { file = "disk.img"; }'
    echo '  }'
    [ -n "$usb" ] && echo "  $usb = ali_usb { }"
    echo '}'
  } > "$d/es40.cfg"
}

# vms_board <guest> <platform> <cpus> <rom|-> <ali> <ide> <usb>: OpenVMS 8.4
# on one board: the DCL login, then the checks the header lists. Returns 1
# after fail() has reported.
vms_board() {
  local g=$1 plat=$2 cpus=$3 rom=$4 ali=$5 ide=$6 usb=$7 src=$WORK/vms84/disk-installed.img f
  rm -rf "$D"; mkdir -p "$D"
  cp -c "$src" "$D/disk.img" || { fail "no image $src"; return 1; }
  if [ "$rom" = - ]; then
    for f in flash.rom dpr.rom; do
      cp "$WORK/ds20l-lfu/$f" "$D/" && chmod u+w "$D/$f" || { fail "no $WORK/ds20l-lfu/$f"; return 1; }
    done
  else
    f=$ROMS/alpha-firmware-v7.3/$rom; rom=$(basename "$rom")
    cp "$f" "$D/" && chmod u+w "$D/$rom" || { fail "no console $f"; return 1; }
  fi
  board_cfg "$D" "$PORT" "$plat" "$cpus" "$rom" "$ali" "$ide" "$usb"
  local steps=(); while IFS= read -r l; do steps+=("$l"); done < <(vms_steps dqa0 "$BTMO" '(P00>>>|\n>>>)')
  steps+=("send=SHOW CPU" "expect=\\$ \$@60"
          "send=WRITE SYS\$OUTPUT \"GC-HW=[\"+F\$GETSYI(\"HW_NAME\")+\"]\"" "expect=\\$ \$@60"
          "send=WRITE SYS\$OUTPUT \"GC-CPUS=\"+F\$STRING(F\$GETSYI(\"ACTIVECPU_CNT\"))" "expect=\\$ \$@60"
          "send=DELETE SYS\$SCRATCH:GC_COPY.EXE;*" "expect=\\$ \$@60"
          "send=BACKUP SYS\$COMMON:[SYS\$LDR]SYS\$BASE_IMAGE.EXE SYS\$SCRATCH:GC_COPY.EXE" "expect=\\$ \$@600"
          "send=WRITE SYS\$OUTPUT \"GC-BACKUP-DONE\"" "expect=\\$ \$@60"
          "send=DIFFERENCES SYS\$COMMON:[SYS\$LDR]SYS\$BASE_IMAGE.EXE SYS\$SCRATCH:GC_COPY.EXE" "expect=\\$ \$@600"
          "send=WRITE SYS\$OUTPUT \"GC-DIFF-DONE\"" "expect=\\$ \$@60"
          "send=DELETE SYS\$SCRATCH:GC_COPY.EXE;*" "expect=\\$ \$@60"
          "send=SHOW ERROR" "expect=\\$ \$@60"
          "send=WRITE SYS\$OUTPUT \"GC-ERR-DONE\"" "expect=\\$ \$@60"
          "send=@SYS\$SYSTEM:SHUTDOWN" "reply=(\\]\\??:?|\\?) ?\$>" "expect=SHUTDOWN COMPLETE|halted CPU|P00>>>@300")
  serial_guest "$g" "$PORT" "${steps[@]}" || { fail "console dialogue: $SG_RESULT"; return 1; }
  local txt hw ncpu nd ne why=
  txt=$(tr -d '\r\000' < "$D/console.log")
  hw=$(echo "$txt" | sed -n 's/^GC-HW=\[\(.*\)\]$/\1/p' | head -1)
  ncpu=$(echo "$txt" | sed -n 's/^GC-CPUS=\([0-9]*\)$/\1/p' | head -1)
  # DIFFERENCES prints "Number of difference sections found: 0" when equal
  nd=$(echo "$txt" | sed -n 's/.*difference sections found: *\([0-9]*\).*/\1/p' | head -1)
  # SHOW ERROR lists a device only when it has errors: count "NAME: n" lines
  # between the command and the marker. The IDE positions with no drive
  # (DQA1, DQB0, DQB1) each count the one failed probe at boot: not errors.
  ne=$(echo "$txt" | awk '/^\$ SHOW ERROR/ {on=1; next} /^GC-ERR-DONE/ {on=0} on && /^[A-Za-z_$][A-Za-z0-9_$]*: +[0-9]+ *$/ && $1 !~ /(DQA1|DQB0|DQB1):$/ {n++} END {print n+0}')
  [ -n "$hw" ] || why="$why no HW_NAME;"
  [ "$ncpu" = "$cpus" ] || why="$why active CPUs '$ncpu' not $cpus;"
  [ "$nd" = 0 ] || why="$why DIFFERENCES sections '$nd';"
  # BACKUP takes only the first directory of a search-list logical; a copy
  # that selected nothing (or failed) makes the DIFFERENCES meaningless.
  echo "$txt" | awk '/^\$ BACKUP/ {on=1} /^GC-BACKUP-DONE/ {on=0} on' | grep -aqE '%BACKUP-[WEF]-' && why="$why BACKUP warned or failed;"
  [ "$ne" = 0 ] || why="$why $ne devices with errors;"
  echo "$txt" | grep -aqE 'BUGCHECK|CPUSPINWAIT|INVEXCEPTN' && why="$why bugcheck;"
  [ -z "$why" ] || { fail "logged in, but:$why"; return 1; }
  stop_emu
  report "$g" PASS "$(secs)" "DCL login, HW_NAME $hw, $ncpu CPUs, BACKUP + DIFFERENCES 0, SHOW ERROR clean, SHUTDOWN"
}

# vms_es40_setup: the vms-es40 guest's run directory $D: the console, a
# clone of the installed disk, an ES40 with its console on port $PORT.
# `vms_steps dqa0` then boots it and logs in. On failure, $SETUP_WHY.
SETUP_WHY=
vms_es40_setup() {
  local src=$WORK/vms84/disk-installed.img
  rm -rf "$D"; mkdir -p "$D"
  cp "$R/test/rom/cl67srmrom.exe" "$D/" && chmod u+w "$D/cl67srmrom.exe"
  cp -c "$src" "$D/disk.img" || { SETUP_WHY="no image $src"; return 1; }
  serial_cfg "$D" "$PORT" 1 30 disk.img
}

# linux_setup: the linux guest's run directory $D: the console, a flash
# with os_type UNIX from the run that installed the system, a clone of the
# installed disk, an ES40 with its console on port $PORT.
linux_setup() {
  local src=$WORK/linux f
  rm -rf "$D"; mkdir -p "$D"
  cp "$R/test/rom/cl67srmrom.exe" "$D/" && chmod u+w "$D/cl67srmrom.exe"
  for f in flash.rom dpr.rom; do
    cp "$src/run-disk/$f" "$D/" || { SETUP_WHY="no $src/run-disk/$f"; return 1; }
  done
  cp -c "$src/disk/gentoo-installed.img" "$D/disk.img" || { SETUP_WHY="no image"; return 1; }
  serial_cfg "$D" "$PORT" 1 30 disk.img
}

# linux_login_steps: the dialogue that boots the linux guest and logs in as
# root, one step a line
linux_login_steps() {
  printf '%s\n' "expect=P00>>>@240" "send=boot dqa0 -flags 0" "expect=login: @$BTMO" \
    "send=root" "expect=Password: @60" "send=alphabox" "expect=# \$@120"
}

# Sourced (clock_drift.sh): the helper and the functions only.
if (return 0 2>/dev/null); then return 0; fi

rc_all=0
for g in $GUESTS; do
  idx=0; for x in $ALL $EXTRA; do [ "$x" = "$g" ] && break; idx=$((idx + 1)); done
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
  w2k|w2k-beta|xp64|vms-es40|vms-es47|linux|linux-es47|arc|netbsd|vms-ds20e|vms-ds20|vms-ds10|vms-ds20l) ;;
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
    vms_es40_setup || { fail "$SETUP_WHY"; continue; }
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
      echo "  cpu0 = $EV7CPU { speed = 1000M;$EV7OPT }"; echo "  cpu1 = $EV7CPU { speed = 1000M;$EV7OPT }"
      echo '  pci2.1 = sym53c895 { disk0.0 = file { file = "disk.img"; } }'
      echo '  pci0.1 = dec21143 { type = "null"; }'
      echo "  serial0 = serial { address = \"127.0.0.1\"; port = $PORT; }"
      echo '}'
    } > "$D/es40.cfg"
    steps=(); while IFS= read -r l; do steps+=("$l"); done < <(vms_steps dka0 "$BTMO")
    steps+=("send=SHOW TIME" "expect=\\$ \$@60" "send=SHOW CPU" "expect=\\$ \$@60")
    extra_steps vms
    steps+=("send=@SYS\$SYSTEM:SHUTDOWN" "reply=(\\]\\??:?|\\?) ?\$>" "expect=SHUTDOWN COMPLETE|halted CPU|P00>>>@300")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      d=$(date +%e-%b-%Y | tr a-z A-Z | sed 's/^ //')
      na=$(vms_active_count "$D/console.log")
      if grep -aqE "(^|[^0-9])0?$d" "$D/console.log" && [ "$na" = "$EV7N" ]; then
        stop_emu; report "$g" PASS "$(secs)" "DCL login, SHOW TIME ($d), SHOW CPU ($na active), SHUTDOWN"
      else fail "logged in, but SHOW TIME or SHOW CPU output not as expected ($na active, not $EV7N)"; continue; fi
    else fail "console dialogue: $SG_RESULT"; continue; fi ;;
  vms-ds20e) vms_board "$g" ds20e 2 DS20/PC264SRM.ROM pci0.5 pci0.6 "" || continue ;;
  vms-ds20) vms_board "$g" ds20 2 DS20/PC264SRM.ROM pci0.5 pci0.6 "" || continue ;;
  vms-ds10) vms_board "$g" ds10 1 DS10/DS10SRM.ROM pci0.7 pci0.13 pci0.1 || continue ;;
  vms-ds20l) vms_board "$g" ds20l 2 - pci0.7 pci0.15 pci0.19 || continue ;;
  linux)
    linux_setup || { fail "$SETUP_WHY"; continue; }
    steps=(); while IFS= read -r l; do steps+=("$l"); done < <(linux_login_steps)
    steps+=("send=uname -a; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@60"
           "send=echo '#include <stdio.h>' > /tmp/t.c; echo 'int main(){printf(\"gc-sum=%d\\n\",6*7);return 0;}' >> /tmp/t.c; gcc -O1 -o /tmp/t /tmp/t.c && /tmp/t; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@300")
    extra_steps linux
    # the date last, as on the ES47
    steps+=("send=date; echo GC-DONE-\$?" "expect=^GC-DONE-[0-9]@60"
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
  linux-es47)
    src=$WORK/linux
    rm -rf "$D"; mkdir -p "$D"
    cp -c "$src/disk/gentoo-es47.img" "$D/disk.img" ||
      { fail "no image $src/disk/gentoo-es47.img (test/tools/es47_linux.sh retarget)"; continue; }
    # the ES47's two EV7s, 512 MB each; the disk on the CMD 649 (dqa0, sda)
    {
      echo 'sys0 = tsunami'; echo '{'
      echo '  platform = "es47";'; echo '  memory.bits = 29;'
      echo "  rom.srm = \"$ROMS/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE\";"
      echo '  rom.nvram = "cmm_nvram.bin";'
      echo "  cpu0 = $EV7CPU { speed = 1000M;$EV7OPT }"; echo "  cpu1 = $EV7CPU { speed = 1000M;$EV7OPT }"
      echo '  pci2.2 = cmd649 { disk0.0 = file { file = "disk.img"; } }'
      echo "  serial0 = serial { address = \"127.0.0.1\"; port = $PORT; }"
      echo '}'
    } > "$D/es40.cfg"
    # No `set os_type unix` and `init`: aboot switches to the UNIX PALcode
    # itself, and the console keeps no environment here (docs/linux.md)
    steps=("expect=P00>>>@600" "send=boot dqa0 -flags 0" "expect=login: @$BTMO" "send=root"
           "expect=Password: @60" "send=alphabox" "expect=# \$@120"
           "send=uname -a; echo GC-DONE-\$?" "expect=GC-DONE-[0-9]@60"
           "send=grep -E 'cpus active|platform string' /proc/cpuinfo; echo GC-DONE-\$?" "expect=GC-DONE-[0-9]@60")
    extra_steps linux
    # the date last: it is compared with the host's clock after the
    # shutdown, and the extra commands may take minutes
    steps+=("send=date; echo GC-DONE-\$?" "expect=GC-DONE-[0-9]@60"
            "send=sync; poweroff" "expect=halted CPU|P00>>>@180")
    if serial_guest "$g" "$PORT" "${steps[@]}"; then
      # the date against the host's local time, as for linux above
      gt=$(grep -a -o "$(date '+%a %b %e') [0-9][0-9]:[0-9][0-9]:[0-9][0-9] [^ ]* $(date +%Y)" "$D/console.log" | head -1)
      gt=$(echo "$gt" | grep -o '[0-9][0-9]:[0-9][0-9]:[0-9][0-9]' | head -1)
      gm=$(( 10#${gt:0:2} * 60 + 10#${gt:3:2} )); hm=$(( 10#$(date +%H) * 60 + 10#$(date +%M) ))
      dm=$(( gm > hm ? gm - hm : hm - gm )); [ $dm -gt 720 ] && dm=$((1440 - dm))
      txt=$(tr -d '\r\000' < "$D/console.log")
      if echo "$txt" | grep -aq 'Linux es40 [0-9].* EV7 Marvel' &&
         echo "$txt" | grep -aqE "cpus active[[:space:]]*: $EV7N\$" &&
         echo "$txt" | grep -aq 'platform string.*AlphaServer ES47' &&
         [ -n "$gt" ] && [ $dm -le 5 ]; then
        stop_emu; report "$g" PASS "$(secs)" "login, uname -a (EV7 Marvel), $EV7N CPUs active, ES47, date (${gt:0:5}, host local time within ${dm} min), poweroff"
      else fail "logged in, but uname/cpuinfo/date output not as expected (date '$gt')"; continue; fi
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
