# AlphaServer DS20E work packet

Pilot packet: the first machine after the ES40, chosen because it shares the
chipset and the CPU family, so almost everything it exercises is the
machine-independent structure rather than new hardware. See
[platforms.md](../platforms.md) for the layers and the acceptance ladder.

**Branch**: `platform/ds20e` (own worktree) · **Config**: `platform = "ds20e";`
· **Status**: not started

## The machine

| | |
| --- | --- |
| Family, code name | Tsunami family. "Goldrack" per a hardware listing (assumed); Linux's Tsunami variation table has DP264, Monet, Clipper (= ES40), Goldrush, Webbrick (= DS10), Shark (= DS20L) and no Goldrack. **Which variation this firmware reports, and therefore which board wiring it expects, is open question 1 below.** |
| CPU | 1-2, EV6 500 MHz, EV67 667 MHz or EV68AL 833 MHz (assumed, from a hardware listing). Alphabox has EV68CB only, so this packet adds at least one CPU model row |
| Chipset | Tsunami/Typhoon 21272, as the ES40 (known: same family; whether this board has one or two Pchips is open question 2) |
| Memory | up to 4 GB (assumed) |
| PCI | 6 slots (assumed); how they map to hoses and which interrupt bits they use is open question 2 |
| South bridge | assumed an ALi M1543C as on the ES40; the original DS20 used a different part, so this must be confirmed from the firmware |
| Console devices | serial console; VGA where fitted |
| Board hardware | flash, and whatever the firmware expects in place of the ES40's management processor and FPGA registers: unknown, and the main discovery work |

Everything above marked assumed is a starting hypothesis, not a fact. The
firmware decides.

## Firmware

- Image: `roms/alpha-firmware-v7.3/DS20/PC264SRM.ROM` (Alpha firmware CD
  V7.3, directory `DS20`, which serves DS20 and DS20E). 735 KB, standard
  Alpha ROM header (`c3c3 5a5a 3c3c a5a5`, 0x38 bytes, destination
  0x900000), SRM console V7.3-1, OpenVMS PALcode V1.98-79, Tru64 PALcode
  V1.92-74 (from `DS20/FWREADME.TXT`).
- Also present: `PC264NT.ROM` (AlphaBIOS 5.71), `PC264FSB.ROM` (fail-safe
  booter), `DS20_V7_3.EXE` (the update bundle). Not needed for L2.
- Alphabox loads the ES40's bundle format today; this raw form needs a
  loader that honours the ROM header. The destination address matches the
  one the ES40 path already uses, so the self-decompression step should be
  reusable.

## Sources

- Linux `arch/alpha/kernel/sys_dp264.c`: the per-board interrupt tables
  (`dp264_map_irq`, `monet_map_irq`, `webbrick_map_irq`, `clipper_map_irq`)
  and machine vectors; `arch/alpha/kernel/setup.c` for the Tsunami variation
  names; `core_tsunami.c` for the chipset.
- NetBSD `sys/arch/alpha/pci/pci_6600.c`: takes the interrupt line the
  console assigned rather than a board table -- a guest that behaves this
  way will work as soon as the console's own numbering is right.
- The ES40 code in Alphabox, which is the nearest machine.
- DS20E owner's and service documentation for the slot and connector layout.

## Reference output

`test/platforms/ds20e/` : `show config`, `show memory`, `show device` from a
real DS20E. **Not yet obtained** -- without it L3 cannot be claimed
honestly. Sources to try, in order: documentation that quotes a console
session, a recorded session from a real machine, or (weakest, and marked as
such) the structure of the ES40 listing with DS20E slot names.

## Plan

| # | Item | Level | Status |
| --- | --- | --- | --- |
| 1 | Raw ROM-header image loader, selected by the platform descriptor | L1 | |
| 2 | `platform = "ds20e"` descriptor: CPUs, memory limits, slots, firmware | L1 | |
| 3 | CPU model row(s): EV67 and/or EV68AL identity values | L1 | |
| 4 | Run with the unknown-access trace; catalogue what the firmware touches that the ES40 model does not provide | L1 | |
| 5 | Board registers and devices the trace turned up | L2 | |
| 6 | Interrupt wiring: the slot-to-interrupt-bit map this firmware expects | L2 | |
| 7 | Console listings compared with the reference | L3 | |
| 8 | Console tests: network boot with `net_peer.py`, disk boot, `test` | L4 | |
| 9 | Guest boot, media permitting | L5 | |
| 10 | ES40 regression sweep and JIT cross-check | L6 | |

## Open questions

1. **Which Tsunami variation is this firmware?** Decide from evidence, not
   from a name: read the machine type and variation the firmware records for
   the operating system at boot, and compare the interrupt bits it programs
   per slot against the Linux tables. Record the answer in **Findings**.
2. **One hose or two, and what is in each slot?** The ES40 has two; the
   listing the firmware prints at L3 will say. Until then, do not copy the
   ES40's slot table silently -- copy it and mark it assumed.
3. **What replaces the ES40's management processor and FPGA registers?**
   Likely different, possibly absent. The trace answers this.

## Findings

_(append as the work proceeds)_

## Rules

- Firmware images are copied from media the user owns, never downloaded.
- The ES40 must not change behaviour: its console-log check stays clean and
  the JIT cross-check stays at 0 mismatches.
- An unknown register access is traced and reported, never given a
  convenient value to make the firmware proceed. A value chosen to satisfy
  the firmware is marked as such, in the code and in **Findings**.
- Threading uses `std::` facilities; `src/base/` is off limits.
- Format changed lines with the repository's `.clang-format`.
- Report the highest acceptance level actually reached, with the command
  output that shows it.
