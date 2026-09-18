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
| Acer Labs M1543C USB | `ali_usb` | OHCI host-controller registers only; no USB devices attach |
| Acer Labs M1543C PMU | `ali_pmu` | |
| NCR 53C810 | `sym53c810` | `n810` console driver: bootable |
| NCR 53C825 (825A) | `sym53c825` | `n810` console driver: bootable; wide, 4 KB SCRIPTS RAM |
| NCR 53C875 | `sym53c875` | `n810` console driver: bootable; Ultra-Wide, 4 KB SCRIPTS RAM |
| NCR 53C895 | `sym53c895` | `n810` console driver: bootable; Ultra2-Wide, 4 KB SCRIPTS RAM |
| NCR 53C896 | `sym53c896` | `n810` console driver: bootable on both channels; two Ultra2-Wide cores as PCI functions 0 and 1, 8 KB SCRIPTS RAM each, 256-byte register file with the phase-mismatch jump block. Windows 2000 drives it with `sym_hi`, not the `symc8xx` of the single-channel parts |
| QLogic ISP1020, ISP1040 (KZPBA) | `isp1020`, `isp1040` | `isp1020` console driver: bootable; Windows 2000 drives it with its own QLogic driver |
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
| Ensoniq Sound Card | `es1371` | the AudioPCI 97: an AC'97 codec on a serial link, and a sample rate converter where the ES1370 had fixed rates. This is the only audio part the console's table names (1274:1371); Windows 2000 binds `es1371mp.sys` to it, though only after its INF is given an NT install section -- the one on the Alpha media is decorated `.NTX86` and so matches nothing here |
| (not named) | `es1370` | the part before it, with a mixer of its own. The console has no table entry for 1274:5000, so `show config` prints only its ids |

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
| ATI Mach64 | M–L | common workstation card for NT and the free Unixes |

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
   QLogic driver both drive it.
6. ~~Older Tulips~~ (done): the 21040, 21041 and 21140 join the 21143 as
   parts of one family, each with the identity and media machinery it
   really had -- a parallel address ROM read through CSR9, the older serial
   ROM format, a general purpose port in place of a SIA. The console boots
   over all four. What remains is a 21140 board with an MII PHY behind it.
7. TGA (ZLXp 21030): native DECwindows/CDE graphics.

Each new device gets its own directory under `src/devices/<bus>/` (as
`video/s3/` and `video/cirrus/` do), split by concern, and is verified
against the real firmware or guest driver that names it.

**Verify against a guest driver, not only the console.** The console is
undemanding: it drove the QLogic adapter while three things were wrong that
Windows would not tolerate -- a missing self-identification after reset, and
two mistakes in the queue entries that carry the buffer segments of a large
transfer, which the console never exercised because it only ever issues
single-segment commands. Each of them was invisible until a guest driver
ran. `win_storage.sh` (a file copy inside Windows) is the cheapest such
test for a storage controller.
