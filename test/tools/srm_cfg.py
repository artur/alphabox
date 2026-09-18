#!/usr/bin/env python3
"""Generate an SRM test configuration from test/rom/es40.cfg.

The base is the regression test's no-VGA machine: one EV68CB, 64 MB
(memory.bits = 26), serial0 on telnet port 21000, ALi ISA/IDE/USB. Options
change it for the SRM probes:

  --port N              serial0 telnet port
  --cpus N              1-4 CPUs (copies of cpu0)
  --membits N           memory.bits
  --cpu-opt KEY=VALUE   extra setting on every CPU (repeatable),
                        e.g. palcode.vms.nohle=true
  --cpu1-opt KEY=VALUE  extra setting on cpu1 only (repeatable)
  --scsi CTRL           pci0.3 = sym53c810|825|875|895|896 with disk0.0 = a
                        sparse 1 GB image (dka0.img, created in --dir) and
                        disk0.5 = a 10 MB RAM disk. The two-channel 896 also
                        gets disk1.0 = dkb0.img on its second channel.
  --nic CLASS           pci0.4 = dec21143|de600|i82557|i82558|i82559 on the
                        null network backend (nothing received, sends
                        dropped; needs no host privileges)
  --platform NAME       machine to emulate (default: the ES40 the base
                        configuration describes); sets platform = "NAME"
  --rom FILE            console firmware image (rom.srm); the decompressed
                        cache is named after it
  --nic-udp N:P         put that NIC on the UDP backend instead: listening
                        on 127.0.0.1:N, sending to 127.0.0.1:P (net_peer.py)
  --extra-cfg FILE      configuration text added inside the machine block
                        (bridges, more PCI devices, ...)
  --ide-cfg FILE        body of the pci0.15 ali_ide block (drives)
  --floppy IMAGE        fdc0 with disk0.0 = IMAGE
  --exit-on-halt        sys0 exit_on_pal_halt = true
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
BASE = os.path.join(HERE, "..", "rom", "es40.cfg")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dir", help="directory for generated disk images (default: --out's)")
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--port", type=int, default=21000)
    ap.add_argument("--cpus", type=int, default=1, choices=[1, 2, 3, 4])
    ap.add_argument("--membits", type=int)
    ap.add_argument("--cpu-opt", action="append", default=[])
    ap.add_argument("--cpu1-opt", action="append", default=[])
    ap.add_argument("--scsi", choices=["sym53c810", "sym53c825", "sym53c875",
                                       "sym53c895", "sym53c896"])
    ap.add_argument("--nic", choices=["dec21143", "de600", "i82557", "i82558", "i82559"])
    ap.add_argument("--platform")
    ap.add_argument("--rom")
    ap.add_argument("--nic-udp")
    ap.add_argument("--extra-cfg")
    ap.add_argument("--ide-cfg")
    ap.add_argument("--floppy")
    ap.add_argument("--exit-on-halt", action="store_true")
    args = ap.parse_args()
    out_dir = args.dir or os.path.dirname(os.path.abspath(args.out))

    t = open(args.base).read()

    t, n = re.subn(r"port = \d+;", "port = %d;" % args.port, t, count=1)
    if n != 1:
        sys.exit("srm_cfg: serial port setting not found in base config")

    if args.membits is not None:
        t, n = re.subn(r"memory\.bits\s*=\s*\d+;", "memory.bits = %d;" % args.membits, t)
        if n != 1:
            sys.exit("srm_cfg: memory.bits not found in base config")

    m = re.search(r"\n(  cpu0 = ev68cb\s*\{[^}]*\})\n", t)
    if not m:
        sys.exit("srm_cfg: cpu0 block not found in base config")
    cpu0 = m.group(1)

    def with_opts(block, opts):
        lines = "".join("\n    %s = %s;" % tuple(o.split("=", 1)) for o in opts)
        return re.sub(r"\{", "{" + lines, block, count=1) if lines else block

    blocks = [with_opts(cpu0, args.cpu_opt)]
    for i in range(1, args.cpus):
        b = cpu0.replace("cpu0", "cpu%d" % i)
        opts = args.cpu_opt + (args.cpu1_opt if i == 1 else [])
        blocks.append(with_opts(b, opts))
    t = t[:m.start(1)] + "\n\n".join(blocks) + t[m.end(1):]

    if args.exit_on_halt:
        t, n = re.subn(r"(sys0\s*=\s*tsunami\s*\{)", r"\1\n  exit_on_pal_halt = true;", t)
        if n != 1:
            sys.exit("srm_cfg: sys0 block not found in base config")

    if args.ide_cfg:
        body = open(args.ide_cfg).read()
        t, n = re.subn(r"(  pci0\.15 = ali_ide\s*\{)\s*\}",
                       lambda mm: mm.group(1) + "\n" + body + "  }", t)
        if n != 1:
            sys.exit("srm_cfg: empty ali_ide block not found in base config")

    extra = ""
    if args.scsi:
        def sparse_disk(bus, name):
            with open(os.path.join(out_dir, name), "wb") as f:
                f.truncate(1 << 30)  # sparse 1 GB
            return ("    disk%d.0 = file\n    {\n      file = \"%s\";\n"
                    "      read_only = false;\n    }\n") % (bus, name)
        scsi = sparse_disk(0, "dka0.img")
        scsi += "    disk0.5 = ramdisk\n    {\n      size = 10M;\n    }\n"
        # The 896 is two controllers in one: give its second channel a disk
        # too, so a probe can see both of them.
        if args.scsi == "sym53c896":
            scsi += sparse_disk(1, "dkb0.img")
        extra += "\n  pci0.3 = %s\n  {\n%s  }\n" % (args.scsi, scsi)
    if args.nic:
        if args.nic_udp:
            nic_port, peer_port = args.nic_udp.split(":")
            backend = ("    type = \"udp\";\n"
                       "    udp_local = \"127.0.0.1:%s\";\n"
                       "    udp_remote = \"127.0.0.1:%s\";\n") % (nic_port, peer_port)
        else:
            backend = "    type = \"null\";\n"
        extra += "\n  pci0.4 = %s\n  {\n%s  }\n" % (args.nic, backend)
    if args.platform:
        t = re.sub(r"(sys0 = tsunami\s*\{)",
                   lambda mm: mm.group(1) + '\n  platform = "%s";' % args.platform,
                   t, count=1)
    if args.rom:
        t = t.replace('rom.srm = "cl67srmrom.exe";', 'rom.srm = "%s";' % args.rom)
        t = t.replace('rom.decompressed = "decompressed.rom";',
                      'rom.decompressed = "%s-decompressed.rom";' % args.rom)
    if args.extra_cfg:
        extra += "\n" + open(args.extra_cfg).read()
    if args.floppy:
        extra += ("\n  fdc0 = floppy\n  {\n    disk0.0 = file\n    {\n"
                  "      file = \"%s\";\n      read_only = false;\n    }\n  }\n") % args.floppy
    if extra:
        i = t.rstrip().rfind("}")
        t = t[:i] + extra + t[i:]

    with open(args.out, "w") as f:
        f.write(t)
    return 0


if __name__ == "__main__":
    sys.exit(main())
