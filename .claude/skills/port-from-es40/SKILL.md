---
name: port-from-es40
description: Evaluate upstream ES40-Emu changes and selectively adopt the good ones into axpbox. Use whenever the user asks to compare with, bring in, or port changes from the es40 repo (github ES40-Emu/es40). Covers triage and critical review, delta computation, manual adoption vs reviewed whole-file takes, the axpbox invariants (std:: threading, .hpp, clang-format, rebrand), and the verification gates.
---


# Evaluating and adopting ES40-Emu changes in axpbox

axpbox (this repo) is its own project; the ES40-Emu emulator
(https://github.com/ES40-Emu/es40, clone it locally, e.g. to /tmp/es40/)
is a source of candidate fixes. **Neither upstream nor axpbox is the source
of truth -- the goal is the best code, not parity.** For every upstream
change: review it critically against axpbox's code (and the hardware
datasheets), then adopt it, adapt it, improve on both, or reject it, and say
which (and why) in the commit message. Never take a change just because
upstream made it; skip version bumps and churn. When asked to look at "new
upstream changes", the base of the range is the tip of the last reviewed
range -- check this file's history section (bottom) and
`git -C /tmp/es40 log --oneline` to find it.

**NEVER `git apply` / `git cherry-pick` upstream patches.** They never
apply: axpbox is clang-formatted, renames headers `.h` → `.hpp`,
rewrote threading to `std::`, merged the two upstream binaries into
one, and moved files. Follow the recipe below instead.

## Step 0 — survey the range

```bash
E=/tmp/es40           # upstream clone
BASE=<last-ported-upstream-commit>     # e.g. 7140555
git -C $E fetch --all                  # only if the user says they updated it
git -C $E log --oneline $BASE..HEAD    # list commits to port
git -C $E diff --stat $BASE..HEAD      # list files touched
```

Group the commits into themes (JIT, timing, disk, net, config, GUI,
build, docs) and triage each: what it fixes, whether axpbox already has it
or an equivalent, value, conflict risk with axpbox-only work, effort, and a
verdict (adopt / adapt / improve / reject / already-have). Verify the
claimed bugs in axpbox's own source before acting. For large rewrites, do a
critical code review (correctness vs hardware, threading, savestates, debug
noise, dead code) before deciding what to take. Plan roughly one axpbox
commit per theme. Every commit must build in all lanes and pass the SRM
test.

Files to ALWAYS skip (axpbox does not have them):
`src/Visual Studio/*`, `configure.ac`, `config_vms.h`,
`config_win32.h`, `.github/ISSUE_TEMPLATE/*`. Upstream `CMakeLists.txt`
changes almost never apply — axpbox has its own; read the upstream
hunk and decide if the *idea* (a new source file, a new dependency) is
needed. axpbox versioning is its own (`project(AXPBox VERSION x.y.z)`
in CMakeLists.txt) — NEVER adopt upstream's version number.

## Step 1 — file mapping (upstream path → axpbox path)

Upstream keeps every source in a flat `src/`; axpbox does not (reorganized
2026-09-16). Never assume a path — find the axpbox home of a file with
`git ls-files 'src/**/X.*'` — then:

- `src/X.h` → `X.hpp` (ALL headers, wherever the file now lives).
- Device models → `src/devices/<area>/`, grouped by bus: `pci/`
  (PCIDevice, all four AliM1543C components — the bridge and its
  `_ide`/`_usb`/`_pmu` functions are all PCI functions — DEC21143,
  ES1370, Sym53C810/895, SCSIBus, SCSIDevice), `isa/` (only the legacy
  devices behind the bridge: DMA, FloppyController, Keyboard, Serial,
  MPU401), `storage/` (Disk, DiskController, DiskDevice, DiskFile,
  DiskRam), `video/` (S3Trio64, VGA, ibm8514a, Cirrus, the MAME shims),
  `net/` (Ethernet, NetworkBackend/Pcap/Tap).
- CPU → `src/cpu/` (AlphaCPU*, `cpu_*.hpp`, vmspal, FP). Chipset,
  config and firmware NVRAM → `src/system/` (System, SystemComponent,
  Configurator, DPR, Flash, Port80, i2c_spd, TraceEngine). Shared
  headers → `src/common/` (StdAfx, datatypes, es40_debug, es40_endian,
  config_debug, banner, telnet, lockstep).
- `src/es40.cfg` → repo-root `es40.cfg` (plus per-test copies in
  `test/rom/`, `test/vms/`, `test/nt/`, `test/arc/`).
- `src/es40-cfg.cpp` → `src/es40-cfg.cpp` — entry points stay at `src/`
  root — but its `main()` is named `main_cfg()` (single `axpbox` binary;
  `src/Main.cpp` dispatches `axpbox run` → `main_sim`,
  `axpbox configure` → `main_cfg`).
- Includes are unqualified (`#include "System.hpp"`): every source
  directory is on the include path, so never write `../` paths.
- New upstream headers: create as `.hpp` in the matching directory,
  rename the include guard (`__X_H__` → `__X_HPP__`), and give it the
  axpbox license header (see Step 4). A new `.cpp` in an EXISTING
  directory is picked up by that directory's glob, but GLOB runs at
  *configure* time: after adding, MOVING or deleting a source file,
  re-configure each lane (`cmake -S . -B <lane>` keeps its cached
  options) — `cmake --build` alone reuses the stale list and fails with
  `no such file or directory` for the old path. A new DIRECTORY must
  also be added to the glob list AND to `target_include_directories` in
  CMakeLists.txt, or it is silently ignored.

## Step 2 — choose a strategy per file

Compute two diffs for each file F in the range:

```bash
S=<scratch dir>
# (a) what upstream changed:
git -C $E diff $BASE..HEAD -- src/F        # use: git --no-pager diff --no-ext-diff
# (b) what axpbox changed relative to the last ported upstream state:
git -C $E show $BASE:src/F > $S/F.base
diff -u $S/F.base <(git show <preformat>:src/F_axpbox_path)
```

For (b) you need the axpbox snapshot from BEFORE the repo-wide
clang-format, or the diff drowns in formatting noise. For the original
port that snapshot is `3e35a80~1`. If the file was clang-formatted
since, fall back to `diff -w -B` against the current file and read
carefully. ALSO check for axpbox commits made after that snapshot:
`git log --oneline <snapshot>..HEAD -- src/F` — their changes are part
of the axpbox delta too.

Pick the strategy:

- **Manual edit (M)** — when (a) is small (< ~100 changed lines).
  Apply upstream's hunks by hand with the Edit tool onto the current
  axpbox file, translating style (see Step 3).
- **Reviewed whole-file take (W)** — only when a critical review found
  upstream's version better overall, (a) is large (hundreds+ of lines, many
  commits of churn) AND (b) is small/enumerable. Trim what the review
  rejected (unused layers, debug noise) and fix what it found. Recipe:
  1. `git -C $E show HEAD:src/F > <axpbox path of F>` (see Step 1 — it
     is not `src/F` any more).
  2. `sed`-rename every `#include "X.h"` to `"X.hpp"`. Verify none
     remain: `grep -n '\.h"' src/F | grep -v '\.hpp"'`.
  3. Re-apply the axpbox delta. Fastest reliable way: save diff (b) to
     a file and run `patch --no-backup-if-mismatch -F3 src/F <
     delta.diff`, then open every `.rej` and hand-apply what failed.
     Read the result around every merge point.
  4. Deliberately DROP delta hunks that upstream has now superseded
     (e.g. a config option upstream removed) — list these in the
     commit message.
  5. `clang-format-14 -i --style=file src/F` (clang-format is
     installed as `clang-format-14`, no bare `clang-format`).

## Step 3 — axpbox invariants (apply to every ported line)

1. **No Poco-style wrappers in new code.** Upstream uses `CThread`,
   `CMutex`, `CSemaphore`, `SCOPED_M_LOCK`, `CRunnable` (from
   `src/base/`). New/ported code must use `std::` equivalents:
   - `CThread::sleep(ms)` → `std::this_thread::sleep_for(std::chrono::milliseconds(ms))`
   - `CThread* myThread` → `std::unique_ptr<std::thread> myThread;`
     started as `myThread = std::make_unique<std::thread>([this]() { this->run(); });`
     joined as `myThread->join(); myThread = nullptr;`
   - thread-death flag: `std::atomic_bool myThreadDead{false};`
     set in the run() catch block, checked in check_state().
   - `CMutex` + `SCOPED_M_LOCK(m)` → `std::recursive_mutex m;` +
     `std::lock_guard<std::recursive_mutex> lock(m);` (upstream CMutex
     IS recursive — keep recursive semantics unless you can prove
     no re-entry). `#include <mutex>` in the header.
   - Pre-existing `CMutex`/`CSemaphore` uses already in axpbox
     (e.g. Sym53C810, the CPU semaphore) may stay — do not churn them.
2. **Preserve the axpbox-only bug fixes** when replacing CPU/JIT code:
   - `execute()` advances the PC with `next_pc()`, never bare
     `state.pc += 4` (keeps `pc_phys`/`rem_ins_in_page` in sync).
   - `break_seq_icache()` in AlphaCPU.hpp also clears
     `state.rem_ins_in_page = 0;`.
   - `vmspal_int_initiate_exception` has the `vmspal_exc_depth >= 3`
     recursion guard (guard in AlphaCPU.cpp, ++/-- in
     AlphaCPU_vmspal.cpp, member in AlphaCPU.hpp).
   - `skip_memtest_hack` config option (AlphaCPU init + gated
     `0x8b000` page check in `execute()`), replaces upstream's
     `#ifdef SKIP_SRM_MEMTEST`.
   - `fread` return-value checks in RestoreState.
   - Debug env hooks (documented in README "Headless testing"):
     `AXPBOX_PC_SAMPLE` (AlphaCPU::check_state), `AXPBOX_DUMP_FB`,
     `AXPBOX_AUTOKEY_ENTER`, `AXPBOX_KEYSCRIPT`, `AXPBOX_KEYPIPE`,
     `AXPBOX_AUTOMOUSE`, `AXPBOX_MOUSE_DEBUG` (gui/sdl.cpp; the aux-cmd
     trace part of MOUSE_DEBUG is in Keyboard.cpp ctrl_to_mouse).
   - SDL grab handling: re-grab on FOCUS_GAINED after a compositor
     focus bounce (WSLg) — do not revert to plain
     "FOCUS_LOST => ungrab" when porting upstream sdl.cpp changes.
   - DiskFile: default-image-filename fallback (`defaultFilename`
     member) instead of upstream's FAILURE when no file is configured.
   - Disk: `SCSICMD_START_STOP_UNIT` case answered with
     `do_scsi_error(SCSI_OK)`.
   - Configurator: the `cirrus` class row exists and fails with a
     helpful "use s3 instead" message.
   - es40-cfg: the `skip_memtest_hack` question.
3. **Settled decisions — do not re-litigate:**
   - icache is hardcoded ON (`icache_enabled = true` in init/reset,
     no config read, no configurator question) — upstream ed9bf49,
     user-confirmed 2026-07-09.
   - The interval timer fires immediately at thread start
     (`next_timer_fire = start_time`) — the old 1-second grace is
     gone; the RTC boot tick (`toy_stored_data[0x0a] = 0x26`) and the
     vmspal guard cover the early-boot transient.
   - mouse is always present/captured (no `mouse.enabled`).
4. **Configurator allow-lists** (`kv_*[]` arrays in
   `src/system/Configurator.cpp`): any NEW config key a ported device reads
   must be added to that device's list, and axpbox-only keys must
   never be lost: `skip_memtest_hack` (kv_ev68cb), `timezone`
   (kv_ali), `rom.decompressed` (kv_tsunami), `address` (kv_serial).
   Audit after porting:
   ```bash
   grep -rn 'myCfg->get_\(text\|num\|bool\)_value("' src --include='*.cpp' \
     | grep -o 'value("[^"]*"' | sort -u
   ```
   then start the emulator once with every shipped config and require
   ZERO `%SYS-W-UNKNOWNCFG` (see Step 5.4).

## Step 4 — rebranding rules

All HOST-side user-visible text says **AXPbox**, never ES40:
window titles (sdl/x11/win32), the startup banner
(`src/common/banner.hpp` — `print_axpbox_banner`, shows axpbox's own
`VERSION`, credits authors by era: Camiel Vanderhoeven 2007-2010,
Tim Stark/fsword7 2018, Tomas Glozar 2020-2023, Remy van Elst
2020-2026, gdwnldsKSC 2023-2026), configurator wizard text,
serial telnet greeting, debugger greeting, stat lines.

Do NOT rename GUEST-visible or protocol identifiers: disk serial
default `"ES40EM00000"`, ramdisk model `"ES40RAMDISK"`, the MAC seed
`"ES40"`, the `es40.cfg` file name, `"[]ES40.CFG"`, hardware/SRM
references ("AlphaServer ES40" is the emulated machine — keep), state
file magics, include guards.

License header for files whose axpbox copy already has it, and for
new files:

```
/* AXPbox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 * ... (GPL v2 boilerplate as in existing files)
 */
```

Some ported files (e.g. AlphaCPU.cpp) still carry the upstream ES40
header — match whatever the file's current axpbox copy has. Strip
upstream's `$Id$` / `X-1.xx` CVS changelog blocks from any file you
wholesale-replace (keep only the `\file` description line).

Text in `banner.hpp` must stay ASCII (`Tomas`, not `Tomáš`) — the
column padding is strlen-based and multi-byte UTF-8 breaks alignment.

## Step 5 — verification gates (run in this order)

1. **Build all lanes** (see the `build-lanes` skill): at least an
   interpreter, a JIT and a headless lane; on the development Mac the full
   set, including the JIT_VERIFY, JIT_STATS/REGPROF, x86-64 and debug-flag
   lanes. Check the rc of every lane, and that JIT lanes really define
   `ES40_JIT` (the zsh word-splitting trap).
2. **SRM regression** (see the `srm-boot-test` skill): one run per lane,
   each on its own port, expecting `diff clean`. On Linux `test/rom/test.sh`
   works (it deletes the tracked ROM files at the end — `git checkout --
   test/rom/`); on macOS use the per-lane runner. `axp_correct.log`
   contains NUL bytes (grep needs `-a`) and embeds the serial telnet
   greeting — if you change that greeting, patch the expected log
   binary-safely (python bytes replace, not sed).
3. **JIT differential check** after ANY jit/ or AlphaCPU change: the SRM
   run on `build-jit-verify` and `build-jit-verify-x64` must show
   `[JIT][VERIFY] ... 0 mismatches`. JIT_VERIFY compiles the chain gates
   and inline page-cache fast paths out: changes there also need
   `build-jit-x64` (SRM), guest boots and, for performance work, the
   benchmark below.
4. **Config-warning sweep**: start the emulator from each directory whose
   config you touched (and a scratch dir holding the root `es40.cfg`),
   output redirected to a file, and look for `SYS-W` lines (expected:
   only `%SYS-W-NOSERIAL` on configs without serial1). Stop it gracefully
   after `P00>>>` (or wait for startup to finish): a signal during startup
   loses buffered stdout, and macOS has no `timeout`/`stdbuf` by default.
5. **Probes and guests for the area you changed**: SRM probes from the
   `srm-boot-test` skill (SMP `init` with 1/2/4 CPUs, `show memory`/
   `show fru` across `memory.bits`, exit paths); Windows 2000 guests
   booted headless on an APFS clone of the install with a final screenshot
   (RC2 to the desktop, the Japanese beta to its logon screen, both on 2
   CPUs); `AXPBOX_IRQSTATS=1` for interrupt changes.
6. **Benchmark** for JIT/dispatch changes: Windows 2000 RC2, 2 CPUs,
   `build-jit-stats-sdl`, `AXPBOX_NO_IDLE=1`, 300 s, per-CPU MIPS p50
   compared with a baseline measured the same way on the parent commit.
7. **OpenVMS boot** after CPU/disk/timing changes (see boot-openvms
   skill, when its media is available) — run on both interpreter and JIT
   lanes; expect `RESULT: SUCCESS: login prompt reached` and OPCOM
   timestamps consistent with the date the script answers.
8. **ARC smoke** after RTC/superio/VGA changes (see test-arc skill).
9. **Configurator smoke**: `printf 'no\n' | build/axpbox configure`
   shows the AXPbox banner and exits cleanly.

Never `pkill`/`killall axpbox` — other sessions on the host may be running
guest installs. Stop only the PIDs your own tests started.

## Phase workflow (large ranges)

What worked for the 9f7554d..2aa5e11 review (phases 1-7):

1. **Triage first, read-only.** For each phase, have an agent compare the
   upstream commits with axpbox's code and report per commit: what it
   does, axpbox status with file:line, merit (including bugs in the
   upstream change itself), a take/adapt/reject verdict, tests. Decide
   from the report; verify its key claims in the code before editing.
2. **One branch per phase** (`port/phaseN-<area>`) off the integration
   branch (`arm64-jit`). **One commit per theme**; when two themes touch
   the same file, finish and commit the first before editing for the
   second (or stage an index blob with the other hunk reverted) instead
   of splitting hunks at the end.
3. **Commit message** = what changed and why, which upstream commits were
   reviewed and what was taken, adapted or rejected, and a `Verified:`
   paragraph stating the tests actually run and their results (write it
   from the test output, never ahead of it).
4. **Tests from frozen copies** of test scripts (editing a script a
   background job is running corrupts that run), with a memory-pressure
   check before guest boots.
5. **Every intermediate commit builds**: build each commit of the phase in
   a separate worktree (see `build-lanes`) before fast-forwarding.
6. **Fast-forward the integration branch and push** after each phase, then
   record the phase in the port history below.

## Step 6 — commit conventions

One thematic commit per upstream area, message = what was ported +
which upstream commits + which axpbox deltas were preserved/dropped.
Do not sweep in unrelated untracked files (`test/nt/` may hold user
disk images and logs; there may be a user stash `WIP on s3-port` —
leave both alone). Trailer:
`Co-Authored-By: Claude <the current model's attribution line>`.

## Port history

- up to upstream `7d94c9e` — 12 commits on baseline `e81dce5`
  (2026-07-05).
- `7d94c9e..7140555` (v0.74→0.75.1) — 7 commits `29f83b4..377c362`
  (2026-07-09): JIT 0.75.1 + timing bundle, BIN/CUE, DEC21143 lock,
  Sym53C810 disconnect, configurator warnings, SDL mouse options,
  AXPbox rebrand + banner. Next port starts at upstream `7140555`.
- `7140555..9f7554d` (v0.75.1→0.75.4) — 4 commits (2026-07-11):
  DEBUG_ARITH_TRAP printf gating, LL/SC reservation invalidation on
  PCI DMA writes (cpu_llsc_dma_gate reader/writer gate + guards in
  interpreter/vmspal/JIT paths + DEBUG_INSTALL_PAL_TRAP hook), fsqrt64
  udiv128to64 fix (Tru64), and upstream's REVERT of the DEC21143
  register mutex (myRegLock removed again — follow upstream, do not
  re-add). Skipped: version bumps, configure.ac/m4, build.yml macOS
  pcap logic. Next port starts at upstream `9f7554d`.
  NOTE: the DEC21143/NetworkBackend area diverges from ES40-Emu since
  the lenticularis TAP/TUN merge (NetworkPcap/NetworkTap/NetworkFilter
  live only in axpbox+lenticularis) — port upstream DEC21143 pcap
  changes into src/devices/net/NetworkPcap.cpp instead.
- Pitfall: piping `bash test.sh | tail` can hang even after the test
  finishes (a lingering child keeps the pipe open) — redirect test.sh
  output to a file instead, then read the file.
- Post-port axpbox-only additions (2026-07-10, keep across future
  ports): input-injection/diagnostic hooks (AXPBOX_KEYPIPE,
  AXPBOX_AUTOMOUSE, AXPBOX_MOUSE_DEBUG + aux-cmd trace in
  Keyboard.cpp), the focus-bounce re-grab in sdl.cpp, README
  "Headless testing" + "Mouse on WSLg" sections.
- `9f7554d..2aa5e11` (v0.75.4→v0.85, 254 commits) -- reviewed on merit
  (2026-09-15), on branch arm64-jit (fork artur/axpbox), in phases:
  1 latent CPU/JIT bugs (SQRT classify, compile snapshot, irq_h re-kick,
  RestoreState flush, unknown IPR read-zero, interpreter run loop, FLTV
  current_pc, x64 underflow bails); 2 timing (RPCC per-read sync + floor,
  PAL reset-vector icache flush, JIT-batched ROM decompress, per-tick
  instruction pacing -- shipped OFF by default, timer.max_instr_per_tick);
  3 chipset (merged DMA core without upstream's unused service layer, FDC
  based on upstream with extra fixes, ALi PIC cascade / wall-clock RTC PF /
  8254 read-back with a live count, keyboard queuing with upstream's
  output-buffer purge rewritten, S3 LFB config widths, PMU datasheet
  values); 4 storage (DiskRam fixes; Sym53C810/895 race fixes adapted;
  removable media redesigned rather than taking upstream's mailbox);
  5 host/network (SIGTERM graceful exit, exit_on_pal_halt via a CSystem
  flag instead of a throw on the CPU thread, --version with features,
  Release build default, set-3 backslash scancode, DEC21143 RU latch and
  missed-frame counting, user-configurable SDL hotkeys plus our
  hotkey.media_force; found and fixed a Serial stop_threads abort on the
  way; deferred: DEC21143 SE-on-BME-off and the TAP wake thread, untestable
  without host networking); 6 SMP/memory (64-bit memory sizes with
  memory.bits bounded 26..35 and bounds-checked state files; Typhoon AAR
  multi-array plus one DIMM model driving SPD EEPROMs and the DPR memory
  bytes -- SRM show memory/fru verified 64 MB..32 GB, which also settled the
  SPD cache slot map; DPR quiet-period/reset/exit saves with atomic flags;
  nohle system-wide; DPR CPU start only for parked CPUs, TIG halt lines +
  IPCRs, IRQ4 under vmspal; found on the way: a throwing CSystem constructor
  left theSystem dangling and main_sim's failure handler crashed); 7 JIT
  performance ideas from upstream's own AArch64 engine, measured rather than
  taken on faith (skip_memtest_hack ignored in JIT builds; chain gates
  dropped on forward-only exits, gated per exit on AArch64, +1-1.5% MIPS;
  PALmode interrupt deferral and data-page-cache probe reuse instrumented
  and rejected -- ~10 interpreted instructions per 100M and 0.1-0.4% reuse
  candidates. Caveat: the RC2 NO_IDLE steady state is the NT idle loop, so
  later perf work needs a real guest workload).
  Not taken: upstream's own AArch64 JIT (ideas only), version bumps,
  autotools/Visual Studio/licence churn, the x64 engine split.
- Post-review fixes (2026-09-16), from the open-issue list rather than a
  range: upstream `1136df9` (LL/SC ABA guard) adopted as
  `CSystem::cpu_stx_c`, the single STx_C path for the interpreter, PALcode
  and JIT, with our DMA reader guard kept around it and a page-crossing
  STx_C still consuming the reservation. Found while reviewing it: our own
  port of upstream `1111047` (c3e9c8b) had put the DMA writer gate and
  reservation clearing on `do_pci_read` as well, where upstream guards only
  writes -- removed, a DMA read changes no memory. Also fixed on our side:
  the x86-64 S-float emitter narrowed operands with cvtsd2ss and so hid
  underflow traps (AXPBOX_JIT_FPTEST 42104 failures -> 0), and expected
  ATAPI probe results (sense keys 5/6) no longer print warnings.
- macOS pitfall: `test/rom/test.sh` can never pass on macOS (BSD sed
  rejects `\x00`) and leaks its emulator on timeout; use
  `PORT=<port> test/tools/srm_run.sh <binary> <label>` per lane instead.
- Source reorganization (2026-09-16): `src/` was split into `cpu/`,
  `system/`, `common/` and `devices/{isa,pci,storage,video,net}/`
  alongside the existing `base/`, `gui/`, `jit/`. Renames only — the
  `../X.hpp` includes in gui/jit/base were flattened in a separate
  commit first so rename detection (and `log --follow`) survives it.
  Upstream diffs are still against its flat `src/`, so map every path
  through Step 1 rather than pasting upstream paths.
