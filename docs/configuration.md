# Running and configuring Alphabox

```
alphabox configure     # interactive generator, writes es40.cfg
alphabox run           # start the machine (reads es40.cfg)
alphabox run my.cfg    # ... or another configuration file
alphabox --version     # version, commit and compiled-in features
```

The sample [es40.cfg](../es40.cfg) documents every configuration value; the
generator covers the common ones. If a section contains a value that its
device doesn't use, Alphabox warns at startup (`%SYS-W-UNKNOWNCFG`).

## Firmware

You need an SRM console ROM image (`rom.srm`). The flash and DPR NVRAM
images (`rom.flash`, `rom.dpr`) start blank when missing and are saved as
they change and when the run ends.

The graphics cards run their real VGA BIOS, which you supply:

| Card | Config section | BIOS |
|---|---|---|
| S3 Trio64 | `pci0.2 = s3` | `86c764x1.bin` from the [86Box ROM set](https://github.com/86Box/roms/tree/master/video/s3) |
| Cirrus CL-GD5434 | `pci0.2 = cirrus` | `video/cirruslogic/gd5434.BIN` from the 86Box ROM set |
| Cirrus CL-GD5430 | `pci0.2 = cirrus` with `chip = "gd5430";` | `video/cirruslogic/pci.bin` from the 86Box ROM set |

Point the section's `rom` value at the file. Use one graphics card per
machine. With `vga_console = true` in the `ali` section, the SRM console
appears on the graphics window instead of a serial port.

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

## Machines

`platform` in the machine block selects which machine is emulated:

```
sys0 = tsunami {
  platform = "es40";
}
```

- `es40` (the default): the AlphaServer ES40.
- `ds20e`: the AlphaServer DS20E, under construction.
- `ds10`: the AlphaServer DS10, started; its console does not reach the
  prompt yet. Its console runs and
  lists its configuration, but the board's own hardware is not modelled yet;
  [platforms.md](platforms.md) and its
  [work packet](platforms/ds20e.md) say what is missing.

A machine expects its own console firmware, named with `rom.srm` (each
machine's default is its usual file name). Images come from the firmware
media you own; Alphabox reads both the update-bundle form and the raw form
behind the standard Alpha ROM header.

## Networking

Two NIC families are available, each in any free PCI slot:

- `dec21143`: the DEC 21143 (Tulip, DE500-BA); the console calls it `ewa0`.
- `de600`: the DE600-AA, an Intel 82559 board, which the console calls
  `eia0` and shows by name; `i82557`, `i82558` and `i82559` are Intel's
  own PRO/100 boards with those controllers.

Either connects to the host through one of four backends, selected with
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
- `type = "null"`: the NIC is present but nothing is ever received and
  transmissions are discarded. Needs no privileges; useful for tests.

Every NIC takes `mac` (default `08-00-2B-E5-40-<n>`, `n` counting the NICs
in the machine). The `dec21143` also takes `queue` (receive queue depth,
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

`pci1.1 = es1370 {}` adds an Ensoniq AudioPCI ES1370 (SDL builds only).
Guest drivers exist for Windows NT 4; other guests ignore it.

## Keyboard, mouse and window

- **Mouse**: click the window to grab it, Ctrl+F10 to release.
  `mouse.speed`, `mouse.invert_x` and `mouse.invert_y` tune it.
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

- **Disk images**: raw image files (`file`), host devices (`device`) and RAM
  disks (`ramdisk`), on SCSI (`sym53c810`, `sym53c825`, `sym53c875`,
  `sym53c895`), IDE (`ali_ide`) or
  the floppy controller.
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
