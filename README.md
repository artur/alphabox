# Alphabox

**An AlphaServer ES40 emulator** (formerly AXPbox).

[![Build](https://github.com/artur/alphabox/actions/workflows/build-test-and-artifact.yml/badge.svg)](https://github.com/artur/alphabox/actions/workflows/build-test-and-artifact.yml)
[![License: GPL v2+](https://img.shields.io/badge/license-GPL--2.0--or--later-blue.svg)](LICENSE)

Alphabox emulates an HP/DEC AlphaServer ES40 — one to four Alpha EV68 CPUs on
the Tsunami/Typhoon chipset — well enough to run the operating systems of the
Alpha era: OpenVMS, Tru64 UNIX, NetBSD and Windows NT/2000. It runs on x86-64
and AArch64 hosts under Linux, macOS and Windows.

| OpenVMS 8.4 with the CDE desktop | Windows 2000 (build 2128) | Windows 2000 on two processors |
|---|---|---|
| ![OpenVMS 8.4 desktop](screenshots/openvms.png) | ![Windows 2000 desktop](screenshots/win2000.png) | ![Windows 2000 Task Manager showing two CPUs](screenshots/win2000-smp.png) |

## Features

- **Real firmware.** Boots the genuine SRM console and, from it, the
  ARC/AlphaBIOS console; graphics cards run their real VGA BIOS.
- **Fast.** An asmjit-based JIT for x86-64 and AArch64 hosts (Apple Silicon
  included), with block chaining and register pinning. An interpreter build
  is always available too.
- **Quiet when idle.** Idle pacing recognizes the guest's idle loops and
  sleeps until an interrupt arrives, so an idle guest costs a few percent of
  a host core.
- **SMP and large memory.** Up to four CPUs and up to 32 GB of RAM,
  presented to the firmware with matching memory arrays and DIMM data.
- **Removable media done carefully.** CD and floppy images are validated
  before insertion, swapped between guest commands, and respect guest
  drive locks; BIN/CUE images are supported.
- **Scriptable.** Runs without a window, dumps the screen, and accepts
  scripted keyboard and mouse input — handy for automated installs and
  tests.

## Emulated hardware

| Area | Devices |
|---|---|
| Machine | AlphaServer ES40; the DS20E and DS20L consoles run and the DS10 is started ([docs/platforms.md](docs/platforms.md)) |
| CPU | 1–4 × Alpha EV68CB (21264) |
| Chipset | Tsunami/Typhoon: Cchip, Dchip, 2 × Pchip, TIG, DPR/RMC |
| Memory | 64 MB – 32 GB |
| Storage | Symbios 53C810 / 53C825 / 53C875 / 53C895 / 53C896 (two channels) and QLogic ISP1020 / ISP1040 (KZPBA) SCSI, ALi M1543C IDE (disks and ATAPI CD-ROM), 82077AA floppy, RAM disk |
| ISA bridge | ALi M1543C: 8259 PIC, 8254 PIT, MC146818 RTC, 8237 DMA, SuperIO, PMU |
| Graphics | S3 Trio64 (with IBM 8514/A acceleration); Cirrus Logic CL-GD5430 / CL-GD5434 (with BitBLT) |
| Network | DEC 21040 / 21041 / 21140 / 21143 (Tulip); Intel 82557/82558/82559 (DE600-AA) — host access through pcap, TUN/TAP (Linux), a UDP link or a null back end |
| Sound | Ensoniq AudioPCI ES1370 |
| Expansion | DECchip 21050/21052/21152/21153/21154 PCI-PCI bridges (nested buses, multi-port boards) |
| Other | OHCI USB, 2 × 16550 serial ports (telnet or unconnected), keyboard and PS/2 mouse, flash and NVRAM persistence |

[docs/peripherals.md](docs/peripherals.md) lists every device the ES40
firmware knows and which ones are coming next.

## Guest operating systems

| Guest | Notes |
|---|---|
| OpenVMS | Boots, including the CDE desktop ([installation guide](https://github.com/lenticularis39/axpbox/wiki/OpenVMS-installation-guide)) |
| Tru64 UNIX | Boots |
| NetBSD | Boots ([installation guide](https://github.com/lenticularis39/axpbox/wiki/NetBSD-9.2-install-guide)) |
| Windows NT / 2000 | Through AlphaBIOS, on the S3 or Cirrus graphics card; Windows 2000 with up to two CPUs ([installation guide](https://web.archive.org/web/20260705122517/https://www.zx.net.nz/computers/dec/axpemu-es40.shtml)) |

See also the upstream [guest support](https://github.com/lenticularis39/axpbox/wiki/Guest-support)
page.

## Quick start

```
git clone --recurse-submodules https://github.com/artur/alphabox
cd alphabox
cmake -S . -B build
cmake --build build -j
./build/alphabox configure    # writes es40.cfg
./build/alphabox run
```

You also need an SRM console ROM image, and a VGA BIOS for the graphics card.
The [documentation](docs/README.md) covers the details:

- [Building](docs/building.md) on Linux, macOS and Windows, and enabling the
  JIT
- [Running and configuring](docs/configuration.md): firmware, consoles,
  networking, media, hotkeys
- [Headless operation and debug hooks](docs/headless.md)
- [Development and testing](docs/development.md)

## Performance

With the JIT build on an Apple M-series host, translated Alpha code runs at
about 4300 MIPS per emulated CPU on an arithmetic loop and 4000 on a memory
loop, and 96.6 % of guest instructions execute as host code. Inside a
Windows 2000 guest, a CPU-bound 15-million-iteration `cmd` loop takes about
62 s. An idle two-CPU Windows 2000 desktop uses about 3-6 % of one host
core.

A MIPS figure taken from a running guest is worth less than it looks: a
booting guest spends its time spinning on the cycle counter waiting for real
time to pass, and a guest scrolling a console window is waiting for the
drawing engine, so neither gets faster when the processor emulation does.
[docs/performance.md](docs/performance.md) has the measurements, what bounds
which workload, and the optimizations that turned out not to pay.

## Known limitations

- More than two CPUs in a guest: SRM runs with four, but Windows 2000
  Professional is licensed for two, and the Windows 2000 Server beta HAL
  only sends inter-processor interrupts to CPUs 0–1. OpenVMS and Tru64 are
  untested with more than one CPU.
- Big-endian hosts.
- Some SCSI and IDE commands; copying large files from an IDE CD-ROM to an
  IDE disk can fail (this rarely affects an OpenVMS installation).
- Cirrus screen-to-system BitBLT transfers (Windows 2000 does not use them).

## History and acknowledgements

Alphabox began as **es40**, the emulator by **Camiel Vanderhoeven** and the
ES40 Emulator Project (2007–2010), with later work by **Tim Stark**
(fsword7). **Tomáš Glozar** revived it as
[AXPbox](https://github.com/lenticularis39/axpbox) — CMake, a single binary,
modern C++ threading and many crash fixes — with **Remy van Elst**. The
[ES40-Emu](https://github.com/ES40-Emu/es40) project (gdwnldsKSC) continues
es40 in parallel and is a welcome source of ideas: each of its changes is
reviewed on its merits here and adopted, adapted or improved.

This repository continued AXPbox as its own project and was renamed Alphabox
in 2026. Its work includes the
AArch64 JIT, idle pacing, SMP and memory fixes, the Cirrus Logic cards and
their blitter, removable-media handling, configurable hotkeys, and the
headless test tooling.

Alphabox builds on the work of others:

- **MAME** — the VGA core and IBM 8514/A emulation (Barry Rodewald, Aaron
  Giles, Vas Crabb, Olivier Galibert; BSD-3-Clause).
- **Bochs** — the GUI layer (MandrakeSoft) and parts of the disk and CD-ROM
  command handling.
- **QEMU** — the ES1370 sound device (Vassili Karpov); its Cirrus model was
  the behavioural reference and the test oracle for the Cirrus blitter.
- **POCO** — the original threading wrappers (Applied Informatics).
- **86Box** — its ROM set supplies the VGA BIOS images, and its Cirrus model
  was a reference.
- **SDL3**, **asmjit**, **libpcap** and **Npcap**.

Guest-visible identifiers (disk serial numbers, the `es40.cfg` file name, the
machine type) deliberately keep their ES40 names.

## License

GNU General Public License, version 2 or later — see [LICENSE](LICENSE).
Third-party code keeps its original license, as noted in the source file
headers.
