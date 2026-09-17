# Peripherals the ES40 firmware knows

The ES40 SRM console (V7.3-1, `test/rom/cl67srmrom.exe`) carries a table of
the PCI devices it recognises by name, and a separate set of console
drivers for the ones it can *use* (boot from, or print to). That list is a
good definition of "a peripheral an ES40 could really have", so it is the
list AXPbox works from.

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

| Firmware name | AXPbox class | Notes |
| --- | --- | --- |
| Acer Labs M1543C | `ali` | ISA bridge: PIT, PIC, DMA, RTC/TOY, keyboard/mouse, COM1/COM2, LPT1, floppy |
| Acer Labs M1543C IDE | `ali_ide` | bootable (IDE disks and CD-ROMs) |
| Acer Labs M1543C USB | `ali_usb` | OHCI host-controller registers only; no USB devices attach |
| Acer Labs M1543C PMU | `ali_pmu` | |
| NCR 53C810 | `sym53c810` | `n810` console driver: bootable |
| NCR 53C825 (825A) | `sym53c825` | `n810` console driver: bootable; wide, 4 KB SCRIPTS RAM |
| NCR 53C875 | `sym53c875` | `n810` console driver: bootable; Ultra-Wide, 4 KB SCRIPTS RAM |
| NCR 53C895 | `sym53c895` | `n810` console driver: bootable; Ultra2-Wide, 4 KB SCRIPTS RAM |
| DECchip 21143-AA / DE500-BA | `dec21143` | Tulip console driver: network boot |
| S3 Trio64/Trio32 | `s3` | `vga_bios`: console, ARC/AlphaBIOS, Windows NT |
| Cirrus CL-GD5430 | `cirrus`, `chip = "gd5430"` | `vga_bios`: console |
| Cirrus CL-GD5434 | `cirrus` | `vga_bios`: console; Windows 2000 draws its desktop through the BitBLT engine |
| Ensoniq Sound Card | `es1370` | |

## Candidates, by value

"Guests" names operating systems known to ship a driver. Effort is a rough
guess: **S** is a variant of something already emulated, **M** a new but
well-documented chip with an open reference model, **L** a chip with
on-board firmware or a large command set.

### 1. Cheap variants of what exists

| Firmware name | Effort | Why |
| --- | --- | --- |
| NCR 53C895A, 53C896 | S–M | same `n810` driver; the 895A has 8 KB of RAM and a 256-byte register window, the 896 is two channels as two PCI functions |
| DECchip 21040/21041/21140, DE500-AA/-FA/-XA | S–M | older Tulips on the same driver; the 21140 matters for Windows NT and old Tru64 |

### 2. New devices with high payoff

| Firmware name | Effort | Why |
| --- | --- | --- |
| QLogic ISP10x0 (`isp1020`, KZPBA) | L | the standard ES40 SCSI adapter; bootable; drivers in OpenVMS, Tru64, NetBSD, Linux and NT. The command interface runs through on-board RISC firmware, so it's a mailbox/IOCB model, not a register model |
| DECchip ZLXp 21030 (TGA) | L | DEC's own 2D/3D workstation graphics; DECwindows/CDE on OpenVMS and Tru64 expect it; NetBSD has a driver |
| DE600-AA / DE602 (Intel 8255x) | M | the other common ES40 NIC; Intel console driver: network boot; drivers everywhere; QEMU's eepro100 documents the chip |
| DECchip 21052/21152/21153/21154 (PCI-PCI bridges) | M | more slots, a second bus, and the multi-port cards built on them (for example a quad 21143 behind a 21152) |
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

## Suggested order

1. ~~GD5430 variant and the Cirrus BitBLT engine~~ (done; screen-to-system
   blits are still ignored).
2. ~~53C8xx variants~~: the 825, 875 and 895 are done; the 895A and 896
   remain.
3. Intel 8255x (DE600/DE602): a second NIC family on a well-documented chip.
4. PCI-PCI bridge (21152/21154): unlocks multi-function cards and more slots.
5. QLogic ISP1040 (KZPBA): the ES40's reference SCSI adapter.
6. TGA (ZLXp 21030): native DECwindows/CDE graphics.

Each new device gets its own directory under `src/devices/<bus>/` (as
`video/s3/` and `video/cirrus/` do), split by concern, and is verified
against the real firmware or guest driver that names it.
