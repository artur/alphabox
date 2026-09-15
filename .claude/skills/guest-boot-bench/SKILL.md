---
name: guest-boot-bench
description: Boot an installed Windows 2000 guest headless on a throwaway clone, check what it reached from the final framebuffer, and measure host CPU and JIT MIPS. Use after CPU, JIT, interrupt, timer, SMP, storage or input changes that SRM alone can't exercise, and for any JIT performance comparison.
---

# Guest boot and benchmark

`test/tools/win_bench.sh` boots an installed guest from an APFS clone of its
install directory (the install itself is never modified), with SDL's dummy
video driver and framebuffer dumps, for a fixed time. It prints the last
screen as a PNG, host CPU% near the end, per-CPU MIPS (JIT_STATS builds),
idle-loop recognition and notable warnings.

```bash
test/tools/win_bench.sh <label> <axpbox-binary> <install-dir> <cfg> <seconds> [VAR=value ...]
# output: $AXPBOX_WORK/bench-<label>/ (bench.log, last.png, fb/)
```

`AXPBOX_WORK` defaults to `<repo>/lab` (git-excluded), where the guest
installs live. Never boot the original install directory by hand: always
work on a clone or copy.

## Guests on the development host

| Install dir | Config | Expected |
|---|---|---|
| `win2k-installed` | `es40-window-smp.cfg` (2 CPUs, IDE disk + RC2 CD) | Windows 2000 Professional RC2 (build 2128) desktop within 300 s |
| `win2k-jp-install` | `es40-2cpu.cfg` | Japanese Windows 2000 Server beta 2 (build 1877) logon screen within 330 s |

The beta's HAL only sends IPIs to CPUs 0-1: run it with 2 CPUs (4 CPUs
hangs at boot). It needs the EV6 `a221264.pal` on its system partition and
the EXC_SUM MTPR fix. Ctrl+Alt+Delete is Ctrl+Alt+End, or a
`hotkey.ctrl_alt_delete` binding (e.g. `"GUI+Shift+D"` on a Mac keyboard).

## Always check

1. **The screenshot** (`last.png`): desktop / logon screen, not a blue
   screen, a hang at the boot progress bar or a black screen. The rest of
   the output means nothing if the guest didn't get there.
2. **Idle recognition**: `%CPU-I-IDLE: CPUn idle loop recognized` for each
   CPU (and `parked-processor loop` for the other), and host CPU at idle in
   the low single digits (about 4-6% on the development Mac). Tens of
   percent at an idle desktop means idle pacing broke.
3. **Warnings**: no `Emulator Failure`, `OPCDEC`, `SYS-W-UNKNOWNCFG` or
   `Unknown TIG` lines.

Useful extra environment: `AXPBOX_IRQSTATS=1` (interrupt rates every 5 s,
e.g. `eir 4:N` halt-line interrupts), `AXPBOX_MEDIA_SWAP=<iso1>:<iso2>:<ms>`
(CD change stress), `AXPBOX_KEYSCRIPT` / `AXPBOX_AUTOMOUSE` (input
injection, see README "Headless testing").

## MIPS benchmark

```bash
test/tools/win_bench.sh rc2-mips build-jit-stats-sdl/axpbox win2k-installed es40-window-smp.cfg 300 AXPBOX_NO_IDLE=1
```

Compare per-CPU p50 against a run of the parent commit made the same way,
on a quiet host (no builds or other guests running). Reference on the
development Mac (arm64-jit at 7e2e399): about 2250-2265 MIPS p50 per CPU,
host CPU ~138%.

**Caveat: this measures the idle loop.** With the guest idle at its desktop
and idle pacing off, the steady state is the NT idle loop (a JIT_REGPROF
build shows a handful of registers with identical access counts). Use it to
catch regressions in dispatch and chaining, not to judge optimizations of
real guest code: for those, measure a workload that keeps the guest busy
(the boot phase, or a CPU-bound program run inside the guest).

JIT counters on the same run: a `build-jit-stats-sdl` log carries per-window
throughput, cold-path reasons (`int`, `int-pal`, `budget`, ...) and bail
causes; `build-jit-regprof-sdl` adds exec-weighted code expansion, hot
registers, the memory-op share and DPC-reuse candidates. Early windows cover
boot; later ones the idle desktop.
