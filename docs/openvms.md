# OpenVMS

What was verified, on 2026-10-01, with HP OpenVMS Alpha V8.4 (the
`ALPHA084.ISO` distribution CD) on the ES40 SRM console V7.3-1
(`cl67srmrom.exe`), on an Apple-silicon host.

| | Result |
|---|---|
| Installation from the CD | Completes on the JIT build in about 15 minutes, onto an IDE disk, with the CD on the other IDE channel (see below) |
| Boot from the installed disk | To the login prompt; SYSTEM logs in on the serial console |
| Interpreter build | Boots to login with `palcode.vms.nohle = true`; with the default (the OpenVMS PALcode routines replaced natively) it stops after `%STDRV-I-STARTUP` |
| Two and four CPUs (JIT) | OpenVMS starts every secondary (`%SMP-I-CPUTRN, CPU #n has joined the active set`); `SHOW CPU` lists 0-3 active |
| DECwindows on the 3Dlabs Permedia 2 | The display server starts and the CDE login box (dtlogin) is drawn at 1024x768. Without licences nothing further: see Licences |

## Configuration

```
sys0 = tsunami
{
  memory.bits = 30;                          // 1 GB
  rom.srm = "cl67srmrom.exe";
  rom.decompressed = "decompressed.rom";
  rom.flash = "flash.rom";
  rom.dpr = "dpr.rom";
  cpu0 = ev68cb { speed = 833M; }            // cpu1..cpu3 the same for SMP
  serial0 = serial { address = "127.0.0.1"; port = 21000; }
  serial1 = serial { null_attach = true; }
  pci0.7 = ali { vga_console = false; }
  pci0.15 = ali_ide
  {
    disk0.0 = file { file = "disk.img"; read_only = false; cdrom = false; }
    disk1.0 = file { file = "ALPHA084.ISO"; read_only = true; cdrom = true; }
  }
  pci0.19 = ali_usb { }
  // pci0.2 = permedia2 { rom = "SYN80700.PAN"; }   // for DECwindows (needs a gui section)
}
```

The console is the serial port (telnet to it). The system disk is `dqa0`
(a 4 GB sparse file is plenty: the installed system uses about 2 GB), the CD
`dqb0`.

**Put the CD on the second IDE channel** (`disk1.0`, `dqb0`), not beside the
disk on the first (`disk0.1`, `dqa1`). With both on one channel the
installation stopped at 10% with `%PCSI-E-WRITEERR ... -SYSTEM-F-CTRLERR,
fatal controller error` writing `[SYS$LDR]PROCESS_MANAGEMENT.EXE`; on two
channels it completed. (The CD itself boots from either.)

## Installing

1. `boot dqb0` at `P00>>>`. Answer the date prompt (`DD-MMM-YYYY HH:MM`).
2. Menu choice 1; INITIALIZE; target `DQA0`; ODS-5; a SYSTEM password; not
   a cluster member, not a Galaxy instance; SCSNODE; DECnet no; a time zone;
   no PAKs.
3. Optional products: DECwindows Motif yes, DECnet-Plus and DECnet Phase IV
   no, TCP/IP Services yes; defaults for all options.
4. When it finishes, menu choice 9 shuts down to `P00>>>`.
5. `boot dqa0`. The first boot runs AUTOGEN and reboots by itself; the
   second comes up to `SYSTEM job terminated`. Press Return on the console
   for `Username:`.

## Timings

Single runs, wall clock, on a host busy with other work: indications, not
benchmarks. Seconds from `boot dqa0` to the end of startup / to `Username:`.

| Build | CPUs | Startup done | Username |
|---|---|---|---|
| JIT | 1 | 25 | 27 |
| JIT | 2 | 26 | 28 |
| JIT | 4 | 28 | 30 |
| Interpreter, `palcode.vms.nohle = true` | 1 | 45 | 47 |

A DCL loop of 20000 iterations took 1.0-1.1 s of guest time on the JIT
build and 12.3 s on the interpreter. MACRO-32 (`MACRO.EXE`, part of the base
system) compiles, links and runs a program.

## Graphics

OpenVMS 8.4 configures these PCI display adapters (`SYS$SYSTEM:SYS$CONFIG.DAT`)
for DECwindows, as `G` devices:

| Card | PCI id | Device | Alphabox |
|---|---|---|---|
| ELSA GLoria Synergy (3Dlabs Permedia 2, TI TVP4020) | 104C:3D07 | GZ, server `DECW$SERVER_DDX_P2` | `permedia2` -- the login box draws |
| 3Dlabs Permedia 2V | 3D3D:0009 | GZ | no |
| ZLXp-E (DEC TGA) | 1011:0004 | GY | `tga` -- not tried |
| ZLX2-E / PowerStorm 3D30, 4D20 (TGA2) | 1011:000D | GY | no |
| PowerStorm 300/350 | 10BA:0304 | GB | no |
| Oxygen VX1 (Permedia 3, 4) | 3D3D:000A, 000C | GF | no |
| ATI Radeon 7500 / 7000, ES1000 | 1002:5157, 5159, 515E | GH | no |
| ATI Mach64 GX / CX / CT | 1002:4758, 4358, 4354 | GQ | `mach64` (CT) -- not tried |
| S3 Trio32/64, DEC864 (Vision864) | 5333:8811, 88C0 | GQ | `s3` (Trio64) -- not tried |

The Permedia 2 needed two fixes to draw: `InFIFOSpace` now reads 256 (the
server waits for at least 50), and RasterizerMode's LimitsEnable is active
high (the server leaves it clear and never loads the limits).

SRM does not use the Permedia 2 as its console (README, known limitations), so the
console stays on the serial port while DECwindows takes the card. With a
graphics card present the startup sets `WINDOW_SYSTEM` to 1
(`%DECW-I-BADVALUE`) on the first boot.

## Licences

No PAKs were loaded (`SHOW LICENSE`: none). What happens without them:

- Every boot logs `%LICENSE-E-NOAUTH, DEC OPENVMS-ALPHA use is not
  authorized`, and SYSTEM may log in only on the console
  (`%LOGIN-S-LOGOPRCON, login allowed from OPA0:`).
- DECwindows is not started at boot: `DECW$SERVER_0_ERROR.LOG` says "The
  OpenVMS license check failed and the display server was not started".
  `@SYS$MANAGER:DECW$STARTUP` run by hand from the SYSTEM session starts the
  server and dtlogin.
- Motif clients refuse to run: `%LICENSE-F-NOAUTH, DEC DW-MOTIF use is not
  authorized on this node` (DECW$CLOCK, a DECterm). So no CDE session was
  reached, and keyboard input to the login box was not verified.
- TCP/IP Services was installed but not configured or started.

## Known problems

- Interpreter build: with the native OpenVMS PALcode routines (the default
  there), startup stops after `%STDRV-I-STARTUP` (twice, at 400 s and 10
  minutes). `palcode.vms.nohle = true` avoids it. JIT builds always run the
  real PALcode.
- A cached `decompressed.rom` written by a JIT build does not boot: SRM
  prints its banner and nothing else. The JIT runs a few instructions past
  the decompressor's jump into the console before it notices, and the cache
  saves that later PC (0x12d24 instead of 0x12d10) but not the registers
  those instructions set. Delete `decompressed.rom` before each run, or
  create it with the interpreter build.
- With any cached `decompressed.rom`, both builds, SRM reports
  `CPU0: unexpected exception/interrupt through vector 440` during its
  initialisation and then carries on; a run that decompresses the ROM
  itself does not.
