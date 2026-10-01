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

| Firmware name | Alphabox class | Notes |
| --- | --- | --- |
| Acer Labs M1543C | `ali` | ISA bridge: PIT, PIC, DMA, RTC/TOY, keyboard/mouse, COM1/COM2, LPT1, floppy |
| Acer Labs M1543C IDE | `ali_ide` | bootable (IDE disks and CD-ROMs) |
| Acer Labs M1543C USB | `ali_usb` | OHCI 1.0a, three ports: the schedule, the root hub, and USB devices on it -- a tablet, mass storage, passed-through host devices ([usb.md](usb.md)). Windows 2000 and Whistler drive it with their own USB stacks |
| Acer Labs M1543C PMU | `ali_pmu` | |
| NCR 53C810 | `sym53c810` | `n810` console driver: bootable |
| NCR 53C825 (825A) | `sym53c825` | `n810` console driver: bootable; wide, 4 KB SCRIPTS RAM |
| NCR 53C875 | `sym53c875` | `n810` console driver: bootable; Ultra-Wide, 4 KB SCRIPTS RAM |
| NCR 53C895 | `sym53c895` | `n810` console driver: bootable; Ultra2-Wide, 4 KB SCRIPTS RAM |
| NCR 53C896 | `sym53c896` | `n810` console driver: bootable on both channels; two Ultra2-Wide cores as PCI functions 0 and 1, 8 KB SCRIPTS RAM each, 256-byte register file with the phase-mismatch jump block. Windows 2000 drives it with `sym_hi`, not the `symc8xx` of the single-channel parts |
| QLogic ISP1020, ISP1040 (KZPBA) | `isp1020`, `isp1040` | `isp1020` console driver: bootable; Windows 2000 drives it with its own QLogic driver (`ql10wnt`) |
| QLogic ISP1080 | `isp1080` | Ultra2 Wide, low-voltage differential. The console does not know PCI device 0x1080 -- its table carries QLogic 0x1020 and the Fibre Channel parts and nothing else -- so `show config` prints the raw id and it is not bootable. Windows 2000 drives it with `ql1080` |
| QLogic ISP1240 | `isp1240` | two Ultra Wide buses on one PCI function (not two functions, the way the 53C896 is built): one set of queues and mailboxes, and a command entry names its bus in the top bit of the target byte. Disks are `disk0.<id>` and `disk1.<id>`. Unknown to the console for the same reason as the 1080; Windows 2000 drives it with `ql1240` |
| DECchip 21143-AA / DE500-BA | `dec21143` | Tulip console driver: network boot |
| DECchip 21140-AA | `dec21140` | same driver; no SIA, media through the general purpose port. The board described has nothing wired to those pins and no MII PHY |
| DECchip 21041-AA | `dec21041` | same driver; 10 Mb SIA, the older serial ROM format. The console drives it at 10BaseT from its own table and reads only the station address out of the ROM |
| DECchip 21040-AA | `dec21040` | same driver; 10 Mb SIA, and no serial ROM at all: the station address comes out of a parallel ROM read a byte at a time through CSR9 |
| DE600-AA (Intel 82559), Intel 8255x Ethernet | `de600`; `i82557`, `i82558`, `i82559` | `ei` console driver: network boot, loopback self-test |
| DE602-AA, DE602-B* (two 8255x behind a bridge) | `de602`, `de602b` | `ei` console driver: network boot on either port |
| DECchip 21050-AA, 21052-AA, 21152-AA, 21153-AA, 21154-AA | `dec21050` ... `dec21154` | PCI-PCI bridges; the console numbers and probes the buses behind them, nested too, and what the forwarding windows cover is what is reachable; Windows 2000 drives a 53C810 behind one |
| S3 Trio64/Trio32 | `s3` | `vga_bios`: console, ARC/AlphaBIOS, Windows NT |
| Cirrus CL-GD5430 | `cirrus`, `chip = "gd5430"` | `vga_bios`: console |
| Cirrus CL-GD5434 | `cirrus` | `vga_bios`: console; Windows 2000 draws its desktop through the BitBLT engine |
| ATI Mach64 (CT) | `mach64` | `vga_bios`: console. The CT is the first Mach64 with its DAC and clock synthesizer on the chip; emulated are its register file, the GUI drawing engine (rectangles, lines, screen and host sources, patterns, the sixteen mixes, colour compare), the hardware cursor and the extended display modes at 4 to 32 bpp. **Verified with Windows 2000**, which names it "ATI Technologies Inc. mach64 CT PCI", binds its own inbox miniport (`ati.sys`/`ati.dll`) with no files supplied, reports the device working, and after the restart that installation asks for runs the desktop in eight-bit colour through the drawing engine -- six million pixels over four thousand commands to open one window (`ALPHABOX_BLIT_STATS=1`). Before the restart it is the sixteen-colour VGA fallback, which is what a card with no driver yet looks like. The driver's own mode list works, **up to 32 bpp** -- which is the part the console cannot reach at all, since it exercises the extended CRTC, the DAC in a packed four-byte layout and the engine at four bytes to the pixel. A session of mode changes and window dragging drew 12.2M pixels over 7041 engine commands at 6.4 ns/pixel |
| (not named) | `mach64`, `chip = "vt2"` | the 264VT2: the CT's register file plus a video overlay (stored, not drawn), PCI id 1002:5654, for which the console has no table entry -- `show config` prints only its ids |
| (not named) | `s3virge` (`chip = "dx"`) | the S3 ViRGE/DX (86C375, 5333:8A01): the S3 SVGA with its RAMDAC and clock synthesiser, the streams processor (24-bit packed modes, a scaled YUV/RGB overlay) and the S3d engine -- 2D BitBLT/fill/line/polygon and 3D triangles with Z buffer, textures, fog and alpha blending. The console has no table entry for it but runs its BIOS (S3 reference BIOS 2.01.16) and uses it as its `vga_bios` console; AlphaBIOS draws on it. **Verified with Windows 2000**: it installs from the RC2 media with this card as its only display (AlphaBIOS, text-mode setup, and GUI setup, whose device detection installs `s3m` and the monitor it reads over DDC); in an installed system the inbox `s3m.sys`/`s3mtrio.dll`/`s3mvirge.dll` install when the card is found and, after a restart, run the desktop at 640x480 to 1024x768 in 8, 16 and 24 bpp (the driver offers no 32 bpp on the DX); dxdiag reports DirectDraw and Direct3D acceleration enabled and its Direct3D test passes on the hardware. `d3d_check.sh` (30 scenes against Direct3D's software rasteriser, 16-bit desktop): Gouraud, flat, fog, alpha blending, Z buffer, bilinear, ARGB8888 textures, points and clipping match; nearest-texel scenes (point, modulate, perspective, texture alpha, ARGB4444, colour key) differ only on texel boundaries, which the driver rounds up (it biases U and V by 1/256 texel) where the reference rounds down; the rest are the chip's or the driver's limits -- no alpha test, no texture clamping, no specular colour (the driver fogs towards white instead), square textures only, no mipmaps from the driver, no multitexture, lines drawn as thin triangles whose colour does not follow the line. The YUY2 overlay matches; there is no planar YV12 overlay. dxdiag's DirectDraw test passes too. The `gx` row (the DX's id, revision 1) has not been run on its own |
| (not named) | `s3virge`, `chip = "virge"` | the original S3 ViRGE (86C325, 5333:5631), with the S3 reference BIOS 1.00-10 (`86c325.bin`). What sets it apart from the DX, as the ViRGE data book and the Windows driver's register values have it: 2-bit PLL R; texels taken nearest the sample point (texel centres on whole coordinates, where the DX truncates), and perspective U/W and V/W with four more fraction bits. SRM console and AlphaBIOS as on the DX (the same settled frames). **Verified with Windows 2000**: `s3m` installs for it (DISPLAY.INF "S3 ViRGE"), desktop at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test passes. `d3d_check.sh`: the DX's matches and limits, plus two differences of the driver's own: it starts U and V a hair lower than on the DX, so point-sampled samples that land exactly on a texel boundary take the texel below (ARGB8888 and ARGB4444 textures, texture alpha, colour key, perspective; the first row and column wrap), and for bilinear filtering it adds -1/16 texel through TBU/TBV, which leaves the bilinear scene 1/16 texel off the reference |
| (not named) | `s3virge`, `chip = "vx"` | the S3 ViRGE/VX (86C988, 5333:883D): VRAM, 2, 4 or 8 MB (`memory`), CR36 memory straps as its data book gives them. There is no S3 reference BIOS image; Diamond's Stealth 3D 3000 BIOS 1.00 (`diamondstealth3000.vbi`, the default) and STB's Velocity 3D 1.10 both run as the SRM console (their own fonts, so their own settled frames) and under AlphaBIOS. Texture coordinates as on the ViRGE. **Verified with Windows 2000**: `s3m` installs ("S3 ViRGE/VX"), desktop at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test passes, with 4 and with 8 MB (the miniport records 8 MB; dxdiag reports 4 MB, the display driver's view). `d3d_check.sh` as on the ViRGE, except that the VX's driver offers no overlay and no overlay colour keys (a choice by device ID: the capability flags `s3m.sys` sets for 883D lack two bits it sets for 5631 and 8A01), so the overlay scenes cannot run and the colour-keyed texture draws its key colour |
| (not named) | `s3virge`, `chip = "gx2"` | the S3 ViRGE/GX2 (86C357, 5333:8A10), 4 MB, with the S3 reference BIOS 2.16.13 (`flagpoint.VBI`, 40 KB, so a 64 KB ROM BAR); Diamond's Stealth 3D 4000 BIOS (`86c357.bin`) runs too. PLL R in SR12 and SR29, one VCLK a 16-bit pixel, a power-management capability. SRM console and AlphaBIOS as on the DX. **Verified with Windows 2000**: `s3m` installs ("S3 ViRGE GX2"), desktop at 640x480 and 800x600 in 16 bpp, dxdiag's Direct3D test passes. `d3d_check.sh`: better than the DX -- its driver places U and V exactly, so point, texture alpha, ARGB4444, colour key and modulate match where the DX's differ on texel boundaries; the rest as on the DX, the YUY2 overlay included. The GX2's own streams processor layout and AGP are not modelled (the driver's desktop and overlay did not need them) |
| DECchip ZLXp 21030 | `tga` (`model = "e1"`) | the DEC ZLXp-E1 (DECchip 21030 "TGA", 1011:0004, step B; 8 planes, 2 MB, a Bt485 RAMDAC). Not a VGA, no option ROM: the 128 MB space of core-space copies (alternate ROM, registers, frame buffer), the 21030 manual's graphics modes (simple, stipple, fill, block, copy with the byte shifter, DMA-read and DMA-write copy, lines with the slope registers), the video timing registers, the Bt485 and its cursor, end-of-frame interrupts. **SRM V7.3-1 lists it** in `show config` as `tga0.0.0.2.0` but has no console driver for it -- its driver list has none and it never touches the card -- so with `vga_console = true` the console stays on serial. **AlphaBIOS V5.71 draws on it** (640x480). **Verified with Windows 2000**: RC2's inbox `tga.sys`/`tga.dll` (V3.5-1, "Digital ZLXp-E1") run the desktop at 640x480, 800x600, 1024x768, 1024x864, 1152x900 and 1280x1024 in 256 colours, the Bt485 drawing the pointer. `tga.dll` draws everything in simple mode, a Dword at a time with the one-shot pixel mask as the byte mask, so the other modes have no guest that checks them. The driver installs per PCI slot; the guest (`lab/tga-win`, slot 3) took it with the S3 beside the TGA, which a TGA allows: it then leaves the window to the VGA card. The 24-plane E2/E3 (Bt463) are not modelled. **Start > Shut Down on a TGA ends on the cleared desktop** (1280x1024 and 640x480 tried, 256 colours), with no "It is now safe to turn off your computer": Windows 2000 draws that screen with `bootvid.dll`, which knows only VGA registers (its 16 colours: the S3 shows it in 12) and finds none on a TGA, as on the real ZLXp-E1. The processor then spins at 100% in the HAL's HalHaltSystem (`HALTSUMP.DLL` 0xa2b0: an endless loop; the power-button status it would poll is skipped, the HAL having set up no sleep state) -- with the S3 too, so the spin is the halted machine, not the card |
| Ensoniq Sound Card | `es1371` | the AudioPCI 97: an AC'97 codec on a serial link, and a sample rate converter where the ES1370 had fixed rates. This is the only audio part the console's table names (1274:1371); Windows 2000 binds `es1371mp.sys` to it, though only after its INF is given an NT install section -- the one on the Alpha media is decorated `.NTX86` and so matches nothing here |
| (not named) | `es1370` | the part before it, with a mixer of its own. The console has no table entry for 1274:5000, so `show config` prints only its ids |
| (none: an add-in card) | `ehci` | a USB 2.0 card presenting itself as a NEC uPD720101: EHCI (function 2, 1033:00e0) with two OHCI companions (functions 0-1, 1033:0035) for full-speed devices; four ports (`companions = false`: the EHCI alone). The console does not know it; no Alpha Windows ships a driver -- nada's `nadaehci.sys` drives it on Windows 2000 ([usb.md](usb.md)) |
| (none: paravirtual) | `virtio_blk`, `virtio_net` | legacy virtio-pci block device (1AF4:1001) and NIC (1AF4:1000). The console lists them by ID only; no Alpha operating system ships a driver; nada's `nadavblk.sys` and `nadavnet.sys` drive them on Windows 2000 -- [virtio.md](virtio.md) is the reference for writing one |

## Candidates, by value

"Guests" names operating systems known to ship a driver. Effort is a rough
guess: **S** is a variant of something already emulated, **M** a new but
well-documented chip with an open reference model, **L** a chip with
on-board firmware or a large command set.

### 1. Cheap variants of what exists

| Firmware name | Effort | Why |
| --- | --- | --- |
| NCR 53C895A | S | same `n810` driver, and the same 256-byte register window and 8 KB of RAM as the 896 that is done; a single-channel part, so mostly a table row. Windows 2000's Alpha media carries no driver bound to its id (`DEV_0012`), so only the console would exercise it |
| DE500-AA/-FA/-XA | S–M | named boards built on the 21140 and the 21143: a subsystem id the console recognises, and, for the 21140 boards, a real MII PHY on the general purpose port's reset pin |

### 2. New devices with high payoff

| Firmware name | Effort | Why |
| --- | --- | --- |
| DECchip ZLXp 21030 (TGA) | L | DEC's own 2D/3D workstation graphics; DECwindows/CDE on OpenVMS and Tru64 expect it; NetBSD has a driver |
| DE602-F*/-T* (DE602 add-on modules) | S | extra ports for a DE602 |
| DE504-BA and other quad 21143 boards | S | four `dec21143` behind a bridge: already possible by hand; a board class would name them |

### 3. Storage beyond SCSI-2 parallel

| Firmware name | Effort | Why |
| --- | --- | --- |
| Adaptec AIC-7880/7891/7892/7895/7897/7899, 2940UW/U2W, 29160 (`aic78xx`) | L | bootable; NT, NetBSD, Linux. Sequencer-driven; the free Unix drivers document it well |
| Mylex DAC960, AcceleRAID 150, eXtremeRAID 1100/2000 (`dac960`) | L | bootable RAID; Tru64, OpenVMS (some), NT |
| HP/Compaq Smart Array 5300A, 5312A, 641A, 642A, 6400 | L | the late-life RAID adapters |
| CMD PCI0646 / 649 IDE | M | alternative IDE; little gain while the M1543C IDE works |
| KGPSA (Emulex), QLogic QLA2100/2200/2300, LSI FC909/919/929, FCA-2354/2384/2684 | L | Fibre Channel; needed for SAN-style cluster setups |
| DEC KZPSA (`vip7407`), DEC KFPSA / 53C825 DSSI (`n825_dssi`) | L | legacy FWD SCSI and DSSI |

### 4. Networking beyond Ethernet

| Firmware name | Effort | Why |
| --- | --- | --- |
| DEGPA-SA/-TA (Alteon), DEGXA / BCM5703 (Broadcom) | L | gigabit Ethernet; on-board firmware (DEGPA) or a large register set |
| DEC PCI FDDI (`DEFPA`) | L | bootable, but needs an FDDI peer to be useful |
| ATMworks 350/351, Fore ATM, DAPBA/DAPCA | L | ATM; needs a network to talk to |
| PBXNP Token Ring, EssCom HiPPI, QSW ELan3 | L | niche |
| CIPCA (`cipca`) | L | CI cluster adapter; OpenVMS clusters |
| DEC PCI MC (Memory Channel) | L | TruCluster interconnect |

### 5. Graphics beyond VGA

| Firmware name | Effort | Why |
| --- | --- | --- |
| ELSA GLoria Synergy, Permedia P2V, PowerStorm 300/350/350D, 4D10 | L | 3Dlabs Permedia-class cards; DECwindows/CDE 3D |
| 3D Labs OXYGEN VX1 (PCI/AGP), Radeon 7500, ATI Radeon AGP | L | late-life options; the ES40 has no AGP slot, so only the PCI variants |
| S3 Trio864, S3 Savage 4, ATI Mach32, Digital 2T-PMPAA/Pixelwork, DEC PV-PCI | M–L | rare on ES40 |
| Cateyes 4D51T | L | niche |

### 6. Everything else

Serial/sync (Systech EtPlex, Arnet Sync 570, DataFire SYNC), PCMCIA/CardBus
bridges (Cirrus PD6729, PCI to CardBus/PC Card), DEC PCI NVRAM/Prestoserve
(`pci_nvram`), DEC RT clock, AXL300 accelerator, 3X-KPCON fault management,
Tundra Universe II (VME), Yukon hot-plug controller, IBM PCI-X and BIT 3
bridges, Intel 82375/82378 (EISA/ISA bridges of other Alpha systems),
Cypress 82C693 and CMD CSA-6730 USB (other Alpha systems), DPT PM3755 and
AMI 431 RAID, Compaq 2000/P and 1280/P, Toshiba Meteor. Few of these would
change what a guest can do on an emulated ES40.

Other machines (a DS20E, an ES45, an EV7 system) are a different axis of
work: see [platforms.md](platforms.md).

## Suggested order

1. ~~GD5430 variant and the Cirrus BitBLT engine~~ (done, both directions of
   host transfer: a screen-to-system blit hands the rectangle back through
   the aperture a line at a time).
2. ~~53C8xx variants~~: the 825, 875, 895 and the two-channel 896 are
   done; only the 895A remains, and no Windows 2000 driver binds to it.
3. ~~Intel 8255x~~: the DE600 and Intel's 82557/82558/82559 boards are
   done; the dual-port DE602 waits for the bridges.
4. ~~PCI-PCI bridges~~: the 21050/21052/21152/21153/21154 and the DE602
   boards are done, forwarding windows and all: a device behind a bridge is
   reached because the bridge's I/O, memory or prefetchable window covers
   it. The aliases of the VGA I/O ranges, which a real bridge decodes with
   the address bits above bit 9 as don't-cares, are not claimed.
5. ~~QLogic ISP1040 (KZPBA)~~ (done): the console and Windows 2000's own
   QLogic driver both drive it. The ISP1080 and the dual-bus ISP1240 came
   later and are driven by Windows alone: the ES40 console's PCI table has
   no entry for either device id, so it cannot name them or boot from them
   whatever the emulation does.
6. ~~Older Tulips~~ (done): the 21040, 21041 and 21140 join the 21143 as
   parts of one family, each with the identity and media machinery it
   really had -- a parallel address ROM read through CSR9, the older serial
   ROM format, a general purpose port in place of a SIA. The console boots
   over all four. What remains is a 21140 board with an MII PHY behind it,
   and that one waits for a driver that would exercise it: a transceiver was
   written and thrown away again after measurement showed that neither the
   console nor Windows 2000 ever reads a PHY register on this machine. Every
   CSR9 write the console makes has the management clock low -- it is
   talking to the serial ROM, never to a transceiver. Verifying an MII PHY
   needs NetBSD or Tru64, whose drivers do probe it.
7. TGA (ZLXp 21030): native DECwindows/CDE graphics.

Each new device gets its own directory under `src/devices/<bus>/` (as
`video/s3/` and `video/cirrus/` do), split by concern, and is verified
against the real firmware or guest driver that names it.

**Some devices the console can never see.** The SRM console does not
discover what a card is; it looks the PCI vendor and device id up in a
fixed table built into the firmware (V7.3-1 keeps it at offset 0x140a0c of
the decompressed image, as 28-byte records of vendor+device, subsystem,
names and console driver). A card whose id is not in that table is still
configured -- it gets its BARs and its interrupt, and a guest driver can
drive it normally -- but `show config` prints the bare numeric id, there is
no console driver attached, and the console cannot boot from it.

This is a property of the firmware, not of the emulation, and no amount of
work on a device model changes it. The ES40 of 2007 knew the adapters that
an ES40 shipped with; parts that arrived later, or that belonged to other
machines, are simply absent. Two of ours are in that position: the QLogic
**ISP1080** (`1077:1080`) and **ISP1240** (`1077:1240`) -- the table's only
QLogic SCSI record is the ISP1020, plus the Fibre Channel parts. Windows
2000 drives both perfectly well with `ql1080` and `ql1240`; SRM will never
name them or boot them. The same table explains why the bare 21040, 21041
and 21140 print their chip names while the 21143 prints "DE500-BA": only
rows carrying a board's subsystem id (0x500a, 0x500b, 0x500f) have a board
name to print.

The practical consequence is a rule for what to build next: **a device that
the console cannot name has to be verified against a guest driver, because
the cheap console test does not exist for it.** Where no guest we can run
has a driver either -- as with the 53C895A, which no driver on the Windows
2000 Alpha media binds to -- there is no way to verify the work at all, and
it is better not to start it.

**Verify against a guest driver, not only the console.** The console is
undemanding: it drove the QLogic adapter while three things were wrong that
Windows would not tolerate -- a missing self-identification after reset, and
two mistakes in the queue entries that carry the buffer segments of a large
transfer, which the console never exercised because it only ever issues
single-segment commands. Each of them was invisible until a guest driver
ran. `win_storage.sh` (a file copy inside Windows) is the cheapest such
test for a storage controller.

## What a guest driver's traffic shows: the 2026-09 audit

A Windows 2000 cold boot was profiled by device register (`JIT_STATS`, the
device-address histogram in [performance.md](performance.md)) and the
device models were then read against their data sheets for the class of
bug the profile pointed at: a device that answers a question the real part
answers differently, so the driver asks again -- millions of times. The
guest's time goes on the asking, and the emulator cannot see why. Found and
fixed, each with what the driver was doing:

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
  guest runs PIO: see the DMA note in performance.md -- the generic Alpha
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
  the display driver's idle wait always re-polled; the input status
  register's display-enable bit only pulsed at vertical retrace, so a
  driver syncing to horizontal blank waited a frame; and the PCI status
  word advertised a capability list and a parity error the card cannot
  have. All three corrected to what an 86C764 reports.

Found by reading rather than by traffic, and corrected in the same pass:

- **8254 read-back command** (control word with bits 7:6 set) was not
  modelled. It now latches the status and the count of every selected
  counter, and the status byte -- OUT, NULL COUNT, read/write mode, mode,
  BCD -- comes out on the next read before any latched count.
- **OHCI never wrote the HCCA.** The controller now posts the frame number
  (and a zero done head) into the block the driver gave it, whenever the
  driver touches a register and the number has moved; a driver that checks
  the HCCA to see the controller alive sees it advance. (Since superseded:
  the OHCI now runs its whole schedule on a frame thread, with devices on
  its ports -- [usb.md](usb.md).)
- **SMBus host** on the PMU function had the PIIX4's status bits, not the
  M7101's. It now runs the M7101 protocol: IDLE, a transaction started by
  the start register completes at once with DONE and a device error, since
  the emulated bus carries no device; a probe finds nothing and moves on.

Nothing from the audit remains open.
