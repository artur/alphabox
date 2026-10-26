#!/usr/bin/env python3
"""Generate an SRM test configuration from test/rom/es40.cfg.

The base is the regression test's no-VGA machine: one EV68CB, 64 MB
(memory.bits = 26), serial0 on telnet port 21000, ALi ISA/IDE/USB. Options
change it for the SRM probes:

  --port N              serial0 telnet port
  --serial-address A    serial0's address (default the base's 127.0.0.1):
                        IPv4, IPv6 (::1, :: for both families) or a name;
                        "none" leaves the key out (the emulator's default,
                        127.0.0.1 and ::1)
  --cpus N              1-4 CPUs (copies of cpu0)
  --cpu-class CLASS     the processors' class (default: the board's own,
                        CPU_CLASS), e.g. ev8 on the ES40, which takes the EV7
                        family's rows as an Alphabox construction
  --membits N           memory.bits
  --cpu-opt KEY=VALUE   extra setting on every CPU (repeatable),
                        e.g. palcode.vms.nohle=true
  --cpu1-opt KEY=VALUE  extra setting on cpu1 only (repeatable)
  --scsi CTRL           pci0.3 (on the ES40; SLOTS has each board's) = one
                        of the Symbios parts (sym53c810, 825, 875, 895, 896) or the QLogic ones (isp1020, isp1040,
                        isp1080, isp1240), with disk0.0 = a sparse 1 GB image
                        (dka0.img, created in --dir) and disk0.5 = a 10 MB
                        RAM disk. The two-bus parts, the 896 and the 1240,
                        also get disk1.0 = dkb0.img on their second bus.
  --nic CLASS           pci0.4 (on the ES40; see SLOTS) = one of the Tulips
                        (dec21040, dec21041, dec21140, dec21143) or the Intel parts (de600,
                        i82557, i82558, i82559) on the null network backend
                        (nothing received, sends dropped; needs no host
                        privileges)
  --platform NAME       machine to emulate (default: the ES40 the base
                        configuration describes); sets platform = "NAME"
  --rom FILE            console firmware image (rom.srm); the decompressed
                        cache is named after it
  --nic-udp N:P         put that NIC on the UDP backend instead: listening
                        on 127.0.0.1:N, sending to 127.0.0.1:P (net_peer.py)
  --nic-udp-host A      the address of both ends of --nic-udp instead of
                        127.0.0.1: IPv4, IPv6 (::1, written [::1]:N) or a
                        name (net_peer.py --host A)
  --nic-nat TFTPDIR     put that NIC on the NAT backend instead (libslirp):
                        BOOTP offers 10.0.2.15 and TFTP serves TFTPDIR,
                        bootfile boot.img
  --extra-cfg FILE      configuration text added inside the machine block
                        (bridges, more PCI devices, ...)
  --ide-cfg FILE        body of the pci0.15 ali_ide block (drives)
  --floppy IMAGE        fdc0 with disk0.0 = IMAGE
  --exit-on-halt        sys0 exit_on_pal_halt = true
  --gdb PORT            sys0 gdb = PORT and gdb.wait = true: the processors
                        wait at their first instruction for a gdb to attach
                        on 127.0.0.1:PORT (docs/debugging.md)
  --gui                 a gui = sdl block, for devices that need one (the
                        graphics cards); run it with SDL_VIDEO_DRIVER=dummy
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Where each board takes the base configuration's devices: the ISA bridge,
# its IDE and USB functions (None drops one), the --scsi and the --nic
# controller. The functions' places are the board rows' own (board_function
# in src/platforms/Platforms.cpp), which the emulator insists on; the slots
# are places each board's slot rule accepts. The ES40's are the base
# configuration's own. The DS20E's console scans devices 0-10 and wires hose 0 devices 6-9 and hose 1 7-10
# (docs/platforms/ds20e.md), so its ISA bridge goes to 5, the IDE to the
# board's own device 6, and the SCSI and NIC into slots 7 and 8; the DS20
# runs the same console on the same slots (docs/platforms/ds20.md). The
# DS10's console table puts the IDE at device 13 and the USB at device 1;
# its on-board NICs are at 9 and 11 and its slots at 14-17
# (docs/platforms/ds10.md). The DS20L has the ES40's places without the
# power management unit, in hose 0 slots 3-6. The ES45, DS25 and DS15 put
# the IDE at 16, 16 and 13 with no USB; their slots are hose 0 devices 10
# and 11 (ES45, also its Model 1), 9 and 10 (DS25) and hose 2 devices 7 and 8
# (DS15). The EV7 boards have no ALi: the I/O expander on PID 0's hose 2
# carries a CMD 649 in the ALi IDE's place (the class changes with it) and a
# USS-344 USB; their slots are hose 1's two.
EV7_SLOTS = {"ali": None, "ali_ide": "pci2.2", "ali_usb": None,
             "ide_class": "cmd649", "uss344": "pci2.3",
             "scsi": "pci1.1", "nic": "pci1.2"}
# The processor class of each board's row (cpu_model in Platforms.cpp).
CPU_CLASS = {"es47": "ev7", "es80": "ev7", "gs1280": "ev7"}
SLOTS = {
    "es40": {"ali": "pci0.7", "ali_ide": "pci0.15", "ali_usb": "pci0.19",
             "scsi": "pci0.3", "nic": "pci0.4"},
    "ds20e": {"ali": "pci0.5", "ali_ide": "pci0.6", "ali_usb": None,
              "scsi": "pci0.7", "nic": "pci0.8"},
    "ds20": {"ali": "pci0.5", "ali_ide": "pci0.6", "ali_usb": None,
             "scsi": "pci0.7", "nic": "pci0.8"},
    "ds10": {"ali": "pci0.7", "ali_ide": "pci0.13", "ali_usb": "pci0.1",
             "scsi": "pci0.14", "nic": "pci0.9"},
    "ds20l": {"ali": "pci0.7", "ali_ide": "pci0.15", "ali_usb": "pci0.19",
              "scsi": "pci0.3", "nic": "pci0.4"},
    "es45": {"ali": "pci0.7", "ali_ide": "pci0.16", "ali_usb": None,
             "scsi": "pci0.10", "nic": "pci0.11"},
    "es45m1": {"ali": "pci0.7", "ali_ide": "pci0.16", "ali_usb": None,
               "scsi": "pci0.10", "nic": "pci0.11"},
    "ds25": {"ali": "pci0.7", "ali_ide": "pci0.16", "ali_usb": None,
             "scsi": "pci0.9", "nic": "pci0.10"},
    "ds15": {"ali": "pci0.7", "ali_ide": "pci0.13", "ali_usb": None,
             "scsi": "pci2.7", "nic": "pci2.8"},
    "es47": EV7_SLOTS,
    "es80": EV7_SLOTS,
    "gs1280": EV7_SLOTS,
}
BASE = os.path.join(HERE, "..", "rom", "es40.cfg")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dir", help="directory for generated disk images (default: --out's)")
    ap.add_argument("--base", default=BASE)
    ap.add_argument("--port", type=int, default=21000)
    ap.add_argument("--serial-address")
    ap.add_argument("--cpus", type=int, default=1, choices=[1, 2, 3, 4])
    ap.add_argument("--cpu-class")
    ap.add_argument("--membits", type=int)
    ap.add_argument("--cpu-opt", action="append", default=[])
    ap.add_argument("--cpu1-opt", action="append", default=[])
    ap.add_argument("--scsi", choices=["sym53c810", "sym53c825", "sym53c875",
                                       "sym53c895", "sym53c896",
                                       "isp1020", "isp1040", "isp1080",
                                       "isp1240"])
    ap.add_argument("--nic", choices=["dec21040", "dec21041", "dec21140", "dec21143",
                                     "de600", "i82557", "i82558", "i82559"])
    ap.add_argument("--platform")
    ap.add_argument("--rom")
    ap.add_argument("--nic-udp")
    ap.add_argument("--nic-udp-host", default="127.0.0.1")
    ap.add_argument("--nic-nat")
    ap.add_argument("--extra-cfg")
    ap.add_argument("--ide-cfg")
    ap.add_argument("--floppy")
    ap.add_argument("--exit-on-halt", action="store_true")
    ap.add_argument("--gdb", type=int)
    ap.add_argument("--gui", action="store_true")
    args = ap.parse_args()
    out_dir = args.dir or os.path.dirname(os.path.abspath(args.out))

    t = open(args.base).read()

    t, n = re.subn(r"port = \d+;", "port = %d;" % args.port, t, count=1)
    if n != 1:
        sys.exit("srm_cfg: serial port setting not found in base config")

    if args.serial_address == "none":
        t, n = re.subn(r'\n[ \t]*address = "[^"]*";[^\n]*', '', t, count=1)
        if n != 1:
            sys.exit("srm_cfg: serial address setting not found in base config")
    elif args.serial_address is not None:
        t, n = re.subn(r'address = "[^"]*";', 'address = "%s";' % args.serial_address,
                       t, count=1)
        if n != 1:
            sys.exit("srm_cfg: serial address setting not found in base config")

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

    platform = args.platform or "es40"
    if platform not in SLOTS:
        sys.exit("srm_cfg: no places for platform %s (add it to SLOTS)" % platform)
    cpu0 = cpu0.replace("= ev68cb", "= " + (args.cpu_class or
                                            CPU_CLASS.get(platform, "ev68cb")), 1)

    blocks = [with_opts(cpu0, args.cpu_opt)]
    for i in range(1, args.cpus):
        b = cpu0.replace("cpu0", "cpu%d" % i)
        opts = args.cpu_opt + (args.cpu1_opt if i == 1 else [])
        blocks.append(with_opts(b, opts))
    t = t[:m.start(1)] + "\n\n".join(blocks) + t[m.end(1):]

    if args.gui:
        t = "gui = sdl\n{\n  keyboard.use_mapping = false;\n}\n\n" + t

    if args.exit_on_halt:
        t, n = re.subn(r"(sys0\s*=\s*tsunami\s*\{)", r"\1\n  exit_on_pal_halt = true;", t)
        if n != 1:
            sys.exit("srm_cfg: sys0 block not found in base config")

    if args.gdb:
        t, n = re.subn(r"(sys0\s*=\s*tsunami\s*\{)",
                       r"\1\n  gdb = %d;\n  gdb.wait = true;" % args.gdb, t)
        if n != 1:
            sys.exit("srm_cfg: sys0 block not found in base config")

    if args.ide_cfg:
        body = open(args.ide_cfg).read()
        t, n = re.subn(r"(  pci0\.15 = ali_ide\s*\{)\s*\}",
                       lambda mm: mm.group(1) + "\n" + body + "  }", t)
        if n != 1:
            sys.exit("srm_cfg: empty ali_ide block not found in base config")

    slots = SLOTS[platform]
    for dev in ("ali", "ali_ide", "ali_usb"):
        base = SLOTS["es40"][dev]
        if slots[dev] is None:
            t, n = re.subn(r"\n  %s = %s\s*\{[^}]*\}\n" % (re.escape(base), dev), "\n", t)
        else:
            cls = slots.get("ide_class", dev) if dev == "ali_ide" else dev
            t, n = re.subn(r"\b%s = %s\b" % (re.escape(base), dev),
                           "%s = %s" % (slots[dev], cls), t)
        if n != 1:
            sys.exit("srm_cfg: %s block not found in base config" % dev)

    extra = ""
    if "uss344" in slots:
        extra += ("\n  %s = uss344\n  {\n    port1 = \"tablet\";\n"
                  "    port2 = \"keyboard\";\n  }\n") % slots["uss344"]
    if args.scsi:
        def sparse_disk(bus, name):
            with open(os.path.join(out_dir, name), "wb") as f:
                f.truncate(1 << 30)  # sparse 1 GB
            return ("    disk%d.0 = file\n    {\n      file = \"%s\";\n"
                    "      read_only = false;\n    }\n") % (bus, name)
        scsi = sparse_disk(0, "dka0.img")
        scsi += "    disk0.5 = ramdisk\n    {\n      size = 10M;\n    }\n"
        # The 896 and the 1240 have two SCSI buses: give the second one a
        # disk too, so a probe can see both of them.
        if args.scsi in ("sym53c896", "isp1240"):
            scsi += sparse_disk(1, "dkb0.img")
        extra += "\n  %s = %s\n  {\n%s  }\n" % (slots["scsi"], args.scsi, scsi)
    if args.nic:
        if args.nic_udp:
            nic_port, peer_port = args.nic_udp.split(":")
            host = args.nic_udp_host.strip("[]")
            # An IPv6 address carries brackets when a port follows.
            if ":" in host:
                host = "[%s]" % host
            backend = ("    type = \"udp\";\n"
                       "    udp_local = \"%s:%s\";\n"
                       "    udp_remote = \"%s:%s\";\n") % (host, nic_port, host, peer_port)
        elif args.nic_nat:
            backend = ("    type = \"nat\";\n"
                       "    tftp_root = \"%s\";\n"
                       "    bootfile = \"boot.img\";\n") % args.nic_nat
        else:
            backend = "    type = \"null\";\n"
        extra += "\n  %s = %s\n  {\n%s  }\n" % (slots["nic"], args.nic, backend)
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
