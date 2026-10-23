# How faithful the processor is

Alphabox emulates an Alpha 21264 of the EV68CB kind; the EV7 (21364) rows
of the Marvel machines run the same core. This file records where it does
not match that processor, because a list of known divergences is worth
more than a belief that there are none. It says what a guest can and
cannot be used to test here, and it stops the same question being asked
twice.

Everything below was established against the *Alpha Architecture Reference
Manual*, 4th edition (ARM) and the 21264/EV68CB *Hardware Reference Manual*
(HRM), with section numbers. Where it could be, it was also demonstrated by
running Alpha code inside the emulator rather than by reading alone. The
first audit was made on 2026-09-18; later entries carry their dates.

| Section | What is in it |
| --- | --- |
| [How to check the processor yourself](#how-to-check-the-processor-yourself) | the verify lane, its blind spots, guest programs, trap tracing |
| [Known divergences](#known-divergences) | where the processor differs, ranked by what a guest would notice |
| [What was checked and found correct](#what-was-checked-and-found-correct) | so that it does not get re-litigated |
| [A guest's floating-point exception that is the guest's own](#a-guests-floating-point-exception-that-is-the-guests-own) | Direct3D 7 on Windows 2000 RC2 |
| [The rule this audit produced](#the-rule-this-audit-produced) | two references, not one |

## How to check the processor yourself

- **Interpreter against JIT**: build with `-DCMAKE_CXX_FLAGS="-DJIT_VERIFY"`
  and boot anything. Every compiled block is re-run by the interpreter and
  compared; a clean SRM boot is about 130 million blocks. This finds
  disagreements between the two engines, not disagreements with the
  architecture: both can be wrong together.
- **What counts as a mismatch**: any difference in a GPR, an FP register,
  an IPR, the PC, a store, an `STx_C`, or the number of stores. Since
  2026-10-03 also a load from another address or a different number of
  loads. (The compiled pass is handed the values the interpreter loaded, in
  order, so a compiled load from the wrong address used to come back with
  the right value and pass; it printed `LOAD ADDR MISMATCH` and counted
  nothing.)
- **What the verify lane cannot see**: it runs every block from the
  dispatcher, through the memory helpers, with `RPCC` through its helper.
  The production JIT also chains blocks into one another, probes the data
  page cache inline (and reuses a probe for the next access on the same
  page), and reads `RPCC` through a generated stub.
  `ALPHABOX_JIT_CHAIN=0`, `ALPHABOX_JIT_INLMEM=0` and `ALPHABOX_JIT_RPCC=0`
  take those away from a production build one at a time
  ([headless.md](headless.md)). A fault that goes with one of them is in
  it, and one that survives all three is not the JIT's compiled code.
  Also compiled out of a verify build, and so unseen by it: on AArch64 the
  static exits' data links, the inline computed-jump cache, the
  conditional exit at a block's end, the memory operations' cold stubs
  and the second-level page-cache probe, the inline `HW_LD`/`HW_ST` and
  the gates on exits; on x86-64 the chain gates. A block whose compiled
  pass bails before its end while the interpreter runs it through (a
  device access, say) is compared up to the bail (since 2026-10-22,
  JIT-06): the PC, the loads and stores before it, and every GPR the rest
  of the block does not write. A block whose interpreted pass takes a
  trap or an interrupt (a fault) is still not compared. The instruction
  oracle runs these paths on the production lanes (below, "The JIT").
- **The floating-point self-test**: `ALPHABOX_JIT_FPTEST=1` on a
  `JIT_VERIFY` build checks the JIT's inline IEEE operations against the
  interpreter over 8.5 million cases at startup.
- **The floating-point test**: `alphabox_fptest`, built with every lane,
  checks the interpreter's IEEE and VAX operate instructions against the
  architecture without a guest: IEEE results and flags against the host
  FPU in every rounding mode, VAX results against an exact model, and hand
  cases from the manuals ([development.md](development.md)).
- **The instruction oracle**: `test/cpu/cpu_oracle.py <binary>` runs about
  18,500 results from 16,000 cases without firmware, each from the
  interpreter and from compiled code, against a model written from the
  ARM and the HRM ([development.md](development.md)). A divergence listed
  here has a case that expects the manual's value and names the entry, so
  the case reports KNOWN until it is fixed and PASS after.
- **Guest programs**: `test/tools/bench_image.py` writes a bootable image
  from a list of instruction words, and `srm_probe.sh` boots one
  (`FLOPPY=<image> EXIT_ON_HALT=1 CMDS="boot dva0" AFTER=wait-exit`). A
  program ending in `CALL_PAL 0` halts the machine. Leave `EXIT_ON_HALT`
  off and the console comes back, so results stored in memory can be read
  with `examine -p -q <address>`. This is how most of the findings below
  were shown rather than argued.
- **Arithmetic traps**: `ALPHABOX_TRACE_ARITH=<n>` prints the first `n`
  IEEE arithmetic traps ([headless.md](headless.md)): the trigger's address
  and instruction word, both operands as they were, the FPCR and the
  summary. With the address of an exception a guest reports, that is the
  faulting instruction and what it was given, without a debugger in the
  guest.
- **A guest that traps**: the SRM bootstrap has no operating system behind
  it, so an enabled trap ends in "kernel stack not valid halt". That makes a
  fine yes/no trap detector, but anything about what a *handler* sees needs
  OpenVMS or Tru64.

## Known divergences

Ranked by what a guest would notice.

### Unaligned data accesses do not trap

`cpu_defs.hpp`'s `DATA_PHYS` performs an unaligned load or store at the
unaligned address instead of raising the alignment trap, unless the access
crosses the effective page boundary. This is deliberate and inherited from
es40: the host does the access natively and, both machines being
little-endian, the data is the same, which is why guests boot.

What is lost is everything built on the trap:

- OpenVMS PALcode fixes an unaligned access up, and the DAT bit decides
  whether the OS hears about it (ARM 14.3.3.1).
- Tru64 reports through `entUna` and offers `uac`.
- NetBSD counts them and panics for a kernel-mode one.

Under Alphabox all of those report nothing, for ever. So **Alphabox cannot
be used to find misaligned accesses in a guest**, and a clean bill of
health from it means nothing.

The page-crossing case that *is* implemented is worse than either
alternative. It fires on a condition the architecture does not define, and
whether it fires depends on what happens to be resident in the translation
buffer (an 8 KB fallback, or a granularity-hint page's mask of up to 4 MB).
So a loop over a misaligned buffer can run for a thousand iterations and
trap on the one that straddles a page. The byte-at-a-time page-crossing
code in `READ_VIRT`/`WRITE_VIRT` and friends is unreachable: `pbc` is
declared false and never set.

Deciding this properly means choosing one of:

- trap always (faithful, and a guest's fixup handler finally gets
  exercised);
- keep the shortcut and drop the page-crossing special case (honest, fast,
  and consistently wrong in one direction).

Two parts of this *are* fixed. A misaligned `LDx_L`/`STx_C` now takes the
trap the architecture requires; it used to hand a misaligned address to a
host atomic compare-and-swap, which is a bus error that killed the
emulator. And the alignment trap no longer claims an access violation in
`MM_STAT`.

### Machine checks are never raised

`MCHK` is defined and never used. An unclaimed physical address returns data
from `CSystem::ReadMem` instead of a machine check, so firmware or an OS
probing a bus for a device that is not there sees a plain read, and a
guest's machine-check handling cannot be exercised in Alphabox at all.

### A busy-wait is handed the time it is waiting for

Windows' `KeStallExecutionProcessor` delays a driver by spinning on `RPCC`
until the requested cycles have passed. On hardware the processor really is
busy for that long. Here the processor is asking *this emulator's* counter
to advance, and spinning through it in real time was 62 of the 95 seconds
of a Windows 2000 boot: 70% of every instruction executed.

So the dispatcher recognises that loop by its instruction words, which pin
its registers: the count asked for, and the counter when the wait began.
When nothing else is pending, it adds the remaining cycles to `state.cc` so
the loop falls out on its next turn.

- A wait longer than a tenth of a second is left alone: that is not a
  delay, it is a guest waiting for something to happen.
- The block is kept out of the JIT, because compiled it chains to itself
  and spins a whole dispatch batch before the dispatcher is asked.

**The divergence:** the guest's cycle counter runs ahead of real time by
whatever it would have spent waiting. During driver initialisation that is
up to about six times real time (`ALPHABOX_RATE` reports it).

- A guest that compares `RPCC` against the interval timer therefore
  measures a processor faster than the configured `speed`.
- Time of day is unaffected: that comes from the interval timer and the
  TOY clock, not from `RPCC`.
- On a multiprocessor guest each processor's counter jumps independently.
  That is architecturally allowed (`PCC` has a per-processor offset on real
  hardware, and software may not compare it across processors), but it
  makes the skew much larger than hardware would.

`ALPHABOX_STALL_SKIP=0` restores the real-time wait. Measured, same binary:
95 s to the desktop with it off, 60 s with it on, and a third fewer
instructions executed ([performance.md](performance.md)).

### Windows 2000 RC2 setup loses a race of its own

**What happens.** GUI setup of Windows 2000 Professional RC2 (build 2128)
can stop at *Installing Components* with

    Exception c0000005 at address 65ABB3F0 with the following parameters: 00000000 65ABB3F0

in `setuperr.log`, restart its GUI phase through AlphaBIOS, and stop there
again.

**It is the guest's own race**, not a translation error. COM+ setup
registers `msdtctm.dll`:

1. `LoadLibrary` runs its `DllMain`, which starts two worker threads per
   processor at 65ABB3F0;
2. then `DllRegisterServer`;
3. then `FreeLibrary`, which does not wait for the workers.

A worker that has not reached its start routine when the image is unmapped
takes an instruction access violation on its first instruction. Traced
from one post-text-mode checkpoint, counting instructions from the
`DllMain` attach:

| | workers enter 65ABB3F0 | `DllMain` detach | interval ticks, attach to detach |
| --- | --- | --- | --- |
| JIT | +10.95 M, +130.7 M | +10.63 M | 3 |
| interpreter | +8.76 M, +8.78 M | +10.70 M | 15 |

Who wins depends on how many interval ticks (7.8 ms: Windows 2000 programs
the clock at 128 Hz here) the setup thread's ~10.6 million instructions
span:

- `timer.max_instr_per_tick` moves the outcome within one binary: at
  1250000 setup crashed 3 of 3, at 500000 it got past the point 3 of 3
  (250000: 1 of 1).
- The interpreter usually wins, not always: four runs passed, one lost the
  race on both of its passes.
- The `JIT_VERIFY` build, slower, passed with 0 mismatches over 23 billion
  compiled-block executions.
- The fix for the icache probe (875b499) does not change it: the control
  build without it and current main crash alike, and
  `ALPHABOX_TRACE_ICPROBE=1` reports no `ICPROBE-RECORD` or `ICPROBE-CHAIN`
  in the crashing run.

**Why it installed on hardware.** An 800 MHz EV68 has 6.2 million cycles
per 7.8 ms tick. Unless it averaged under about 0.1 instructions a cycle
here, it would sit nearer the JIT (3.5 million instructions a tick) than
the interpreter (0.7 million), so processor speed alone does not explain
why the release installed on hardware. **Disk latency does.**

`DllMain` creates the workers and then touches a page of `msdtctm.dll`
that is not yet in memory. The setup thread takes a page fault, and the
paging read (an IDE `READ MULTIPLE` of 5 sectors) is the first thing it
waits for. On a real disk that wait is milliseconds, the thread blocks,
and the ready workers run. Here the image file answers in about 0.1 ms,
and the workers do not get the processor until the setup thread is past
`FreeLibrary`. With `ALPHABOX_IDETRACE=1` and a diagnostic address trace
(not in the tree), same checkpoint, JIT:

| | that read takes | workers enter 65ABB3F0 | `DllMain` detach |
| --- | --- | --- | --- |
| no latency model | 0.1 ms | +12.24 M, +124.3 M (after: crash) | +10.98 M |
| 8 ms access time | 10 ms | +0.28 M, +0.29 M | +10.81 M |

**The latency model.** The disks can be given mechanical latency
(es40.cfg, the `latency.*` keys of a disk; `ALPHABOX_DISK_LATENCY_US` for
every IDE hard disk, see [headless.md](headless.md)), off by default. GUI
setup from the checkpoint, JIT at the default `timer.max_instr_per_tick`:

- the outcome is read from `setuperr.log` and the GUI-setup restarts in
  `setupact.log`;
- "finished" means setup completed, rebooted and showed the Network
  Identification Wizard;
- the last column is launch to the evaluation-copy notice at the end of
  GUI setup. One "off" and one 8 ms run were the diagnostic build that
  produced the trace above, not timed.

| access time (+0.3 ms per command, 20 MB/s) | runs | crash at 65ABB3F0 | finished | clone to end of setup |
| --- | --- | --- | --- | --- |
| off | 4 | 4 (each restarted GUI setup, 1-5 times before it was stopped) | 0 | |
| 0.5 ms | 2 | 0 | 2 | 556 s, 586 s |
| 2 ms | 2 | 0 | 2 | 658 s, 673 s |
| 8 ms | 3 | 0 | 2 (the third was stopped once past COM+) | 971 s, 974 s |
| 15 ms | 2 | 0 | 2 | 1325 s, 1364 s |

**What it costs.** The time from launch to the desktop of the installed
RC2 guest (2 CPUs, JIT, frame-dump resolution about 2 s, three interleaved
boots each) went from 82-85 s with the model off to 95-98 s at 0.5 ms and
128-130 s at 8 ms.

It stays an option, off by default:

- It is the better way to install RC2 with the JIT:
  `ALPHABOX_DISK_LATENCY_US=500` (or, on the system disk,
  `latency.access_us = 500; latency.command_us = 300; latency.mb_per_s = 20;`,
  the same model) for GUI setup costs about 15% more wall time at boot.
- `timer.max_instr_per_tick = 500000;` also gets past this point, but holds
  the processor to about 64 million instructions a second throughout.
- A realistic default would make every boot of every guest a sixth to a
  half slower, for a race that only this release is known to lose.
- The model covers IDE hard disks only (the ALi's and the CMD 649's, which
  share the IDE core). The SCSI adapters, virtio-blk, USB storage and
  ATAPI CD-ROMs still complete at host speed.

### Data translations outlive their TB entry

A real EV6 has 128 data-TB entries. Once one is evicted, the next access to
its page takes a DTB miss, and PALcode reads the PTE again. Alphabox keeps
the translation after eviction, in two places:

- the translation shadow (`m_tb_shadow`, 4096 entries), which refills the
  TB without the trap;
- the data page cache (`data_page_cache` and its second level), which a TB
  fill no longer empties for the page it evicts.

This is legal. The architecture lets a TB hold a translation for as long as
it likes, so software must invalidate after changing a PTE (`TBIS`, `TBIA`,
`TBIAP`, and the ASN rules), and each of those still empties the shadow and
the page cache.

**The divergence:** a guest that changes a valid PTE without invalidating,
and happens to get the new one on real hardware because the old entry was
evicted in the meantime, keeps the old translation here for much longer. A
missing `TBIS` is a guest bug that hardware usually hides; Alphabox hides
it less often. `ALPHABOX_DPC_KEEP=0` makes the page cache drop an evicted
entry's page again. The shadow has no switch.

#### Closed 2026-10-03: the page cache outlived the shadow too

The page cache is only an invisible longer-lived TB while every page it
maps is one the DTB still translates without PALcode: in the TB, or in the
shadow, from which `FindTBEntry` refills. It was not. The shadow is
direct-mapped and a later fill overwrites its slot, so a page could leave
the TB and the shadow and stay in the page cache.

Compiled code's loads and stores probe the page cache and kept hitting such
a page; the interpreter and the helpers ask the TB and took a DTB miss on
it. That was more than two hundred misses in every DS20E boot of OpenVMS
8.4 (the diagnostic stopped counting there), and none with the fix. Almost
all are harmless: PALcode refills the TB.

One is fatal. The VMS PALcode (on the DS20E, the DS25) builds an exception
frame like this:

1. it stores two quadwords to the kernel stack with the PALshadow
   registers on;
2. it clears `I_CTL[SDE]`;
3. it stores the interrupted code's R4-R7 to the same 64 bytes.

That last store cannot miss on real hardware, since the first two just
used the page. Here the first two ran compiled, the next ones interpreted,
and the miss came with `SDE` clear. The miss handler ran on the interrupted
code's registers, its double-miss path read a garbage impure pointer, found
`VA_CTL` "corrupt" and entered the console: "halt code = 5, PC = 0" on one
processor, `CPUSPINWAIT` on the other, in about one boot in twenty-five on
two processors (lab/smp-flaky).

Now a TB fill that evicts an entry, or pushes one out of the shadow, also
drops that page from the page cache unless the TB or the shadow still has
it (`dtb_still_maps`).

Two more holes of the same shape were closed with it:

- **The TB index** (`m_tb_idx`, eight ways a set) drops the oldest way of a
  full set. Its own comment said a false negative "costs a refill from the
  shadow at worst", but the shadow may not have the page, and then a page
  the TB held took a DTB miss. A set that overflowed is now marked, and a
  miss there scans the TB.
- **`flush_data_page_cache_range`** compared a TB entry's address, which
  keeps VA<47:13> only, with the page cache's sign-extended one. For a
  system-space page (`ffffffff8...`) it never matched, so the
  `ALPHABOX_DPC_KEEP=0` eviction path and the per-entry pass of `tbis_d`
  did nothing there. It compares VA<47:13> now.

### Closed 2026-10-20: `FNOP` took a floating-point disabled fault

Every floating-point operate instruction checked the FPU enable first, so
`FNOP` (`CPYS F31,F31,F31`, `5FFF041F`) took a FEN fault while the FPU was
disabled. The *Alpha Architecture Handbook* (1992), appendix A, "Stylized
Code Forms", says of the standard `NOP` and `FNOP`: "These generate no
exceptions". The 21264/21364 Compiler Writer's Guide, 3.2, says the
processor "discards NOP instructions early in the pipeline".

GNU as pads code alignment with `nop; fnop; unop`. Linux's `memmove` runs
such padding from the first `printk`, before `trap_init` has set any trap
vector, with the FPU disabled. The fault went to a vector of 0, the fetch
from 0 faulted again, and the frames pushed for each fault walked down
over the kernel and the PALcode. Linux never printed a line
([linux.md](linux.md)).

Now the exact `FNOP` encoding does nothing in the interpreter and is a NOP
to the JIT. Any other floating-point instruction, `F31` operands or not,
still faults with the FPU disabled: the architecture says any of them may.

### Closed 2026-10-22: what the instruction oracle found

The CPU audit of 2026-10-22 (the legacy audit's CPU lane; its findings
are named CPU-nn in the oracle's KNOWN cases) found these with the
instruction oracle. Each is fixed in both engines, and the oracle passes
its cases on the interpreter, AArch64 JIT, `JIT_VERIFY` and x86-64 JIT
lanes.

**Loads to R31** (CPU-03, CPU-05). `LDQ_U` to R31 (`UNOP`) was performed:
translated, able to fault, and read. So were the other integer loads to
R31, a device read included, because the `if (Ra != 31)` in the load
macros guarded only the first statement of `READ_VIRT`. Compiled code
skipped `LDQ`, `LDBU` and `LDWU` to R31 altogether and took no DTB miss,
so the two engines disagreed where `JIT_VERIFY` could not see it (its
compiled pass goes through the helper, which faulted like the
interpreter). One rule now holds in the interpreter and in both
emitters:

- `LDQ_U` to R31 is retired without execution (HRM 2.5, Table 2-6): no
  translation, no fault.
- `LDL`, `LDQ`, `LDBU` and `LDWU` to R31 are prefetches (HRM 2.6): they
  are translated and take the DTB miss, access violation or
  fault-on-read the load would take ("Load operations to R31 and F31 may
  generate exceptions. These exceptions must be dismissed by PALcode"),
  and nothing is read. A prefetch to I/O space is converted to one to
  memory space (HRM 4.2, p. 4-5), so no device sees it.
- Compiled code answers a prefetch from its page cache and otherwise asks
  `jit_read` to translate only.

Floating-point loads to F31 are still not translated: they take no DTB
miss, which the "may" of HRM 2.6 allows, and the two engines agree. With
the fix the SRM console log is unchanged on the interpreter, JIT and
`JIT_VERIFY` lanes (106 million compiled blocks, 0 mismatches); Windows
2000 RC2 on two processors reached its desktop on the JIT and on
`JIT_VERIFY` (768 million compiled blocks, 0 mismatches); Gentoo Linux
logged in, compiled and ran a C program (`guests_check.sh linux`).

**An operate into R31 that overflows** (CPU-04). `ADDL/V`, `ADDQ/V`,
`SUBL/V`, `SUBQ/V`, `MULL/V` and `MULQ/V` with R31 as the destination
took the arithmetic trap. The 21264 retires an integer operate into R31
at decode without executing it (HRM 2.5, Table 2-6), so nothing overflows;
the ARM leaves it UNPREDICTABLE (14.3.2). They no longer trap. Compiled
code does not compile the `/V` forms, so the interpreter's rule is the
only one. SRM's console log is unchanged on the interpreter, JIT and
`JIT_VERIFY` lanes, and Windows 2000 RC2 on two processors reached its
desktop.

**`CALL_PAL` wrote `EXC_ADDR`** (CPU-02), with its own PC, in both
engines. On the 21264 `CALL_PAL` is not an exception: HRM 6.8 gives
PALcode two classes of entry point, `CALL_PAL` and exceptions, and 5.2.7
says `EXC_ADDR` is updated "when it encounters an exception or
interrupt". The return address goes to R23 alone (6.8.1). `EXC_ADDR` now
keeps the last exception's PC through a `CALL_PAL`. The PALcodes were
written for the hardware, so none can depend on the old value; the
`ALPHABOX_TRACE_ICFLUSH` report, which did, now names its callers by
R23. OpenVMS 8.4 logged in on the ES40 (JIT and interpreter lanes, the
second with the native `vmspal` routines) and on the ES47, and Windows
2000 RC2 reached its desktop on one processor and on two.

**A load or store between `LDx_L` and `STx_C`** (CPU-01). The `STx_C`
succeeded. On the 21264 "a store-conditional is forced to fail if there
is an intervening memory operation between the store-conditional and its
address-matching LDxL" (HRM D.19, Restriction 23, p. D-13). The list there
is `LDL/Q/F/G/S/T`, `STL/Q/F/G/S/T`, `LDQ_U` (not to R31) and `STQ_U`, and
`HW_LD`, `HW_ST`, `ECB` and `WH64` are named as not doing it; the ARM
leaves the case UNPREDICTABLE (4.2.4). Now those operations clear a flag
of the processor's that `LDx_L` and `HW_LD/L` set, and a `STx_C` or
`HW_ST/C` to memory fails when it is clear. One to I/O space still always
succeeds (HRM Table 4-1). Compiled code clears the flag with one store, at
the first listed operation of a block and the first after an `LDx_L` in
it: a block is entered at its top and runs straight on, so any later one
would only store the same zero. The flag is not in the state file, so a
`STx_C` right after a restore fails, which the architecture allows.

The byte and word forms (`LDBU`, `LDWU`, `STB`, `STW`) are not in the
HRM's list and do not fail the `STx_C` here; the HRM does not say what
the 21264 does with them. `ALPHABOX_LLSC_MEMOP=0` turns the rule off in
both engines. Correct code never does this ("no useful program should do
this", ARM 4.2.4), and none of the guests did: OpenVMS 8.4 on the DS20E
with two processors (BACKUP and DIFFERENCES of `SYS$BASE_IMAGE.EXE`, no
differences) and on the ES47 with two, Windows 2000 RC2 on two
processors, and SRM on four (interpreter, JIT and `JIT_VERIFY`: 87
million compiled blocks, 0 mismatches) all ran as before. What it costs
compiled code is in [performance.md](performance.md) ("The LDx_L
window"): nothing that a busy host's noise did not cover.

**The reserved barriers `18.4800` and `18.4C00`** (CPU-06). ARM C.13
reserves them for weaker memory barriers, and until those are defined
they "must operate as a Memory Barrier instruction (MB 18.4000)".
`18.4800` flushed the instruction cache and the ITB and fenced nothing,
and `18.4C00` took OPCDEC. Both are `MB` now, in both engines. `DO_IMB`,
which only `18.4800` ran, is gone, and with it the line here that said
`IMB` invalidates the ITB: `CALL_PAL IMB` is the PALcode's, which flushes
the instruction cache with `HW_MTPR IC_FLUSH` (the interpreter's native
OpenVMS routine does the same) and leaves the ITB alone.

**The `CALL_PAL` linkage and `I_CTL`** (CPU-08, CPU-10). With
`I_CTL[SDE<1>]` clear the return address of a `CALL_PAL` went to the
native R23 and clobbered it; the hardware writes the PALshadow R23
whatever `SDE` says ("PALshadow register R23 is used as the linkage
register", HRM 6.8.1), and so do both engines now. `I_CTL<20>` was
forced to 1 on every write; on the EV68CB it is `ST_WAIT_64K`, read-write
and cleared by reset (HRM Table 5-11; on the EV6 that bit is
`CALL_PAL_R23`), and it keeps what PALcode writes. The value `I_CTL`
starts with, the SROM's 0x502086, read back `SDE<1>` set while the
PALshadow registers stayed off until the console's first `HW_MTPR
I_CTL`; it starts at 0x502006 now, so a read agrees with what the
processor does. PALcode runs with `SDE` set and saves and restores
`I_CTL` whole, so nothing a guest runs saw either.

**The lock flag on access faults, and an interrupt still pending**
(CPU-09, CPU-15). The access violations and faults on read or write that
the translation raises itself (`DFAULT`, `IACV`) set the PC without
going through `GO_PAL`, so they left the lock flag set, where every other
exception but a TB miss clears it. They clear it now; the ARM leaves the
case UNPREDICTABLE (4.2.4), so this is the code's own rule made whole.
Delivering an interrupt clears the doorbell (`check_int`) that makes the
dispatcher look for one, so one still pending when PALcode went back to
native mode without writing an interrupt register waited for the next
line change or IPR write. The 21264 signals it again "when the PALcode
returns to native mode" (HRM 5.2.11): now the first `HW_RET` to native
mode after a delivery raises `check_int`, in the interpreter and in
compiled code. PALcode that lowers the IPL rewrites `IER`, which raised
it anyway, so this was latency at most.

With all of these, `guests_check.sh` passed Windows 2000 (desktop, clock
within a minute, shutdown), OpenVMS on the ES40 and the ES47, and Linux,
and Windows 2000 on two processors reached its desktop on `JIT_VERIFY`
(749 million compiled blocks, 0 mismatches).

### Smaller ones

| What | Where | Consequence |
| --- | --- | --- |
| `VA_FORM` is derived from the VPTE address on a double TB miss, where the HRM says the faulting address | `AlphaCPU.cpp`, `virt2phys` VPTE branch | Latent: shipping PALcode uses VA+PTBR, so nothing observed. New firmware reading `VA_FORM` there would compute the wrong page-table address |
| A `STx_C` after the native `REI` can still succeed (the ARM clears lock_flag on REI, 4.2.4) | `AlphaCPU_vmspal.cpp` | Every real preemption reaches REI through an exception, which does clear it, so only a deliberate `LDx_L; REI; STx_C` sees it. The console's PALcode REI loads its frame with `LDQ`, which fails a later `STx_C` since CPU-01 |
| `MT_FPCR` does not take its synchronous trap (HRM 6.7.3) | `cpu_fp_operate.hpp` | PALcode bookkeeping hung on that trap never runs. Nothing we boot needs it |
| `EXC_SUM[63:48]` is not the sign extension of `SET_IOV` | `AlphaCPU_ieeefloat.cpp` | PALcode testing EXC_SUM's sign as a fast "did IOV happen" check. We also set FPCR ourselves, so nothing reads it |
| Performance-counter, corrected-read and serial-line interrupts are never raised | `ISUM`, `int_deliverable` | Nothing on this machine uses them |
| The instruction cache is 2 MB with 2 KB lines; a real EV6 has 64 KB with 64-byte lines | `AlphaCPU.hpp` | Legal, but a guest that gets away with a missing `IMB` on real hardware can fail here (below) |
| `AMASK` is advertisement, not a gate: the CIX/MVI/BWX/FIX instructions execute whatever a CPU row claims | `CpuModels.cpp`, `cpu_misc.hpp` | Correct for the EV68CB, which implements all of them. The EV7 rows claim the same set as an inference: neither EV7 PALcode nor the ES47 console executes `AMASK` or `IMPLVER` ([marvel.md](platforms/marvel.md), E1). It matters the day a row is added for a part that does not |
| Some written IPR fields have nothing behind them: `M_CTL<5:4>`, `DC_CTL` and `PCTR_CTL` (its counters never count) are kept; `C_SHFT`, index 0x2d (`M_FIX`), `HW_INT_CLR<28>`, `VA_CTL<0>` (byte order) and `VA_CTL<3>` are dropped; `C_DATA` and `I_CTL<23>`/`<14>` read 0 | `cpu_pal.hpp` | None seen. The ES40's and both EV7 PALcodes configure caches and error reporting through them, mask `I_CTL<23>`/`<14>` off before saving it, and read `C_DATA` only on their error path, where 0 is "no error" (E1) |

**The instruction cache** is architecturally legal: a virtual I-cache need
not be coherent, and `IMB` works. But stale bytes live far longer here.
Its lines carry the flush generation they were filled in, so an `IMB`
invalidates every one of them with a single store instead of a walk. It
must invalidate them, whatever else a flush decides to skip: the lines are
tagged by virtual address, so code mapped at an address that has held
other code is a change no physically-keyed bookkeeping can see.

**`AMASK`**: no document lists the 21364's AMASK, and neither EV7 PALcode
nor the ES47 console executes `AMASK` or `IMPLVER` (E1), so the EV7 rows'
value is an inference (`CpuModels.cpp` marks it as one).

## What was checked and found correct

Recorded so that it does not get re-litigated. All of this was verified
against the manuals, and most of it by running code.

### Integer, branch and PALcode

- **Integer arithmetic, logic and bit manipulation** (opcodes 0x10–0x13,
  0x1C): every function code, against handbook pseudocode, over ~3.5
  million operand combinations. That includes all six `/V` overflow forms,
  the byte extract/insert/mask family's mod-64 shift edge, the literal
  form, and destination aliasing. Zero disagreements, and `JIT_VERIFY` runs
  of 144 million blocks on AArch64 and 117.5 million on x86-64 found none
  either.
- **Branch and jump**: displacements at both extremes (+1048575 and
  −1048576, checked by landing them), `JMP` with `Ra == Rb`, hint bits
  correctly ignored, FP branches treating `0x8000000000000000` as zero.
- **PALcode and privileged instructions**: entry offsets match HRM Table
  6-8 (including `DTBM_DOUBLE_3`/`_4` selected by `I_CTL[VA_48]`), the
  `CALL_PAL` entry PC assembly, the R23 linkage including its PALmode bit,
  PALRES gating, `HW_LD`/`HW_ST` type decoding, `HW_RET`, and PALshadow
  being R4–R7 and R20–R23 under SDE.
- **The EV7 PALcodes' internal registers** (E1, 2026-10-19): every
  `HW_MFPR` and `HW_MTPR` in the ES47 console's OpenVMS PALcode V2.11-25
  and UNIX PALcode V2.08-19, found from all entry points by
  `test/tools/ev7_pal_iprs.py`. They use 45 and 43 registers, all of them
  21264 ones (the set the ES40's PALcodes use), each implemented with the
  fields the code tests or sets, and every `HW_LD`/`HW_ST` type they use is
  decoded. OpenVMS 8.4 to DCL on the ES47, and the UNIX PALcode under
  `test/tools/unix_pal_probe.py`, ran with no unknown register. The table
  is in [marvel.md](platforms/marvel.md).
- **Floating point**: which rounding mode applies (the instruction's field,
  with FPCR consulted only for `/D`), the IEEE and VAX trap-mode tables
  being mirror images (a classic emulator bug, and we decode both), sticky
  bits set independently of trap enables, NaN propagation and quieting,
  `CMPTxx` signalling rules, `DNZ`, and the S-format load/store mapping.

### Floating point (2026-10-20)

`alphabox_fptest` puts every IEEE and VAX operate instruction through the
interpreter's routines and compares result, FPCR and `EXC_SUM` with the
manuals: 21 million cases, including every rounding mode, trap qualifier
and FPCR disable. These were settled in that round.

- **A conversion to integer that overflows** (`CVTTQ`) is an integer
  overflow: the low 64 bits are the result, and only `/V` traps (ARM
  4.7.7.2, 4.7.7.9). With `/S` it is also an invalid operation, which
  `INVD` can disable: HRM Table A-11, which lists what the processor does
  for "IEEE instructions with /S", gives "Integer overflow: Truncated
  result, Invalid Op". Without `/S` it is not one: a plain conversion that
  overflows "uses the low-order bits of the integer as the result without
  trapping" (ARM 4.7.7.2), and reporting it to the user as invalid is the
  completion handler's job (ARM B.2.1). Guests agree. GCC compiles
  `(long)x` to `CVTTQ/C` without `-mieee` and to `CVTTQ/SVC` with it;
  Linux sends `SIGFPE` for any arithmetic trap without the completion bit
  (`do_entArith`), so an invalid operation on `CVTTQ/C` would kill every C
  program that converts an out-of-range double; and the completion
  handlers of Linux (`arch/alpha/math-emu`) and NetBSD (`fp_complete.c`)
  raise the invalid operation themselves. Until 2026-10-20 every form
  raised it here.
- **`FPCR<DNZ>`** makes a denormal operand a signed zero only for an
  instruction with `/S`: "these denormal control bits only affect denormal
  handling by IEEE instructions that are modified by any valid qualifier
  combination that includes the /S (exception completion) qualifier" (ARM
  4.7.7.11), and the HRM, A.8: "when FPCR[DNZ] is set, denormal input
  operand traps can be avoided for arithmetic operations that include the
  /S qualifier". Without `/S` a denormal operand takes the unmaskable trap
  whatever `DNZ` says (until 2026-10-20 it was a zero here). `CVTST`,
  which converts an S denormal exactly and without a trap, does so without
  `/S` whatever `DNZ` says.
- **Where an IEEE underflow begins**: the true result is rounded to the
  destination format, denormals' grid included, before the range check.
  ARM 4.7.5 maps to an underflow only "non-zero true results < MIN - 1/2
  LSB" under normal rounding, those "<= +MIN - 1 LSB" under plus infinity,
  and so on, so a true result that rounds to MIN is MIN, inexact. Until
  2026-10-20 the interpreter rounded at full precision with an unbounded
  exponent and flushed some of these to zero (under normal rounding, those
  from MIN - 1/2 LSB up to MIN - 1/4 LSB), while the JIT, which computes on the host's IEEE format,
  returned MIN: `ALPHABOX_JIT_FPTEST` with those operands added showed 185
  disagreements, and shows none now.

- **An underflow with `UNFD` set and `UNDZ` clear traps** (closed
  2026-10-20). A 21264 cannot produce a denormal result. With `/U` and
  `/S`, FPCR `UNFD` set and `UNDZ` clear the instruction traps "to supply
  a possible denormal result" (HRM Table 2-14, p. 2-36; the EV6, EV67 and
  EV68CB/DC manuals say the same), and the operating system's completion
  handler computes the denormal. Only `UNFD` with `UNDZ` suppresses the
  trap and writes a true zero. The ARM would allow an implementation to
  compute the denormal itself: with a trap disable bit set, "the hardware
  implementation sets the result of the operation to the nontrapping
  result value as specified in the IEEE standard ... If the
  implementation is unable to calculate the required result, it ignores
  the trap disable bit and signals a trap as usual" (4.7.7.10, p. 4-79),
  and `UNDZ` clear makes that result "the non-trapping result (denorm, +0
  or -0) specified in the IEEE standard" (Table 4-11, p. 4-81). The 21264
  does not compute it, and neither does Alphabox. Before 2026-10-20 the
  interpreter flushed to zero without trapping (`UNDZ` was never read);
  for a few hours it computed the denormal itself (commit `9ad20bc`),
  which no 21264 does and which hid the completion handler's work.
  What the trap leaves behind is what the existing underflow trap leaves:
  `EXC_SUM<UNF>` and `<SWC>` set (HRM Table 5-9, p. 5-14), `<SET_UNF>` set
  if `FPCR<UNF>` was clear, `<REG>` the destination register, and
  `<SET_INE>` and `<INE>` for an `/SUI` instruction that is not
  `INED`-disabled; `FPCR<UNF>` and `<INE>` are set, as the architecture
  defines underflow, which "does not depend on whether the result is
  inexact" (ARM 4.7.7.7). The destination register holds a true zero:
  Table A-11 (HRM p. A-16) gives "Exponent underflow: +0", and A.8 says a
  true zero is written. The manuals do not say what the register holds on
  the trap "to supply a denormal", only that software supplies the
  result, so the zero is an inference, the same one the plain `/U` trap
  already made. A result that rounds up to MIN (FP-11) is not an
  underflow and does not trap. The JIT completes no underflow: a denormal
  result or a zero from nonzero operands sends the instruction to the
  interpreter, so it never sees this trap.

**On a guest.** The Gentoo install on the ES40 ([linux.md](linux.md))
compiled one C program twice with gcc 15, with and without `-mieee`: it
divides `DBL_MIN` by 3, halves it, divides `FLT_MIN` by 3, and converts
1e30 to `long`. gcc emitted `divt/su`, `mult/su`, `divs/su` and
`cvttq/svc` for the first, `divt`, `mult`, `divs` and `cvttq/c` for the
second. Linux runs both with `UNFD` set and `UNDZ` clear.

| | before 2026-10-20 | now |
| --- | --- | --- |
| `-mieee` | `0x0p+0` three times | `0x0.5555555555555p-1022`, `0x0.8p-1022`, `0x1.555558p-128` (the IEEE results, computed by the kernel's completion handler after the underflow trap) |
| without | zeros; the conversion kills the program with `SIGFPE` | zeros (true zeros: no `/U`); the conversion gives 5076964154930102272, the low 64 bits of the double 1e30 |

### The exception path

`EXC_ADDR` is the faulting instruction for a *fault*, which is what a fault
needs: PALcode restarts it. `EXC_SUM` and `MM_STAT` bit layouts, AST
delivery only below IPL 2, interrupts blocked in PALmode, `lock_flag`
cleared on every exception except the transparent TB fills, and OPCDEC
coverage are all as the manuals say.

The same PC is used for an *arithmetic trap*, and that is worth the
paragraphs it took to settle. The architecture reads the other way: "the
trap PC is an arbitrary number of instructions past the one triggering the
trap" (ARM 4.7.6.1), and the trap-shadow rules exist so a handler can "find
the trigger instruction via a linear scan backwards from the trap PC" (ARM
4.7.6).

An audit flagged this file for saying otherwise, and the reading is
correct. So it was changed to save the instruction after the trigger, and
**Windows 2000 stopped booting**: `STOP 0x00000012 TRAP_CAUSE_UNKNOWN`, on
one processor and on two, reproducibly, where the unchanged build reaches
the desktop.

What that says is that the PALcode image we run makes the adjustment
itself, which is what the line above always claimed. The 21264's own
manual says so too: "If the exception was a fault or a synchronous trap,
EXC_ADDR contains the PC of the instruction that triggered the fault or
trap" (HRM 5.2.7, p. 5-8), and Table 6-8 lists ARITH as a synchronous
trap. The instruction oracle checks it on every `/V` case. The architecture
describes the PC the *operating system's* handler is entitled to. Between
the hardware and that handler sits PALcode, and this one expects the
trigger's own PC from the processor. Anyone revisiting this needs a guest
boot, not a reading of the handbook: the handbook is not wrong, and neither
is the code, and only running one of them tells you which layer you are
looking at.

### The native OpenVMS PALcode routines

`AlphaCPU_vmspal.cpp` replaces the ES40 console's OpenVMS PALcode
(V1.98-104, at PAL_BASE 0x8000) with native routines: 49 `CALL_PAL`
routines, the interrupt entries (device, interval timer, IPI, software,
AST) and the TB-miss and fault entries. They run in interpreter builds on
the ES40 only, whenever PAL_BASE is 0x8000, so on that lane they serve the
SRM console as well as OpenVMS. JIT builds, the other boards and
`palcode.vms.nohle = true` run the console's PALcode itself.

`test/vms/vmspal_oracle.sh` ([development.md](development.md#the-vmspal-oracle))
compares every one of them with that PALcode doing the same work, from the
same state, on an OpenVMS 8.4 boot with a DCL workload and SHUTDOWN: about
35 million events a run, with the PALcode as the expected value. On
2026-10-22 (base 932c955) the guest called 47 of the 49 routines, and all
but `MTPR_DATFX` agreed in every compared field (PROBER and PROBEW apart
from the PALcode's scratch temporaries); so did the interrupt and TB-miss
entries, apart from those temporaries, from races (an interrupt arriving
while the PALcode ran) and from the first row below. These differ, and are
open:

| What | Where | Consequence |
| --- | --- | --- |
| After a nested double TB miss the DTB-miss routine takes `EXC_ADDR` for `MM_STAT` (the double-miss routine leaves `MM_STAT` in p7<63:16>, which the PALcode takes back at 0xd080): a write fault is reported as a read (R5, the MM flag, is 0; TNV or ACV is decided on the read enable), and a `STQ R31`/`STL R31` to a page that is not valid is dismissed as if it were a load to R31, so the store is skipped without a fault | `vmspal_ent_dtbm_single` | OpenVMS hits it: 115 wrong R5 values and 17 skipped stores in one boot. A skipped store of zero is lost data on a transition or paged-out page. ARM 14.3.1 |
| `MTPR_DATFX` writes the inverse of R16<0> into bit 63 of PCB+0 (the saved KSP) instead of R16<0> into bit 63 of PCB+56 | `vmspal_call_mtpr_datfx` | The process's DAT bit is never set (the PALcode's unaligned-access handler reads it); the KSP slot is rewritten at the next SWPCTX before anything reads it. ARM 13.3.4 |
| `MTPR_FEN` stores R16's low longword at PCB+56, not R16<0> | `vmspal_call_mtpr_fen` | None while OpenVMS passes 0 or 1. ARM 13.3.6 |
| b_irq<0> (the Cchip's error interrupts) and b_irq<5> are never delivered, and DIR<50> stops the emulator | interrupt check in `AlphaCPU.cpp`, `vmspal_ent_ext_int` | Latent: nothing raises them today. The PALcode dispatches all six lines |
| In the console's 1-on-1 mode the I/O-hole check tests VA bit 43 inverted, and one bound differs from the PALcode's table | `vmspal_ent_dtbm_single` | Latent: the console reaches I/O through the superpage |
| A `STx_C` after the native `REI` can still succeed | `vmspal_call_rei` | The ARM clears lock_flag on REI (4.2.4); the PALcode's REI loads its frame with `LDQ`, which fails a later `STx_C` since CPU-01, the native one with physical reads. Only `LDx_L; REI; STx_C` sees it |
| IPI before the interval timer when both are pending (the PALcode takes the timer first); MCES<7:5> cleared on write; MTPR_SIRR ignores R16 >= 16 instead of using R16<3:0> | `vmspal_ent_ext_int`, `vmspal_call_mtpr_mces`, `vmspal_call_mtpr_sirr` | None observed |

With the first three fixed on the oracle's copy, their differences were
gone. The full list, with the PALcode addresses compared and the proposed
fixes, is the vmspal audit's (VMSPAL-01 to -14). What vmspal buys in speed
is in [openvms.md](openvms.md#timings).

Before that, a comparison restricted to `REI` (2026-10-01: the native
routine first, its result kept and undone, then the PALcode, compared at
the PALcode's `HW_RET`) found these, all fixed:

1. From a non-kernel mode, a DTB miss on the first frame load ran the fill
   routine, which uses p4-p7 as scratch, after the old mode had been put in
   p7. The outgoing stack pointer was then saved to PCB+0 (the kernel stack
   slot) and the incoming one loaded from a slot never updated; OpenVMS 8.4
   startup hung after `%STDRV-I-STARTUP`. The PALcode reads the old mode
   only after its loads.
2. That same branch (non-kernel to non-kernel, e.g. exec to user) did not
   re-evaluate interrupts, where the PALcode's `HW_MTPR CM` does: an AST
   for the mode entered waited for the next interrupt.
3. The IER table's rows for IPL 30 and 31 enabled performance-counter and
   corrected-read interrupts that the PALcode's table at 0xD00 does not
   (unobservable, neither is ever raised).

After the fixes, ~480000 compared `REI`s differed only where an external
interrupt arrived while the PALcode ran. A spurious master-PIC interrupt
(vector 7) stopped the emulator in the native interrupt entry; it is now
dismissed after reading the in-service register, as the PALcode does.

`ALPHABOX_VMSPAL_OFF` hands single `CALL_PAL` routines (or the interrupt
entries) back to the PALcode, for bisecting a guest that misbehaves only
with vmspal.

### The JIT

**The bail protocol**: every fault-capable helper probes without side
effects and returns before touching memory, a register or the lock flag.
So a fault is taken once, by the interpreter, with the instruction not
half-executed.

**Against the interpreter on the main guests** (2026-10-03, AArch64, the
verifier counting every kind of difference, loads included): no mismatch in
any of them. Compiled blocks executed and compared, both processors
together:

| guest | machine, CPUs | blocks | mismatches |
| --- | --- | --- | --- |
| OpenVMS 8.4 to DCL, `SHOW CPU`, `SHOW SYSTEM`, a DCL loop, a MACRO-32 program | ES40, 1 | 1.44 billion | 0 |
| the same | ES40, 2 | 1.85 billion | 0 |
| OpenVMS 8.4 to DCL, `SHOW CPU`, `SHOW DEVICE`, `SHOW MEMORY/PHYSICAL` | DS20E, 2 | 5.5 billion | 0 |
| the same | ES45, 2 | 1.8 billion | 0 |
| OpenVMS 8.4 to DCL with USB devices (M7b's runs, before loads were counted) | ES47, 1 and 2 | 0.55 billion each | 0 |
| Windows 2000 RC2 to the desktop, then ten `dir /s c:\winnt` | ES40, 1 | 1.26 billion | 0 |
| the same | ES40, 2 | 1.69 billion | 0 |
| Whistler (Windows XP 64-bit, build 2210) to the desktop | ES40, 1 | 1.60 billion | 0 |
| SRM to `P00>>>` (`srm_run.sh`) | ES40, 1 | 112 million | 0 |

The x86-64 emitter was not run in that round: the host had no Rosetta
then. The verify lane cannot see compiled code's chaining, inline memory
paths or the `RPCC` stub (above); for those, run a production build with
`ALPHABOX_JIT_CHAIN=0`, `ALPHABOX_JIT_INLMEM=0` and `ALPHABOX_JIT_RPCC=0`
against one without.

**The instruction oracle's JIT groups** (2026-10-22, the legacy audit's
JIT lane; findings JIT-nn in `lab/legacy-audit/jit/AUDIT.md`) run on the
production lanes what the verify lane cannot see: every case register
live across chained exits and helper calls, branches whose direction
changes once a block has compiled, faults in the middle of a compiled
block at five positions, an interrupt at the return from PALcode,
LDx_L/STx_C across blocks, PALmode blocks, and rewritten code before an
`IC_FLUSH` ([development.md](development.md)). The interpreter passes
all of them, and so do both back ends under every JIT switch, except
for two divergences of compiled code, KNOWN on the oracle's last pass
until they were fixed:

- **A chained block ran in a mode it was not looked up for** (JIT-01,
  both back ends, closed 2026-10-22). Blocks are keyed by the processor
  mode they were translated in, because that is when the mode's right to
  execute the page was settled, but the lookup that resurrected a block
  after a flush did not compare the mode, nor did AArch64's inline
  computed-jump cache. A block compiled in kernel mode was then run in
  user mode, and its chained exits entered what they linked to in kernel
  mode: user mode executed a page only kernel mode may, with no `IACV`
  (HRM Table 6-8). Only a virtual address executed in two modes, with a
  successor the second mode may not execute, showed it; correct guests
  never branch into code their mode cannot run. Now one rule
  (`CJitEngine::key_matches`) decides on every path that finds a block:
  the lookup, the record after an interpreted span (under the mode the
  span started in) and the re-lookup after a flush. The chain fills and
  `jit_indirect` go through the lookup; a chained link into a block's
  cache slot compares that block's mode with its own, because a slot is
  re-recorded for another mode under the same address and epoch and the
  links to it stay; and the computed-jump cache keeps the mode in its
  tag and compares it with the live one. A PALmode block has no mode in
  its key (JIT-07): PALmode fetches are physical and checked by no mode
  (HRM 6.2), and a PAL entry reached from another mode used to miss the
  lookup and be re-hashed on every dispatch. `jitmode` passes on both
  back ends; SRM's console log is unchanged on the AArch64, `JIT_VERIFY`
  (107 million compiled blocks, 0 mismatches) and x86-64 lanes; OpenVMS
  8.4 logged in on the ES40 and on the DS20E with two processors (BACKUP
  and DIFFERENCES, no differences); Windows 2000 RC2 reached its desktop
  on one processor and on two.
- **AArch64's inline `HW_ST` did not tell the code-page map** (JIT-02,
  closed 2026-10-22). Compiled PALcode stored a physical `HW_ST` to
  DRAM inline without reporting the write, where the helper (and the
  x86-64 emitter, which always calls it) does. An `IC_FLUSH` after such
  a store into compiled code was skipped as having nothing to flush, and
  the old instructions ran (HRM 5.2.17). The
  inline store now tests the map's bit for its page first, and a store
  into a page that holds compiled code takes the helper, which reports
  it: the rule the page cache's write row already kept for virtual
  stores. `jitsmc` passes on both back ends, and so does `smc_test.sh`
  in all four arms. No guest is known to rewrite instructions with
  `HW_ST`; Windows 2000 RC2, whose PALcode does physical `HW_ST`s
  constantly, reached its desktop on one processor and on two.

**Also from that audit**, with no result a case shows:

- **The code-page map between processors** (JIT-05). A compiled block's
  source was checked once more and only then marked as code, so a store
  by another processor between the two was neither seen by the check nor
  reported by the store, and a later `IMB` with nothing reported was
  skipped with the old code published. The bits are set first now, then
  a fence, then the check. A processor that had the page cached as
  writable inline when it became code counts one write when an `IMB`
  makes it drop that translation, as a timer check already did. What
  remains: a store and its own report are not fenced from each other,
  so a store still in its processor's store buffer while the compiler
  checks can be missed; and a processor that still holds the page as
  writable inline stores into it unreported until its next timer check
  or `IMB`, so another processor's `IMB` in that window can still be
  skipped. Closing the first needs a fence on the store path, and the
  second a request every other processor answers before the block is
  published; neither was added. Windows 2000 RC2 and OpenVMS 8.4 on
  two processors, `smc_test.sh` on two and SRM on four ran as before.
- **What a block marks as code** (JIT-04). A compiled block copied,
  checked again, hashed and marked in the code-page map its whole
  interpreted span, up to a dispatch batch of 2000 instructions, though
  it compiles at most 64 and stops at its 8 KB page; past the page those
  words are the physically next page, which is not where the code goes
  on. A write anywhere in them made the next `IMB` real and the block
  compile again, and that page took the slower write path. Only the
  compiled prefix is used now. Nothing compiled ran differently: the
  extra words only invalidated more. SRM's console log is unchanged on
  the AArch64, `JIT_VERIFY` and x86-64 lanes, `smc_test.sh` passes on
  both back ends, and Windows 2000 RC2 reached its desktop.
- **The IPR and floating-point helpers** (JIT-08). Compiled code's
  `HW_MFPR`, `HW_MTPR`, FLTL and FLTV helpers were copies of the
  interpreter's code by hand. They agreed line by line, and nothing
  checked that they would go on agreeing. Both engines now call one
  implementation: `CAlphaCPU::hw_mfpr` and `hw_mtpr` (`cpu_pal.hpp`),
  `fltv_operate` and `fltl_operate` (`AlphaCPU.cpp`). A compiled
  `IC_FLUSH` now records its epoch cause as an interpreted one does
  (statistics only). The data helpers' side-effect-free translation is
  still a copy of `virt2phys`'s: calling it with `FAKE` instead would
  put a call on every page-cache miss and fold the `JIT_STATS` bail
  causes (TB miss, access violation, fault) into one, which was not done
  without a measurement. SRM's console log is unchanged on the
  interpreter, AArch64 and `JIT_VERIFY` lanes, `ALPHABOX_JIT_FPTEST`
  passes, Windows 2000 RC2 reached its desktop and OpenVMS 8.4 logged in.
- **A chained entry to the PAL reset vector** (JIT-13). The dispatcher
  flushes the instruction cache and the compiled blocks when it enters
  `PAL_BASE` (a firmware updater rewrote the image in place, or a
  restart), so a computed jump there is never chained (`jit_indirect`);
  but a static exit, a PC-relative branch in PALcode to `PAL_BASE`,
  could be, and then skipped the flush. The dispatcher no longer fills a
  link into it.

## A guest's floating-point exception that is the guest's own

Recorded because it looks exactly like an emulator fault and is not one
(2026-10-05; `lab/radeon-tcl/`).

Direct3D 7's software vertex pipeline in Windows 2000 RC2 for Alpha
(`d3dim700.dll`, build 2128) ends a program with
`STATUS_FLOAT_INVALID_OPERATION` (c0000090) in two places:

- every time a texture stage asks for
  `D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR` (the exception at
  `d3dim700 + 0x3285c`);
- once a run, in a scene that draws vertices without normals after scenes
  with them (`+ 0x34114`).

Both were followed to the instruction with `ALPHABOX_TRACE_ARITH`:

| | `+ 0x32858` | `+ 0x34110` |
| --- | --- | --- |
| instruction | `MULS f10, f11, f10` (`594b104a`), no `/S` | `MULS f12, f25, f12` (`5999104c`), no `/S` |
| operands | f10 = `00000000a0000000`, then `0000200000000000`: the words 5 and 0x10000 loaded by `LDS`, denormals; f11 = 0 | f12 = `fff0101000000000`: the word `ff808080` loaded by `LDS`, a signalling NaN; f25 = 1.0 |
| FPCR | `8990000000000000`: no `DNZ`, no trap disabled | the same |
| where the operand comes from | 12 bytes past a three-float temporary on the stack | the colour of a vertex (`ARGB ff808080`), read as its normal |

**The first is a loop in the DLL that is wrong as built.** The reflection
vectors are computed by a loop at `+ 0x53020` whose counter and both of
its pointers step *backwards* (`SUBL r13, #1`; `LDA r9, -12(r9)`;
`LDA r10, -68(r10)`) under an unsigned `CMPULT r13, count`, so it runs
once. The texture transform that follows (`+ 0x32830`) then walks `count`
vectors of 12 bytes from that one result into whatever the stack holds.
On x86 the same slip costs nothing: the wrong values are multiplied,
overwritten on the next call and never trap.

**The second** reads a normal from a vertex format that has none, because
a flag (bit 30 of the word at `+ 5532` of the device) was left set by an
earlier draw.

**Whether hardware does the same is not in doubt.** A denormal or a
signalling NaN as the operand of an IEEE arithmetic instruction is an
invalid-operation trap that nothing masks:

- "the 21264/21364 traps on a Denormal input operand for all arithmetic
  operations unless FPCR[DNZ] = 1"; "SNaN operand ... Invalid Op"
  (21264/21364 Compiler Writer's Guide, appendix C);
- "This trap is always enabled" (the Alpha Architecture Handbook 4.7.6).

Without `/S` there is nothing for the operating system to complete:
Windows raises the exception with the destination register's mask as its
argument (`00000400` and `00001000` here: F10 and F12). The operands are
words the guest's own integer code put there.

The emulator's part was checked three ways:

- the interpreter (`ALPHABOX_INTERP=1`) takes the same traps at the same
  addresses with the same operands;
- a `JIT_VERIFY` build ran the whole test (2.7 billion compiled blocks)
  with 0 mismatches and the same traps;
- the stack words are the same on every run.

So it is Microsoft's pre-release code, compiled without IEEE mode for a
processor that, unlike the x86 it was written on, traps on operands that
are not numbers.

**A related question.** An FP instruction that traps has its destination
written first here (the result the operation gives with the trap masked),
so a handler that completes an `/S` instruction whose destination is also
an operand finds the operand gone.

That is within the architecture: "if this trap occurs, an UNPREDICTABLE
value is stored in the result register", and software completion requires
that no instruction in the trap shadow modify a register the trigger reads
(Handbook 4.7.6, conditions 1 and 3). So code generated for software
completion keeps the
destination apart from the operands (GCC's `-mieee` patterns are
early-clobber for this reason). A compiler that does not is relying on
what no Alpha promised.

## The rule this audit produced

Two references are needed, not one. The handbook says what should happen;
a guest says whether the emulator can still run. The first audit's round
(2026-09-18) found three bugs that a guest would never have shown (a NaN
returned for an overflow under chopped rounding, denormals worth half
their value, memory barriers compiled to nothing) and one change that the
handbook endorses but a guest must approve before it ships (`UNDZ`;
settled on 2026-10-20 by trapping as the 21264 does, above).
Neither reference alone would have been enough.
