# Peripherals the ES40 firmware knows

The ES40 SRM console (V7.3-1, `test/rom/cl67srmrom.exe`) carries a table of
the PCI devices it recognises by name, and a separate set of console
drivers for the ones it can *use* (boot from, or print to). That list is a
good definition of "a peripheral an ES40 could really have", so it is the
list Alphabox works from.

Two kinds of recognition matter:

- **Named**: `show config` prints the device's name. An unnamed device still
  gets configured (BARs, interrupt) and a guest OS may still drive it; SRM
  just shows the vendor/device id.
- **Driven**: SRM has a console driver for it, so it can boot from it
  (disks, network) or use it as the console (graphics). The console driver
  names in the image are: `pci_isa`, `n810` (all NCR/Symbios 53C8xx),
  `n825_dssi`, `isp1020`, `aic78xx`, `dac960`, `kgpsa`, `qla2100/2200/2300`,
  `cipca`, `vip7407`, `DEFPA`, `acer_pmu`, `pci_nvram`, `vga_bios`, and the
  Tulip and Intel Ethernet drivers.

The names below were extracted from the decompressed image with
`strings`; grouping and notes are ours.

## Emulated today

| Firmware name | Alphabox class | What the ES40 console does with it |
| --- | --- | --- |
| Acer Labs M1543C | `ali` | the ISA bridge |
| Acer Labs M1543C IDE | `ali_ide` | boots from IDE disks and CD-ROMs |
| Acer Labs M1543C USB | `ali_usb` | names it |
| Acer Labs M1543C PMU | `ali_pmu` | names it |
| NCR 53C810, 53C825 (825A), 53C875, 53C895 | `sym53c810` ... `sym53c895` | boots (`n810`) |
| NCR 53C896 | `sym53c896` | boots on both channels (`n810`) |
| QLogic ISP1020, ISP1040 (KZPBA) | `isp1020`, `isp1040` | boots (`isp1020`) |
| (not named) | `isp1080`, `isp1240` | prints the raw id; cannot boot |
| DECchip 21143-AA / DE500-BA | `dec21143` | network boot (Tulip driver) |
| DECchip 21140-AA, 21041-AA, 21040-AA | `dec21140`, `dec21041`, `dec21040` | network boot (Tulip driver) |
| DE600-AA (Intel 82559), Intel 8255x Ethernet | `de600`; `i82557`, `i82558`, `i82559` | network boot (`ei`), loopback self-test |
| Intel 82559ER (8086:1209) | `i82559er` | names it (`ei` driver); the DS25's console lists it as `eia0` |
| DE602-AA, DE602-B* (two 8255x behind a bridge) | `de602`, `de602b` | network boot on either port |
| DECchip 21050-AA, 21052-AA, 21152-AA, 21153-AA, 21154-AA | `dec21050` ... `dec21154` | numbers and probes the buses behind them |
| S3 Trio64/Trio32 | `s3` | console (`vga_bios`) |
| Cirrus CL-GD5430, CL-GD5434 | `cirrus` | console (`vga_bios`) |
| ATI Mach64 (1002:4354, the CT) | `mach64` | console (`vga_bios`) |
| (not named) | `mach64` with `chip` `vt2`, `vt3`, `rage2p`, `ragepro` | prints the ids |
| ELSA GLoria Synergy, PowerStorm 4D10 (by subsystem) | `permedia2` | not its console: the BIOS is started wrongly |
| (not named) | `s3virge` | runs its BIOS: console (`vga_bios`) |
| DECchip ZLXp 21030 | `tga` (`model = "e1"`) | lists it; no console driver |
| DEC PowerStorm (TGA2) | `tga` (`model = "3d30"`, `"4d20"`) | lists it; no console driver |
| Radeon 7500 AGP, Radeon 7500 PCI | `radeon` | console (`vga_bios`) |
| Ensoniq Sound Card | `es1371` | names it |
| (not named) | `es1370` | prints the ids |
| USB (subsystem 11C1:5803) | `uss344` | the ES47 console lists usba..usbd |
| CMD 649 PCI-IDE | `cmd649` | the ES47 console lists `dqa` and boots from a CD on it |
| (none: an add-in card) | `ehci` | prints the ids |
| (none: paravirtual) | `virtio_blk`, `virtio_net` | prints the ids |

What each does, and which guests drive it, is below.

### The ALi M1543C

- `ali`: the ISA bridge with the PIT, PIC, DMA, RTC/TOY, keyboard and
  mouse, COM1/COM2, LPT1 and floppy.
  The floppy controller's DOR reads back what was written, and SENSE
  INTERRUPT STATUS with no interrupt pending answers with the one byte 80h.
- `ali_ide`: bootable IDE disks and CD-ROMs.
- `ali_usb`: OHCI 1.0a, three ports: the schedule, the root hub, and USB
  devices on it: a tablet, a keyboard, mass storage, a speaker,
  passed-through host devices ([usb.md](usb.md)). Windows 2000 and
  Whistler drive it with their own USB stacks.
- `ali_pmu`: the power management unit.

The bridge's ISA core is four models of their own in `src/devices/isa/`,
each with a unit test on a clock the test moves (`ctest`: `i8254`,
`i8259`, `mc146818`, `superio`): the 8254 timer, the two 8259s, the
MC146818 TOY clock, and the Super I/O and ISA Plug and Play ports. Their
registers stay in the bridge's state file as before. What the guest sees
of them, from the data sheets:

- **8254**: in mode 0, OUT stays high after terminal count until the
  counter is written again, and the count wraps on; a gate drop holds the
  count exactly. A count written to a running mode 2 or 3 counter waits
  for the end of the cycle (half-cycle in mode 3). Reads and writes of one
  counter may interleave. BCD counters count four decades. A snapshot
  saves where each counter is in its count, and a restore goes on from
  there.
- **8259**: entering or leaving special mask mode updates the interrupt
  line at once. All eight bits of both edge/level registers are read and
  write, as the M1543C data sheet lists them.
- **TOY clock**: port 71h reads the selected register when it is read,
  and register C clears when read. UIP is high in the last 244 us before
  the clock's own second changes, so a guest that waits for it never reads
  a torn time. 12-hour mode reads 12 at noon and midnight. Registers C and
  D and the UIP bit are read only, and a time byte written while SET is
  clear sets the running clock. `ALPHABOX_TRACE_TOY=1` shows what a guest
  does with it.
- **Wide accesses**: a 16- or 32-bit access to any of the bridge's legacy
  ports is served as byte accesses at consecutive ports; it used to stop
  the emulator.
- **ISA PnP**: the READ_DATA port is placed only in 0x203-0x3FF, as the
  PnP specification allows; a snapshot maps it, and the parallel port,
  again on restore.
- **PM timer**: it survives a snapshot taken in another run, and a byte
  or word read of it returns that part of the count.

### IDE and ATAPI behaviour

What a guest can rely on from the ATA/ATAPI core (`ali_ide` and `cmd649`),
each point checked against ATA/ATAPI-6 or SFF-8038i by `test/tools/ide_test.sh`:

- **A command the drive does not implement is aborted** (ERR, ABRT in
  Error), never fatal: SMART, the 48-bit set, security and the rest. PACKET
  with the overlap bit is aborted the same way.
- **EXECUTE DEVICE DIAGNOSTIC (90h)** runs on both drives whichever is
  selected: Error 01h, the ATA or the PACKET signature, DRDY only on an ATA
  drive, an interrupt.
- **Addresses outside the drive end the command with IDNF** (Error 10h) and
  leave the task file at the failing sector: LBA past the last sector, a
  CHS address with sector 0, a sector or head the geometry lacks. A sector
  the image cannot read is UNC (40h). READ/WRITE SECTORS, MULTIPLE, DMA and
  READ VERIFY all go through the same check; a write past the end no longer
  lands on the previous sector.
- **READ SECTORS to a CD-ROM drive is aborted** with the PACKET signature in
  the cylinder registers.
- **SET MULTIPLE MODE** takes 0 to 128 and aborts above; with 0, READ and
  WRITE MULTIPLE are aborted until the next count.
- **FLUSH CACHE (E7h) flushes the image**, and IDENTIFY words 83 and 86 bit
  12 say so for a disk (not for a CD-ROM drive). IDENTIFY words 60-61 never
  exceed 0FFFFFFFh; a larger image is reachable only with 48-bit commands,
  which are not implemented.
- **A software reset, or a new command, ends a DMA command still waiting
  for the bus master's Start bit**; the channel used to stay wedged after a
  driver's timeout and reset.
- **Bus master registers** read as bytes, words or dwords (a 16-bit read
  used to stop the emulator); the descriptor table pointer reads with bits
  1:0 clear; a descriptor table may have up to 8192 entries and ends at the
  entry whose bit 31 is set, whatever its other reserved bits hold; a table
  that does not end sets the Error bit and raises no interrupt. The status
  byte is updated under one lock, so a poll cannot undo the Interrupt bit.
- **ATAPI data-out** (MODE SELECT and the like) arrives in blocks of at most
  65534 bytes, as data-in already did.
- **A software reset** leaves 01h in Error on each drive that is present
  and the signature; a CD-ROM drive then has DRDY, DF and DSC clear, as
  after DEVICE RESET and EXECUTE DEVICE DIAGNOSTIC.
- **SET FEATURES transfer modes are per drive**, and a PIO mode leaves the
  selected DMA mode in IDENTIFY words 63 and 88; a disk's Ultra DMA choice
  no longer disappears when the CD-ROM drive on the channel selects PIO 4.
- **The bus master Interrupt bit follows INTRQ** whether or not the engine
  was started, so a PIO command sets it and the driver clears it by
  writing 1.
- **An odd ATAPI byte count** is transferred as (n+1)/2 words with a zero
  pad, not truncated.

### Keyboard and mouse

The ALi's keyboard controller sits at ports 60h and 64h with IRQ 1 and 12,
and has a PS/2 keyboard and mouse behind it (`src/devices/isa/Keyboard.cpp`).
The ALi datasheet says only that it is software compatible with the 8042AH
and runs a ROM programmed to the customer's order (M1543C datasheet V1.10,
section 4.4.5), so the commands follow IBM's reference for the PS/2
(84F9735, PS/2 Hardware Interface Technical Reference, Common Interfaces:
the Keyboards and Keyboard/Auxiliary Device Controller sections).

- The keyboard powers up, and comes out of Reset (`FF`), in scan code set
  2, which the controller translates to set 1 while bit 6 of its command
  byte is set. Set Default (`F6`) and Default Disable (`F5`) restore the
  typematic rate and the set 3 key types but keep the scan code set.
- Every console and guest traced selects a set itself
  (`ALPHABOX_TRACE_KBC=1`): SRM set 3, untranslated, and then `F6`;
  AlphaBIOS and Windows 2000 set 2, translated; OpenVMS set 3,
  untranslated ([openvms.md](openvms.md#keyboard-and-mouse)).
- Keys typed while the controller inhibits the keyboard (`AD`, or bit 4
  of the command byte) wait in the keyboard's 16-byte buffer. A key event
  that does not fit is lost and the overrun code takes the buffer's last
  position (`00` in sets 2 and 3, `FF` in set 1 and whenever the
  controller translates), as on the real keyboard; so is every key after
  it until the guest reads. A release lost that way leaves the key down in
  the guest, as it would on hardware.
- The controller answers the commands IBM lists for its translating
  (Type 1) 8042. Its RAM other than the command byte is not kept: `21`-`3F`
  read 0 and `61`-`7F` swallow their byte. No password is ever installed
  (`A4` answers `F1`; `A5`'s bytes are swallowed up to the null). `E0`
  reads 0. `F0`-`FF` pulse output port bits, and the reset bit (`FE`, or
  `D1` with bit 0 clear) does not reset the machine. `C1`/`C2` and the
  vendor commands outside IBM's list (`A0`, `A1`, `AF`, `AC`) are not
  modelled; the ALi datasheet does not list its firmware's commands.
- The mouse follows the PS/2 mouse command set as A. Chapweske's "The
  PS/2 Mouse Interface" (2003) describes it; IBM's and Microsoft's
  documents for it are not public. A resolution other than 0-3 or a
  sample rate other than 10, 20, 40, 60, 80, 100 or 200 gets Resend (`FE`)
  and the mouse waits for the value again. Enable (`F4`) turns reporting
  on in whichever mode the mouse is in. It also releases the controller's
  aux interface, as it always has here: SRM leaves the interface disabled
  (`A8`, `D4 F4`, `A7`), OpenVMS's driver never enables it again, and its
  mouse works. After its reset the mouse is in stream mode.
  2:1 scaling (`E7`) applies to stream packets and not to Read Data
  (`EB`); the resolution (`E8`) is reported back but not applied.
- The mouse is an IntelliMouse Explorer: the sample rates 200, 100, 80 in
  a row switch on the wheel (ID 3, 4-byte packets), and 200, 200, 80 after
  that the fourth and fifth buttons as well (ID 4: the wheel in four bits,
  the buttons in bits 4 and 5 of the fourth byte). The host's back and
  forward buttons (SDL's X1 and X2) are buttons 4 and 5. Windows 2000
  probes both and runs the mouse at ID 4. This mouse takes the
  five-button sequence only once the wheel is on, the order Windows and
  Linux send them in; OpenVMS probes five buttons first, while the mouse
  is at ID 0, then the wheel, and so runs it at ID 3. No document says
  what a real Explorer does with that order. Until a guest switches the
  wheel on, wheel turns send nothing.

### The serial ports

`serial0` and `serial1` are the M1543C's COM1 (0x3f8, IRQ 4) and COM2
(0x2f8, IRQ 3): a 16550 with FIFOs (`Uart16550.cpp`) behind a host side
(`Serial.cpp`), a telnet listener, `null_attach` or `disabled`
([configuration.md](configuration.md#serial-consoles)). The ES40's
console, OpenVMS, Windows 2000 and Linux drive them; on the ES47 the CMM
reads `serial0` for the console terminal. `uart16550_test`
([development.md](development.md#unit-tests)) checks the model against
the data sheets: the National PC16550D (TL/C/8652, June 1995) and the ALi
M1543C's own (v1.10, 1543DSC2.doc).

Where the port knowingly differs from the chip:

| What | Consequence |
| --- | --- |
| The receive FIFO holds 1023 bytes, not 16, and typed input is offered a character at a time, once the guest has read the previous one | Typed or pasted input is never overrun; an overrun (LSR.OE) happens only in loopback |
| RDA is raised at one byte whatever FCR's trigger level, and the character-timeout indication (IIR 0x0c) is never given | Drivers read until LSR.DR clears either way |
| Transmission is instantaneous: THRE and TEMT always read 1; the divisor, word length, parity, stop bits and LCR's break bit have no effect | A guest cannot time anything by the line |
| The interrupt is driven whatever MCR bit 3 says. On the M1543C that bit "enables the interrupt" (its data sheet, p. 263), as OUT2 gates IRQ on a PC | The ES40 console and OpenVMS set it before using interrupts; Windows 2000's serial.sys clears it at times with IER=0f and would be interrupted here where the chip would stay quiet. Not changed until Windows, Tru64 and Linux have been checked against it |
| The modem inputs read CTS and DSR asserted, DCD and RI clear, client or not; they change only in loopback | A guest cannot see a hang-up |
| A telnet BREAK opens the emulator's menu instead of sending a line break (LSR.BI) | The guest never sees a break |

Changed in October 2026, each from the data sheets:

- IER bits 7:4 and MCR bits 7:5 read 0 (PC16550D 8.7, 8.8; M1543C
  pp. 262-263). Linux took a writable IER bit 6 for an XScale UART.
- Writes to LSR and MSR change nothing (PC16550D 8.4; M1543C p. 260);
  they used to land in the scratch register.
- In loopback the receiver takes nothing from the line (PC16550D 8.8);
  host input waits until loopback ends.
- MSR's delta bits and the modem-status interrupt (PC16550D 8.9 and
  Table IV; M1543C p. 264).
- An overrun sets LSR.OE and raises the receiver-line-status interrupt
  (PC16550D 8.4 and Table IV).
- Turning the FIFOs on or off empties them, and FCR's other bits act only
  with bit 0 set (PC16550D 8.5).

### SCSI

- **53C825, 875, 895**: wide, 4 KB SCRIPTS RAM; the 875 Ultra-Wide, the
  895 Ultra2-Wide. OpenVMS 8.4 drives the 875 with `SYS$PKWDRIVER`
  ([openvms.md](openvms.md#scsi-controllers)).
- **53C896**: two Ultra2-Wide cores as PCI functions 0 and 1, 8 KB SCRIPTS
  RAM each, a 256-byte register file with the phase-mismatch jump block.
  Windows 2000 drives it with `sym_hi`, not the `symc8xx` of the
  single-channel parts.
- **ISP1020, ISP1040**: Windows 2000 drives them with its own QLogic
  driver (`ql10wnt`), OpenVMS 8.4 with `SYS$PKQDRIVER`
  ([openvms.md](openvms.md#scsi-controllers)).
- **ISP1080**: Ultra2 Wide, low-voltage differential. The console does not
  know PCI device 0x1080 (its table carries QLogic 0x1020 and the Fibre
  Channel parts and nothing else), so `show config` prints the raw id and
  it is not bootable. Windows 2000 drives it with `ql1080`.
- **ISP1240**: two Ultra Wide buses on one PCI function (not two
  functions, the way the 53C896 is built): one set of queues and
  mailboxes, and a command entry names its bus in the top bit of the
  target byte. Disks are `disk0.<id>` and `disk1.<id>`. Unknown to the
  console for the same reason as the 1080; Windows 2000 drives it with
  `ql1240`.

Every disk and CD has its own serial number, and the control mode page
(0x0A) that OpenVMS reads on every mount
([configuration.md](configuration.md#serial-numbers)).

The disks and CDs on every SCSI adapter (and, through the same code, the
IDE and USB CD paths) are a DEC RZ58 and RRD42 that report SCSI-2 and
follow it, and SPC-3, SBC-3 and MMC-3 where SCSI-2 has nothing to say;
`test/tools/cdb_test.sh` checks them command by command
([development.md](development.md#usb-storage-and-sound)). What a guest
can rely on:

- READ and WRITE of any length (6, 10, 12 and 16-byte forms), moved
  through the drive's buffers a piece at a time; READ CAPACITY(16), and
  READ CAPACITY(10) answering FFFFFFFFh on a disk past 2 TiB.
- A read-only image, BIN/CUE or RAM disk is write protected: WP in MODE
  SENSE, DATA PROTECT on a write. A CD has no WRITE command. A write the
  host cannot complete (its disk full) is a HARDWARE ERROR, not a GOOD.
- An LBA or length past the end, a mode page, a page control, a TOC
  format or a block length the drive lacks are CHECK CONDITION with the
  standard sense, never a stop of the emulator. Sense data lasts until
  the next command; a logical unit other than 0 is LOGICAL UNIT NOT
  SUPPORTED.
- A SCSI bus reset or BUS DEVICE RESET resets the drive: a locked tray
  unlocks, a MODE SELECT block length returns to its power-on value, and
  the next command gets a unit attention (06/29/00).
- VPD pages 0x00, 0x80 (the serial), 0x83 (an identifier built from the
  serial) and, on disks, 0xB0.
- READ TOC's MSF addresses are the LBA plus 150 frames, in binary;
  BIN/CUE images with one file per track are laid out track after track.
- Selected without ATN, a drive asks for the command first, as a SCSI-2
  target that takes SCSI-1 initiators does, and the LUN comes from the
  CDB.

The 53C8xx SCRIPTS processor follows the Symbios data manuals (the 895's
v3.0 is the reference; `cdb_test.sh` runs SCRIPTS programs against it):

- A reserved Transfer Control opcode, a Memory Move with a reserved bit
  set or with source and destination of different alignment are an
  illegal instruction (DSTAT IID), never a stop of the emulator. A
  selection of the chip's own id or of an empty one is a selection
  time-out, reported by the next instruction that needs the target, as
  the manual describes for SELECT.
- A Memory Move takes its whole 24-bit count (up to 16 MB) and leaves
  TEMP alone.
- A table-indirect SELECT loads SCNTL3 and SXFER from the table with the
  ID. SELECT passes its ATN bit to the target.
- The general purpose timer (STIME1 GEN) runs in host time: 125 us for
  GEN 1, doubling per step, 16 times that with GENSF.
- SFBR takes the first byte received in STATUS, DATA IN and MESSAGE IN,
  never a byte sent. SBCL shows the bus control lines (REQ, ACK, BSY,
  SEL, ATN and the phase); ATN is SOCL bit 3, dropped at the end of a
  MESSAGE OUT move, and a MESSAGE IN move leaves ACK asserted until a
  Clear ACK. SSTAT1's phase bits change only when a target has a phase
  up.
- A PCI reset resets the cores, a running SCRIPTS program included. A
  WAIT RESELECT ends when an interrupt halts SCRIPTS or the driver starts
  another program, so a later SIGP cannot redirect it.
- The chip arbitrates as id 7 whatever SCID says (a disk cannot be
  configured at 7).

### Network and bridges

- **The Tulips' receive path** is the chip's on every back end. The NIC
  filters addresses itself: the setup frame's filtering type (perfect,
  hash, inverse or hash only, shown in CSR6's read-only HP, HO and IF) and
  CSR6's PR, PM and RA decide which frames reach the ring, and pcap's
  host filter is only a coarser first cut. SRM V7.3-1 loads a perfect
  table of its own address and nothing else, so it hears no broadcast.
  Short frames are padded to 60 bytes in both directions (unless the
  transmit descriptor sets DPD); while the receiver is stopped, frames are
  read and dropped; a frame that finds the receive queue full counts in
  CSR8's FIFO overflow counter, one that finds no descriptor in the
  missed-frame counter. `trace_packets` shows each setup frame and marks
  the frames the filter refused. Seen from the guests: Linux's `tulip`
  driver loads perfect tables, and a hash table past 14 multicast
  groups; OpenVMS 8.4's EW driver, with MOP enabled, loads its station
  address and AB-00-00-01-00-00; Windows 2000 installs the device and
  its driver sends DHCP requests.
- **Tulip threads**: the CPU's CSR accesses and the NIC's own thread share
  one lock, never held across DMA, the interrupt line or the back end
  ("Threads and locking" in `Tulip.cpp`). Two Tulips, each under a flood
  of a few thousand frames a second, network-boot side by side.
- **21140**: no SIA, media through the general purpose port. The board
  described has nothing wired to those pins and no MII PHY.
- **21041**: 10 Mb SIA, the older serial ROM format. The console drives it
  at 10BaseT from its own table and reads only the station address out of
  the ROM.
- **21040**: 10 Mb SIA, and no serial ROM at all: the station address
  comes out of a parallel ROM read a byte at a time through CSR9.
- **Intel 8255x**: OpenVMS 8.4 (`EIA0`) receives unicast and multicast; it
  configures the chip to refuse broadcasts until something asks for them
  ([openvms.md](openvms.md#network)).
- **82559ER**: the DS25 builds its `i82559er` at `pci0.8` by itself,
  unconnected; declare it there to give it a back end
  ([ds25.md](platforms/ds25.md)).
- **Bridges**: what the forwarding windows cover is what is reachable,
  nested buses too. Windows 2000 drives a 53C810 behind one.

Every NIC can have a loopback plug on its port (`type = "loopback"`), with
which the console's wire-level `nettest` modes pass
([configuration.md](configuration.md#networking)).

### Graphics: S3 Trio64, Cirrus

- **S3 Trio64**: the console, ARC/AlphaBIOS, Windows NT.
- **CL-GD5434**: Windows 2000 draws its desktop through the BitBLT engine.
- **Snapshots**: both cards save and restore their own registers beyond the
  VGA core (the Cirrus bank, BitBLT and hidden-DAC registers; a block of
  its own, versioned, that older state files simply lack and restore
  without). A resumed text or 8 bpp screen is redrawn from the saved
  palette instead of black.
- **S3 after 24 bpp**: leaving a 24 bpp mode for 8, 15 or 16 bpp now draws
  in the new format; the flag used to stay set until a reset. A read
  through the linear window (BAR0) at 0xA0000 to 0xBFFFF returns display
  memory, as the write does.
- **Unassigned VGA ports**: a guest write to 0x3c1, 0x3ca to 0x3cd or
  0x3db is ignored, as on the chips.

### Graphics: ATI Mach64

**The CT** is the first Mach64 with its DAC and clock synthesizer on the
chip. Emulated:

- its register file;
- the GUI drawing engine: rectangles, lines, screen and host sources,
  patterns, the sixteen mixes, colour compare;
- the hardware cursor and the extended display modes at 4 to 32 bpp.

**Verified with Windows 2000**, which names it "ATI Technologies Inc.
mach64 CT PCI" and binds its own inbox miniport (`ati.sys`/`ati.dll`) with
no files supplied:

- it reports the device working;
- after the restart the installation asks for, it runs the desktop in
  eight-bit colour through the drawing engine: six million pixels over four
  thousand commands to open one window (`ALPHABOX_BLIT_STATS=1`);
- before the restart it is the sixteen-colour VGA fallback, which is what
  a card with no driver yet looks like;
- the driver's own mode list works, **up to 32 bpp**, which the console
  cannot reach at all: it exercises the extended CRTC, the DAC in a packed
  four-byte layout and the engine at four bytes to the pixel. A session of
  mode changes and window dragging drew 12.2M pixels over 7041 engine
  commands at 6.4 ns/pixel.

**The other parts** have no table entry, so `show config` prints only
their ids:

- `vt2`: the 264VT2 (1002:5654), the CT's register file plus the video
  overlay;
- `vt3`: the 264VT3 (1002:5655);
- `rage2p`: the 3D Rage II+ (1002:4755). Windows binds
  `atirage.sys`/`atirage.dll`, DirectDraw only;
- `ragepro`: the 3D Rage Pro (1002:4742), with the triangle setup engine.
  Windows binds `atimpab.sys`/`atidrab.dll`, whose Direct3D HAL starts at
  the Rage Pro. The SRM console runs on it, drawing what it draws on the
  VT2.

### Graphics: 3Dlabs Permedia 2

The ELSA GLoria Synergy and the DEC PowerStorm 4D10T are boards on the
Permedia 2 (TI TVP4020, 104C:3D07); the console names them by subsystem.
It is not the SRM console: SRM V7.3-1 starts the card's BIOS without
telling it where the card is ([limitations.md](limitations.md)).
AlphaBIOS runs it. Windows 2000's driver runs the desktop and Direct3D on
its graphics processor and delta unit, and OpenVMS 8.4's DECwindows draws
its login box on it ([openvms.md](openvms.md#graphics)).

### Graphics: the S3 ViRGE family

The console has no table entry for any of them, but runs their BIOS and
uses the card as its `vga_bios` console; AlphaBIOS draws on them.

**The ViRGE/DX** (`chip = "dx"`, 86C375, 5333:8A01), with the S3
reference BIOS 2.01.16. Emulated:

- the S3 SVGA with its RAMDAC and clock synthesiser;
- the streams processor: 24-bit packed modes, a scaled YUV/RGB overlay;
- the S3d engine: 2D BitBLT, fill, line and polygon, and 3D triangles with
  Z buffer, textures, fog and alpha blending.

**Verified with Windows 2000**:

- it installs from the RC2 media with this card as its only display:
  AlphaBIOS, text-mode setup, and GUI setup, whose device detection
  installs `s3m` and the monitor it reads over DDC;
- in an installed system the inbox `s3m.sys`/`s3mtrio.dll`/`s3mvirge.dll`
  install when the card is found and, after a restart, run the desktop at
  640x480 to 1024x768 in 8, 16 and 24 bpp (the driver offers no 32 bpp on
  the DX);
- dxdiag reports DirectDraw and Direct3D acceleration enabled, and its
  DirectDraw and Direct3D tests pass on the hardware.

`d3d_check.sh` (30 scenes against Direct3D's software rasteriser, 16-bit
desktop):

- Gouraud, flat, fog, alpha blending, Z buffer, bilinear, ARGB8888
  textures, points and clipping match;
- nearest-texel scenes (point, modulate, perspective, texture alpha,
  ARGB4444, colour key) differ only on texel boundaries, which the driver
  rounds up (it biases U and V by 1/256 texel) where the reference rounds
  down;
- the rest are the chip's or the driver's limits: no alpha test, no
  texture clamping, no specular colour (the driver fogs towards white
  instead), square textures only, no mipmaps from the driver, no
  multitexture, lines drawn as thin triangles whose colour does not follow
  the line;
- the YUY2 overlay matches; there is no planar YV12 overlay.

The `gx` row (the DX's id, revision 1) has not been run on its own.

**The original ViRGE** (`chip = "virge"`, 86C325, 5333:5631), with the S3
reference BIOS 1.00-10 (`86c325.bin`). What sets it apart from the DX, as
the ViRGE data book and the Windows driver's register values have it:

- a 2-bit PLL R;
- texels taken nearest the sample point (texel centres on whole
  coordinates, where the DX truncates);
- perspective U/W and V/W with four more fraction bits.

SRM console and AlphaBIOS as on the DX (the same settled frames).
**Verified with Windows 2000**: `s3m` installs for it (DISPLAY.INF "S3
ViRGE"), desktop at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test
passes. `d3d_check.sh` gives the DX's matches and limits, plus two
differences of the driver's own:

- it starts U and V a hair lower than on the DX, so point-sampled samples
  that land exactly on a texel boundary take the texel below (ARGB8888 and
  ARGB4444 textures, texture alpha, colour key, perspective; the first row
  and column wrap);
- for bilinear filtering it adds -1/16 texel through TBU/TBV, which leaves
  the bilinear scene 1/16 texel off the reference.

**The ViRGE/VX** (`chip = "vx"`, 86C988, 5333:883D): VRAM, 2, 4 or 8 MB
(`memory`), CR36 memory straps as its data book gives them. Texture
coordinates as on the ViRGE.

- There is no S3 reference BIOS image. Diamond's Stealth 3D 3000 BIOS 1.00
  (`diamondstealth3000.vbi`, the default) and STB's Velocity 3D 1.10 both
  run as the SRM console (their own fonts, so their own settled frames)
  and under AlphaBIOS.
- **Verified with Windows 2000**: `s3m` installs ("S3 ViRGE/VX"), desktop
  at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test passes, with 4
  and with 8 MB (the miniport records 8 MB; dxdiag reports 4 MB, the
  display driver's view).
- `d3d_check.sh` as on the ViRGE, except that the VX's driver offers no
  overlay and no overlay colour keys. That is a choice by device ID: the
  capability flags `s3m.sys` sets for 883D lack two bits it sets for 5631
  and 8A01. So the overlay scenes cannot run, and the colour-keyed texture
  draws its key colour.

**The ViRGE/GX2** (`chip = "gx2"`, 86C357, 5333:8A10), 4 MB, with the S3
reference BIOS 2.16.13 (`flagpoint.VBI`, 40 KB, so a 64 KB ROM BAR);
Diamond's Stealth 3D 4000 BIOS (`86c357.bin`) runs too.

- PLL R in SR12 and SR29, one VCLK a 16-bit pixel, a power-management
  capability. SRM console and AlphaBIOS as on the DX.
- **Verified with Windows 2000**: `s3m` installs ("S3 ViRGE GX2"), desktop
  at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test passes.
- `d3d_check.sh`: better than the DX. Its driver places U and V exactly,
  so point, texture alpha, ARGB4444, colour key and modulate match where
  the DX's differ on texel boundaries; the rest as on the DX, the YUY2
  overlay included.
- The GX2's own streams processor layout and AGP are not modelled (the
  driver's desktop and overlay did not need them).

### Graphics: the DEC TGA

**The ZLXp-E1** (DECchip 21030 "TGA", 1011:0004, step B; 8 planes, 2 MB,
a Bt485 RAMDAC). Not a VGA, and no option ROM. Emulated:

- the 128 MB space of core-space copies: alternate ROM, registers, frame
  buffer;
- the 21030 manual's graphics modes: simple, stipple, fill, block, copy
  with the byte shifter, DMA-read and DMA-write copy, lines with the slope
  registers;
- the video timing registers, the Bt485 and its cursor, end-of-frame
  interrupts.

What runs on it:

- **SRM V7.3-1 lists it** in `show config` as `tga0.0.0.2.0` but has no
  console driver for it (its driver list has none and it never touches
  the card), so with `vga_console = true` the console stays on serial.
- **AlphaBIOS V5.71 draws on it** (640x480).
- **Verified with Windows 2000**: RC2's inbox `tga.sys`/`tga.dll` (V3.5-1,
  "Digital ZLXp-E1") run the desktop at 640x480, 800x600, 1024x768,
  1024x864, 1152x900 and 1280x1024 in 256 colours, the Bt485 drawing the
  pointer.
- `tga.dll` draws everything in simple mode, a Dword at a time with the
  one-shot pixel mask as the byte mask, so the other modes have no guest
  that checks them.
- The driver installs per PCI slot. The guest (`lab/tga-win`, slot 3) took
  it with the S3 beside the TGA, which a TGA allows: it then leaves the
  window to the VGA card.
- The 24-plane E2/E3 (Bt463) are not modelled.

**Start > Shut Down on a TGA ends on the cleared desktop** (1280x1024 and
640x480 tried, 256 colours), with no "It is now safe to turn off your
computer". Windows 2000 draws that screen with `bootvid.dll`, which knows
only VGA registers (its 16 colours: the S3 shows it in 12) and finds none
on a TGA, as on the real ZLXp-E1. The processor then spins at 100% in the
HAL's HalHaltSystem (`HALTSUMP.DLL` 0xa2b0: an endless loop; the
power-button status it would poll is skipped, the HAL having set up no
sleep state). It does so with the S3 too, so the spin is the halted
machine, not the card.

**The TGA2** (`model = "3d30"` or `"4d20"`; 1011:000d, rev 0x22): the
21030 core with an external-device window (ICS9110 clock, RAMDAC), a
rectangle engine and 128-byte copies.

- The 3D30 has 8 planes and a Bt485; the 4D20 32-bit pixels, 16 MB and an
  IBM RGB561.
- Windows 2000 RC2's `tga2.sys`/`tga2.dll` run the desktop. The 3D30: 640x480
  and 1280x1024 in 256 colours. The 4D20: 640x480 and 1600x1200 in 32 bpp.
- Neither console drives it: it runs beside a VGA card, as the real board
  has its own Cirrus VGA. SRM lists it as "DEC PowerStorm".

### Graphics: the ATI Radeon 7500

The ATI Radeon 7500 (RV200, 1002:5157), the card HP shipped in the
ES47/ES80/GS1280. Its class takes `chip = "rv200"` (the default and only
part so far) and `model = "agp"` (the default) or `"pci"`; the consoles
tell the AGP and PCI boards apart by subsystem (1002:013A, 1002:013B).
[radeon.md](radeon.md) describes the model in full.

The board: 32 or 64 MB; a 128 MB framebuffer BAR, the I/O and 64 KB
register BARs, a 128 KB ROM, AGP 2.0 and power-management capabilities.
Emulated:

- the register file the BIOS programs: PLLs, memory controller, CRTC, DAC;
- standard VGA, the primary CRTC's extended modes, the hardware cursor
  (mono and ARGB), the DDC monitor;
- the overlay scaler: a scaled YUV or RGB video window with colour keys;
- the 2D engine: every ROP3, mono and colour brushes, memory, mono and
  host sources, clipping, write mask, colour compare, lines with dash
  patterns;
- the command processor: ring buffer, indirect buffers, programmed-I/O
  queue, the 2D packets;
- the 3D engine: TCL (transform, lighting, fog, texture matrices,
  clipping), the setup engine and rasteriser, three texture units (14
  formats including DXT, mip-mapping, the wrap modes), the combiners,
  alpha/stencil/Z tests, blending. It is emulated from the open drivers
  (Mesa's r100, X.org's composite).

How each part is checked:

- **A guest** has checked the CP ring and indirect buffers, solid and 8x8
  mono pattern fills, mono host-data expansion (glyph runs in
  `CNTL_HOSTDATA_BLT` packets) and the mono cursor.
- **The self-test** (`ALPHABOX_RADEON_SELFTEST=1`) checks the rest of the
  2D engine, the CP's other packets and the ARGB cursor against ATI's Rage
  128 Pro register guide and AMD's PM4 packet chapter, and the 3D engine
  with its scenes.
- **nada's Windows 2000 driver** drives the 3D engine through Direct3D
  ([guest-drivers.md](guest-drivers.md)).

Consoles and guests:

- The RV200 BIOS of 2002/04/16 runs under the ES40's SRM (console on the
  card) and AlphaBIOS.
- Under the ES47's SRM in the AGP slot (`pci3.5`), `show config` lists it
  as the real machine does ("Radeon 7500 AGP", `vga0.0.0.5.3`), and
  `console graphics` puts the SRM console on it. In the ES45 Model 1's
  AGP slot (`pci2.5`) it is the SRM graphics console too.
- **OpenVMS 8.4's DECwindows** (device GH) draws the CDE login box on it at
  1024x768 in 24 bpp, through the command processor
  ([openvms.md](openvms.md#graphics)).
- **No Alpha Windows has a driver for it**: Windows 2000 RC2's driver
  cabinet has only the Mach64 family, and Whistler 2210's DISPLAY.INF
  lists only the Rage 128 Pro, commented out. Windows 2000 and Whistler
  boot to the desktop on the standard VGA driver at 640x480 in 16 colours,
  with the boot splash and the shut-down screen, and offer the card as an
  unknown "Video Controller (VGA Compatible)",
  `PCI\VEN_1002&DEV_5157&SUBSYS_013A1002`.

### Sound

- **ES1371**: the AudioPCI 97, an AC'97 codec on a serial link, and a
  sample rate converter where the ES1370 had fixed rates. This is the only
  audio part the console's table names (1274:1371). Windows 2000 binds
  `es1371mp.sys` to it, though only after its INF is given an NT install
  section: the one on the Alpha media is decorated `.NTX86` and so matches
  nothing here.
- A machine reset (the console's `init`, a guest reboot) puts either card
  back to its power-on state: the DMA channels stop, the interrupt latch and
  the PCI interrupt line clear, and the host streams are released. A
  snapshot keeps the card: its registers, the converter's RAM, the codec's
  registers and each channel's DMA position are saved, and a restore
  rebinds the host streams, so a tone that was playing carries on. A state
  file from before the card was saved holds nothing for it, and restores
  with the card at power-on, as it always did.
- The ADC's stop bit (control register bit 31 on the ES1370, bit 13 on the
  ES1371) holds off the record transfers, as the specifications say; the
  sample ring's end is counted from the byte the next transfer starts at, so
  a mono or 8-bit stream no longer reads or writes past the guest's ring.
- **ES1370**: the part before it, with a mixer of its own. The console has
  no table entry for 1274:5000, so `show config` prints only its ids.

### The ES47's on-board USB and IDE

- **USS-344** (`uss344`): the Agere (Lucent) USS-344 QuadraBus, the `ehci`
  card's OHCI functions without the EHCI, four of them (11C1:5803, one
  root hub port each, all on INTA). The ES47 console lists them as
  usba..usbd, as the real machine does; OpenVMS 8.4 configures OHA0..OHC0
  ([usb.md](usb.md)).
- **CMD 649** (`cmd649`): the ES47's on-board IDE (1095:0649, both channels
  native, INTA), on the same ATA/ATAPI core as `ali_ide`.

### Add-in and paravirtual devices

- **EHCI** (`ehci`): a USB 2.0 card presenting itself as a NEC uPD720101:
  EHCI (function 2, 1033:00e0) with two OHCI companions (functions 0-1,
  1033:0035) for full-speed devices, and four ports (`companions = false`:
  the EHCI alone). No Alpha Windows ships a driver; nada's `nadaehci.sys`
  drives it on Windows 2000 ([usb.md](usb.md)).
- **virtio** (`virtio_blk`, `virtio_net`): the legacy virtio-pci block
  device (1AF4:1001) and NIC (1AF4:1000). No Alpha operating system ships
  a driver; nada's `nadavblk.sys` and `nadavnet.sys` drive them on Windows
  2000. [virtio.md](virtio.md) is the reference for writing one.

## Candidates, by value

"Guests" names operating systems known to ship a driver. Effort is a rough
guess:

- **S** is a variant of something already emulated;
- **M** a new but well-documented chip with an open reference model;
- **L** a chip with on-board firmware or a large command set.

### 1. Cheap variants of what exists

| Firmware name | Effort | Why |
| --- | --- | --- |
| NCR 53C895A | S | the same `n810` driver, and the same 256-byte register window and 8 KB of RAM as the 896; a single-channel part, so mostly a table row. No driver on Windows 2000's Alpha media binds to its id (`DEV_0012`), so only the console would exercise it |
| DE500-AA/-FA/-XA | S–M | named boards built on the 21140 and the 21143: a subsystem id the console recognises, and, for the 21140 boards, a real MII PHY on the general purpose port's reset pin |
| ATI Radeon AGP (1002:5144) | S | the first Radeon, of the R100 generation the model has: a chip row ([radeon.md](radeon.md#adding-a-generation-or-a-part)) |
| CMD PCI0646 IDE | S | a `cmd649` row (the 0646's own revision and interrupt bits) |

### 2. More ports for what exists

| Firmware name | Effort | Why |
| --- | --- | --- |
| DE602-F*/-T* (DE602 add-on modules) | S | extra ports for a DE602 |
| DE504-BA and other quad 21143 boards | S | four `dec21143` behind a bridge: already possible by hand; a board class would name them |

### 3. Storage beyond SCSI-2 parallel

| Firmware name | Effort | Why |
| --- | --- | --- |
| Adaptec AIC-7880/7891/7892/7895/7897/7899, 2940UW/U2W, 29160 (`aic78xx`) | L | bootable; NT, NetBSD, Linux, OpenVMS; the ES47's and DS25's on-board SCSI ([below](#the-adaptec-aic-78xx)) |
| Mylex DAC960, AcceleRAID 150, eXtremeRAID 1100/2000 (`dac960`) | L | bootable RAID; Tru64, OpenVMS (some), NT |
| HP/Compaq Smart Array 5300A, 5312A, 641A, 642A, 6400 | L | the late-life RAID adapters |
| KGPSA (Emulex), QLogic QLA2100/2200/2300, LSI FC909/919/929, FCA-2354/2384/2684 | L | Fibre Channel; needed for SAN-style cluster setups |
| DEC KZPSA (`vip7407`), DEC KFPSA / 53C825 DSSI (`n825_dssi`) | L | legacy FWD SCSI and DSSI |

#### The Adaptec AIC-78xx

OpenVMS's `SYS$PKADRIVER` lists 9005:008F as "Adaptec AIC-7892". The
chips are sequencer-driven, and each driver downloads its own sequencer
program: the console's is Adaptec's CHIM (`scsihloadsequencer`, 138 KB of
console code), OpenVMS's its own. So an emulation has to run the
sequencer (its instruction set, the SCB RAM, the queue FIFOs, the SCSI
bus phases at REQ/ACK level), not a command interface.

The DS25 has a configuration-space stand-in (`Ds25Aic7899`); the ES47
keeps a `sym53c895` in its slot. It is item A1 of
[backlog.md](backlog.md#the-plan).

### 4. Networking beyond Ethernet

| Firmware name | Effort | Why |
| --- | --- | --- |
| DEGPA-SA/-TA (Alteon), DEGXA / BCM5703 (Broadcom) | L | gigabit Ethernet; on-board firmware (DEGPA) or a large register set. The BCM5703c is the DS25's on-board chip (N1 in [backlog.md](backlog.md#the-plan)) |
| DEC PCI FDDI (`DEFPA`) | L | bootable, but needs an FDDI peer to be useful |
| ATMworks 350/351, Fore ATM, DAPBA/DAPCA | L | ATM; needs a network to talk to |
| PBXNP Token Ring, EssCom HiPPI, QSW ELan3 | L | niche |
| CIPCA (`cipca`) | L | CI cluster adapter; OpenVMS clusters |
| DEC PCI MC (Memory Channel) | L | TruCluster interconnect |

### 5. Graphics beyond what exists

| Firmware name | Effort | Why |
| --- | --- | --- |
| Permedia P2V, PowerStorm 300/350/350D | L | 3Dlabs-class cards beyond the Permedia 2; DECwindows/CDE 3D |
| 3D Labs OXYGEN VX1 (PCI/AGP) | L | a late-life option; the AGP board fits the ES45 Model 1 and the EV7 machines, not the ES40 |
| S3 Trio864, S3 Savage 4, ATI Mach32, Digital 2T-PMPAA/Pixelwork, DEC PV-PCI | M–L | rare on ES40 |
| Cateyes 4D51T | L | niche |

### 6. Everything else

- Serial and sync: Systech EtPlex, Arnet Sync 570, DataFire SYNC.
- PCMCIA/CardBus bridges: Cirrus PD6729, PCI to CardBus/PC Card.
- DEC PCI NVRAM/Prestoserve (`pci_nvram`), DEC RT clock, AXL300
  accelerator, 3X-KPCON fault management.
- Bridges: Tundra Universe II (VME), IBM PCI-X and BIT 3 bridges, Intel
  82375/82378 (EISA/ISA bridges of other Alpha systems); the Yukon
  hot-plug controller.
- Cypress 82C693 and CMD CSA-6730 USB (other Alpha systems).
- DPT PM3755 and AMI 431 RAID, Compaq 2000/P and 1280/P, Toshiba Meteor.

Few of these would change what a guest can do on an emulated ES40.

Other machines are a different axis of work: see [platforms.md](platforms.md)
and [backlog.md](backlog.md).

## What was done, and what comes next

The order this page once suggested is done:

1. ~~GD5430 variant and the Cirrus BitBLT engine~~, both directions of
   host transfer: a screen-to-system blit hands the rectangle back through
   the aperture a line at a time.
2. ~~53C8xx variants~~: the 825, 875, 895 and the two-channel 896. Only the
   895A remains, and no Windows 2000 driver binds to it.
3. ~~Intel 8255x~~: the DE600 and Intel's 82557/82558/82559 boards, and
   later the 82559ER.
4. ~~PCI-PCI bridges~~: the 21050/21052/21152/21153/21154 and the DE602
   boards, forwarding windows and all (see below).
5. ~~QLogic ISP1040 (KZPBA)~~: the console and Windows 2000's own QLogic
   driver both drive it. The ISP1080 and the dual-bus ISP1240 came later
   and are driven by Windows alone (see below).
6. ~~Older Tulips~~: the 21040, 21041 and 21140 (see below).
7. ~~TGA (ZLXp 21030)~~, and the TGA2: Windows 2000 draws on both, and
   AlphaBIOS on the ZLXp-E1; OpenVMS has not been tried on them.

Beyond the list came the Mach64 family, the Permedia 2, the S3 ViRGE
family, the Radeon 7500, the Ensoniq ES1370 and ES1371, the CMD 649, the
USS-344, the EHCI card and virtio.

What comes next is in the candidate tables above, cheapest first, and in
the device items of [backlog.md](backlog.md#the-plan) (A1, the
Adaptec AIC-78xx sequencer; N1, the BCM5703c; H1, the hot-plug
controllers), each its own decision. No guest needs those: OpenVMS runs
on the stand-ins.

Notes on the finished items:

- **Bridges**: a device behind a bridge is reached because the bridge's
  I/O, memory or prefetchable window covers it. The aliases of the VGA I/O
  ranges, which a real bridge decodes with the address bits above bit 9
  as don't-cares, are not claimed.
- **The ISP1080 and ISP1240**: the ES40 console's PCI table has no entry
  for either device id, so it cannot name them or boot from them whatever
  the emulation does.
- **The older Tulips** join the 21143 as parts of one family, each with
  the identity and media machinery it really had: a parallel address ROM
  read through CSR9, the older serial ROM format, a general purpose port
  in place of a SIA. The console boots over all four.
- **What remains of the Tulips** is a 21140 board with an MII PHY behind
  it, and that one waits for a driver that would exercise it. A
  transceiver was written and thrown away again after measurement showed
  that neither the console nor Windows 2000 ever reads a PHY register on
  this machine. Every CSR9 write the console makes has the management
  clock low: it is talking to the serial ROM, never to a transceiver.
  Verifying an MII PHY needs NetBSD or Tru64, whose drivers do probe it.

Each new device gets its own directory under `src/devices/<bus>/` (as
`video/s3/` and `video/cirrus/` do), split by concern, and is verified
against the real firmware or guest driver that names it.

## Some devices the console can never see

The SRM console does not discover what a card is. It looks the PCI vendor
and device id up in a fixed table built into the firmware (V7.3-1 keeps
it at offset 0x140408 of the decompressed image: 186 records of 28 bytes,
each a vendor+device, a subsystem, names and a console driver). A card whose id is
not in that table is still configured: it gets its BARs and its
interrupt, and a guest driver can drive it normally. But `show config`
prints the bare numeric id, there is no console driver attached, and the
console cannot boot from it.

This is a property of the firmware, not of the emulation, and no amount
of work on a device model changes it. The ES40 of 2007 knew the adapters
that an ES40 shipped with; parts that arrived later, or that belonged to
other machines, are simply absent.

- Two of ours are in that position: the QLogic **ISP1080** (`1077:1080`)
  and **ISP1240** (`1077:1240`). The table's only QLogic SCSI record is
  the ISP1020, plus the Fibre Channel parts. Windows 2000 drives both
  perfectly well with `ql1080` and `ql1240`; SRM will never name them or
  boot them.
- The same table explains why the bare 21040, 21041 and 21140 print their
  chip names while the 21143 prints "DE500-BA": only rows carrying a
  board's subsystem id (0x500a, 0x500b, 0x500f) have a board name to
  print.

The practical consequence is a rule for what to build next: **a device
that the console cannot name has to be verified against a guest driver,
because the cheap console test does not exist for it.** Where no guest we
can run has a driver either (as with the 53C895A, which no driver on the
Windows 2000 Alpha media binds to), there is no way to verify the work at
all, and it is better not to start it.

**Verify against a guest driver, not only the console.** The console is
undemanding: it drove the QLogic adapter while three things were wrong
that Windows would not tolerate:

- a missing self-identification after reset;
- two mistakes in the queue entries that carry the buffer segments of a
  large transfer, which the console never exercised because it only ever
  issues single-segment commands.

Each of them was invisible until a guest driver ran. `win_storage.sh` (a
file copy inside Windows) is the cheapest such test for a storage
controller.

## What a guest driver's traffic shows: the 2026-09 audit

A Windows 2000 cold boot was profiled by device register (`JIT_STATS`, the
device-address histogram in [performance.md](performance.md)). The device
models were then read against their data sheets for the class of bug the
profile pointed at: a device that answers a question the real part
answers differently, so the driver asks again, millions of times. The
guest's time goes on the asking, and the emulator cannot see why.

Found and fixed, each with what the driver was doing:

- **Unclaimed PCI I/O read 00h instead of FFh.** The parallel-port class
  driver negotiated IEEE 1284 against port 0x3BC, which this board does
  not wire, and a peripheral that answers 00h to everything is one that
  says "present and ready" to every question: 18M status reads per boot.
  An idle bus floats high; the read now returns all ones and the probe
  finds no port.
- **The UART did not identify its FIFO.** IIR bits 7:6 now reflect FCR
  bit 0, so a driver sees a 16550 rather than a 16450 (an interrupt and
  two register reads per character otherwise); FCR bit 1 flushes the
  receive ring.
- **The IDE disk aborted SET FEATURES 02h/66h**, which atapi.sys issues at
  every device start. Accepted now, like a drive. It was not the reason the
  guest runs PIO (see the DMA note in performance.md): the generic Alpha
  pciide driver picks PIO 4 on its own.
- **The SCSI disk offered no tagged queuing** and aborted the transfer on
  any message it did not know. INQUIRY now reports CmdQue and Sync, the
  queue-tag, abort, reset and no-op messages are accepted, and anything
  else gets MESSAGE REJECT. The QLogic adapter's GET TARGET PARAMS returns
  its flags in the byte the driver reads.
- **The port 0x61 refresh toggle** is real time, so a HAL stall loop that
  counts its flips reads it 19M times in a second. Not a fidelity bug, but
  paced: after 64 back-to-back reads the port sleeps the caller to the next
  edge (same wall time, `ALPHABOX_PORT61_PACE=0` to spin).
- **The S3's drawing engine reported busy once after every command**, so
  the display driver's idle wait always re-polled. The input status
  register's display-enable bit only pulsed at vertical retrace, so a
  driver syncing to horizontal blank waited a frame. And the PCI status
  word advertised a capability list and a parity error the card cannot
  have. All three corrected to what an 86C764 reports.

Found by reading rather than by traffic, and corrected in the same pass:

- **8254 read-back command** (control word with bits 7:6 set) was not
  modelled. It now latches the status and the count of every selected
  counter, and the status byte (OUT, NULL COUNT, read/write mode, mode,
  BCD) comes out on the next read before any latched count.
- **OHCI never wrote the HCCA.** The controller now posts the frame number
  (and a zero done head) into the block the driver gave it, whenever the
  driver touches a register and the number has moved; a driver that checks
  the HCCA to see the controller alive sees it advance. (Since superseded:
  the OHCI now runs its whole schedule on a frame thread, with devices on
  its ports; see [usb.md](usb.md).)
- **SMBus host** on the PMU function had the PIIX4's status bits, not the
  M7101's. It now runs the M7101 protocol: IDLE, a transaction started by
  the start register completes at once with DONE and a device error, since
  the emulated bus carries no device; a probe finds nothing and moves on.

Nothing from the audit remains open.
