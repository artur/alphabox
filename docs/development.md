# Development and testing

## Layout

| Directory | Contents |
|---|---|
| `src/cpu/` | the Alpha CPU interpreter, PALcode fast paths, IEEE/VAX floating point |
| `src/jit/` | the asmjit translator (x86-64 and AArch64 emitters) |
| `src/system/` | the machine, the Tsunami/Typhoon chipset, configuration, NVRAM |
| `src/devices/isa/` | devices behind the ISA bridge |
| `src/devices/pci/` | PCI devices, grouped by bus rather than by chip |
| `src/devices/storage/` | disk and image back ends |
| `src/devices/video/` | the MAME-derived VGA core, the shared card base, and one directory per card family (`s3/`, `cirrus/`) |
| `src/devices/net/` | network back ends |
| `src/gui/` | the GUI layer; SDL3 is the maintained back end |
| `test/` | firmware regression data and the test tools |
| `docs/` | this documentation |

New code uses `std::thread`/`std::mutex`/`std::chrono`, not the inherited
Poco-style wrappers in `src/base/`. Format changed lines with the repository's
`.clang-format` (`git clang-format`).

## Test tools

The scripts in `test/tools/` find the repository from their own location and
write their output to `$ALPHABOX_WORK` (default `lab/`, which git ignores).
They stop only the emulator processes they started.

| Tool | Purpose |
|---|---|
| `srm_run.sh` | SRM firmware boot to `P00>>>` for one build, with a console-log diff against `test/rom/axp_correct.log` and the JIT_VERIFY mismatch count. Give each build its own `PORT` to run several at once. |
| `srm_probe.sh` | SRM probes: CPU count, `memory.bits`, SCSI/IDE/floppy drives, a NIC (`NIC=<class>`, optionally on a UDP link to `net_peer.py` with `NET_PEER=<nic port>:<peer port>`), any extra configuration (`EXTRA_CFG=<file>`: bridges, more devices), console commands, and the SIGTERM, disconnect and halt exit paths. |
| `JIT_VERIFY` + `ALPHABOX_JIT_FPTEST=1` | Every compiled block re-run by the interpreter and compared (a clean SRM boot is ~130 million blocks), and the JIT's inline IEEE operations checked over 8.5 million cases. Finds JIT/interpreter disagreements -- not disagreements with the architecture, for which see [cpu-fidelity.md](cpu-fidelity.md). |
| Bring-up traces | `ALPHABOX_TRACE_UNKNOWN`, `ALPHABOX_TRACE_CALLS`, `ALPHABOX_TRACE_I2C`, `ALPHABOX_TRACE_MP`, `ALPHABOX_TRACE_FLASH`, `ALPHABOX_DUMP_MEMORY`: what a firmware asked for and did not get. See [headless.md](headless.md). |
| `net_peer.py` | The other end of a NIC's UDP link: answers ARP, BOOTP and TFTP with a CALL_PAL HALT image, so `boot eia0`/`boot ewa0` exercises a full network boot. |
| `vga_boot.sh` | SRM on the S3 or Cirrus VGA console (`CARD=s3\|cirrus`, `CHIP=gd5430\|gd5434`), window-less; reports the hashes of the frames the screen settles on. The known sets are in the script header. |
| `cpu_bench.sh`, `bench_image.py` | How fast the emulator executes Alpha code (see [performance.md](performance.md) for what each workload is actually bounded by): a boot block of known instruction count, run at two sizes so the console's boot subtracts out. `BODY=` sets the loop length -- long measures the translated code, short measures leaving one block for the next. Do not use a `JIT_STATS` build: its counters are part of what would be measured. |
| `win_bench.sh` | Headless guest boot on a throwaway clone of an installed guest: final screenshot, host CPU use, per-CPU MIPS. **The MIPS are only meaningful if the guest is busy**: at an idle desktop the number is the guest's idle loop (and the host will sit near 1% CPU, which is the tell). |
| `win_workload.sh` | Times a CPU-bound command inside a booted Windows guest (a real-workload benchmark). Narrow: the `cmd` loop it usually runs is integer-only over a handful of code pages, so it cannot show the FP path, the address path or call overhead. Prefer `nt_bench.sh` for those. |
| `nt_bench.sh`, `ntbench/axpbench.c` | A real NT application benchmark, per JIT datapath (`alu branch call ldst stride fp byte div sort`), plus JScript under `cscript` and `makecab` LZX. The guest times itself and writes the numbers to `C:\NADA\OUT.TXT`, which the harness reads back with mtools -- no keyboard injection and no framebuffer timing. Needs [nada](https://github.com/artur/nada) for `-t alpha-windows` (`NADA=<dir>`, default `~/Documents/proj/nada`) and `mtools`. The disk staging, Startup hook, shutdown helper and end-detection are from nada's own `tests/run-ntalpha.sh`. |
| `s3_bench.sh` | Graphics workload: boots an installed Windows guest, opens a command prompt and times a directory listing scrolling in it -- what a guest feels as a slow or fast card. With `ALPHABOX_BLIT_STATS=1` the drawing engine reports its pixels, commands and transfers. Writes the emulator's pid to `emulator.pid`: profile *that* pid, since other emulators may be running on the host. |
| `win_storage.sh` | Adds a storage controller (`CTRL=<class>`) with a FAT16 test disk to a clone of an installed Windows guest, has Windows copy a file on it, and checks the copy on the host; `BRIDGE=<class>` puts the controller behind a PCI-PCI bridge. |
| `build_lanes.sh`, `build_revs.sh` | Build every configured build directory, or every commit of a series in a worktree. |
| `ppm2png.py`, `keys_for.py`, `mips_summary.py`, `fat_disk.py` | Helpers: frame-dump conversion, key tokens for a line of text, MIPS summaries, FAT16 test disks. |

On Linux, `cd test/rom && bash test.sh` is the original firmware regression
test; it cannot pass on macOS (BSD `sed`). It deletes the tracked ROM files at
the end; restore them with `git checkout -- test/rom/` before committing.

## Verifying a change

A change is ready when:

1. **Builds:** every build configuration compiles (interpreter with SDL3,
   JIT, headless; see [building.md](building.md)).
2. **SRM:** `srm_run.sh` gives `diff clean` on each build, with
   `0 mismatches` on a JIT_VERIFY build.
3. **Graphics:** for graphics changes, `vga_boot.sh` reproduces the known
   settled frames. Run it alone: on a busy host the cursor phase can hide
   one of the two frames.
4. **Devices:** a device is checked against the real firmware or guest
   driver that uses it, not only against a reference model. The Cirrus
   blitter, for example, matched QEMU's model exactly and still dropped a
   blit that the Windows 2000 driver depends on.
5. **Speed:** a change made for speed is measured, on a workload that the
   change could actually affect, with the runs interleaved between the two
   builds -- under 10% on a single pair of runs is noise on a shared host.
   [performance.md](performance.md) says what bounds each workload; several
   plausible optimizations have measured as no change at all, and saying so
   in the commit message is part of the job.

## Peripherals

[peripherals.md](peripherals.md) lists the PCI devices the ES40 firmware
knows, what Alphabox emulates, and the order in which more are being added.
