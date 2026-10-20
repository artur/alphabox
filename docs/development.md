# Development and testing

## Where things are

| Directory | Contents |
| --- | --- |
| `src/` | the emulator; [source-layout.md](source-layout.md) maps it |
| `test/rom/` | the ES40 firmware regression test and its reference console log |
| `test/tools/` | the test tools below |
| `test/unit/` | unit tests: one model or device's source with the rest of the machine stubbed out |
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
| `srm_dwell.sh` | Boot to the prompt and sit there for N seconds: what the console's polling loop costs. |
| `srm_cfg.py` | The test machine's configuration, generated from `test/rom/es40.cfg`; the three scripts above and `gdb_check.sh` use it. |
| `gdb_check.sh` | The gdb stub against the console: attach at reset, breakpoints (one inside a compiled loop), steps, registers, memory, the monitor commands, the clock across a stop, Ctrl-C, one or more CPUs ([debugging.md](debugging.md)). |
| `srm_console.py` | Drives the console over its telnet port: waits for the prompt, runs commands, stops the emulator. |
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
compares the command output of two runs.

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
- `EXTRA_CFG=<file>` adds any configuration: bridges, more devices.

### CPU and JIT correctness

| Tool | Purpose |
| --- | --- |
| `JIT_VERIFY` build | Every compiled block re-run by the interpreter and compared. A clean SRM boot is ~130 million blocks. |
| `ALPHABOX_JIT_FPTEST=1` on that build | The JIT's inline IEEE operations checked over 8.5 million cases. |
| `alphabox_fptest` (built with every lane; `ctest` runs it) | The interpreter's IEEE and VAX operate instructions without a guest, about 20 million cases: IEEE results and flags against the host FPU in every rounding mode, VAX results against an exact model, hand tables from the manuals, and the same routines as they were at the commit `ALPHABOX_FP_BASELINE` names. Each difference from that baseline must be one of the intended changes listed in `test/fp/fp_test.cpp`. |
| `smc_test.sh`, `smc_image.py` | A guest that rewrites its own compiled code: the emulator halts if the change was seen, hangs if the stale block ran. |
| `bench_image.py` | A bootable image from a list of instruction words. |
| Bring-up traces | `ALPHABOX_TRACE_UNKNOWN`, `_CALLS`, `_I2C`, `_MP`, `_FLASH`, `ALPHABOX_DUMP_MEMORY`: what a firmware asked for and did not get ([headless.md](headless.md)). |

`JIT_VERIFY` finds disagreements between the JIT and the interpreter, not
disagreements with the architecture; for those see
[cpu-fidelity.md](cpu-fidelity.md).

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
| `cdb_test.sh` | The SCSI disk's conformance test (`alphabox cdbtest`): status, sense, data and lengths of each command, with no guest. |
| `win_storage.sh` | A storage controller (`CTRL=<class>`) with a FAT16 disk added to a Windows guest, which copies a file on it. |
| `fat_disk.py` | Builds and inspects small FAT16 test disks. |
| `aboot_disk.py` | A sparse disk SRM boots Linux from: boot block, BSD disklabel, aboot's `bootlx`, a `/boot` and a root partition ([linux.md](linux.md)). |

`win_storage.sh` checks the copy on the host; `BRIDGE=<class>` puts the
controller behind a PCI-PCI bridge.

`cdb_test.sh` builds an image disk, a read-only one, two RAM disks and two
CD-ROM drives (one empty) on a SCSI bus of their own and drives them the
way a host adapter does: arbitration, selection, IDENTIFY, the CDB, the
data, the status and COMMAND COMPLETE, plus the ATAPI form with no message
phases that the IDE and USB paths use. Each check compares the answer
with the standards (SCSI-2, SPC-3, SBC-3, MMC-3); a command that would stop
the emulator fails its check. It takes a second, needs no firmware, and
is the first thing to run after a change to `CDisk` or the SCSI bus.

### Builds and helpers

| Tool | Purpose |
| --- | --- |
| `build_lanes.sh` | Builds every configured build directory. |
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
3. **Storage:** for changes to the disks or the SCSI bus, `cdb_test.sh`
   passes.
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
