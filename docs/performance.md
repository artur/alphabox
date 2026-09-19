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

- A compiled chain runs **~27 instructions** before returning to the
  dispatcher.
- **~80%** of those returns are cached-link misses, **18%** are computed
  jumps whose inline cache missed. The computed-jump cache hits only ~40%
  of the time.
- The link misses are **not** a fanout problem. Instrumentation buckets the
  missing links by how many distinct successors the source block has:
  `f1=690 sources / 811278 misses`, and f2 through f5+ are exactly zero.
  Every one of them has a single, stable successor. More link slots would
  do nothing.
- The mechanism is that an address-space switch bumps a **global** epoch
  counter, and the link guard compares each successor's validation epoch
  against it -- so one context switch invalidates every cached link in the
  engine at once. ~690 hot blocks times ~1160 invalidations per window
  accounts for the miss count.
- Removing that invalidation (as an unsafe experiment) did not help, but
  that experiment was run on the console-scrolling workload, which is
  drawing-bound and could not have shown it either way. It needs redoing
  with `win_workload.sh` before the result means anything. The same mistake
  is described under [the cycle counter](#the-cycle-counter).

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
