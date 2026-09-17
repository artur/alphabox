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

### What exists today

- **Processor**: a row per part in `cpu/CpuModel.hpp` and `cpu/CpuModels.cpp`,
  chosen by the configuration class. Only the EV68CB, whose values are
  established; the JIT emits them per processor.
- **Board**: a row per machine in `platforms/Platform.hpp` and
  `platforms/Platforms.cpp`, chosen with `platform = "<name>";` (default
  `es40`). It carries the processor and CPU count, the memory limits, the
  slot-to-interrupt wiring that the PCI code asks for, the slots that refuse
  add-in devices, and the firmware image and its format.
- **Chipset**: still `CSystem` itself. Separating it earns nothing until a
  machine needs a different one (Titan, for the ES45 and DS25), and it is
  the riskiest of the three, so it waits for that machine rather than being
  done on speculation.
- **The traces**: `ALPHABOX_TRACE_UNKNOWN=1` reports every access no device
  claimed, with the instruction that made it, and `ALPHABOX_TRACE_CALLS=1`
  reports the firmware's own subroutine calls, each site-to-routine pair
  once. The second answers "which of its routines ran", which is the
  question a console that fails silently leaves you with.
- **The tools**: `PLATFORM=` and `ROM=` select the machine and its firmware
  in `srm_probe.sh`, and the `onboard-platform` skill carries the process.

### Which machines run

| Machine | State |
| --- | --- |
| AlphaServer ES40 | emulated: the machine this project is about |
| AlphaServer DS20E | under construction: its console runs, finds both processors, drives disks and network-boots ([packet](platforms/ds20e.md)) |
| AlphaServer DS10 | under construction: its console runs and names the machine, once the board's I2C bus controller was modelled ([packet](platforms/ds10.md)) |
| AlphaServer DS20L | under construction: its own update utility installs its console, which then runs to the prompt and names itself correctly ([packet](platforms/ds20l.md)) |

## Where the firmware comes from

The Alpha firmware update CD V7.3 carries the console images for DS10, DS15,
DS20/DS20E, DS20L, DS25, ES40, ES45, GS140, GS320, GS1280 and the
AlphaServer 4x00. Images are never downloaded by us or by an agent: they are
copied from media you own into the git-ignored `roms/` directory, and a
packet names the file it needs.

Three forms appear, and the board's descriptor says which one its firmware
takes:

- **a console image behind the standard Alpha ROM header** (`c3c3 5a5a 3c3c
  a5a5`, header size and load address in the header): `PC264SRM.ROM`
  (DS20/DS20E) and `DS10SRM.ROM`;
- **a console image behind a fixed wrapper**: the ES40's `cl67srmrom.exe`;
- **a firmware update utility with no header at all**, starting directly
  with its self-decompressor: the CD's `*_V7_3.EXE` and `*_V6_6.EXE` files,
  which is how most machines ship.

The third form is not a console: it is the utility a real machine runs to
**install** its console into the flash. Run it once, answer its questions,
stop the emulator so the flash is saved, and boot again without naming a
firmware image: Alphabox searches the flash for a console image and starts
it. That is how the DS20L was brought up
([its packet](platforms/ds20l.md) has the exact steps), and it is the way
in for every machine on the CD that ships only an update file.

## What these consoles have taught us

Facts that cost time to find and apply to the next machine:

- **The console is the specification, and it disagrees with documents.**
  Every board fact below was settled by watching what the firmware did, not
  by reading a table: the DS20E's interrupt map was confirmed by the
  interrupt numbers its console assigned, and a guess about its flash was
  disproved by disassembling the code that wrote it.
- **Machines differ in how they start their other processors.** The ES40's
  console starts them itself through the management processor, so they wait
  for it; the DS20E has none and expects every processor to be running
  already, asserting a halt line and waiting for an answer. Getting this
  wrong looks exactly like "the console only sees one processor".
- **Machines differ in which PCI device numbers they look at.** The DS20E's
  console scans devices 0 to 10 and no further, so devices at 15 and 19 --
  where the ES40 keeps its own -- are invisible on it. A device the console
  does not list may be a numbering difference, not a broken device.
- **A wrong interrupt map is quiet.** The console polls its own devices, so
  it reaches its prompt and lists a controller with the wiring wrong; what
  fails is the disk behind it. Test interrupts with a guest driver, or with
  a console operation that waits for one.
- **One firmware serves many machines.** The DS20/DS20E console carries a
  table of machine names and codes and picks by a code it reads from the
  board; without it, it falls back to the first entry and calls itself
  something else. The name a console prints is a machine fact, not proof
  that the emulation is right.
- **Consoles are tolerant.** All three machines run with hardware they
  cannot find, printing a complaint and continuing. A trace of unclaimed
  accesses (below) shows what they wanted; most of it does not matter.

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

1. ~~**Make the layers explicit**~~ (done, no behaviour change): CPU model
   rows, a board descriptor for the ES40, the trace, the `PLATFORM=`
   selector, the template and the skill. Separating the chipset is left for
   the first machine that needs a different one.
2. **Pilot: DS20E** ([packet](platforms/ds20e.md)), in progress: the
   console runs (L2). What remains is the machine's own hardware -- how it
   finds a second processor, the processor SROM data it reads, its flash --
   and a real machine's listing to check against.
3. **The rest of the Tsunami family**: DS10 and DS20L (their consoles run),
   DS20, and the UP2000 and XP1000 boards.
4. **Titan** (ES45, DS25) with EV67/EV68 rows.
5. **Separate projects**, each large enough to be its own plan: the EV5 core
   with an EV5 machine (the AlphaServer 4x00 firmware is on the CD), and EV7
   with the GS1280.

The EV7 machines are the far end of this: the processor carries its own
memory controller and talks to I/O bridges instead of a chipset, and its
console depends on the system's management hardware. The layering above is
what would make it a packet rather than a rewrite.
