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
  **stale 5766038 against new target 4378** on the scan-style exits, and
  **stale 65889244 against new 163168** on the static exits (the AArch64
  data links, which were not counted at all until the census below). 99.9%
  of link misses are the guard rejecting a successor that was right there.
  No larger or cleverer successor cache can address any of it.
- The mechanism is a **global** epoch counter that the link guard compares
  each successor's validation epoch against, so one bump invalidates every
  cached link in the engine at once.
- **Which event bumps it is the part that was guessed wrong, and it changes
  the conclusion.** Attributing every bump to its cause (the census printed
  by `JIT_STATS`, and `ALPHABOX_TRACE_ICFLUSH=1` for the PC behind it):

  | Cause | Bumps, Windows run | Bumps, 18 s SRM boot |
  | --- | --- | --- |
  | guest `IC_FLUSH` | 1692262 | 1830608 |
  | address-space switch | 34876 | 2 |
  | `tbis` | 19385 | 0 |
  | `tbia` | 182 | 4 |

  It is not the context switch. It is the guest's own icache flush, and in
  the SRM boot **100% of 2.74 million of them come from a single PAL
  instruction** -- `77ff1310`, `HW_MTPR IC_FLUSH`, four NOPs and a `HW_RET`
  around it: the PALcode's IMB routine. On real hardware that costs tens of
  cycles and nobody cared; here each one rejects the entire link graph.
- **And none of it touches the benchmark.** Those flushes stop before the
  measured workload begins: over the last hundreds of `JIT_STATS` windows of
  a CPU-bound `cmd` loop the deltas are zero flushes, zero `tbis`, and about
  two address-space switches per 100M instructions. Stale link misses fall
  to ~1000 per 100M instructions. All of the staleness above is accumulated
  while **booting**, which is delay-loop-bound and cannot speed up.
- So the earlier "3% from disabling the address-space half" was measuring
  noise plus boot, and the ranked plan built on it -- branch patching, an
  inline lookup probe, a return stack, cross-page linking -- targets at most
  the 3.4% below, of which linking is a sliver. The reverse-index
  implementation is in the tree behind `ALPHABOX_JIT_DLINK=1`, default off,
  for whoever needs firmware to link well.

## Where the time goes on a CPU-bound guest workload

Same windows as above, the `cmd` loop running, per 100M guest instructions:

```
native 100.0% | chain avg 2020 instr over 49494 dispatches
time-split: compiled 96.6% | interp 0.0% | dispatch 3.4%
bail-cause: link 0% | jump 0% | gate/other 100%
helper calls: read 373634 write 43096 locked 85983 stc 85983 indirect 86090
helper bails: unaligned 0 | tbmiss 0 | acv 0 | fault 0 | mmio 0
throughput 1597 MIPS | 105 host-bytes/instr (static avg)
```

Read it in this order:

- **96.6% of the time is inside compiled code.** Dispatch is 3.4%, and 100%
  of the returns to the dispatcher are the budget/interrupt gate, not a
  missed link. Nothing on the dispatch side can be worth more than 3.4%.
- **675k helper calls per 100M instructions**, one per ~148 guest
  instructions, and *none* of them are hard cases -- every bail counter
  (unaligned, TB miss, ACV, fault, MMIO) is zero. Integer loads do have an
  inline fast path, so these are **misses of the inline data-page-cache
  probe**, which is what a helper call costs when the page simply is not in
  the slot.
- The cache behind that probe was 64 slots, indexed by `(va >> 13) & 63` --
  a *slice* of the address. Classifying the misses says **100% "another page
  is in this slot"**, with mode, address space, MMIO and empty all at zero.

### Slicing an index, and why fixing it still lost

The instinct is to make the table bigger, and it is wrong. Two pages that
differ only *above* the sliced field collide whatever the table size, which
is exactly what a kernel page and a user page do. Measured: 64 -> 256 slots
removed 8% of the misses and did not move the workload.

Hashing the index instead -- fold the high bits down before masking --

```c
u64 h = va >> 13;  h ^= h >> 13;  h ^= h >> 26;  return h & kDpcMask;
```

costs two instructions in the inline probe and, on matched `JIT_STATS`
windows of the same phase:

| | read helpers /100M instr | write helpers /100M instr |
| --- | --- | --- |
| sliced | 364984 | 45560 |
| hashed | **242727** (-33%) | **115** (-99.7%) |

On the `cmd` loop that was worth 2-3%: sliced medians of 59.4 s and 58.4 s
against 57.3 s twice.

**And then it lost on the other benchmark, by more.** `nt_bench.sh axp all`,
three runs per arm, no overlap between them:

| | round 1 | round 2 | round 3 |
| --- | --- | --- | --- |
| sliced | 24429 | 24609 | 24429 ms |
| hashed | 26054 | 25968 | 26007 ms |

6.5% slower, and far better resolved than the win it contradicts -- under 1%
spread inside each arm, because the guest times itself instead of being timed
by watching a console window.

The two are reconcilable, and the reconciliation is the useful part. The hash
costs two instructions on every **hit** and removes only **conflict** misses.
The `cmd` loop has a small working set where kernel and user pages alias, so
it is nearly all conflict misses and the hash pays. The `nt_bench` sections
either fit the cache (no misses at all, so the two instructions buy nothing)
or stream over 48 MB (all capacity misses, which no index function helps) --
and its compiler runs no optimizer, so its code is unusually memory-dense,
which weights the per-hit cost heavily. `stride`, the section built to stress
this exact path, came out a wash.

**So it was reverted.** Retrying is only worth it with an index that costs at
most one extra instruction -- a single fold such as `(va>>13) ^ (va>>31)`
separates kernel from user for one `eor` -- or with one that costs nothing.

Two things to carry away. The change is a **trade**, not an improvement: fewer
misses against a higher price per hit, and which way it settles is a property
of the workload, not of the code. And **the index lives in three places** --
`CAlphaCPU::dpc_index`, the AArch64 probe in `jitemit_a64.hpp`, and three
sites in the x86-64 emitter. Change one and compiled code and the helpers
disagree about where a page lives.

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
| Hashing the data-page-cache index instead of slicing it | fewer of the 373k read-helper calls per 100M instructions | it worked -- reads 364984 -> 242727, writes 45560 -> 115 -- and still lost: 2-3% faster on the `cmd` loop, **6.5% slower** on `nt_bench.sh axp` (24429 against 26007 ms, three runs each, no overlap). Two instructions on every hit, to remove only conflict misses. Reverted |
| Computed-jump inline cache 1024 -> 16384 entries (`kIndBits` 10 -> 14), on QEMU's measured -8.7% wall for the same change on an Alpha guest | fewer dispatcher round-trips | dispatcher round-trips did roughly halve at matched window indices, and the wall clock did not resolve: 57.3 against 55.3 in round one, 56.2 against **58.4** in round two. Opposite signs, so no effect we can measure |
| Data page cache 64 -> 256 slots per direction (`kDpcBits` 6 -> 8), 512 KB -> 2 MB of coverage | fewer of the 373k read-helper calls per 100M instructions | read-helper calls 373634 -> 343914, only 8%, and the workload did not move (61.7 s against 59.4 s). The probe misses are not conflict misses, so slot count is the wrong lever |

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
  dominant noise. Know its limits: the `cmd /c for /l` loop it usually runs
  is integer-only across a handful of hot code pages, runs at 100% native
  coverage with 2000-instruction chains, and so is *incapable* of showing
  pressure on the FP path, the block cache or calls. Conclusions drawn from
  it hold for it, and are not general.
- `nt_bench.sh` -- a real NT application benchmark, one section per JIT
  datapath, where the guest times itself and the harness reads the numbers
  off C: afterwards. Use this when the question is which datapath costs
  what, rather than whether one number moved.
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
