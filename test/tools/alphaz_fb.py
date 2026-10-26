#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# alphaz_fb.py: the frame checks of alphaz_guest.sh, on the PPM dumps
# ALPHABOX_DUMP_FB writes (one every ~2 s).
#
#   window DIR TMO PID      wait for a frame with AlphaZ's window up (its
#                           band, RGB 0,128,128, one of Windows' twenty
#                           static colours, so solid at any depth); print it
#   change DIR REF TMO PID  wait for a frame newer than REF whose AlphaZ
#                           window differs from REF's, then for the next to
#                           agree with it (the page has finished drawing);
#                           print it, or "same" when nothing changed
#   gone DIR TMO PID        wait for a frame without the window
#   band FILE               the band's box and its text (tesseract, if it is
#                           installed): what tab the frame shows
#   check REPORT EXPECT...  compare report values with the configuration's:
#                           EXPECT items are key=value (see alphaz_guest.sh)
# Copyright (C) 2026 Artur Goulão
import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

TEAL = b"\x00\x80\x80"


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    w, h = int(m.group(1)), int(m.group(2))
    return w, h, data[m.end():m.end() + w * h * 3]


def frames(d):
    f = glob.glob(os.path.join(d, "*.ppm"))
    f.sort(key=lambda p: (os.path.getmtime(p), p))
    return f


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def band_box(path):
    """The teal band's box (x0, y0, x1, y1) or None: the rows that are
    teal for most of a 300-pixel run."""
    try:
        w, h, b = read_ppm(path)
    except (OSError, AttributeError):
        return None
    rows = []
    for y in range(h):
        line = b[y * w * 3:(y + 1) * w * 3]
        n = sum(1 for x in range(0, w, 2) if line[3 * x:3 * x + 3] == TEAL)   # every other pixel
        if n * 2 >= 300:
            rows.append(y)
    if len(rows) < 12:
        return None
    y0, y1 = rows[0], rows[-1] + 1
    line = b[y0 * w * 3:(y0 + 1) * w * 3]
    xs = [x for x in range(w) if line[3 * x:3 * x + 3] == TEAL]
    return xs[0], y0, xs[-1] + 1, y1


def window_box(path):
    """The part of the screen AlphaZ draws: from the band's top less the
    tab row to some 380 pixels below, or None."""
    bb = band_box(path)
    if not bb:
        return None
    x0, y0, x1, y1 = bb
    return max(0, x0 - 8), max(0, y0 - 30), x1 + 8, y0 + 380


def differs(a, b, box):
    wa, ha, da = read_ppm(a)
    wb, hb, db = read_ppm(b)
    if (wa, ha) != (wb, hb):
        return True
    x0, y0, x1, y1 = box
    y1 = min(y1, ha)
    x1 = min(x1, wa)
    n = 0
    for y in range(y0, y1):
        o0, o1 = (y * wa + x0) * 3, (y * wa + x1) * 3
        if da[o0:o1] != db[o0:o1]:
            n += 1
    return n > 2


def cmd_window(d, tmo, pid):
    end = time.time() + tmo
    while time.time() < end and alive(pid):
        f = frames(d)
        if f and band_box(f[-1]):
            print(f[-1])
            return 0
        time.sleep(2)
    print("none")
    return 1


def cmd_change(d, ref, tmo, pid):
    box = window_box(ref) or (0, 0, 10000, 10000)
    t_ref = os.path.getmtime(ref)
    end = time.time() + tmo
    while time.time() < end and alive(pid):
        new = [p for p in frames(d) if os.path.getmtime(p) > t_ref]
        if new and differs(ref, new[-1], box):
            cand = new[-1]
            # the next frame must agree: the page has finished drawing
            t_c = os.path.getmtime(cand)
            end2 = time.time() + 12
            while time.time() < end2 and alive(pid):
                newer = [p for p in frames(d) if os.path.getmtime(p) > t_c]
                if newer:
                    if not differs(cand, newer[-1], box):
                        print(newer[-1])
                        return 0
                    cand, t_c = newer[-1], os.path.getmtime(newer[-1])
                time.sleep(1)
            print(cand)
            return 0
        time.sleep(1)
    print("same")
    return 1


def cmd_gone(d, tmo, pid):
    end = time.time() + tmo
    while time.time() < end and alive(pid):
        f = frames(d)
        if f and not band_box(f[-1]):
            print(f[-1])
            return 0
        time.sleep(2)
    print("none")
    return 1


def cmd_band(path):
    bb = band_box(path)
    if not bb:
        print("no band")
        return 1
    text = ""
    if shutil.which("tesseract"):
        w, h, b = read_ppm(path)
        x0, y0, x1, y1 = bb
        scale = 4
        rows = []
        for y in range(y0, y1):
            r = bytearray()
            for x in range(x0, x1):
                o = (y * w + x) * 3
                r += bytes([255 - (b[o] * 30 + b[o + 1] * 59 + b[o + 2] * 11) // 100]) * scale
            rows += [bytes(r)] * scale
        fd, pg = tempfile.mkstemp(suffix=".pgm")
        with os.fdopen(fd, "wb") as f:
            f.write(b"P5\n%d %d\n255\n" % ((x1 - x0) * scale, len(rows)))
            f.write(b"".join(rows))
        try:
            r = subprocess.run(["tesseract", pg, "-", "--psm", "7"], capture_output=True, text=True, timeout=60)
            text = r.stdout.strip()
        finally:
            os.unlink(pg)
    print("band %d,%d-%d,%d: %s" % (bb + (text,)))
    return 0


def value(report, label):
    """The value of the first row with this label, or None."""
    for line in report:
        if line.startswith(label + " ") and len(line) > 29:
            v = line[29:]
            v = re.sub(r"  \[[^\]]*\]$", "", v)
            return v.strip()
    return None


def cmd_check(path, items):
    """Each item: key=value. Keys: implver, amask, cpus, speed (Hz, the
    `speed` key), mem_kb (memory.bits as KB), ev8 (cases), os (windows or
    linux)."""
    rep = [l.rstrip("\r\n") for l in open(path, encoding="latin-1")]
    exp = dict(i.split("=", 1) for i in items)
    out, bad = [], 0

    def line(ok, what, guest, config):
        nonlocal bad
        bad += not ok
        out.append("%-4s %-24s guest: %-40s config: %s" % ("ok" if ok else "DIFF", what, guest, config))

    v = value(rep, "IMPLVER")
    line(v == exp["implver"], "IMPLVER", v, exp["implver"])
    v = value(rep, "AMASK, features present")
    line(v is not None and int(v, 16) == int(exp["amask"], 16), "AMASK features present", v,
         "%s (the row's AMASK in CpuModels.cpp)" % exp["amask"])
    clk = value(rep, "Clock")
    m = re.match(r"([0-9.]+) MHz", clk or "")
    want = int(exp["speed"]) / 1e6
    ok = bool(m) and abs(float(m.group(1)) - want) <= want * 0.02
    line(ok, "Clock (RPCC)", clk, "speed = %gM (within 2%%)" % want)
    if exp["os"] == "windows":
        v = value(rep, "Processors")
        n = int(v.split()[0]) if v else -1
        line(n == int(exp["cpus"]), "Processors (GetSystemInfo)", v, "%s cpu rows" % exp["cpus"])
        v = value(rep, "Physical memory")
        kb = int(v.split()[0]) if v else -1
        mem = int(exp["mem_kb"])
        line(mem * 0.7 <= kb <= mem, "Physical memory", v, "memory.bits: %d KB (Windows keeps some)" % mem)
    else:
        v = value(rep, "cpus active")
        line(v == exp["cpus"], "cpus active (/proc)", v, "%s processors" % exp["cpus"])
        v = value(rep, "MemTotal")
        kb = int(v.split()[0]) if v else -1
        mem = int(exp["mem_kb"])
        line(mem * 0.7 <= kb <= mem, "MemTotal (/proc)", v, "memory.bits: %d KB (the kernel keeps some)" % mem)
        v = value(rep, "cpu model")
        if "model" in exp:
            line(v == exp["model"], "cpu model (/proc)", v, exp["model"])
    v = value(rep, "Every processor")
    if v:
        m = re.match(r"(\d+) of (\d+): IMPLVER (\d+), AMASK features (0x[0-9a-f]+)", v)
        ok = bool(m) and m.group(3) == exp["implver"] and int(m.group(4), 16) == int(exp["amask"], 16) \
            and m.group(1) == exp["cpus"]
        line(ok, "Every processor", v, "%s processors, each IMPLVER %s" % (exp["cpus"], exp["implver"]))
    if exp.get("ev8"):
        v = value(rep, "Self-check")
        line(v is not None and v.startswith("%s of %s agree" % (exp["ev8"], exp["ev8"])), "EV8 self-check", v,
             "every case agrees")
    print("\n".join(out))
    return 1 if bad else 0


def cmd_row(src, name):
    """IMPLVER and AMASK of the processor row `name`, read from
    src/cpu/CpuModels.cpp and the AMASK_ values of CpuModel.hpp."""
    hpp = open(os.path.join(src, "cpu", "CpuModel.hpp")).read()
    bits = {m.group(1): int(m.group(2), 16) for m in re.finditer(r"#define (AMASK_\w+) U64\((0x[0-9A-Fa-f]+)\)", hpp)}
    cpp = open(os.path.join(src, "cpu", "CpuModels.cpp")).read()
    m = re.search(r'\{"%s",\s*"[^"]*",\s*(\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*([A-Z0-9_|\s]+),\s*CPU_FAMILY' % re.escape(name), cpp)
    if not m:
        print("none")
        return 1
    amask = 0
    for f in re.findall(r"AMASK_\w+", m.group(5)):
        amask |= bits[f]
    print("%s 0x%x %s %s" % (m.group(4), amask, m.group(2), m.group(3)))
    return 0


def main():
    a = sys.argv[1:]
    if a[0] == "row":
        return cmd_row(a[1], a[2])
    if a[0] == "window":
        return cmd_window(a[1], float(a[2]), int(a[3]))
    if a[0] == "change":
        return cmd_change(a[1], a[2], float(a[3]), int(a[4]))
    if a[0] == "gone":
        return cmd_gone(a[1], float(a[2]), int(a[3]))
    if a[0] == "band":
        return cmd_band(a[1])
    if a[0] == "check":
        return cmd_check(a[1], a[2:])
    return 2


sys.exit(main())
