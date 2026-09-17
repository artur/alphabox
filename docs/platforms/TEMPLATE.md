# <Machine> work packet

> Copy this file to `docs/platforms/<name>.md` and fill it in. Anything you
> could not establish stays in the file as an open question with the
> evidence you have; do not delete a question by guessing. See
> [platforms.md](../platforms.md) for how the layers fit together.

**Branch**: `platform/<name>` (own worktree) · **Config**: `platform = "<name>";`
· **Status**: not started / L0 ... L6

## The machine

| | |
| --- | --- |
| Family, code name | |
| CPU | model(s), how many |
| Chipset | |
| Memory | minimum, maximum, how it is arranged |
| PCI | hoses, slots per hose, what is on board |
| South bridge | |
| Console devices | serial, VGA, keyboard |
| Board hardware | flash, management processor, memory serial-presence data, anything else the firmware talks to |

State for every row whether it is **known** (with the source) or **assumed**.

## Firmware

- Image: `roms/<path>` (from which medium), format, load address.
- Other images (fail-safe booter, AlphaBIOS) and whether this packet needs them.
- What the firmware is known to require that the emulator does not have yet.

## Sources

- Hardware and service manuals.
- Linux: `arch/alpha/kernel/sys_*.c` (the board's interrupt map and vector),
  `arch/alpha/kernel/core_*.c` (the chipset).
- NetBSD: `sys/arch/alpha/`.
- Tru64 headers, where they describe registers.
- Existing Alphabox code for the nearest machine.

Prefer the firmware's own behaviour over any document: when they disagree,
the firmware is the machine.

## Reference output

The console listing the real machine prints, kept in
`test/platforms/<name>/`: `show config`, `show memory`, `show device`. Cite
where each came from (a manual, a real machine, a recorded session).

## Plan

Work items, smallest first, each with the level it reaches. Keep this list
current: it is the progress report.

| # | Item | Level | Status |
| --- | --- | --- | --- |
| 1 | | | |

## Findings

Append as you go: what the firmware asked for, what it turned out to be, what
was wrong in the assumptions above. This is the part of the packet that is
worth keeping afterwards.

## Rules

- Firmware images are copied from media the user owns, never downloaded.
- Existing machines must not change behaviour: the ES40 console-log check
  stays clean and the JIT cross-check stays at 0 mismatches (L6).
- An unknown register access is traced and reported, never given a
  convenient value to make the firmware proceed. A value chosen to satisfy
  the firmware is marked as such, in the code and in **Findings**.
- New device families get their own directory, split by concern, in the
  style of `devices/pci/sym53c8xx/` and `devices/pci/i8255x/`.
- Threading uses `std::` facilities; `src/base/` is off limits.
- Format changed lines with the repository's `.clang-format`.
- Report the highest acceptance level actually reached, with the command
  output that shows it. Never report a level you did not run.
