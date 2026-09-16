#!/usr/bin/env python3
"""Run a command inside a booted Windows 2000 guest and time it.

Called by win_workload.sh while the emulator runs with AXPBOX_DUMP_FB (frames
every ~2 s) and AXPBOX_KEYPIPE. Steps:
  1. wait for the desktop: a light-gray taskbar row near the bottom and a
     mostly blue frame, seen in several consecutive dumps;
  2. wait --settle seconds (startup programs finish);
  3. per repeat: open the Run dialog (win-r, retried, then Ctrl+Esc and R),
     clear the box and type the command, e.g.
     "cmd /c for /l %i in (1,1,25000000) do @rem". The box completes what
     we type from its history and leaves the completion SELECTED, so the
     command is followed by Delete before Enter -- without it a repeat once
     ran "do @remnd", which prints 25M errors and never closes its window;
  4. time its console window: first dump with a large pure-black area ->
     first dump without it, from the dump files' modification times (about
     2 s resolution, so keep each run 100 s or more).

Repeating inside one boot (--repeats) removes boot-to-boot variance, which
is the dominant noise when comparing two binaries; compare the medians.

Prints status lines, "workload_seconds=<n>" per repeat, then
"workload_runs=<a,b,c>" and "workload_median=<n>". Exit status 0 when at
least one repeat was measured.
"""
import argparse
import os
import statistics
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


def differs(a, b):
    """True when two frames differ in more than 1% of sampled pixels."""
    w0, h0, p0 = a
    w1, h1, p1 = b
    if (w0, h0) != (w1, h1):
        return True
    diff = n = 0
    for y in range(0, h1, 4):
        row = y * w1 * 3
        for x in range(0, w1, 4):
            i = row + x * 3
            n += 1
            if abs(p0[i] - p1[i]) > 24 or abs(p0[i + 2] - p1[i + 2]) > 24:
                diff += 1
    return bool(n) and diff / n > 0.01


class Guest:
    def __init__(self, args):
        self.args = args
        self.seen = set()

    def new_frames(self):
        return frames(self.args.fb, self.seen)

    def latest(self, timeout=30):
        """The most recent complete frame, waiting for one if needed."""
        last = None
        end = time.time() + timeout
        while time.time() < end:
            for _, _, fr in self.new_frames():
                last = fr
            if last is not None:
                return last
            time.sleep(1)
        return None

    def changed_frame(self, base, timeout):
        """A new frame differing from base, or None."""
        end = time.time() + timeout
        while time.time() < end:
            for _, _, fr in self.new_frames():
                if differs(base, fr):
                    return fr
                base = fr
            time.sleep(1)
        return None

    def send(self, tokens):
        with open(self.args.keypipe, "a") as kp:
            kp.write(" ".join(tokens) + "\n")

    def wait_desktop(self):
        streak = 0
        t0 = time.time()
        while True:
            if not alive(self.args.pid):
                return False
            if time.time() - t0 > self.args.boot_timeout:
                print("status=no-desktop")
                return False
            for _, _, fr in self.new_frames():
                desk, blk = classify(fr)
                streak = streak + 1 if (desk and blk < 0.05) else 0
            if streak >= 3:
                print("status=desktop after %.0fs" % (time.time() - t0))
                return True
            time.sleep(1)

    def open_run_dialog(self):
        """Never type without seeing the dialog: keys sent to the desktop
        select icons by first letter and Enter launches one."""
        for attempt, keys in enumerate((["win-r"], ["win-r"], ["ctrl-esc"],
                                        ["ctrl-esc"])):
            base = self.latest()
            if base is None:
                return False
            self.send(keys)
            got = self.changed_frame(base, 12)
            if got is None:
                print("status=no-dialog (attempt %d: %s)"
                      % (attempt + 1, " ".join(keys)))
                continue
            if keys == ["ctrl-esc"]:  # Start menu open: R = Run...
                self.send(["r"])
                if self.changed_frame(got, 12) is None:
                    print("status=no-dialog (attempt %d: start-menu r)"
                          % (attempt + 1))
                    self.send(["esc", "esc"])
                    continue
            return True
        print("status=no-run-dialog")
        return False

    def recover_console(self, timeout=120):
        """Close a console still running after a timeout, so the remaining
        repeats can go ahead. True when the desktop is back."""
        for attempt in range(3):
            self.send(["ctrl-c"])
            end = time.time() + timeout / 3
            while time.time() < end:
                if not alive(self.args.pid):
                    return False
                for _, _, fr in self.new_frames():
                    desk, blk = classify(fr)
                    if desk and blk < 0.05:
                        print("status=console-recovered (attempt %d)"
                              % (attempt + 1))
                        return True
                time.sleep(1)
        print("status=console-stuck")
        return False

    def time_command(self, tokens):
        """Type the command and time its console window. Seconds, or None."""
        # Clear from a known caret position: the box may open with its
        # remembered text already selected.
        self.send(["end", "shift-home", "del"])
        time.sleep(1)
        self.send(tokens)
        time.sleep(1)
        # The box completes from history and leaves the completion selected;
        # Enter would commit that too. Delete drops a selection, and is a
        # no-op when the caret is at the end with nothing after it.
        self.send(["del"])
        self.send(["enter"])
        typed_at = time.time()
        for _, _, _ in self.new_frames():
            pass  # ignore frames from before the command started
        opened = None
        while True:
            if not alive(self.args.pid):
                print("status=emulator-exited")
                return None
            if time.time() - typed_at > self.args.run_timeout:
                print("status=timeout (console %s)"
                      % ("open" if opened else "never opened"))
                return None
            for name, mtime, fr in self.new_frames():
                _, blk = classify(fr)
                if opened is None and blk >= self.args.black:
                    opened = mtime
                    print("status=console-open (%s, black %.0f%%)"
                          % (name, blk * 100))
                elif opened is not None and blk < self.args.black / 3:
                    print("status=console-closed (%s)" % name)
                    return mtime - opened
            time.sleep(1)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fb", required=True, help="frame dump directory")
    ap.add_argument("--keypipe", required=True)
    ap.add_argument("--command", required=True)
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--boot-timeout", type=int, default=400)
    ap.add_argument("--settle", type=int, default=60)
    ap.add_argument("--run-timeout", type=int, default=900)
    ap.add_argument("--repeats", type=int, default=1,
                    help="times to run the command in this boot "
                         "(compare medians)")
    ap.add_argument("--between", type=int, default=10,
                    help="seconds to quiesce between repeats")
    ap.add_argument("--black", type=float, default=0.25,
                    help="black fraction that means the console is open")
    args = ap.parse_args()

    toks = keys_for.tokens(args.command)  # time_command adds del + enter
    g = Guest(args)

    if not g.wait_desktop():
        return 1
    time.sleep(args.settle)

    runs = []
    for i in range(args.repeats):
        if i:
            time.sleep(args.between)
        if not g.open_run_dialog():
            break
        secs = g.time_command(toks)
        if secs is None:
            # One bad repeat must not lose the repeats that would follow.
            if i + 1 < args.repeats and g.recover_console():
                continue
            break
        runs.append(secs)
        print("workload_seconds=%.1f (run %d/%d)" % (secs, i + 1, args.repeats))

    if not runs:
        return 1
    print("workload_runs=%s" % ",".join("%.1f" % s for s in runs))
    print("workload_median=%.1f" % statistics.median(runs))
    return 0


if __name__ == "__main__":
    sys.exit(main())
