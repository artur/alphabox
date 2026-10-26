# How faithful the processor is

Alphabox emulates an Alpha 21264 of the EV68CB kind; the EV7 (21364) rows
of the Marvel machines run the same core, and so does the EV8 (21464) row,
in an EV7-compatible mode with the 21464's own instructions added stage by
stage ([below](#the-ev8-row-what-it-implements-and-what-alphabox-decided-2026-10-24)).
This file records where it does
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
  the case reports KNOWN until it is fixed and PASS after. `--cpu` runs it
  on any processor row: the EV7 rows give the EV68CB's verdicts, with
  their own IMPLVER (3), and the EV8 row the same with IMPLVER 4 and the
  cases of its own instructions (group `ev8`), which the other rows skip.
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
(Inside a PCI hose's memory, I/O and configuration spaces the read is a
master abort and returns all ones, as the Pchip does: HRM 8.8.2.1, PDF
p. 216. Only an address outside every PCI space reaches this entry.)

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

### Unused opcode 18 function codes are NOPs (2026-10-24)

An `18.xxxx` function code with no instruction completes as a NOP on every
processor row (the EV6 family, the EV7, and the EV8 row, which is of the
EV7 family), in the interpreter and,
because the JIT leaves those words to the interpreter, in compiled code:
no trap, no register or memory change, the next instruction runs. It was
OPCDEC before.

The Alpha Architecture Reference Manual allows either. The source for the
choice is the 21464 Internal Design Specification (Rev 1.1k, PDF page 676):
a Compaq engineer tried every `18.xxxx` code on hardware, and "most of the
unused ones trap (illegal instruction trap) on the 21164, but are NOPs on
the 21064 and the 21264". It is one engineer's test and it says "most": which
codes trapped on the 21164 is not recorded, so here all of them are NOPs.
The rule is a property of the processor family (`cpu_family_unused_misc_is_nop`
in `CpuModel.hpp`). An instruction a row adds in that space is decoded
before the rule applies: the EV8 row's CCB (18.EC00), LDL_ARM, LDQ_ARM and
QUIESCE (18.B000, 18.B400, 18.B800) are
([below](#the-ev8-row-what-it-implements-and-what-alphabox-decided-2026-10-24)).
`test/cpu` (group `misc`) checks a spread of codes.

### The EV8 row: what it implements and what Alphabox decided (2026-10-24)

The `ev8` row is the 21464 in EV7-compatible mode (option A of
[ev8.md](platforms/ev8.md), decision D1): the EV7's privileged interface
(IPRs, `HW_RET`, the 44-bit physical address, the CSR window), so the EV7
console and both EV7 PALcodes run unchanged, with the 21464's identity and
user-visible instructions on top. A real 21464 had a different privileged
interface ([ev8.md](platforms/ev8.md), "The PALcode interface"); this one
is the published EV8 as user code and an operating system see it. The
encodings and results are [ev8-isa.md](platforms/ev8-isa.md)'s. IDS is the
*21464 Internal Design Specification*, Rev 1.1k, by PDF page.

| What | The `ev8` row | Basis |
| --- | --- | --- |
| Family | `CPU_FAMILY_EV7`, so the ES47, ES80 and GS1280 take it and the Tsunami and Titan boards refuse it; `cpu_model::ext` = `CPU_EXT_EV8` says what the EV8 adds | **Alphabox decision** (D1): the family is what decides the address map, the start-up and the interrupts, all the EV7's in this mode |
| `IMPLVER` | 4 | [IDS p. 76] |
| `AMASK` | 0x1F07: BWX, FIX, CIX, MVI, precise traps, prefetch with modify intent (bits 0-2, 8, 9, 12), the core's; SMT (bit 10) since S3; the 1C SIMD set and paired single (bit 11) since S5 and S6 | of the IDS's 0x1F07 [IDS p. 76] the row reports only what it executes, so software that tests `AMASK` never uses a missing feature: **Alphabox decision**. Bit 10 (SMT) came with LDx_ARM and QUIESCE (S3), the instructions software tests it for [IDS pp. 668, 674], although the row has one TPU per chip until S4; bit 11 covers the 1C SIMD set and paired single by the owner's decision (2026-10-24) and came with S6, once both execute; bit 3, Tarantula's, when all its instructions execute: S7 gave the state, VC, VV and VS, and the bit waits for the memory instructions (S8) |
| Chip ID, processor type | chip ID 4, the EV7z's: the console calls it "EV7 rev 3.0", type 15 minor 5 | **Alphabox decision** (D2): the IDS gives CPU_CNFG<CHIP_ID> no value [IDS p. 487], and the console names the processor from its EV7 table |
| Caches | L1 64 KB each, L2 3 MB; the console shows 1.75 MB | [IDS pp. 42-43]; `BBOX_CTL` has seven 256 KB ways, all an EV7 console can count (`Ev7Csr.cpp`): **Alphabox decision** to keep the IDS's size in the row |
| CCB (18.EC00) | a no-op, which no address faults | "a hint with no architectural effect on data" [IDS pp. 411, 661]; no fault as for ECB, whose predecode it shares **[inference]** |
| CMOV2 (11.68) | Rc <- Rav, Rb or the literal unread | its predicate is clear outside the pair, so the CMOV2 Rc,Rb,Rc the chip makes copies Rc to Rc [IDS pp. 73-74]; Rc <- Rav for Ra != Rc **[inference]** |
| FCMOV2 (17.068) | Fc <- Fav; FEN with the FPU disabled | as CMOV2 [IDS pp. 73-74]; the FEN fault of every FP operate instruction but FNOP **[inference]**: the IDS says nothing of it |
| WH64EN, MAP_ALIGN | no-ops, on every row as before | [ARM 4.11.11]; MAP_ALIGN is `XOR R31,R31,R31` [IDS p. 75] |
| LDL_ARM, LDQ_ARM (18.B000, 18.B400) | Ra <- MEM[Rbv], LDL_ARM's sign-extended; no displacement; the faults are LDQ's, here this core's (misaligned only across a page, [above](#unaligned-data-accesses-do-not-trap)); then, if it did not fault, the watch on the 64-byte line read and watch_flag set | [IDS pp. 669-670]. The line is the LDx_L lock range, which the IDS requires the watched range to be [p. 670]. Performed whatever Ra, as LDx_L, and a memory operation for a pending STx_C, as LDQ: **[inference]** from "as similar as possible to the LDx_L instructions" [p. 669]. Fails to arm while an interrupt the processor would take is pending, in any mode [p. 678; the mode, **Alphabox decision**]; an address outside memory is never armed (**Alphabox decision**: nothing tells a processor of a store to I/O space) |
| What clears watch_flag | another processor's store or successful STx_C, a device's DMA write, in the line; an interrupt the processor would take; the timeout; every CALL_PAL; every exception or interrupt that clears the load lock (not the TB-miss fills); stopping the processor's thread, a reset, a remote write to its RBOX_SCRATCH1 (the Marvel start hand-off) | stores, STx_C, the timeout and REI, rti, rfe, retsys, urti [IDS p. 670]; the interrupt [p. 677]. **Alphabox decisions**: every CALL_PAL, where the IDS leaves the others UNPREDICTABLE [p. 670], so no PALcode's function codes need knowing; the exceptions, which "may" clear it for implementation-specific reasons; the processor's own store does not (**[inference]**: the IDS names another processor's). WH64 changes no data here, so it clears nothing [p. 671] |
| QUIESCE (18.B800) | with watch_flag clear, nothing; set, the host thread waits on a condition variable until it clears, bounded by QUIESCE_TIMEOUT (10240 cycles at the configured clock); never a trap, in any mode | [IDS pp. 668, 672-673, 678]; 10240 cycles as from power-up, since no IPR sets it in EV7-compatible mode (**Alphabox decision**, D16, [ev8.md](platforms/ev8.md)). A wake-up on the host's timer comes late, never early: the timeout is a minimum. Processor 0 fires the machine's interval timer from inside the wait. The QUIESCE does not sleep while another running processor may still store into a newly watched page through a translation cached as inline-writable (compiled code); it then fails, which the IDS allows [p. 671] |
| After a snapshot | every watch_flag clear | not in the state file (no format change): a wake-up the IDS allows, since software tests its lock again after QUIESCE [p. 674]. **Alphabox decision** |
| TPUs, by default | one processor per chip (`tpus` absent or 1): TPU 0, the chip's PID, nothing of TPUs 1-3 | the faithful EV8 under the EV7 console: the four TPUs share the chip's network address (R_WHOAMI [IDS p. 527]) and differ only in TPU_CNFG<57:56> [p. 488], and with no WAKEUP entry in the EV7 PALcode TPUs 1-3 would most likely never start **[inference]**; the owner's default (2026-10-25). In the native interface (S11-S13) TPUs are done the IDS's way: one node per chip, the TPU from TPU_CNFG |
| TPUs as processors (`tpus = 2..4`) | each TPU a processor of its own, a `CAlphaCPU` with its own registers, IPRs, TBs (Mode 2: every TPU its own TB group [IDS p. 306]), cycle counter, LL/SC lock and watch_flag; TPU 0 keeps the chip's PID, TPU t is PID \| t << s, s the lowest bit pair no configured chip's PID uses (ES47 and ES80: PID + 2t); TPUs 1-3 have a 4 MB register block of their own, no memory and no IO7, and start parked on their RBOX_SCRATCH1 like a Marvel secondary | **Alphabox construction** [decision, the owner's, 2026-10-24 (D4, D5, D7) and 2026-10-25 (the numbering, in place of D6, and the default)]: per-TPU state is the IDS's [pp. 46, 477-479, 482-483], the PIDs and register blocks of TPUs 1-3 are invented, so that OpenVMS and Linux see and use the threads. TPU 0 keeping the chip's PID matches R_WHOAMI. Interrupts (D5) follow the blocks: each TPU's RBOX_INT/IMASK/IREQ, its interval timer and its GIO port, an IO7 LSI or MSI naming the TPU's PID; the IDS leaves steering "TBD" [pp. 527-528] |
| More than 32 processors | refused at startup and by `--check`, with the reason | `CSystem::kMaxCPUs`: the LL/SC and watch flags are a bit each of a 32-bit word ([ev8.md](platforms/ev8.md), S4, has what raising it would take) |
| The other codes | decoded exactly: 11.68, 17.068 and 18.EC00 only | **Alphabox decision**. The IDS's decode tables use don't-care bits, so on a 21464 every `18.xxxx` code is some instruction (18.D000 decodes as RPCC, 18.E400 as RC, 18.F400 as RS) [IDS p. 661], 11.68 to 11.6B and 11.78 to 11.7B are CMOV2 [IDS p. 655], and opcode 17 is decoded on the low bits of its function, so more codes than 17.068 are FCMOV2 [IDS p. 660]. The row keeps the core's rule for codes that are no instruction (unused 18.xxxx no-ops, OPCDEC elsewhere), as the owner chose for S2 |
| The 1C SIMD set (S5) | every row of [ev8-isa.md](platforms/ev8-isa.md) 3.3 in the register and literal forms, with the sheet's results (D8): the zz sizes as agreed, SAT, PERMB8's selectors (13 reads 0), GPKBLB4, VMINMAX's running pair, the tree sums exact in 64 bits, the shift counts modulo the element width (`cpu/Ev8Simd.cpp`); nothing traps or reads the FPCR. The other 1C codes as before on every row: the core's 00, 01, 30-3F, 70, 78, the rest OPCDEC | the results are the sheet's proposals (D8, point 6). Beyond the sheet: CMPLGE compares unsigned, as CMPWGE **[inference]** ("by analogy"); the unpacks read Rb only and ignore Ra, as this core's UNPKBW **[decision]** ("the operand as MVI's"); VMINMAX reads only the element-size low bits of each of Rb's longwords **[inference]** |
| Paired single (S6), the decode | every row of [ev8-isa.md](platforms/ev8-isa.md) 3.4 at the 0x080 \| Fnc base, under the qualifiers its class accepts (P-ARITH and P-CVT: none, /U (/V), /SU, /SUI with /C, /M, none, /D; P-CMP none and /SU; P-EST none, /U, /SU with any Rnd; P-BIT none); every other function code OPCDEC, before FEN; FEN with the FPU disabled for every class (`cpu/AlphaCPU_paired.cpp`) | the base and the classes are the sheet's [inference; decision; P-EST agreed]. The unary forms (PARCPLx, PARSQRTx, PCVTFI, PEXTL, PEXTH) ignore Fa **[decision]**, as this core's CVTxx do (its SQRTx alone raise OPCDEC for Fa != 31). Fc = F31: nothing written, the exceptions still raised, as for a scalar instruction |
| Paired single, the arithmetic | each half as the scalar S instruction with the word's own Trp and Rnd and the FPCR, through the core's IEEE routines: ADDS, SUBS, MULS for P-ARITH, the operands by the table (cross and horizontal forms), PMULLN and PMULHN negated after rounding; the compares as CMPTxx (NEQ, UN as EQ), writing 1.0 or a true zero; PCVTFI as CVTTQ at 32 bits (out of range the low 32 bits, IOV, INV with /S); a half's operand is the half as LDS maps it, its result as STS packs it. Each half's exceptions set the FPCR's status bits; both halves are always computed and written; a trap is the scalar instruction's on this core: EXC_SUM the OR of both halves' reports, SWC with /S, REG = Fc on `ev8` (the 21464's register fields on `ev8n`), EXC_ADDR what a scalar ARITH gives | [IDS p. 280]; D9; [ev8-isa.md](platforms/ev8-isa.md) 3.4 and section 5. The negation flips the sign of whatever the rounded product is, a NaN's too, and an underflow's true zero becomes -0 **[inference]** ("negated after rounding"). A denormal half is a denormal operand: the unmaskable trap, or with DNZ under /S a zero, as for a scalar S operand |
| Paired single: PFMAX, PFMIN | (faX < fbX) ? fbX : faX, and (faX < fbX) ? faX : fbX, compared as PCMPLT (INV for any NaN); ordered, the operand's bits as they are; unordered, the NaN quieted, Fb's first | the sheet's **[decision]**; "the reverse" as the same compare with the results swapped, so for equal operands (+0, -0) PFMAX gives faX and PFMIN fbX **[inference]**; the unordered half "gets the result the scalar rules give one": ADDS's for NaN operands **[inference]** |
| Paired single: the estimates | the exact reciprocal (PARCPLx) or reciprocal square root (PARSQRTx) chopped to 14 fraction bits whatever Rnd is; the exceptions DIVS of 1.0 by the operand raises (the root: SQRTS's), never INE; the H and L forms' other half 0, its operand unread; a NaN quieted, its fraction kept; 1/sqrt(+-0) the infinity of the zero's sign, with no exception; a negative root an invalid operation, the canonical quiet NaN | the sheet's **[decision]**. The reciprocal is DIVS/C then the fraction chopped, which truncates the exact quotient; the root is computed exactly by integers. The zero's root has no value in the sheet: the infinity as 1.0 / SQRTS(+-0) gives it, and nothing raised as SQRTS raises nothing **[inference]**: IEEE 754's rSqrt would signal division by zero, a point for the owner and nada. A NaN's fraction is not chopped (the rule speaks of a reciprocal) **[inference]**. After a denormal operand's trap the result is UNPREDICTABLE: the reciprocal what DIVS computes, the root +inf **[decision]** |
| Rows without `CPU_EXT_EV8` | unchanged: 11.68 and 17.068 raise OPCDEC, 18.EC00 is an unused code (a no-op), the new 1C codes and opcode 07 raise OPCDEC | |
| Tarantula's state (S7) | per processor, so per TPU: $vr0-$vr31 of 128 quadwords ($vr31 reads zero and discards writes), vl (0-128), vs, vm (128 bits), VCTL<VEN>; all clear at power-up and at a reset. On the rows with `CPU_EXT_TARANTULA` (`ev8`, `ev8n`) only, kept outside `state`: no other row's processor, state or state file changes | [TAR pp. 282-284]; [ev8-isa.md](platforms/ev8-isa.md) 4.1. The values are UNPREDICTABLE but VEN's: clearing them all is the sheet's **Alphabox decision**. Tarantula a flag of its own beside `CPU_EXT_EV8` **[decision]**: the paper's "follow-on candidate to the EV8", with an AMASK bit of its own |
| VCTL, the vector enable | the quadword CSR at offset 0x3F0000 of the processor's own register window (`CEv7Csr` answers it from the processor), VEN <0>, <63:1> RAZ/IGN, reset 0; with VEN = 0 every word of opcodes 01-06 raises OPCDEC with EXC_ADDR its own PC, and changes nothing | **decision D11** and the owner's offset (2026-10-25), [ev8-isa.md](platforms/ev8-isa.md) sections 2 and 5. Not reported as an unknown opcode (`%CPU-W-OPCDEC`): it is how an OS's lazy switching works |
| VCTL on the ES40 | under `ALPHABOX_CPU_ANY_BOARD` the processor answers its VCTL address itself, so the CPU oracle sets VEN with the store an OS would use; the Tsunami ignores PA<42:35> in its I/O space, so every PID's window is one address there and each processor reaches its own | **[decision]** (S7), [headless.md](headless.md) |
| VC (opcode 05) | SETVL (vl <- MIN(Rbv unsigned, 128), Rc <- vl), SETVS, SETVM (below vl; zero above), GETVL, GETVS, GETVM (below vl; the rest unchanged), register and literal forms | [ev8-isa.md](platforms/ev8-isa.md) 4.7 |
| VV, VS (01, 02): integer | every function of 4.4's table on each active element (i < vl, and vm[i] under the mask); inactive destination elements unchanged; the arithmetic wraps, nothing raises integer overflow; a compare writes the quadword 1 or 0; VVMRG and VSMRG select by vm for every i < vl | [ev8-isa.md](platforms/ev8-isa.md) 4.3, 4.4; leaving inactive elements unchanged is the sheet's Alphabox choice (where it makes them UNPREDICTABLE, point 7) |
| VV, VS: floating point | each active element as the scalar instruction with /SUI and dynamic rounding: ADDT, SUBT, MULT, DIVT, SQRTT /SUID; the compares as CMPTxx/SU (NE as EQ, GT and GE as LT and LE), writing 1 or 0; CVTQT/SUID, CVTTQ/SVID, and VVCVTTQC as CVTTQ/SVIC; CPYSx raise nothing. The FPCR is updated element by element, in element order. Every active element is computed and written (the IEEE default where one trapped); then one arithmetic trap if any element would have trapped: EXC_SUM the OR of the elements' with SWC and REG 31, raised as the scalar instruction raises its own, so EXC_ADDR is what a scalar ARITH gives here (the trigger's PC, which the PALcode turns into the next) | D13 and point 11, [ev8-isa.md](platforms/ev8-isa.md) 4.3 and section 5. "Would have trapped" is the scalar instruction's on this core, the 21264's: an exception enabled and not disabled by the FPCR, or an FPCR status bit to set (EXC_SUM<SET_x>, 21264 HRM Table 5-9) **[inference]**. The scalar words' Fa, Fb and Fc are 31, so the trap names no scalar register **[decision]** |
| FEN | with the FPU disabled, VV and VS functions 20-3F take FEN; the integer functions and VC do not | **[inference]**: each element is computed as a scalar FP instruction, which takes it; the functions 20-3F are the FP half of the map |
| The field rules | an SBZ <15:13>, VV's <12>, a literal on a VS code that reads Fb, M or a literal on a VC instruction that has none, M on VVMRG or VSMRG, an unassigned code: OPCDEC whatever VEN is. A register field an instruction does not use is ignored | [ev8-isa.md](platforms/ev8-isa.md) 4.3, 4.4, 4.7; VV's <12> treated as SBZ **[inference]** (it "must be 0" and is no register field); a literal on SETVM, GETVL, GETVS, GETVM OPCDEC **[decision]**, as a literal on VS's FP codes |
| SM, RM (opcodes 03, 04); 06 | OPCDEC: the memory instructions are S8's; 06 is spare | **[decision]** for 03 and 04 until S8, which AMASK bit 3 (clear) lets software expect; 06 [ev8-isa.md](platforms/ev8-isa.md) 4.2 |
| In PALmode | the vector instructions execute as in any mode | UNPREDICTABLE there ([ev8-isa.md](platforms/ev8-isa.md) section 5): **[decision]** |
| The JIT | opcodes 01-06 end every compiled block (`classify()` compiles none of them, on every row) and the interpreter runs them, so JIT_VERIFY never meets one | S10 is the JIT's |
| The state file | a section of its own after the processor's block (and after the native EV8's IPRs), with a magic word, for the rows with Tarantula; a state file without it, written before S7, restores with the vector state clear and VEN 0. No version step | **[decision]** (S7): rows without Tarantula write and read exactly what they did |

The EV7 console never executes `IMPLVER` or `AMASK`; OpenVMS 8.4 and
Linux do, and take the same paths for 4 as for the EV7's 3
([ev8.md](platforms/ev8.md), "S1 and S2").

A TPU's state is a processor's, so neither JIT needed anything for S4:
compiled code addresses its own `CAlphaCPU` and its own bits of the lock
and watch words, by processor number.

Both JITs compile CCB, CMOV2 and FCMOV2 on the `ev8` row only:
`classify()` takes the row's `ext` (`set_cpu_identity`), and on the other
rows the words stay the interpreter's. The 1C SIMD set and paired single
(S5, S6) compile on the rows with `CPU_EXT_EV8` as calls to the
interpreter's own routines that do not end the block: `jit_simd` (Rc from
Rav and Rbv as compiled code holds them) and `jit_paired`
(`paired_operate`: a FEN bails to the interpreter, an arithmetic trap
leaves through the ARITH entry, as a compiled FLTV does); a code the sheet
does not assign, and the same words on any other row, end a block before
them. Groups `simd` and `paired` check both, interpreted on the first
pass and compiled on the last. `test/cpu` group `ev8` checks each
instruction where the row has it (`rows.Own`), and groups `misc` and `ev8`
what the other rows do with the same words.

LDL_ARM, LDQ_ARM and QUIESCE are the interpreter's in every build: a
block ends before each. Compiled code keeps the watch's rules in two
places: a compiled CALL_PAL clears the processor's watch_flag on the
`ev8` row, and no compiled store goes into a watched page inline (the
page cache refuses such a page as writable, as it does a code page, and
on a machine with an `ev8` processor the AArch64 HW_ST physical forms take
the helper), so every store into a watched line passes the code that
clears the flags. Group `quiesce` checks the instructions with one
processor; `test/cpu/quiesce_mp.py` the wake-ups by another processor's
store and STx_C, by an interrupt and by DMA
([development.md](development.md)).

Tarantula (S7, [ev8.md](platforms/ev8.md)) is the interpreter's. Group
`tarantula` checks the rows above on `ev8` and `ev8n`, and opcodes 01-06
raising OPCDEC on every other row; `test/cpu/tarantula_vv.py` runs daxpy,
a dot product, a masked update and a strip-mined loop against scalar code
in one image. Both read the vector registers through a test hook,
`ALPHABOX_TARANTULA_PEEK` ([headless.md](headless.md)), since S7 has no
vector loads or stores.

### The native EV8 (`ev8n`): what it implements and what Alphabox decided (2026-10-25)

The `ev8n` row is the 21464 on its own privileged interface, the IDS's
(stage S11 of [ev8-native.md](platforms/ev8-native.md), whose decisions
N1 to N31 the owner accepted on 2026-10-25). No firmware or operating
system exists for it yet: the CPU oracle runs it on the ES40 through
`ALPHABOX_CPU_ANY_BOARD`, with a PALcode of the image's own, and the
Marvel boards take it in 21464 mode (S11b, [below](#the-marvel-model-in-21464-mode-ev8n-on-the-es47-es80-and-gs1280-2026-10-25)),
started from a boot image. Its user
instructions and AMASK are the `ev8` row's; what differs is everything
PALcode sees. The code is `cpu/cpu_pal_ev8.hpp` and `cpu/AlphaCPU_ev8.cpp`.
IDS is the *21464 Internal Design Specification*, Rev 1.1k, by PDF page.

| What | The `ev8n` row | Basis |
| --- | --- | --- |
| Family, row | `CPU_FAMILY_EV8`, a family of its own; IMPLVER 4, AMASK 0x1F07 (the `ev8` row's, bit 11 since S5 and S6), type 18 minor 0, chip ID 0, `CPU_EXT_EV8`; the ES47, ES80 and GS1280 take it in 21464 mode (below), other boards only through the test hook | **Alphabox decisions** N16 and D3 |
| Modes | PC<1:0>: 00 normal, 01 PALmode, 11 SuperPALmode; EXC_ADDR and DTBMS_RET_ADDR carry them | [IDS pp. 484-485, 573-574] |
| SuperPALmode | entered by a RET with Rb<1:0> = 11, behaves as PALmode; no PNMI is ever raised (no PALmode timeout, no TPU restart) | **decision N1** |
| Jumps in PALmode | every jump-format instruction takes the new mode from Rb<1:0>; the link of every jump, BR and BSR included, keeps the mode bits; Rb<1:0> = 10 is normal mode | **decision N2**; BR and JMP/RET as BSR and JSR, one rule for one instruction with several hints **[inference]**; 10 **[decision]**: no mode has the encoding |
| Jumps in normal mode | Rb<1:0> ignored, no RET raises the mode; a target outside the virtual address space (no enabled superpage's, not sign-extended for I_CTL<VA_SIZE>) takes BAD_JUMP_IVA at the jump, which does not complete | [IDS pp. 573, 629-630, 633]; a branch or a sequential walk out of the space is IACV [p. 630] |
| HW_MFPR, HW_MTPR | 0x19 with Rc <4:0> the destination, 0x1D with Rb <20:16> the source, the index <12:5>; the reader and writer classes ignored | **decision N3**; the classes order a pipeline that an emulator retiring in order does not have **[inference]** |
| HW_LD, HW_ST | the types 000, 010 (loads), 100, 101 (loads), 110, 111 (loads), the four lengths, the 11-bit displacement, the address forced to the data size, no alignment trap | [IDS pp. 569-570]; 001 and 011 OPCDEC (**decision N4**); a load-only type on a store OPCDEC **[decision]**: an unassigned code |
| IFETCHB (0x1E) | a no-op in PALmode | an emulator that executes in order and commits every write at once has nothing to wait for **[inference]** |
| The PALcode forms outside PALmode | 0x19, 0x1B, 0x1D, 0x1E, 0x1F raise OPCDEC (no HWE) | **[inference]**: the 21464's I_CTL has no HWE [IDS pp. 489-490] |
| Shadow registers | S0 and S1, integer only, per TPU, by position: operand A and the destination R24/R25, operand B R22/R23 | [IDS p. 580]; integer only **decision N5**; per TPU **[inference]** |
| CMOV in PALmode | a native CMOV (followed by `bis $31,$31,$31`) executes as the chip splits it, so its Rc's old value is read as operand B; a legacy CMOV's Rc is not replaced at all | [IDS pp. 574-575, 580]; the consequence for Rc = R22 or R23 (the value kept is S0 or S1) **[inference]**, as for Restriction 13's R24 and R25 |
| CALL_PAL | the target PAL_BASE<47:15> \| 0x2000 \| function<7> << 12 \| function<5:0> << 6 \| PC<1> \| 1; S1 gets the return PC with the caller's mode; EXC_ADDR is not written; the 21264's OPCDEC rule for privileged and unassigned functions | [IDS pp. 149, 499]; S1 and EXC_ADDR **[inference]** (Table 22-3 does not list CALL_PAL); the OPCDEC rule **[inference]** |
| IPR field selects | each register's low index bits select the fields a write changes (I_CTL, M_CTL, I_MODE, M_MODE, I_PCTX, M_PCTX, CC); any of a register's indexes reads all of it | [IDS pp. 482-509]; the reads **[inference]**: the IDS gives one index range per register |
| CPU_BASE, VCTL | CPU_BASE 0xAB, the chip's number <9:0>, read-only; VCTL 0xAC, VEN <0>, per TPU, the vector enable of Tarantula since S7 (as on the `ev8` row, above); the CSR offset 0x3F0000 is not decoded (in 21464 mode it is TPU 0's R_INT_MASK, N22); `ALPHABOX_TARANTULA_PEEK`'s window on the ES40 follows the 21464 mode's map (N18) | **decisions N6, N8**; the map **[decision]** (S7) |
| IER_CM, SIRR, ISUM, HW_INT_CLR | 0xB0-0xB3, the 21264's layouts without ASTEN; IER_CM<CM>, I_MODE<CURRENT> and M_MODE<CURRENT> one field; no AST registers | **decision N7**; the 21464 has no AST registers [IDS pp. 491-492] |
| CC | MFPR reads OFFSET, COUNT and ENABLE; RPCC reads OFFSET and the count's 32 bits, as on the other rows | MFPR is "TBD" [IDS p. 482]: **decision N7**. RPCC's low four bits are the counter's, where the register has RAZ and ENABLE [p. 483] **[decision]**: RPCC as on the 21264 (N7), and the JIT's inline RPCC stays the other rows' |
| PAL_BASE | <47:15>, a physical address; <51:48> read zero | **decision N6** |
| DTB_PTE0/1, DTB_TAG0/1 | one entry: a PTE write at either index writes it, and the second of a pair replaces the first's entry; a fill of a page its TPU group already holds replaces it | **decision N9**; the replacement of any such entry **[decision]**, as on the other rows (two matching entries are the chip's "multiple matches" [p. 578]) |
| A PTE with VALID clear, or from R31 | no entry; counted, and traced with `ALPHABOX_TRACE_EV8` | the chip hangs [IDS pp. 505, 578]: **decision N10** |
| ITB_IS | shares its storage with ITB_TAG, so a write to it is a tag too | [IDS p. 495] |
| IC_FLUSH, IC_FLUSH_ASM | the whole instruction cache | "this group's blocks" [p. 493]: the cache has no group tag here, and flushing more is invisible **[inference]** |
| Performance registers | read zero, writes ignored | **[decision]**: nothing is counted (chapter 19) |
| An index the table lacks | reads zero, the write ignored, a warning once | **[decision]**, as on the other rows (`UNKNOWN_IPR`) |
| Reset values | zero, but QUIESCE_TIMEOUT 0x2800, TPU_CNFG<TPU_ID>, CPU_BASE, CHIP_ID 0, DC_CTL<SET_EN> 11, PAL_BASE the loader's | Table 16-1 classes them as initialized, with no value: **[decision]**; QUIESCE_TIMEOUT [IDS p. 511]; SET_EN "at least one set must be enabled" [p. 501] **[inference]** |
| Entries | chapter 22's: DTBM_DOUBLE, DTBM_DOUBLE_ALT, FEN, UNALIGN, DTBM_SINGLE, DFAULT, OPCDEC, IACV, ITB_MISS, ARITH, INTERRUPT, MT_FPCR, DTBM_SINGLE_CONS, ITB_MISS_CONS, BAD_JUMP_IVA, MILD_RESET (0xB00, power-up); never MCHK, IMCHK, DST_NXM, FAST_RESET, WAKEUP, TPU_RESTART | **decision N11** [IDS pp. 633-634]; machine checks are never raised on any row (above) |
| What an entry writes | Table 22-3: DTBM_SINGLE(_CONS) DTBMS_RET_ADDR, not EXC_ADDR; ARITH and MT_FPCR the next instruction's PC; EXC_SUM's register fields the instruction's <25:21>, <20:16>, <4:0>; M_STAT<OPCODE> the plain opcode; a HW_LD/PTE writes no VA | [IDS pp. 483-486, 510-511, 633]; EXC_SUM<SET_*> read zero **[decision]**: the core sets the FPCR's bits itself, as on the other rows |
| MT_FPCR outside PALmode | traps to its entry after writing the FPCR | [IDS pp. 581, 630]; the 21264 rows do not take theirs (below) |
| Console mode | I_MODE<CONSOLE> and M_MODE<CONSOLE> send the ITB and DTB misses to the _CONS entries | [IDS pp. 491, 508] |
| Double miss | M_CTL<DBL_ALT> picks DTBM_DOUBLE_ALT; a bad VA sign is DFAULT with M_STAT<ACV> | **decision N12** |
| Translation | Appendix C's superpages (SPE52, SPE43, SPE32) checked first, to bit 63, then the sign for VA_SIZE; 52-bit VA with 64 KB pages, every other VA_SIZE/PAGE_SIZE combination as 43-bit VA and 8 KB pages; VA_FORM and IVA_FORM in their three formats; the superpage enables per TPU | **decisions N13, N14** [IDS pp. 489-490, 497-498, 507, 512-513, 688-695] |
| Big-endian | M_CTL<BIG_ENDIAN> stored and read back, nothing inverted | **decision N15** |
| TB entries | the tag VA<51:13> (fewer bits with a granularity hint), ASN or ASM, and the TPU group of the fill; with 64 KB pages the PFN is PA<47:16> and the hints are 8, 64 and 512 such pages | [IDS pp. 305-307, 495-496, 504-506] |
| ITB_IM, DTB_IM | the IDS's modes 00, 01, 03, 05, 0D, 10, 16, and every other value read as bits: <0> this group, <1> ASM entries, <2> non-ASM entries, <3> this ASN, <4> the write pointer | [IDS pp. 494, 502-503]; the bits **[inference]**: they give the IDS's seven exactly. ITB_IM's "this group" is I_PCTX's, where its text says M_PCTX's and ITB_IS's says I_PCTX's [pp. 494-495, **conflict**] **[inference]** |
| **A TB per TPU** | each TPU keeps a TB of its own, its entries tagged with the group of the fill; a TPU_GRP write shows the new group's entries only | **[decision]**: the chip's TPUs share one TB [IDS p. 46], and sharing it would put a lock or a cross-thread invalidation on every lookup and fill of four host threads, and on the data page cache in front of them. Mode 2 (every TPU in a group of its own, N25's for Linux) is exact |
| **Divergence: Mode 3** | TPUs in one group do not see each other's fills, and an invalidation reaches only the writer's TB | on the chip a group's entries serve all its TPUs, and an invalidation by mode "all groups" reaches every group [IDS pp. 306, 494]. Software that invalidates on every TPU it changes a translation for sees no difference but in the misses it takes |
| QUIESCE | QUIESCE_TIMEOUT decides: TIMEOUT cycles at the clock with TIMEOUT_EN, no timeout without; WATCH_EN clear, no store clears watch_flag | [IDS pp. 510-511]; **decision N17** follows the register (enables 0 at reset); `ALPHABOX_QUIESCE_TIMEOUT` is the `ev8` row's and does not apply |
| TPUs | four per chip by default, each a processor with its own registers and per-TPU IPRs; the per-chip CPU_CNFG, DC_CTL and CPU_BASE shared; every TPU of every chip resets at PAL_BASE + 0xB00 with its TPU_ID; off the Marvel boards each TPU is one of the board's processors | [IDS pp. 46, 477-481, 488]; all at 0xB00 **decision N17** |
| The JIT | PALmode code interpreted, never compiled; CALL_PAL, JMP/JSR/RET/JSR_CO and MT_FPCR the interpreter's; normal-mode loads and stores compiled, their helpers translating by the 21464's rules through `virt2phys_fast`, the interpreter's own fast path (`ev8_superpage`, the group's TB entries) | **[decision]** (part of N16): the 21464's forms through helpers once the interpreter's are proven; the jumps for BAD_JUMP_IVA |
| The state file | the native IPRs after the core's block, for this family only | **[decision]**: no other row's format changes |

`test/cpu` group `ev8n` checks the table's rows with one TPU (the image's
EV8 PALcode, `image.build_pal8`, runs PALmode snippets through a CALL_PAL
of its own), and `test/cpu/tpu_mp.py` the four TPUs
([development.md](development.md)).

### The Marvel model in 21464 mode (`ev8n` on the ES47, ES80 and GS1280, 2026-10-25)

What the system logic does around an `ev8n` processor (stage S11b,
[ev8-native.md](platforms/ev8-native.md) section 2, decisions N18 to N23).
The chip's Rbox, Zbox and GIO registers are the processor's own on a real
21464, so they are listed here; the board side is
[marvel.md](platforms/marvel.md), "21464 mode". The code is
`cpu/ev7/Ev7.hpp` and `chipsets/marvel/`. The EV7 rows and the `ev8` row
are untouched by all of it.

| What | 21464 mode | Basis |
| --- | --- | --- |
| The mode | chosen by the processors' family: a Marvel board of `ev8n` processors; a board refuses the two families mixed | the board rows' `other_cpu_model`; mixing **[decision]** |
| The map | 48 bits; ipe8(n) = (~n & 0x1fff) << 35; the window at \| 0x7FFC << 20; IO7 port p at \| (~p & 7) << 32; memory of PID n at ((n & 3) \| (n & 0x3fc) << 1) << 34 | **decision N18** |
| An I/O address whose inverted field names no 10-bit PID (PA<46:45> not 11) | nobody's: traced as unclaimed | **[inference]** |
| Memory per PID | at most 16 GB (memory.bits 34), as on the EV7 | the IDS's 32 GB per processor [p. 43] is N18's two halves; the second, at + 2^36, is not modelled **[decision]** |
| PIDs | dense: chip c is PID c, in the configuration's order; a chip's TPUs share its PID, register block and coordinates | **decision N19** |
| The IO7s | where the board's layout cables them, by the console's number for the place; so the hoses (PID x 4 + port) follow the dense PIDs: an ES80's are 0-3, 8-11, 16-19, 24-27 | **[inference]**: N19 with the hose numbering kept |
| R_WHOAMI | EW <9:5>, NS <4:0>: the chip's coordinates | [IDS p. 527], N19 |
| CPU_BASE | the chip's dense PID, checked against the topology at start | N6, N19 |
| R_ROUT | 532 entries at 0x2000, 0x10 apart, entry n the route to PID n: VAL and the access bits <6:1> set, CAD <7> set, BOU, STF and PAR 0, RNS <10> and REW <11> the hops' directions, DEW <15:12>, DNS <19:16>, IND <21:20> the first hop; N/S hops first; North and East the higher coordinates; the sharing-mask entries 512-531 zero | the fields [IDS pp. 526-527, the text layer's order]; the values **[decision]**: nothing but our own code reads the table |
| R_CFG2 and the new port registers | R_CFG2 at 0x0E0, R_n_CFG2 at +0x50 of each port block, R_IO_CFG2 at 0xC050, R_IO_T2CFG at 0xC060; stored | **decision N20** and **[decision]** for the offsets; R_n_T2CFG is the EV7's RBOX_n_T2CFG at +0x40 |
| R_INT_STAT | 0x050, one per chip, 32 bits: IPIs <3:0> (level 3), ticks <7:4> (2), halts <11:8> (4), the queue <12> (1), GIO_INT <13> and GIO_HINT <14> (4), performance <15> and correctable <16> (0), uncorrectable <17> (5), software <31:18> (3); written with ones to clear, <12> only by the queue emptying | **decision N22** |
| R_INT_MASK | one per TPU, 32 bits: TPU 0's at 0x060 and 0x3F0000, TPU t's at 0x3F0000 + 0x40 x t; TPU t's level L is asserted while a bit of level L is set in R_INT_STAT & R_INT_MASK[t], into its own ISUM<EI>, gated by its own IER_CM<EIEN> | **decision N22** (N7 for the IPRs) |
| R_INT_REQ | 0x070: sets its bits of <3:0>, <11:8> and <31:18> in R_INT_STAT; reads back what was last written | the fields N22's table gives it **[inference]**; the read-back as the EV7's **[decision]** |
| The interval timer | R_INTER_TIM's ITV <7:0>; period (ITV + 1) x 2^17 cycles; 0 off; every tick sets <7:4> on each chip whose ITV is not 0; the machine's schedule follows PID 0's | the period **[inference]** (the EV7's, N22); 0 off **[inference]**, as the EV7 model |
| The interrupt queue | four IIDs of 24 bits; R_INT_QUE reads the head with <24> valid, a write removes it; a fifth refused | **decision N21** [IDS pp. 464, 528] |
| A processor's write to a full R_INT_QUEADD | dropped and counted | **[decision]**: a store cannot see the WrIONack [IDS p. 528] |
| The IO7's refused IID | kept, and sent again as soon as an entry of that queue is taken; not in the state file | "retry the same one" [IDS p. 465]; when **[decision]**; the state file **[decision]** |
| GIO_DATA | 32 bits: a write passes <31:0> on, a read returns DONE <63> and the far register's low 32 bits; DONE reads 1 at reset | **decision N23**; DONE's reset [IDS p. 474] |
| GIO_CNFG | DIVISOR <7:1>, the rest reading 0; zero (the reset value) stops the clock: a transaction begun then waits, DONE clear, until a divisor is written; the waiting transaction is not in the state file | [IDS pp. 472-473]; the state file **[decision]** |
| The EV7 console's GIO lock (0x80000) | not decoded | 2.5: our PALcode's lock is a word in memory |
| The management side's interrupt | R_INT_STAT<13> | N22 |
| Halts, GIO_HINT, errors, performance | <11:8> set by R_INT_REQ only; <14> to <17> never raised | nothing modelled raises them (N22's table: "not raised") |
| Start | no console: a boot image given as `rom.decompressed`, refused without one; every TPU of every chip at its PAL_BASE + 0xB00 | N17; the refusal **[decision]**: the EV7 console's decompressor would run on the 21464 |
| 32 processors | WHAMI = PID x 4 + TPU within Alphabox's 32: more is refused at configuration | N25 and `CSystem::kMaxCPUs` |

`test/cpu/marvel8_mp.py` checks the table on the three layouts, from a boot
image with the EV8 PALcode, and `test/unit/marvel21464_test.cpp` the map
and the topology ([development.md](development.md)).

### Smaller ones

| What | Where | Consequence |
| --- | --- | --- |
| `VA_FORM` is derived from the VPTE address on a double TB miss, where the HRM says the faulting address | `AlphaCPU.cpp`, `virt2phys` VPTE branch | Latent: shipping PALcode uses VA+PTBR, so nothing observed. New firmware reading `VA_FORM` there would compute the wrong page-table address |
| `MT_FPCR` does not take its synchronous trap (HRM 6.7.3) | `cpu_fp_operate.hpp` | PALcode bookkeeping hung on that trap never runs. Nothing we boot needs it. The native EV8 (`ev8n`) takes the 21464's (above) |
| `EXC_SUM[63:48]` is not the sign extension of `SET_IOV` | `AlphaCPU_ieeefloat.cpp` | PALcode testing EXC_SUM's sign as a fast "did IOV happen" check. We also set FPCR ourselves, so nothing reads it |
| Performance-counter, corrected-read and serial-line interrupts are never raised | `ISUM`, `int_deliverable` | Nothing on this machine uses them |
| The instruction cache is 2 MB with 2 KB lines; a real EV6 has 64 KB with 64-byte lines | `AlphaCPU.hpp` | Legal, but a guest that gets away with a missing `IMB` on real hardware can fail here (below); another processor's `IMB` drops the lines since 2026-10-24 |
| `AMASK` is advertisement, not a gate: the CIX/MVI/BWX/FIX instructions execute whatever a CPU row claims | `CpuModels.cpp`, `cpu_misc.hpp` | Correct for the EV68CB, which implements all of them. The EV7 rows claim the same set as an inference: neither EV7 PALcode nor the ES47 console executes `AMASK` ([marvel.md](platforms/marvel.md), E1). Their `IMPLVER` is 3, the ARM's value for the 21364 (Table D-4), since 2026-10-24. The EV8 row has the same set; its own instructions are gated by the row (`CPU_EXT_EV8`), not by `AMASK`, which reports each of them only once its stage is done (above). It matters the day a row is added for a part that lacks one of the set |
| Some written IPR fields have nothing behind them: `M_CTL<5:4>`, `DC_CTL` and `PCTR_CTL` (its counters never count) are kept; `C_SHFT`, index 0x2d (`M_FIX`), `HW_INT_CLR<28>`, `VA_CTL<0>` (byte order) and `VA_CTL<3>` are dropped; `C_DATA` and `I_CTL<23>`/`<14>` read 0 | `cpu_pal.hpp` | None seen. The ES40's and both EV7 PALcodes configure caches and error reporting through them, mask `I_CTL<23>`/`<14>` off before saving it, and read `C_DATA` only on their error path, where 0 is "no error" (E1) |

**The instruction cache** is architecturally legal: a virtual I-cache need
not be coherent, and `IMB` works. But stale bytes live far longer here.
Its lines carry the flush generation they were filled in, so an `IMB`
invalidates every one of them with a single store instead of a walk. It
must invalidate them, whatever else a flush decides to skip: the lines are
tagged by virtual address, so code mapped at an address that has held
other code is a change no physically-keyed bookkeeping can see.

Since 2026-10-24 an `IMB` on one processor also drops every other
processor's lines, at its next dispatch batch (the interpreter build: its
next timer check), through `g_icache_flush` (`AlphaCPU.hpp`). The 21264's
Icache is not coherent ("Icache coherence is not maintained by hardware",
EV68CB/DC HRM 4.5.1), but it keeps old code only in 64-byte blocks it had
fetched; a 2 KB line kept far more, until the processor's own `IMB`. Linux
on four EV7s died of it: the first processor to idle traps on `CALL_PAL
WTINT` (the EV7 UNIX PALcode has none), Linux patches the call out of
`arch_cpu_idle` and runs `IMB` on that processor only (`traps.c`,
`do_entIF`), and the boot processor, which had run `copy_thread` in the
same 2 KB, executed the old call, trapped, found the patched word and
stopped with "Instruction fault 4" ([linux.md](linux.md)). Seeing another
processor's change at that processor's `IMB` is more coherent than the
hardware, which the architecture allows.

**`AMASK`**: no document lists the 21364's AMASK, and neither EV7 PALcode
nor the ES47 console executes `AMASK` or `IMPLVER` (E1), so the EV7 rows'
value is an inference (`CpuModels.cpp` marks it as one). Their `IMPLVER`
is not: the Alpha Architecture Reference Manual (4th ed., Table D-4) gives
3 for the 21364/EV7 and EV79, which the rows return since 2026-10-24. Two
of Linux's three uses of `implver()` test for `IMPLVER_EV6` (2) only, so
on an EV7 Linux takes the other path, as on a real one: `fpu.h`'s
`swcr_update_status` does not merge the FPCR's accrued exception flags
into the software control word, and `xor.h` picks the measured fastest
`xor` routine instead of forcing the prefetching one. The third,
`ELF_PLATFORM`, tests for EV5 and then `AMASK`.

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

### The native OpenVMS PALcode routines (removed October 2026)

Until October 2026 the interpreter replaced the ES40 console's OpenVMS
PALcode (V1.98-104, at PAL_BASE 0x8000) with native routines,
`AlphaCPU_vmspal.cpp`, inherited from es40: 49 `CALL_PAL` routines, the
interrupt entries (device, interval timer, IPI, software, AST) and the
TB-miss and fault entries. They ran in interpreter builds on the ES40 only,
whenever PAL_BASE was 0x8000, so they served the SRM console as well as
OpenVMS. JIT builds, the other boards and `palcode.vms.nohle = true` ran
the console's PALcode itself. They are gone: every build runs the
console's own PALcode, so the interpreter executes the code the JIT
compiles and `JIT_VERIFY` checks it. `palcode.vms.nohle` is accepted and
ignored, so that old configurations load without a warning.

Why they went (the vmspal audit, 2026-10-22, VMSPAL-01 to -14):

- **Little to gain.** About 4 s of a 39 s OpenVMS boot (10-12%) and 5-7% of
  DCL work, on the interpreter only; the console reached `P00>>>` in the
  same 25-26 s either way. An estimate from `test/vms/vms_ab.py` (removed
  with them: one binary, the two arms differing only by
  `palcode.vms.nohle`, 13 runs of 512 MB and one CPU on a host shared with
  other work), not a `perf_ab.py` result. Of the instructions a boot, a DCL workload and SHUTDOWN execute,
  3.3% were the PALcode doing what a native routine replaced.
- **Two PALcode paths for one guest.** A guest difference on one lane did
  not reproduce on the other, `JIT_VERIFY` never saw the routines, and
  every change to exceptions, interrupts or the lock flag had to be made
  and reasoned about twice (CPU-01, CPU-02 and CPU-15 each were). They were
  tied to one PALcode build by its absolute addresses, its scratch layout
  and a copy of its IER table: the DS20E's and DS20L's consoles put other
  builds at 0x8000, and OpenVMS hung or bugchecked on those boards until
  their rows were given the real PALcode.
- **Their record.** Every comparison with the PALcode found guest-visible
  bugs.

The last comparison was `test/vms/vmspal_oracle.sh` (in the tree up to
6a24457, with its patch): each native routine run with its effects
recorded, its result kept and undone, then the PALcode run from the same
state and the two compared, with the PALcode as the expected value, about
35 million events on an OpenVMS 8.4 boot with a DCL workload and
SHUTDOWN. On 2026-10-22 (base 932c955) the guest called 47 of the 49
routines, and all but `MTPR_DATFX` agreed in every compared field (PROBER
and PROBEW apart from the PALcode's scratch temporaries); so did the
interrupt and TB-miss entries, apart from those temporaries, from races (an
interrupt arriving while the PALcode ran) and from the first row below.
The first three rows were fixed on 2026-10-23 (5afe3ef); the others were
open when the routines were removed:

| What | Where | Consequence |
| --- | --- | --- |
| After a nested double TB miss the DTB-miss routine took `EXC_ADDR` for `MM_STAT`: a write fault was reported as a read (R5, the MM flag, 0), and a `STQ R31`/`STL R31` to a page that is not valid was dismissed as if it were a load to R31, the store skipped without a fault | `vmspal_ent_dtbm_single` | OpenVMS hit it: 115 wrong R5 values and 17 skipped stores in one boot. ARM 14.3.1 |
| `MTPR_DATFX` wrote the inverse of R16<0> into bit 63 of PCB+0 (the saved KSP) instead of R16<0> into bit 63 of PCB+56 | `vmspal_call_mtpr_datfx` | The process's DAT bit was never set. ARM 13.3.4 |
| `MTPR_FEN` stored R16's low longword at PCB+56, not R16<0> | `vmspal_call_mtpr_fen` | None while OpenVMS passed 0 or 1. ARM 13.3.6 |
| b_irq<0> (the Cchip's error interrupts) and b_irq<5> were never delivered, and DIR<50> stopped the emulator | the native interrupt check | Latent: nothing raises them. The PALcode dispatches all six lines |
| In the console's 1-on-1 mode the I/O-hole check tested VA bit 43 inverted, and one bound differed from the PALcode's table | `vmspal_ent_dtbm_single` | Latent: the console reaches I/O through the superpage |
| A `STx_C` after the native `REI` could still succeed | `vmspal_call_rei` | The ARM clears lock_flag on REI (4.2.4); the PALcode's REI loads its frame with `LDQ`, which fails a later `STx_C` since CPU-01 |
| IPI before the interval timer when both were pending; MCES<7:5> cleared on write; MTPR_SIRR ignored R16 >= 16 | `vmspal_ent_ext_int`, `vmspal_call_mtpr_mces`, `vmspal_call_mtpr_sirr` | None observed |

Before that, a comparison restricted to `REI` (2026-10-01: the native
routine first, its result kept and undone, then the PALcode, compared at
the PALcode's `HW_RET`) found these, all fixed then:

1. From a non-kernel mode, a DTB miss on the first frame load ran the fill
   routine, which uses p4-p7 as scratch, after the old mode had been put in
   p7. The outgoing stack pointer was then saved to PCB+0 (the kernel stack
   slot) and the incoming one loaded from a slot never updated; OpenVMS 8.4
   startup hung after `%STDRV-I-STARTUP`. The PALcode reads the old mode
   only after its loads. `ALPHABOX_VMSPAL_OFF`, which handed single routines
   back to the PALcode, found it by bisection.
2. That same branch (non-kernel to non-kernel, e.g. exec to user) did not
   re-evaluate interrupts, where the PALcode's `HW_MTPR CM` does: an AST
   for the mode entered waited for the next interrupt.
3. The IER table's rows for IPL 30 and 31 enabled performance-counter and
   corrected-read interrupts that the PALcode's table at 0xD00 does not
   (unobservable, neither is ever raised).

A spurious master-PIC interrupt (vector 7) also stopped the emulator in the
native interrupt entry until it was dismissed, as the PALcode does.

Before they went, a check (2026-10-23) that the console's PALcode does not
stall where they did not. Under heavy host load the audit had seen
disk-bound DCL steps stall for a minute or more more often with
`palcode.vms.nohle` than with the routines (5 of 8 runs against 1 of 7).
Repeated with `vms_dcl.py --workload dcl` (512 MB, one CPU): on a host with
nothing added, 2 of 10 runs stalled with the PALcode (one for good, ending
in `-SYSTEM-F-CTRLERR`) and 2 of 3 with the routines (a minute each); with
twelve busy loops added and the two arms run side by side, 1 of 10 with the
PALcode (for good) and none with the routines, apart from one round in
which both boots took about three minutes; the JIT build, which always ran
the PALcode, 0 of 13. The stall comes with and without the routines, and
CPU load does not bring it on, so it is not the PALcode path's
([openvms.md](openvms.md#timings), where it is open). Only runs on the
PALcode have failed to recover from one (2 of 20 here and 3 of 8 in the
audit, against 0 of 20 with the routines): too few to tell from chance.

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
  makes it drop that translation, as a timer check already did. Two
  windows were left, both closed on 2026-10-25:
  1. *A store and its own report were not fenced from each other.* Still
     in its processor's store buffer while a compiler checked, a store
     was missed by the check, and its report read the line's old bit. A
     fence between the store and its read of the bit now makes the two a
     Dekker pair: either the check sees the store, or the store sees the
     bit. It is in `CCodePageMap::note_write`, which every reporting store
     passes (the interpreter's, the JIT's store helpers, `STx_C`,
     `CSystem::WriteMem`), and it is paid only where the page's bit is
     already set. A store that finds the page bit clear while the page is
     being marked is the next window's case: the marker asks every
     processor to drop its cached translations after setting the bit, and
     the writer's `MB` honours that request and counts a write, which pairs
     with the marker's fence and check. For every program that follows the
     ARM's rule below, this gives the guarantee a fence on every store
     would, on hosts that are multi-copy atomic (x86-64 and AArch64, every
     host the JIT runs on). AArch64's inline physical `HW_ST`, which reads
     the page bit before it stores, is the next window's case too.
  2. *A processor that still held the page as writable inline* (cached
     before it became code) stored into it unreported until its next
     timer check or `IMB`, so another processor's `IMB` in that window was
     skipped. The ARM's way to hand another processor new instructions is
     the writer's `MB` (`IMB` if it runs them too), then a flag, then the
     executor's `IMB` (Alpha Architecture Handbook, chapter 5,
     "Multiple-Processor Instruction Stream"; `WMB` is not in that rule).
     So `MB`, and the reserved 18.4800 and 18.4C00 that act as one, now
     fence, then test the processor's page-cache flush request, and with
     one pending drop the cached translations, count a write and fence
     again, so the guest's flag cannot become visible before the count
     (`CAlphaCPU::memory_barrier`). Compiled `MB` tests the request inline
     and calls the helper only when one is pending. The fence before the
     test pairs with the compiler, which asks before its own fence and
     check: either the check sees the writer's stores, or the `MB` sees
     the request. `IC_FLUSH` fences the same way before it reads the map
     (the handbook's Table 5-1 orders `IMB` as `MB`). And `note_code` is
     serialised: a processor that set a page's bit and was preempted
     before it asked the others could otherwise have a second processor
     compile elsewhere in that page, find the bit set, ask nobody and
     publish in between.

  `test/cpu/codepage_mp.py` shows the second window: a writer that stores
  the new code through a translation it cached before the page became
  code, then `MB` and a flag, and an executor that `IMB`s and calls it.
  On 35b8e15 the executor ran the old code in 2681 of 3000 iterations on
  AArch64 and in 2949 on x86-64; now in none, on every lane. The first
  window is a race of nanoseconds, which no test here shows. A device's
  DMA reports before and after its transfer (`CPCIDMAWriteGuard`), not
  between a store and a fence; a guest runs code a device writes only
  after the transfer completes, which orders it. The CPU oracle's
  verdicts are main's on the interpreter, AArch64, `JIT_VERIFY` and x86-64
  lanes for `ev68cb`, `ev7`, `ev8` and `ev8n` (`JIT_VERIFY`: 0
  mismatches); `quiesce_mp.py` and `tpu_mp.py` pass on the four lanes;
  SRM's console log is unchanged on the interpreter, AArch64 and
  `JIT_VERIFY` lanes (116 million compiled blocks, 0 mismatches);
  `smc_test.sh` passes on one processor and on two. Windows 2000 RC2 on two
  processors, OpenVMS 8.4 on the ES40, on the DS20E with two processors
  and on the ES47, Gentoo Linux and Windows XP 64-bit passed
  `guests_check.sh`. The fixes are behind switches still
  (`ALPHABOX_JIT05_MB=0` and `ALPHABOX_JIT05_FENCE=0` restore the old
  paths, [headless.md](headless.md)): `perf_ab.py` measured more than 2%
  on one workload each, on a busy host and two rounds, and the owner
  decides ([performance.md](performance.md), "The code-page map's fences,
  and one translation").
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
  (statistics only). SRM's console log is unchanged on the interpreter,
  AArch64 and `JIT_VERIFY` lanes, `ALPHABOX_JIT_FPTEST` passes, Windows
  2000 RC2 reached its desktop and OpenVMS 8.4 logged in. The data
  helpers' translation, still a copy of `virt2phys`'s fast path then, is
  one routine since 2026-10-25: `CAlphaCPU::virt2phys_fast`
  (`AlphaCPU.hpp`, inline, so no call is added): the address's form, the
  superpages, the TB and the entry's access and fault bits, by each
  row's rules (the native EV8's through `ev8_superpage` and its TB
  groups), with no page-table walk, no fault vectored and no TB fill.
  It returns what it found, and `virt2phys` vectors the rest (on the
  `ev8n` row through `ev8_virt2phys`, which translates again), while
  the helpers (`jit_read`, `jit_read_locked`, `jit_read_vpte`,
  `jit_read_wchk`, `jit_write`, `jit_stc`, and `jit_indirect`'s check of
  a chained jump after an ITB invalidate) bail to the interpreter and
  keep the `JIT_STATS` causes (TB miss, access violation, fault). Two
  now ask what the interpreter asks: `jit_read_vpte` checks the
  address's form and the superpages first (a malformed address could
  have matched the TB entry of its canonical twin), and `jit_indirect`
  checks the execute right and the fault bit in the live mode and
  translates a superpage target rather than leaving it to the
  dispatcher. Group `jitxlate` runs a compiled load through the helper
  on each superpage, a TB hit, a miss, an access violation and a
  fault-on-read, on every row; the other proofs are JIT-05's, above
  (the same branch). `ALPHABOX_JIT08_XLATE=0` selects the old copies
  until the quiet-host measurement the owner asked for on 2026-10-25
  settles the switch ([performance.md](performance.md), "The code-page
  map's fences, and one translation").
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
