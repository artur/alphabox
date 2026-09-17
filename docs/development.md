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
write their output to `$AXPBOX_WORK` (default `lab/`, which git ignores).
They stop only the emulator processes they started.

| Tool | Purpose |
|---|---|
| `srm_run.sh` | SRM firmware boot to `P00>>>` for one build, with a console-log diff against `test/rom/axp_correct.log` and the JIT_VERIFY mismatch count. Give each build its own `PORT` to run several at once. |
| `srm_probe.sh` | SRM probes: CPU count, `memory.bits`, SCSI/IDE/floppy drives, console commands, and the SIGTERM, disconnect and halt exit paths. |
| `vga_boot.sh` | SRM on the S3 or Cirrus VGA console (`CARD=s3\|cirrus`, `CHIP=gd5430\|gd5434`), window-less; reports the hashes of the frames the screen settles on. The known sets are in the script header. |
| `win_bench.sh` | Headless guest boot on a throwaway clone of an installed guest: final screenshot, host CPU use, per-CPU MIPS. |
| `win_workload.sh` | Times a CPU-bound command inside a booted Windows guest (a real-workload benchmark). |
| `win_storage.sh` | Adds a storage controller (`CTRL=<class>`) with a FAT16 test disk to a clone of an installed Windows guest, has Windows copy a file on it, and checks the copy on the host. |
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

## Peripherals

[peripherals.md](peripherals.md) lists the PCI devices the ES40 firmware
knows, what AXPbox emulates, and the order in which more are being added.
