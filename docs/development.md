# Development and testing

## Where things are

| Directory | Contents |
| --- | --- |
| `src/` | the emulator; [source-layout.md](source-layout.md) maps it |
| `test/rom/` | the ES40 firmware regression test and its reference console log |
| `test/tools/` | the test tools below |
| `test/unit/` | unit tests: one model or device's source with the rest of the machine stubbed out |
| `test/cpu/` | the instruction conformance oracle: the image builder, the cases, a model of the instructions written from the Architecture Reference Manual, and the processor rows it runs |
| `test/arc/`, `test/vms/`, `test/nt/` | configurations and firmware for the ARC, OpenVMS and Windows NT runs |
| `test/platforms/` | real machines' console output, for platform bring-ups |
| `docs/` | this documentation |

Threading uses `std::thread`, `std::mutex` and `std::chrono`; the Poco-style
wrappers inherited from es40 are gone and stay gone. Format changed lines
with the repository's `.clang-format` (`git clang-format`).

## Test tools

The scripts in `test/tools/` find the repository from their own location and
write their output to `$ALPHABOX_WORK` (default `lab/`, which git ignores).
They stop only the emulator processes they started. Each script's header
gives its usage and variables.

### SRM firmware

| Tool | Purpose |
| --- | --- |
| `srm_run.sh` | SRM boot to `P00>>>` for one build: console-log diff against `test/rom/axp_correct.log`, JIT_VERIFY mismatch count, emit errors. |
| `srm_probe.sh` | SRM on a variant of the test machine: CPUs, memory, drives, a NIC, extra configuration, console commands, exit paths. |
| `boards_check.sh` | Every board of `src/platforms/Platforms.cpp` to its console prompt, with its own firmware from `roms/`: `show config` (and `show fru` on the EV7 boards), a PASS/FAIL line per board, normalised console logs. |
| `marvel_init_check.sh` | The EV7 boards with more processors (es47 2, es80 and gs1280 4): `init` twice, and every secondary started again each time, counted in the console's messages, the HWRPB's processor slots and `show cpu`. `boards_check.sh` runs it after its boards. |
| `snap_roundtrip.sh` | Every board saved at its console prompt, restored and saved again: the two state files compared byte for byte, part by part, and the restored console asked the same commands again. |
| `guests_check.sh` | Every guest operating system with media (Windows 2000, the 2000 beta, XP 64-bit, OpenVMS on the ES40 and ES47, Linux, NetBSD, and the ARC console; OpenVMS on the DS20E, DS20, DS10 and DS20L when named) booted on a clone of its disk and checked: a desktop, a DCL or shell login with a few commands, an installer menu. A PASS/FAIL line per guest, the last frame and the console log. |
| `srm_dwell.sh` | Boot to the prompt and sit there for N seconds: what the console's polling loop costs. |
| `srm_cfg.py` | The test machine's configuration, generated from `test/rom/es40.cfg`; the three scripts above and `gdb_check.sh` use it. |
| `gdb_check.sh` | The gdb stub against the console: attach at reset, breakpoints (one inside a compiled loop), steps, registers, memory, the monitor commands, the clock across a stop, Ctrl-C, one or more CPUs ([debugging.md](debugging.md)). |
| `srm_console.py` | Drives the console over its telnet port: waits for the prompt (`--wake` asks a restored console for it), runs commands, stops the emulator or saves it (`--after sigusr1`). |
| `net_peer.py` | The other end of a NIC's UDP link: answers ARP, BOOTP and TFTP with a CALL_PAL HALT image. |
| `make_halt_floppy.py` | A floppy whose boot block runs CALL_PAL HALT. |
| `ev7_pal_iprs.py` | Every internal-register access in a console's PALcodes: `HW_MFPR`/`HW_MTPR` with their entry point and the bits tested or set, `HW_LD`/`HW_ST` and the EV7 registers they reach, `IMPLVER`/`AMASK`; checked against the core and the Marvel register table. `--dis` disassembles. Written for the EV7 ([platforms/marvel.md](platforms/marvel.md)); works on the ES40's console too. |
| `unix_pal_probe.py` | A boot image that switches to the console's UNIX PALcode with SWPPAL, calls a dozen of its routines (optionally clock interrupts and a cache flush) and halts: the UNIX PALcode without a UNIX guest. |

Give each `srm_run.sh` its own `PORT` to run several builds at once.

`boards_check.sh <alphabox> <label> [board ...]` is the gate for a change to
a shared board or chipset file, which `srm_run.sh` (the ES40 only) cannot
see. It boots the boards one after another on ports 21600 and up, writes
`$ALPHABOX_WORK/runs/boards-<label>/<board>.log` (normalised, so two labels
diff) and the emulator's `%SYS-W-`, `%SYS-E-` and FAILURE lines, and exits
0 only if every board reached its prompt. The machines that ship only an
update utility (DS20L, ES45, DS25, DS15) first run it to install their
console into the flash, as on a real machine. `boards_check.sh --diff A B`
compares the command output of two runs. A board passes only if the
emulator also exits with status 0. After the boards, the EV7 boards among
them are booted again for `marvel_init_check.sh` (ports 21612 to 21614,
`<board>-init` lines; `BOARDS_INIT=0` skips it): `init` restarts a Marvel
console's secondaries, which the boot alone never exercises.
`BOARDS_EV7_CPU` gives the EV7 boards, in both scripts, another processor
class than `ev7` (`ev7z`, `ev8`), and with `ev8` `BOARDS_EV8_TPUS` each
chip's `tpus`: the scripts then want chips x TPUs processors in `show
config`, `show cpu` and the HWRPB ([ev8.md](platforms/ev8.md), S4).

`snap_roundtrip.sh <alphabox> <label> [board ...]` checks that a machine's
state survives a snapshot. For each board, `boards_check.sh` boots it, runs
its commands and saves it at the prompt (`SIGUSR1`); the emulator then
restores that file and saves it again before anything runs
(`ALPHABOX_RESTORE_SAVE`, [headless.md](headless.md#snapshots)), and the two
files must be the same byte for byte. The one exception is a field that
follows the host's clock, which runs on between the two saves: the ALi's
8254 counts and OUT bits, listed in the script with why. Where the files
differ, `<board>.cmp` names the part (memory, system, chipset or a
component and its class) and the offsets in it, from the map
`ALPHABOX_SNAPSHOT_MAP` writes. Last, the emulator restores the first file
again, the console is woken with a carriage return, and the same commands
must print the same output. All twelve boards run by default. On an EV7
board the memory array spans every processor's memory, 16 GB apart, up to
2^40 bytes, and its state file (version 2.3) keeps only the processors'
memory, so the save and the restore do not touch the holes.
Output goes to `$ALPHABOX_WORK/runs/snap-<label>/`, the boot to
`boards-snap-<label>/`.

`guests_check.sh <alphabox> <label> [guest ...]` is the gate for a change
that only a guest exercises: input, timers, disks, video, USB, sound,
floating point. It boots each guest from an APFS clone of its image (never
the original), on ports 21950 and up, and waits for a marker with a timeout.
The Windows guests (`w2k`, `w2k-beta`, `xp64`) must reach a desktop with a
taskbar and no dialog; for `w2k` the taskbar clock is also read, by
matching the font's glyphs against a table in the script, and must be
within 5 minutes of the host's time (a clock it cannot read is reported,
not failed). `vms-es40`, `vms-es47` and `linux` log in on the serial
console and run commands (`SHOW TIME`, `uname -a`, a C program compiled
and run), `arc` waits for the AlphaBIOS menu, which it reads with
tesseract (without it `arc` fails), and `netbsd` for the installer's
menu. It writes
`$ALPHABOX_WORK/runs/guests-<label>/<guest>.png` (the last frame),
`<guest>.log` (the console) and `<guest>.warn`, and exits 0 only if every
guest passed. A whole run takes about 15 minutes. `GUESTS_EV7_CPU` boots
`vms-es47` and `linux-es47` on another processor class than `ev7` (`ev7z`,
`ev8`; with `ev8`, `GUESTS_EV8_TPUS` sets each chip's `tpus`, and SHOW
CPU and Linux's "cpus active" must count every TPU), and
`GUESTS_EXTRA_CMDS` gives those two more commands (separated by `;`)
before they shut down.

Four more guests run only when named, so the default run stays short:
`vms-ds20e`, `vms-ds20`, `vms-ds10` and `vms-ds20l` boot the same
OpenVMS 8.4 disk on those boards, each with its own console, IDE place
and processor count (2, 2, 1, 2). Each logs in, runs `SHOW CPU`, reads
`F$GETSYI("HW_NAME")` and the active CPU count, copies
`SYS$BASE_IMAGE.EXE` with BACKUP (a DMA exercise) and compares the copy
with `DIFFERENCES` (no differences allowed), runs `SHOW ERROR` (no device
with errors) and shuts down. Run them after a change to the Tsunami
chipset code, the board rows or the chip revisions, which an operating
system may read and act on: `guests_check.sh <alphabox> <label> vms-ds20e
vms-ds20 vms-ds10 vms-ds20l`. The DS20L's console is the one its update
utility wrote into `lab/ds20l-lfu/flash.rom`.

### Configuration

| Tool | Purpose |
| --- | --- |
| `cfg_corpus.py` | The configuration parser and rules, without starting a machine: every `.cfg` in the repository, the cases in `test/config/valid/`, the `srm_cfg.py` machines of every board, and what `alphabox configure` writes for every board and for each `test/config/configure/*.answers`, against their resolved trees in `test/config/golden/`; and the malformed files in `test/config/malformed/` (plus generated ones), each of which must be refused with an error naming its file, line and column. |

`cfg_corpus.py build/alphabox` prints PASS or FAIL per case. It uses
`alphabox run --dump-config` ([configuration.md](configuration.md)), so it
needs no ROM and takes seconds. A change to a golden file is a change of
behaviour: regenerate them with `--update` only when that is the intent,
and say why in the commit. The golden exit statuses are those of a build
with SDL and pcap; on another build only the trees are compared.

`srm_probe.sh` takes its variants from the environment:

- `CPUS=`, `MEMBITS=`, `PLATFORM=` and `ROM=` pick the machine and firmware.
- `SCSI=<class>`, `IDE_CFG=<file>` and `FLOPPY=<image>|halt` add drives.
- `NIC=<class>` adds a NIC; `NET_PEER=<nic port>:<peer port>` puts it on a
  UDP link to `net_peer.py`, so `boot eia0`/`boot ewa0` runs a full network
  boot.
  `NAT=1` with `NIC=` instead puts it on the NAT back end (libslirp) with a
  CALL_PAL HALT image as `boot.img`; type `set ewa0_protocols bootp` before
  `boot ewa0`.
- `EXTRA_CFG=<file>` adds any configuration: bridges, more devices.

### CPU and JIT correctness

| Tool | Purpose |
| --- | --- |
| `JIT_VERIFY` build | Every compiled block re-run by the interpreter and compared. A clean SRM boot is ~130 million blocks. |
| `ALPHABOX_JIT_FPTEST=1` on that build | The JIT's inline IEEE operations checked over 8.5 million cases. |
| `alphabox_fptest` (built with every lane; `ctest` runs it) | The interpreter's IEEE and VAX operate instructions without a guest, about 20 million cases: IEEE results and flags against the host FPU in every rounding mode, VAX results against an exact model, hand tables from the manuals, and the same routines as they were at the commit `ALPHABOX_FP_BASELINE` names. Each difference from that baseline must be one of the intended changes listed in `test/fp/fp_test.cpp`. |
| `test/cpu/cpu_oracle.py` | Instruction conformance against the Architecture Reference Manual and the 21264 HRM, no firmware: about 20,000 results from 16,500 cases (integer operate, CMOV, the /V traps, loads and stores, BWX, CIX, MVI, LDx_L/STx_C, branches and jumps, the FIX moves, OPCDEC, CALL_PAL, user mode, the EV8's additions, and the JIT groups), each from the interpreter and from compiled code, on any processor row (`--cpu`). A second per lane (below). |
| `smc_test.sh`, `smc_image.py` | A guest that rewrites its own compiled code: the emulator halts if the change was seen, hangs if the stale block ran. |
| `bench_image.py` | A bootable image from a list of instruction words. |
| Bring-up traces | `ALPHABOX_TRACE_UNKNOWN`, `_CALLS`, `_I2C`, `_MP`, `_FLASH`, `ALPHABOX_DUMP_MEMORY`: what a firmware asked for and did not get ([headless.md](headless.md)). |

`JIT_VERIFY` finds disagreements between the JIT and the interpreter, not
disagreements with the architecture; for those see
[cpu-fidelity.md](cpu-fidelity.md).

#### The instruction oracle

```
test/cpu/cpu_oracle.py <alphabox-binary> [label] [--cpu ROW] [--port N] [--group G] [--passes N]
```

The expected value of every result comes from the manuals, not from the
interpreter: `test/cpu/aarm.py` is the Architecture Reference Manual's
pseudocode for each instruction, and the cases (`test/cpu/cases.py`) cite
the HRM wherever the 21264 decides what the manual leaves open (a load to
R31 takes its DTB miss, an operate into R31 is retired without executing
and so never traps).

- **The image** (`test/cpu/image.py`) replaces the console firmware: it is
  written as the decompressed console the emulator loads, so nothing is
  booted. A small PALcode of its own sets up the processor (kernel mode,
  interrupts masked, the KSEG superpages, PALshadow registers), records
  every exception it takes in a trap record and resumes after it, and
  serves a few CALL_PALs (FPE on and off, user mode and back, and the
  JIT cases' interrupt, I_CTL, PALshadow and IC_FLUSH services). The
  results go out over COM1, which the tool reads; `exit_on_pal_halt` ends
  the run.
- **Both engines in one run.** Every case runs 20 times. The first pass
  is stored apart: it is always interpreted, and on a JIT lane the last
  pass is compiled code (the default compile threshold is 16; a
  `JIT_VERIFY` build compiles at once and reports over 1,000,000
  compiled-block executions in a run). `ENGINES DIFFER` lists the cases
  whose two passes disagree.
- **Every case is a block.** The JIT records a block from a dispatch to
  the first change of flow and does not record a span that a dispatch
  batch's budget cuts, so each case starts with a `BR` over one word
  (`image.Builder.block`; a `BR` to the next instruction does not end a
  recorded span). Without that a group was one straight run that the JIT
  recorded from wherever a batch ended, differently each pass, and the
  last pass ran 8% of the case words compiled; with it, 71% (the rest are
  the skipped words and what follows an instruction the JIT does not
  compile). `--no-blocks` gives the old layout.
- **The JIT groups** (`jitregs`, `jitbranch`, `jitfault`, `jitint`,
  `jitllsc`, `jitpal`, `jitmode`, `jitsmc`, `jitmisc`; `test/cpu/jit_cases.py`)
  are shaped for the translator: all case registers live across chained
  exits and helper calls, branches that go one way while a block is
  recorded and the other way when it runs compiled, faults in the middle
  of a compiled block, an interrupt at the return from PALcode,
  LDx_L/STx_C split across blocks, PALmode blocks with and without the
  shadow registers, one block entered in two modes, and code rewritten
  before an `IC_FLUSH`. A case that must behave differently while its
  blocks compile does so through an `edge` register, 1 in the first and
  the last pass only, which are the two checked.
- **Verdicts.** PASS is the manual's value. KNOWN is a documented
  divergence's value: the case names it (docs/cpu-fidelity.md, or a
  finding of the CPU or the JIT audit, CPU-nn or JIT-nn), and some apply
  to compiled code only. FAIL is
  anything else, and so is a trap no case expected. The exit status is 0
  when nothing failed. A KNOWN case that passes on both engines is listed,
  so that its entry is removed with the fix.
- **Cost.** Under a second per lane (about 1.2 s under Rosetta), in
  `$ALPHABOX_WORK/runs/cpu-oracle-<label>`, removed when the run passes.
  `--port` (or `$PORT`) picks COM1's telnet port; without one a free port
  is used. The results, about 550 KB, come back over COM1, which keeps
  only its newest 256 KB when its reader falls behind: a run that lost
  some (`%SRL-W-DROPPED`, a host too busy to read) is run once more, and
  fails if it loses some again.
- **Any processor row.** `--cpu` names the row (`ev68cb`, the default,
  `ev7`, `ev7z`, `ev8`; `--list` shows them). The board is always the ES40:
  plain instructions do not depend on it, and the image replaces the
  console, so a row of another family (the EV7's, whose own boards need
  the Marvel console) runs there through the `ALPHABOX_CPU_ANY_BOARD`
  test hook ([headless.md](headless.md)), which the tool sets; the
  emulator's `%CPU-W-ANYBOARD` line is checked to name the row's part.
  Every row the oracle can run has the 21264's core, which its PALcode
  is written for.
- **What is row-specific** is in `test/cpu/rows.py`: IMPLVER, the AMASK
  feature mask and so the extensions (BWX, FIX, CIX, MVI, prefetch with
  modify intent), whether an unused 18.xxxx code is a no-op
  (`cpu_family_unused_misc_is_nop`), its extensions (`cpu_model::ext`),
  and the instructions a row has beyond the 21264's (the EV8 row's CCB,
  CMOV2 and FCMOV2, which group `ev8` checks, and LDL_ARM, LDQ_ARM and
  QUIESCE, group `quiesce`). The values are the
  manuals', as `src/cpu/CpuModels.cpp` gives them, and every run first
  reads that file and `CpuModel.hpp` and fails if the two tables differ,
  so a row added to the emulator is added to `rows.py` with it.
- **SKIP.** A case runs only where the row has what it needs: the
  extension of every word it emits (found from the words themselves),
  and what it asks for with `b.need()` (an encoding that must be unused,
  the 18.xxxx no-op rule); `b.part()` does the same for a span inside a
  case. Elsewhere the builder takes back everything the case emitted, and
  the run prints its results as `SKIP`, by reason: the EV8's instructions
  on the other rows (`rows.Own`), and on the EV8 row the cases that need
  those words to be unused (`rows.Unused`). The image's own code needs BWX
  (its printer stores bytes), so a row without it is refused. A case that
  may be skipped must not leave anything a later case relies on, and its
  setup belongs to it: a case that loads operands before `b.begin()`
  calls `b.start()` first.

A case is a few lines of `cases.py`: load operands with `b.load`, emit the
instruction, store each register or trap-record field to check with
`b.store` or `b.trap_check`, giving the value from `aarm.py` or the manual.
The registers R24 to R29 belong to the harness (checked after every
group); a case uses R0 to R23 and R30. A JIT case builds a sequence with
`jit_cases.Seq`, which runs the words through `aarm.py` to give the
expected registers.

**QUIESCE.** On a row that has it the oracle sets the quiesce timeout to
2^24 cycles (`ALPHABOX_QUIESCE_TIMEOUT`, [headless.md](headless.md)), and
group `quiesce` reads RPCC around each QUIESCE: one that slept took the
timeout, one that did not a few instructions. With one processor nothing
else writes, so an armed QUIESCE ends at the timeout. What other
processors and devices do is `test/cpu/quiesce_mp.py`'s:

```
test/cpu/quiesce_mp.py <alphabox-binary> [label] [--cpu ROW] [--port N]
```

two processors of the row on the ES40 (the same hook and image PALcode;
processor 0 starts processor 1 through the dual-port RAM's start word),
the timeout at 2^28 cycles. Processor 1 waits on a lock, as the 21464 IDS
writes the loop; processor 0 releases it 2^24 cycles later with a store,
then with a STx_C; then nobody writes; then processor 0 sends an
interprocessor interrupt; then a sym53c810's SCRIPTS Memory Move writes
the lock (DMA). Each wait must end after the release and before the
timeout (the third at it), and `ALPHABOX_TRACE_QUIESCE` must count one
wake-up of each cause. About a second. The Linux side is
`test/tools/es47_quiesce.py` ([ev8.md](platforms/ev8.md), "S3").

### Performance

| Tool | Purpose |
| --- | --- |
| `perf_ab.py` | The only way a performance claim gets made (below). |
| `nt_bench.sh`, `ntbench/` | A real NT application benchmark, one section per JIT datapath, timed by the guest itself. |
| `nt_snap.sh` | The same benchmark resumed from a desktop snapshot instead of a cold boot. |
| `sect_mips.sh` | The guest's MIPS on one benchmark section, from the snapshot. |
| `cpu_bench.sh`, `bench_image.py` | How fast the emulator executes a loop of Alpha code, with no OS. |
| `boot_rate.sh` | How much guest code a Windows boot gets through, and when the desktop arrives; two arms of one binary. |
| `jit_profile.sh`, `jit_profile.py` | A host sampling profile of compiled code, mapped back to guest blocks and instructions. |
| `win_bench.sh` | Headless guest boot on a throwaway clone: final screenshot, host CPU use, per-CPU MIPS. |
| `win_workload.sh`, `win_workload.py` | Times a CPU-bound command inside a booted Windows guest. |
| `mips_summary.py` | Percentiles of the `JIT_STATS` throughput lines in emulator logs. |
| `test/vms/vms_dcl.py` | OpenVMS 8.4 on the ES40 booted from a clone, logged in and timed through a DCL workload. Wall-clock times on the host as it is: an estimate, not a `perf_ab.py` result. |

[performance.md](performance.md) says what bounds each workload. What each
tool needs to be read correctly:

- **`perf_ab.py`** runs two binaries, or one binary with
  `--env-base K=V` on the base arm. A runtime switch in one binary is how
  anything under ~10% per section is measured, since code layout alone
  moves a section by that much between builds.
  - N >= 2 interleaved rounds of `nt_bench.sh`, or with `--snapshot` of
    `nt_snap.sh run`.
  - Per-section medians with ranges and an overlap flag; identical computed
    results checked; hashes, HEAD and the method recorded.
  - `--expect section:pct` predictions are marked hit or miss.
  - It waits for a quiet host before each round and times a round again
    if anything else ran during it (`--busy-ok` times anyway, and the
    ledger says so).
  - Results are appended to `lab/results/ledger.md`.
- **`nt_bench.sh`** runs `ntbench/axpbench.c` (sections `alu branch call
  ldst stride fp byte div sort`), JScript under `cscript` and `makecab`
  LZX. The guest writes its numbers to `C:\NADA\OUT.TXT`, which the
  harness reads back with mtools: no keyboard injection and no framebuffer
  timing. It needs [nada](https://github.com/artur/nada) for
  `-t alpha-windows` (`NADA=<dir>`, default `~/Documents/proj/nada`) and
  `mtools`. The disk staging, Startup hook, shutdown helper and
  end-detection come from nada's own `run-ntalpha.sh` test script.
- **`nt_snap.sh`** `make` boots the install once and snapshots the
  logged-in desktop (`SIGUSR1` saves with all threads stopped;
  `ALPHABOX_SNAPSHOT`, `ALPHABOX_SNAPSHOT_EXIT`). `run` resumes it
  (`ALPHABOX_RESTORE`), types the run command into Start > Run and runs a
  benchmark section: DONE ~16 s after launch instead of a two-minute
  boot. The S3 card's registers, VRAM and mode decode have been in the
  file since 2026-09-19, so the resumed desktop draws and takes input.
- **`cpu_bench.sh`** reads the rate the processor reports for itself
  (`ALPHABOX_RATE`) over the loop's steady stretch. `BODY=` sets the loop
  length: long measures the translated code, short measures leaving one
  block for the next. Do not use a `JIT_STATS` build: its counters are
  part of what would be measured.
- **`win_bench.sh`**: the MIPS are only meaningful if the guest is busy. At
  an idle desktop the number is the guest's idle loop, and the host sits
  near 1% CPU, which is the tell.
- **`win_workload.sh`** is narrow: the `cmd` loop it usually runs is
  integer-only over a handful of code pages, so it cannot show the FP
  path, the address path or call overhead. Prefer `nt_bench.sh` for those.
- **`jit_profile.sh`** needs the `JIT_DISASM` lane and macOS `sample`.
- **`vms_dcl.py`** boots `$ALPHABOX_WORK/vms84/disk-installed.img` (an
  APFS clone, removed afterwards) on `guests_check.sh`'s `vms-es40`
  machine and times, in host seconds: `srm` (start to `P00>>>`), `boot`
  (`boot dqa0` to the end of startup), `username`, `login`, then each DCL
  step to its prompt: SHOW SYSTEM, a DCL loop (`SYS$MANAGER:LOOP.COM`), a
  MACRO-32 program, three BACKUP + DIFFERENCES of `SYS$BASE_IMAGE.EXE`, a
  BACKUP saveset of `SYS$LDR` and its listing, SPAWN (`--workload full`
  adds MACRO, LINK, SHOW MEMORY, SHOW DEVICE). `--env`, `--memory-bits`,
  `--shutdown`. The decompressed console is cached, so `srm` does not time
  the decompressor. A guest whose memory the host has paged out measures
  the host: two 1 GB guests on a host already swapping stopped making
  progress for minutes on 2026-10-22.

### Graphics

| Tool | Purpose |
| --- | --- |
| `vga_boot.sh` | SRM on a VGA console, window-less; reports the hashes of the frames the screen settles on. |
| `s3_bench.sh` | Times a directory listing scrolling in a Windows command prompt. |
| `d3d_check.sh`, `d3dcheck/` | Direct3D conformance on a Windows 2000 guest: the card's HAL against Microsoft's software rasteriser. |
| `radeon_me_dis.py` | An annotated listing of a Radeon CP micro-engine image ([radeon-microcode.md](radeon-microcode.md)). |
| `ppm2png.py` | Converts a frame dump to PNG. |

`vga_boot.sh` takes `CARD=s3|cirrus|mach64|permedia2|s3virge|radeon|tga`
and a `CHIP=` for the families with several parts; the known frame sets
are in the script header.

`s3_bench.sh` measures what a guest feels as a slow or fast card. With
`ALPHABOX_BLIT_STATS=1` the drawing engine reports its pixels, commands
and transfers. It writes the emulator's pid to `emulator.pid`: profile
*that* pid, since other emulators may be running on the host.

### USB, storage and sound

| Tool | Purpose |
| --- | --- |
| `usb_bench.sh` | USB storage throughput in Windows 2000, by the guest's clock; `FAULTS=` fires `usb:` fault tokens. |
| `usb_audio.sh` | The USB speaker in Windows 2000: plays a sound, captures the stream and compares it with the file. |
| `usb_snap.sh` | USB devices across a snapshot and a restore of a running Windows 2000. |
| `wav_compare.py` | Compares a captured WAV with its reference. |
| `cdb_test.sh` | The SCSI disk's and the 53C8xx's conformance test (`alphabox cdbtest`): status, sense, data and lengths of each command, and SCRIPTS programs, with no guest. |
| `ide_test.sh` | The IDE controller's register-level test (`alphabox idetest`): task file, control block and bus master registers driven the way a driver does, with its own PRD tables in guest memory, no guest and no firmware. Each check cites ATA/ATAPI-6 or SFF-8038i. |
| `win_storage.sh` | A storage controller (`CTRL=<class>`) with a FAT16 disk added to a Windows guest, which copies a file on it. |
| `fat_disk.py` | Builds and inspects small FAT16 test disks. |
| `aboot_disk.py` | A sparse disk SRM boots Linux from: boot block, BSD disklabel, aboot's `bootlx`, a `/boot` and a root partition ([linux.md](linux.md)). |

`win_storage.sh` checks the copy on the host; `BRIDGE=<class>` puts the
controller behind a PCI-PCI bridge, and `SHARE=<class>` adds a second
controller with a disk of its own four devices further along the bridge's
bus, where its interrupt pin lands on the same chipset input as the
first's, and runs the two copies at once.

`cdb_test.sh` builds an image disk, a read-only one, two RAM disks and two
CD-ROM drives (one empty) on a SCSI bus of their own and drives them the
way a host adapter does: arbitration, selection, IDENTIFY, the CDB, the
data, the status and COMMAND COMPLETE, plus the ATAPI form with no message
phases that the IDE and USB paths use. Each check compares the answer
with the standards (SCSI-2, SPC-3, SBC-3, MMC-3); a command that would stop
the emulator fails its check. A second part builds an ES40 from a
configuration with a 53C875 and a disk on its bus, writes SCRIPTS programs
to memory, starts them through the chip's registers and checks what the
Symbios data manual says they leave (selection, interrupts, SFBR, SBCL,
the timer, Memory Move). It takes a few seconds, needs no firmware, and is
the first thing to run after a change to `CDisk`, the SCSI bus or the
53C8xx.

### Device access records

| Tool | Purpose |
| --- | --- |
| `trace_diff.py` | Compares two `ALPHABOX_RECORD` files ([headless.md](headless.md)) stream by stream, one stream per device and processor. |

A record is the oracle for a change to a device the guests reach through
registers: run the same boot with the base and the changed build, with
`ALPHABOX_RECORD` naming the device, and compare. Give both the same
`time = "2026-01-01 00:00:00";` in `sys0` (`EXTRA_CFG=` for `srm_probe.sh`),
or the DPR's power-up time differs. The streams of the DPR, the flash, the
I2C controller and PCI configuration space then come out the same in every
run of one build, on one processor or four. The Tsunami's
registers also carry the interrupt path, which lands at a different place
in every run; leave it out with

```
trace_diff.py --ignore 'csr cpu\d+ [rw] cchip (80|200|240|280|2c0|600|640|680|6c0) ' base.txt head.txt
```

Each difference left is then a change in what the guest was told, with one
gap: the ignored registers are the interrupt path, so a "same" says nothing
about which interrupt lines were asserted when. The routing the console
programs is still compared (it is in configuration space); a change to the
line state itself, such as a shared input's wire-OR, needs a unit test and
a guest boot that exercises it. A device
that depends on nothing but its accesses (the flash, an EEPROM, the I2C
parts) can also be tested by replay: record it with `raw` in the list,
keep the part of the file that matters under `test/unit/data/` with `#`
lines saying where it came from and what was left out, and have its unit
test read it with `test/unit/AccessReplay.hpp`, feed the writes and check
every read. `flash_test` replays the ES40 console setting a console
variable, `i2c_test` the DS20E console's boot through its PCF8584. A fix
that changes what the device answers changes the record too: record it
again with the fixed build, and say so in the commit.

### Builds and helpers

| Tool | Purpose |
| --- | --- |
| `build_lanes.sh` | Builds every configured build directory and runs its unit tests (`ctest`: `test/unit/` and the FP differential); a lane whose tests fail counts as failed. |
| `build_revs.sh` | Builds every commit of a series in a worktree. |
| `keys_for.py` | Key tokens for a line of text. |
| `alphabox hvprobe` | Hypervisor.framework's mechanics measured on the host. |

`alphabox hvprobe` is not a script but a subcommand, built with
`-DALPHABOX_HVF=ON` (macOS on Apple silicon, JIT lane). It measures ID
registers, exit round trips and a stage-1 fault handled inside the VM, for
[hypervisor.md](hypervisor.md).

### Unit tests

`test/unit/` holds tests that build one device's source with the rest of
the machine stubbed out and drive it through its registers, as a guest
driver does. Each is an executable in every build directory, and `ctest
--test-dir <build>` runs them all.

| Test | What it drives |
| --- | --- |
| `kbc_test` | The 8042 keyboard controller, keyboard and mouse (`Keyboard.cpp`): ports 60h and 64h, host keys and mouse motion, save and restore. |
| `uart16550_test` | The 16550 UART model (`Uart16550.cpp`): the register sequences consoles and drivers make, against the data sheet's answers. |
| `irqlines_test` | The wire-OR of the chipset's interrupt inputs (`InterruptLines.hpp`): devices and functions sharing an input, from several threads at once. |
| `pcicfg_test` | The configuration space every PCI device inherits (`PCIDevice.cpp`): write masks, BAR and ROM sizing, access sizes, accesses past a function's 256 bytes, the state file. |
| `pcibridge_test` | The PCI-PCI bridge (`bridge/`), configured as the ES40 console configures one: the type 1 header, configuration cycles once the bus is numbered, the windows and what a master abort reads, the interrupt swizzle, the secondary bus reset. |

### The original regression test

On Linux, `cd test/rom && bash test.sh` is the original firmware regression
test; it cannot pass on macOS (BSD `sed`). It deletes the tracked ROM files at
the end; restore them with `git checkout -- test/rom/` before committing.

## Verifying a change

A change is ready when:

1. **Builds:** every build configuration compiles (interpreter with SDL3,
   JIT, headless; see [building.md](building.md)).
2. **SRM:** `srm_run.sh` gives `diff clean` on each build, with
   `0 mismatches` on a JIT_VERIFY build.
3. **Storage:** for changes to the disks, the SCSI bus or the 53C8xx,
   `cdb_test.sh` passes.
4. **Graphics:** for graphics changes, `vga_boot.sh` reproduces the known
   settled frames. Run it alone: on a busy host the cursor phase can hide
   one of the two frames.
5. **Devices:** a device is checked against the real firmware or guest
   driver that uses it, not only against a reference model. The Cirrus
   blitter, for example, matched QEMU's model exactly and still dropped a
   blit that the Windows 2000 driver depends on.
6. **Speed:** a change made for speed is measured with `perf_ab.py`, on a
   workload that the change could actually affect. Under 10% on a single
   pair of runs is noise on a shared host.
   [performance.md](performance.md) says what bounds each workload. Several
   plausible optimizations have measured as no change at all, and saying so
   in the commit message is part of the job.

## Peripherals and open work

[peripherals.md](peripherals.md) lists the PCI devices the ES40 firmware
knows, what Alphabox emulates, and a suggested order for more.
[backlog.md](backlog.md) is the plan for what the machines' processors and
chipsets do not model yet.
