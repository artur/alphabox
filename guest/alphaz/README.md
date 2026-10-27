# AlphaZ

AlphaZ shows what an Alpha guest of Alphabox says about its processors and
its machine: a window of tabs on Windows (`ALPHAZ.EXE`, and `ALPHAZ64.EXE`
for 64-bit Windows), and the same pages
as a text report on Linux (`alphaz-linux`), both from one core written in C
and compiled by nada, a C compiler with its own assembler and linker that
targets Windows NT and Linux on the Alpha. It is guest-side software:
nothing in the emulator's build reads this directory.

Every value says where it came from: an instruction (IMPLVER, AMASK, RPCC),
an operating system call, the registry, `/proc`, or a **table value**,
which is what a document says about the part, not what this machine
answered. Nothing is guessed into a field that looks measured; a value the
system does not give is shown as "not reported".

## What it shows

| Tab | Rows | Where each comes from |
| --- | --- | --- |
| CPU | the name, AlphaZ's reading of the part, IMPLVER and the family it names, the AMASK feature mask and every bit by name (BWX, FIX, CIX, Tarantula bit 3, MVI, precise traps, SMT bit 10, EV8 SIMD and paired single bit 11, prefetch with modify intent), the clock, the processor count, IMPLVER and AMASK on each processor | the instructions; the clock is RPCC against the OS timer (below); Windows: `GetSystemInfo` and `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\<n>` (every value of processor 0, the main ones of the rest); Linux: `/proc/cpuinfo`'s cpu lines, cycle frequency, BogoMIPS, cpus detected and active |
| Caches | what the system reports, then the documented on-chip caches of the part IMPLVER names | Windows 2000 reports no cache sizes; Linux prints `L1 Icache` and the like where its kernel does. The documented sizes are table values (21264 HRM, the 21364 as Linux and the ES47 consoles give it, 21464 IDS pp. 42-43). No timing probe is run: Alphabox models no cache latency, so one would find nothing |
| System | the machine, the operating system, memory | Windows: the registry's `HKLM\HARDWARE\DESCRIPTION\System` (Identifier, SystemBiosVersion, and every other value), `GetVersionExA`, `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion`, `GlobalMemoryStatus`, `GetSystemInfo`; Linux: `/proc/cpuinfo`'s system and platform lines, `/proc/version`, `/proc/meminfo`, and any `/proc/cpuinfo` line no other page shows |
| EV8 | only when IMPLVER is 4: SMT (AMASK bit 10), the SIMD and paired-single sets (bit 11), Tarantula (bit 3), and a live check of twelve instructions | each instruction's result against a C model of what Alphabox's EV8 sheet ([ev8-isa.md](../../docs/platforms/ev8-isa.md), sections 3.3 and 3.4) says it does: VADDUB8, VADDSW4, VSUBUB8, TABSERRUB8, TMULSW4, PKUWB8, UPKUBW4, PMUL, PADD, PSUB, PHADD, PCMPLT. None of them runs unless AMASK reports bit 11 |
| Bench | an integer loop (xorshift64), a floating-point loop (a multiply-add chain), and on the EV8 VADDUB8 against the same in C | each runs for about 300 ms of the OS timer; rounds a second, and RPCC cycles a round. **These are this guest's own measurement of AlphaZ's loops**, compiled by nada without an optimizer; they are not performance claims about Alphabox, which come from `test/tools/perf_ab.py` only |
| About | the version, the nada commit it was built with, the licence | build.sh |

**The clock** is the cycle counter counted against the operating system's
timer over 500 ms on processor 0: Windows' `QueryPerformanceCounter`
(and a second opinion against `GetTickCount`), Linux's
`clock_gettime(CLOCK_MONOTONIC)`. RPCC's low 32 bits wrap (every 4.3 s at
1 GHz), so the loop reads the counter continuously and adds up the 32-bit
differences, counting the wraps. In Alphabox the counter advances by host
time times the processor's `speed` key, so the clock should read `speed`.

**The part** (AlphaZ's reading, a table value) combines IMPLVER with the
processor type the operating system reports: 2 the 21264 family (EV6,
EV67, EV68CB and the rest by the HWRPB's type), 3 the 21364 (EV7, and rev
3.0, the EV7z), 4 the 21464, in EV7-compatible mode (type 15, the `ev8`
row) or on its own interface (type 18, the `ev8n` row). Every processor
row of `src/cpu/CpuModels.cpp` is covered, and nothing is tied to one
board: the machine is what the system's own words say.

## Running it

Windows 2000 (and NT 4.0, if it runs a PE32 for the Alpha). On XP 64-bit
(Whistler build 2210, AXP64), which loads no 32-bit Alpha program, run
`ALPHAZ64.EXE` instead, with the same switches:

    ALPHAZ.EXE                 the window
    ALPHAZ.EXE /report FILE    every page as text into FILE, no window
    ALPHAZ.EXE /close N        the window closes itself after N seconds
    ALPHAZ.EXE /tour N         the window moves to the next tab every N s
    ALPHAZ.EXE /keylog FILE    every key the window sees, into FILE

In the window, Ctrl+Tab and Ctrl+Shift+Tab move to the next and the
previous tab (Left and Right too, when the tabs have the focus), PgDn,
PgUp, Up and Down scroll a page longer than the window, Esc closes it.

Linux: `alphaz-linux` prints the report on standard output, between the
lines `ALPHAZ-REPORT-BEGIN` and `ALPHAZ-REPORT-END`. It is a static ELF and
needs nothing from the system but `/proc`.

## Building it

    guest/alphaz/build.sh

needs nada (`NADA=`, default `~/Documents/proj/nada`), which it uses
read-only (its `./nada`, `include/` and `lib/`), and python3. It writes to
`OUT` (default `$ALPHABOX_WORK/runs/alphaz-build`, `ALPHABOX_WORK` being
the repository's `lab/` unless set):

- `ALPHAZ.EXE`, built with `-t alpha-windows -mcpu=ev6`: PE32 for the
  Alpha. nada writes every PE as a console program, so build.sh sets the
  optional header's Subsystem to 2 (WINDOWS_GUI) afterwards and no console
  opens behind the window.
- `alphaz-linux`, built with `-t alpha -mcpu=ev6`.
- `NADA-COMMIT.txt`: the nada commit (and whether its tree had changes),
  the commands, and the sha256 of every input and output.

Both programs are built for the 21264, whose BWX, FIX and MVI every
processor Alphabox emulates has. The EV8 instructions cannot be written as
mnemonics at that level, so `az_ev8.c` has each as its `.long` word, in a
function of its own; build.sh assembles each mnemonic with nada at
`-mcpu=ev8` and fails unless the word is the one nada writes.

The `.def` files name the DLL calls AlphaZ makes (user32, gdi32,
comctl32, advapi32, and the kernel32 calls beyond those of nada's own
`lib/windows/kernel32.def`), and `az_win32.h` declares what nada's
`include/windows/` does not yet have (the tab control, among others).
`mkicon.py` draws the icon; `alphaz.rc` holds it and the version.

## Testing it

`test/tools/alphaz_guest.sh <alphabox> <label> <guest>...` builds AlphaZ
and runs it in emulated guests, each on a clone of its disk, and checks
each value against the configuration it ran with (see
[docs/development.md](../../docs/development.md), "Test tools").

Tested (2026-10-27, build at nada 90ed176f, Alphabox 503dd1e, every value as
configured): Windows 2000 on `ev68cb` (2 CPUs), `ev8` (6 tabs, the 12 EV8
checks agree) and `ev7` (through `ALPHABOX_CPU_ANY_BOARD`), and Linux on
the ES40 (`linux`). Not yet clean: the ES47 Linux guests (`ev7z`, `ev8`)
read the right IMPLVER, AMASK, processors and memory, but their RPCC clock
came out 7 to 14 percent above the configured speed when the host's load
was 90 to 210 (the guest's timer lags the host, see the status file);
`ev8t4` timed out typing the EV8 binary at the console; Windows 2000 on
`ev7z` started one processor of two (a secondary starved by the host,
not the row: docs/cpu-fidelity.md). XP 64 (`xp64`, one `ev68cb`) PASS with
`ALPHAZ64.EXE`, every value as configured; `ALPHAZ.EXE` does not load
there. A guest's
clock reading is only as good as its OS
timer, and on a loaded host that timer lags.

## Files

| File | What it is |
| --- | --- |
| `az.h`, `az_core.c` | the core: pages of rows, the instructions, the clock, the part and cache tables, the loops, the report |
| `az_ev8.c` | the EV8 page: the words, their C models, the check, the SIMD loop |
| `alphaz_win.c`, `az_win32.h` | the Windows front end: Windows' own words, the window, `/report` |
| `alphaz_linux.c` | the Linux front end: `/proc`, the report |
| `*.def`, `alphaz.rc`, `mkicon.py` | imports, resources, the icon |
| `build.sh` | the build |

Licence: GPL-2.0-or-later, as the rest of the repository. Copyright (C)
2026 Artur Goulão.
