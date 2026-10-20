# Running and configuring Alphabox

```
alphabox configure                 # interactive generator, writes es40.cfg
alphabox run                       # start the machine (reads es40.cfg)
alphabox run my.cfg                # ... or another configuration file
alphabox run --check my.cfg        # check it without starting anything
alphabox run --dump-config my.cfg  # ... and print what it resolves to
alphabox --version                 # version, commit and compiled-in features
```

The sample [es40.cfg](../es40.cfg) documents every configuration value; the
generator covers the common ones. If a section contains a value that its
device doesn't use, Alphabox warns at startup (`%SYS-W-UNKNOWNCFG`).

## Syntax

A configuration is a list of statements. Each is a value or a section:

```
name = value;
name = class { statements }
```

- **Names and classes** are letters, digits, `_` and `.` (`pci0.7`,
  `memory.bits`), at most 255 characters.
- **Values** are such a word (`800M`, `true`, `disk.img`) or a quoted
  string. A value with any other character (a space, `/`, `:`, `-`)
  must be quoted: `file = "img/disk 1.img";`. A quoted string ends on its
  own line, and `""` in it stands for one `"`.
- Every value ends with `;`. A `;` on its own is allowed (after a `}`,
  say).
- **Comments**: `//` to the end of the line; `;` to the end of the line
  when it is the first thing on its line; `/* ... */` anywhere outside a
  quoted string, over several lines if need be.
- Sections nest at most 16 deep. A real machine needs five.
- A file may start with a UTF-8 byte order mark. Line ends may be LF or
  CRLF.
- A value named twice in one section keeps its first value; the later one
  is ignored. A section named twice makes two devices
  (`%SYS-W-DUPSECTION`).

Anything else is refused with the file, the line and the column, and what
was expected there:

```
Emulator Failure: Configuration error: es40.cfg:12:5: expected ';' after the value of lpt.outfile, found '}': ...
```

Errors found after parsing (an unknown class, a slot the board does not
have) give the place of their section, and `%SYS-W-UNKNOWNCFG` the place
of the value.

## Checking a configuration

`alphabox run --check my.cfg` reads the file and applies every rule the
emulator applies when it starts, but builds no device: it opens no image,
ROM or port. It checks:

- the syntax;
- each section's class, its name (`pci<hose>.<slot>`, `disk<bus>.<unit>`)
  and its parent;
- the board (`platform`): its slots, how many processors it takes and of
  which family;
- the values a device does not read (`%SYS-W-UNKNOWNCFG`), and the chip
  and model names of the graphics cards.

What only a device itself checks is left to the run: whether a disk's
`file` exists, or whether a size suits the device.

Each error ends its section and the check goes on with the next one, so
one run lists them all. Messages go to stderr and end with a count. The
exit status is 0 when there was no error and 1 otherwise; `alphabox run`
itself also exits with 1 when it stops on an error.

`alphabox run --dump-config my.cfg` does the same and writes the resolved
configuration to stdout, in the file's own syntax:

- every value is quoted;
- a value named twice in a section is commented out after the first,
  which is the one used;
- the sections the emulator adds by itself are marked `// added`: `fdc0`,
  the serial ports, a board's own devices, the ports of a multi-port
  board.

The dump is itself a valid configuration.

## Machines

`platform` in the machine block selects which machine is emulated. The
block is `tsunami` on every machine:

```
sys0 = tsunami {
  platform = "es40";
}
```

| `platform` | Machine | Chipset | Processors, at most | Console image (`rom.srm` default) | Notes |
| --- | --- | --- | --- | --- | --- |
| `es40` (the default) | AlphaServer ES40 | Tsunami | 4 | `cl67srmrom.exe` | below |
| `ds20e` | AlphaServer DS20E | Tsunami | 2 | `PC264SRM.ROM` | [ds20e.md](platforms/ds20e.md) |
| `ds20` | AlphaServer DS20 | Tsunami | 2 | `PC264SRM.ROM` | [ds20.md](platforms/ds20.md); the DS20E's console and slots |
| `ds10` | AlphaServer DS10 | Tsunami | 1 | `DS10SRM.ROM` | [ds10.md](platforms/ds10.md) |
| `ds20l` | AlphaServer DS20L | Tsunami | 2 | `DS20L_V6_6.EXE` | [ds20l.md](platforms/ds20l.md) |
| `es45` | AlphaServer ES45 (Model 2) | Titan | 4 | `ES45_V7_3.EXE` | [es45.md](platforms/es45.md) |
| `es45m1` | AlphaServer ES45 Model 1 | Titan | 4 | `ES45_V7_3.EXE` | its AGP slot is `pci2.5` |
| `ds25` | AlphaServer DS25 | Titan | 2 | `DS25_V7_3.EXE` | [ds25.md](platforms/ds25.md) |
| `ds15` | AlphaServer DS15 | Titan | 1 | `DS15_V7_3.EXE` | [ds15.md](platforms/ds15.md) |
| `es47` | AlphaServer ES47 | Marvel (EV7) | 2 | `SRM_V7_3.EXE` | [marvel.md](platforms/marvel.md); its AGP slot is `pci3.5` |
| `es80` | AlphaServer ES80 | Marvel (EV7) | 8 | `SRM_V7_3.EXE` | [marvel.md](platforms/marvel.md) |
| `gs1280` | AlphaServer GS1280 | Marvel (EV7) | 16 | `SRM_V7_3.EXE` | [marvel.md](platforms/marvel.md) |

All of them boot OpenVMS 8.4; [platforms.md](platforms.md) says how far
each one goes.

- **Processors**: `cpu0` to `cpu<n>` are `ev68cb` on the Tsunami and Titan
  machines, and `ev7` or `ev7z` on the EV7 machines. A board refuses
  processors of the other family.
- **Firmware**: each machine expects its own console, from the firmware
  media you own. Alphabox reads the update-bundle form, the raw form
  behind the standard Alpha ROM header, and the update utilities the
  DS20L, ES45, DS25 and DS15 consoles come as, which install the console
  into the flash.
- **Slots**: every board refuses a device in a place it has no slot for.
- **The EV7 machines**: their console terminal is the first serial port
  (`serial0`), and `rom.nvram` names the file the console's NVRAM is kept
  in (default `cmm_nvram.bin`).

The ES40's ten PCI slots are `pci0.1`-`pci0.4` and `pci1.1`-`pci1.6`. The
M1543C's functions sit at `pci0.7`, `pci0.15`, `pci0.17` and `pci0.19`.
Any other place is refused: Windows 2000's HAL routes no interrupt to
one, and a driver starting on `pci0.5` stops it with 0xA5. A bridge in a
slot adds more.

### Memory

`memory.bits` gives the memory as a power of two (30 = 1 GB), and the
chipset fills the board's memory arrays from it. On the EV7 machines it is
the memory of each processor, and the only way to give it.

On the Tsunami and Titan boards `memory.arrays` gives the arrays
themselves instead, in MB, array 0 first, 0 for an empty one:

```
  memory.arrays = "2048,1024,512,512";   // 4 GB in four arrays
  memory.arrays = "1024,0,1024";         // a DS15: arrays 0 and 2
```

- The memory is then the sum (it need not be a power of two), and
  `memory.bits` is not used.
- Each array is a power of two from 64 to 8192 MB, modelled as four
  identical DIMMs (eight of 1 GB for 8192).
- The largest array gets the lowest addresses and arrays of one size keep
  their order, as a real machine's serial ROM places them. So the
  console's `show memory` lists a 512 MB array 0 above a 1024 MB array 2.

A layout the board cannot hold is refused at startup:

- an array it has no slots for (the DS15 has arrays 0 and 2 only, the DS10
  and DS20L arrays 0 and 1);
- an array larger than its console reads (1024 MB on the DS20E, DS20,
  DS10 and DS20L);
- a sum outside the board's limits.

## Firmware and graphics BIOS

You need the machine's SRM console ROM image (`rom.srm`, above). The flash
and DPR NVRAM images (`rom.flash`, `rom.dpr`) start blank when missing and
are saved as they change and when the run ends.

The graphics cards run their real VGA BIOS, which you supply. Point the
section's `rom` value at the file:

| Card | Config section | Default `rom` | A BIOS that works |
| --- | --- | --- | --- |
| S3 Trio64 | `s3` | `vgabios.bin` | `video/s3/86c764x1.bin` from the [86Box ROM set](https://github.com/86Box/roms) |
| Cirrus CL-GD5434 | `cirrus` | `gd5434.bin` | `video/cirruslogic/gd5434.BIN` from the 86Box ROM set |
| Cirrus CL-GD5430 | `cirrus` with `chip = "gd5430";` | `gd5430.bin` | `video/cirruslogic/pci.bin` from the 86Box ROM set |
| ATI Mach64 family | `mach64` with `chip` | by chip (`mach64ct.bin` for the CT) | for the CT and VT2, `video/mach64/` in the 86Box ROM set |
| 3Dlabs Permedia 2 | `permedia2` | `SYN80700.PAN` | ELSA GLoria Synergy 8.07.00 |
| S3 ViRGE family | `s3virge` with `chip` | by chip (`86c375_4.bin` for the DX) | `video/s3virge/` in the 86Box ROM set |
| DEC TGA and TGA2 | `tga` with `model` | none: the card has no BIOS | |
| ATI Radeon 7500 | `radeon` | `radeon7500.rom` | the RV200 BIOS of 2002/04/16 |

The sample `es40.cfg` names every chip's default file. Use one VGA card
per machine; a TGA may sit beside it. With `vga_console = true` in the
`ali` section, the SRM console appears on the graphics window instead of
a serial port.

### ARC / AlphaBIOS and Windows NT

The Windows NT family boots from the ARC (AlphaBIOS) console:

1. Flash AlphaBIOS from the Alpha Systems Firmware v7.3 CD.
2. Enter `arc` at the SRM prompt.

Windows 2000 has been booted this way on both the S3 and the Cirrus
GD5434; with the Cirrus card it installs its own Cirrus driver.

## Stopping

Ctrl-C or SIGTERM ends a run gracefully, saving the flash and DPR images.
With `exit_on_pal_halt = true` in the `sys0` section, a guest halt (an OS
shutting down to the console) ends the run the same way, which is handy for
scripted use.

## Debugging the guest

`gdb = <port>;` in the `sys0` section (or `ALPHABOX_GDB=<port>`) starts a
gdb stub on 127.0.0.1, and `gdb.wait = true;` holds the processors until
gdb attaches. [debugging.md](debugging.md) describes it.

## Serial consoles

The sample configuration attaches both UARTs as `null_attach`: present but
unconnected. The emulator then starts without waiting for anything, and the
console lives on the VGA window (`vga_console = true`).

To use a telnet console instead, set `port = 21264;` in a serial section.
The emulator then **waits at startup** until a client connects
(`nc localhost 21264`). Ports left out of the configuration are added as
`null_attach` automatically.

Sending a telnet BREAK opens a small menu: continue, exit, abort, save the
machine state to `autosave.axp`, or load it back.

## Networking

Three NIC families are available, each in any free PCI slot:

- `dec21143`: the DEC 21143 (Tulip, DE500-BA); the console calls it `ewa0`.
  Its older relatives are `dec21140` (10/100, media chosen through the
  general purpose port), `dec21041` and `dec21040` (10 Mb); the console
  names those by their chip and boots over any of them.
- `de600`: the DE600-AA, an Intel 82559 board, which the console calls
  `eia0` and shows by name. `i82557`, `i82558` and `i82559` are Intel's
  own PRO/100 boards with those controllers.
- `i82559er`: the 82559ER, the controller on the AlphaServer DS25's board.
  That machine has one at `pci0.8` whether the file names it or not
  (unconnected, `type = "null"`, when it does not).
- `virtio_net`: a paravirtual NIC, for a guest with a driver for it
  ([virtio.md](virtio.md)).

Each connects to the host through one of five back ends, selected with
`type`:

- `type = "pcap"` (default): captures on an existing host interface. Set
  `adapter = "eth0";` (Linux) or the `\Device\NPF_{...}` name
  (Windows/Npcap). On Linux the binary needs capture permission (see
  [building.md](building.md#linux)); without it, startup fails with "Error
  opening adapter". Don't leave `adapter` unset on unattended runs: the
  emulator then asks interactively.
- `type = "tap"` (Linux only): uses a TUN/TAP device, so the host can reach
  the guest and it can be bridged onto the LAN. Options:
  - `adapter = "tap0";`: the device, created if needed (needs
    CAP_NET_ADMIN or root);
  - `tap_create = true;`;
  - `host_ip = "10.0.0.1/24";`;
  - `bridge = "br0";`;
  - `uplink = "eno1";`.
- `type = "udp"`: a point-to-point link to another program, one Ethernet
  frame per UDP datagram (the framing of QEMU's `-netdev dgram`). Set
  `udp_local = "127.0.0.1:5555";` (where the NIC listens) and
  `udp_remote = "127.0.0.1:5556";` (where its frames go). The peer can be a
  second Alphabox, a QEMU guest, or `test/tools/net_peer.py`, which answers
  ARP, BOOTP and TFTP so the console can network-boot. Needs no privileges.
- `type = "loopback"`: a loopback plug on the port. Every frame the
  controller transmits comes back on its own receiver; the link is up and
  nothing reaches the host. Needs no privileges. With it the SRM console's
  wire-level loopback tests pass, which need the connector on a real
  machine too: `nettest ewa0 -mode ex`, `-mode nc`, and on the Intel NICs
  plain `nettest eia0` (its driver sends the default and the external
  test to the wire). The controllers' own internal loopback does not need
  it.
- `type = "null"`: the NIC is present but nothing is ever received and
  transmissions are discarded. Needs no privileges; useful for tests.

Every NIC takes `mac` (default `08-00-2B-E5-40-<n>`, `n` counting the NICs
in the machine). The Tulips also take `queue` (receive queue depth,
default 1024), `crc` and `trace_packets`.

## PCI-PCI bridges and multi-port boards

A bridge takes a PCI slot and opens a second bus behind it; the devices on
that bus go inside the bridge's block, named `pci.<device>` (0-15):

```
pci0.3 = dec21152 {
  pci.0 = sym53c875 { disk0.0 = file { file = "disk.img"; } }
  pci.1 = de600 { type = "null"; }
}
```

The bridge classes are the ones the ES40 console names: `dec21050`,
`dec21052`, `dec21152`, `dec21153` and `dec21154`. Bridges nest. The
console numbers the buses (the first bridge's bus is 2; bus 1 is ISA) and
shows the devices as `eia0.0.0.2001.0` and so on; interrupts reach the
bridge's slot with the usual rotation by device number.

The dual-port DE602 boards are bridge classes too: `de602` (DE602-AA:
a 21152 and two 82558 ports) and `de602b` (DE602-B*: an Intel 21154 and two
82559 ports). Their ports are `pci.0` and `pci.1` of class `de602_port` or
`de602b_port`, which take the NIC options above; a port left out is added
unconnected (`type = "null"`).

## Sound

`pci1.1 = es1371 {}` adds an Ensoniq AudioPCI 97 (SDL builds only),
and `es1370` the part before it. Neither takes a configuration value.

The ES1371 is the one to reach for:

- it is the only sound card the ES40 console names in `show config`
  ("Ensoniq Sound Card");
- it carries an AC'97 codec instead of the ES1370's own mixer;
- it is what the Windows 2000 and Windows NT 4 drivers were written for.

Windows 2000 on Alpha needs a hand to install it. The `wdma_ens.inf` on
the Alpha media decorates its install sections `.NTX86`, so setup falls
through to the Windows 95 section and refuses the driver, although
`es1371mp.sys` in `driver.cab` is an Alpha binary. A copy of that INF with
the sections renamed `.NT` installs and plays.

The `es1370` is kept for Windows NT 4 and for anything written against
that part; other guests ignore it.

## Keyboard, mouse and window

- **Mouse**: click the window to grab it, Ctrl+F10 to release.
  `mouse.speed`, `mouse.invert_x` and `mouse.invert_y` tune it.
- **USB tablet and keyboard**: for guests with a USB HID stack (Windows
  2000 and later), `port1 = "tablet";` on `ali_usb` gives an absolute
  pointer instead: the guest cursor follows the host's, with no grab at
  all. `"keyboard"` gives a USB keyboard, the only keyboard of the EV7
  machines. See [USB](usb.md), which also covers USB disks and host
  passthrough.
- **Window scaling**: `video.scale_ratio` and `video.scale_change_enable`.
- **Hotkeys**: every GUI shortcut can be rebound with `hotkey.*` in the `sdl`
  section, e.g. `hotkey.ctrl_alt_delete = "GUI+Shift+D";` on a Mac keyboard
  without an End key. The active bindings are printed at startup
  (`%SDL-I-HOTKEYS`) and shown in the window title. Defaults:

  | Hotkey | Action |
  |---|---|
  | Ctrl+F10 | Grab or release the mouse |
  | Ctrl+F11 | Change the CD (file picker) |
  | Ctrl+Shift+F11 | Change the CD even if the guest has locked the drive |
  | Ctrl+Alt+End | Send Ctrl+Alt+Delete to the guest |
  | Ctrl+Alt+Home | Reset the window size |
  | Ctrl+PageUp | Scale the window up (with `video.scale_change_enable = true`) |
  | Ctrl+PageDown | Scale the window down (likewise) |

### Mouse on WSLg / Wayland

On WSLg, the default Wayland backend delivers **no relative mouse motion**
while the mouse is grabbed: the grab succeeds but the guest pointer never
moves. Run through XWayland instead:

```
SDL_VIDEO_DRIVER=x11 DISPLAY=:0 SDL_RENDER_DRIVER=software alphabox run
```

`SDL_RENDER_DRIVER=software` avoids a fatal GLX error under WSLg's
XWayland.

To diagnose, set `ALPHABOX_MOUSE_DEBUG=1`:

- Motion lines with `grab=1` mean host input reaches the guest.
- No motion lines after a `grab -> 1` line mean the host backend isn't
  delivering relative motion.

## Disks, CDs and floppies

### Controllers

Disk images are raw image files (`file`), host devices (`device`) or RAM
disks (`ramdisk`), on:

- SCSI: `sym53c810`, `sym53c825`, `sym53c875`, `sym53c895`, `sym53c896`,
  and the QLogic `isp1020`, `isp1040`, `isp1080` and `isp1240`;
- IDE: `ali_ide` (the M1543C's) and `cmd649` (the CMD 649 PCI IDE, the
  ES47's on-board one, both channels native);
- `virtio_blk`, a paravirtual disk ([virtio.md](virtio.md)), one disk,
  `disk0.0`;
- USB: `disk<port>.0` on `ali_usb`, `ehci` or `uss344` ([usb.md](usb.md));
- the floppy controller.

Some controllers have two buses:

- The `sym53c896` has two channels, which appear as two PCI functions of
  the one device: disks named `disk0.<target>` hang on the first,
  `disk1.<target>` on the second.
- The `isp1240` also has two SCSI buses, but on one PCI function, and
  disks name them the same way.

The ES40 console knows the `isp1020` and `isp1040` and boots from them. It
has no entry for the `isp1080` or the `isp1240`, which only a guest
operating system can use.

### Serial numbers

A drive's `serial_number` (alias `serial_num`) is what the guest reads:

- from ATA IDENTIFY (`ali_ide`, `cmd649`);
- from `virtio_blk`'s GET_ID;
- from a SCSI disk or CD's INQUIRY (the unit serial number page, 0x80).

Without one, every drive gets its own: `ES40EM`, the controller's part,
the bus (one digit) and the unit (two digits).

| Controller | Its part | Bus, unit | Example |
| --- | --- | --- | --- |
| `ali_ide` | `00`, always | channel, drive | `disk1.1`: `ES40EM00101` |
| `cmd649`, `virtio_blk` | two digits, counted from `01` in configuration order (from `00` when there is no `ali_ide`) | channel, drive (`virtio_blk`: `000`) | the first `cmd649`'s `disk1.0`: `ES40EM01100` |
| `sym53c8xx`, `isp10x0`, `isp1240` | `S` and the adapter's PCI place: hose and slot, two digits each | SCSI bus (the 53C896's channel, the ISP1240's bus), SCSI id | `pci0.3`'s `disk0.5`: `ES40EMS0003005` |
| a SCSI adapter behind bridges | as above, then `B` and the slot on each bridge's bus | as above | `pci.2` behind a bridge in `pci0.3`, `disk0.0`: `ES40EMS0003B02000` |
| USB disks (`ali_usb`, `ehci`, `uss344`) | none: the device's own | none | `0000A1FA0001`, ... ([USB](usb.md)) |

More examples: the `ali_ide`'s `disk0.0` is `ES40EM00000` and its
`disk1.0` `ES40EM00100`; `pci1.2`'s SCSI `disk1.12` is `ES40EMS0102112`.
A USB disk's INQUIRY and iSerialNumber say the same.

Why it is built this way:

- The `ali_ide`'s `disk0.0` is `ES40EM00000`, the serial every IDE drive
  had before, so an installed guest's boot disk is unchanged.
- A SCSI adapter's part is where it sits, not a count, so adding, removing
  or reordering controllers never changes another disk's serial. The
  letter keeps it apart from the counted controllers.
- Windows 2000 stops with 0xCA (duplicate device) when two IDE drives
  report one serial (seen on one channel and across two controllers), so
  give explicit serials distinct values.

Until 2026-10-05, SCSI disks reported `SRL` and their SCSI id twice
(`SRL0000`, `SRL0101`, ...), the same on every adapter and for every USB
disk. A guest that keeps a disk's identity by that serial gets the old one
back with `serial_number = "SRL0000";`. None of Windows 2000, the 64-bit
Whistler beta or OpenVMS 8.4 reads the page for a parallel SCSI disk.

The vital product data pages a SCSI disk has are 0x00 and 0x80; any other
(0x83, device identification) is refused with ILLEGAL REQUEST.

### CDs and floppies

- **CD images**: a cdrom `file` ending in `.cue` is read as a BIN/CUE image
  (multi-file, MODE1/MODE2/audio tracks); anything else is a flat ISO. CD
  drives are read-only unless `read_only = false`.
- **Empty drives**: a CD or floppy drive with no `file` (or an unreadable
  one) starts empty.
- **Changing the CD**: **Ctrl+F11** opens a file picker and inserts the
  chosen image into the first CD drive.
  - The image is opened and checked immediately. A bad file is reported and
    the current disc stays.
  - The swap happens between guest commands, and the guest sees a normal
    "medium changed" notification.
  - A drive the guest has locked (PREVENT MEDIUM REMOVAL) refuses the
    change; **Ctrl+Shift+F11** forces it.
- **Tray**: guests may open and close the tray themselves unless
  `allow_guest_eject = false`.

## Guest installation guides

- [OpenVMS 8.4 on Alphabox](openvms.md): configuration, install answers,
  what was verified, licences
- [OpenVMS](https://github.com/lenticularis39/axpbox/wiki/OpenVMS-installation-guide)
  (upstream wiki)
- [OpenVMS CDE desktop](https://github.com/lenticularis39/axpbox/wiki/GUI-Desktop-Environment-(CDE))
  (upstream wiki)
- [NetBSD](https://github.com/lenticularis39/axpbox/wiki/NetBSD-9.2-install-guide)
  (upstream wiki)
- [Windows 2000 / NT](https://web.archive.org/web/20260705122517/https://www.zx.net.nz/computers/dec/axpemu-es40.shtml)
  (zx.net.nz, archived)
- [Guest support status](https://github.com/lenticularis39/axpbox/wiki/Guest-support)
  (upstream wiki)
