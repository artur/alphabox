# How fast it is, and what that depends on

Alphabox is fast enough that the interesting question is no longer "is the
JIT working" but "what is this particular guest waiting for". This file
records what has been measured, how to measure it again, and -- the part
that saves the most time -- which workloads **cannot** be improved by making
the processor emulation faster, because something else bounds them.

Every number here was taken on an Apple M-series host with the JIT build.
Absolute figures move with the host; the shape of the findings does not.

## What bounds what

This is the first thing to establish about any workload, before optimizing
anything for it.

| Workload | Bounded by | Faster CPU emulation helps? |
| --- | --- | --- |
| `cpu_bench.sh` (an Alpha loop, no OS) | the emulator | yes -- this is the honest measure |
| A CPU-bound command in a booted guest (`win_workload.sh`) | the emulator, plus whatever the OS does around it | yes |
| Booting Windows to the desktop | the guest's own timed delay loops | **no** |
| Scrolling a console window (`s3_bench.sh`) | the drawing engine | **no** -- see [the drawing engine](#the-drawing-engine) |
| An idle desktop | idle pacing (by design) | no |

The boot case is worth spelling out, because it looks like a CPU benchmark
and is not. A guest's device initialisation waits by spinning on the cycle
counter until real time passes. The emulator therefore sits at 100% of a
core for as long as the guest asked to wait, whatever speed it runs at:
making the emulator faster buys more spinning, not an earlier desktop. A
"MIPS" number taken from a booting -- or idle -- guest counts those spin
iterations and means nothing.

## The numbers

- **Translated Alpha code**: about 4300 MIPS on a tight arithmetic loop and
  4000 MIPS on a memory loop, per emulated CPU (`cpu_bench.sh`). For scale,
  the fastest Alpha ever built, the EV7z at 1.30 GHz, was about 10300 MIPS.
- **A CPU-bound command inside Windows 2000**: a 15-million-iteration `cmd`
  batch loop takes about 62 s (`win_workload.sh`, median of three). This is
  the most representative number we have, because it is real guest code that
  neither waits on real time nor draws anything.
- **An idle two-CPU Windows 2000 desktop**: 3-6% of one host core.
- **Coverage**: 96.6-96.9% of guest instructions execute as translated host
  code; 3.8% of the time is interpretation. Coverage is not the bottleneck
  and has not been for some time.

## What the JIT is actually doing

From `JIT_STATS` on a Windows 2000 guest. Treat its *counts* as reliable and
its *time shares* as not: the instrumented build runs at a fraction of the
production speed, so its own counters are part of what it measures.

- Chain length is a property of the **workload**, not of the engine. The
  same build gives 27 instructions per chain at 96.6% native while scrolling
  a console, where the guest traps to the drawing engine constantly, and
  **878 instructions at 99.7% native** running a CPU-bound command. Quote
  the one that matches the workload being discussed.
- **~80%** of those returns are cached-link misses, **18%** are computed
  jumps whose inline cache missed. The computed-jump cache hits only ~40%
  of the time.
- The link misses are **not** a fanout problem, and not a prediction problem
  either. Bucketing them by how many distinct successors the source block
  has gives `f1=690 sources / 811278 misses` with f2 through f5+ exactly
  zero, and counting whether the successor was already in a slot gives
  **stale 5766038 against new target 4378**. 99.92% of link misses are the
  guard rejecting a successor that was right there. No larger or cleverer
  successor cache can address any of it.
- The mechanism is that an address-space switch bumps a **global** epoch
  counter, and the link guard compares each successor's validation epoch
  against it -- so one context switch invalidates every cached link in the
  engine at once. ~690 hot blocks times ~1160 invalidations per window
  accounts for the miss count.
- Disabling the address-space half of that invalidation, as an unsafe
  experiment on the CPU-bound workload, is worth about 3%: 59.3 s against
  57.2 s, medians of three, with the run ranges overlapping. That measures
  one source of staleness, not the design -- the epoch is bumped by every
  ITB invalidate as well. A fix has to remove the dependency, not one of
  its causes.

## The cycle counter

A Windows 2000 guest reads RPCC about **21 million times a second** -- 1.5
billion in ninety seconds of booting -- and never reads RC or RS at all. A
host profile of the emulator during that boot puts 88% of the CPU thread
inside the helper serving them.

Each read syncs the wall-clock cycle counter, which means reading the host
clock. On this host the same underlying counter costs 14.8 ns through
`std::chrono::steady_clock::now()`, 5.0 ns through `mach_absolute_time()`,
and 0.28 ns read as a register. The sync now reads the register where there
is one.

This is worth 6% of a CPU-bound guest workload: the `cmd` loop above went
from a 65.8 s median to 61.6 s, with the two sets of runs not overlapping
(70.0/65.8/65.6 against 61.6/59.5/61.6).

Finding that took three tries, and the two failures are the lesson. The
arithmetic benchmark showed nothing, because a tight Alpha loop never reads
RPCC. Booting showed nothing, because the guest spends those reads inside
delay loops bounded by real time -- a cheaper read buys more spinning, not
an earlier desktop. Only a guest doing real work with an operating system
around it exercises the path at all. **A change can be real and still be
invisible to every benchmark you happen to have.**

## What other emulators do here, and what they gave up on

Surveyed in September 2026 against Dolphin, QEMU TCG, PCSX2, PPSSPP,
RPCS3, box64, FEX, dynarmic and the binary-translation literature. The
three findings that agreed across independent sources:

- **Nobody else validates a block link against a global counter.** Dolphin
  puts the processor mode into the block's *identity* and treats a link as
  valid until the target block is destroyed, keeping a reverse index
  (`links_to`) so destroying a block unpatches its inbound links. QEMU
  reaches the same place by keying blocks physically and chaining only
  within a page. Either way the invalidation is surgical, never global.
- **Nobody else consults a data structure on a link hit.** Dolphin, PCSX2,
  PPSSPP and RPCS3 all patch a direct branch into the compiled code, so a
  taken link is one predicted branch; Dolphin even folds the cycle-budget
  test into its condition. Our two-slot cache costs four dependent loads
  and an indirect branch the host cannot predict.
- **Nobody else uses a per-site inline cache for indirect branches.**
  Dolphin has none at all: it makes guest calls and returns into real host
  `BL`/`RET` so the host's own return predictor works, measured at 8%.
  QEMU uses one large PC-hashed table probed inline, measured at 95.8% hit
  against our 40%. Alpha tags `JSR`/`RET` architecturally, so we can
  classify at compile time with no heuristic.

  The point of that trick is finer than "keep a shadow stack", and it is the
  same in Rosetta 2 (which reserves a memory region named *Rosetta Return
  Stack* for it) and in Transitive's patent on the technique: **validate in
  software, predict in hardware**. A guest return compiled as anything other
  than a host return is an indirect branch, and the patent puts the cost
  plainly -- it is "very difficult for target hardware to effectively
  predict the addresses of indirect branches, and not surprisingly such a
  solution can perform very poorly". The software stack only checks that the
  return went where it should; the host's return-address predictor is what
  makes it fast.

### The trace tier stays dormant

`jitengine.cpp` already carries the note that traces preempt block chaining
and measured as a net loss. The outside evidence is emphatic enough that
this should not be revisited without a new reason:

- IBM's production trace JIT, retrofitted from its own method JIT, reached
  **95.5%** of it on DaCapo -- 10.5% more code, 27% more compile time, worse
  startup -- winning on one benchmark of thirteen and losing by over 15% on
  three (CGO 2011). The same authors later measured each basic block
  duplicated **13 times** across traces, with 40% of traces short-lived.
- Mozilla deleted TraceMonkey entirely, 67,643 lines, because a method JIT
  with type inference was faster on average and being knocked off trace
  "happens a lot - more than anyone expected".
- The one positive trace-versus-method result is against HotSpot's
  *non-optimizing* compiler; against the real one the same system reached
  67% / 85% / 93% on SPECjbb2005 / SPECjvm2008 / DaCapo.

## The drawing engine

Listing 5551 files in a console window draws **903,099,931 pixels**, over
259,344 drawing commands and 903,099,931 transfers -- one transfer per
pixel. The guest's driver sets a rectangle up with the engine and then feeds
it the pixels one at a time, about 3500 per command, so the cost of this
workload is the cost of a pixel and nothing else. That is why it is listed
above as drawing-bound: the processor is barely involved.

`ALPHABOX_BLIT_STATS=1` makes the engine report this itself.

## Changes that did not pay off

Recorded so they are not tried twice. Each is still in the tree where it is
strictly better code; none of them is in it for a speedup.

| Change | Expectation | Measured |
| --- | --- | --- |
| Device lookup: flat bounds array + last-hit cache instead of walking ~100 separately allocated ranges | cheaper MMIO | no change (18 s either way) |
| Not invalidating block links on an address-space switch (unsafe experiment) | longer chains | slower, if anything |

And two that did, for contrast:

| Change | Measured |
| --- | --- |
| The 8514/A pixel routine: mask instead of six divides, direct memory instead of eight virtual calls | the listing 20 s -> 18 s |
| Reading the host clock as a register instead of through `steady_clock` | the `cmd` loop 65.8 s -> 61.6 s |

## Measuring it yourself

All of these live in `test/tools/` and are described in
[development.md](development.md).

- `cpu_bench.sh` with `bench_image.py` -- translated-code throughput, no OS
  involved. Never on a `JIT_STATS` build.
- `win_workload.sh` -- a CPU-bound command inside a booted guest, with
  `REPEATS=` to take a median and remove boot-to-boot variance, which is the
  dominant noise.
- `s3_bench.sh` -- times a directory listing scrolling in a console window,
  and reports the drawing engine's own counters. It writes the emulator's
  pid to `emulator.pid` in its run directory: **profile that pid**, because
  other emulators may be running on the same host.
- `win_bench.sh` -- boots a guest and reports MIPS, which are meaningful
  only while the guest is busy with real work.

Two practical notes, both learned the hard way. A guest run that "did not
reach the desktop" is either a bugcheck or a timeout, and only the last
framebuffer tells you which -- convert it with `ppm2png.py` and look.
And when comparing two builds, interleave the runs: a difference of under
10% on a single pair of runs is noise on a shared host.
