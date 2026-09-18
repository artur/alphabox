# How faithful the processor is

Alphabox emulates an Alpha 21264 of the EV68CB kind. This file records where
it does not match that processor, because a list of known divergences is
worth more than a belief that there are none: it says what a guest can and
cannot be used to test here, and it stops the same question being asked
twice.

Everything below was established against the *Alpha Architecture Reference
Manual*, 4th edition (ARM) and the 21264/EV68CB *Hardware Reference Manual*
(HRM), with section numbers, and — where it could be — demonstrated by
running Alpha code inside the emulator rather than by reading alone.

## How to check the processor yourself

- **Interpreter against JIT**: build with `-DCMAKE_CXX_FLAGS="-DJIT_VERIFY"`
  and boot anything. Every compiled block is re-run by the interpreter and
  compared; a clean SRM boot is about 130 million blocks. This finds
  disagreements between the two engines, not disagreements with the
  architecture — both can be wrong together.
- **The floating-point self-test**: `ALPHABOX_JIT_FPTEST=1` on a `JIT_VERIFY`
  build checks the JIT's inline IEEE operations against the interpreter over
  8.5 million cases at startup.
- **Guest programs**: `test/tools/bench_image.py` writes a bootable image
  from a list of instruction words, and `srm_probe.sh` boots one
  (`FLOPPY=<image> EXIT_ON_HALT=1 CMDS="boot dva0" AFTER=wait-exit`). A
  program ending in `CALL_PAL 0` halts the machine; leave `EXIT_ON_HALT`
  off and the console comes back, so results stored in memory can be read
  with `examine -p -q <address>`. This is how most of the findings below
  were shown rather than argued.
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

What is lost is everything built on the trap. OpenVMS PALcode fixes an
unaligned access up and the DAT bit decides whether the OS hears about it
(ARM 14.3.3.1); Tru64 reports through `entUna` and offers `uac`; NetBSD
counts them and panics for a kernel-mode one. Under Alphabox all of those
report nothing, for ever — so **Alphabox cannot be used to find misaligned
accesses in a guest**, and a clean bill of health from it means nothing.

The page-crossing case that *is* implemented is worse than either
alternative: it fires on a condition the architecture does not define, and
whether it fires depends on what happens to be resident in the translation
buffer (an 8 KB fallback, or a granularity-hint page's mask of up to 4 MB),
so a loop over a misaligned buffer can run for a thousand iterations and
trap on the one that straddles a page. The byte-at-a-time page-crossing
code in `READ_VIRT`/`WRITE_VIRT` and friends is unreachable: `pbc` is
declared false and never assigned.

Deciding this properly means choosing one of: trap always (faithful, and a
guest's fixup handler finally gets exercised), or keep the shortcut and drop
the page-crossing special case (honest, fast, and consistently wrong in one
direction).

Two parts of this *are* fixed: a misaligned `LDx_L`/`STx_C` now takes the
trap the architecture requires — it used to hand a misaligned address to a
host atomic compare-and-swap, which is a bus error that killed the emulator
— and the alignment trap no longer claims an access violation in `MM_STAT`.

### FPCR[UNDZ] is never read

An underflow with `UNFD` set is flushed to zero. The architecture wants a
trap when `UNDZ` is clear, so that software can supply the denormal this
implementation cannot produce (ARM 4.7.2; HRM Table 2-14, "trap to supply a
possible denormal result"). A program that asked for gradual underflow —
Tru64 `-fptm su`, OpenVMS `/IEEE_MODE=DENORM_RESULTS` — gets silent zeros
and its completion handler is never called.

Honouring `UNDZ` makes underflows trap that do not trap today. Changing
trap delivery needs a guest to prove it, which is why this one is written
down rather than fixed.

### Machine checks are never raised

`MCHK` is defined and never used. An unclaimed physical address returns data
from `CSystem::ReadMem` instead of a machine check, so firmware or an OS
probing a bus for a device that is not there sees a plain read, and a
guest's machine-check handling cannot be exercised in Alphabox at all.

### Smaller ones

| What | Where | Consequence |
| --- | --- | --- |
| `VA_FORM` is derived from the VPTE address on a double TB miss, where the HRM says the faulting address | `AlphaCPU.cpp`, `virt2phys` VPTE branch | Latent: shipping PALcode uses VA+PTBR, so nothing observed. New firmware reading `VA_FORM` there would compute the wrong page-table address |
| `lock_flag` is not cleared by `CALL_PAL REI` | `AlphaCPU_vmspal.cpp`, `cpu_pal.hpp` | Every real preemption reaches REI through an exception, which does clear it, so only a deliberate `LDx_L; REI; STx_C` sees it |
| One `REI` branch does not set `check_int` | `vmspal_call_rei` | An AST that became deliverable by that REI waits for an unrelated event. Interpreter lanes only — JIT builds use native PALcode |
| `MT_FPCR` does not take its synchronous trap (HRM 6.7.3) | `cpu_fp_operate.hpp` | PALcode bookkeeping hung on that trap never runs. Nothing we boot needs it |
| Reserved barrier code point `18.4C00` is OPCDEC, where ARM App. C.13 says it must act as `MB` | `AlphaCPU.cpp` opcode 0x18 | Only hand-written barrier tests |
| `EXC_SUM[63:48]` is not the sign extension of `SET_IOV` | `AlphaCPU_ieeefloat.cpp` | PALcode testing EXC_SUM's sign as a fast "did IOV happen" check. We also set FPCR ourselves, so nothing reads it |
| A VAX *dirty zero* operand does not raise invalid operation | `AlphaCPU_vaxfloat.cpp` | VMS code that plants dirty zeros to catch uninitialised data |
| Performance-counter, corrected-read and serial-line interrupts are never raised | `ISUM`, `int_deliverable` | Nothing on this machine uses them |
| The instruction cache is 2 MB with 2 KB lines; a real EV6 has 64 KB with 64-byte lines | `AlphaCPU.hpp` | Architecturally legal — a virtual I-cache need not be coherent, and `IMB` works — but a guest that gets away with a missing `IMB` on real hardware can fail here, because stale bytes live far longer |
| `IMB` also invalidates the ITB, which the architecture does not require | `DO_IMB` | Performance only, and it can hide a guest's missing `IMB` after a page remap |
| `AMASK` is advertisement, not a gate: the CIX/MVI/BWX/FIX instructions execute whatever a CPU row claims | `CpuModels.cpp`, `cpu_misc.hpp` | Correct for the only row that exists (EV68CB implements all of them). It matters the day a row is added for a part that does not |

## What was checked and found correct

Recorded so that it does not get re-litigated. All of this was verified
against the manuals, and most of it by running code:

- **Integer arithmetic, logic and bit manipulation** (opcodes 0x10–0x13,
  0x1C): every function code, against handbook pseudocode, over ~3.5 million
  operand combinations including all six `/V` overflow forms, the byte
  extract/insert/mask family's mod-64 shift edge, the literal form, and
  destination aliasing. Zero disagreements, and `JIT_VERIFY` runs of 144
  million blocks on AArch64 and 117.5 million on x86-64 found none either.
- **Branch and jump**: displacements at both extremes (+1048575 and
  −1048576, checked by landing them), `JMP` with `Ra == Rb`, hint bits
  correctly ignored, FP branches treating `0x8000000000000000` as zero.
- **PALcode and privileged instructions**: entry offsets match HRM Table
  6-8 (including `DTBM_DOUBLE_3`/`_4` selected by `I_CTL[VA_48]`), the
  `CALL_PAL` entry PC assembly, the R23 linkage including its PALmode bit,
  PALRES gating, `HW_LD`/`HW_ST` type decoding, `HW_RET`, and PALshadow
  being R4–R7 and R20–R23 under SDE.
- **The exception path**: `EXC_ADDR` is the triggering instruction for a
  fault *or a synchronous trap* (HRM 5.2.7 — the +4 an OpenVMS exception
  frame needs is PALcode's job), `EXC_SUM` and `MM_STAT` bit layouts, AST
  delivery only below IPL 2, interrupts blocked in PALmode, `lock_flag`
  cleared on every exception except the transparent TB fills, OPCDEC
  coverage.
- **The JIT's bail protocol**: every fault-capable helper probes without
  side effects and returns before touching memory, a register or the lock
  flag, so a fault is taken once, by the interpreter, with the instruction
  not half-executed.
- **Floating point**: which rounding mode applies (the instruction's field,
  with FPCR consulted only for `/D`), the IEEE and VAX trap-mode tables
  being mirror images (a classic emulator bug, and we decode both), sticky
  bits set independently of trap enables, NaN propagation and quieting,
  `CMPTxx` signalling rules, `DNZ`, and the S-format load/store mapping.

## The rule this audit produced

Two references are needed, not one. The handbook says what should happen;
a guest says whether the emulator can still run. Today's round found three
bugs that a guest would never have shown (a NaN returned for an overflow
under chopped rounding, denormals worth half their value, memory barriers
compiled to nothing) and one change that the handbook endorses but a guest
must approve before it ships (`UNDZ`). Neither reference alone would have
been enough.
