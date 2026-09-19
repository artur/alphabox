# What other dynamic recompilers do, in enough detail to build from

A survey made in September 2026 of Dolphin, QEMU TCG, PCSX2, PPSSPP, RPCS3,
box64, FEX-Emu, dynarmic, Rosetta 2, Transitive/QuickTransit and the
published binary-translation literature, kept because the detail is what
makes a technique implementable and the detail is what gets lost.

[performance.md](performance.md) has what *we* measured and what it means
for Alphabox. This file is the outside evidence: what each system does,
where its code is, what it measured, and -- the part worth as much as the
rest -- what it tried and abandoned.

Two standing cautions. Percentage gains from different systems do not
compose; and several of the numbers below are from workloads with nothing
in common with ours, so they indicate direction and rough size, not what we
would get. Where a claim is someone's inference rather than a measurement,
it says so.

## The three places every one of them disagrees with us

Independently, from different architectures and decades:

1. **Nobody validates a block link against a global counter.** Links are
   valid until the target block is *destroyed*, and destruction unpatches
   them through a reverse index.
2. **Nobody consults a data structure on a link hit.** They patch a direct
   branch into the compiled code.
3. **Nobody uses a per-site inline cache for indirect branches.** They use
   either the host's own return predictor or one large hashed table.

## Block linking

### How the identity is chosen so links never need an epoch

**Dolphin** looks a block up by `(effectiveAddress, feature_flags)`, where
`feature_flags` encodes the MSR bits that change how code is translated. The
flags are part of the dispatcher's index, part of the value pushed for the
return check, and baked into the compiled code. A link from A to B is
therefore unconditionally valid while B exists: if A is running, the machine
is already in A's mode, and B was compiled for that mode. The obligation
taken on in exchange is that **any instruction that can change the key must
end the block** (`mtmsr`/`rfi` are `FL_ENDBLOCK`).
`Source/Core/Core/PowerPC/JitCommon/JitCache.cpp`.

**QEMU** gets there differently: a translation block is keyed by its
*physical* PC with no address-space identifier at all, and `goto_tb`
chaining is restricted to targets on the same guest page
(`translator_use_goto_tb()` in `accel/tcg/translator.c`). Same page means
the target's physical address is pinned by the source's own physical page,
so the chain cannot be invalidated by any MMU change -- only by the code
itself changing. Note QEMU still throws its *software TLB* away on an
address-space change (`target/alpha/sys_helper.c`: `helper_tbia` →
`tlb_flush`); it solves the chaining half only.

Alphabox already keys blocks by `(virtual PC, ASN, mode)`, which is
Dolphin's shape. What we lack is the reverse index below.

### The reverse index that replaces the epoch

QEMU keeps, per block, a singly linked list of its *incoming* jumps, with
the slot number stashed in the low bit of the pointer:

```c
static inline void tb_add_jump(TranslationBlock *tb, int n, TranslationBlock *tb_next)
{
    tb_set_jmp_target(tb, n, (uintptr_t)tb_next->tc.ptr);
    tb->jmp_list_next[n] = tb_next->jmp_list_head;
    tb_next->jmp_list_head = (uintptr_t)tb | n;
}
```

Invalidating a block then walks that list and repoints every inbound jump at
its own reset address (`tb_jmp_unlink`, `tb_reset_jump`, in
`accel/tcg/tb-maint.c`). A global `tb_flush` happens only when the code
buffer fills. Dolphin's equivalent is a `links_to` map from exit address to
the set of source blocks.

### Patching the branch instead of caching the successor

Every system here patches. The shapes differ only in the mechanics:

- **Dolphin (AArch64)** reserves a fixed-size slot at each exit
  (`BLOCK_LINK_SIZE`, padded with `BRK` so it stays re-patchable). Linked, it
  becomes a direct branch *whose condition is the cycle-budget test*, because
  a `SUBS` ran just before it:

  ```cpp
  s64 block_distance = ((s64)dest->normalEntry - (s64)emit.GetCodePtr()) >> 2;
  if (block_distance >= -0x40000 && block_distance <= 0x3FFFF) {
    emit.B(CC_GT, dest->normalEntry);   // budget left: straight into the next block
    emit.B(source.exitFarcode);         // else: farcode, then timing
  }
  ```

  A taken link costs **one conditional branch** -- no load, no compare, no
  indirect jump. Destroying a block writes `BRK` over the entry point only
  ("we might still be within this block") and defers freeing the code range
  to the next codegen. `JitArm64/JitArm64Cache.cpp`.
- **PCSX2** emits `js rel32` and patches the displacement, recording sites in
  a `std::multimap<u32 startpc, uptr patch_site>`; when a target finally
  compiles, every recorded site is patched. `pcsx2/x86/BaseblockEx.cpp`.
- **PPSSPP** reserves `MIN_BLOCK_EXIT_LEN = 4` bytes, finds incoming edges
  through `std::unordered_multimap<u32,int> linksTo_`, and patches by
  constructing a throwaway emitter at the offset.
- **RPCS3** uses a 16-byte patchpoint overwritten atomically; on AArch64
  `ldr x9,#8 / br x9 / <u64>` with `ISB`/`DSB ISH`.

Practical constraints for us: AArch64's `B` reaches ±128 MB, which caps how
large a code arena can be before links stop being one instruction (Dolphin
handles the out-of-range case by inverting the condition and using a full
branch). On Apple Silicon every patch needs
`pthread_jit_write_protect_np` around it plus an icache flush -- Dolphin
wraps each one in `ScopedJITPageWriteAndNoExecute`.

## Indirect branches and returns

### Returns: validate in software, predict in hardware

This is the single best-documented win in the survey, and its point is
finer than "keep a shadow stack". A guest return compiled as anything other
than a host return is an indirect branch. Transitive's patent
(US 8,893,100) states the consequence plainly: it is *"very difficult for
target hardware to effectively predict the addresses of indirect branches,
and not surprisingly such a solution can perform very poorly"*. So: emit a
real host call and a real host return, and demote the software stack to a
*validator*.

Dolphin's AArch64 mechanics, which are the clearest to copy. At a guest
call exit, push a 16-byte frame and make the linked form end in `BL` (it
must be last, so the host return address matches what was pushed):

```cpp
MOVI2R(X1, feature_flags << 32 | exit_address_after_return);
ADR(X0, adr_offset);                       // host address just past the link slot
STP(IndexType::Pre, X0, reg_to_push, SP, -16);
```

At a guest return, pop, compare target *and* mode in one 64-bit compare, and
on a match do a real `RET`:

```cpp
LDP(IndexType::Post, X2, X1, SP, 16);
ORRI2R(X0, DISPATCHER_PC, feature_flags << 32, X0);
CMP(X1, X0);
FixupBranch no_match = B(CC_NEQ);
DoDownCount();
RET(X2);                                   // the host return stack predicts this
SetJumpTarget(no_match);
ResetStack(); DoDownCount(); B(dispatcher);
```

Three details that are not obvious and cost real debugging:

1. **Unbalanced guest calls overflow the host stack.** Dolphin puts a 64 KiB
   guard page 192 KiB into the thread stack, catches the SIGSEGV in
   `JitBase::HandleStackFault`, and *permanently disables* the optimisation
   for that session ("BLR cache disabled due to excessive BL in the emulated
   program").
2. **The host link register must be locked** while a call has been followed
   but its return not yet compiled. Skipping this shipped a real crash class
   in Dolphin and was fixed seven years later.
3. **Every exit that is not a matching return must reset the stack pointer**,
   including the dispatcher and the cache-clear path.

Measured: Dolphin **8% across most games** (blog, September 2014).
MAMBO-X64 on SPEC CPU2006 versus native: basic blocks alone **11.1%
overhead**, hardware-assisted returns alone **4.1%**, trace formation alone
**2.9%**, both together **1.0% faster than native** (PLDI 2016). box64's
`BOX64_DYNAREC_CALLRET` (default 2) claims "more than 10%". Rosetta 2 does
the same thing and names the memory region *Rosetta Return Stack*.

box64 records one negative alongside it: use `BL` only at *real* guest
calls, never for ordinary block-to-block jumps, because that "breaks the
return-address-stack prediction on every block-to-block jump, which is not
worth it".

**Alpha makes the classification free.** Opcode `0x1A` encodes the kind in
`disp<15:14>` -- JMP, JSR, RET, JSR_COROUTINE -- because a real 21264 has a
hardware return stack driven by exactly those bits. No heuristic is needed.
There is also a 14-bit hint field carrying a static prediction of the
target's low bits; DEC compilers and PALcode set it, but the Microsoft
compiler leaves it zero, so measure coverage per guest before relying on it.

### Everything else indirect: one big hashed table, probed inline

QEMU's 2026 series (Matt Turner) is the most directly relevant measurement
in this document, because **its benchmark is an Alpha guest** -- `qemu-alpha`
running an emulated Alpha gcc compiling SQLite. Its thesis matches ours:
most of what a JIT executes is fixed overhead around blocks.

The winning patch replaces a helper call at every indirect exit with an
inlined probe of a global PC-hashed table:

1. hash the destination PC into a 64 K-entry array;
2. load `entry.pc`, compare, branch out on mismatch (checked *first*: "on a
   hash miss the pc is the field most likely to differ");
3. load `entry.tb`, branch out if NULL;
4. one 64-bit load covering `flags` and `cflags` together (adjacent
   `uint32_t`s combined with `deposit64()`), compare;
5. compare `cs_base`;
6. jump to `tb->tc.ptr`.

Each exit emits **its own** final jump rather than branching to a shared
stub, to avoid a temp being spilled and reloaded on every dispatch.

Measured for that patch alone: **−34.67% instructions, −25.94% wall clock**,
hit rate **95.8%**, the lookup helper falling from **31.01% of profile
samples to 0.35%**, and L1-icache load misses **−39.0%**. Enlarging the
table from 4 K to 64 K entries was separately worth **−5.92% instructions,
−8.71% wall**. Cumulative for the nine-patch series: instructions −50.26%,
wall −41.96%.

dynarmic does the same with a 2^20-entry × 16-byte table hashed by CRC32
over its "location descriptor" -- the exact analogue of our
`(PC, ASN, mode)` key, and one 64-bit compare covers all three if packed.
It writes the entry *before* the lookup and fills in the code pointer after,
so there is no separate insert path.

Rosetta 2's chain is hash map → red-black tree → JIT, with no inline caching
at any level (`IndirectBranchHashMap.cpp`, `TwoLevelOffsetMap.cpp`).

### Making the table survive a context switch

Hong et al., *"Efficient and Retargetable Dynamic Binary Translation on
Multicores"* (TACO 2016), is the paper written for our situation: a
full-system DBT whose guest has software-managed address spaces. Keying the
indirect-branch table on guest virtual address alone forces a flush at every
guest context switch, which both costs time and caps the table size worth
affording. Their fix stores the guest *physical* page alongside the virtual
one in a small instruction-side TLB, and both addresses in the table. A
context switch then flushes only the little iTLB; the big table survives,
because a cross-page transfer cannot be taken until the iTLB revalidates it.

That is what let them go from 2^13 to 2^16 entries, with measured hit rates
of 94.33% (worst, mcf) to 99.99%, and 95.62% on an Android boot.

## Cross-page block linking

Our compiler truncates a block at the 8 KB guest page boundary
(`src/jit/jitengine.cpp`, the `page_end` test), for the correct reason: past
it the next instruction's physical address need not be `phys + 4`. QEMU
refuses to chain across a page for the same reason. Hong et al. instead
*guard* the crossing, and the guard is three instructions -- load the page
number from the iTLB, compare it with the target's page, conditional branch
-- with no index computation, because the target address is known at
translation time.

They also measured **lazy** checking (the guard at the end of the source
block, only on edges that actually cross) against **proactive** checking
(a guard at every block's entry, as Embra and PinOS do): 1.38× versus 1.34×
on SPEC, 1.16× versus 1.12× on Android.

Cross-page linking alone: **1.19× on SPEC CINT2006**, 1.06× on Android, with
guard hit rates of 97.9-99.98%. QEMU's user-mode version of the same idea
measured −4.84% wall.

Alpha's software TLB is what makes this legitimate for us and not for QEMU's
hardware-MMU guests: the guest is architecturally obliged to execute
`TBIS`/`TBIA` PALcode whenever a mapping changes, and an address-space
switch needs no TBI at all -- which is the entire point of ASNs. That gives
us a single invalidation funnel QEMU has only in user mode.

## The memory path

QEMU's software TLB is the closest analogue to our data page cache, and two
of its ideas are worth taking whatever else we do.

**One entry holds every comparator.** `CPUTLBEntry` is 32 bytes:
`addr_read`, `addr_write`, `addr_code`, `addend`. A store needs no
permission check, because a read-only page simply never matches
`addr_write`; disabling a permission is `address = -1`, with no branch
anywhere. `addend = host_ram_ptr + xlat - guest_page_addr`, so the access is
`host = guest_vaddr + addend` with no masking -- the same bias we adopted.
`mask` is pre-shifted so the index computation yields a byte offset, and
`mask` and `table` are an aligned pair so the backend loads both with one
`LDP`.

**One compare rejects everything.** Bits [9:6] of the stored comparator are
free -- above the largest alignment, below the smallest page -- and hold
flags: invalid, MMIO, not-dirty, watchpoint, byte-swap, discard-write. The
stored tag is `page_vaddr | flags`; the generated code computes
`addr & (TARGET_PAGE_MASK | a_mask)`. A single `CMP` therefore settles wrong
page, wrong permission, MMIO, dirty-tracking **and misalignment** at once.
For an access whose alignment requirement is smaller than its size, the
compared value is `addr + (size_mask - align_mask)` -- the last byte -- so
page-crossing is caught by that same compare.

The whole AArch64 fast path is 8 instructions plus the access:

```
LDP  tmp0, tmp1, [env, #tlb_mask_table_ofs]      ; mask, table
AND  tmp0, tmp0, addr, LSR #(PAGE_BITS - ENTRY_BITS)
ADD  tmp1, tmp1, tmp0
LDR  tmp0, [tmp1, #off(comparator)]
LDR  tmp1, [tmp1, #off(addend)]
AND  tmp2, addr_adj, #(PAGE_MASK | a_mask)
CMP  tmp0, tmp2
B.NE slow
```

**Victim buffer.** Eight fully-associative entries behind the main table;
on a miss, scan them before the expensive refill and three-way swap a hit
back into the main slot. Measured in QEMU system mode at **+10.7% average
and +25.4% peak on SPECINT2006** with no regression on any benchmark (Xin
Tong, 2014); a later measurement of the same idea reports +11.9%/+26.2%.
It costs nothing in emitted code -- it lives entirely in the refill helper.

**Dynamic resizing.** Track peak occupancy over a ~100 ms window; above a
70% use rate, double; below 30% at window end, shrink. Cota and Carloni,
CGO 2017. Note the *negative* result attached to it: keying the decision on
the use rate measured at flush time "resulted in a slowdown on average" --
the recent-past window is what works.

**PCSX2's bias-plus-sign-bit encoding** is a neat trick with a caveat. Its
table entry is `host_ptr - vaddr` for RAM and `(handler_id | 1<<63) + paddr
- vaddr` for MMIO, so one `add` reconstructs both cases and the *sign* of
the result discriminates -- no tag compare, no separate MMIO test. It works
only because the table is a complete map of a 4 GB space and because the
guest has no misaligned accesses. Ours is a cache, not a map, so we keep the
tag compare; the bias half we already have.

### Fastmem, and why not in general

Mapping guest memory into the host address space so an access is one
instruction, with a SIGSEGV handler backpatching the sites that turn out to
be MMIO. Dolphin's handler is the clean version -- no instruction decoding,
just an ordered map from code range to handler:

```cpp
auto it = m_fault_to_handler.upper_bound(pc);   // keyed by END of each fast region
if (it == m_fault_to_handler.end()) return false;
if (pc < it->second.fast_access_code) return false;
// overwrite the fast sequence with a call to the slow one, pad with NOPs,
// erase the entry (one-shot), flush icache, resume at the patched site
```

Dolphin extended this to page-table-mapped memory in February 2026 by
noticing that the guest *must* announce every page-table change (`tlbie`),
then diffing the guest page table against a shadow copy and issuing
`mmap`/`mprotect`. Their own ledger is mixed: Rogue Squadron III doubled,
Spider-Man 2 *lost* performance to the tracking overhead, an early
implementation was slower than doing nothing, and the accuracy cost is
explicit -- "the TLB is now effectively infinitely large".

For Alphabox the general case is a bad trade, for four independent reasons:
our software page cache already costs ~4 instructions rather than the
~1000-instruction page walk Dolphin was escaping; ASNs mean several address
spaces are live at once, so one host view is not enough; the backpatch is
one-shot, so a site pessimised by one process's layout stays slow forever;
and 8 KB Alpha pages against 16 KB Apple Silicon host pages is a hard
mismatch. PCSX2 documents the last problem precisely -- it only maps a page
when all four consecutive guest pages are contiguous and aligned.

**The subset that does fit**: Alpha's superpage/KSEG addresses translate to
physical by masking, never change, and are never per-process. A single
mapped physical view plus backpatching, for those accesses only, would give
a one-instruction load for kernel and PALcode traffic with no TLB tracking
and no accuracy loss.

## Code layout and block length

**Farcode.** Two arenas; everything cold -- slow memory paths, exception
exits, deopt stubs, the "budget expired" half of every link -- is emitted
into a far arena so the hot arena stays dense. Dolphin's measurement is
striking because the cause was not what they expected: MMU emulation was
slow not from the MMU work but because "the quantity of this code was so
enormous that it overflowed CPU caches". Result: Rogue Leader up to **twice
as fast**, other MMU titles **30% or more**, and **~6% even on non-MMU
titles**. Low risk, and it matters more the shorter the chains are.

**Branch following.** Dolphin extends a block past an unconditional branch
at analysis time, capped at `BRANCH_FOLLOWING_THRESHOLD = 2`, and follows a
return only when the matching call was itself followed. Enabled by default
in 5.0-2178 -- and **disabled per-game nine years later** for Rogue Squadron
II, because bigger blocks made invalidation and re-JIT stalls worse. box64
has the aggressive version (`BIGBLOCK`, `FORWARD=128`, plus `SEP` for
secondary entry points into an existing block).

Worth noting how little of Dolphin's chain length comes from this: it
follows at most two branches. Its chains are long because **linking works**,
not because blocks are long.

## Tried and abandoned

Kept because a negative result is cheaper to read than to reproduce.

| What | Who | Outcome |
| --- | --- | --- |
| Trace/IL tier | Dolphin | JitIL **deleted** (2017): "never could match the performance and compatibility of the regular JIT" |
| Tracing JIT | Mozilla | TraceMonkey **deleted**, 67,643 lines; a method JIT with type inference was faster on average, and being knocked off trace "happens a lot - more than anyone expected" |
| Trace JIT vs method JIT | IBM J9 (CGO 2011) | **95.5%** of the method JIT it was retrofitted from; +10.5% code, +27% compile time; won on 1 of 13 benchmarks, lost >15% on 3 |
| Trace selection footprint | IBM (OOPSLA 2011) | each basic block duplicated **13×** across traces; 40% of traces short-lived; removing 69% of the footprint cost nothing and *gained* 10% on a large app via fewer L2 misses |
| Trace-based HotSpot | JKU (2013) | 67% / 85% / 93% of the *optimizing* compiler on SPECjbb2005 / SPECjvm2008 / DaCapo; the positive result was only against the non-optimizing one |
| SPU "Mega" block size as default | RPCS3 | merged Nov 2025, **reverted** Dec 2025 for correctness |
| "Giga" whole-call-graph inlining | RPCS3 | merged with known boot crashes and a 10+ FPS regression |
| IR JIT replacing hand-written JITs | PPSSPP | "at parity or a bit faster"; the author predicted "the gains will probably be tiny" |
| Several IR passes | PPSSPP | written (~700 lines) and left out of the enabled list |
| Most replacement functions | PPSSPP | `sinf/cosf/atanf/sqrtf/strcmp/...` all disabled; only the bulk-memory ones stayed |
| Standalone fastmem | PCSX2 | abandoned unmerged; "5-10% depending on the game, often <5%" |
| ARM64 recompiler | PCSX2 | closed unmerged; PCSX2 has no ARM64 EE/IOP/VU recompiler |
| TLB resize keyed at flush time | Cota/Hong line | "resulted in a slowdown on average" |
| Branch following | Dolphin | disabled per-game where invalidation dominated |
| Traces preempting chaining | **Alphabox** | our own note in `jitengine.cpp`: a net loss |

Two techniques that simply do not apply to us: **Apple's TSO bit** (Alpha's
memory model is *weaker* than AArch64's -- we want plain loads, and turning
TSO on would only cost us), and **host-MMU shadow mapping** of guest virtual
space (page-size mismatch, per-ASN multiplication, and MMIO still needs a
trap).

## Where to start

Ordered by measured gain over effort, for our measurements
(see [performance.md](performance.md)):

1. **Inline the block-lookup probe at exits**, backed by a large hashed
   table keyed on `(PC, ASN, mode)`. Closest measured analogue: −25.94% wall
   on an Alpha guest.
2. **Software return stack** on Alpha's `JSR`/`RET` function field, wired so
   a miss falls into (1). Measured analogues: 8%, and 11.1% → 4.1% overhead.
3. **Patch direct branches** instead of walking a successor array, with an
   incoming-jump list so destruction unpatches. This is also what removes
   the global epoch, which our own instrumentation blames for 99.92% of
   link misses.
4. **Cross-page linking** with the three-instruction guard, if chain length
   still looks short on the workloads we care about.
5. **Victim buffer** behind the data page cache -- no emitter work at all.

## Sources

Dolphin: `Source/Core/Core/PowerPC/` (`JitCommon/JitCache.cpp`,
`JitArm64/JitArm64Cache.cpp`, `JitArm64/Jit.cpp`,
`JitArm64/JitArm64_BackPatch.cpp`, `JitCommon/JitBase.cpp`,
`PPCAnalyst.cpp`), and the progress reports of 2014-07, 2014-09, 2017-01,
2020-07, 2021-06/07, 2023-08/10 and 2026-03 at dolphin-emu.org/blog.

QEMU: `include/exec/tlb-common.h`, `include/exec/tlb-flags.h`,
`accel/tcg/cputlb.c`, `accel/tcg/cpu-exec.c`, `accel/tcg/tb-maint.c`,
`accel/tcg/tb-jmp-cache.h`, `accel/tcg/translator.c`,
`tcg/aarch64/tcg-target.c.inc`, `tcg/i386/tcg-target.c.inc`. Matt Turner's
Aug 2026 series "accel/tcg: cut per-block dispatch overhead" on qemu-devel.
Victim TLB: qemu-devel 2014-01. Cota & Carloni, CGO 2017 and VEE 2019.

Others: PCSX2 `pcsx2/vtlb.h`, `pcsx2/x86/BaseblockEx.cpp`,
`pcsx2/x86/ix86-32/recVTLB.cpp`; PPSSPP `Core/MIPS/IR/`,
`Core/MIPS/ARM64/`; RPCS3 `rpcs3/Emu/Cell/SPUCommonRecompiler.cpp`,
`PPUTranslator.cpp`; dynarmic `src/dynarmic/backend/x64/`; box64
`docs/box64.pod` and `src/dynarec/arm64/`; FEX "The scourge of x86
emulation".

Papers: Hong et al., TACO 2016 (cross-page block linking, IBTC with
physical tagging). d'Antras et al., MAMBO-X64, PLDI 2016 (hardware-assisted
returns, ReTrace). Inoue et al., CGO 2011 and OOPSLA 2011/2012 (trace JIT
negative results). Häubl et al., PPPJ 2011 and CLSS 2013 (trace-based
HotSpot). Transitive/IBM patents US 2004/0255279 A1 (group blocks,
isoblocks) and US 8,893,100 B2 (return address optimisation). Dougall
Johnson, "Why is Rosetta 2 fast?".
