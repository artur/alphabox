#!/usr/bin/env python3
"""The Alphabox console for the native 21464 (firmware/ev8/console.c, S12b)
on the Marvel boards in 21464 mode, started the way N29 has it: the
emulator loads a kernel, an initrd and a command line (the system block's
`kernel`, `initrd`, `cmdline`, `auto_action`) and the console builds what
Linux reads and enters the kernel (docs/platforms/ev8-native.md, section 4
and S12b).

    console8_test.py <alphabox-binary> [label] [--port N] [--keep]
                     [--nada PATH] [--only RUN[,RUN...]]

The "kernel" is test/cpu/console8_kernel.c, built here by nada as an ELF
linked where vmlinux is (0xfffffc0001010000): it reads the HWRPB through
INIT_HWRPB, walks the slots, the memory descriptors, the GCT, the CTB and
the CRB as setup.c, smp.c, gct.c and core_marvel.c do, calls GETC, PUTS
and GET_ENV through the CRB, calls FIXUP and moves to page tables of its
own as mm/init.c's callback_init and switch_to_system_map do, calls the
console again through the remapped descriptors, takes timer interrupts,
starts every secondary as smp.c does, and halts as Linux's poweroff does
(srm_paging_stop, CALL_PAL HALT). It prints what it read, "@ name value";
this script compares each value with what the ARM [pages as in
ev8-native.md] and Linux require, and with the decisions of S12b.

The runs (each a boot of its own):

  es47        2 chips x 4 TPUs, the kernel with KSEG program headers, an
              initrd and a command line, auto_action boot; then at the
              console's prompt after the halt: show, boot (refused: the
              kernel has run), an unknown word
  es47-halt   the same machine, auto_action halt, the kernel with physical
              program headers, no initrd, no command line: the prompt
              first, then boot
  es80        8 chips x 4 TPUs (32 processors, 31 secondaries started)
  gs1280      8 chips x 4 TPUs on the GS1280's layout
  nokernel    no kernel: the prompt, boot refused, show
  gs1280-16   16 chips x 2 TPUs (32 processors, WHAMIs up to 61: the
              owner's 16 chips, 2026-10-31)
  refusals    configurations the emulator refuses, each for its reason

Run output goes to $ALPHABOX_WORK/runs/<label> (kept on failure or --keep).
Exit status 0 when everything holds.
"""
import argparse
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, "firmware", "ev8"))

import build as fw  # noqa: E402  (firmware/ev8/build.py)
import cpu_oracle  # noqa: E402

L = fw.layout()
KERNEL_SRC = os.path.join(HERE, "console8_kernel.c")
KSEG = 0xFFFFFC0000000000
KERNEL_VA = 0xFFFFFC0001010000      # vmlinux.lds: KERNEL_START_PHYS + 0x10000
VMALLOC = 0xFFFFFFFC00000000        # the kernel's own map of the CRB's pages
VPTB_LINUX = 0xFFFFFFFE00000000
SPEED_MHZ = 800
MEM_BITS = 29                       # 512 MB per PID
TK_STACK = 0x2010000                # console8_kernel.c's stacks
CMDLINE = "console=srm0 root=/dev/sda2 io7=0 s12b.test=1"
TIMEOUT = 150


def kseg(pa):
    return KSEG | pa


def pid_base(n):
    """N18: the memory of PID n."""
    return ((n & 3) | (n & 0x3FC) << 1) << 34


# ------------------------------------------------------------------------
# Building
# ------------------------------------------------------------------------
def nada_path(arg):
    return arg or fw.default_nada()


def build_kernel(nada, out, physical=False, base=KERNEL_VA):
    b = "%#x" % base
    if physical:
        b += "=%#x" % (base & ((1 << 41) - 1))
    cmd = [nada, "-t", "alpha-none", "-mcpu=ev8", "-mcmodel=large", "-n",
           "-f", "elf", "-B", b, "-o", out, KERNEL_SRC]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        raise SystemExit("nada failed on console8_kernel.c:\n" + p.stdout +
                         p.stderr)
    with open(out, "rb") as f:
        elf = f.read()
    return struct.unpack_from("<Q", elf, 24)[0]


def initrd_bytes():
    """Three and a half pages of a fixed pseudo-random stream."""
    x, out = 0x12345678, bytearray()
    for _ in range(0x7123):
        x = (x * 1103515245 + 12345) & 0x7FFFFFFF
        out.append((x >> 16) & 0xFF)
    return bytes(out)


def fnv64(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & ((1 << 64) - 1)
    return h


# ------------------------------------------------------------------------
# Running
# ------------------------------------------------------------------------
CFG = """sys0 = tsunami
{
  platform = "%(platform)s";
  memory.bits = %(bits)d;
  rom.srm = "absent.exe";
  rom.decompressed = "%(rom)s";
  rom.nvram = "cmm_nvram.bin";
%(keys)s
%(cpus)s
  serial0 = serial { address = "127.0.0.1"; port = %(port)d; }
}
"""


def config(platform, port, chips, tpus, rom="console8.rom", keys=(),
           cpu="ev8n", serial=True):
    cpus = "\n".join("  cpu%d = %s { speed = %dM; tpus = %d; }"
                     % (c, cpu, SPEED_MHZ, tpus) for c in range(chips))
    if cpu != "ev8n":
        cpus = "\n".join("  cpu%d = %s { speed = %dM; }"
                         % (c, cpu, SPEED_MHZ) for c in range(chips))
    k = "\n".join('  %s = "%s";' % kv for kv in keys)
    cfg = CFG % {"platform": platform, "bits": MEM_BITS, "rom": rom,
                 "keys": k, "cpus": cpus, "port": port}
    if not serial:   # a refusal at load time: nobody connects to wait for
        cfg = "\n".join(ln for ln in cfg.split("\n") if "serial0" not in ln)
    return cfg


def run(binary, workdir, cfg, port, steps, timeout=TIMEOUT):
    """Boot; read the CMM terminal; each step (pattern, text) waits for the
    pattern after the previous step's match and sends the text. Ends after
    the last step's pattern, or at the timeout. Stops our own emulator."""
    with open(os.path.join(workdir, "es40.cfg"), "w") as f:
        f.write(cfg)
    out = open(os.path.join(workdir, "alphabox.out"), "wb")
    t0 = time.time()
    proc = subprocess.Popen([binary, "run"], cwd=workdir, stdout=out,
                            stderr=subprocess.STDOUT)
    data = bytearray()
    done = 0
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
            sock.settimeout(0.5)
            pos = 0
            while done < len(steps) and time.time() < deadline:
                pat, text = steps[done]
                i = data.find(pat, pos)
                if i >= 0:
                    pos = i + len(pat)
                    if text:
                        time.sleep(0.05)
                        sock.sendall(text)
                    done += 1
                    continue
                try:
                    chunk = sock.recv(65536)
                except socket.timeout:
                    if proc.poll() is not None:
                        break
                    continue
                if not chunk:
                    break
                data += chunk
            # what follows the last step, briefly
            end = time.time() + 0.3
            while time.time() < end:
                try:
                    chunk = sock.recv(65536)
                    if not chunk:
                        break
                    data += chunk
                except socket.timeout:
                    break
            sock.close()
    finally:
        if proc.poll() is None:
            proc.send_signal(signal.SIGTERM)     # our own emulator only
            try:
                proc.wait(timeout=20)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
        out.close()
    with open(os.path.join(workdir, "alphabox.out"), "rb") as f:
        emu = f.read().decode("latin-1")
    text = data.decode("latin-1")
    with open(os.path.join(workdir, "serial.txt"), "w") as f:
        f.write(text)
    return text, emu, done, time.time() - t0


def results(text):
    """The kernel's "@ name value" lines: a dict of ints, or of texts for
    the [bracketed] ones."""
    r = {}
    for ln in text.replace("\r", "").split("\n"):
        m = re.match(r"^@ (\S+) (\[.*\]|[0-9a-f]+)$", ln)
        if not m:
            continue
        v = m.group(2)
        r[m.group(1)] = v[1:-1] if v.startswith("[") else int(v, 16)
    return r


class Checks:
    def __init__(self, run):
        self.run, self.passes, self.fails = run, 0, []

    def ok(self, what, got, want=None, good=None):
        if good is None:
            good = got == want
        if good:
            self.passes += 1
        else:
            self.fails.append("%s: %s: got %s%s" % (
                self.run, what,
                ("%#x" % got) if isinstance(got, int) else repr(got),
                "" if want is None else ", want " +
                (("%#x" % want) if isinstance(want, int) else repr(want))))

    def has(self, what, text, sub):
        self.ok(what, sub in text, True)


# ------------------------------------------------------------------------
# What must hold
# ------------------------------------------------------------------------
def intr_freq(hz):
    """The EV7 console's rule for the interval timer (marvel.md, M4): ITV n
    with n + 1 = ceil(hz / 2^27), the fastest rate not above 1024 Hz, a tick
    every (n + 1) x 2^17 cycles; the HWRPB's frequency is that x 4096."""
    n1 = -(-hz // (1 << 27))
    return 4096 * hz // (n1 << 17)


def check_kernel(c, r, m):
    """Everything the test kernel printed, against the ARM and Linux. `m`:
    the machine and what the run loaded."""
    hz = SPEED_MHZ * 1000000
    chips, tpus = m["chips"], m["tpus"]
    mem = 1 << MEM_BITS
    hwpa = L["HWRPB_PA"]

    def q(name, default=-1):
        """A value the kernel printed; -1, which no check takes, if not"""
        return r.get(name, default)
    need = ("hw.id", "slot.0.flags", "mem.numclusters", "gct.visits",
            "crb.dispatch_va", "fixup.result", "own.puts", "p0.ticks",
            "cb.regs_bad")
    for n in need:
        c.ok("printed " + n, n in r, True)
    # The entry [ARM pp. 719-721, Table 27-9; S12a's list]
    c.ok("entry.ps (kernel mode, IPL 7)", q("entry.ps"), 7)
    c.ok("entry.whami", q("entry.whami"), 0)
    c.ok("entry.mces (8)", q("entry.mces"), 8)
    c.ok("entry.sp is the boot PCB's KSP", q("entry.sp"), q("slot.0.pcb.ksp"))
    c.ok("entry.sp in KSEG, quadword aligned", q("entry.sp") >> 41 == 0x7FFFFE
         and q("entry.sp") % 8 == 0, True)
    # The HWRPB [ARM pp. 618-626], through INIT_HWRPB (hwrpb.h)
    c.ok("INIT_HWRPB->phys_addr", q("init_hwrpb.phys"), hwpa)
    c.ok("hw.phys", q("hw.phys"), hwpa)
    c.ok("hw.id", q("hw.id"), int.from_bytes(b"HWRPB\0\0\0", "little"))
    c.ok("hw.revision 14 (>= 5, setup.c's DSR)", q("hw.revision"), 14)
    c.ok("hw.cpuid", q("hw.cpuid"), 0)
    c.ok("hw.pagesize", q("hw.pagesize"), 8192)
    c.ok("hw.pa_bits", q("hw.pa_bits"), 48)
    c.ok("hw.ext_va", q("hw.ext_va"), 0)
    c.ok("hw.max_asn (MAX_ASN 255)", q("hw.max_asn"), 255)
    ssn = q("hw.ssn", "")
    ssn = ssn if isinstance(ssn, str) else ""
    c.ok("hw.ssn not MILO (alpha_using_srm)", not ssn.startswith("MILO"), True)
    c.ok("hw.ssn not QEMU", "QEMU" not in ssn, True)
    c.ok("hw.sys_type (ST_DEC_MARVEL)", q("hw.sys_type"), 39)
    c.ok("hw.sys_variation (MPCAP, embedded console, no RX/TX extent)",
         q("hw.sys_variation"), 5)
    c.ok("hw.intr_freq", q("hw.intr_freq"), intr_freq(hz))
    c.ok("hw.cycle_freq", q("hw.cycle_freq"), hz)
    c.ok("hw.vptb (Linux's)", q("hw.vptb"), VPTB_LINUX)
    c.ok("hw.res1", q("hw.res1"), 0)
    for off in range(224, 288, 8):
        c.ok("hw[%d] zero at bootstrap" % off, q("hw.q%x" % off), 0)
    c.ok("hw.chksum", q("hw.chksum"), q("hw.chksum_calc"))
    c.ok("hw.rxrdy", q("hw.rxrdy"), 0)
    c.ok("hw.txrdy", q("hw.txrdy"), 0)
    c.ok("hw.cdb_offset", q("hw.cdb_offset"), 0)
    for off in ("tbhb", "ctbt", "crb", "mddt", "frut", "dsr", "processor"):
        v = q("hw.%s_offset" % off)
        c.ok("hw.%s_offset quadword aligned, nonzero" % off,
             v is not None and v and v % 8 == 0, True)
    nslots = 4 * chips
    c.ok("hw.nr_processors (4 per PID, N25)", q("hw.nr_processors"), nslots)
    size = q("hw.processor_size")
    c.ok("hw.processor_size (x 128, >= 624)", size % 128 == 0 and size >= 624,
         True)
    c.ok("hw.processor_offset (x 128)", q("hw.processor_offset") % 128, 0)
    c.ok("hw.ctb_nr", q("hw.ctb_nr"), 1)
    c.ok("hw.ctb_size (>= 31 quadwords)", q("hw.ctb_size") >= 248, True)
    ends = [q("hw.processor_offset") + nslots * size,
            q("hw.ctbt_offset") + q("hw.ctb_size"),
            q("hw.crb_offset") + 48 + 24 * q("crb.map_entries"),
            q("hw.mddt_offset") + (3 + 7 * q("mem.numclusters")) * 8,
            q("hw.tbhb_offset") + 64,
            q("hw.frut_offset") + 0x80 * q("gct.visits")]
    c.ok("hw.size covers the blocks", q("hw.size") >= max(ends), True)
    c.ok("hw.size below the console", hwpa + q("hw.size") <= L["CONSOLE_PA"],
         True)
    c.ok("region 0 maps the HWRPB and its blocks [ARM p. 618]",
         q("region0.mismatch"), 0)
    for off in range(0, 64, 8):
        c.ok("tbb[%d]" % off, q("tbb.q%x" % off), 0)
    c.ok("dsr.sysname (platform_string)", q("dsr.sysname"), m["sysname"])
    c.ok("dsr.sysname_len", q("dsr.sysname_len"), len(m["sysname"]))
    # The slots [ARM pp. 630-639; smp.c]
    present = [w for w in range(nslots) if (w & 3) < tpus]
    for w in range(nslots):
        s = "slot.%x." % w
        if w not in present:
            c.ok(s + "zero (no processor)", q(s + "nonzero"), 0)
            continue
        c.ok(s + "pa", q(s + "pa"), hwpa + q("hw.processor_offset") + w * size)
        fl = q(s + "flags")
        c.ok(s + "flags PA PP PL PMV PV (0x1cc)", fl & 0x1CC, 0x1CC)
        if w == 0:
            c.ok(s + "flags BIP, CV set, RC clear [ARM pp. 718-721]",
                 fl & 0x23, 0x21)
        else:
            c.ok(s + "flags BIP, CV, RC clear", fl & 0x23, 0)
        c.ok(s + "flags halt requested 0", fl >> 16 & 0xFF, 0)
        c.ok(s + "pal_mem_size", q(s + "pal_mem_size"), L["PAL_MEM_LEN"])
        c.ok(s + "pal_scr_size", q(s + "pal_scr_size"), L["PAL_SCR_LEN"])
        c.ok(s + "pal_mem_pa", q(s + "pal_mem_pa"), L["PAL_MEM_PA"])
        c.ok(s + "pal_scr_pa", q(s + "pal_scr_pa"), L["PAL_SCR_PA"])
        c.ok(s + "pal_rev", q(s + "pal_rev"), L["PAL_REVISION"])
        c.ok(s + "type (18, D3)", q(s + "type"), 18)
        c.ok(s + "variation IEEE", q(s + "variation") & 2, 2)
        c.ok(s + "halt_pcbb (its HWPCB)", q(s + "halt_pcbb"), q(s + "pa"))
        for f in ("halt_pc", "halt_ps", "halt_arg", "halt_ra", "halt_pv",
                  "halt_reason", "ipc0"):
            c.ok(s + f + " zero", q(s + f), 0)
        c.ok(s + "pal_avail2 (variation 2 loaded)", q(s + "pal_avail2"),
             L["PAL_REVISION"])
        c.ok(s + "pal_avail0 (the console's revision)",
             q(s + "pal_avail0") != 0, True)
    p0 = "slot.0.pcb."
    c.ok(p0 + "usp", q(p0 + "usp"), 0)
    c.ok(p0 + "ptbr in the console's cluster", (q(p0 + "ptbr") << 13) <
         L["FIRMWARE_TOP"], True)
    c.ok(p0 + "pcc, asn", q(p0 + "pcc_asn"), 0)
    c.ok(p0 + "unique", q(p0 + "unique"), 0)
    c.ok(p0 + "flags FEN", q(p0 + "flags"), 1)
    c.ok("smp.probed", q("smp.probed"), len(present))
    # The memory descriptors [ARM pp. 705-707; setup_memory]: the console's
    # cluster, the OS's, a cluster per PID at N18's place
    n = q("mem.numclusters")
    c.ok("mem.numclusters", n, 2 + (chips - 1))
    c.ok("mem.chksum", q("mem.chksum"), q("mem.chksum_calc"))
    c.ok("mem.optional_pa", q("mem.optional_pa"), 0)
    cl = [(q("mem.%x.start_pfn" % i), q("mem.%x.numpages" % i),
           q("mem.%x.usage" % i)) for i in range(n)]
    c.ok("mem clusters by increasing PFN",
         all(cl[i][0] + cl[i][1] <= cl[i + 1][0] for i in range(n - 1)), True)
    c.ok("mem.0 the console's (usage 1) from 0", (cl[0][0], cl[0][2] & 3),
         (0, 1))
    c.ok("mem.0 covers the firmware", cl[0][1] << 13 >= L["CONSOLE_TOP"], True)
    c.ok("mem.0 ends below the kernel", (cl[0][0] + cl[0][1]) << 13 <=
         0x1000000, True)
    c.ok("PID 0's memory, all of it", (cl[0][1] + cl[1][1]) << 13, mem)
    c.ok("mem.1 the OS's", (cl[1][0], cl[1][2]), (cl[0][1], 0))
    for p in range(1, chips):
        c.ok("mem for PID %d (N18)" % p, cl[1 + p] if 1 + p < n else None,
             (pid_base(p) >> 13, mem >> 13, 0))
    for i in range(n):
        for f in ("numtested", "bitmap_va", "bitmap_pa", "bitmap_chksum"):
            c.ok("mem.%x.%s" % (i, f), q("mem.%x.%s" % (i, f)), 0)
    # The GCT (gct.c, core_marvel.c)
    c.ok("gct root at frut_offset", q("gct.0.off"), 0)
    visits = q("gct.visits")
    c.ok("gct nodes all GLXY", all(q("gct.%x.magic" % k) == 0x59584C47
                                   for k in range(visits)), True)
    io7s = sorted(int(x, 16) for x in re.findall(
        r"@ gct\.io7_pid ([0-9a-f]+)", m["text"]))
    c.ok("gct IO7 PIDs", io7s, m["io7"])
    ids = sorted(q("gct.%x.id" % k) for k in range(max(visits, 0))
                 if q("gct.%x.type" % k) == 0x0E)
    c.ok("gct IO7 node ids PID << 8 (4.1)", ids, [p << 8 for p in m["io7"]])
    c.ok("gct nodes: the root and one per IO7", visits, 1 + len(m["io7"]))
    # The CTB and CRB [ARM pp. 685-690]
    c.ok("ctb type 2, a serial line", q("ctb.q0"), 2)
    c.ok("ctb TERM_TYPE not graphics (Linux leaves the VGA)",
         q("ctb.term_type") != 3, True)
    c.ok("ctb DSD length", q("ctb.q3"), q("hw.ctb_size") - 32)
    ent = q("crb.map_entries")
    c.ok("crb.map_entries", ent >= 1, True)
    tot = sum(q("crb.map.%x.count" % i) for i in range(ent))
    c.ok("crb.map_pages", q("crb.map_pages"), tot)
    mva, mpa, mcnt = (q("crb.map.0.va"), q("crb.map.0.pa"),
                      q("crb.map.0.count"))
    c.ok("crb map 0 page aligned", mva % 8192 == 0 and mpa % 8192 == 0, True)
    c.ok("crb map 0 in region 0", 0x10000000 <= mva < 0x20000000, True)
    for pd in ("dispatch", "fixup"):
        va, pa = q("crb.%s_va" % pd), q("crb.%s_pa" % pd)
        c.ok("crb.%s in map 0's pages [ARM p. 687]" % pd,
             0 <= va - mva < mcnt * 8192 and va - mva == pa - mpa, True)
        c.ok("crb.%s quadword aligned" % pd, va % 8 == 0 and pa % 8 == 0,
             True)
        c.ok("crb.pd_%s entry through VA and PA" % pd,
             q("crb.pd_%s.entry" % pd), q("crb.pd_%s.entry_pa" % pd))
        c.ok("crb.pd_%s entry" % pd, q("crb.pd_%s.entry" % pd) % 4 == 0 and
             q("crb.pd_%s.entry" % pd) != 0, True)
    # The zero page (setup.h) and the initrd
    c.ok("zp.cmdline", q("zp.cmdline"), m["cmdline"])
    c.ok("zp.cmdline_len", q("zp.cmdline_len"), len(m["cmdline"]))
    if m["initrd"]:
        rd = m["initrd"]
        pa = (mem - len(rd)) & ~0x1FFF
        c.ok("zp.initrd_start (KSEG)", q("zp.initrd_start"), kseg(pa))
        c.ok("zp.initrd_size", q("zp.initrd_size"), len(rd))
        c.ok("zp.initrd bytes", q("zp.initrd_fnv"), fnv64(rd))
        c.ok("initrd in the OS's memory", cl[1][0] <= pa >> 13 and
             (pa + len(rd)) >> 13 <= cl[1][0] + cl[1][1], True)
    else:
        c.ok("zp.initrd_start", q("zp.initrd_start"), 0)
        c.ok("zp.initrd_size", q("zp.initrd_size"), 0)
    # GET_ENV [ARM pp. 642-644, 676]
    texts = {1: m["auto_action"], 7: m["cmdline"], 8: m["cmdline"],
             9: "OFF", 0xB: "ON", 0xD: "0", 0xE: "36", 0xF: "0"}
    for tag in ("env.", "env2."):
        for i in list(range(1, 16)):
            e = tag + "%x." % i
            c.ok(e + "status (recognized)", q(e + "status"), 0)
            if i in texts:
                c.ok(e + "text", q(e + "text"), texts[i])
                c.ok(e + "count", q(e + "count"), len(texts[i]))
        for i in (0, 0x10, 0x40, 0x80, 0xFF):
            e = tag + "%x." % i
            c.ok(e + "status (not recognized, 110)", q(e + "status"), 6)
            c.ok(e + "count", q(e + "count"), 0)
        cut = m["cmdline"][:5]
        c.ok(tag + "8 truncated: status", q(tag + "8.short.status"),
             1 if len(m["cmdline"]) > 5 else 0)
        c.ok(tag + "8 truncated: count", q(tag + "8.short.count"), len(cut))
        c.ok(tag + "8 truncated: text", q(tag + "8.short.text"), cut)
    # PUTS and GETC [ARM pp. 650-653]; FIXUP [p. 680]
    c.ok("puts.result (6 written, status 000)", q("puts.result"), 6)
    for g, ch in (("getc", "Q"), ("getc2", "Z")):
        c.ok(g + " with nothing typed: status 10x", q(g + ".empty") >> 62, 2)
        c.ok(g + ".status (a character)", q(g + ".status") in (0, 1), True)
        c.ok(g + ".char", q(g + ".char"), ord(ch))
        c.ok(g + ".unit", q(g + ".unit"), 0)
    c.ok("fixup.result <63> clear", q("fixup.result") >> 63, 0)
    c.ok("switch.old_pcbb (the boot PCB, slot 0's HWPCB)",
         q("switch.old_pcbb"), q("slot.0.pa"))
    c.ok("switch.orig_ptbr", q("switch.orig_ptbr"), q("slot.0.pcb.ptbr"))
    c.ok("own.dispatch_va", q("own.dispatch_va"),
         VMALLOC + q("crb.dispatch_va") - mva)
    c.ok("own.map0_va", q("own.map0_va"), VMALLOC)
    c.ok("own.puts (through the remapped descriptor)", q("own.puts"), 5)
    c.ok("cb.regs_bad (R2-R15, R29, SP kept)", q("cb.regs_bad"), 0)
    # The timer the console started (the EV7 console's rate)
    c.ok("p0.ticks", q("p0.ticks") >= 3, True)
    c.ok("p0.ticktype (entInt 1)", q("p0.ticktype"), 1)
    c.ok("p0.other interrupts", q("p0.other"), 0)
    # The rate: each chip's ITV as the HWRPB's frequency says (the ticks'
    # spacing is the host's to keep, and a loaded host bunches them)
    for p in range(chips):
        c.ok("itv.%d (R_INTER_TIM, n + 1 = ceil(hz / 2^27))" % p,
             q("itv.%x" % p), -(-hz // (1 << 27)) - 1)
    # The secondaries, started as smp.c starts them
    for w in present[1:]:
        s = "sec.%x." % w
        c.ok(s + "bip (the console's ack)", q(s + "bip"), 1)
        c.ok(s + "flags at its start: BIP, CV, RC clear [ARM p. 726]",
             q(s + "flags") & 0x1EF, 0x1ED)
        c.ok(s + "done", q(s + "done"), 1)
        c.ok(s + "r27 (CPU_restart_data)", q(s + "r27"), q("kernel.sec_start"))
        ipcb = kseg(TK_STACK + w * 0x4000 + 0x3000)
        c.ok(s + "sp (the idle PCB's, after swpctx)",
             ipcb - 0x400 <= (q(s + "sp") or 0) < ipcb, True)
        c.ok(s + "ps", q(s + "ps"), 7)
        c.ok(s + "whami", q(s + "whami"), w)
        c.ok(s + "ticks", q(s + "ticks"), 1)
        c.ok(s + "ticktype", q(s + "ticktype"), 1)
        c.ok(s + "halt_reason", q(s + "halt_reason"), 5)
        c.ok(s + "halt_flags remain halted", q(s + "halt_flags") >> 16 & 0xFF,
             4)


def check_console(c, text, m):
    """What the console prints of itself: what it built, and the kernel's
    entry (S12b's own lines)."""
    c.has("banner", text, "Alphabox EV8 console")
    c.has("machine line", text, "%%CON8-I-MACHINE, %s: %d chip%s of %d TPUs, "
          "%d MHz" % (m["sysname"], m["chips"], "" if m["chips"] == 1 else
                      "s", m["tpus"], SPEED_MHZ))
    for p in range(m["chips"]):
        c.has("memory line PID %d" % p, text, "%%CON8-I-MEMORY, PID %d: "
              "%d MB at %x" % (p, (1 << MEM_BITS) >> 20, pid_base(p)))
    for p in m["io7"]:
        c.has("IO7 line PID %d" % p, text, "%%CON8-I-IO7, PID %d" % p)
    c.has("HWRPB line", text, "%%CON8-I-HWRPB, at %x, %d slots"
          % (L["HWRPB_PA"], 4 * m["chips"]))
    if m["entry"]:
        c.has("kernel line", text, "%%CON8-I-KERNEL, entry %x" % m["entry"])
        c.has("cmdline line", text, '%%CON8-I-CMDLINE, "%s"' % m["cmdline"])
    if m["initrd"]:
        pa = ((1 << MEM_BITS) - len(m["initrd"])) & ~0x1FFF
        c.has("initrd line", text, "%%CON8-I-INITRD, %x, %d bytes"
              % (pa, len(m["initrd"])))


def check_halt(c, text):
    """The halt (Linux's poweroff) back at the console's prompt."""
    i = text.find("@@DONE")
    tail = text[i:] if i >= 0 else ""
    c.has("halt line", tail, "%CON8-I-HALT, CPU 0, reason 5")
    c.has("halt: requested 4 (remain halted)", tail, "requested 4")
    c.has("prompt after the halt", tail, "P00>>>")


# ------------------------------------------------------------------------
# The runs
# ------------------------------------------------------------------------
def machine(platform, chips, tpus, io7, entry, initrd, cmdline,
            auto="BOOT"):
    names = {"es47": "AlphaServer ES47", "es80": "AlphaServer ES80",
             "gs1280": "AlphaServer GS1280"}
    return {"platform": platform, "chips": chips, "tpus": tpus, "io7": io7,
            "entry": entry, "initrd": initrd, "cmdline": cmdline,
            "auto_action": auto, "sysname": names[platform]}


KERNEL_STEPS = [(b"@@TYPE1", b"Q"), (b"@@TYPE2", b"Z"), (b"@@DONE", None),
                (b"P00>>>", None)]


def kernel_run(binary, work, label, port, keys, m, steps=None, checks=None):
    wd = os.path.join(work, label)
    os.makedirs(wd, exist_ok=True)
    for f in ("console8.rom", "kernel.elf", "kernelp.elf", "initrd.img"):
        src = os.path.join(work, f)
        if os.path.exists(src):
            shutil.copy(src, os.path.join(wd, f))
    cfg = config(m["platform"], port, m["chips"], m["tpus"], keys=keys)
    text, emu, done, secs = run(binary, wd, cfg, port,
                                steps or KERNEL_STEPS)
    m = dict(m, text=text)
    c = Checks(label)
    c.ok("steps reached", done, len(steps or KERNEL_STEPS))
    check_console(c, text, m)
    r = results(text)
    try:
        check_kernel(c, r, m)
    except (TypeError, ValueError, ZeroDivisionError, IndexError) as e:
        c.ok("the kernel's values could be checked (%s)" % e, False, True)
    check_halt(c, text)
    mism = [ln for ln in emu.splitlines() if "MISMATCH" in ln]
    c.ok("JIT_VERIFY MISMATCH lines", len(mism), 0)
    if checks:
        checks(c, text, emu)
    print("  %-10s %5.1f s, %d results: PASS %d, FAIL %d"
          % (label, secs, len(r), c.passes, len(c.fails)))
    return c


def run_es47(binary, work, port, entry):
    m = machine("es47", 2, 4, [0], entry, initrd_bytes(), CMDLINE)
    keys = [("kernel", "kernel.elf"), ("initrd", "initrd.img"),
            ("cmdline", CMDLINE)]
    steps = KERNEL_STEPS + [(b"", b"show\r"), (b"%CON8-I-HWRPB", None),
                            (b"P00>>>", b"boot\r"), (b"%CON8-W-BOOTED", None),
                            (b"P00>>>", b"frob\r"), (b"%CON8-W-WHAT", None),
                            (b"P00>>>", None)]

    def more(c, text, emu):
        c.has("emulator: the kernel loaded", emu, "%MVL-I-KERNEL: kernel.elf")
        i = text.find("@@DONE")
        tail = text[i:]
        c.ok("show repeats the summary", tail.count("%CON8-I-HWRPB"), 1)
        c.has("boot after the kernel ran: refused", tail, "%CON8-W-BOOTED")
        c.has("an unknown word", tail, "%CON8-W-WHAT")
        c.ok("booted at once (no prompt before the kernel)",
             text[:text.find("@@BEGIN")].count("P00>>>"), 0)
    return kernel_run(binary, work, "es47", port, keys, m, steps, more)


def run_es47_halt(binary, work, port, entry):
    m = machine("es47", 2, 4, [0], entry, b"", "", "HALT")
    keys = [("kernel", "kernelp.elf"), ("auto_action", "halt")]
    steps = [(b"P00>>>", b"boot\r")] + KERNEL_STEPS

    def more(c, text, emu):
        i = text.find("@@BEGIN")
        c.ok("prompt before the kernel", text[:i].count("P00>>>") >= 1, True)
        c.has("the boot line", text[:i], "%CON8-I-BOOT, entering the kernel")
    return kernel_run(binary, work, "es47-halt", port, keys, m, steps, more)


def run_big(binary, work, port, entry, platform, io7, chips=8, tpus=4,
            label=None):
    m = machine(platform, chips, tpus, io7, entry, initrd_bytes(), CMDLINE)
    keys = [("kernel", "kernel.elf"), ("initrd", "initrd.img"),
            ("cmdline", CMDLINE)]
    return kernel_run(binary, work, label or platform, port, keys, m)


def run_nokernel(binary, work, port):
    wd = os.path.join(work, "nokernel")
    os.makedirs(wd, exist_ok=True)
    shutil.copy(os.path.join(work, "console8.rom"), wd)
    cfg = config("es47", port, 2, 4)
    steps = [(b"P00>>>", b"boot\r"), (b"%CON8-W-NOKERNEL", None),
             (b"P00>>>", b"help\r"), (b"%CON8-I-HELP", None),
             (b"P00>>>", None)]
    text, emu, done, secs = run(binary, wd, cfg, port, steps, timeout=60)
    c = Checks("nokernel")
    c.ok("steps reached", done, len(steps))
    m = machine("es47", 2, 4, [0], 0, b"", "")
    check_console(c, text, m)
    c.ok("no kernel line", "%CON8-I-KERNEL" in text, False)
    print("  %-10s %5.1f s: PASS %d, FAIL %d" % ("nokernel", secs, c.passes,
                                                 len(c.fails)))
    return c


def run_refusals(binary, work, port, nada):
    """Configurations refused at start, each with its reason."""
    wd = os.path.join(work, "refused")
    os.makedirs(wd, exist_ok=True)
    for f in ("console8.rom", "pal8only.rom", "kernel.elf"):
        shutil.copy(os.path.join(work, f), wd)
    with open(os.path.join(wd, "notelf.bin"), "wb") as f:
        f.write(b"not an ELF file at all" * 4)
    build_kernel(nada, os.path.join(wd, "far.elf"), base=0xFFFFFC0040000000)
    # Malformed ELFs whose sizes and offsets wrap a sum: a p_memsz that
    # brings the segment's end round past 2^64 to just above 0, a p_offset
    # that does the same with the file's end, an e_phoff past it
    with open(os.path.join(wd, "kernel.elf"), "rb") as f:
        elf = bytearray(f.read())
    phoff = struct.unpack_from("<Q", elf, 32)[0]
    pa = struct.unpack_from("<Q", elf, phoff + 24)[0] & ((1 << 41) - 1)
    for name, at, value in (
            ("memsz.elf", phoff + 40, (1 << 64) - pa + 0x1000),
            ("offset.elf", phoff + 8, (1 << 64) - 0x100),
            ("phoff.elf", 32, (1 << 64) - 0x10)):
        bad = bytearray(elf)
        struct.pack_into("<Q", bad, at, value)
        with open(os.path.join(wd, name), "wb") as f:
            f.write(bad)
    es40 = """sys0 = tsunami
{
  platform = "es40";
  memory.bits = 27;
  rom.srm = "absent.exe";
  kernel = "kernel.elf";
  cpu0 = ev68cb { speed = 800M; }
}
"""
    cases = (
        ("kernel on an ES40", es40,
         "only an ES47, ES80 or GS1280 of ev8n processors"),
        ("kernel with an image that has no console",
         config("es47", port, 2, 4, rom="pal8only.rom",
                keys=[("kernel", "kernel.elf")], serial=False),
         "the boot image has no console to take them"),
        ("kernel not an ELF", config("es47", port, 2, 4,
                                     keys=[("kernel", "notelf.bin")],
                                     serial=False),
         "not an ELF file"),
        ("kernel past PID 0's memory", config("es47", port, 2, 4,
                                              keys=[("kernel", "far.elf")],
                                              serial=False),
         "is not in PID 0's memory"),
        ("initrd without a kernel", config("es47", port, 2, 4,
                                           keys=[("initrd", "kernel.elf")],
                                           serial=False),
         "there is no kernel to take it"),
        ("auto_action wrong", config("es47", port, 2, 4,
                                     keys=[("auto_action", "maybe")],
                                     serial=False),
         "boot or halt"),
        ("kernel with a p_memsz past 2^64", config(
            "es47", port, 2, 4, keys=[("kernel", "memsz.elf")], serial=False),
         "is not in PID 0's memory"),
        ("kernel with a p_offset past 2^64", config(
            "es47", port, 2, 4, keys=[("kernel", "offset.elf")], serial=False),
         "a segment's bytes are not in the file"),
        ("kernel with an e_phoff past 2^64", config(
            "es47", port, 2, 4, keys=[("kernel", "phoff.elf")], serial=False),
         "the program headers are not in the file"),
        ("a GS1280 of 16 chips x 4 TPUs (the emulator's 32 processors)",
         config("gs1280", port, 16, 4, keys=[("kernel", "kernel.elf")],
                serial=False),
         "Alphabox runs at most 32"),
    )
    c = Checks("refusals")
    for what, cfg, reason in cases:
        with open(os.path.join(wd, "es40.cfg"), "w") as f:
            f.write(cfg)
        try:
            p = subprocess.run([binary, "run"], cwd=wd, capture_output=True,
                               timeout=60)
            rc, out = p.returncode, (p.stdout + p.stderr).decode("latin-1")
        except subprocess.TimeoutExpired as e:   # killed: our own emulator
            rc = None
            out = ((e.stdout or b"") + (e.stderr or b"")).decode("latin-1")
        c.ok("refused: %s (rc %s)" % (what, rc),
             rc is not None and rc > 0 and reason in out, True)
        if not (rc is not None and rc > 0 and reason in out):
            for ln in out.splitlines()[-4:]:
                print("   |", ln)
    print("  %-10s: PASS %d, FAIL %d" % ("refusals", c.passes, len(c.fails)))
    return c


SHOW_FAILS = 200
RUNS = ("es47", "es47-halt", "es80", "gs1280", "gs1280-16", "nokernel",
        "refusals")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label", nargs="?", default="console8")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", "0")))
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--nada", default=None)
    ap.add_argument("--only", default=",".join(RUNS))
    args = ap.parse_args()
    work = os.environ.get("ALPHABOX_WORK", os.path.join(REPO, "lab"))
    work = os.path.join(os.path.abspath(work), "runs", args.label)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    binary = os.path.abspath(args.binary)
    port = args.port or cpu_oracle.free_port()
    nada = nada_path(args.nada)

    pal, _ = fw.build_pal(args.nada)
    con = fw.build_console(args.nada, L)
    fw.write_rom(os.path.join(work, "console8.rom"),
                 fw.image(pal, L, con, [fw.boot_block(L)]), L)
    fw.write_rom(os.path.join(work, "pal8only.rom"), fw.image(pal, L), L)
    entry = build_kernel(nada, os.path.join(work, "kernel.elf"))
    entryp = build_kernel(nada, os.path.join(work, "kernelp.elf"),
                          physical=True)
    # nada writes a physical e_entry with physical program headers; the
    # console enters the KSEG address of the entry's PA either way
    entryp = kseg(entryp & ((1 << 41) - 1))
    with open(os.path.join(work, "initrd.img"), "wb") as f:
        f.write(initrd_bytes())
    print("== the EV8 console (S12b): PALcode %#x bytes, console %#x bytes, "
          "kernel entry %x" % (len(pal), len(con), entry))
    only = args.only.split(",")
    checks = []
    if "es47" in only:
        checks.append(run_es47(binary, work, port, entry))
    if "es47-halt" in only:
        checks.append(run_es47_halt(binary, work, port, entryp))
    if "es80" in only:
        checks.append(run_big(binary, work, port, entry, "es80", [0, 2, 4, 6]))
    if "gs1280" in only:
        checks.append(run_big(binary, work, port, entry, "gs1280", [0]))
    if "nokernel" in only:
        checks.append(run_nokernel(binary, work, port))
    if "gs1280-16" in only:
        checks.append(run_big(binary, work, port, entry, "gs1280", [0],
                              chips=16, tpus=2, label="gs1280-16"))
    if "refusals" in only:
        checks.append(run_refusals(binary, work, port, nada))
    passes = sum(c.passes for c in checks)
    fails = [f for c in checks for f in c.fails]
    for f in fails[:SHOW_FAILS]:
        print("  FAIL " + f)
    if len(fails) > SHOW_FAILS:
        print("  ... %d more" % (len(fails) - SHOW_FAILS))
    print("  CONSOLE8: %s (PASS %d, FAIL %d)"
          % ("PASS" if not fails else "FAIL", passes, len(fails)))
    if not fails and not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print("  run directory: %s" % work)
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
