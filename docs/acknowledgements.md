# History and acknowledgements

Alphabox began as **es40**, the emulator by **Camiel Vanderhoeven** and the
ES40 Emulator Project (2007–2010), with later work by **Tim Stark**
(fsword7). **Tomáš Glozar** revived it as
[AXPbox](https://github.com/lenticularis39/axpbox) — CMake, a single binary,
modern C++ threading and many crash fixes — with **Remy van Elst**. The
[ES40-Emu](https://github.com/ES40-Emu/es40) project (gdwnldsKSC) continues
es40 in parallel and is a welcome source of ideas: each of its changes is
reviewed on its merits here and adopted, adapted or improved.

This repository continued AXPbox as its own project and was renamed Alphabox
in 2026. Its work includes the AArch64 JIT and the performance work behind
it, idle pacing, SMP and memory fixes, the headless test tooling, USB, the
machines beyond the ES40 -- the Tsunami and Titan boards and the EV7
systems -- and most of the device families it emulates ([hardware](hardware.md)): the Symbios 53C8xx and
QLogic ISP SCSI adapters, the Tulip and Intel 8255x network cards, the
PCI-PCI bridges and the multi-port boards built on them, the Ensoniq sound
cards, and the Cirrus Logic, ATI Mach64, S3 ViRGE, 3Dlabs Permedia 2, DEC
TGA and ATI Radeon graphics cards with their drawing engines.

Alphabox builds on the work of others:

- **MAME** — the VGA core and IBM 8514/A emulation (Barry Rodewald, Aaron
  Giles, Vas Crabb, Olivier Galibert; BSD-3-Clause).
- **Bochs** — the GUI layer (MandrakeSoft) and parts of the disk and CD-ROM
  command handling.
- **QEMU** — the ES1370 sound device (Vassili Karpov); its Cirrus model was
  the behavioural reference and the test oracle for the Cirrus blitter, and
  its ati-vga model (BALATON Zoltan) a register reference for the Radeon.
- **POCO** — the original threading wrappers (Applied Informatics), since
  replaced by the C++ standard library.
- **86Box** — the ATI Mach64 drawing engine is ported from it (Sarah Walker,
  Miran Grca, Connor Hyde; GPL-2); its ROM set supplies the VGA BIOS images,
  and its Cirrus model was a reference.
- **Mesa**, the **Linux** radeon driver and **X.org**'s radeon driver — the
  register semantics the Radeon 7500 model follows, as AMD never published
  an R100 3D reference.
- **SDL3**, **asmjit**, **libpcap**, **Npcap** and **libusb**.

Guest-visible identifiers (disk serial numbers, the `es40.cfg` file name, the
machine type) deliberately keep their ES40 names.
