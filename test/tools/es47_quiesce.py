#!/usr/bin/env python3
"""Linux on the ES47 with two `ev8` processors: a user program that waits
on a lock with LDQ_ARM and QUIESCE (docs/platforms/ev8.md, S3).

    es47_quiesce.py <alphabox-binary> <label> [--port N] [--cpu ROW]
                    [--args "N HOLD"]

A clone (cp -c) of the installed disk ($ALPHABOX_WORK/linux/disk/
gentoo-es47.img, test/tools/es47_linux.sh retarget) gets
test/cpu/quiesce_lock.c in /root (debugfs, e2fsprogs); the ES47 boots it
from the CMD 649 (the linux-es47 machine of test/tools/guests_check.sh,
with the row given), logs in as root, prints /proc/cpuinfo's processor
lines, builds the program with the guest's gcc, runs it, and powers off.
The emulator runs with ALPHABOX_TRACE_QUIESCE=1, so each processor's
LDx_ARM count and what woke its QUIESCEs are in its output at the end.

Output in $ALPHABOX_WORK/runs/<label>/: console.log (the serial session),
alphabox.out (the emulator's), summary.txt. The disk clone is deleted.
Exit status 0 when the program printed "QTEST PASS", poweroff halted, and
a processor was woken by a store.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SRC = os.path.join(REPO, "test", "cpu", "quiesce_lock.c")
P2_OFFSET = 1048576 + 128 * 1048576   # the root partition (es47_linux.sh)


def debugfs():
    for d in ("/opt/homebrew/opt/e2fsprogs/sbin", "/usr/local/opt/e2fsprogs/sbin",
              "/sbin", "/usr/sbin"):
        if os.access(os.path.join(d, "debugfs"), os.X_OK):
            return os.path.join(d, "debugfs")
    raise SystemExit("es47_quiesce: no debugfs (e2fsprogs)")


class Chat:
    def __init__(self, port, log):
        self.log = open(log, "wb")
        self.buf = ""
        end = time.time() + 60
        self.s = None
        while time.time() < end and self.s is None:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), timeout=2)
            except OSError:
                time.sleep(0.5)
        if self.s is None:
            raise SystemExit("es47_quiesce: no console on port %d" % port)
        self.s.settimeout(0.5)

    def rd(self):
        try:
            d = self.s.recv(65536)
        except socket.timeout:
            return
        if not d:
            raise EOFError
        d = re.sub(rb"\xff[\xfb-\xfe].", b"", d).replace(b"\x00", b"")
        self.log.write(d)
        self.log.flush()
        self.buf += d.decode("latin1").replace("\r", "")

    def send(self, text):
        b = text.encode() + b"\r"
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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "23790")))
    ap.add_argument("--cpu", default="ev8")
    ap.add_argument("--args", default="20000 200")
    args = ap.parse_args()
    work = os.path.abspath(os.environ.get("ALPHABOX_WORK", os.path.join(REPO, "lab")))
    roms = os.path.abspath(os.environ.get("ALPHABOX_ROMS", os.path.join(REPO, "roms")))
    disk = os.path.join(work, "linux", "disk", "gentoo-es47.img")
    d = os.path.join(work, "runs", args.label)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    img = os.path.join(d, "disk.img")
    subprocess.check_call(["cp", "-c", disk, img])
    part = "%s?offset=%d" % (img, P2_OFFSET)
    dbg = debugfs()
    for cmd in ("write %s /root/quiesce_lock.c" % SRC,
                "sif /root/quiesce_lock.c uid 0", "sif /root/quiesce_lock.c gid 0"):
        subprocess.check_call([dbg, "-w", "-R", cmd, part],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    with open(os.path.join(d, "es40.cfg"), "w") as f:
        f.write("""sys0 = tsunami
{
  platform = "es47";
  memory.bits = 29;
  rom.srm = "%s/alpha-firmware-v7.3/GS1280/SRM_V7_3.EXE";
  rom.nvram = "cmm_nvram.bin";
  cpu0 = %s { speed = 1000M; }
  cpu1 = %s { speed = 1000M; }
  pci2.2 = cmd649 { disk0.0 = file { file = "disk.img"; } }
  serial0 = serial { address = "127.0.0.1"; port = %d; }
}
""" % (roms, args.cpu, args.cpu, args.port))
    env = dict(os.environ, ALPHABOX_TRACE_QUIESCE="1", SDL_VIDEO_DRIVER="dummy")
    out = open(os.path.join(d, "alphabox.out"), "wb")
    t0 = time.time()
    proc = subprocess.Popen([os.path.abspath(args.binary), "run", "es40.cfg"],
                            cwd=d, stdout=out, stderr=subprocess.STDOUT, env=env)
    result, steps = "FAIL", []
    try:
        c = Chat(args.port, os.path.join(d, "console.log"))

        def step(send, rx, tmo, what):
            if send is not None:
                c.send(send)
            m = c.expect(rx, tmo)
            steps.append("%s: %s" % (what, "ok" if m else "TIMEOUT"))
            if not m:
                raise RuntimeError(what)
            return m

        step(None, r"P00>>>", 600, "console prompt")
        step("boot dqa0 -flags 0", r"login: ", 900, "login prompt")
        step("root", r"Password: ", 60, "password prompt")
        step("alphabox", r"# $", 120, "shell")
        step("grep -E 'cpu model|cpus active|platform string' /proc/cpuinfo; "
             "echo GC-DONE-$?", r"GC-DONE-[0-9]", 60, "cpuinfo")
        step("gcc -O2 -pthread -o /root/quiesce_lock /root/quiesce_lock.c; "
             "echo GC-DONE-$?", r"GC-DONE-0", 300, "gcc")
        step("/root/quiesce_lock %s; echo GC-DONE-$?" % args.args,
             r"GC-DONE-[0-9]", 900, "run")
        step("sync; poweroff", r"halted CPU|P00>>>", 180, "poweroff")
        result = "PASS"
    except (RuntimeError, EOFError) as e:
        steps.append("stopped at %s" % e)
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)    # our own emulator only
            try:
                proc.wait(timeout=60)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        out.close()
        os.remove(img)
    secs = time.time() - t0
    con = open(os.path.join(d, "console.log"), "rb").read().decode("latin1")
    emu = open(os.path.join(d, "alphabox.out"), "rb").read().decode("latin1")
    lines = [ln.strip() for ln in con.replace("\r", "").splitlines()
             if re.search(r"AMASK\(|^count |QTEST|cpu model|cpus active|"
                          r"platform string", ln)]
    rep = [ln.strip() for ln in emu.splitlines() if "%CPU-I-QUIESCE" in ln]
    stores = sum(int(m.group(1)) for ln in rep
                 for m in [re.search(r"woken by store (\d+)", ln)] if m)
    if result == "PASS" and not ("QTEST PASS" in con and stores > 0):
        result = "FAIL"
    summary = ["== es47_quiesce %s: %s, two %s, %.0f s" % (
        args.label, result, args.cpu, secs)] + ["  " + s for s in steps] + \
        ["  guest: " + ln for ln in lines] + ["  emulator: " + ln for ln in rep]
    open(os.path.join(d, "summary.txt"), "w").write("\n".join(summary) + "\n")
    print("\n".join(summary))
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
