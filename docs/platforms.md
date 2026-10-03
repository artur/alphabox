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
  chosen by the configuration class, with its core family (EV6 or EV7). The
  EV68CB, whose values are established, and the EV7 and EV7z
  (`cpu/ev7/` holds what that family changes: the physical address map and
  the state the XSROM hands the console); the JIT emits them per processor.
  A board takes processors of one family and refuses the other.
- **Board**: a row per machine in `platforms/Platform.hpp` and
  `platforms/Platforms.cpp`, chosen with `platform = "<name>";` (default
  `es40`). It carries the chipset, the processor and CPU count, the memory
  limits, the slot-to-interrupt wiring that the PCI code asks for, the slots
  that refuse add-in devices, the firmware image and its format, how the
  secondary processors start, where the native PALcode routines apply, the
  console patches, and a function that builds the board's own hardware. The
  functions and hardware live in `platforms/<board>/`.
- **Chipset**: `chipsets/Chipset.hpp` (`CChipset`), with the Tsunami in
  `chipsets/tsunami/`, the Titan (four hoses on two PA-chips) in
  `chipsets/titan/` ([notes](platforms/titan.md)), and Marvel -- the EV7's
  on-chip registers and its GIO port, memory per PID -- in
  `chipsets/marvel/`; the board row picks it (`CHIPSET_TSUNAMI`,
  `CHIPSET_TITAN`, `CHIPSET_MARVEL`). What the Tsunami and the Titan share
  is in `chipsets/PciWindows.hpp` (DMA windows) and `chipsets/DimmModel.*`
  (the DIMM model and its SPD parts).
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
| AlphaServer DS20E | L5: OpenVMS 8.4 boots from its CD and an installed disk on one and two processors; the interrupt map is the console's own, checked with OpenVMS ([packet](platforms/ds20e.md)) |
| AlphaServer DS10 | L5: OpenVMS 8.4 boots from its CD and an installed disk; network boot works, and the power-up network test passes ([packet](platforms/ds10.md)) |
| AlphaServer DS20L | L5: its own update utility installs its console (V6.6-10); OpenVMS 8.4 boots an installed disk to login on one and two processors ([packet](platforms/ds20l.md)) |
| AlphaServer ES45 (Titan) | L5: its own update utility installs its console (V7.3-2) in a two-part flash; `show config` matches a real ES45's core logic, on-board devices and slots; network boot; OpenVMS 8.4 boots its CD and an installed disk to login on one, two and four processors, with NIC interrupts checked on all four hoses ([packet](platforms/es45.md)) |
| AlphaServer DS25 (Titan) | L5: its update utility installs its console (V7.3-2) in the flash, with the on-board AIC-7899 present as far as the console needs it (configuration space and expansion ROM, no SCSI); network boot; OpenVMS 8.4 boots its CD and an installed disk to login on one and two processors, with NIC interrupts checked on all four hoses; `show config` matches a real DS25's (owner's guide) in name, core logic but the Cchip pass, on-board places and slot numbering ([packet](platforms/ds25.md)) |
| AlphaServer DS15 (Titan) | L5: its update utility installs its console (V7.3-2); its RMC is reached through a different mailbox and its halt register does not halt the processor (both found in the console's code); one PA-chip, so hoses 0 and 2; network boot; OpenVMS 8.4 boots its CD and an installed disk to login, with NIC interrupts checked on both hoses; `show config` matches a real DS15's (owner's guide) in name, one PA-chip, core logic but the Cchip pass, on-board places and slot numbering ([packet](platforms/ds15.md)) |
| AlphaServer ES47 / ES80 / GS1280 (Marvel, EV7) | L5 on emulated EV7s (`es47`, `es80` and `gs1280` rows, packets M0-M6): the console reaches `P00>>>` on the telnet console that the emulated CMMs carry (GIO); `show config` lists the IO7s with their four buses and the devices behind them -- on the ES47 the I/O expander's CMD 649 IDE (`cmd649`, the CD as `dqa0`) and four-function USB (`uss344`) as the real listing has them, a 53C895 and a DE500-BA standing in for the AIC-7892 and the gigabit card; OpenVMS 8.4 boots from its CD to DCL on every processor: the ES47 with two (`ev7`, or `ev7z` at 1300 MHz, which the console names "EV7 rev 3.0" as a real 7/1300 does), the ES80 with eight and four IO7s, the GS1280 with sixteen. The staged plan is in the [packet](platforms/marvel.md) |

## Source layout (agreed 2026-10-02, reached by the chipset split)

`CSystem` used to be the Tsunami chipset, with ES40 board hardware (DPR,
flash) and board hooks mixed in. The chipset split (packet M0 in
[marvel.md](platforms/marvel.md)) moved the code to this layout; `cpu/ev7/`
and `chipsets/marvel/` came with packets M1-M3, `titan/` comes with its own.
What M0 left where it was, and why, is in that packet's notes.

```
src/
  cpu/                 the Alpha core, common to every family
    CpuModels.cpp      rows: EV6, EV67, EV68AL/CB/DC, EV7, EV7z
    ev7/               only what EV7 changes: 44-bit physical addresses,
                       PID, interrupts from the on-chip router
  chipsets/
    Chipset.hpp        the interface: non-memory decode, interrupt delivery,
                       interval timer, PCI hose access, DMA translation
    tsunami/           Cchip, Dchip, Pchip, TIG (out of System.cpp)
    titan/             ES45, DS25, DS15
    marvel/            IO7, the EV7's on-chip CSR window (Rbox, Zbox), GIO
  platforms/
    Platforms.cpp      board rows: chipset, CPU row and count, slots,
                       interrupt wiring, firmware
    es40/ ds10/ ...    board-only hardware: the ES40's DPR/RMC, flash
                       layouts, I2C/SPD; the Marvel CMM/MBM stand-in
  system/              CSystem, generic only: memory, components, CPU
                       threads, ROM loading, configuration
  devices/             unchanged: devices never know which board they're on
```

The rules:

- **One binary runs every machine,** selected at run time by `platform =`
  and table rows, never by `#ifdef`.
- **The processor family decides the PALcode interface and the address
  width,** and only where it truly differs. The EV7 core is EV68-derived (the
  Marvel PALcode runs unchanged on it), so EV7 is a small delta in
  `cpu/ev7/`, not a second CPU class. Its router and memory-controller
  registers belong to `chipsets/marvel/`.
- **The chipset owns decode, interrupts, timer and DMA translation.** A PCI
  device works the same on a Tsunami Pchip and on a Marvel IO7 hose.
- **Board facts live in board rows and `platforms/<board>/`,** never in
  chipset or device code.
- **Moving code changes no behaviour:** the ES40 console-log check, the JIT
  cross-check (0 mismatches) and a `perf_ab` run on the moved hot paths
  (decode and interrupts) prove it.

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
  wrong looks exactly like "the console only sees one processor". The EV7
  machines do it a third way: each parked processor polls a register of its
  own (RBOX_SCRATCH1) for an address the console writes there in two
  halves, with an echo between them.
- **The registers at entry can be part of the interface.** The Marvel
  console's PALcode takes its processor ID from r28 and picks a cold start
  or a restart by r19 and r18; starting it with zeroed registers sent it
  down the restart path, reading a pointer from r21 (docs/platforms/
  marvel.md, M3). Read what the firmware does with its registers before
  deciding they do not matter.
- **Machines differ in which PCI device numbers they look at.** The DS20E's
  console scans devices 0 to 10 and no further, so devices at 15 and 19 --
  where the ES40 keeps its own -- are invisible on it. A device the console
  does not list may be a numbering difference, not a broken device.
- **Consoles carry their interrupt map as data.** The DS20E's and the
  DS10's consoles each hold a table of the line every pin of every device
  gets; reading it out of the image settled maps that Linux's tables had
  slightly wrong. Where a console has none, the lines it writes into
  configuration space for cards at each device number are the answer.
- **Native shortcuts belong to one firmware.** The interpreter's native
  OpenVMS PALcode routines were written against the ES40 console's PALcode;
  the DS20E's and DS20L's consoles put their own builds at the same address,
  and OpenVMS hung or bugchecked until those boards were given the real
  PALcode (`vmspal_pal_base` in the board row, 0 for them).
- **Who becomes primary is a race unless the board decides.** Where every
  processor runs from reset, the first through the PALcode reset wins the
  console's election; the emulator lets processor 0 go first.
- **A wrong interrupt map is quiet.** The console polls its own devices, so
  it reaches its prompt and lists a controller with the wiring wrong; what
  fails is the disk behind it. Test interrupts with a guest driver, or with
  a console operation that waits for one.
- **One firmware serves many machines.** The DS20/DS20E console carries a
  table of machine names and codes and picks by a code it reads from the
  board; without it, it falls back to the first entry and calls itself
  something else. The name a console prints is a machine fact, not proof
  that the emulation is right.
- **Speed patches are addresses in one console.** The ES40's console
  patches, applied to the ES45's console, replaced words in its scheduler
  and it hung silently in its idle loop; applied to the DS10's, one of
  them removed a routine's stack-frame allocation and its power-up network
  test halted. A board row has its own patch table or none.
- **A console can carry its own answers.** The Marvel console checks a
  magic word in low memory (0xcafebeef at 0xfc: its developers'
  simulator) and then builds every reply its management processors would
  send -- partition database, MBM configuration, memory assignment --
  itself. Those built-in replies are the best documentation of each
  message's layout; the console's symbol table (procedure descriptor and
  name pairs) names every routine that builds or reads them
  (docs/platforms/marvel.md, M4).
- **A clock's register may not be the chip's.** The Marvel TOY lives in the
  CMM's memory laid out like an MC146818, but OpenVMS waits for byte 10 to
  read zero, where an MC146818 keeps its divider bits (0x26); the console
  only ever tested <7>. A guest that hangs after its banner with no device
  access left is worth a PC sample and a memory dump: the loop it is in
  names what it waits for (docs/platforms/marvel.md, M5).
- **A console can gate its own probe on hot plug.** The Marvel console
  configures only the PCI slots it has powered and connected through the
  IO7's hot-plug registers, so with none modelled it probed nothing and
  reported no error at all.
- **A management processor's state bytes steer the processors.** Each EV7
  reads a byte the CMM keeps for it: zero makes its PALcode build its PAL
  area from scratch (right for the primary), non-zero keeps what the
  console copied there (a secondary). With zero for both, the secondary
  mapped the console's addresses onto its own empty memory and halted.
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
4. **Titan**: the ES45 ([packet](platforms/es45.md)), the DS25
   ([packet](platforms/ds25.md)) and the DS15 ([packet](platforms/ds15.md))
   are at L5, with L3 checked structurally against owner's guide listings.
5. **Separate projects**, each large enough to be its own plan: the EV5 core
   with an EV5 machine (the AlphaServer 4x00 firmware is on the CD), and EV7
   with the ES47/ES80/GS1280 ([packet](platforms/marvel.md)). The EV7 plan,
   in packets:
   - **M0** (done): separate the chipset from `CSystem`, with no behaviour
     change. This is shared with Titan, and everything below depends on it.
   - **M1** (done): an EV7 processor model, kept apart from the EV6 code
     wherever the PALcode interface differs.
   - **M2** (done): each EV7's on-chip registers: router, interrupts,
     interval timer, memory controllers and the GIO management port.
   - **M3** (done): the state the SROM/XSROM leave behind, in place of
     running them.
   - **M4** (done): the management processors' side of the GIO protocol,
     reverse-engineered from the console, with the CMM firmware (an Intel
     386EX) and the XSROM as witnesses. It carries the console terminal,
     the configuration, the partition database, NVRAM, FRU, environment
     and the TOY: `platforms/es47/Cmm.*`.
   - **M5** (done): the IO7 I/O bridge (PCI-X/AGP hoses, hot-plug slots,
     DMA windows, interrupt routing into the Rbox): `chipsets/marvel/Io7.*`.
     OpenVMS 8.4 boots its CD with it, after two CMM fixes (the TOY's update
     flag, the console terminal's interrupts).
   - **M6** (done): the route table OpenVMS reads, the EV7z row, and board
     rows for the ES80 (eight processors, four IO7s) and the GS1280
     (sixteen), from a processor topology (`chipsets/marvel/Topology.*`)
     and a CMM per module.
   - **M7**: OpenVMS 8.4 to an installed disk, and Linux.

The EV7 machines are the far end of this: the processor carries its own
memory controller and talks to I/O bridges instead of a chipset, and its
console depends on the system's management hardware. The first contact
confirmed both: the console's first access is to the processor's own
management port (GIO), and with the EV7 and its registers emulated (M1-M3)
the console reaches nothing else until that answers. With the CMM's side
of it emulated (M4) the console comes up to its prompt, and with the IO7
(M5) it finds its devices and boots OpenVMS.
