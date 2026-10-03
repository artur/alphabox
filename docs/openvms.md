# OpenVMS

What was verified, on 2026-10-01, with HP OpenVMS Alpha V8.4 (the
`ALPHA084.ISO` distribution CD) on the ES40 SRM console V7.3-1
(`cl67srmrom.exe`), on an Apple-silicon host.

| | Result |
|---|---|
| Installation from the CD | Completes on the JIT build in about 15 minutes, onto an IDE disk, with the CD on either IDE channel (see below) |
| Boot from the installed disk | To the login prompt; SYSTEM logs in on the serial console |
| Interpreter build | Boots to login with the default settings (the OpenVMS PALcode routines replaced natively) and with `palcode.vms.nohle = true` (the real PALcode) |
| Two and four CPUs (JIT) | OpenVMS starts every secondary (`%SMP-I-CPUTRN, CPU #n has joined the active set`); `SHOW CPU` lists 0-3 active |
| DECwindows on the 3Dlabs Permedia 2 | The display server starts and the CDE login box (dtlogin) is drawn at 1024x768. Without licences nothing further: see Licences |
| DECwindows on the ATI Radeon 7500 | The same, at 1024x768 in 24-bit colour: the Radeon server (`GHA0`) draws through the command processor's ring, and the login box matches the Permedia 2's frame except for the pointer and the text cursor. Typed keys reach it. See Graphics |
| SCSI: NCR 53C875 (`sym53c875`), QLogic ISP1040 (`isp1040`) | Since 2026-10-02 (until then the system crashed in startup): `PKA0`/`PKB0` online, disks `DKA100`/`DKB100` initialised, mounted, written and read back (COPY, DIFFERENCES, BACKUP), on the ES40 (JIT and interpreter) and the DS20E (JIT). See SCSI controllers |
| Network: DE500 (`dec21143`, `EWA0`), DE600 (`de600`, `EIA0`) | Both receive. The DE600 refuses broadcasts while no protocol has asked for them, as OpenVMS configures it: see Network |

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
  // pci0.2 = radeon { rom = "ATI.7500.64.Hynix50_020416.rom"; }   // or this
}
```

The console is the serial port (telnet to it). The system disk is `dqa0`
(a 4 GB sparse file is plenty: the installed system uses about 2 GB), the CD
`dqb0`.

The CD can also sit beside the disk on the first channel (`disk0.1`,
`dqa1`; then `boot dqa1`). Until 2026-10-01 that failed: the installation
stopped at 10% with `%PCSI-E-WRITEERR ... -SYSTEM-F-CTRLERR, fatal
controller error` writing `[SYS$LDR]PROCESS_MANAGEMENT.EXE`, and a DCL
`COPY` from the CD to the disk failed the same way in 3 of 5 runs. The
controller raised a DMA write's completion interrupt before it had written
the disk and cleared BSY; OpenVMS's driver moved on to the CD on the same
channel, and the end of the disk command then landed on the CD, leaving the
disk busy for good. Fixed in the IDE core (`IdeController.cpp` since the CMD 649 joined it): a command's interrupt is
delivered only once the command is complete, and the controller finishes a
command on the drive it was issued to, whichever drive the guest selects
meanwhile. Since then the installation completes with the CD on `dqa1`, and
the copy passed 3 of 3 runs.

## SCSI controllers

Two of the SCSI adapters OpenVMS 8.4 has drivers for are emulated: the NCR
53C875 (`SYS$PKWDRIVER`, an IntraServer driver) and the QLogic ISP1040, the
KZPBA (`SYS$PKQDRIVER`). Disks on them are `DKA<id>00`, `DKB<id>00`, ...,
in the order the console lists the controllers. An example, beside the
system disk on IDE:

```
  pci0.2 = isp1040
  {
    disk0.1 = file { file = "s0.img"; read_only = false; }
  }
  pci1.1 = sym53c875
  {
    disk0.1 = file { file = "s1.img"; read_only = false; }
  }
```

Until 2026-10-02 either controller crashed the system during startup
(`INVEXCEPTN`, current image `SYSMAN`), on the ES40 and the DS20E, under the
interpreter and the JIT, with the native PALcode routines and the real
PALcode. Both were faults in the models, not in the platform code. What
the drivers needed (evidence in `lab/vscsi-evidence/`):

- *53C875, the expansion ROM.* The SDA crash points at
  `SYS$CPU_ROUTINES_2208+050A0`, an `LDL` through a null pointer. That is
  the worker of `IOC$READ_IO`, not a configuration-space read: PKWDRIVER's
  unit initialisation reads the last 4 KB of the adapter's flash through an
  I/O handle it maps only when the expansion ROM BAR sizes to something.
  The model had no ROM BAR, the handle stayed 0. The 825, 875, 895 and 896
  now have a 64 KB ROM, erased except for the identification block the
  driver checks in its last 4 KB (the maker's name, a checksum to
  0x012435c5, a version: `%PKA0, ... PKW V2.1.22 ROM V1.0`). Without the
  block the port stays offline ("ROM Checksum read error").
- *53C875, SCRIPTS that poll.* PKWDRIVER leaves its SCRIPTS in a loop that
  reads ISTAT until the driver sets SIGP. The model aborted any SCRIPTS
  program after 100000 instructions; the driver answered the abort with a
  chip and bus reset, and an `INITIALIZE` running at that moment failed
  with `MEDOFL`. Long runs are now paced instead (50 us every 4096
  instructions, and a SIGP write wakes the thread at once).
- *ISP1040, the RISC's memory.* Before it loads firmware, PKQDRIVER reads
  the header of the image already in the chip with READ RAM WORD -- its
  length, and where its copyright text ends -- and trusts both. The model
  answered 0, and the driver copied 0xeffb words into a 128-byte buffer on
  the kernel stack. The RISC's memory is now kept, and holds at power-on
  the header of an image naming the firmware version ABOUT FIRMWARE
  reports (4.65); PKQDRIVER finds its own 5.57 newer and loads it.
- *ISP1040, CHECK CONDITION.* A command that ended in CHECK CONDITION was
  reported as DATA UNDERRUN. Once the disk offered tagged queuing,
  PKQDRIVER took that for a controller fault, reset the adapter three times
  and shut the port down ("Port shutdown due to numerous or serious ctrlr
  errors"); `INITIALIZE` then failed with `MEDOFL` on the MODE SENSE of a
  page the disk lacks. It now completes with the SCSI status, and carries
  the sense data (automatic request sense, which every driver enables).
- *ISP1040, selection timeouts* completed with the code of NOP MESSAGE
  FAILED; each empty ID the driver probed counted as a port error (631 on
  `PKB0` after a boot, now 1).

Windows 2000 still copies a file on both (`test/tools/win_storage.sh`), and
the console still lists them and their disks (`show config`, `show dev`).

## Network

The DE500 (`EWA0`) and the DE600 (`EIA0`) both receive. With only MOP
enabled (`MC LANCP SET DEVICE EIA0/MOPDLL=ENABLE`), OpenVMS configures the
DE600's 8255x with Broadcast Disable set (configure byte 15 = 0xea) and
loads one multicast address, AB-00-00-01-00-00; the console's driver sets
the same bit. So the chip refuses broadcast frames, as the emulation does,
and the counters stay at 0 for a test that sends only broadcasts. Frames to
the station address and to that multicast address are counted ("Packets
received", "Unrecognized unicast/multicast destination" while no protocol
claims them). The DE500 has no such bit in use: its driver counts the
broadcasts as unrecognised multicast. `lab/vscsi-evidence/frame_tx3.py`
sends all three kinds.

"Unavailable station buffers" on `EIA0` counts the frames that arrived
while the receive unit was idle -- between the driver's initialisation at
boot and LANCP starting the device -- which the model counts as resource
errors.

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
| Interpreter, default (native PALcode routines) | 1 | 38 | 40 |

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
| ATI Radeon 7500 / 7000, ES1000 | 1002:5157, 5159, 515E | GH, server `DECW$SERVER_DDX_GH` | `radeon` (7500) -- the login box draws |
| ATI Mach64 GX / CX / CT | 1002:4758, 4358, 4354 | GQ | `mach64` (CT) -- not tried |
| S3 Trio32/64, DEC864 (Vision864) | 5333:8811, 88C0 | GQ | `s3` (Trio64) -- not tried |

The Permedia 2 needed two fixes to draw: `InFIFOSpace` now reads 256 (the
server waits for at least 50), and RasterizerMode's LimitsEnable is active
high (the server leaves it clear and never loads the limits).

The Radeon 7500 (`radeon`, its AGP board; the ES40's SRM names it
"Radeon 7500 AGP" and runs its BIOS) is driven by the "Radeon Server DDX
for OpenVMS" (HP, 2002-2008), which logs "Found AGP device" and
"Recognized Radeon 7500 AGP", sets 1024x768 at depth 24 and reports XAA-style acceleration: screen to
screen blits, solid and 8x8 mono pattern fills, CPU-to-screen colour
expansion, solid and dashed lines, its own bitmap writes and glyph
renderer. It does all of this through the command processor: it loads the
CP microcode, puts a ring buffer and indirect buffers in host memory behind
the card's AGP window (`MC_AGP_LOCATION`, `AGP_BASE`; the ES40 has no AGP,
so this is bus mastering through the Pchip's scatter-gather window), and
fills them with register writes (type-0 packets) and `CNTL_HOSTDATA_BLT`
packets carrying runs of glyphs. It sets the hardware cursor (mono, 64x64)
for its pointer. The login box, typing into it and its Help dialog draw
correctly; the trace of one session is in
`lab/platforms/radeon/p3-decw-trace.out`. On the ES47 (the card in the
AGP slot, `pci3.5`), booted from the CD, `SYSMAN IO AUTOCONFIGURE` finds
the card and tries to configure `GHA0`, but the CD's minimal system has no
`SYS$GHDRIVER.EXE`. An installed system has it: the DECwindows Motif kit
on the same CD installs `SYS$GHDRIVER.EXE` and
`DECW$SERVER_DDX_RADEON.EXE`, and on an ES47 installed from the CD the
server draws the same login box ("Welcome to ES47") once it has a
keyboard and a mouse -- on the EV7 machines, which have no 8042, those
are USB devices (`KBD0`, `MOU0`): see docs/platforms/marvel.md, M7b.

SRM does not use the Permedia 2 as its console (README, known limitations), so the
console stays on the serial port while DECwindows takes the card. With a
graphics card present the startup sets `WINDOW_SYSTEM` to 1
(`%DECW-I-BADVALUE`) on the first boot.

### Keyboard and mouse

The display server reads the PS/2 keyboard and mouse through `IKA0` and
`IMA0`, OpenVMS's drivers for the 8042 (`src/devices/isa/Keyboard.cpp`).
Keys typed through `ALPHABOX_KEYPIPE` reach the CDE login box -- the user
name appears in the field, Backspace and Shift work, Return moves on to the
password prompt -- and `ALPHABOX_AUTOMOUSE` moves the pointer. Frames:
`lab/vms84/kbd-*.png`.

What the drivers send (`ALPHABOX_TRACE_KBC=1`):

- The command byte is left as SRM set it, 0x03: translation off, both
  interrupts on. The keyboard therefore runs untranslated.
- Keyboard, three times within 30 s: `FF` (reset), `F5`
  (disable), `AB` and `AF`, `F0 03` (scan code set 3), `F8` (all keys make
  and break), `F4` (enable); then `ED 00` (LEDs) now and then. `AB` and `AF`
  are not keyboard commands; the keyboard answers Resend (`FE`) as a real
  one does, and the driver carries on.
- Mouse: `FF`; sample rates 200, 200, 80 and `F2` (the five-button probe:
  ID 0); 200, 100, 80 and `F2` (the wheel probe: ID 3); rate 100; `F4`.

Scan code set 3 gives each key a type (commands `F7`-`FD`): whether it
sends a break code on release. The keyboard now keeps these types, with the
power-on defaults -- only Caps Lock, both Shifts, left Ctrl, left Alt and
the Windows and Menu keys send break codes -- restored by `FF`, `F5` and
`F6`. OpenVMS's `F8` gives every key make and break, which is what the
emulated keyboard sent before for every key anyway. SRM selects set 3 too
but sends only `FC 39` and `FC 58` (right Alt and right Ctrl make/break), so
it now receives letters as make codes only, as from a real keyboard.
AlphaBIOS and Windows 2000 select set 2 (`F0 02`) and are not affected.
Key repeat comes from the host, so the typematic type is recorded and not
acted on.

An earlier run (`lab/vms84/gfx/run8-limits`) recorded keys typed with
`ALPHABOX_KEYPIPE` as not reaching the login box. That did not happen again:
three boots on the same commit, with and without the trace, typing 15 s
after `DECW$STARTUP` and minutes later, all showed the text in the box. The
driver traffic of that run (the `kbd:` lines in its log) is the same as in
the working ones. Why its frame showed an empty field is not known.

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
  reached; the login box itself takes keyboard and mouse input (see
  Keyboard and mouse).
- TCP/IP Services was installed but not configured or started.

## Known problems

- Fixed 2026-10-02: a 53C875 or an ISP1040 in the configuration crashed
  the system in startup (`INVEXCEPTN` in `SYSMAN`). See SCSI controllers.

- Fixed: on the interpreter build with the native OpenVMS PALcode routines
  (the default there), startup used to stop after `%STDRV-I-STARTUP`. The
  native `REI` lost the old processor mode when reading the exception frame
  took a DTB miss, saved the stack pointer of the mode being left into the
  PCB's kernel-stack slot and loaded a stale one for the mode being entered
  (see `docs/cpu-fidelity.md`). JIT builds were never affected: they always
  run the real PALcode, and `palcode.vms.nohle = true` does the same on the
  interpreter. `ALPHABOX_VMSPAL_OFF` (docs/headless.md) hands single native
  routines back to the PALcode; bisecting with it found this one.
