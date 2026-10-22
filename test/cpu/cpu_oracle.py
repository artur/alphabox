#!/usr/bin/env python3
"""Instruction conformance test of the Alpha CPU against the Architecture
Reference Manual (the oracle of docs/development.md, "Test tools").

    cpu_oracle.py <alphabox-binary> [label] [--port N] [--passes N]
                  [--group G ...] [--verbose] [--keep]

It builds an image of test cases (cases.py) whose expected results come
from aarm.py, a model written from the manual, boots it on the emulator in
place of the console firmware (image.py), reads every result back over
COM1, and compares. No firmware is needed and nothing else is booted: a run
is about a second.

Every case runs `passes` times (default 20). The first pass is stored
apart from the others: on a JIT lane it is interpreted, and the last pass
runs compiled once a block has been dispatched more than the JIT's compile
threshold (16), so one run checks both engines against the manual. Every
case starts a block of its own (image.Builder.block) so that it does.
On an interpreter lane the two simply agree.

Each result is PASS (the manual's value), KNOWN (the value of a divergence
documented in docs/cpu-fidelity.md, which the case names; a CPU-nn or JIT-nn
name is a finding of the CPU or the JIT audit, listed there with it) or
FAIL, and so is a trap
that no case expected. Exit status 0 when nothing FAILed. A KNOWN case that
passes on both engines is listed, so its entry can go with the fix.
"""
import argparse
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import cases  # noqa: E402
import image  # noqa: E402

CFG = """sys0 = tsunami
{
  memory.bits = 26;
  rom.srm = "absent.exe";
  rom.decompressed = "oracle.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  exit_on_pal_halt = true;

  cpu0 = ev68cb
  {
    speed = 800M;
  }

  serial0 = serial
  {
    address = "127.0.0.1";
    port = %d;
  }

  pci0.7 = ali
  {
  }
}
"""


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def run(binary, workdir, port, env_extra, timeout):
    """Boot the image; return (serial text, emulator output, seconds)."""
    with open(os.path.join(workdir, "es40.cfg"), "w") as f:
        f.write(CFG % port)
    env = dict(os.environ)
    env.update(env_extra)
    out = open(os.path.join(workdir, "alphabox.out"), "wb")
    t0 = time.time()
    proc = subprocess.Popen([binary, "run"], cwd=workdir, stdout=out,
                            stderr=subprocess.STDOUT, env=env)
    data = bytearray()
    try:
        sock = None
        deadline = t0 + timeout
        while sock is None and time.time() < deadline:
            if proc.poll() is not None:
                break
            try:
                sock = socket.create_connection(("127.0.0.1", port), timeout=1)
            except OSError:
                time.sleep(0.05)
        if sock is not None:
            sock.settimeout(max(1.0, deadline - time.time()))
            try:
                while b"@@END" not in data and b"@@FATAL" not in data:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                if b"@@FATAL" in data:   # the trap record follows: 7 lines
                    end = time.time() + 2
                    while data.count(b"\n", data.index(b"@@FATAL")) < 9 and time.time() < end:
                        chunk = sock.recv(65536)
                        if not chunk:
                            break
                        data += chunk
            except socket.timeout:
                pass
            sock.close()
        try:
            proc.wait(timeout=max(5.0, deadline - time.time()))
        except subprocess.TimeoutExpired:
            proc.send_signal(signal.SIGTERM)   # our own emulator only
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    finally:
        out.close()
        if proc.poll() is None:
            proc.kill()
            proc.wait()
    secs = time.time() - t0
    with open(os.path.join(workdir, "alphabox.out"), "rb") as f:
        emu = f.read().decode("latin-1")
    return data.decode("latin-1"), emu, secs


def parse(text):
    """The two result buffers printed after @@BEGIN."""
    i = text.find("@@BEGIN")
    if i < 0:
        return None, None
    lines = text[i:].replace("\r", "").split("\n")
    n = int(lines[0].split()[1])
    bufs, cur = [], []
    for ln in lines[1:]:
        ln = ln.strip()
        if ln == "@@" or ln == "@@END":
            bufs.append(cur)
            cur = []
            if ln == "@@END" or len(bufs) == 2:
                break
            continue
        if ln:
            cur.append(int(ln, 16))
    if len(bufs) != 2 or any(len(b) != n for b in bufs):
        return None, None
    return bufs[0], bufs[1]


def verdict(res, got, tag):
    case, fld, expect, mask, known = res
    v = got & mask
    if v == expect:
        return "PASS"
    if (known is not None and v == known[0] and
            (known[2] == "all" or tag == "last pass")):
        return "KNOWN"
    return "FAIL"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="cpu")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")),
                    help="COM1 telnet port (default: $PORT, else a free one)")
    ap.add_argument("--passes", type=int, default=20)
    ap.add_argument("--group", action="append", default=[],
                    help="only these case groups (repeatable); see --list")
    ap.add_argument("--list", action="store_true", help="list the groups and exit")
    ap.add_argument("--verbose", action="store_true",
                    help="list every KNOWN result, not only the summary")
    ap.add_argument("--keep", action="store_true", help="keep the run directory")
    ap.add_argument("--timeout", type=float, default=120)
    ap.add_argument("--env", action="append", default=[],
                    help="KEY=VALUE for the emulator (repeatable)")
    ap.add_argument("--no-blocks", action="store_true",
                    help="do not start every case with a block of its own "
                         "(the cases then mostly run interpreted on a JIT lane)")
    args = ap.parse_args()

    if args.list:
        for g, doc in cases.GROUPS:
            print("%-10s %s" % (g, doc))
        return 0

    binary = os.path.abspath(args.binary)
    if not os.access(binary, os.X_OK):
        print("cpu_oracle: %s is not executable" % binary)
        return 2
    groups = args.group or [g for g, _ in cases.GROUPS]
    b = image.Builder(args.passes, blocks=not args.no_blocks)
    img = b.build(lambda bb: cases.emit(bb, groups))

    work = os.environ.get("ALPHABOX_WORK",
                          os.path.join(HERE, "..", "..", "lab"))
    workdir = os.path.join(os.path.abspath(work), "runs", "cpu-oracle-" + args.label)
    shutil.rmtree(workdir, ignore_errors=True)
    os.makedirs(workdir)
    image.write_rom(os.path.join(workdir, "oracle.rom"), img)

    port = args.port or free_port()
    env = dict(kv.split("=", 1) for kv in args.env)
    text, emu, secs = run(binary, workdir, port, env, args.timeout)
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)

    print("== cpu oracle %s: %d results (%d cases), %d instruction words, "
          "%d passes, %.1f s" % (args.label, len(b.results),
                                 len({r[0] for r in b.results}),
                                 b.code_words, args.passes, secs))
    if "@@FATAL" in text:
        tail = text[text.index("@@FATAL"):].split()
        print("  FATAL: an exception the image cannot resume from:", " ".join(tail[1:8]))
        print("  (count, vector, EXC_ADDR, EXC_SUM, MM_STAT, VA, total)")
        print("  run directory: %s" % workdir)
        return 1
    first, last = parse(text)
    if first is None:
        print("  no results: the image did not finish (see %s)" % workdir)
        for ln in emu.splitlines()[-8:]:
            print("   |", ln)
        return 1

    mism = [ln for ln in emu.splitlines() if "MISMATCH" in ln]
    # The pass's trap total must be what the cases' windows saw: anything
    # more is a trap no case expected.
    counted = [k for k, r in enumerate(b.results) if r[1] == image.TRAP_COUNT]
    for k, r in enumerate(b.results):
        if r[1] == image.TRAPS_IN_PASS:
            b.results[k] = (r[0], "traps in the pass, outside the cases' "
                            "windows", 0, r[3], None)
            for buf in (first, last):
                buf[k] -= sum(buf[j] for j in counted)
    counts = {}
    fails, newly, differ = [], [], []
    for k, res in enumerate(b.results):
        vs = {}
        for tag, val in (("first pass", first[k]), ("last pass", last[k])):
            v = verdict(res, val, tag)
            vs[tag] = v
            counts[(tag, v)] = counts.get((tag, v), 0) + 1
            if v == "FAIL":
                fails.append((tag, res, val))
            elif v == "KNOWN" and args.verbose:
                print("  KNOWN %-10s %s: %s = %016x (%s)" % (
                    tag, res[0], res[1], val, res[4][1]))
        if (res[4] is not None and res[4][2] == "all" and
                vs["first pass"] == vs["last pass"] == "PASS"):
            newly.append(res)
        if (first[k] ^ last[k]) & res[3]:
            differ.append((res, first[k], last[k], vs))
    for tag in ("first pass", "last pass"):
        print("  %-10s PASS %d  KNOWN %d  FAIL %d" % (
            tag, counts.get((tag, "PASS"), 0), counts.get((tag, "KNOWN"), 0),
            counts.get((tag, "FAIL"), 0)))
    shown = 0
    for tag, (case, fld, expect, mask, known), val in fails:
        if shown < 60:
            print("  FAIL %-10s %s: %s = %016x, the manual gives %016x%s" % (
                tag, case, fld, val & mask, expect,
                "" if mask == (1 << 64) - 1 else " (mask %x)" % mask))
        shown += 1
    if shown > 60:
        print("  ... %d more FAIL lines" % (shown - 60))
    known_refs = sorted({r[4][1] for r in b.results if r[4] is not None})
    if known_refs:
        print("  known divergences exercised: %s" % ", ".join(known_refs))
    for res in newly:
        print("  NOW PASSES (retire its KNOWN entry): %s: %s (%s)" % (
            res[0], res[1], res[4][1]))
    # The first pass is interpreted; on a JIT lane the last is compiled.
    by_case = {}
    for res, f, l, vs in differ:
        by_case.setdefault(res[0], []).append(
            "%s %x/%x" % (res[1], f & res[3], l & res[3]))
    for case, fields in list(by_case.items())[:20]:
        print("  ENGINES DIFFER %s (first/last pass): %s" % (case, ", ".join(fields)))
    if len(by_case) > 20:
        print("  ... %d more cases differ" % (len(by_case) - 20))
    if mism:
        print("  JIT_VERIFY: %d MISMATCH lines in the emulator's output" % len(mism))
    vlines = [ln for ln in emu.splitlines() if "compiled-block execs" in ln]
    if vlines:
        print("  %s" % vlines[-1].strip())
    ok = not fails and not mism
    print("  CPU ORACLE: %s" % ("PASS" if ok else "FAIL"))
    if ok and not args.keep:
        shutil.rmtree(workdir, ignore_errors=True)
    else:
        print("  run directory: %s" % workdir)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
