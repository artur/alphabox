# Machines: adding CPUs, chipsets and boards

Alphabox emulates one machine, the AlphaServer ES40: an EV68CB on the
Tsunami/Typhoon chipset. This document is the plan for emulating more of
them, and the contract a piece of that work is handed over with -- to a
person or to an agent.

The firmware decides what "done" means. A machine is finished when its own
console firmware runs on it and agrees with a real one about what the
machine is. Everything below serves that.

## What a machine is made of

Three layers, each added in a different way:

| Layer | What it is | Adding one |
| --- | --- | --- |
| CPU model | the processor's identity and features: version code, feature bits (BWX, FIX, CIX, MVI), chip revision, translation buffer and cache sizes | a table row, when the core family already exists (EV6, EV67, EV68AL/CB/DC); a new module for a new family (EV4, EV5, EV7), since the processor-state and PALcode interfaces differ |
| Chipset | memory and I/O decoding, the PCI hoses and their DMA translation, the interrupt controller, the interval timer, multiprocessor support | a module: Tsunami/Typhoon today, Titan (ES45, DS25) or Pyxis (EV5 workstations) next |
| Board | the machine around the chipset: which CPUs and how many, memory limits, the PCI slots and how their interrupts are wired, the on-board devices, the console firmware image and how it loads, the machine's own registers (flash, management processor, memory serial-presence data) | mostly a data descriptor plus a few hooks, selected with `platform = "<name>";` in the machine block |

Today all three are implicit: the CPU identity is compile-time constants in
`cpu/cpu_defs.hpp`, `CSystem` *is* the Tsunami chipset, and the ES40's board
facts are spread between `PCIDevice.cpp` (interrupt wiring), `Configurator.cpp`
(reserved slots), `System.cpp` (firmware image and its decompression) and the
board devices that are always present. Making these three layers explicit is
the work that has to happen before a second machine is worth starting; it must
not change what the ES40 does, and the ES40's reference logs prove that.

## Where the firmware comes from

The Alpha firmware update CD V7.3 carries the console images for DS10, DS15,
DS20/DS20E, DS20L, DS25, ES40, ES45, GS140, GS320, GS1280 and the
AlphaServer 4x00. Images are never downloaded by us or by an agent: they are
copied from media you own into the git-ignored `roms/` directory, and a
packet names the file it needs.

Two image formats appear:

- a raw console image starting with the standard Alpha firmware ROM header
  (`c3c3 5a5a 3c3c a5a5`, 0x38 bytes, destination 0x900000), which is what
  `PC264SRM.ROM` (DS20/DS20E) and `DS10SRM.ROM` are;
- an update bundle ("LFU APU") holding several images, which is what the
  ES40's `cl67srmrom.exe` and the CD's `*_V7_3.EXE` files are.

Alphabox reads the ES40 bundle today; the raw form needs a loader, which is
a first work item for the pilot.

## The work packet

One machine, one file in `docs/platforms/<name>.md`, written from
[`TEMPLATE.md`](platforms/TEMPLATE.md), on its own branch and in its own
worktree. A packet states:

- **the machine**: CPUs, chipset, slots, on-board devices, memory;
- **its sources**: hardware manuals, the Linux, NetBSD and Tru64 code that
  describes the board, and the firmware file to use;
- **what is known and what is guessed**: every value taken on trust is
  marked, and a guess that the firmware later contradicts is a finding, not
  a defeat;
- **the acceptance ladder** below, which is how the work reports progress;
- **the rules**: no downloaded firmware, no behaviour changes to existing
  machines, and an unknown register access is traced and reported rather
  than quietly satisfied.

### The acceptance ladder

Each level is a command with a result to show. A packet is finished at L6;
partial work is reported as the highest level reached.

| Level | What it proves | How |
| --- | --- | --- |
| L0 | it builds | `test/tools/build_lanes.sh`: every lane |
| L1 | the firmware starts | the console image loads and executes; the emulator survives the first instructions |
| L2 | the firmware runs | the console prompt appears (`srm_probe.sh`) |
| L3 | the machine is right | `show config`, `show memory` and `show device` match the reference listing for the real machine, kept in `test/platforms/<name>/` |
| L4 | the console can work | its own tests pass: network boot against `net_peer.py`, disk boot, `test` |
| L5 | a guest runs | an operating system boots, whichever media exists |
| L6 | nothing else broke | the ES40 console-log check is clean and the JIT cross-check reports 0 mismatches |

L3 is the level that catches wrong guesses: the console prints what it
believes the hardware is, and a mistake shows up as a wrong slot, a missing
device or an absent CPU.

### Tools a packet relies on

- `srm_probe.sh` and `srm_run.sh` with a `PLATFORM=` selector and per-machine
  reference logs.
- An unknown-access trace: every read or write that no device claims,
  reported with the program counter that made it. Bringing up new firmware is
  mostly the loop "the firmware wants register X; find out what X is", and
  this is the tool for it.
- `net_peer.py` for console network boot, and the Windows and VGA checks
  where they apply.

## Order of work

1. **Make the layers explicit** (no behaviour change): CPU model rows, a
   board descriptor for the ES40, the Tsunami chipset separated from the
   generic system, the trace, the `PLATFORM=` selector, this template and the
   skills that go with it.
2. **Pilot: DS20E** ([packet](platforms/ds20e.md)). Same chipset and CPU
   family, its own firmware and board: it tests the contract with the least
   new hardware.
3. **The rest of the Tsunami family**: DS10, DS20, DS20L, and the UP2000 and
   XP1000 boards.
4. **Titan** (ES45, DS25) with EV67/EV68 rows.
5. **Separate projects**, each large enough to be its own plan: the EV5 core
   with an EV5 machine (the AlphaServer 4x00 firmware is on the CD), and EV7
   with the GS1280.

The EV7 machines are the far end of this: the processor carries its own
memory controller and talks to I/O bridges instead of a chipset, and its
console depends on the system's management hardware. The layering above is
what would make it a packet rather than a rewrite.
