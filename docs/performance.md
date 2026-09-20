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

- **Real guest code** (the NT application benchmark through the desktop
  snapshot, JIT_STATS windows of 100M instructions, 2026-09-19 with
  everything below landed): about **3000 MIPS** overall on one emulated
  CPU -- 4040 median on the alu loop, 2800-3500 on most sections, 2200-2600
  on the slowest -- roughly 1.35 host cycles per Alpha instruction on an
  M3 Max. The stats build is a few percent under production.
- **Translated Alpha code in a tight loop** (`cpu_bench.sh`, production
  build, same day): 4200 MIPS on an arithmetic loop and 3540 on a load/store
  loop, per emulated CPU; earlier in the month 4300 and 4000, the memory
  figure within the tool's run-to-run spread and not A/B'd. For scale, the
  ES40's own EV68 at 667 MHz did roughly 1300-1500 MIPS in practice, and the
  fastest Alpha ever built, the EV7z at 1.30 GHz, about 10300.
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

## Where the host's time goes

A host profile (`sample`, 25 s, 1 ms) of the emulator while the guest ran
`nt_bench.sh`, counting only the busy thread -- the other seven are device
and GUI threads asleep in `semwait`/`cvwait`:

| where | share |
| --- | --- |
| emitted JIT code | **~92%** |
| `jit_run` -- the dispatcher, with the memory helpers inlined into it | ~5% |
| `FindTBEntry` -- the TB lookup behind a page-cache miss | ~2% |
| `execute` -- the interpreter | 0.4% |
| clock reads, interrupts, everything else | <1% |

No lock contention, no syscall cost, no helper hot spot: on this workload
`jit_read` and `jit_write` do not even appear as leaf frames. The guest CPU
thread is on a performance core -- forcing `QOS_CLASS_USER_INTERACTIVE` on
it (`ALPHABOX_CPU_QOS=1`, an experiment hook) changed nothing (-0.4%,
inside the noise). **The remaining cost is the emitted code itself, and
nothing around it.**

How much code that is, from `JIT_REGPROF` on the same workload:

- **Real MIPS on real code: ~1400** on the instrumented build, ~1530 on the
  production build (scaling by wall time), against ~4300 for the
  self-looping `cpu_bench.sh` loop. The host is an M3 Max whose performance
  cores run at 4.05 GHz, so that is **2.9 host cycles per guest instruction
  instrumented, ~2.65 in production**. With ~10-12 host instructions per
  guest instruction on the executed path the IPC is about 4 -- high, but
  below what the core can sustain, so part of each guest instruction is
  serial latency on the address chain rather than pure instruction count.
  Fewer instructions will do most of the work; shortening the chain does
  the rest.
- **Memory ops are 36.6% of hot instructions**, and 28.9% of those follow an
  access through the same base register within 8 KB -- probe-hoisting
  candidates, which is the only technique here that takes the probe off the
  address chain entirely instead of shortening it.
- **Five hot registers are not pinned.** Ranked by executions times
  accesses, R25, R15, R28, R13 and R14 are each in the top sixteen at
  0.8-1.2 billion, and none has a host register, so every reference is a
  load or a store. The pin set was chosen from a Windows 2000 *setup*
  profile; this benchmark's compiler uses different registers. A fixed
  global pin set is workload-dependent by construction, and the callee-saved
  registers are all spoken for -- freeing `x20` and `x28` (the register-file
  and epoch bases, both reachable from `x19` with a merged hot struct) buys
  two more, and per-block allocation buys the rest.
- The report's "93 bytes per instruction, execution-weighted" is **not** the
  executed path length: it weights each block's *whole* size, prologue and
  cold stubs included, by how often the block runs. The executed path is the
  29-33 instructions per specimen in `lab/jit-disasm-sample.md`.

So the answer to "why is it this slow" has one part, not several: about
ten host instructions run for every guest instruction, on a core that
executes them as fast as it can. Reaching an EV7z's 10300 MIPS would need
about seven times fewer -- roughly one and a half host instructions per
guest instruction, which is native-compiler territory: register allocation
across blocks, no per-access probe on the common path, and blocks long
enough that exits stop mattering. Peepholes and the exit and access work
above are worth having and are worth perhaps a third in total; the rest is
a different kind of code generator.

### Helper time, measured

`JIT_STATS` times every helper entry to return (callees included) with the
counter register and reports the share per window. On `nt_bench.sh`, the
whole benchmark: **8.8% of wall time inside helpers**, distributed as

| section | helper share | per call | what |
| --- | --- | --- | --- |
| alu, branch, call, div | ~0.1% | -- | never miss the page cache |
| sort | ~5% | ~10 ns | bursty read + write |
| byte | ~9% | ~10 ns | many cheap misses over a 1 MB buffer |
| ldst | ~17% | ~10 ns | many cheap misses: read 11%, write 5% |
| stride | **~63%** | **~78 ns** reads, plus 4.5M `HW_MTPR` + 4.4M `HW_MFPR` per 100M instructions | **PALcode's DTB-miss handler** -- 19% of the window in the IPR helpers alone |

Two different problems, then. `stride` pays for *slow* misses that go through
PALcode; the lever is a host-side shadow of translations consulted before a
miss is delivered, invisible to the guest while every invalidate is honoured.
`ldst` and `byte` pay for *many* cheap misses -- conflict misses in a 64-slot
direct-mapped cache -- and the fix there must tax misses only (a second way,
or a second-level lookup in the cold stub), not hits, which is exactly what
the hashed index got wrong.

Instrument note: the first measurement used `steady_clock` at ~15 ns a read
and reported 11.9%; three points of that were the clock. And the counter the
builtin reads runs at 1 GHz on this host though `hw.tbfrequency` says 24 MHz
-- the conversion is calibrated at first use rather than assumed, because
assuming it put every per-call figure out by 42x.

### The shadow, and what a miss really cost

The shadow landed (`be4de87`): stride -75.8% (2,308 -> 558 ms), the whole
benchmark -8.1%, results byte-identical. The prediction had been -40%,
priced from the helper share alone, and the miss was informative: of the
469 ms a stride window lost per 100M instructions, **helpers were 49%**,
the interpreter (the faulting load re-run, then the trap) 22%, dispatch
11%, and the compiled PAL handler itself ~19%. A path is priced from the
`time-split` line, not from one counter. What is left of stride is 98%
inside `jit_read` at 47 ns a call, 3.7M calls per 100M instructions, with
no TB miss and no interpreter: the 128-entry TB scan when the per-page
hint misses, which stride's eviction pattern guarantees.

That A/B also said ldst +6.7%, sort +5.7% and byte -9.6%, none of them
predicted. The per-window census (windowed page-cache miss causes, helper
calls and per-call cost, TB-miss bails) found the steady-state windows of
ldst identical with and without the shadow, so the change was A/B'd again
**inside one binary** -- `ALPHABOX_TB_SHADOW=0` on the base arm, the same
executable on both (`perf_ab.py --env-base`), three rounds. Result: stride
-76.7%, ldst -1.8%, sort -2.9%, byte -1.0%, everything else within +-2%
and overlapping; -9.5% in total. The three unexplained numbers were
**code layout**: two different builds of the CPU file place the
interpreter's and the helpers' hot loops differently, and that alone moves
a section by 5-10% either way. Rule from it: a section-level effect under
~10% between two *builds* is not attributed to the change until a
same-binary switch, or a layout-neutral comparison, has reproduced it.

### PALmode, measured

`JIT_REGPROF` tells PALmode blocks apart (`e6bbc0b`): on the benchmark,
**0.3-0.6% of hot instructions run in PALmode**, and PAL blocks cost 84-88
bytes per instruction against 101 for everything else. The shadow bank
(R23, R21, R4, R22, R7) is PAL's hottest register set and is never pinned,
so a PAL-specific pin set is a real idea -- bounded at about 0.1% of the
benchmark, against a spill/reload at every chained CALL_PAL/HW_RET edge.
Not worth it on this evidence.

### The icache-flush storm

The one kernel-heavy phase in a Windows 2000 boot -- 15 s at 186 MIPS
under the SRM console's PALcode -- is a flush storm: one `CALL_PAL IMB`
site (`0x1a10b4`, 99.3% of 3.4M flushes) every ~1,200 instructions. Every
flush dropped every uncompiled block, `record()` restarted its hotness
count, and nothing reached `compile_after`: 26M blocks recorded, 1.5k
compiled, interpreter 54% and dispatch 31% of the phase. `106b1c3` keeps
only the count across a flush (nothing derived from the old bytes
survives) and the phase went to interpreter 1.2%, 291 MIPS -- and took
the same 15 s, running 1.6x the instructions and 1.6x the IMBs. The
firmware is polling on real time around disk I/O (680k `HW_MFPR` and 620k
locked ops per 100M instructions), so that phase is a device-pacing
question, not a JIT one. Dispatch stays at 58% there because every flush
still bumps the epoch and kills every link; a no-op flush through
dirty-code-page tracking is the other half, if the phase ever matters.
The prediction was written down before the run
(`lab/results/flushfix-prediction.md`): three of four held, and the
fourth's miss is the finding.

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
| Four instructions off the inline memory access (`ubfx` index, `ldp` for tag+bias, the displacement folded into one `add`, loading into the destination's pin) | access sequence 16 -> 12 host instructions; `nt_bench.sh axp` **-4.6%**, 24335-24640 ms against 23328-23398, no overlap. `stride` flat, because it misses the page cache on every access and never runs the probe |
| An advisory page -> TB-entry index in front of `FindTBEntry`'s linear scan | `nt_bench.sh axp` **-4.6%** via `perf_ab.py` (22570-22695 ms -> 21562-21609, results identical): `ldst` -25.7%, `byte` -13.3%, `stride` only -3.0%. The prediction was the reverse. **Hypothesis, not yet tested:** `stride`'s consecutive pages share one granularity-hint entry so the last-match guess already hit there, and `ldst`/`byte` alternate between an array page and the stack, which defeats a single guess. The test is per-section helper-call and TB-miss counts from single-section runs (`nt_snap.sh run ... axp <section>`), and for `stride` a count of PAL-mode DTB-miss flows -- if those dominate, the lever is a larger host-side shadow of translations consulted before a miss is delivered to PALcode |
| A shadow of 4096 8 KB data translations kept past TB eviction, refilled from inside `FindTBEntry` instead of trapping to PALcode | `nt_bench.sh axp` **-8.1%** via `perf_ab.py` (21210-21523 ms -> 19601-19656, results identical): `stride` **-75.8%** (predicted -40), `byte` -9.6%. **Unexplained and recorded as such:** `ldst` +6.7% and `sort` +5.7%, no overlap, predicted flat. The mechanism for those two is not known; next is `tbmiss` and refill counts per window on single-section runs |
| Branching on a conditional terminator's own condition instead of building both successor PCs and comparing them | `nt_bench.sh axp` **-2.2%**, 23000-23015 ms against 22476-22515. `div` **-12.5%**, because the benchmark's compiler emits division as an inline restoring loop of conditional branches |

Together those compound to about **6.7%** on that benchmark. Quote the two
measured deltas rather than the difference between the first and last
absolute numbers: the same binary measured 23328-23398 ms in one sitting and
23000-23015 in the next, so there is ~1.5% of drift between sittings that
only a within-pair comparison controls for.


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
- `nt_snap.sh` -- the same benchmark without the boot. `make` boots the
  install once, stages the benchmark, and takes a whole-machine snapshot with
  every thread stopped (`SIGUSR1`; `ALPHABOX_SNAPSHOT_EXIT=1` exits right
  after, so the disk image is exactly what the snapshot saw). `run` resumes
  it (`ALPHABOX_RESTORE`) and types the run command into Start > Run. A cold
  boot was ~120 s of a ~165 s run; a resume is seconds. Nothing is written to
  C: behind the resumed OS -- that would corrupt its cached FAT -- so the
  benchmark files are staged before the snapshot and the run command carries
  its arguments. `perf_ab.py --snapshot` uses it. Until 2026-09-19 the S3
  saved 32 bytes of bookkeeping and no registers or VRAM, so a resumed
  desktop drew nothing; the VGA core, the VRAM and the S3's own state (the
  extended registers, the 8514 engine, the linear-window cache) and the SVGA
  mode decode (bank registers, the colour-depth selects -- derived state the
  write handlers keep outside the core; without it a resume drew a black
  1280x480) are now in the file, the derived timing, pitch and mapping are
  recomputed on restore, and the render thread re-pushes the text font and
  redraws. First resume after
  the change: the guest took the typed command and wrote DONE 17 s after
  launch.
- **`perf_ab.py` -- the only way a performance claim gets made.** Two
  binaries, N >= 2 interleaved rounds of `nt_bench.sh`, per-section medians
  with ranges and an overlap flag, a check that every section computed the
  same result under both arms, both binaries' hashes and HEAD recorded, and
  `--expect section:pct` predictions written down before the run and marked
  hit or miss after it. It refuses to start while a build or another guest
  is running. Everything it measures is appended to `lab/results/ledger.md`;
  quote that file, not memory. Each of those refusals corresponds to a
  mistake that was actually made on 2026-09-19.
- `s3_bench.sh` -- times a directory listing scrolling in a console window,
  and reports the drawing engine's own counters. It writes the emulator's
  pid to `emulator.pid` in its run directory: **profile that pid**, because
  other emulators may be running on the same host.
- `win_bench.sh` -- boots a guest and reports MIPS, which are meaningful
  only while the guest is busy with real work.

### The per-block items, measured one binary at a time

Every emitter change now carries a compile-time switch (`ALPHABOX_JIT_*=0`
emits the old shape), so each is A/B'd inside one binary, three rounds,
predictions on record:

| change | total | verdict |
| --- | --- | --- |
| the PC store off the hot path: `state.pc` written only on an exit's miss path or in a gate stub, never where a link hit tails into the next body | **-1.7%**, no overlap, every section -0.8..-2.8% | kept (`ALPHABOX_JIT_PCSTORE`) |
| `x27` as a down-counter: the gate's budget half one `tbnz` instead of a load and a compare | 0.0% (two runs, both inside the noise) | reverted; the flag-free exit that enabled it stays |
| exit records in an arena two `add`s from `x28` instead of a 3-4 instruction 64-bit constant | +0.4%, inside the noise | reverted |

Three instructions removed from a hot path bought 1.7%; a load, a compare
and a four-instruction constant removed from the same paths bought nothing
at all. On this core, instruction count off the address and branch chains
is not time, and only a measurement says which is which.

### Extended blocks

The last per-block item. A block used to end at every branch, taken or
not, and the cold pass that sizes a block already runs on through the
branches it does not take (`n_instr` ends at the one it takes). So the
translator now runs on through an integer conditional branch the cold pass
fell through, and the AArch64 emitter makes its taken side an exit in the
middle of the block -- count, gate if backward, a static exit with a link
slot of its own -- while the fall-through is simply the next instruction:
the pins stay live and the value-forward slot survives the branch. BR/BSR
always leave; FP branches still end a block; the x86-64 emitter is
untouched (the scan is arch-gated). `ALPHABOX_JIT_EBB=0` restores one
branch per block in the same binary.

Measured inside one binary through the snapshot (ledger: `ebb`, 3
rounds, results identical): **-2.0% in total, no overlap** -- alu -5.9%,
ldst -3.7%, byte -2.5%, branch -2.3%, the rest inside the noise. The
prediction was -4%: the exit sequence a not-taken branch used to pay is
cheaper than its instruction count, like everything else on this core.
JIT_VERIFY earned its keep on the first build: 41k mismatches, because
`emit_op` loaded the branch register into `x0` before deciding to defer
the branch and then recorded `x0` as still holding the previous op's
forwarded value -- harmless while a branch always ended the block, wrong
once instructions followed it. Block length, measured: compiled blocks average 9.0 instructions with extended blocks against 6.3 without (chains 257.2 vs 192.8 instructions between dispatches), JIT_STATS at the end of a resumed benchmark run.

### The TB index

What was left of stride after the shadow was 98% inside `jit_read` at 47 ns
a call: the 128-entry linear scan of the TB whenever the advisory hint
missed, which on code that walks memory is every page-cache miss. The scan
is now replaced by an exact index of the TB -- which slot holds each 8 KB
page, eight ways per set, 2048 sets, kept in step by every insert,
eviction and invalidation (`add_tb`, the shadow refill, `tbia`, `tbiap`,
`tbis`, `tbis_d`), rebuilt from `state.tb` on reset and restore, never
saved. A miss in the index means the page is not in the TB, so the scan is
skipped and the shadow refill or the DTB-miss trap follows at once; an
entry with a granularity hint spans pages and cannot be indexed, so while
any is live the scan is used. A hit is validated against the entry exactly
as the scan would validate it, so the index can never return a wrong
mapping; a false negative can only cost a refill. In a JIT_VERIFY build
the scan runs as the oracle after every index miss: 0 mismatches, and the
false negatives it counted were all in one set -- page 0, live under many
ASNs at once under SRM -- 317 of 6.4M probes with two ways, 270 with four,
3 of 6303123 with eight. `ALPHABOX_TB_INDEX=0` keeps the scan in the same binary.

Measured (ledger: `tb-index2`, same binary against the scan alone; and
`tb-index-vs-head`, against a clean HEAD build that still had the hint,
both through the snapshot, three rounds, results identical): against HEAD
**-4.5% in total, stride -70.8%** (562 to 164 ms), the other sections
-1% to -3% and inside the two-build band; against the bare scan -8.1%,
ldst -27%, byte -15%, which is what the hint had already been buying.

### The device traffic, by address

The page-cache census counts device accesses; `JIT_STATS` now also keeps a
per-address histogram of the ones the helpers serve (`dump_device_pages`,
printed when the CPU goes away). Over a Windows 2000 boot-plus-benchmark
run, by exact register: 58% the IDE data port 0x1F0 (81.6M reads, 10.8M
writes -- the guest moves its disk data by PIO: its stack is the generic
`pciide.sys` + `pciidex.sys` + `atapi.sys`, and the trace shows 5,061 READ
MULTIPLE, 662 WRITE MULTIPLE and not one bus-master start, though the
controller advertises DMA in every register a driver reads: prog-if 0xFA,
BAR4 relocatable, the bus-master status "DMA capable" bits, IDENTIFY words
49/53/63/88. Tried and reverted: the bus-master-enable bit set at power on
and held against the guest's write -- Windows read the bus-master status
and still chose PIO. Its registry says the same: `TimingModeAllowed` =
0xFFFFFFFF, nothing persisted forbids DMA, and `TimingMode` = 0x10, PIO 4
achieved at every boot. The decision is inside the Alpha build's generic
miniport; the emulator's remaining fidelity gap there, SET FEATURES 02/66
aborted where a real drive accepts them, is fixed); 14% port 0x61 (22M reads,
19M of them in one burst of 1.05 s: the system control port's refresh-toggle
bit, which the emulator flips every 15 us of wall time and a HAL stall loop
counts); 11%
the parallel port's status (18M reads, a driver's detection loop); 7% PIT
channel 2 (the other delay timer); 5.5% one Pchip CSR, TBA2, read 8.6M
times in the loader phase; 3% the legacy VGA window in text mode; the
S3's linear framebuffer does not appear at all, because the driver draws
through the accelerator. So the framebuffer-on-the-fast-path item, built
and verified (the S3 offers BAR0's VRAM to the page caches,
`CSystem::set_direct_memory`, switch `ALPHABOX_LFB_DIRECT`; a resumed
desktop draws through it), buys nothing on this guest and stays for the
guests that do write pixels. At ~15-20 ns an access the whole device
traffic is ~2-3 s of a 140 s boot, and every one of those phases is as
long as the guest's own delay loops make it: a faster port would not
shorten them, and a faster toggle would cheat every driver's delays. What
an emulator may do is pace them -- sleep the host thread to the next
toggle edge instead of spinning -- which is idle pacing's idea again.

The band is also a lever: if two builds of one file swing a section by
5-10%, some hot loop is alignment-sensitive. Aligning the hot helpers and
the dispatch loop to 64 bytes and fixing the symbol order with a linker
order file would shrink the band for every future comparison, and
whichever layout produced the faster ldst is a free, permanent gain once
chosen deliberately. Not done yet.

Three practical notes, all learned the hard way. Build-to-build code
layout moves a benchmark section by 5-10% on its own (see the shadow's
same-binary A/B above), so a change worth less than that is measured with
a runtime switch in one binary -- `perf_ab.py --env-base K=V` runs the base
arm with the switch off and the head arm with the same executable -- or
not claimed. A guest run that "did not
reach the desktop" is either a bugcheck or a timeout, and only the last
framebuffer tells you which -- convert it with `ppm2png.py` and look.
And when comparing two builds, interleave the runs: a difference of under
10% on a single pair of runs is noise on a shared host.
