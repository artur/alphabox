# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

Alphabox emulates an HP/DEC AlphaServer ES40 (Alpha EV68 CPU + Tsunami/Typhoon
chipset) well enough to boot OpenVMS, Tru64, NetBSD, and Windows NT/2000.
It is a modernized fork of the es40 emulator, evolving as its own project
(fork remote `origin` = github.com/artur/alphabox, `upstream` =
lenticularis39/axpbox). The [ES40-Emu/es40](https://github.com/ES40-Emu/es40)
revival is a source of candidate fixes, not a source of truth: each upstream
change is reviewed on its merits and adopted, adapted, improved or rejected --
the goal is the best code, not parity (see the `port-from-es40` skill).

## Build

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
```

At least three configurations must always compile (see the `build-lanes`
skill for the full lane set, JIT_VERIFY/JIT_STATS/x86-64 lanes, and the zsh
word-splitting trap when configuring):

- an interpreter lane, with SDL3 GUI by default. SDL3 comes from the system
  when available, else built statically from the `third_party/SDL`
  submodule (`git submodule update --init`)
- a JIT lane — `-DES40_DISABLE_ASMJIT=OFF`; needs asmjit cloned at pin
  `0bd5787b54b575ed94bf32ac452153b34385c514` into `third_party/asmjit`
  (gitignored plain clone)
- a headless lane — `-DDISABLE_SDL=yes`

Without `-DCMAKE_BUILD_TYPE` the build defaults to Release. `alphabox --version`
prints the version, commit and compiled-in features.

Single executable `alphabox` with subcommands: `alphabox run` (main_sim in
`src/AlphaSim.cpp`) and `alphabox configure` (main_cfg in `src/es40-cfg.cpp`).
Sources are collected by one `file(GLOB ...)` entry per source directory.
GLOB is evaluated at *configure* time, so after adding, **moving** or
deleting a source file re-run the configure step (`cmake -S . -B <lane>`,
which keeps that lane's cached options) — `cmake --build` alone reuses the
stale file list and fails with `no such file or directory` for the old
path. Editing CMakeLists.txt re-triggers configure on its own; a plain
`git mv` does not. A new *directory* must also be added to that glob list
and to `target_include_directories` in CMakeLists.txt, or its `.cpp` files
are silently left out. C++17. Debug builds of the JIT: add
`-DCMAKE_CXX_FLAGS="-DJIT_VERIFY"` (differential check of every compiled
block against the interpreter; expect 0 mismatches) or `-DJIT_STATS`;
see `src/common/config_debug.hpp` for all debug flags.

## Test

```bash
cd test/rom && bash test.sh        # Linux only: SRM boot to P00>>> + console-log diff; expect "diff clean"
PORT=21000 test/tools/srm_run.sh build/alphabox <label>   # portable: one lane, prompt + diff + JIT_VERIFY count
```

The portable tools are in `test/tools/` and are described in
[docs/development.md](docs/development.md) ("Test tools"): SRM runs and
probes, Windows guest boots and benchmarks, VGA frame hashes, USB, Direct3D
(`d3d_check.sh`), `build_lanes.sh`. Their output goes to `$ALPHABOX_WORK`
(default `lab/`, git-excluded, which also holds guest images). Recipes are in
the skills: `srm-boot-test`, `guest-boot-bench`, `build-lanes`,
`boot-openvms`, `test-arc`, `verify-vga-sdl`. Headless driving (fb dumps,
key/mouse injection, the `ALPHABOX_*` hooks): [docs/headless.md](docs/headless.md).

Pitfalls: `test.sh` cannot pass on macOS (BSD sed) and deletes the tracked
ROM files at the end -- restore with `git checkout -- test/rom/` before
committing. `test/rom/axp_correct.log` contains NUL bytes (grep needs `-a`;
edit binary-safe). Always copy a guest disk image before booting it.

Never `pkill`/`killall alphabox`: other sessions on the same host may be
running long guest installs. Stop only the emulator PID you started (SIGTERM
exits gracefully once the main loop runs, saving flash and DPR), and check
memory pressure before starting large guests.

**Performance claims go through `test/tools/perf_ab.py`, and are quoted
from `lab/results/ledger.md`, never from memory.** It waits for a quiet
host, needs two interleaved rounds, and checks the computed results are
identical across arms. Two builds of one file differ by 5-10% per section
from code layout alone, so an effect under ~10% is measured **inside one
binary** with a runtime switch (`perf_ab.py label bin bin --env-base
SWITCH=0`); `--snapshot` resumes the desktop snapshot instead of
cold-booting. A number that did not come out of it is an estimate and must
be labelled one; static figures (bytes per instruction) are never quoted as
dynamic cost. Details: [docs/performance.md](docs/performance.md).

Formatting: repo LLVM style via `.clang-format`; format only changed lines
with `git clang-format --binary <clang-format> --diff HEAD -- src`.
`clang-format-14` matches the existing code exactly; where a newer
clang-format disagrees, keep the existing form.

## Architecture

The full map -- every directory, device family and chip, the CPU, the JIT,
configuration and threading -- is in
[docs/source-layout.md](docs/source-layout.md). Read the part you need
before working in an area. In short, under `src/`:

| Directory | Contents |
| --- | --- |
| `cpu/` | `AlphaCPU*` (per-instruction interpreter, `execute()`; `jit_run()` dispatch), opcode headers, vmspal, IEEE/VAX FP, the processor rows (`CpuModels`), `ev7/` |
| `jit/` | asmjit translator: `jitengine.cpp` (x86-64), `jitemit_a64.hpp` (AArch64) |
| `system/` | `System` (memory, device ranges, CPU threads, console loading, state files), `SystemComponent`, `Configurator` |
| `chipsets/` | `CChipset` and its implementations: `tsunami/`, `titan/`, `marvel/` (EV7 CSRs, GIO, IO7, topology) |
| `platforms/` | one board row per machine (`Platforms.cpp`) and one directory per board for what only it has |
| `devices/common/`, `isa/`, `pci/`, `storage/`, `usb/`, `net/` | devices grouped by bus: the ALi M1543C and its functions, IDE (`IdeController`, `Cmd649`), SCSI (`sym53c8xx/`, `isp1040/`), NICs (`tulip/`, `i8255x/`), bridges, EHCI/OHCI, virtio, sound; disks; USB devices; network back ends |
| `devices/video/` | `VGA` core, `VGACard` base, one directory per card family (`s3/`, `cirrus/`, `mach64/`, `permedia2/`, `virge/`, `tga/`, `radeon/` with `r100/`) |
| `gui/` | `bx_gui` back ends; SDL3 (`sdl.cpp`) is the maintained one |
| `common/` | `StdAfx`, datatypes, debug and endian helpers, `Exception.hpp`, `WakeSemaphore.hpp` |

Things that hold everywhere:

- Headers are included unqualified (`#include "System.hpp"`); every source
  directory is on the include path. A new directory must be added to the
  CMake glob list and include directories.
- Everything hangs off `CSystem`; devices derive from `CSystemComponent`
  (PCI ones from `CPCIDevice`, disk controllers from `CDiskController`),
  register memory ranges, and implement `ReadMem`/`WriteMem`,
  `SaveState`/`RestoreState` and optional thread start/stop.
- Board facts belong in the board row (`platforms/`), never in device code;
  chip facts in a family's chip-row table.
- Configuration: each device class has an allow-list (`kv_*[]`) of the keys
  it reads; unknown keys warn at startup, so add new keys there and to the
  sample `es40.cfg`, which documents every value.
- Threading: `std::thread`/`std::mutex`/`std::condition_variable` only; no
  wait may turn a host sleep into a fatal timeout.
- Guest timing is wall-clock based (see `cpu/AlphaCPU.hpp`).
- Where the emulated processor knowingly differs from a real 21264 is in
  `docs/cpu-fidelity.md`: read it before "fixing" CPU behaviour, and add to
  it when a divergence is found or closed.

## Other machines

Alphabox emulates the ES40; other machines are added as work packets under
`docs/platforms/` (see `docs/platforms.md` and the `onboard-platform`
skill). Board facts belong in the board row, never spread through device
code. Bring-up traces, all off by default:
`ALPHABOX_TRACE_UNKNOWN` (accesses nothing claims, with the instruction and
return address), `ALPHABOX_TRACE_CALLS` (the firmware's own calls),
`ALPHABOX_TRACE_I2C`, `ALPHABOX_TRACE_MP` (how a console starts other
processors), `ALPHABOX_TRACE_FLASH`, `ALPHABOX_DUMP_MEMORY`.
`PLATFORM=` and `ROM=` select machine and firmware in `srm_probe.sh`.
Firmware images live in the git-ignored `roms/`; never download one.

## Project rules and settled decisions

- The project was renamed from AXPbox to Alphabox in September 2026
  (binary `alphabox`, env hooks `ALPHABOX_*`, repository
  github.com/artur/alphabox). Historical references to AXPbox (credits,
  the lenticularis39/axpbox upstream, port history) keep the old name.
- Headers are `.hpp`; upstream es40 patches never apply textually (see the
  `port-from-es40` skill for the porting recipe and the list of
  alphabox-specific code to preserve).
- icache is hardcoded ON, the mouse is always present/captured, and the
  first interval-timer tick fires immediately — these mirror upstream
  0.75.1 and were deliberate; don't reintroduce the config options.
- User-facing text says "Alphabox" (banner in `src/common/banner.hpp` with the
  author-era credits); guest-visible identifiers deliberately keep their
  ES40 names (the `ES40EM` disk serial prefix, `ES40RAMDISK`, MAC seed
  "ES40", the `es40.cfg` filename). The default disk serial is unique per
  drive: the ALi IDE's disk0.0 keeps `ES40EM00000` (installed guests'
  boot disks unchanged), every other drive is numbered from its place --
  controller, bus, unit (`CDisk::get_serial`): IDE and virtio-blk
  controllers by a two-digit count, SCSI adapters by `S` and their PCI
  hose and slot (`ES40EMS0003005`: pci0.3, bus 0, id 5), which INQUIRY
  page 0x80 reports; a USB disk's is its iSerialNumber. Never renumber
  them; two drives with one serial stop Windows 2000 with 0xCA.
- Alphabox versions itself via `project(Alphabox VERSION x.y.z)` in
  CMakeLists.txt — never adopt upstream's version number.
- In `src/gui/sdl.cpp`, keep the focus-bounce re-grab logic (WSLg
  compositors bounce focus when the mouse grab engages) and the `ALPHABOX_*`
  debug hooks when merging upstream GUI changes.
