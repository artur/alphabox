# Headless operation and debug hooks

For automated or headless use (CI, scripted firmware navigation, driving the
emulator over SSH), the emulator reads a set of environment variables at
startup. Each one is off unless set. This file lists every `ALPHABOX_*`
variable the binary reads.

| Section | What is in it |
| --- | --- |
| [Running without a display](#running-without-a-display) | `SDL_VIDEO_DRIVER`, a fully headless run |
| [Screen and keyboard](#screen-and-keyboard) | frame dumps, scripted keys, the key names and other tokens |
| [Mouse](#mouse) | synthetic PS/2 motion, mouse tracing |
| [Snapshots](#snapshots) | saving and resuming a running machine |
| [Self-tests](#self-tests) | virtio, Radeon and EHCI checked without a guest driver |
| [USB](#usb), [Storage](#storage), [Video](#video) | per-device switches and traces |
| [Guest diagnostics](#guest-diagnostics) | where a guest is stuck, interrupts, [the gdb stub](#the-gdb-stub), arithmetic traps, the GUI lock |
| [Timing and idle](#timing-and-idle) | idle pacing, delay loops, the rate report |
| [JIT builds](#jit-builds) | the JIT's switches, self-tests and traces |
| [Bring-up traces](#bring-up-traces) | what a firmware asked for and did not get; [a processor row on another board](#processor-rows) |
| [Hypervisor runtime](#hypervisor-runtime) | the `-DALPHABOX_HVF=ON` switches |

## Running without a display

To run the whole GUI stack without a window or a display server, set
`SDL_VIDEO_DRIVER`:

- `dummy` works on every host tried, macOS included.
- `offscreen` works on Linux, but on macOS it fails to create the window
  ("Could not initialize OpenGL / GLES library") and nothing renders.

A fully headless firmware run:

```
SDL_VIDEO_DRIVER=dummy ALPHABOX_DUMP_FB=fb \
ALPHABOX_KEYPIPE=keys.txt ALPHABOX_KEYSCRIPT="40:a,41:r,42:c,43:enter" \
alphabox run &
# watch fb-*.ppm to see the screen; echo keys >> keys.txt to react
```

A `serial` section with a `port` waits for a telnet connection before the GUI
comes up. For unattended runs, connect a client or use
`null_attach = true`.

## Screen and keyboard

| Variable | Effect |
| --- | --- |
| `ALPHABOX_DUMP_FB=<prefix>` | Write the emulated screen as a PPM image (`<prefix>-NNN-WxH.ppm`) every ~2 seconds. |
| `ALPHABOX_KEYSCRIPT="<sec>:<key>,..."` | Press named keys at fixed second offsets from GUI start. |
| `ALPHABOX_KEYPIPE=<file>` | Type the keys appended to `<file>` while the emulator runs, one token per ~120 ms. |
| `ALPHABOX_AUTOKEY_ENTER=<sec>` | Press Enter every `<sec>` seconds (a blunt tool for firmware "press any key" prompts). |

`ALPHABOX_DUMP_FB` is how you "see" the VGA output on a headless run;
`test/tools/ppm2png.py` converts a dump to PNG.

Whenever the guest changes the PS/2 keyboard's LEDs, the console gets a
`%KBC-I-LEDS: Num Lock on, Caps Lock off, Scroll Lock off` line, and the
window title shows the ones that are on (`[Num Caps]`). A script that
types through `ALPHABOX_KEYPIPE` can tell from it whether Caps Lock is on.

`ALPHABOX_KEYSCRIPT="40:a,41:r,42:c,43:enter"` types `arc` and Enter at the
SRM prompt 40 s in. `ALPHABOX_KEYPIPE` is the interactive variant:
`echo "f2 down down enter" >> keys.txt`. Start with an empty file; the
emulator remembers how far it has read.

### Key names

These names work for `ALPHABOX_KEYSCRIPT` and `ALPHABOX_KEYPIPE`:

- letters and digits: `a`–`z`, `0`–`9`;
- editing and navigation: `enter`, `esc`, `tab`, `space`, `bksp`, `del`,
  `ins`, `home`, `end`, `pgup`, `pgdn`, `up`, `down`, `left`, `right`;
- punctuation: `bslash`, `dot`, `minus`, `equals`, `comma`, `slash`,
  `semicolon`, `quote`, `lbracket`, `rbracket`, `grave`;
- function keys: `f1`–`f12`;
- modifiers and system keys: `ctrl`, `shift`, `alt`, `win`, `menu`;
- locks: `capslock`, `numlock`, `scrolllock`.

For a chord, prefix a key with modifiers joined by `-`: `win-r`, `shift-5`
(`%` on a US layout), `ctrl-alt-del`. `test/tools/keys_for.py "<text>"` turns
a line of text into tokens for a US keyboard layout.

### Other tokens

Besides keys, the two variables take these tokens:

- **`dbg-dump`** (`KEYPIPE` only, JIT builds): every processor prints its
  state at its next dispatch batch, as `%DBG-I-DUMP`/`CODE`/`STACK` lines.
  That is the PC, SDE, mode, ASN, PAL base, `EXC_ADDR`, `EXC_SUM`, `VA`,
  `MM_STAT`, both register banks, the code around the PC (read physically
  when the PC is in PALcode) and 512 bytes of stack. It is for looking at
  a guest that has hung without changing how it runs until then. A run
  without a GUI, which has no key pipe, gets the same dump from
  `kill -USR2 <pid>`.
- **`tablet:X:Y[:B]`** puts the USB tablet's pointer (`port1 = "tablet";`
  on `ali_usb`) at X,Y, with buttons B (bit 0 left, 1 right, 2 middle).
  X and Y are fractions of the screen when at most 1, else guest pixels.
- **`sc:<hex>[:<mods>]`** presses and releases a host key through SDL's own
  event queue, so the whole host chain runs: the map from the SDL scancode to
  the guest key, the hotkeys and the release bookkeeping, where the named keys
  enter at the guest. `<hex>` is the SDL scancode, which is the USB HID
  keyboard usage (`48` Pause, `46` Print Screen, `65` the context-menu key,
  `64` the 102nd key left of Z, `32` Non-US `#`); `<mods>` are modifiers
  held for the event, as letters: `c` Ctrl, `a` Alt, `s` Shift, `g` GUI; `r`
  sends only the auto-repeat press of a key held down.
  `sc:48:c` is Ctrl+Pause (Break), `sc:46:a` is Alt+Print Screen (SysRq).
  Pair it with `ALPHABOX_TRACE_KEYS=1`.
- **`hmotion:DX:DY`, `hbutton:N:1|0`, `hwheel:Y`** put a host mouse event in
  SDL's queue (relative motion in host pixels; SDL button N, 1 left, 2 middle,
  3 right, 4 and 5 the side buttons, pressed or released; wheel notches,
  fractions allowed), so the grab, the speed and invert settings, button
  bookkeeping and wheel accumulation run, where `mouse:` enters at the guest.
  The guest gets them only while the mouse is grabbed (`sc:43:c`, Ctrl+F10,
  toggles that); `ALPHABOX_TRACE_KEYS=1` reports what reached it as
  `%GUI-I-MOUSE`.
- **`mouse:DX:DY[:B[:Z]]`** moves the PS/2 mouse by DX, DY counts (DY
  positive is down the screen) with buttons B held (bit 0 left, 1 right,
  2 middle, 3 and 4 the fourth and fifth buttons), and turns the wheel by
  Z notches (positive away from the user). B is the state the buttons are
  left in, so a click is two tokens: `mouse:0:0:1 mouse:0:0:0`. The moves
  are relative and the guest applies its own acceleration, so to reach a
  place, park the pointer in a corner with a large move first
  (`mouse:-3000:3000` is the bottom left corner) and move from there in
  small steps. A large move reaches the guest over several packets (at
  most 255 counts each, 50 a second): give it that long before the next
  token, or a click lands on the way and a small move is lost against
  the screen edge. Windows 2000's Start button is `mouse:12:-8` from that
  corner; a click on the corner itself does not open it. As with a real
  mouse, nothing arrives until the guest's driver has
  enabled the mouse, and the wheel and buttons 4 and 5 only once it has
  switched the mouse to a mode that has them.
- **`usb:<op>:<port>[:<ep>[:<n>]]`** injects a fault on a USB root hub port
  (the `ehci` card's if there is one, else `ali_usb`'s), for testing a
  guest's USB driver. `test/tools/usb_bench.sh` fires them with
  `FAULTS=`/`PRE_FAULTS=`.

The `usb:` operations:

| Token | Fault |
| --- | --- |
| `usb:detach:<p>`, `usb:attach:<p>` | Unplug and replug the device, with the connect change and interrupt hardware gives. |
| `usb:stall:<p>:<ep>[:<n>]` | Endpoint `<ep>` answers STALL until the host sends CLEAR_FEATURE(ENDPOINT_HALT), and halts again on its next transfer, `n` times in all (default 1). |
| `usb:phase:<p>` | The USB disk's next command ends with a phase-error CSW (status 2). |
| `usb:nak:<p>:<ep>:<ms>` | The endpoint NAKs for `<ms>` milliseconds. |

`<ep>` is an endpoint address: `0x81` is IN 1, `0x02` is OUT 2.

## Mouse

| Variable | Effect |
| --- | --- |
| `ALPHABOX_AUTOMOUSE=<sec>` | From `<sec>` seconds in, inject synthetic PS/2 mouse motion (a square pattern plus a periodic left click), bypassing host input. |
| `ALPHABOX_MOUSE_DEBUG=1` | Trace host mouse motion, grab and focus transitions, relative-mode failures, the guest's aux commands and dropped mouse bytes (`MOUSEDBG` lines). |

As with a real PS/2 mouse, `ALPHABOX_AUTOMOUSE` sends nothing until the
guest driver enables data reporting. For moves and clicks at chosen times,
use the `mouse:` token in `ALPHABOX_KEYSCRIPT` or `ALPHABOX_KEYPIPE`
([Other tokens](#other-tokens)).

## Snapshots

`SIGUSR1` saves the whole machine at the next safe point, with every
thread stopped, as the serial console's `<BREAK>` menu does.

| Variable | Effect |
| --- | --- |
| `ALPHABOX_SNAPSHOT=<file>` | Where `SIGUSR1` saves (default `autosave.axp`). |
| `ALPHABOX_SNAPSHOT_EXIT=1` | Exit right after saving, so the disk images on exit are the ones the snapshot saw. |
| `ALPHABOX_RESTORE=<file>` | Resume a saved machine instead of cold-booting. |
| `ALPHABOX_RESTORE_SAVE=<file>` | A test hook, with `ALPHABOX_RESTORE`: save the restored machine to `<file>` before anything runs, and exit. |
| `ALPHABOX_SNAPSHOT_MAP=<file>` | A test hook: each save writes to `<file>` where each part of the state file begins (the system and chipset parts, then every component with its name and class). |

A state file is version 2.2: the memory image of the whole array (zero runs
compressed), then the system, chipset and component parts. On a board whose
memory has holes (the EV7 boards: each processor's memory at its PID's base,
16 GB apart) it is version 2.3: a region count, then each region's base and
length (64 bits each) and its image in the same encoding. Every board reads
both; a 2.3 region outside the configured memory is refused.

The disk images in the working directory must be the ones the snapshot
was taken against. `test/tools/nt_snap.sh` uses the first three to skip a
Windows boot.

The two test hooks are for `test/tools/snap_roundtrip.sh`
([development.md](development.md#test-tools)): a machine saved, restored and
saved again must give the same file, and the map names the component where
two files differ. A map line is `<offset> <name> <class>`; the memory image
runs from offset 8 to the first line.

`SIGTERM` (or Ctrl-C) stops the emulator gracefully: threads stopped,
flash and DPR saved.

## Self-tests

Three devices can be checked from inside the emulator, without a guest
driver. Each prints its own lines and PASS or FAIL.

### virtio (`ALPHABOX_VIRTIO_SELFTEST=1`)

At start-up, a minimal driver drives every `virtio_blk` and `virtio_net`
device through its legacy registers ([virtio.md](virtio.md)). Its rings
are 1 MB per device, 16-32 MB below the top of guest RAM, addressed
physically. It checks:

- reset, features and queues;
- blk: GET_ID, sector 0 (must end in 55 AA), a write of the last sector
  read back and restored, FLUSH, the error statuses;
- net: transmit and receive. This needs a back end that returns frames:
  UDP with `udp_local` = `udp_remote`.

It prints `%VIRTIO-I-SELFTEST` lines. `ALPHABOX_VIRTIO_SELFTEST_DELAY=<s>`
runs it `<s>` seconds later, through the DMA window the console set up.

`ALPHABOX_VIRTIO_TRACE=1`, with or without the self-test, traces virtio
register accesses, queue addresses, notifies and interrupt changes
(`VIRTIO` lines).

### Radeon (`ALPHABOX_RADEON_SELFTEST=1`)

At the end of the `radeon` card's start-up, before the machine runs, it
drives the 2D engine, the command processor, the cursor and the 3D engine
through the MMIO BAR and the CP ring. Each result is compared with a
software reference (91 checks, [radeon.md](radeon.md)), and the card's
registers and VRAM are put back afterwards.

- It prints `%RADEON-I-SELFTEST` lines.
- `=exit` ends the emulator afterwards, with status 0 when every check
  passed.
- The 3D scenes are written as PNGs (the frame, and a
  frame/reference/difference comparison) to
  `ALPHABOX_RADEON_SELFTEST_DIR`, default `$ALPHABOX_WORK/radeon-3d`.

### EHCI (`ALPHABOX_EHCI_SELFTEST=1`)

At start-up, before any firmware runs, a minimal EHCI driver drives the
`ehci` card, with its structures in the top 16 MB of guest RAM. In order:

1. Port reset, enumeration, SET_ADDRESS and SET_CONFIGURATION.
2. A Bulk-Only READ(10) of sector 0 from the USB disk on port 1, which
   must end in 55 AA.
3. A phase error and the host's reset recovery (class reset,
   CLEAR_FEATURE on both bulk endpoints), then a command that must
   succeed; and a class reset in a command's data stage.
4. With companions (the default), the port routing: CONFIGFLAG 0 and 1,
   and a full-speed device (the tablet, or a probe plugged into a free
   port) left disabled by the EHCI's port reset, handed to its companion
   with PORT_OWNER, and its device descriptor read from there.
5. High-speed isochronous iTDs against a loopback device plugged into a
   free port: OUT transactions read back IN with their data and lengths,
   IOC, and a transaction error for a missing device.

It prints `%EHCI-I-SELFTEST` lines.

## USB

| Variable | Effect |
| --- | --- |
| `ALPHABOX_USB_ASYNC_US=<us>` | USB storage and the USB speaker answer like devices behind libusb, `<us>` microseconds late. |
| `ALPHABOX_USBAUDIO_WAV=<file>` | The USB speaker (`port<n> = "audio";`) writes the PCM stream the guest sends it into `<file>` as a WAV. |
| `ALPHABOX_OHCI_SELFTEST=1` | Drive the ALi function's OHCI from a thread of its own (`%OHCI-I-SELFTEST` lines, then PASS or FAIL). Needs `port1 = "tablet";` on `ali_usb`. |
| `ALPHABOX_USBTRACE=1` | Trace the OHCI and the USB devices (`USBT` lines). |
| `ALPHABOX_USBTRACE=2` | As `1`, and every TD the OHCI retires and every done-queue write-back. |
| `ALPHABOX_OHCI_EARLY_CTL=1` | Let a done queue holding a control-endpoint TD go back at once. For A/B runs. |

`ALPHABOX_USB_ASYNC_US` tests and times the controller's passthrough path
without a host device (`test/tools/usb_bench.sh`, `test/tools/usb_audio.sh`):

- Bulk and interrupt transfers finish on another thread after `<us>`
  microseconds, NAKed until then.
- Isochronous OUT packets reach the device `<us>` later. A full queue of
  64 gives the controller's underrun status.
- An isochronous IN endpoint is streamed from the device, each packet
  ready `<us>` after it was fetched. None ready gives the controller's
  overrun status.
- The EHCI self-test's isochronous loopback goes behind it too.

`ALPHABOX_USBAUDIO_WAV` writes 16-bit stereo at the rate the guest chose; a
rate change part way starts a new file, with `-1`, `-2`... before the
extension. The header is kept current every quarter second, so the file is
whole even after a kill. Use it with `SDL_AUDIO_DRIVER=dummy` to keep a
headless run off the host's speakers.

### Sound card (ES1370, ES1371)

| Variable | Effect |
| --- | --- |
| `ALPHABOX_ES137X_WAV=<file>` | The card's playback, after the AC'97 mixer's gain, goes into `<file>` as a 16-bit stereo WAV at the host device's rate. The file starts at the first non-silent buffer. |
| `ALPHABOX_ES137X_TRACE=1` | Print every register write the guest makes to the card, and now and then the first bytes of what it plays (`ES137X write ...`, `ES137X dac...` lines). |
| `ALPHABOX_ES137X_SELFTEST=<mode>[:<seconds>]` | A thread plays the card the way a driver would, through its register window, with no guest: `tone` (a looping 440 Hz ring), `stress` (start, stop, pause and re-rate a playing channel as fast as possible; the report line `no hang` is the pass), `mixer` (write all ones to every AC'97 register and check what reads back), `mute` (the tone at 0 dB, -12 dB, then muted), `ring` (record into a 100 byte 8-bit mono ring and report whether the bytes after it were touched), `ringstop` (the same with the ADC stop bit set: nothing may be written). It waits for the console to have opened the PCI DMA window. |

Use them with `SDL_AUDIO_DRIVER=dummy` on a machine with `pci1.1 = es1371`.

`ALPHABOX_USBTRACE=1` traces:

- OHCI register writes, with read counts in between;
- the root hub port status each time the driver reads it;
- every TD retired with an error (condition code);
- isochronous TDs retired late and packets not moved in time
  (`iso packet ... BufferOverrun/BufferUnderrun`);
- every control request a USB device serves;
- each USB storage command and status wrapper.

`=2` adds every retired TD (address, endpoint, length moved, condition
code, delay interrupt).

By default a done queue holding a control TD waits until a millisecond
after the TD ran and then for a frame end, as no real controller answers
sooner ([usb.md](usb.md)). `ALPHABOX_OHCI_EARLY_CTL=1` lets it go back at
once, as bulk and interrupt TDs may.

## Storage

| Variable | Effect |
| --- | --- |
| `ALPHABOX_IDETRACE=1` | A timestamped IDE timeline: commands, packets, bus-master starts and interrupts, check conditions. |
| `ALPHABOX_IDETRACE=2` | As `1`, and on a `cmd649` every other register access but the data port (`CMDR`/`CMDW`, the first 200000). |
| `ALPHABOX_DISK_LATENCY_US=<access>[,<command>[,<MB/s>]]` | Give every IDE hard disk a mechanical service time; `=0` turns the model off. |
| `ALPHABOX_MEDIA_SWAP=<image1>:<image2>:<ms>` | Alternate two images in the first CD drive every `<ms>` ms. |
| `ALPHABOX_TRACE_INQUIRY=1` | Every SCSI INQUIRY a disk or CD gets. |
| `ALPHABOX_TRACE_SCSI=1` | Every command a disk or CD is given, and the sense data of each CHECK CONDITION. |

`ALPHABOX_IDETRACE=1` shows commands, ATAPI packets (the opcode, and bytes
2-11 as words), bus-master starts and interrupts, and every ATAPI check
condition with its sense key. On a `cmd649` it also shows the chip's own
registers: configuration space from 0x40, and bus-master bytes 1, 3, 9
and 11 (`CMDT` lines, the first 400).

`ALPHABOX_DISK_LATENCY_US` charges `<command>` µs per command (default
300), `<access>` µs more when an access does not start where the last one
ended, and a media rate (default 20 MB/s). It overrides the disks'
`latency.*` keys (see es40.cfg, and
[cpu-fidelity.md](cpu-fidelity.md) for the guest that needs it).

`ALPHABOX_MEDIA_SWAP` is a media-change stress test: the swap is forced
past a guest lock and applied between guest commands.

`ALPHABOX_TRACE_INQUIRY` covers a SCSI adapter, USB mass storage and ATAPI
packets alike. It reports the page asked for and, for the unit serial
number page, the serial answered: which guests read a disk's serial at
all, and what they are told. `ALPHABOX_TRACE_SCSI` shows what a guest's
"device error" count is made of.

## Video

| Variable | Effect |
| --- | --- |
| `ALPHABOX_BLIT_STATS=1` | Every 5 s, print what the 8514/A drawing engine has drawn. |
| `ALPHABOX_LFB_DIRECT=0` | Keep every linear-framebuffer access on the device path, instead of offering the VRAM to the CPUs' page caches. |
| `ALPHABOX_TGA_DUMP=<prefix>` | A TGA that does not own the window writes its screen as `<prefix>-NNN-WxH.ppm` every two seconds. |
| `ALPHABOX_RADEON_SYNC=1` | The `radeon` card's engine as it was before its command FIFO. For A/B runs. |
| `ALPHABOX_RADEON_ME_DUMP=<file>` | Write the micro-engine RAM to `<file>` when a driver finishes loading the CP microcode. |
| `ALPHABOX_TRACE_LFB=1` | The S3's linear framebuffer window as it is offered and withdrawn. |

`ALPHABOX_BLIT_STATS` reports pixels, drawing commands and host-data
transfers, with the host time the commands took. It says whether a
sluggish-feeling guest is drawing-bound, and how the driver is drawing (a
listing that scrolls a console draws ~900 million pixels, one transfer
each). The timing is per command, not per pixel, so it does not swamp
what it measures. It does slow a drawing-heavy guest noticeably, so leave
it off when timing anything.

`ALPHABOX_LFB_DIRECT=0` applies to the S3 Trio64, the ViRGE, the Mach64,
the Permedia 2 and the Radeon.

`ALPHABOX_RADEON_SYNC=1` runs every engine command (and the CP's ring)
inside the register write that starts it, and RBBM_STATUS always reads an
empty FIFO and an idle engine. The default is the FIFO and an engine
thread with a modelled busy time ([radeon.md](radeon.md)).

`ALPHABOX_RADEON_ME_DUMP` writes the linux-firmware layout (each entry
DATAH then DATAL, big-endian), for `test/tools/radeon_me_dis.py`. The card
logs the image's CRC-32 and, for a known image, its name
([radeon-microcode.md](radeon-microcode.md)).

### Card traces

- **`ALPHABOX_TRACE_VIRGE`** traces the S3 ViRGE, as a comma-separated
  list:
  - `1`: every port, register and window access;
  - `cmd`: each engine command, one line with its registers (named in
    `src/devices/video/virge/S3VirgeRegs.hpp`);
  - `mode`: each change of the extended display mode;
  - `file:<path>`: every access, but only while `<path>` exists. It is
    polled, so a trace can be switched on around the one step that
    matters (`touch trace.on` ... `rm trace.on`).
- **`ALPHABOX_TRACE_TGA`** traces the DEC TGA (`tga`):
  - `1`: every register, alternate-ROM and RAMDAC access (frame buffer
    writes are too many to list);
  - `first`: each register and each graphics mode the first time it is
    used, which is how to see what a driver uses;
  - `file:<path>`: as `1`, but only while `<path>` exists (polled every
    frame).
- **`ALPHABOX_TRACE_RADEON`** traces the ATI Radeon 7500 (`radeon`):
  - `1`: every register access, by the path it came (`io`, `mmio`, or `cp`
    for a register written by a command-processor packet), every PLL
    access and every type-3 packet with its first dwords. Capped at 200000
    lines; `ALPHABOX_TRACE_RADEON_MAX` sets the cap. Frame buffer accesses
    are not listed.
  - `new`: only the first read and the first write of each register, which
    is how to see what a BIOS or driver touches;
  - `file:<path>`: as `1`, but only while `<path>` exists (polled every
    10 ms).

  Command-processor operations the model does not have are reported once
  each, trace or not.
- **`ALPHABOX_TRACE_MACH64`** traces the ATI Mach64 family: `1` every
  register and configuration access (not the framebuffer); `new` only the
  first read and the first write of each register; `trap` every register a
  trapezoid is drawn from, for the first 64 of them.
- **`ALPHABOX_TRACE_PERMEDIA2`** traces the Permedia 2: `1` every register
  and port access; `gp` the graphics processor's input, a register a line.

## Guest diagnostics

| Variable | Effect |
| --- | --- |
| `ALPHABOX_PC_SAMPLE=1` | Print each CPU's program counter every state poll (~100 ms); finds where a guest is stuck. |
| `ALPHABOX_IRQSTATS=1` | Every 5 s, print interrupt rates. Spots interrupt storms. |
| `ALPHABOX_IRQTRACE=<n>` | Log interrupt entries `n`..`n+39`, with the IER/SIRR/CM writes and ISUM reads between them. |
| `ALPHABOX_TRACE_ARITH=<n>` | Report the first `n` IEEE arithmetic traps (200 for `=1`). |
| `ALPHABOX_TRACE_GUILOCK=1\|<ms>` | Report long waits for, and long holds of, the GUI lock. |

`ALPHABOX_IRQSTATS` counts CPU interrupt entries by source, Cchip
interval-timer ticks, 8259 edges and acknowledges per ISA IRQ, and Cchip
DRIR rises.

### The gdb stub

| Variable | Effect |
| --- | --- |
| `ALPHABOX_GDB=<port>` | Listen for gdb on 127.0.0.1:`<port>`, as the `gdb` key of `sys0` does; overrides it. |
| `ALPHABOX_GDB_WAIT=1` | Hold every processor before its first instruction until a gdb attaches and continues (`gdb.wait`); `0` overrides the key. |

A stopped guest, its breakpoints, single steps, registers and memory, and
the `monitor` commands for the Alpha state gdb cannot show are in
[debugging.md](debugging.md).

### Arithmetic traps

`ALPHABOX_TRACE_ARITH` reports, for each trap:

- the trigger's address and instruction word;
- its two operands as they were;
- the FPCR;
- the trap summary's bits (`trap`: SWC 01, INV 02, DZE 04, OVF 08, UNF 10,
  INE 20, IOV 40; 00 when the trap only asks PALcode to set the status
  bits in `set`);
- `/S` when the instruction has it, and the processor's mode.

```
%CPU-I-ARITH: cpu0 pc 00000000701a2858 ins 594b104a fa 00000000a0000000 fb 0000000000000000 fpcr 8990000000000000 trap 02 set 02 cm 1
```

With the address a guest reports for a floating-point exception, this is
the faulting instruction and what it was given
([cpu-fidelity.md](cpu-fidelity.md) has the case it was written for).

### The GUI lock

The display thread's redraw, the GUI's event handling and the CPUs' VGA
register writes share one lock. `ALPHABOX_TRACE_GUILOCK=1` watches it with
a threshold of 1000 ms; a larger number is the threshold in ms.

- A wait that long prints `%GUI-W-LOCKWAIT` with the waiter's thread and
  call site, and the holder's thread, call site and hold time so far. It
  repeats every threshold while it lasts, so a deadlock keeps naming its
  holder, and once more when the lock is got.
- A hold that long prints `%GUI-W-LOCKHOLD` with the site that took it.
- `main` is the thread that created the GUI (the SDL pump on macOS).

The lock has no timeout, so this only reports. A host that sleeps, or a
stopped process, shows up as one long wait or hold.

## Timing and idle

| Variable | Effect |
| --- | --- |
| `ALPHABOX_NO_IDLE=1` | JIT builds: disable idle pacing. |
| `ALPHABOX_IDLESTATS=1` | JIT builds: print idle-pacing counters every 2000 idle-loop visits. |
| `ALPHABOX_STALL_SKIP=0` | JIT builds: make a guest's `RPCC` delay loop wait in real time, as the hardware would, instead of handing it the cycles it is waiting for. |
| `ALPHABOX_PORT61_PACE=0` | Spin on the ISA refresh-toggle bit of port 61h instead of sleeping to the next edge. |
| `ALPHABOX_RATE=<sec>` | JIT builds: every `<sec>` seconds (fractions allowed), print each processor's instruction rate and what its `IMB`s, cycle-counter reads and delay loops cost. |

`ALPHABOX_STALL_SKIP=0` costs a Windows 2000 boot 35 seconds; see
[cpu-fidelity.md](cpu-fidelity.md) for what the default buys and what it
diverges on.

`ALPHABOX_PORT61_PACE=0` leaves the guest's timing the same; it only
decides whether a host core burns while the guest waits.

`ALPHABOX_RATE` is accurate enough to measure with: it looks at the clock
once every 256 batches. `cpu_bench.sh` and `sect_mips.sh` read it rather
than timing runs from outside. Under `ALPHABOX_HV=1` it also reports the
VM's exits and escapes.

## JIT builds

The switches, each for a same-binary A/B or for isolating a fault:

| Variable | Effect |
| --- | --- |
| `ALPHABOX_JIT_COMPILE_AFTER=<n>` | Interpret a block `n` times before compiling it. The default is 16 (1 on JIT_VERIFY builds). |
| `ALPHABOX_INTERP=1` | Interpret everything; never compile. The control arm for what compiled code is worth. |
| `ALPHABOX_JIT_NOPFLUSH=0\|2` | `0`: flush the instruction cache on every `IMB`. `2`: the audit, below. |
| `ALPHABOX_JIT_NOPFLUSH_BREAK=1` | Do not report the processor's own stores to the code-page map, as though a write path had been forgotten. |
| `ALPHABOX_DPC_KEEP=0` | A data-TB fill empties the page-cache slots of the page it evicts and of the page it inserts, as before. |
| `ALPHABOX_LLSC_MEMOP=0` | A load or store between `LDx_L` and `STx_C` does not make the `STx_C` fail, in either engine, as before 2026-10-22 (an A/B switch; [cpu-fidelity.md](cpu-fidelity.md)). |
| `ALPHABOX_JIT05_MB=0` | `MB` (compiled and interpreted) does not honour a pending page-cache flush request, `IC_FLUSH` does not fence before it reads the code-page map, and `note_code` is not serialised, as before 2026-10-25 (JIT-05; an A/B switch, on by default, kept until a three-round quiet-host perf_ab settles it, as the owner decided on 2026-10-25: [performance.md](performance.md)). |
| `ALPHABOX_JIT05_FENCE=0\|2` | `0`: a store reported to the code-page map does not fence before it reads the line's bit, as before 2026-10-25. `2`: every such store fences, where the default fences only on a page that holds code (JIT-05; an A/B switch, as above). |
| `ALPHABOX_JIT08_XLATE=0` | `virt2phys` and the JIT's memory helpers translate with the copies they had before 2026-10-25 instead of `virt2phys_fast` (JIT-08; an A/B switch, as above). |
| `ALPHABOX_JIT_PINLOG=1` | Print each change of pin set, with the share of register accesses the old and the new set cover. |
| `ALPHABOX_JIT_CHAIN=0` | Compiled blocks never run on into one another, as on a JIT_VERIFY build. |
| `ALPHABOX_JIT_INLMEM=0` | Every load and store from compiled code calls its helper, as on a JIT_VERIFY build. |
| `ALPHABOX_JIT_RPCC=0` | `RPCC` calls its helper instead of the generated stub, as on a JIT_VERIFY build. |

Notes on some of them:

- **`ALPHABOX_JIT_NOPFLUSH=2`** flushes anyway, but reports any block whose
  source changed while the code-page map said nothing had.
  `ALPHABOX_JIT_NOPFLUSH_BREAK=1` exists so that `test/tools/smc_test.sh`
  can be shown to fail when the tracking is wrong.
- **`ALPHABOX_DPC_KEEP=0`**: see [cpu-fidelity.md](cpu-fidelity.md), "Data
  translations outlive their TB entry".
- **`ALPHABOX_JIT_PINLOG=1`**: the pin set follows the registers the
  running code uses ([performance.md](performance.md), "Sixteen pins,
  chosen by what runs").
- **`CHAIN=0`, `INLMEM=0` and `RPCC=0`** together give the production JIT
  minus what the verify lane cannot see. They are for telling such a
  fault from anything else, not for speed. `INLMEM=0` removes the inline
  page-cache probe (both emitters), and probe reuse and the inline
  physical `HW_LD`/`HW_ST` (AArch64; the x86-64 emitter has neither).

Self-tests and diagnostics:

| Variable | Effect |
| --- | --- |
| `ALPHABOX_JIT_FPTEST=1` | JIT_VERIFY builds only: self-test the inline IEEE FP ops against the interpreter at startup, then exit with the verdict. |
| `ALPHABOX_JIT_RPCCTEST=1` | Check the generated `RPCC` stub against the helper it replaces, from six fixed starting states, and print the verdict. |
| `ALPHABOX_JIT_OFFSETS=1` | Print the field offsets compiled code addresses `this` by. |
| `ALPHABOX_TRACE_CODEWRITE=1` | Report the guest writing to memory some block was compiled from. |
| `ALPHABOX_TRACE_ICFLUSH=1` | At exit, a histogram of the PCs that issued instruction-cache flushes. |
| `ALPHABOX_TRACE_ICPROBE=1` | Watch the physical address the dispatcher gives a block (below). |

`ALPHABOX_JIT_OFFSETS` is for when a member has been added in the wrong
place and the emitter's displacements no longer reach.
`ALPHABOX_TRACE_CODEWRITE` is what decides whether an `IMB` has work.

`ALPHABOX_TRACE_ICPROBE` prints the first 50 of each of:

- `ICPROBE`: the dispatcher's instruction-cache probe meets a line the
  fetch would refuse (filled before the last flush) that names another
  page than the live translation. These are lines the probe refuses, as
  it must.
- `ICPROBE-RECORD`: a block is recorded at a physical address other than
  the one it was just fetched from.
- `ICPROBE-CHAIN`: a computed jump enters code compiled from a physical
  address other than the live one.

The last two should never appear (see [performance.md](performance.md),
"Not the shadow bank").

## Bring-up traces

What a firmware asked for and did not get. A firmware scanning empty
configuration space produces these normally; the traces matter when a
machine's firmware wants hardware that is not emulated yet (see
[platforms.md](platforms.md)).

| Variable | Reports |
| --- | --- |
| `ALPHABOX_TRACE_UNKNOWN=1` | every read or write that no device answered |
| `ALPHABOX_TRACE_BME=1` | a PCI function mastering memory with Bus Master Enable (command bit 2) clear is always reported at its first access (`%PCI-W-BME`, the access is not performed: reads give all ones, writes are dropped); with this set it is reported again at every tenfold count |
| `ALPHABOX_TRACE_I2C=1` | every address the firmware puts on the chipset's I2C bus, and whether anything answered |
| `ALPHABOX_TRACE_MP=1` | the registers a console uses to start other processors and hand work to them |
| `ALPHABOX_TRACE_CALLS=1` | the firmware's own calls, once per call site and routine (interpreter builds) |
| `ALPHABOX_TRACE_FLASH=1` | the commands firmware sends the flash |
| `ALPHABOX_TRACE_SERIAL=<n>` | serial port n (0 for `serial0`): each register write, the IIR reads between writes, each change of MCR |
| `ALPHABOX_TRACE_KBC=1` | the keyboard controller |
| `ALPHABOX_TRACE_KEYS=1` | every key the GUI hands the guest (`%GUI-I-KEY`): the key number, press or release, and its set 1 bytes; and the PS/2 mouse motion, buttons and wheel (`%GUI-I-MOUSE`) |
| `ALPHABOX_TRACE_PORT61=1` | the ISA refresh-toggle port, as bursts of reads |
| `ALPHABOX_TRACE_TOY=1` | the TOY clock: the guest's writes to its registers 00h-0Dh, selections with the NMI-disable bit, the first reads of register C |
| `ALPHABOX_TRACE_CSR=1` | on the Tsunami boards, every access to a Cchip, Dchip, Pchip or TIG register, with the value, the processor (or `dev` for a device's DMA) and the instruction; on Marvel, see below |
| `ALPHABOX_DUMP_MEMORY=1` | guest memory, written to `memory_000000000000.dmp` when the emulator is asked to stop |
| `ALPHABOX_RECORD=<names>` | a record of every access to the named devices, one line each, to `ALPHABOX_RECORD_FILE` (default `alphabox-record.txt` in the working directory), at most `ALPHABOX_RECORD_MAX_MB` (default 256); see below |

`ALPHABOX_RECORD` takes a comma-separated list: a device by its
configuration node (`pci0.7`, `ali`, or `pci0.7(ali)`); `dpr`, `flash` and
`i2c` for the board's dual-port RAM, flash and PCF8584 I2C controller;
`pcicfg` for every PCI device's configuration space, as the bus delivers
it; `csr` for the Tsunami's registers; and `raw` to keep every access. Without
`raw`, a run that repeats with a period of up to four accesses is written
once, so a polling loop records the same however long it spins. Each line
is

```
<device> <cpuN|dev> <r|w> <range or space> <offset> <bits> <value>
```

in hex, the value being what was written or what the read returned. The
file is complete once the emulator has exited. It stops at
`ALPHABOX_RECORD_MAX_MB` (default 256) with a last line saying so, and
`%REC-W-FULL` on the console: name small devices, as a disk controller or
a video card in a guest boot writes far more than that. `test/tools/trace_diff.py`
compares two of them ([development.md](development.md), "Device access
records").

`ALPHABOX_TRACE_UNKNOWN` gives the address, its width, and the instruction
or device that made the access:

```
%SYS-T-UNKNOWN: read  32 bits at 803fe002800 (PCI configuration) from cpu0 pc=00000000001a1358
```

The instruction is rarely the interesting one, because firmware reaches
hardware through access helpers, so the return address is reported with
it.

- `ALPHABOX_TRACE_MP` matters because how a machine brings its processors
  up differs from board to board.
- `ALPHABOX_TRACE_CALLS` is how a silent failure inside a console is read
  (it found the DS10's).
- `ALPHABOX_TRACE_FLASH` tells "it never found the part" from "it read what
  it wanted".
- `ALPHABOX_DUMP_MEMORY` finds what a firmware left in memory, and what it
  did not.

### The EV7 machines

The Marvel chipset ([platforms/marvel.md](platforms/marvel.md)) has its own:

| Variable | Reports |
| --- | --- |
| `ALPHABOX_TRACE_CSR=1` | every access to a modelled EV7 register, named, with the instruction that made it |
| `ALPHABOX_TRACE_IO7=1` | every access to an IO7 register, named, with the instruction and its return address; in 21464 mode also an IID a full interrupt queue refused, and its taking when sent again |
| `ALPHABOX_TRACE_RBOX=<ms>` | any processor that leaves an enabled interrupt pending that long (default 200) |
| `ALPHABOX_TRACE_MP=1` | how the console starts the other processors: each write of a processor's RBOX_SCRATCH1 that changes it or comes from another processor, each reader's first read of a value (and how long it waited for the start hand-off), and each RBOX_IREQ request but the clock rendezvous; with the time in microseconds, who made the access, and where the processor is |
| `ALPHABOX_GIO_LOG=<file>` | the console's transactions with its management side (the CMM on the ES47), written to `<file>` |
| `ALPHABOX_TRACE_CMM=1` | on the ES47, also each change of the PALcode's state word sent to the CMM |
| `ALPHABOX_EV7_START_SECONDARIES=1` | a test hook: start the parked secondaries at once, as the console would |

The CSR and IO7 traces print the first three of each repeated access, then
every power of two. `ALPHABOX_TRACE_RBOX` tells a guest's `CPUSPINWAIT`
caused by an interrupt not raised from one not taken.

### Processor rows

| Variable | Effect |
| --- | --- |
| `ALPHABOX_CPU_ANY_BOARD=1` | a test hook: a board takes a processor row of another core family than its own (an `ev7` on the ES40), where it would refuse the configuration; `%CPU-W-ANYBOARD` says so at startup, and `alphabox run --check` reports it as a warning |

The CPU oracle sets it to run a row's instructions without its console
(`test/cpu/cpu_oracle.py --cpu`, [development.md](development.md)). The
processor keeps its row's identity and the board everything else, so no
console or operating system runs this way.

### LDx_ARM and QUIESCE (the `ev8` row)

| Variable | Effect |
| --- | --- |
| `ALPHABOX_QUIESCE_TIMEOUT=<cycles>` | a test hook: QUIESCE's timeout, in processor cycles at the configured clock, for the 10240 the 21464 resets QUIESCE_TIMEOUT to (decimal, or hex with `0x`; at most 2^40). `0` makes every QUIESCE end at once, as the IDS says a timeout of 0 would. Off by default; in EV7-compatible mode no IPR sets the timeout |
| `ALPHABOX_TRACE_QUIESCE=1` | when an `ev8` processor goes away (the emulator exits), one line, `%CPU-I-QUIESCE`: its LDx_ARMs and how many were refused (an interrupt pending, an address outside memory), the QUIESCEs that slept and for how long in all, what woke them (another processor's store, a successful STx_C, a device's DMA, an interrupt, the timeout, anything else that needed the processor), and the QUIESCEs that did not sleep (watch_flag clear; another processor still holding an inline-writable translation of a newly watched page) |

The timeout hook is what lets a test tell a wake-up by a store or an
interrupt from the timeout: at the default, 10240 cycles, a QUIESCE ends
within about ten microseconds whatever happens. `test/cpu/cpu_oracle.py`
sets 2^24 for a row that has QUIESCE, and `test/cpu/quiesce_mp.py` 2^28
([development.md](development.md)). How the wait works:
[ev8.md](platforms/ev8.md), "S3".

### The native EV8 (the `ev8n` row)

| Variable | Effect |
| --- | --- |
| `ALPHABOX_TRACE_EV8=1` | a line, `%CPU-I-EV8`, for each DTB_PTE written with VALID clear (the first fifty of each processor): the PTE, the tag, the PC. The chip would hang there (Restriction 9); Alphabox makes no entry and counts it (decision N10, [ev8-native.md](platforms/ev8-native.md)) |

On `ev8n` QUIESCE's timeout is the QUIESCE_TIMEOUT IPR's, which PALcode
sets; `ALPHABOX_QUIESCE_TIMEOUT` does not apply.

**Starting a Marvel board in 21464 mode.** An ES47, ES80 or GS1280 of
`ev8n` processors needs no test hook, but it has no console yet
([ev8-native.md](platforms/ev8-native.md), S11b): it starts from a boot
image named by `rom.decompressed`, in the decompressed console's format
(entry PC, PAL_BASE, the board's 0x480000 bytes of memory, the tag
`ALPHDC01`; `test/cpu/image.py` `write_rom`), and refuses to start without
one. Every TPU of every chip enters the image's PALcode at PAL_BASE +
0xB00, and `%MVL-I-21464` lines say so. The image talks to the outside
through GIO and the CMM: its terminal is `serial0`, as the console's is.
`test/cpu/marvel8_mp.py` is the worked example. With
`ALPHABOX_TRACE_IO7=1` an IO7 also says when a full interrupt queue
refused one of its IIDs and when the IID it sent again was taken.

### Tarantula (the `ev8` and `ev8n` rows)

| Variable | Effect |
| --- | --- |
| `ALPHABOX_TARANTULA_PEEK=1` | a test hook, on a board without EV7 register windows (the ES40 under `ALPHABOX_CPU_ANY_BOARD`) only: the processor's vector registers appear at offset 0x3F8000 of its EV7 register window, element i of $vrN at 0x3F8000 + 0x400 x N + 8 x i, for quadword loads and stores ($vr31 reads zero and ignores a store); `%CPU-W-TARPEEK` says so at startup |

Stage S7 has no vector loads or stores (they are S8's), and the contract
moves no element between vector and scalar registers
([ev8-isa.md](platforms/ev8-isa.md), point 13), so this window is how the
CPU oracle's group `tarantula` and `test/cpu/tarantula_vv.py` read results
and the oracle puts operands in ([development.md](development.md)). PID
0's window is at physical 0xFFF_FFC0_0000, so its registers start at
0xFFF_FFFF_8000; on `ev8n` the window is the 21464 mode's (N18), PID 0's
at 0xFFFF_FFC0_0000, and the ES40 folds both into one address.

Under `ALPHABOX_CPU_ANY_BOARD` an `ev8` processor on such a board also
answers VCTL, the vector enable, at offset 0x3F0000 of its window
(0xFFF_FFFF_0000 for PID 0), as the Marvel boards do, so a test sets and
clears VEN with the quadword store an operating system would use. The
Tsunami and the Titan ignore PA<42:35> in their I/O space, so every PID's
window lands on one address there, and each processor reaches its own.
On `ev8n` VCTL is the IPR 0xAC and the offset is not decoded (N8).

## Hypervisor runtime

Builds with `-DALPHABOX_HVF=ON` only ([hypervisor.md](hypervisor.md)):

| Variable | Effect |
| --- | --- |
| `ALPHABOX_HV=1` | Every CPU thread runs its dispatch loop at EL1 inside a Hypervisor.framework VM. |
| `ALPHABOX_HV_JIT=1` | With `ALPHABOX_HV=1`: run compiled code inside the VM too. The default there is the interpreter. |
| `ALPHABOX_HV_TRACE=1` | Print every mapping, exit and proxied call. |
| `ALPHABOX_HV_WATCHDOG=<s>` | Cancel a vCPU that never exits, and print where it was. |
