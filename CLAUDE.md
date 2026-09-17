# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

AXPbox emulates an HP/DEC AlphaServer ES40 (Alpha EV68 CPU + Tsunami/Typhoon
chipset) well enough to boot OpenVMS, Tru64, NetBSD, and Windows NT/2000.
It is a modernized fork of the es40 emulator, evolving as its own project
(fork remote `origin` = github.com/artur/axpbox, `upstream` =
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

Without `-DCMAKE_BUILD_TYPE` the build defaults to Release. `axpbox --version`
prints the version, commit and compiled-in features.

Single executable `axpbox` with subcommands: `axpbox run` (main_sim in
`src/AlphaSim.cpp`) and `axpbox configure` (main_cfg in `src/es40-cfg.cpp`).
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
cd test/rom && bash test.sh        # Linux: SRM firmware boot to P00>>> + console-log diff; expect "diff clean"
```

On macOS `test.sh` can never pass (BSD sed rejects `\x00`). The portable test
tools live in `test/tools/`: `srm_run.sh` (per-lane SRM boot + log diff, own
port per lane), `srm_probe.sh` (SMP init, memory layout, SCSI, exit-path
probes), `win_bench.sh` (headless Windows guest boot + MIPS), `vga_boot.sh`
(SRM on the S3 or Cirrus VGA console, window-less, settled-frame hashes),
`build_lanes.sh` / `build_revs.sh`. Their output goes to `$AXPBOX_WORK`
(default `lab/`, git-excluded, which also holds guest images). See the
`srm-boot-test`, `guest-boot-bench` and `build-lanes` skills. Pitfalls: `test.sh`
deletes the tracked ROM files at the end — restore with
`git checkout -- test/rom/` before committing. `test/rom/axp_correct.log`
contains NUL bytes (grep needs `-a`; edit binary-safe).

Never `pkill`/`killall axpbox`: other sessions on the same host may be
running long guest installs. Stop only the emulator PID you started (SIGTERM
exits gracefully once the main loop runs, saving flash and DPR), and check
memory pressure before starting large guests.

Deeper verification (each has a skill with the full recipe): `boot-openvms`
(full guest boot from `../run-axpbox` media — always copy disk images before
booting them), `test-arc` (AlphaBIOS/ARC console via flash + S3),
`verify-vga-sdl` (framebuffer inspection + input debugging), `srm-boot-test`,
`guest-boot-bench` (Windows 2000 guest boots and the MIPS benchmark).
For headless driving of the emulator (fb dumps, key/mouse injection,
`SDL_VIDEO_DRIVER=offscreen`), the `AXPBOX_*` env hooks are documented in
README "Headless testing".

Formatting: repo LLVM style via `.clang-format`; format only changed lines
with `git clang-format --binary <clang-format> --diff HEAD -- src`.
`clang-format-14` matches the existing code exactly; a newer clang-format
(e.g. Homebrew LLVM on macOS) differs in a few spots — keep the existing
form where they disagree.

## Architecture

Source layout under `src/`:

| Directory | Contents |
| --- | --- |
| `cpu/` | `AlphaCPU*`, the `cpu_*.hpp` opcode headers, vmspal, IEEE/VAX FP |
| `jit/` | asmjit translator: `jitengine.cpp` (x86-64), `jitemit_a64.hpp` |
| `system/` | `System`, `SystemComponent`, `Configurator`, `DPR`, `Flash`, `Port80`, `i2c_spd`, `TraceEngine` |
| `devices/isa/` | the legacy devices behind the bridge: `DMA`, `FloppyController`, `Keyboard`, `Serial`, `MPU401` |
| `devices/pci/` | `PCIDevice`, `AliM1543C` + its `_ide`/`_usb`/`_pmu` functions, `DEC21143`, `ES1370`, `Sym53C810/895`, `SCSIBus`, `SCSIDevice` |
| `devices/storage/` | `Disk`, `DiskController`, `DiskDevice`, `DiskFile`, `DiskRam` |
| `devices/video/` | `VGA` (MAME-derived core), `VGACard` (shared card plumbing + standard VGA registers), `ibm8514a`, MAME-derived shims, the dead pre-MAME `Cirrus`; one subdirectory per card family: `s3/` (`S3Trio64`), `cirrus/` (`CirrusGD54xx` split by concern, the device-independent `CirrusBlitter`, `CirrusGD5430`/`CirrusGD5434`) |
| `devices/net/` | `Ethernet`, `NetworkBackend`, `NetworkPcap`, `NetworkTap` |
| `gui/` | `bx_gui` backends; SDL3 (`sdl.cpp`) is the maintained one |
| `base/` | inherited Poco-style wrappers — do NOT use in new code |
| `common/` | `StdAfx`, `datatypes`, `es40_debug`, `es40_endian`, `config_debug`, `banner`, `telnet`, `lockstep` |
| `src/*` | entry points `Main.cpp`, `AlphaSim.cpp`, `es40-cfg.cpp` (+ its `*Question.hpp`), and `config.hpp.in` |

Headers are included unqualified (`#include "System.hpp"`) — every source
directory is on the include path, so moving a file needs no include changes.
Devices are grouped by bus, not by chip: all four `AliM1543C*` components
are PCI functions (each calls `add_function()`, and the Configurator marks
every one `IS_PCI`), so they live in `devices/pci/` even though the M1543C
is the ISA bridge; `devices/isa/` holds only the devices behind it.

Everything hangs off `CSystem` (`system/System.cpp`), which owns physical
memory and the Tsunami chipset model (Cchip/Dchip/Pchip: memory routing, PCI
windows, interrupts via `cSystem->interrupt()`). Devices derive from
`CSystemComponent` (base class in `system/SystemComponent.cpp`), register
memory ranges with the system, and implement `ReadMem`/`WriteMem`, optional
`init()`/`start_threads()`/`stop_threads()`/`check_state()`, and
`SaveState`/`RestoreState`. PCI devices derive from `CPCIDevice`
(config space, BARs); disk controllers from `CDiskController` with `CDisk`
children (`CDiskFile`/`CDiskDevice`/`CDiskRam`, BIN/CUE support in
`devices/storage/DiskFileBinCue.hpp`).

Major devices: `CAliM1543C` (ISA bridge: PIT/RTC-TOY/PIC/DMA + SuperIO) with
separate `_ide`/`_usb`/`_pmu` PCI functions, `CSerial` (telnet or
null_attach UARTs), `CKeyboard` (KBC + PS/2 aux mouse, Bochs-derived),
`CDEC21143` (NIC via pcap), `CSym53C810/895` (SCSI), `CS3Trio64` + `CVGA` +
MAME-derived rendering into the `bx_gui` plugin layer (`src/gui/`, SDL3 is
the maintained backend), `CCirrusGD5430`/`CCirrusGD5434` (`cirrus` config
class, `chip` key; same `CVGACard` base as the S3), `CFlash`+`CDPR`
(firmware NVRAM).

CPU: `CAlphaCPU` (`cpu/AlphaCPU.cpp`) is a per-instruction interpreter
(`execute()`, opcode implementations in `cpu/cpu_*.hpp` headers included
into it); `cpu/AlphaCPU_vmspal.cpp` is a native fast-path reimplementation
of OpenVMS PALcode entry points; `cpu/AlphaCPU_ieeefloat/vaxfloat` implement
FP. `state` struct = the whole architectural state (savefile format). Guest
timing is wall-clock based: `state.cc` (RPCC) advances by real elapsed time,
CPU 0 fires the Cchip interval timer at dispatch-batch boundaries, and the
8254 PIT/TOY in AliM1543C are wall-clock paced (see `cpu/AlphaCPU.hpp`
comments).

JIT (`src/jit/jitengine.cpp`, `ES40_JIT` builds only): translates Alpha
basic blocks to host code via asmjit -- x86-64 emitter in jitengine.cpp,
AArch64 emitter in `src/jit/jitemit_a64.hpp` (same bail/chain/frame protocol;
`AXPBOX_JIT_FPTEST=1` on a `JIT_VERIFY` build self-tests the inline IEEE FP
ops against the interpreter), direct-mapped block cache keyed by
physical PC, poly-link direct chaining between blocks, register pinning;
bails to the interpreter for anything hairy. The trace tier (`JIT_TRACES`)
is deliberately dormant. `jit_run()` in `cpu/AlphaCPU.cpp` is the dispatch
loop; memory access goes through `jit_read/jit_write` helpers that mirror
`cpu/cpu_memory.hpp` semantics.

Configuration: `CConfigurator` (`system/Configurator.cpp`) parses `es40.cfg` into a
tree and instantiates the device graph; each device class has an allow-list
(`kv_*[]`) of the config values it reads — unknown values warn at startup
(`%SYS-W-UNKNOWNCFG`), so add new config keys to the matching list. The
sample `es40.cfg` at the repo root documents every value.

Threading: each active device runs a `std::thread` (`myThread`, lambda
calling `run()`, `std::atomic_bool myThreadDead` checked by
`check_state()`). `src/base/` contains inherited Poco-style wrappers
(CMutex, CSemaphore, ...) still used by old code — do NOT use them in new
or newly ported code; use `std::mutex`/`std::thread`/`std::chrono`
equivalents (hard project rule).

## Project rules and settled decisions

- Headers are `.hpp`; upstream es40 patches never apply textually (see the
  `port-from-es40` skill for the porting recipe and the list of
  axpbox-specific code to preserve).
- icache is hardcoded ON, the mouse is always present/captured, and the
  first interval-timer tick fires immediately — these mirror upstream
  0.75.1 and were deliberate; don't reintroduce the config options.
- User-facing text says "AXPbox" (banner in `src/common/banner.hpp` with the
  author-era credits); guest-visible identifiers deliberately keep their
  ES40 names (`ES40EM00000` disk serial, `ES40RAMDISK`, MAC seed "ES40",
  the `es40.cfg` filename).
- AXPbox versions itself via `project(AXPBox VERSION x.y.z)` in
  CMakeLists.txt — never adopt upstream's version number.
- In `src/gui/sdl.cpp`, keep the focus-bounce re-grab logic (WSLg
  compositors bounce focus when the mouse grab engages) and the `AXPBOX_*`
  debug hooks when merging upstream GUI changes.
