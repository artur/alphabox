# AlphaServer DS20E work packet

Pilot packet: the first machine after the ES40, chosen because it shares the
chipset and the CPU family, so almost everything it exercises is the
machine-independent structure rather than new hardware. See
[platforms.md](../platforms.md) for the layers and the acceptance ladder.

**Branch**: `platform/ds20e` (own worktree) · **Config**: `platform = "ds20e";`
· **Status**: L2 (console prompt reached)

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
| 1 | Raw ROM-header image loader, selected by the platform descriptor | L1 | done |
| 2 | `platform = "ds20e"` descriptor: CPUs, memory limits, slots, firmware | L1 | done (slots and interrupts still the ES40's, assumed) |
| 3 | CPU model row(s): EV67 and/or EV68AL identity values | L1 | not needed yet: the firmware accepts the EV68CB row |
| 4 | Run with the unknown-access trace; catalogue what the firmware touches that the ES40 model does not provide | L1 | done, see Findings |
| 5 | Second processor: this firmware does not find one | L2 | done: it finds both |
| 6 | Processor SROM data, so the console stops printing rubbish for it | L2 | open |
| 7 | The board's flash, at PCI memory 0xfff80000 | L2 | open |
| 8 | Interrupt wiring: the slot-to-interrupt-bit map this firmware expects | L3 | open |
| 9 | Why the IDE and USB functions are not listed | L3 | open |
| 10 | Console listings compared with the reference | L3 | blocked: no reference yet |
| 11 | Console tests: network boot with `net_peer.py`, disk boot, `test` | L4 | |
| 12 | Guest boot, media permitting | L5 | |
| 13 | ES40 regression sweep and JIT cross-check | L6 | |

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

**The firmware runs, first try (2026-09-17).** With the ROM-header loader,
the `ds20e` descriptor and the existing ES40 hardware model,
`PC264SRM.ROM` reaches `P00>>>`. That settles the shape of the work: this
board is close enough to the ES40 that the remaining items are differences,
not a bring-up.

**It calls itself "AlphaPC 264DP 800 MHz", console V7.3-1**, with OpenVMS
PALcode V1.98-79 and Tru64 PALcode V1.92-74 -- the revisions the CD's
`FWREADME.TXT` lists for DS20/DS20E. So this image is the DP264 firmware,
the design the DS20 and DS20E are built on, which is evidence for open
question 1 but not an answer: the machine type and variation it records for
the operating system still has to be read out.

**The processor row was accepted**: the console prints "Alpha EV68CB pass
4.0 800 MHz" from our EV68CB identity, so an EV67 or EV68AL row is a
refinement, not a prerequisite.

**Differences from the ES40 already visible:**

- "Bcache is disabled", where the ES40 reports 8 MB.
- "SROM Revision:" prints rubbish, and the console says `file open failed
  for iic_cpu0`: it wants per-processor SROM data this machine does not have.
- The floppy is `dva0.0.0.0.0`, not the ES40's `dva0.0.0.1000.0`.
- `show config` lists the M1543C bridge at hose 0 slot 7 but neither the IDE
  nor the USB function that the configuration provides at slots 15 and 19.
- A second configured processor is not found: no "CPU 1" line, and the
  console never starts on it. Processor presence is discovered differently
  here than on the ES40.
- The TIG reports revision 7.30 and an arbiter revision appears, neither of
  which the ES40 listing has.

**Accesses nothing claims** (`ALPHABOX_TRACE_UNKNOWN=1`): the empty-slot
configuration reads any bus scan makes, and byte writes at PCI memory
`0xfff80001` with values 0x20, 0x80, 0xc0, 0xc3 -- a flash command sequence
at a flash this board carries in PCI memory, where the ES40's sits on the
TIG bus. The firmware tolerates the writes going nowhere.

**The second processor: found, and fixed (2026-09-17).** Tracing the
registers a console uses to bring processors up (`ALPHABOX_TRACE_MP=1`)
showed this console *does* try: it writes the halt register for processor 1
(TIG `0x300005c0`, bit 1) and its handshake register, then gives up. Our
second processor was parked waiting to be started, so nothing answered.

The ES40's console starts its processors itself through the management
processor, and they wait until it does. This board has none: every
processor must already be running PALcode when the console asks. Releasing
the secondary at the PALcode reset entry -- where the ES40's management
processor puts one too -- makes it answer, and `show config` lists both
processors. That is now a board property (`console_starts_secondaries`),
not a guess: the console's own attempt is the evidence.

Three things were ruled out along the way:

- *The I2C bus is untouched.* With `ALPHABOX_TRACE_I2C=1` this console
  never addresses the chipset's I2C bus at all, so neither the processor
  data it wants (`iic_cpu0`) nor processor presence arrives that way.
- *The processor does run.* Letting the second processor run from reset
  instead of waiting to be started (which is how the ES40's management
  processor brings it up, and this board has none) gets it executing --
  `*** CPU1 *** STARTING ***` -- but the console still lists one processor.
  That experiment was reverted: it changed nothing and had no evidence
  behind it.
- *It is not the unmodelled TIG registers.* The console reads TIG
  `0x38000140` four times and `0x380001c0` twice and writes `0x38000100`;
  feeding those reads 0xff instead of 0 changes nothing.

Processor discovery turned out to be the halt-line handshake above.

**How to read this firmware** (the technique, for the other open items).
The trace names the instruction and the return address of an access nothing
claims:

```
%SYS-T-UNKNOWN: read 8 bits at 00038000140 (TIG register) from cpu0 pc=...1b5018 ra=...82da0
```

Those land in access helpers: `0x1b5014` reads a quadword, `0x1b50ac`
writes one, and `0x82d60`/`0x82db8` are the TIG wrappers, which build the
address as the TIG base plus the register index times 0x40 (so index
0xe00004 is register `0x38000100`). Searching the decompressed image for
the instruction that loads a given index finds the callers: `0x38000100` is
written with 1 at `0x7f728` and with 0 at `0x81cfc`. The emulator writes
the decompressed console out (`rom.decompressed`), the image starts 16
bytes into that file at address 0, and `lab/alphadis.py` disassembles it.

**Not yet proven:** everything above is the console's own account of itself.
Without the reference listing from a real DS20E, L3 is not claimed.

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
