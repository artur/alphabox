#!/usr/bin/env python3
"""What idle processors of the native EV8 cost the host. The S12b console
(firmware/ev8) boots test/cpu/console8_kernel.c with `idle=N` on its
command line: the kernel starts every secondary, then every processor idles
as Linux's do with nothing to run (do_idle's arch_cpu_idle: wtint at IPL 7,
the interrupt taken at IPL 0) for N seconds of processor 0's ticks, between
the marks @@IDLE-BEGIN and @@IDLE-END. The emulator's CPU time (user +
system) is read at the two marks. With ALPHABOX_TRACE_QUIESCE set, each
processor's QUIESCE counts are printed at exit (they cover the whole run,
boot included). docs/platforms/ev8-native.md, "Idle TPUs".

    idle8.py <alphabox-binary> [label] [--secs N] [--platform es47]
             [--chips 2] [--tpus 4] [--port N] [--keep] [--nada PATH]

Prints the window's host seconds, the emulator's CPU seconds in it and
their ratio (host processors kept busy), the 1-minute load average at both
marks, and per processor the returns from wtint and the ticks per second
in the window, and its QUIESCE counts. A measurement, not a check: the
exit status is 0 when the run reached its marks.
Run output goes to $ALPHABOX_WORK/runs/<label> (kept on failure or --keep).
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import console8_test as c8  # noqa: E402
import cpu_oracle  # noqa: E402

QUIESCE = re.compile(
    r"CPU(\d+) LDx_ARM (\d+) .*?QUIESCE slept (\d+), ([\d.]+) ms in all, "
    r"woken by store (\d+), STx_C (\d+), DMA (\d+), interrupt (\d+), "
    r"timeout (\d+), other (\d+); did not sleep: flag clear (\d+), "
    r"unsettled (\d+)")
Q_FIELDS = ("arms", "sleeps", "slept_ms", "store", "stc", "dma", "interrupt",
            "timeout", "other", "no_flag", "unsettled")


def cpu_seconds(pid):
    """User + system seconds of process `pid` so far."""
    stat = "/proc/%d/stat" % pid
    if os.path.exists(stat):
        with open(stat) as f:
            fields = f.read().rsplit(")", 1)[1].split()
        return (int(fields[11]) + int(fields[12])) / os.sysconf("SC_CLK_TCK")
    out = subprocess.run(["ps", "-o", "time=", "-p", str(pid)],
                         capture_output=True, text=True).stdout.strip()
    days = 0
    if "-" in out:
        d, out = out.split("-", 1)
        days = int(d)
    secs = 0.0
    for part in out.split(":"):
        secs = secs * 60 + float(part)
    return days * 86400 + secs


def quiesce_counts(emu):
    q = {}
    for m in QUIESCE.finditer(emu):
        v = [float(x) if "." in x else int(x) for x in m.groups()[1:]]
        q[int(m.group(1))] = dict(zip(Q_FIELDS, v))
    return q


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="idle8")
    ap.add_argument("--secs", type=int, default=10)
    ap.add_argument("--platform", default="es47",
                    choices=("es47", "es80", "gs1280"))
    ap.add_argument("--chips", type=int, default=2)
    ap.add_argument("--tpus", type=int, default=4)
    ap.add_argument("--port", type=int,
                    default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--nada", default=None)
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(c8.REPO, "lab"))
    work = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    binary = os.path.abspath(args.binary)
    port = args.port or cpu_oracle.free_port()
    nada = c8.nada_path(args.nada)

    pal, _ = c8.fw.build_pal(args.nada)
    con = c8.fw.build_console(args.nada, c8.L)
    c8.fw.write_rom(os.path.join(work, "console8.rom"),
                    c8.fw.image(pal, c8.L, con, [c8.fw.boot_block(c8.L)]),
                    c8.L)
    c8.build_kernel(nada, os.path.join(work, "kernel.elf"))
    cmdline = "%s idle=%d" % (c8.CMDLINE, args.secs)
    keys = [("kernel", "kernel.elf"), ("cmdline", cmdline)]
    cfg = c8.config(args.platform, port, args.chips, args.tpus, keys=keys)
    version = subprocess.run([binary, "--version"], capture_output=True,
                             text=True).stdout.splitlines()
    nproc = args.chips * args.tpus
    print("== idle EV8 processors: %s, %d chips x %d TPUs (%d processors), "
          "%d s of ticks" % (args.platform, args.chips, args.tpus, nproc,
                             args.secs))
    print("  %s; %s" % (binary, "; ".join(v.strip() for v in version[:2])))

    steps = [(b"@@TYPE1", b"Q"), (b"@@TYPE2", b"Z"), (b"@@IDLE-BEGIN", None),
             (b"@@IDLE-END", None), (b"@@DONE", None), (b"P00>>>", None)]
    marks = {}

    def on_step(i, proc):
        if i in (2, 3):
            marks[i] = (time.monotonic(), cpu_seconds(proc.pid),
                        os.getloadavg()[0])

    os.environ["ALPHABOX_TRACE_QUIESCE"] = "1"
    text, emu, done, secs = c8.run(binary, work, cfg, port, steps,
                                   timeout=c8.TIMEOUT + 2 * args.secs,
                                   on_step=on_step)
    r = c8.results(text)
    ok = done == len(steps) and 2 in marks and 3 in marks
    if ok:
        wall = marks[3][0] - marks[2][0]
        cpu = marks[3][1] - marks[2][1]
        print("  window %.2f s, emulator CPU %.2f s: %.3f host processors "
              "busy, %.4f per emulated processor (load %.1f -> %.1f)"
              % (wall, cpu, cpu / wall, cpu / wall / nproc, marks[2][2],
                 marks[3][2]))
        q = quiesce_counts(emu)
        print("  %4s %8s %8s | QUIESCE, whole run: %7s %9s %6s %7s %7s "
              "%9s %7s" % ("cpu", "wtint/s", "ticks/s", "sleeps", "slept ms",
                           "int", "timeout", "other", "unsettled", "noflag"))
        for w in range(nproc):
            name = "idle.%d." % w
            if name + "wtint" not in r:
                continue
            qq = q.get(w, {})
            print("  %4d %8.1f %8.1f | %24d %9.1f %6d %7d %7d %9d %7d"
                  % (w, r[name + "wtint"] / wall, r[name + "ticks"] / wall,
                     qq.get("sleeps", -1), qq.get("slept_ms", -1),
                     qq.get("interrupt", -1), qq.get("timeout", -1),
                     qq.get("other", -1), qq.get("unsettled", -1),
                     qq.get("no_flag", -1)))
        halts = [r.get("idle.%d.halt_reason" % w) for w in range(1, nproc)]
        if any(h != 5 for h in halts):
            print("  secondaries' halt reasons: %s" % halts)
    else:
        print("  FAILED: %d of %d steps, marks %s" % (done, len(steps),
                                                      sorted(marks)))
    print("  run %.1f s" % secs)
    if ok and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("  run directory: %s" % work)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
