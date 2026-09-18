---
name: onboard-platform
description: Bring up another Alpha machine (a board such as the DS20E, DS10 or ES45) or another processor of the EV6 family in alphabox, following a work packet in docs/platforms/. Use when asked to add, port or bring up a new platform, motherboard, system type or CPU model, or to work on an existing packet.
---

# Bringing up another machine

Alphabox emulates the AlphaServer ES40. Another machine is added as a
**work packet**: `docs/platforms/<name>.md`, written from
`docs/platforms/TEMPLATE.md`. Read [docs/platforms.md](../../../docs/platforms.md)
first -- it defines the three layers (processor, chipset, board) and the
acceptance ladder this work reports against. The packet is the plan, the
progress report and the record of findings; keep it current as you go.

**The firmware is the specification.** The machine is finished when its own
console firmware runs and says the machine is what it should be. When a
document and the firmware disagree, the firmware wins, and that disagreement
belongs in the packet's **Findings**.

## Ground rules

- **Never `pkill`/`killall alphabox`.** Other sessions on this host run long
  guest installs. Stop only the PID you started, and check
  `sysctl -n kern.memorystatus_vm_pressure_level` first (4 = critical: wait).
- **Never download firmware.** Images come from media the user owns, copied
  into the git-ignored `roms/`. If the packet's image is missing, stop and
  say so.
- **Never satisfy the firmware with an invented value.** An access nothing
  claims is traced and reported. If you must return something to make
  progress, mark it in the code and in **Findings** as a value chosen to get
  past this point, with what is known about the register.
- **Never change what existing machines do.** The ES40 regression logs are
  the contract: a diff there means you changed shared behaviour.
- Work on `platform/<name>`, in its own worktree.

## Where each layer lives

| Layer | Files | Add by |
| --- | --- | --- |
| Processor | `src/cpu/CpuModel.hpp`, `src/cpu/CpuModels.cpp` | a table row (identity, extensions) for a part of the EV6 family; a different family needs a core, which is a separate project |
| Chipset | `src/system/System.cpp` (Tsunami today) | a module; only needed for a machine whose chipset is not Tsunami |
| Board | `src/platforms/Platform.hpp`, `src/platforms/Platforms.cpp` | a table row: processors, memory limits, slots, interrupt wiring, firmware image and format |

The board is selected with `platform = "<name>";` in the machine block.

## The loop

1. **Start from the packet.** Fill in what is known and mark what is
   assumed. An assumption is a hypothesis with an owner, not a fact.
2. **Add the board row** and whatever processor row the machine needs. Boot
   it and expect failure.
3. **Trace what the firmware wants**:

   ```bash
   ALPHABOX_TRACE_UNKNOWN=1 PLATFORM=<name> CMDS="" \
     test/tools/srm_probe.sh build/alphabox <name> 240
   ```

   `%SYS-T-UNKNOWN` lines give the address, the width and the instruction
   that made the access, with the return address -- firmware reaches
   hardware through helpers, so the caller is what matters. Repeated reads
   of one address usually mean the firmware is waiting for a bit to change.

   The other traces answer different questions: `ALPHABOX_TRACE_CALLS=1`
   (the firmware's own calls, which shows where a silent failure stops),
   `ALPHABOX_TRACE_I2C=1`, `ALPHABOX_TRACE_MP=1` (how a console starts
   other processors), `ALPHABOX_TRACE_FLASH=1`, and
   `ALPHABOX_DUMP_MEMORY=1` (guest memory at exit).
4. **Identify the register before implementing it.** Sources, in order: the
   machine's hardware manual; Linux `arch/alpha/kernel/sys_*.c` and
   `core_*.c`; NetBSD `sys/arch/alpha/`; the equivalent register on the
   ES40. Write what you found in **Findings**.
5. **Climb the ladder** (L0-L6 in the packet), running the commands and
   keeping the output. Report the highest level you actually reached.
6. **Finish each stage** with the ES40 regression: the console-log check
   clean and the JIT cross-check at 0 mismatches.

## The checks

```bash
test/tools/build_lanes.sh                                   # L0: every lane
PLATFORM=<name> CMDS="show config|show memory|show device" \
  PORT=21400 test/tools/srm_probe.sh build/alphabox <name> 300   # L2, L3
PLATFORM=<name> NIC=de600 NET_PEER=21401:21402 \
  CMDS="boot eia0 -protocols bootp" PORT=21403 \
  test/tools/srm_probe.sh build/alphabox <name>-net 300     # L4
PORT=21404 test/tools/srm_run.sh build/alphabox regress      # L6: the ES40
PORT=21405 test/tools/srm_run.sh build-jit-verify/alphabox regress-jv
```

L3 compares against `test/platforms/<name>/`. If that reference does not
exist yet, say so and do not claim L3: a listing that only matches itself
proves nothing.

## Pitfalls seen so far

- **Firmware comes in three forms**, and the board row says which:
  a console behind the standard ROM header (`c3c3 5a5a 3c3c a5a5`); a
  console behind a fixed wrapper (the ES40's `cl67srmrom.exe`); and, for
  most machines on the firmware CD, **an update utility with no header at
  all**. The last is not a console: run it, let it install the console into
  the flash, stop the emulator so the flash is saved, then boot again
  without naming an image -- the flash is searched for a console and it is
  started. That is how the DS20L came up.
- **Consoles differ in how they start other processors.** The ES40's starts
  them itself through the management processor, so ours wait for it; the
  DS20E expects them to be running already and asserts a halt line
  (`ALPHABOX_TRACE_MP=1` shows this). Getting it wrong looks exactly like
  "the console sees one processor". It is a board row property.
- **Consoles differ in which PCI device numbers they scan.** The DS20E
  looks at devices 0 to 10 only; devices where the ES40 keeps its own (15,
  19) are invisible there. A device the console does not list may be a
  numbering difference, not a fault.
- **A wrong interrupt map is quiet.** The console polls, so it reaches its
  prompt and lists a controller with the wiring wrong; the disk behind it
  is what goes missing. Check interrupts with a guest driver (the Windows
  storage check) or an operation that waits for one. The interrupt number
  the console writes into a device's configuration space is the answer key.
- **One firmware serves several machines.** The DS20/DS20E console holds a
  table of machine names and codes and picks by a code it reads from the
  board; ours falls back to the first entry, so it calls itself "AlphaPC
  264DP". The name a console prints is a machine fact, not a verdict on the
  emulation.
- **Processor identity is visible**: the console prints the processor's name
  from the chip identification, so a wrong value shows up at L3.
- **Say when a value is a guess.** The DS20E packet recorded a flash that
  turned out to be a diagnostic display; the correction cost nothing
  because the guess was labelled. An unlabelled guess would have become
  folklore.
