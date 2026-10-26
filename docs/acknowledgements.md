# History and acknowledgements

Alphabox began as **es40**, the emulator by **Camiel Vanderhoeven** and the
ES40 Emulator Project (2007–2010), with later work by **Tim Stark**
(fsword7). **Tomáš Glozar** revived it as
[AXPbox](https://github.com/lenticularis39/axpbox) (CMake, a single binary,
modern C++ threading and many crash fixes) with **Remy van Elst** and
contributors such as **Hendrik Volkmer** (the network back-end split and the
TAP back end).

## ES40-Emu

[ES40-Emu](https://github.com/ES40-Emu/es40), maintained by **gdwnldsKSC**
since 2023, continues es40 in parallel, and a large part of what Alphabox
runs on was written there. In July 2026 Remy van Elst ported ES40-Emu's work
of 2023–2026 into AXPbox; in September 2026 this repository reviewed about
250 later ES40-Emu commits and adopted, adapted or improved the ones it took.
From ES40-Emu came:

- **The x86-64 JIT** (`src/jit/jitengine.*`): gdwnldsKSC's block-compiling
  translator on asmjit: the block cache, chaining, register pinning, inline
  data-page-cache loads, SSE floating point, the bail-out protocol and the
  `JIT_VERIFY` / `JIT_STATS` harnesses, with **Meowthink**'s calling-convention
  work; and the JIT dispatcher now in `cpu/AlphaCPU_jit.cpp`. ES40-Emu
  distributes the JIT under BSD-1-Clause, and its notice is kept.
- **The CPU core rework** (gdwnldsKSC): guest time pinned to the wall clock
  (RPCC, the interval timer, the 8254), the data page cache, the CAS-based
  LL/SC model, and interpreter fixes: byte/word loads, `HW_LD`, the EV68
  translation-buffer masks, IEEE and VAX floating point (with **Yufeng Gao**),
  native VMS PALcode.
- **Graphics**: the MAME-derived VGA core, the IBM 8514/A engine and the
  reworked S3 Trio64 (gdwnldsKSC, with **WNT50**'s S3 and 8514/A fixes), the
  MAME shims, and the SDL3 GUI.
- **The ALi M1543C and its devices**: the PIC/SuperIO rework, RTC and PIT
  timing, the PMU, serial ports, keyboard fixes, and the rewritten DMA and
  floppy controllers (gdwnldsKSC, with **BlueRain-debug** and WNT50 on the
  FDC).
- **Storage**: ATAPI and SCSI CD-ROM commands (**Cacodemon345**, partly from
  Bochs), BIN/CUE images (**GenSayer**), and Symbios 53C810 and disk fixes.
- **Networking and sound**: DEC 21143 fixes (gdwnldsKSC, **Aleksander
  Czajczyński**), the Npcap loader, the ES1370 (Cacodemon345's port of
  QEMU's model).
- **System**: the i2c SPD EEPROMs and DIMM layout, DPR and Flash/ARC
  changes, the configuration layer and `es40-cfg` (with Cacodemon345), and
  the startup banner.

Each source file that carries ES40-Emu code names its authors in its header.

## This repository

This repository continued AXPbox as its own project (**Artur Goulão**) and
was renamed Alphabox in September 2026. Written here:

- the AArch64 JIT back end: its own emitter, written for Apple Silicon and
  other ARM hosts, which plugs into the JIT core that came from ES40-Emu;
  and the performance work behind it;
- idle pacing, SMP and memory fixes, the headless test tooling, and the
  experimental Hypervisor.framework runtime;
- USB (OHCI, EHCI and the devices behind them) and virtio;
- the machines beyond the ES40: the Tsunami boards DS10, DS20E and DS20L,
  the Titan boards ES45, DS25 and DS15, and the EV7 systems ES47, ES80 and
  GS1280;
- most of the device families it emulates ([hardware](hardware.md)): the
  Symbios 53C8xx and QLogic ISP SCSI adapters, the CMD649 IDE controller,
  the Tulip and Intel 8255x network cards, the PCI-PCI bridges and the
  multi-port boards built on them, and the Ensoniq ES1371;
- the Cirrus Logic, ATI Mach64, S3 ViRGE, 3Dlabs Permedia 2, DEC TGA and
  ATI Radeon graphics cards with their drawing engines.

## Third-party code and licences

Alphabox as a whole is distributed under the GNU General Public License,
version 2 or later ([LICENSE](../LICENSE)). Code that came from elsewhere
keeps its own notice in the header of the file that holds it:

| Code | Where | Copyright | Licence |
| --- | --- | --- | --- |
| es40 | most of `src/` | Camiel Vanderhoeven and the ES40 Emulator Project | GPL-2.0-or-later |
| AXPbox | most of `src/` | Tomáš Glozar, Remy van Elst and contributors | GPL-2.0-or-later |
| ES40-Emu | the files listed above | gdwnldsKSC and the ES40-Emu contributors | GPL-2.0-or-later; the JIT core, the ALi M7101 PMU, the SPD EEPROM and the MAME shims BSD-1-Clause |
| MAME | `devices/video/`: `VGA.*`, `VGACard.*`, `s3/S3Trio64.*`, `ibm8514a.*`, `attotime`, `coretmpl`, `logmacro`, `xtal`, `address_map` | Nathan Woods, Peter Trauner, Angelo Salese, Barry Rodewald, Aaron Giles, Vas Crabb, Olivier Galibert | BSD-3-Clause |
| Bochs | `gui/`, the CD-ROM commands in `storage/Disk.cpp`, `isa/Keyboard.*` | MandrakeSoft S.A. (and the plex86 developers in `gui/plugin.hpp`) | LGPL, version 2 or later |
| GXemul | `common/es40_debug.*`, `net/Ethernet.*`, `pci/tulip/` | Anders Gavare | BSD-3-Clause style |
| NetBSD | `pci/tulip/TulipMii.hpp`, `TulipRegs.hpp` | Manuel Bouyer; The NetBSD Foundation | BSD |
| SIMH and SoftFloat | `cpu/AlphaCPU_ieeefloat.cpp`, `AlphaCPU_vaxfloat.cpp`, `cpu_defs.hpp` | Robert M Supnik; John R. Hauser | MIT-style; the SoftFloat terms |
| QEMU | `pci/es137x/ES137x.cpp`, `ES137x.hpp`, `ES137xRegs.hpp` | Vassili Karpov | MIT |
| 86Box | `video/mach64/Mach64Engine.cpp` (ported) | Sarah Walker, Miran Grca, Connor Hyde | GPL-2.0-or-later |

**Changes given back.** Where code came to Alphabox under a permissive
licence, this repository's changes to it are distributed under that
licence too, so that they can go back to where the code came from
(since 2026-10-26; ES40-Emu keeps borrowed code under its upstream licence
for the same reason):

- the JIT core (`jit/jitengine.*`) and the ALi M7101 PMU
  (`pci/AliM1543C_pmu.*`) are BSD-1-Clause as a whole: they hold only
  ES40-Emu's code, which ES40-Emu distributes under BSD-1-Clause, and this
  repository's;
- `VGA.*`, `VGACard.*` and `s3/S3Trio64.*` also hold GPL-2.0-or-later code
  from es40 and AXPbox besides MAME's and ES40-Emu's BSD-3-Clause code, so
  they stay `GPL-2.0-or-later AND BSD-3-Clause`, and this repository's
  changes to them are also distributed under BSD-3-Clause;
- the files under a permissive licence alone (`i2c_spd.*`,
  `mame_shims.hpp`, MAME's `ibm8514a.*`, `attotime`, `coretmpl`,
  `logmacro`, `xtal` and `address_map`) keep it, this repository's
  changes included.

A contribution to one of these files is accepted under the same terms.
Everything else this repository wrote, the AArch64 back end
(`jit/jitemit_a64.hpp`) included, is GPL-2.0-or-later.

Everything in the source is GPL-2.0-or-later or under a licence compatible
with it. (A Windows-only MPU-401 MIDI output that ES40-Emu marks
GPL-2.0-only was removed in October 2026 for that reason.)

Built with, not part of the source: **SDL3** and **asmjit** (zlib),
**libpcap** (BSD-3-Clause), **libusb** (LGPL-2.1-or-later), **libslirp**
(BSD-3-Clause, the user-mode network of QEMU; it links glib, LGPL-2.1-or-later,
dynamically), and **Npcap**
on Windows, which is loaded at run time and installed by the user. The VGA
BIOS images (most of them from 86Box's ROM set) are supplied by the user
and not distributed with Alphabox.

Used as references, with no code taken:

- QEMU's Cirrus model (the behavioural reference and test oracle for the
  Cirrus blitter), its ati-vga model (BALATON Zoltan) and its eepro100 and
  tulip models;
- 86Box's Cirrus, Mach64 register and ViRGE models, and MAME's ViRGE;
- **Mesa**, the **Linux** radeon, tgafb, e100 and qla1280 drivers and
  **X.org**'s radeon driver: the register semantics the Radeon 7500
  follows, as AMD never published an R100 3D reference;
- NetBSD's isp, tga and i82557 drivers.

**POCO**'s threading wrappers (Applied Informatics) were the original es40
base layer. They are gone, and only the names of the exception classes
remain.

Guest-visible identifiers (disk serial numbers, the `es40.cfg` file name, the
machine type) deliberately keep their ES40 names.
