#!/usr/bin/env python3
"""Run a command inside a booted Windows 2000 guest and time it.

Called by win_workload.sh while the emulator runs with AXPBOX_DUMP_FB (frames
every ~2 s) and AXPBOX_KEYPIPE. Steps:
  1. wait for the desktop: a light-gray taskbar row near the bottom and a
     mostly blue frame, seen in several consecutive dumps;
  2. wait --settle seconds (startup programs finish);
  3. open the Run dialog (win-r) and type the command (keys_for.py tokens),
     which should open a console window, e.g. "cmd /c for /l %i in (...) do @rem";
  4. time the console: first dump with a large pure-black area -> first dump
     without it, using the dump files' modification times (about 2 s
     resolution; make the command run long, e.g. 100 s or more).

Prints status lines (status=desktop, status=typed, status=console-open,
status=console-closed) and "workload_seconds=<n>". Exit status 0 when a
duration was measured.
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import keys_for  # noqa: E402


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(b"\n", 3)
    if len(parts) < 4 or parts[0] != b"P6":
        return None
    w, h = map(int, parts[1].split())
    px = parts[3]
    if len(px) < w * h * 3:
        return None  # still being written
    return w, h, px


def classify(frame):
    """(is_desktop, black_fraction) for one frame."""
    w, h, px = frame
    step = 4  # sample every 4th pixel
    black = blue = total = 0
    for y in range(0, h, 2):
        row = y * w * 3
        for x in range(0, w, step):
            i = row + x * 3
            r, g, b = px[i], px[i + 1], px[i + 2]
            total += 1
            if r < 16 and g < 16 and b < 16:
                black += 1
            elif b > 140 and r < 110:
                blue += 1
    y = h - 10  # taskbar row
    gray = n = 0
    row = y * w * 3
    for x in range(0, w, 2):
        i = row + x * 3
        r, g, b = px[i], px[i + 1], px[i + 2]
        n += 1
        if 180 <= r <= 235 and 180 <= g <= 235 and 170 <= b <= 235:
            gray += 1
    desktop = n and gray / n > 0.5 and blue / total > 0.3
    return bool(desktop), black / total


def frames(fb_dir, seen):
    """New complete frames in sequence order: (name, mtime, frame)."""
    names = sorted(f for f in os.listdir(fb_dir) if f.endswith(".ppm"))
    out = []
    for name in names:
        if name in seen:
            continue
        path = os.path.join(fb_dir, name)
        fr = read_ppm(path)
        if fr is None:
            break  # incomplete: retry on the next poll
        seen.add(name)
        out.append((name, os.path.getmtime(path), fr))
    return out


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fb", required=True, help="frame dump directory")
    ap.add_argument("--keypipe", required=True)
    ap.add_argument("--command", required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--boot-timeout", type=int, default=400)
    ap.add_argument("--settle", type=int, default=60)
    ap.add_argument("--run-timeout", type=int, default=900)
    ap.add_argument("--black", type=float, default=0.25,
                    help="black fraction that means the console is open")
    args = ap.parse_args()

    toks = keys_for.tokens(args.command) + ["enter"]
    seen = set()
    t_start = time.time()

    # 1. desktop
    streak = 0
    while True:
        if not alive(args.pid):
            print("status=emulator-exited")
            return 1
        if time.time() - t_start > args.boot_timeout:
            print("status=no-desktop")
            return 1
        for name, _, fr in frames(args.fb, seen):
            desk, blk = classify(fr)
            streak = streak + 1 if (desk and blk < 0.05) else 0
        if streak >= 3:
            break
        time.sleep(1)
    print("status=desktop after %.0fs" % (time.time() - t_start))

    # 2. settle
    time.sleep(args.settle)

    # 3. open the Run dialog and type. Never type without seeing the dialog:
    # keys sent to the desktop select icons by first letter and Enter launches
    # one. Each attempt needs a frame that differs from the pre-keystroke one.
    def send(tokens):
        with open(args.keypipe, "a") as kp:
            kp.write(" ".join(tokens) + "\n")

    def changed_frame(base, timeout):
        """A new frame differing from base in >1% of sampled pixels."""
        end = time.time() + timeout
        while time.time() < end:
            for _, _, fr in frames(args.fb, seen):
                w0, h0, p0 = base
                w1, h1, p1 = fr
                if (w0, h0) != (w1, h1):
                    return fr
                diff = n = 0
                for y in range(0, h1, 4):
                    row = y * w1 * 3
                    for x in range(0, w1, 4):
                        i = row + x * 3
                        n += 1
                        if abs(p0[i] - p1[i]) > 24 or abs(p0[i + 2] - p1[i + 2]) > 24:
                            diff += 1
                if n and diff / n > 0.01:
                    return fr
                base = fr
            time.sleep(1)
        return None

    opened = False
    for attempt, keys in enumerate((["win-r"], ["win-r"], ["ctrl-esc"], ["ctrl-esc"])):
        base = None
        for _, _, fr in frames(args.fb, seen):
            base = fr
        while base is None:
            time.sleep(1)
            for _, _, fr in frames(args.fb, seen):
                base = fr
        send(keys)
        got = changed_frame(base, 12)
        if got is None:
            print("status=no-dialog (attempt %d: %s)" % (attempt + 1, " ".join(keys)))
            continue
        if keys == ["ctrl-esc"]:  # Start menu open: R = Run...
            send(["r"])
            if changed_frame(got, 12) is None:
                print("status=no-dialog (attempt %d: start-menu r)" % (attempt + 1))
                send(["esc", "esc"])
                continue
        opened = True
        break
    if not opened:
        print("status=no-run-dialog")
        return 1

    # The Run box remembers earlier commands and autocompletes: clear it first.
    send(["shift-home", "del"])
    time.sleep(1)
    send(toks)
    typed_at = time.time()
    print("status=typed %d tokens" % len(toks))

    # 4. console open -> closed
    seen.update(f for f in os.listdir(args.fb) if f.endswith(".ppm"))
    opened = None
    while True:
        if not alive(args.pid):
            print("status=emulator-exited")
            return 1
        if time.time() - typed_at > args.run_timeout:
            print("status=timeout (console %s)" % ("open" if opened else "never opened"))
            return 1
        for name, mtime, fr in frames(args.fb, seen):
            _, blk = classify(fr)
            if opened is None and blk >= args.black:
                opened = mtime
                print("status=console-open (%s, black %.0f%%)" % (name, blk * 100))
            elif opened is not None and blk < args.black / 3:
                print("status=console-closed (%s)" % name)
                print("workload_seconds=%.1f" % (mtime - opened))
                return 0
        time.sleep(1)


if __name__ == "__main__":
    sys.exit(main())
