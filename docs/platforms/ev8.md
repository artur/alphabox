# EV8 (Alpha 21464) and Tarantula: research packet

The Alpha 21464, code-named EV8 or "Arana", was an eight-wide out-of-order
core with four-way simultaneous multithreading (SMT), meant to follow the
EV7 in Compaq's Marvel servers. Compaq cancelled it on 25 June 2001, late
in development and before tape-out [WIKI]. Tarantula was a vector
extension to the Alpha architecture, "originally planned as a follow-on
candidate to the EV8" [TAR p. 281]; it was cancelled before any
implementation work started [WIKI], and its team moved to Intel with the
rest of the Alpha group [TAR p. 292].

No machine shipped either, so this packet is not a board bring-up. It
collects what was published, says what each source establishes, and lists
what Alphabox would have to decide itself. It serves the project's aim of
showing "what Alpha could have been": where the hardware never existed,
fidelity means following the published design as far as it goes, and
every gap we fill is an **Alphabox decision**, not history.

**Branch**: `docs/ev8` · **Config**: none yet · **Status**: research done
(2026-10-23); the owner chose the path on 2026-10-24 (option A first, then
the native interface, [below](#the-owners-decision-2026-10-24)). No code
yet ([the plan](#6-the-plan)).

Markers used below:

- a citation in brackets is a source fact, with its page;
- **[inference]** is derived from sources but not stated in them;
- **[decision]** is a choice Alphabox would make, proposed for the owner;
- **[conflict]** marks sources, or parts of one source, that disagree.

## The five facts that matter most

1. **The EV8's own design specification survives.** The *21464 Internal
   Design Specification*, Rev 1.1k of 5 January 2001 (Compaq Confidential,
   714 pages), is on bitsavers [IDS]. It gives the internal registers, the
   PALcode interface, the new instructions with their encodings, and the
   AMASK and IMPLVER values. It is a working draft: the chapters on
   initialization, new instructions, system configurations and physical
   addressing are empty placeholders [IDS pp. 583, 641-646].
2. **Identification**: IMPLVER returns 4 and AMASK's feature mask is 0x1F07
   [IDS p. 76]; AMASK bit 10 means SMT [IDS p. 668].
3. **The PALcode interface is new.** The internal register map is new (no
   register in the table below keeps its 21264 index), HW_RET is replaced
   by RET, HW_MFPR/HW_MTPR and
   HW_LD/HW_ST have new field layouts, and the PALcode entry offsets change
   [IDS pp. 477-481, 498-499, 569-575]. The EV7 PALcodes in the Marvel
   console use 21264 indexes throughout ([marvel.md](marvel.md), E1), so
   they cannot run on a faithful EV8 **[inference]**. EV8 is a new core
   family in the sense of [platforms.md](../platforms.md), not a CPU row.
4. **The OS sees four processors per chip.** Each thread processing unit
   (TPU) has its own registers, PC and most IPRs; caches, TBs and the
   branch predictor are shared [IDS p. 46]. TB sharing is controlled by a
   TPU group number: one group per TPU makes the chip behave like four SMP
   processors ("Mode 2, expected to be used by VMS"), one group for all
   four shares address spaces ("Mode 3, Unix") [IDS p. 306]. LDx_ARM and
   QUIESCE let a thread sleep on a memory location [IDS pp. 667-679].
5. **The EV8 was designed for Marvel systems**: its GIO port talks to the
   Marvel CMM, its I/O ASIC is the IO7, and it adopts the 21364's coherence
   protocol [IDS pp. 58, 457, 471-476]. But its physical address is 48 bits
   (I/O at PA<47>) against the EV7's 44, so the address map Alphabox's
   Marvel model uses would change [IDS pp. 43, 298].

Tarantula is known only from its ISCA 2002 paper [TAR]: 32 vector
registers of 128 64-bit elements, vector length, stride and mask
registers, five instruction classes and their semantics. **No Tarantula
encoding, register number, PALcode support or detection method was ever
published.**

## The processor

| | |
| --- | --- |
| Part, code name | Alpha 21464, EV8, "Arana" [IDS p. 1] (RWT: Spanish for spider [RWT-SM p. 2]) |
| Status | cancelled 25 June 2001, not taped out [WIKI]; "canceled in June 2001 in a late phase of development" [BP p. 1] |
| Process, clock | 0.125 um SOI, copper, low-k [MPF99 slide 4]; the IDS gives 1.4 GHz [IDS p. 42] **[conflict]**: MPF99 1.2-2.0 GHz, MPR 1.7 GHz [MPR99b p. 3], the ISSCC 2002 paper 1.8 GHz [RWT-SM p. 3], the Tarantula simulations 2.13 GHz [TAR p. 289] |
| Threads | four TPUs [IDS p. 46] |
| Width | fetch 16, map, issue and retire 8 per cycle; 8 integer, 4 FP, 4 memory, 4 multimedia operations [IDS pp. 41-42]; 256 instructions in flight, 128-entry issue queue [IDS p. 42] |
| Registers | one 512-entry register file for integer and FP [IDS p. 42]: 4 TPUs x 64 architected plus 256 rename registers [RWT-SM p. 2] |
| L1 | Icache 64 KB, 2-way pseudo set associative; Dcache 64 KB, 2-way; 64-byte blocks [IDS p. 42] |
| L2 | Scache 3 MB, 6-way, 64-byte blocks, write-back [IDS pp. 42-43] **[conflict]**: the Tarantula paper's EV8 baseline is 4 MB, 8-way [TAR pp. 284, 289] |
| TBs | independent 128-entry fully associative ITB and DTB, superpages for kernel maps [IDS p. 43] |
| Addresses | 52-bit virtual, 48-bit physical, 8-bit ASN, 8 KB and 64 KB pages [IDS p. 43] |
| Memory | two RDRAM ports of four channels each, up to 32 GB per processor, 12.8 GB/s [IDS p. 43] |
| Multiprocessing | up to 512 processors, directory CC-NUMA, glueless torus, an optional I/O node per processor [IDS p. 43] |
| Network | N, S, E, W and I/O router ports; "in 21364 mode" each port 3.2 GB/s, "in 21464 mode" 4.8 GB/s [IDS p. 44] |
| Die | about 250 million transistors, about 1100 signal pins [MPF99 slide 4]; 420 mm2 [RWT-SM p. 3] |

## Sources

All files are in `lab/datasheets/ev8-tarantula/` (git-excluded), each with
a `.txt` beside it; the rows are in `lab/datasheets/INDEX.md`.

| Key | Document | Pages cited as |
| --- | --- | --- |
| IDS | *21464 Internal Design Specification*, Rev 1.1k, 5 Jan 2001, Compaq Confidential. bitsavers `pdf/dec/alpha/21464/`. A scan with OCR text | PDF page (the printed page is chapter-page, e.g. 2-6) |
| TAR | R. Espasa, F. Ardanaz, J. Emer, S. Felix, J. Gago, R. Gramunt, I. Hernandez, T. Juan, G. Lowney, M. Mattina, A. Seznec, "Tarantula: A Vector Extension to the Alpha Architecture", ISCA 2002, pp. 281-292, DOI 10.1109/ISCA.2002.1003586 (J. Emer's copy) | printed page (PDF page + 280) |
| MPF99 | J. Emer, "Simultaneous Multithreading: Multiplying Alpha Performance", Microprocessor Forum, October 1999 (slides) | slide |
| PACT01 | J. Emer, "EV8: The Post-Ultimate Alpha", PACT 2001 keynote (slides) | slide |
| MPR99a | K. Diefendorff, "Compaq Chooses SMT for Alpha", Microprocessor Report 13(16), 6 Dec 1999 | page |
| MPR99b | Microprocessor Report 13(15), 15 Nov 1999, "Alpha 21464 Targets 1.7 GHz in 2003" | PDF page |
| BP | A. Seznec, S. Felix, V. Krishnan, Y. Sazeides, "Design Tradeoffs for the Alpha EV8 Conditional Branch Predictor", ISCA 2002 (preprint) | preprint page |
| PAT-Q | US 6,493,741 B1, "Method and apparatus to quiesce a portion of a simultaneous multithreaded central processing unit" (Compaq, filed 1 Oct 1999) | column |
| PAT-S | US 6,591,345 B1, strided vectors in a bank-interleaved cache (Seznec; Compaq/HP, filed 28 Nov 2000) | column |
| PAT-I | US 7,627,735 B2, "Implementing vector memory operations" (the Tarantula team at Intel, filed 2005) | column |
| RWT1, RWT2, RWT-SM | P. DeMone, Real World Technologies: "Alpha EV8 (Part 1)" (13 Dec 2000), "(Part 2): Simultaneous Multi-Threat" (26 Dec 2000), "The Spider and the Mountain" (18 Feb 2002, on the ISSCC 2002 paper) | web page |
| WIKI | Wikipedia, "Alpha 21464", fetched 2026-10-23 | |
| ARM | *Alpha Architecture Reference Manual*, 4th ed., Jan 2002 (`lab/datasheets/alpha-cpu/`) | PDF page |
| GCC | `gcc/config/alpha/driver-alpha.cc` (GCC master, fetched 2026-10-23) | |

Not obtained (full citations in INDEX.md, "Not found"): the ISSCC 2002
paper, R. P. Preston et al., "Design of an 8-wide Superscalar RISC
Microprocessor with Simultaneous Multithreading", ISSCC Digest pp.
334-335, DOI 10.1109/ISSCC.2002.993068 (IEEE Xplore, paywalled); the
Microprocessor Report coverage of 2001-2002; WikiChip's EV8 deck (host
unreachable); the ICS 1999 vector paper Tarantula's coherency rests on
(ACM DL refused the download); and every document the IDS refers to but
does not contain: the ECOs it cites (118 graphics, 127 WH64EN, 129 memory
management) and its "background for booting" document. No EV8 hardware
reference manual, PALcode or console was ever published, and none was
found.

What the software trees say: nothing. Linux knows IMPLVER values up to EV6
and processor types up to EV69 (`special_insns.h`, `hwrpb.h`); binutils
has no EV8 processor and no LDx_ARM or QUIESCE (it does know WH64EN,
marked "ev7"); GCC's driver knows IMPLVER 3 as the EV7 family and nothing
above [GCC]. The ARM's 4th edition, published after the cancellation, lists
IMPLVER values up to 3 (21364) and calls AMASK bits 10 and 11 "Not
available" [ARM pp. 808-809].

**Reading the IDS.** It says "Subject To Change" on every page, and some
parts disagree with others; each disagreement is marked where it matters.
Its text layer is OCR, so a register index read from it was checked
against the binary field in Table 16-1, not the OCR'd hex alone.

## 1. The EV8 core as software sees it

### Identification

| Value | EV8 | Source |
| --- | --- | --- |
| IMPLVER | 4 | [IDS p. 76] |
| AMASK feature mask | 0x1F07: BWX, FIX, CIX (bits 0-2), MVI (8), precise traps (9), SMT (10), bit 11, prefetch with modify intent (12) | the value [IDS p. 76]; bit 10 [IDS p. 668]; bits 0-2, 8, 9 and 12 [ARM p. 808] |
| AMASK bit 11 | set, meaning not stated | **[inference]**: one of the EV8's new instruction sets (short-vector SIMD or paired single precision); the ARM calls it "Not available" [ARM p. 808] |
| Chip ID | CPU_CNFG<63:56>, read-only; the value is not given | [IDS p. 487] |
| TPU number | TPU_CNFG<57:56>, 0-3, read-only | [IDS p. 488] |
| HWRPB processor type | none assigned: the ARM's table stops at 17 (EV69A) | [ARM pp. 806-807] |

A side finding, for the existing EV7 rows: Alphabox's `ev7` and `ev7z`
rows return IMPLVER 2, marked as an inference because "no document lists"
the EV7's ([marvel.md](marvel.md), E1; `cpu/CpuModels.cpp`). The ARM's
Table D-4 lists **3 for the 21364/EV7 and EV79** [ARM p. 809], and GCC's
driver uses 3 for the EV7 family [GCC]. That inference can now be replaced
by a source; whether to change the row (OpenVMS and the console run with
2 today) is the owner's call, and outside this packet.

### New instructions

Every encoding below is from the IDS's decode tables, which are the
design team's own; Appendix B calls the LDx_ARM/QUIESCE codes "proposed"
[IDS p. 675].

| Instruction | Encoding | What it does | Source |
| --- | --- | --- | --- |
| LDL_ARM Ra,(Rb) | 18.B000 (memory-function format, no displacement) | load sign-extended longword, record the physical address in WATCH_PHYS_ADDR and set watch_flag | [IDS pp. 669-670, 675, 661] |
| LDQ_ARM Ra,(Rb) | 18.B400 | the same for a quadword | same |
| QUIESCE | 18.B800 | if watch_flag is set, start the quiesce timer and suspend the TPU until watch_flag clears | [IDS pp. 672-673, 675] |
| CCB (Rb) | 18.EC00 | CleanCacheBlk: give up ownership of a block, keeping a shared copy (a hint; no architectural effect on data) | [IDS pp. 661, 411] |
| WH64EN (Rb) | 18.FC00 | write hint, evict next (proposed as ECO 127); already a NOP in Alphabox | [IDS p. 661]; [ARM p. 218] |
| CMOV2, FCMOV2 | 11.68, 17.068 | the second half of a CMOV the hardware splits; executed alone it copies Rc to Rc, a NOP | [IDS pp. 72-74] |
| MAP_ALIGN | XOR R31,R31,R31 (11.40) | a scheduling hint, architecturally a NOP | [IDS p. 75] |
| Short-vector SIMD | opcode 1C, new function codes (table below) | integer SIMD on bytes, words and longwords in a quadword | [IDS pp. 70-72, 662-664] |
| Paired single precision | opcode 07, 36 instructions (ECO 118) | two IEEE singles packed in an FP register | [IDS pp. 279-283] |

The IDS notes that Matt Reilly tried every unused 18.xxxx code: most trap
on the 21164 "but are NOPs on the 21064 and the 21264" [IDS p. 676].
Alphabox raises OPCDEC for them (`UNKNOWN2` in the opcode 0x18 decode of
`cpu/AlphaCPU.cpp`). The ARM allows either (unused function codes of
opcodes assigned in Version 5 are UNPREDICTABLE, not UNDEFINED [ARM p.
803]), so this is not a bug, but it is a known difference from a real
21264 worth one line in [cpu-fidelity.md](../cpu-fidelity.md) **[for the
owner]**.

**The short-vector SIMD set (opcode 1C).** The IDS lists the mnemonics
[IDS pp. 70-72] and decodes them [IDS pp. 662-664]:

| Function | Mnemonic | Function | Mnemonic |
| --- | --- | --- | --- |
| 04 | CMPWGE | 05 | CMPLGE |
| 08 | PERMB8 | 09 | GPKBLB4 |
| 10-13 | VADD (4 sizes) | 14, 16 | VADDUL2, VADDSL2 |
| 18-1B | VSUB (4 sizes) | 1C, 1E | VSUBUL2, VSUBSL2 |
| 20-23 | VMINMAX (4 sizes) | 24, 26 | VMINMAXUL2, VMINMAXSL2 |
| 28-2B | PKUWB8, PKULW4, PKSWB8, PKSLW4 | 2C-2F | UPKUBW4, UPKUWL2, UPKSBW4, UPKSWL2 |
| 40-43 | TADD (4 sizes) | 44-47 | TSUB (4 sizes) |
| 48-4B | TABSERR (4 sizes) | 4C-4F | TSQERR (4 sizes) |
| 50-53 | TMUL (4 sizes) | 54, 55 | TMULUSB8, TMULUSW4 |
| 59, 5B | VMULLUW4, VMULHUW4 | 60-63 | VSRB8, VSRW4, VSRAB8, VSRAW4 |
| 64, 66 | VSRL2, VSRAL2 | 68, 69, 6C | VSLB8, VSLW4, VSLL2 |

The existing MVI and CIX codes (30-3F, the MINxxx/MAXxxx, PERR, the
packs, CTPOP/CTLZ/CTTZ) and FTOIT/FTOIS keep their places. The size in
the low two bits ("zz") is not spelled out; MAX's existing codes, 1C.3C
to 1C.3F, give UB8, UW4, SB8, SW4 for 0-3 [ARM p. 783] **[inference]**. The IDS
describes how the multimedia unit computes these (tree adder, saturation,
the min/max byte shuffle [IDS pp. 213-221]) but not each instruction's
architectural result: which word a tree add leaves its sum in, what
VMINMAX returns, how PERMB8 takes its control. The ECO that defined them
is not available.

**Paired single precision (opcode 07).** "A graphics instruction set has
been added to the Alpha architecture (ECO 118)" [IDS p. 279]: two IEEE
singles in one 64-bit FP register (high: sign <63>, exponent <62:54>,
fraction <53:32>; low: sign <31>, exponent <30:23>, fraction <22:0>), in
the FP operate format with a 6-bit function in <10:5> [IDS p. 280]. All
four rounding modes; IEEE exceptions raised per half and ORed, FPCR and
trap control as for scalar FP [IDS p. 280]. The 36 instructions, with
their function codes and operations, are in Table 8-20 [IDS pp. 281-283]:
PADD, PSUB, PCADD, PSUBC, PHADD, PHSUB, PHSUBR, PMUL, PMULH, PMULL,
PMULHN, PMULLN, PFMAX, PFMIN, the compares PCMPEQ/NEQ/LT/LE/UN (which
write IEEE 1.0 or a true zero per half), PCPYS/PCPYSN/PCPYSE, the moves
PMOVHH/HL/LH/LL, PCVTSP, PCVTFI, PEXTH/PEXTL, and the reciprocal and
reciprocal square root estimates PARCPx/PARSQRTx (14 bits, chopped).
**[conflict]**: the decode appendix still says opcodes 01-07 are reserved
and raise OPCDEC [IDS p. 653]; the Fbox chapter, which is newer in content,
assigns 07.

### The four thread processing units

**What each TPU has.** "Each TPU has its own dedicated program state that
consists of 32 integer registers, 32 floating-point registers, a PC, and
internal processor registers" [IDS p. 46]. Table 16-1 marks which IPRs
are per TPU: nearly all of them, including EXC_ADDR, EXC_SUM, I_CTL,
I_MODE, I_PCTX, PAL_BASE, PAL_TEMP1/2, the ITB and DTB write and
invalidate registers, M_CTL, M_PCTX, M_STAT, VA, the interrupt registers
(HW_INT_CLR, IER_CM, ISUM, SIRR), the cycle counter, QUIESCE_TIMEOUT and
WATCH_PHYS_ADDR [IDS pp. 477-479]. Per chip: CPU_CNFG, DC_CTL, the
performance-monitoring bundles, and the Rbox and Zbox registers [IDS pp.
477-481]. The return stack and instruction buffer are split four ways;
caches, TBs, branch predictor, execution units and issue queue are shared
dynamically [IDS p. 46; PACT01 slides 26-27]. One TPU cannot read or
write another's IPRs: "currently no mechanism exists" [IDS p. 477].

**How the OS sees them.** "To the outside world, a TPU has all the
capabilities of a conventional CPU. It holds a full process context"
[PAT-Q col. 1]; "1 CPU with 4 Thread Processing Units" [MPF99 slide 19];
a "virtual CMP" [MPR99a p. 5]. The TB rules make this exact. Every TPU
belongs to one of four TPU groups (I_PCTX<17:16>, M_PCTX) [IDS p. 492]:

- TPUs in different groups have distinct ASN spaces, ASM entries,
  superpages and mappings, and "share the same relation to each other as
  different processors within a SMP machine. They could even be running
  different operating systems" [IDS p. 306].
- TPUs in one group with different ASNs run different processes of one
  OS; with the same ASN they share the whole address space (threads of
  one process) [IDS p. 306].
- "Mode 2 (expected to be used by VMS) is the SMP-like mode, where every
  TPU is in a different group. Mode 3 (expected to be used in Unix) is the
  full multithread mode, where all TPU's are in one group" [IDS p. 306].
- Each DTB entry carries four group valid bits; a lookup matches on VA,
  the current TPU's group, and ASN or ASM [IDS p. 306]. DTB_IM invalidates
  all entries, or only the current group's, by ASM or by ASN [IDS pp.
  502-503]. The Icache carries a valid bit per group [IDS p. 91].

So in Mode 2 an EV8 is, to an OS, four processors whose only coupling is
performance **[inference]**: separate TB contexts, coherent memory through
shared caches. That is what an emulator can model without a cache model.

**Processor numbers.** R_WHOAMI holds the chip's 10-bit network address
(EW<9:5>, NS<4:0>) [IDS p. 527]; TPU_CNFG holds the TPU number [IDS p.
488]. How the two make the number CALL_PAL WHAMI returns is PALcode's
choice, and no EV8 PALcode exists **[decision: D6]**.

**Interrupts.** The EV8 takes interrupts the 21364 way: requests arrive as
network writes into the Rbox, either an interrupt ID queued for PALcode to
read (RBOX_INTQ/INTA) or a bit set in a 32-bit mask (RBOX_INT, masked by
RBOX_IMASK); interprocessor interrupts are writes to another chip's
RBOX_IREQ [IDS pp. 75, 464-465]. They reach the core on six levels: 0
correctable/performance counter, 1 interrupt queue, 2 interval timer, 3
interprocessor/software alert, 4 halt, 5 machine check [IDS p. 465]. The
IDS states the problem SMT adds ("each [thread] capable of running PAL
code, interrupt-level service, or user code while the others are active",
[IDS p. 75]) and that the Cbox delivers "thread-specific requests" [IDS p.
57], but **does not say how a chip-level interrupt or IPI is steered to a
TPU**: R_INT_STAT, R_INT_MASK and R_INT_REQ are "TBD" [IDS pp. 527-528],
the queue's depth is given as four [IDS p. 464] and as two [IDS p. 528]
**[conflict]**, and HW_INT_CLR "will be completely reworked" [IDS p.
513]. For the GIO (management) interrupt, "a mask register will define
which TPU(s) are to receive" it [IDS p. 476]. Steering is a gap
**[decision: D5]**.

**Starting TPUs.** There is a WAKEUP PALcode entry (PAL_BASE + 0xA00)
beside RESET (+0xB00), FAULT_RESET and IP_RESET [IDS p. 499], and a
reference to a "background for booting" document with a "Suspending
TPUs" section [IDS p. 511]. The initialization chapter is empty [IDS p.
583]. How TPUs 1-3 come out of reset is unknown **[decision: D7]**.

**The cycle counter** is per TPU: CC[tpu], index 0xB8-0xBB, COUNT <31:4>
incrementing every 16th cycle, OFFSET <63:32> for the OS [IDS pp.
482-483].

### LDx_ARM and QUIESCE

The fullest part of the IDS (Appendix B, [IDS pp. 667-684]), with the same
mechanism in the patent [PAT-Q cols. 7-12]:

- LDx_ARM loads like LDx_L (same faults: access violation, alignment,
  fault on read, translation not valid), sets watch_flag and records the
  physical address; it has no displacement because it uses the
  memory-function format [IDS pp. 669-670]. Big-endian longwords invert
  va<2> as loads do.
- Another processor clears a set watch_flag by a successful store,
  STx_C, or a WH64 that changes memory, anywhere in the watched range: an
  aligned block of 2**N bytes, the same as the LDx_L lock range, at least
  16 bytes and at most a page [IDS p. 670].
- watch_flag also clears on the quiesce timeout and on CALL_PAL REI, rti,
  rfe, retsys and urti; on other CALL_PALs, WH64 and ECB it is
  UNPREDICTABLE; whether an interrupt clears it is UNPREDICTABLE
  architecturally, but the implementation clears it when an interrupt is
  directed to a quiesced TPU, and an LDx_ARM fails to arm while an
  interrupt is pending [IDS pp. 670, 671, 677-678].
- LDx_ARM on one processor never affects another's watch state [IDS p.
  670]. Two LDx_ARMs: the second overwrites; two QUIESCEs: the second
  never sleeps [IDS p. 671]. Memory accesses, branches, subsetted or
  unused-function instructions between LDx_ARM and QUIESCE may make the
  QUIESCE always fail on some implementations [IDS p. 671].
- QUIESCE: if watch_flag is set, start the timer and pause; resume when
  watch_flag clears, after the QUIESCE or at an interrupt routine, which
  may return to the QUIESCE only if watch_flag is known clear [IDS pp.
  672-673].
- Software must test AMASK bit 10 first (the example: `LDA R2,0x400(R31)`,
  `AMASK R2,R2`, `BEQ R2,CheckLock`), because no encoding is a NOP on every
  older processor [IDS pp. 674-676; PAT-Q col. 10]. "Software will not
  emulate any LDx_ARM instruction" [IDS p. 671].
- QUIESCE_TIMEOUT[tpu], index 0x38: TIMEOUT <19:4> in CPU cycles, reset
  value 0x280 (0x2800 = 10240 cycles, the "10K cycles" of [IDS p. 678]),
  WATCH_EN <1> (wake on the watched address) and TIMEOUT_EN <0> [IDS pp.
  510-511]. WATCH_PHYS_ADDR[tpu] holds PA<47:4> and WATCH_FLAG <0>, and is
  not readable [IDS p. 513].

### The PALcode interface

This is where the EV8 leaves the 21264 behind:

| | 21264 / EV7 (what Alphabox implements) | 21464 |
| --- | --- | --- |
| HW_MFPR / HW_MTPR | opcodes 0x19 / 0x1D, index in <15:8> | opcodes 0x19 / 0x1D, 8-bit index in <12:5> (MSB selects Ibox or Mbox), reader and writer classes in <24:21> and <4:0> [IDS pp. 571-573] |
| HW_LD / HW_ST | 0x1B / 0x1F, 12-bit displacement | 0x1B / 0x1F, type <15:13> (physical, VPTE, virtual, write check, alternate mode), length <12:11>, 11-bit displacement [IDS pp. 569-570] |
| Return from PALmode | HW_RET (0x1E) | the ordinary RET; GPR<1:0> selects normal, PALmode or SuperPALmode; HW_RET is gone [IDS p. 573]. Opcode 1E is IFETCHB [IDS p. 650] |
| Modes | PALmode | PALmode and SuperPALmode, entered by a PNMI event [IDS pp. 573-574] |
| Shadow registers | eight in PALmode | **[conflict]**: "given the lack of shadow registers in the 21464" [IDS p. 500], against a "shadow register replacement process in PALmode" for R24/R25 [IDS p. 574] |
| DTB single miss | return PC in EXC_ADDR | in DTBMS_RET_ADDR (0xA6/0xA7); DTBM_SINGLE does not write EXC_ADDR [IDS pp. 483-484] |

Main IPR indexes, from Table 16-1's binary fields [IDS pp. 478-479]:

| IPR | EV8 index | 21264 index ([marvel.md](marvel.md), E1) |
| --- | --- | --- |
| M_CTL | 0x00-0x03 | 0x28 |
| M_MODE | 0x08-0x0F | (DTB_ALTMODE 0x26, CM 0x09) |
| M_PCTX | 0x10-0x13 | (PCTX 0x40-0x5F) |
| M_STAT | 0x20 | (MM_STAT 0x27) |
| VA, VA_FORM | 0x21, 0x23 | 0xC2, 0xC3 |
| QUIESCE_TIMEOUT | 0x38 | none |
| DTB_IM, DTB_IS | 0x40, 0x42 | (DTB_IA 0xA3, DTB_IS0 0x24) |
| DTB_PTE, DTB_TAG | 0x44-0x45, 0x46-0x47 | 0x21/0xA1, 0x20/0xA0 |
| DC_CTL, DC_STAT | 0x48, 0x49 | 0x29, 0x2A |
| I_CTL | 0x80-0x83 | 0x11 |
| I_MODE | 0x88-0x8B | (CM 0x09) |
| I_PCTX | 0x90-0x97 | (PCTX 0x40-0x5F) |
| EXC_SUM, EXC_ADDR | 0xA0, 0xA1 | 0x0F, 0x06 |
| IVA_FORM | 0xA3 | 0x07 |
| DTBMS_RET_ADDR | 0xA6, 0xA7 | none |
| PAL_BASE | 0xA8 | 0x10 |
| PAL_TEMP1, PAL_TEMP2 | 0xA9, 0xAA | none |
| CC | 0xB8-0xBB | 0xC0 |
| ITB_IM, ITB_IS, ITB_PTE, ITB_TAG | 0xC0, 0xC2, 0xC4, 0xC6 | (ITB_IA 0x03, ITB_IS 0x04, ITB_PTE 0x01, ITB_TAG 0x00) |
| CPU_CNFG | 0xC8 | none |
| IC_STAT, IC_FLUSH, IC_FLUSH_ASM | 0xC9, 0xCC, 0xCD | (I_STAT 0x16, IC_FLUSH 0x13, IC_FLUSH_ASM 0x12) |
| TPU_CNFG | 0xD8 | none |

The low index bits of several registers select which fields a write
changes (I_CTL, I_MODE, I_PCTX, CC) [IDS pp. 483, 489-491]. CPU_BASE is
also listed at 0xAA, PAL_TEMP2's index **[conflict, or an OCR error]**.
IER_CM, ISUM, SIRR and HW_INT_CLR are per TPU but their indexes are blank
in the scan [IDS p. 479].

PALcode entry offsets from PAL_BASE [IDS pp. 498-499]: DTBM_DOUBLE 0x100,
DTBM_DOUBLE_ALT 0x180, FEN 0x200, UNALIGN 0x280, DTBM_SINGLE 0x300,
DFAULT 0x380, OPCDEC 0x400, IACV 0x480, MCHK 0x500, ITB_MISS 0x580, ARITH
0x600, INTERRUPT 0x680, MT_FPCR 0x700, IMCHK 0x780, DTBM_SINGLE_CONS
0x800, ITB_MISS_CONS 0x880, BAD_JUMP_IVA 0x900, FAULT_RESET 0x980, WAKEUP
0xA00, IP_RESET 0xA80, RESET 0xB00. The thirteen from 0x100 to 0x700
keep the 21264's offsets; the 21264's RESET/WAKEUP at 0x780 [21264 HRM,
EV68CB edition, PDF p. 198] becomes IMCHK. PALcode must follow sixteen
restrictions, mostly IFETCHB placement around IPR writes [IDS pp.
575-582].

So **neither EV7 PALcode in the Marvel console can run on a faithful EV8**
**[inference]**: they read I_CTL at 0x11 for the chip ID, write DTB_PTE0
at 0x21 (the EV8's read-only VA), and return with HW_RET ([marvel.md](marvel.md),
E1). The OS-visible PALcode interface (CALL_PAL functions, HWPCB, page
tables) is the architecture's and does not change; only the code behind
it would have to be written for the EV8.

### Memory management

- 52-bit VA, 48-bit PA, 8-bit ASN [IDS p. 43]. With 8 KB pages a 32-bit
  PFN reaches only 45 bits of PA, so the 64 KB page mode exists to reach 48
  bits and to cover 52 bits of VA with three page-table levels [IDS p.
  307].
- I_CTL<21> VA_SIZE (43 or 52 bits), <20> PAGE_SIZE (8 or 64 KB), <27>
  REDUCED_PT, <26:24> three superpage enables (PC<63:50> = 0x3FFE maps
  PA<47:0> directly, and two smaller ones), VPTE_BASE <51:33> [IDS pp.
  489-490]; M_CTL repeats these for the D-stream [IDS p. 489].
- Appendix C is a proposed redesign of these registers (ECO 129: only
  three-level page tables, mixed-mode sign extension, reduced page tables)
  [IDS pp. 685-711]. The ARM's 4th edition describes reduced page table
  mode for 64 KB pages and the 21364's implementation of it [ARM pp. 381,
  816].
- Granularity hints 8x, 64x, 512x in both page sizes [IDS p. 306].

### Memory model

Unchanged from the architecture as far as the IDS shows **[inference]**:
the same I/O space ordering rules as the 21264 [IDS p. 467], LDx_L/STx_C
with a lock range [IDS p. 670], the 21364 coherence protocol "with small
enhancements", including an on-chip 256 KB directory cache [IDS pp. 58,
411-412]. TPUs share the L1, so they see each other's stores without
coherence traffic [MPR99a p. 5]. QUIESCE is implemented as a WMB-like
store-queue entry [IDS p. 677].

## 2. The system around EV8

Everything the IDS says about systems points at Marvel, the platform
Alphabox's ES47, ES80 and GS1280 rows model ([marvel.md](marvel.md)):

- **The management port** is the EV7's GIO port, reworked: 32-bit
  transactions instead of 64, no enable bit, a 4-bit datapath, because
  "the Marvel CMM module connector and FPGA are already pin constrained";
  the registers "Marvel has defined" in GIO space are listed [IDS pp.
  471-476]. Among them, COM0 transmit and receive at 04h and 05h are the
  two registers through which Alphabox's CMM model already carries the
  console terminal ([marvel.md](marvel.md), "The machine"): the layout
  agrees.
- **The I/O bridge is the IO7**: packets carry "PID for a packet generated
  by the IO7 ASIC (11 bits)", an I/O ASIC's PID being its processor's
  with the top bit set [IDS pp. 457, 463].
- **The protocol and network** are the 21364's: coherence protocol [IDS
  p. 58], a 21364 mode on the network ports [IDS p. 44], interrupt
  delivery as on the 21364 [IDS pp. 464-465].
- **The memory** is RDRAM on two Zbox ports per chip, 32 GB per processor
  [IDS p. 43].
- **The press** agrees: "the system interface of the 21464 will be
  similar to that of the 21364" [MPR99b p. 3]; "in a conceptual sense at
  least, will be a drop-in replacement for the EV7" [RWT1 p. 1, an
  estimate].
- **Tarantula** "had to be a board-compatible replacement for EV8" and
  reuses the EV8's Zbox and Rbox [TAR pp. 283-284].

What differs from the EV7, and so from Alphabox's Marvel model:

- 48-bit physical addresses with I/O at PA<47> [IDS pp. 43, 298], against
  the EV7's 44 bits with I/O at PA<43> and the PID inverted in PA<43:35>
  ([marvel.md](marvel.md), "The EV7 core against the EV68");
- 10-bit node IDs (11 with the I/O bit) [IDS p. 457] against the EV7's
  9-bit PE field;
- the Rbox and Zbox registers are documented as IPRs [IDS pp. 479-480,
  514-567], reached by other nodes through RdIPR/WrIPR in "the processor
  I/O space" [IDS p. 428]. Whether they keep the EV7's CSR offsets is not
  stated, and several are "TBD".

So: **an EV8 was meant to run in Marvel systems** (CMM, IO7, 21364
network), on a CPU module of its own **[inference]**. No source says an
EV8 module fits an existing ES47/ES80/GS1280 backplane, and the address
map changes. In Alphabox terms the Marvel board rows (CMM, IO7, topology)
are the right home; the EV7 console is not.

## 3. Tarantula

Everything here is from the one paper [TAR], unless marked.

### Architectural state

- 32 vector registers v0-v31, each 128 64-bit elements; v31 reads as
  zero [TAR pp. 282-283].
- vl, the vector length, an 8-bit register; vs, the stride, 64 bits; vm,
  the mask, 128 bits [TAR p. 282]. vl runs from 0 to 128, the loop bound
  of every instruction in Fig. 1 **[inference]**.
- The Vbox "was also multithreaded", so this state exists per TPU [TAR p.
  284]: 32 KB of vector registers per TPU **[inference: 32 x 128 x 8
  bytes]**.

### Instructions

"45 new instructions (not counting data-type variations)" in five groups
[TAR p. 282]:

| Group | Representative | Semantics as published [TAR p. 283, Fig. 1] |
| --- | --- | --- |
| VV, vector-vector operate | VVADDQ Va,Vb,Vc | Vc[i] = Va[i] + Vb[i] for i < vl; Vc[i] UNPREDICTABLE for vl <= i < 128 |
| VS, vector-scalar operate | VSMULQ Va,Fb,Vc | Vc[i] = Va[i] * Fb for i < vl; the rest UNPREDICTABLE |
| SM, strided memory | VLOADQ Vc,off(Rb) | for i in a random permutation of 0..vl-1: Vc[i] = MEM[Rb + off + i*vs]; the rest UNPREDICTABLE |
| RM, random memory (gather/scatter) | VSCATQ Va,Rb,Vc | for i in a random permutation of 0..vl-1: MEM[Va[i] + Rb] = Vc[i] |
| VC, vector control | (setvm, setting vl, vs) | not shown |

Other names in the text: vloadq, vcmpne, vcmpgt, vand, setvm (copy a
vector register into vm), and DrainM (below) [TAR pp. 282, 287]. Masks:
compares write a full vector register of booleans, setvm moves one into
vm, and instructions with an "under-mask" specifier use vm; vm is renamed,
so the next mask can be computed while the current one is in use [TAR p.
282]. Every group except VV takes a scalar operand [TAR p. 284]. Element
types: quadword integer and double-precision floating point appear; "data
type variations" are not listed.

**No encoding is published**: no opcode, no register field layout, no
mask-specifier bit. The EV8 itself reserves opcodes 01-06 (07 went to
paired single precision) [IDS pp. 653, 280], so those are the natural
place for them **[decision: D10]**.

### Memory accesses and ordering

- Element order within one memory instruction is unspecified (the
  random permutation in Fig. 1) [TAR p. 283].
- Vector memory accesses go to the L2 and are coherent with the
  scalar side by a presence bit; hardware covers every case but one: a
  scalar store followed by a vector load, which needs the new memory
  barrier **DrainM** (it drains the store queue and write buffer, then
  replay-traps the next instruction) [TAR pp. 286-287]. A scalar store
  followed by a vector store is handled by writing scalar stores through
  to the L2, for threads that use vectors [TAR p. 287, note 4].
- How vector accesses order against MB, WMB and other processors is not
  stated: "fully integrates into a virtual-memory cache-coherent system
  without changes to its coherency protocol" [TAR p. 281] is all
  **[gap: D12]**.
- Vector prefetches are loads to v31; their page faults and TLB misses
  are ignored [TAR p. 283].

### Exceptions, translation, precision

- Precise at instruction level: a trapping vector instruction (TLB miss,
  divide by zero, and so on) reports its PC, and nothing about which
  element [TAR p. 283].
- Translation: per-lane TLBs (16 lanes x 32 entries, fully associative);
  a miss transfers "control to system software", which may refill the
  missing lanes or read vs and refill everything the instruction may need
  [TAR p. 286]. A 512 MB page size was adopted [TAR p. 286], which is the
  64 KB page mode with reduced page tables [IDS App. C; ARM p. 381]
  **[inference]**.
- Arithmetic: 64-bit elements, the lanes' units are integer add and
  multiply and FP add and multiply [TAR p. 283, Fig. 3]; no fused
  multiply-add [TAR p. 288]. Rounding, the FPCR, trap modes and the trap
  shadow for vector FP are not described **[gap: D13]**.

### Context switch and detection

Neither is published. The paper says the Vbox was multithreaded "to
avoid excessive burden onto the operating system" [TAR p. 284], which
gives per-TPU state but not what PALcode or the OS saves on a process
switch, nor how software learns the Vbox is there (AMASK's 0x1F07 already
uses bits up to 12) **[decisions: D11, D14]**.

## 4. The gaps: decisions for the owner

Each is a choice Alphabox would make where no source exists. A choice made
should be recorded in the code and in [cpu-fidelity.md](../cpu-fidelity.md)
as "Alphabox decision", the way a value chosen to satisfy a console is
marked today.

| # | Area | What is missing | Proposal |
| --- | --- | --- | --- |
| D1 | Privileged interface | the EV8's IPRs exist only in a draft with TBDs and conflicts; no PALcode was written | **two modes**: an *EV7-compatible* EV8 (the EV7's IPRs and PALcode interface, EV8 user ISA) first; a *native* EV8 interface from the IDS later (below) |
| D2 | Chip ID, console naming | CPU_CNFG<CHIP_ID> has no value; the Marvel console maps I_CTL<29:24> through its EV7 table | in EV7-compatible mode report an EV7z chip ID until a probe shows what the console does with an unknown one |
| D3 | HWRPB processor type | none assigned | 18 (the next free value after EV69A, [ARM p. 807]), only where we write the HWRPB ourselves |
| D4 | SMT exposure | how TPUs appear to an EV7 console | first one TPU per chip (S3); then each TPU listed to the console as a processor of its own through the CMM model, memory and IO7 staying with TPU 0 (S4). Whether the console accepts a processor with no memory of its own is for a probe to show |
| D5 | Interrupt and IPI steering to TPUs | R_INT_* "TBD" | each TPU gets its own interrupt mask and request bits; IO7 LSI/MSI targets name PID and TPU; IPIs address a TPU |
| D6 | Processor numbers | WHAMI is PALcode's | PID x 4 + TPU, handed to the console's PALcode at entry the way the PID is today; a one-TPU chip keeps today's numbers |
| D7 | TPU start-up | WAKEUP entry, no boot document | TPUs 1-3 start like a Marvel secondary (RBOX_SCRATCH1 polling) in EV7-compatible mode |
| D8 | Short-vector SIMD results | per-instruction architectural results | define each from the IDS's datapath description and the existing MVI conventions; publish the table in cpu-fidelity.md. Proposed in [ev8-isa.md](ev8-isa.md), 3.3 |
| D9 | Paired single precision | the 21264-style trap shadow and denormal handling for pairs | follow the scalar IEEE rules per half, as [IDS p. 280] says, with the existing FPCR code |
| D10 | Tarantula encodings | all | opcodes 01-06: one per group (VV, VS, SM, RM, VC, and one spare), FP-operate-like fields, a mask bit; DrainM as an 18.xxxx code. Encoded in [ev8-isa.md](ev8-isa.md), section 4 (integer-operate fields; DrainM 18.5000) |
| D11 | Tarantula enable and context switch | no PALcode or OS support defined | a per-TPU vector enable, off at reset, so an OS that does not know vectors (OpenVMS) gets OPCDEC and never shares vector state between processes; a kernel that does (Linux) enables it and saves v0-v31, vl, vs, vm lazily |
| D12 | Vector ordering | against MB/WMB and other processors | MB and WMB order vector accesses like scalar ones; within an instruction, any element order; DrainM as published |
| D13 | Vector FP | rounding, FPCR, traps | FPCR shared with scalar FP; dynamic rounding from the FPCR; traps precise at the instruction (as published), FPCR status ORed over elements |
| D14 | Tarantula detection | no AMASK bit | AMASK bit 3, the first free bit of the low byte reserved for standard extensions [ARM p. 204]. Kept in [ev8-isa.md](ev8-isa.md), section 2, with the vector enable |
| D15 | Vector TLB misses | lane TLB refill IPRs | the emulator keeps no lane TLBs: a vector access that misses raises the ordinary DTB miss for that element's VA, with the instruction's PC; the instruction restarts whole (loads and gathers are idempotent; a restarted scatter rewrites the same values) |
| D16 | QUIESCE in the emulator | (implementation, not architecture) | watch_flag per TPU beside the LL/SC lock flags (`CSystem::cpu_lock`); a watched store from any processor or DMA clears it; the sleeping TPU's host thread waits on a condition variable with a wall-clock timeout from QUIESCE_TIMEOUT at the row's clock, never a fatal one |

The native interface of D1 (for a later stage) would follow the IDS: its
IPR table, RET for PALmode exit, IFETCHB, SuperPALmode, 52-bit VA and 48-bit
PA, TPU groups in the TBs, and its PALcode entry offsets, with every
"TBD" and conflict above settled by a decision of its own.

## 5. How such a machine would boot

The project rule is that a machine's own console firmware defines "done"
([platforms.md](../platforms.md)). The EV8 never had one, so "done" has to
be defined by something else. The options:

**A. An EV8 row on a Marvel board under the EV7 console, in
EV7-compatible mode (D1).** The core keeps the EV7's privileged interface
(IPRs, HW_RET, 44-bit PA, the CSR window), so the console's PALcode and
SRM V7.3-1 run unchanged; the EV8's user-visible ISA is added on top: IMPLVER
4, AMASK 0x1F07, LDx_ARM/QUIESCE, CCB, the SIMD and paired-single sets,
later Tarantula. Needs: a family flag in `cpu/CpuModel.hpp` that turns the
new instructions on, and nothing new in the console path. Oracle: the
console and OpenVMS 8.4 behave as on an `ev7` row (L2 to L5 of the ladder,
which also shows that IMPLVER 4 breaks nothing), the instruction oracle
(`test/cpu/cpu_oracle.py`) with cases written from the IDS's pseudo-code
and tables and Tarantula's Fig. 1, and Linux on the ES47 (backlog E2/M8)
for user programs that use the new instructions. This is not history: a
real EV8 had a different privileged interface. It is the published EV8 as
user code and an OS see it.

**B. A native EV8 with Alphabox's own PALcode and console.** The IDS's
privileged interface, and a PALcode and a console written by the project:
the UNIX/Linux PALcode interface first (it is smaller than OpenVMS's), a
console that builds the HWRPB and loads a Linux kernel, on the Marvel
board's CMM and IO7 models. Needs: a new core family, the PALcode (the
Marvel UNIX PALcode is 45 KB of EV7 code, [marvel.md](marvel.md)), a
console, every D-item settled. OpenVMS would need a VMS PALcode and SRM
console callbacks as well, which puts it out of reach in practice
**[inference]**. Oracle: the IDS for every register and trap, the
conformance suite of A, and Linux booting to a shell.

**C. An EV8 row on a Tsunami or Titan board.** The ES40 takes four
processors: one EV8 with four TPUs would fill it, under its own console and
OpenVMS. Cheap, but further from history than A: the EV8 was a Marvel part
with no 21264 system bus.

**D. A bare-metal harness only.** Boot images (`test/tools/bench_image.py`
and `srm_probe.sh`) that run the new instructions on any board and store
results. Needs nothing but the instructions. Oracle: the published
semantics. It is a test tool every option needs, not a machine.

**Recommendation: A, with D as its test bench, and B as the long-term
target only if the owner wants the native interface.** A keeps a real
firmware and a real OS as the guard that nothing else broke, delivers
everything the published EV8 gave software (identification, quiesce, two
new instruction sets) and Tarantula, and spends nothing on firmware nobody
wrote. B is the only way to show the EV8's own PALcode interface, and its
cost is a PALcode and a console of our own. C is worth it only as a quick
way to see TPUs as processors under OpenVMS before A's interrupt-steering
decisions are made.

### The owner's decision (2026-10-24)

- **A first**: an EV8 row on the ES47 in EV7-compatible mode, with D as its
  test bench (S1-S10).
- **Then B, as planned work, not an option**: the native EV8 interface from
  the IDS (S11-S13). Where the IDS is a draft (TBDs, conflicts, empty
  chapters), the gap is filled by what can be inferred from the IDS's own
  context, the EV7's interface it grew from and the published papers, each
  marked **[inference]** or **[decision]** in the code and in
  [cpu-fidelity.md](../cpu-fidelity.md).
- **D2-D16**: the proposals in section 4 stand unless a stage shows one
  does not work; a stage that changes one says so.

What "done" would mean for A, in the ladder's terms: L0 builds; L1-L2 the
Marvel console starts and prompts on the EV8 row; L3 `show config` prints
what D2 decided (there is no real listing to match); L4 the conformance
cases pass in the interpreter and the JIT (JIT_VERIFY at 0 mismatches);
L5 OpenVMS boots, and Linux runs programs using the new instructions; L6
the ES40 console-log check and the other machines are unchanged.

## 6. The plan

Decided by the owner on 2026-10-24 ([above](#the-owners-decision-2026-10-24)).
Each step is one packet with one question, in the interpreter first and
the JIT after, each with the test that shows it. No step changes an
existing row's behaviour. The encodings, syntax and results S2 to S10
implement are in [ev8-isa.md](ev8-isa.md), the contract with the nada
toolchain.

| Step | What | Engine | Test or guest | Rests on |
| --- | --- | --- | --- | --- |
| S0 | Decide D1-D16 (this packet) | | | done 2026-10-24: A, then B; D2-D16 as proposed |
| S1 | An `ev8` row on the ES47 board in EV7-compatible mode: IMPLVER 4, AMASK 0x1F07 (with bits for what is implemented), chip ID per D2 | interpreter, JIT (constants only) | console to `P00>>>`; OpenVMS 8.4 boots on the row; cpu_oracle cases for IMPLVER/AMASK | D1, D2 |
| S2 | The harmless additions: CCB, WH64EN, CMOV2/FCMOV2, MAP_ALIGN as NOPs; unused 18.xxxx codes stay NOPs as on the EV6/EV7 rows (2026-10-24), but for the EV8's own | both | cpu_oracle cases; JIT_VERIFY | |
| S3 | LDx_ARM and QUIESCE with one TPU per chip, across processors and DMA | interpreter first; the JIT ends a block at QUIESCE and calls the interpreter's routine | a two-processor boot image: one sleeps on a lock, the other releases it; timeout path; interrupt path (D16). Linux: a spinlock patched as in [IDS p. 674] | D16 |
| S4 | TPUs as processors: per-TPU state, D5-D7, TPU groups in the TB model (Mode 2 first) | interpreter, then JIT (per-TPU state is per-CPU state) | the console lists 4 x the chips' processors; OpenVMS boots on all of them; Linux SMP | D4-D7 |
| S5 | The short-vector SIMD set | interpreter, then JIT | cpu_oracle cases from D8's table; a Linux program checked against scalar code | D8 |
| S6 | Paired single precision | interpreter, then JIT | the FP self-test extended (`alphabox_fptest`) per half; cpu_oracle cases | D9 |
| S7 | Tarantula state, VC and VV/VS groups, SaveState/RestoreState of vector state | interpreter | boot-image kernels (daxpy and the like) checked against scalar results | D10, D11, D13, D14 |
| S8 | Tarantula memory: SM, RM, masks, prefetch to v31, DrainM, faults and restart | interpreter | page-crossing and faulting vectors, scatter conflicts, DrainM cases | D12, D15 |
| S9 | Tarantula under an OS: Linux enables vectors and saves them on switch | interpreter | two processes using vectors at once | D11 |
| S10 | Tarantula in the JIT (helpers first, host SIMD later) | JIT | JIT_VERIFY extended to compare vector state; any speed claim through `perf_ab.py` | S7-S9 |
| S11 | The native EV8 privileged interface (option B), from the IDS: its IPR table, RET for PALmode exit, IFETCHB, the HW_MFPR/HW_LD layouts, SuperPALmode, the new PALcode entry offsets, 52-bit VA and 48-bit PA, TPU groups in the TBs; every TBD and conflict settled by inference, marked | interpreter, then JIT | test/cpu cases per register and trap, written from the IDS; a native-mode `ev8n` row beside the EV7-compatible one | S1-S4 |
| S12 | An Alphabox PALcode for the native interface (the UNIX/Linux interface first, being the smaller) and a minimal console that builds the HWRPB and loads a kernel, on the Marvel CMM and IO7 models | PALcode in Alpha assembly, built by the project | boot-image tests of every PALcode entry; the console prompts and loads aboot or a kernel | S11 |
| S13 | Linux on the native EV8: SMT as TPUs, then the new instructions and Tarantula under the native interface | both | Linux to a shell, SMP across TPUs, the S3-S9 tests repeated in native mode | S12, S5-S9 |

Binutils knows none of the new instructions, so test programs carry them
as `.long` words until a small assembler patch exists (out of tree).

## Findings

2026-10-23:

- The EV8's design specification is public on bitsavers (Rev 1.1k, January
  2001), and it goes much deeper than the papers and patents this search
  started from: registers, PALcode interface and encodings are published,
  as a draft.
- The ARM's Table D-4 and GCC give IMPLVER 3 for the EV7, where Alphabox's
  `ev7` and `ev7z` rows return 2 on an inference (Identification, above).
- On a real 21264, unused 18.xxxx function codes are NOPs according to the
  IDS's account of Matt Reilly's test [IDS p. 676]; Alphabox raises
  OPCDEC. Both are allowed by the ARM.
- The 21464 decodes 18.4800 as MB [IDS p. 661], as the ARM requires of
  the two codes reserved for weaker barriers [ARM p. 803] and as Alphabox
  already does.
- Wikipedia cites the 1.7 GHz news item as Microprocessor Report 13(16),
  6 December 1999; the file is in the 15 November issue, 13(15).

## Rules

- Documents may be downloaded; firmware, console or PALcode images never.
  Nothing in this packet needs one: no EV8 firmware exists.
- Existing machines must not change behaviour: the ES40 console-log check
  stays clean and the JIT cross-check at 0 mismatches.
- Every value not taken from a source is an Alphabox decision, marked as
  one in the code and in [cpu-fidelity.md](../cpu-fidelity.md).
- Speed claims go through `test/tools/perf_ab.py`.
