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

**Branch**: `ev8/s1s2` to `ev8/s4`, `ev8/s7` · **Config**: `cpu0 = ev8` (and so every
processor) on `platform = "es47"`, `"es80"` or `"gs1280"`; `tpus = 2..4`
in each to list the TPUs as processors ([S4](#s4-2026-10-25)) ·
**Status**: research done (2026-10-23); the owner chose the path on
2026-10-24 (option A first, then the native interface,
[below](#the-owners-decision-2026-10-24)); S1 and S2 done the same day
([the plan](#6-the-plan), [what they did](#s1-and-s2-2026-10-24)), S3 too
([S3](#s3-2026-10-24)), S4 on 2026-10-25 ([S4](#s4-2026-10-25)), and
S7, Tarantula's state, control and operate instructions, the same day
([S7](#s7-2026-10-25)).

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
2 today) is the owner's call, and outside this packet. (Done: the rows
return 3 since 2026-10-24.)

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
Alphabox raised OPCDEC for them when this packet was written; since
2026-10-24 they are NOPs on every row, as on the 21264
([cpu-fidelity.md](../cpu-fidelity.md)). The ARM allows either (unused
function codes of opcodes assigned in Version 5 are UNPREDICTABLE, not
UNDEFINED [ARM p. 803]). The 21464's own decode tables use don't-care
bits, so on it no 18.xxxx code is unused: 18.D000 is RPCC, 18.E400 RC
[IDS p. 661] (a finding of S2, [below](#s1-and-s2-2026-10-24)).

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
singles in one 64-bit FP register (high: sign <63>, exponent <62:55>,
fraction <54:32>; low: sign <31>, exponent <30:23>, fraction <22:0>; the
IDS's Table 8-19 misprints the high half as exponent <62:54> and fraction
<53:32>, nine and 22 bits, against the low half's layout and PCVTSP's
packing [IDS p. 280]; [ev8-isa.md](ev8-isa.md), 3.4), in
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
choice, and no EV8 PALcode exists **[decision: D6, replaced by the
owner's numbering of 2026-10-25, [S4](#s4-2026-10-25)]**.

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
Chapter 22 has a later entry table, with TPU_RESTART at 0xA80 ("another
TPU has requested a restart") and MILD_RESET at 0xB00 [IDS pp. 631-634],
and the "Suspending TPUs" remark points to secondary TPUs parked with
LDQ_ARM and QUIESCE [IDS p. 511]; the native track follows both
([ev8-native.md](ev8-native.md), sections 1.5 and 1.7, N11 and N17).

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
| HW_MFPR / HW_MTPR | opcodes 0x19 / 0x1D, index in <15:8> | opcodes 0x19 / 0x1D, 8-bit index in <12:5> (MSB selects Ibox or Mbox), the reader class in <24:21>; <4:0> is HW_MFPR's destination Rc and HW_MTPR's writer class [IDS pp. 571-573, 650] ([ev8-native.md](ev8-native.md), 1.2, N3) |
| HW_LD / HW_ST | 0x1B / 0x1F, 12-bit displacement | 0x1B / 0x1F, type <15:13> (physical, VPTE, virtual, write check, alternate mode), length <12:11>, 11-bit displacement [IDS pp. 569-570] |
| Return from PALmode | HW_RET (0x1E) | the ordinary RET; GPR<1:0> selects normal, PALmode or SuperPALmode; HW_RET is gone [IDS p. 573]. Opcode 1E is IFETCHB [IDS p. 650] |
| Modes | PALmode | PALmode and SuperPALmode, entered by a PNMI event [IDS pp. 573-574] |
| Shadow registers | eight in PALmode | two, S0 and S1, by a position-dependent overlay of R22-R25 in PALmode [IDS p. 580], which settles the apparent conflict between "the lack of shadow registers in the 21464" [IDS p. 500] and a "shadow register replacement process" [IDS p. 574] ([ev8-native.md](ev8-native.md), 1.3) |
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
0xA00, IP_RESET 0xA80, RESET 0xB00 (chapter 16; chapter 22's later table
names 0x980 FAST_RESET, 0xA80 TPU_RESTART, 0xB00 MILD_RESET and adds
DST_NXM at 0xB80 [IDS pp. 633-634]: [ev8-native.md](ev8-native.md),
1.5). The thirteen from 0x100 to 0x700
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
| D4 | SMT exposure | how TPUs appear to an EV7 console | first one TPU per chip (S3); then each TPU listed to the console as a processor of its own through the CMM model, memory and IO7 staying with TPU 0 (S4). Whether the console accepts a processor with no memory of its own is for a probe to show. S4: it does; done with `tpus = 2..4`, an Alphabox construction, the default staying one processor per chip (the owner, 2026-10-25) |
| D5 | Interrupt and IPI steering to TPUs | R_INT_* "TBD" | each TPU gets its own interrupt mask and request bits; IO7 LSI/MSI targets name PID and TPU; IPIs address a TPU. For the EV7-compatible row (S4, where each TPU has a PID of its own); the native track revises the IO7 part (N22: an IO7 target names a chip) |
| D6 | Processor numbers | WHAMI is PALcode's | PID x 4 + TPU, handed to the console's PALcode at entry the way the PID is today; a one-TPU chip keeps today's numbers. **Replaced** by the owner on 2026-10-25 after S4's probe showed it wrong ([S4](#s4-2026-10-25)): TPU 0 keeps the chip's PID, TPU t is PID \| t << s, s the lowest pair of PID bits no configured chip uses |
| D7 | TPU start-up | WAKEUP entry, no boot document | TPUs 1-3 start like a Marvel secondary (RBOX_SCRATCH1 polling) in EV7-compatible mode |
| D8 | Short-vector SIMD results | per-instruction architectural results | define each from the IDS's datapath description and the existing MVI conventions; publish the table in cpu-fidelity.md. Proposed in [ev8-isa.md](ev8-isa.md), 3.3 |
| D9 | Paired single precision | the 21264-style trap shadow and denormal handling for pairs | follow the scalar IEEE rules per half, as [IDS p. 280] says, with the existing FPCR code |
| D10 | Tarantula encodings | all | opcodes 01-06: one per group (VV, VS, SM, RM, VC, and one spare), FP-operate-like fields, a mask bit; DrainM as an 18.xxxx code. Encoded in [ev8-isa.md](ev8-isa.md), section 4 (integer-operate fields; DrainM 18.5000) |
| D11 | Tarantula enable and context switch | no PALcode or OS support defined | a per-TPU vector enable, off at reset, a CSR in each EV7's register window (owner, 2026-10-24; [ev8-isa.md](ev8-isa.md), section 2), so an OS that does not know vectors (OpenVMS) gets OPCDEC and never shares vector state between processes; a kernel that does (Linux) enables it and saves v0-v31, vl, vs, vm lazily |
| D12 | Vector ordering | against MB/WMB and other processors | MB and WMB order vector accesses like scalar ones; within an instruction, any element order; DrainM as published |
| D13 | Vector FP | rounding, FPCR, traps | FPCR shared with scalar FP; dynamic rounding from the FPCR; traps precise at the instruction (as published), FPCR status ORed over elements |
| D14 | Tarantula detection | no AMASK bit | AMASK bit 3 (decided, owner, 2026-10-24), the first free bit of the low byte reserved for standard extensions [ARM p. 204]. Kept in [ev8-isa.md](ev8-isa.md), section 2, with the vector enable |
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
with no 21264 system bus. (Since 2026-10-26 the ES40 takes the `ev8` row,
the EV7-compatible one, as an Alphabox construction, one processor per
chip under its own console:
[cpu-fidelity.md](../cpu-fidelity.md#the-es40-with-ev7-family-processors-an-alphabox-construction-2026-10-26).)

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
| S1 | An `ev8` row on the ES47 board in EV7-compatible mode: IMPLVER 4, AMASK 0x1F07 (with bits for what is implemented), chip ID per D2 | interpreter, JIT (constants only) | console to `P00>>>`; OpenVMS 8.4 boots on the row; cpu_oracle cases for IMPLVER/AMASK | D1, D2; **done 2026-10-24** ([below](#s1-and-s2-2026-10-24)) |
| S2 | The harmless additions: CCB, WH64EN, CMOV2/FCMOV2, MAP_ALIGN as NOPs; unused 18.xxxx codes stay NOPs as on the EV6/EV7 rows (2026-10-24), but for the EV8's own | both | cpu_oracle cases; JIT_VERIFY | **done 2026-10-24**; LDx_ARM and QUIESCE are unused codes, NOPs, until S3 |
| S3 | LDx_ARM and QUIESCE with one TPU per chip, across processors and DMA | interpreter first; the JIT ends a block at QUIESCE and calls the interpreter's routine | a two-processor boot image: one sleeps on a lock, the other releases it; timeout path; interrupt path (D16). Linux: a spinlock patched as in [IDS p. 674] | D16; **done 2026-10-24** ([below](#s3-2026-10-24)) |
| S4 | TPUs as processors: per-TPU state, D5-D7, TPU groups in the TB model (Mode 2 first) | interpreter, then JIT (per-TPU state is per-CPU state) | the console lists 4 x the chips' processors; OpenVMS boots on all of them; Linux SMP | D4-D7; **done 2026-10-25** ([below](#s4-2026-10-25)), with the owner's numbering in place of D6 and `tpus` 1 by default: the TPUs as EV7 nodes are an Alphabox construction, the native interface (S11-S13) does them the IDS's way |
| S5 | The short-vector SIMD set | interpreter, then JIT | cpu_oracle cases from D8's table; a Linux program checked against scalar code | D8; **done 2026-10-26** ([below](#s5-and-s6-2026-10-26)): the JIT calls the interpreter's routine |
| S6 | Paired single precision | interpreter, then JIT | the FP self-test extended (`alphabox_fptest`) per half; cpu_oracle cases | D9; **done 2026-10-26** ([below](#s5-and-s6-2026-10-26)), and AMASK bit 11 with it |
| S7 | Tarantula state, VC and VV/VS groups, SaveState/RestoreState of vector state | interpreter | boot-image kernels (daxpy and the like) checked against scalar results | D10, D11, D13, D14; **done 2026-10-25** ([below](#s7-2026-10-25)): the values go in by VS and come out through a test hook until S8 |
| S8 | Tarantula memory: SM, RM, masks, prefetch to v31, DrainM, faults and restart | interpreter | page-crossing and faulting vectors, scatter conflicts, DrainM cases | D12, D15 |
| S9 | Tarantula under an OS: Linux enables vectors and saves them on switch | interpreter | two processes using vectors at once | D11 |
| S10 | Tarantula in the JIT (helpers first, host SIMD later) | JIT | JIT_VERIFY extended to compare vector state; any speed claim through `perf_ab.py` | S7-S9 |
| S11 | The native EV8 privileged interface (option B), from the IDS: its IPR table, RET for PALmode exit, IFETCHB, the HW_MFPR/HW_LD layouts, SuperPALmode, the new PALcode entry offsets, 52-bit VA and 48-bit PA, TPU groups in the TBs, the TPUs the IDS's way (one network node, one register window per chip, the TPU from TPU_CNFG<57:56>, started through WAKEUP); every TBD and conflict settled by inference, marked | interpreter, then JIT | test/cpu cases per register and trap, written from the IDS; a native-mode `ev8n` row beside the EV7-compatible one | S1-S4; **done 2026-10-25** ([ev8-native.md](ev8-native.md), "S11"): the `ev8n` row, its TPUs started at 0xB00 and parked by PALcode (N17) |
| S11b | The Marvel model in 21464 mode: the ES47, ES80 and GS1280 with `ev8n` processors, the 48-bit map, dense PIDs, interrupts steered to the TPUs, the four-deep queue, 32-bit GIO | system logic, the same on every lane | a boot image on the three layouts; every EV7 board check unchanged | S11, N18-N23; **done 2026-10-25** ([ev8-native.md](ev8-native.md), "S11b"): `test/cpu/marvel8_mp.py`; no console yet, a boot image |
| S12 | An Alphabox PALcode for the native interface (the UNIX/Linux interface first, being the smaller) and a minimal console that builds the HWRPB and loads a kernel, on the Marvel CMM and IO7 models | PALcode in Alpha assembly, built by the project | boot-image tests of every PALcode entry; the console prompts and loads aboot or a kernel | S11 |
| S13 | Linux on the native EV8: SMT as TPUs, then the new instructions and Tarantula under the native interface | both | Linux to a shell, SMP across TPUs, the S3-S9 tests repeated in native mode | S12, S5-S9 |

Binutils knows none of the new instructions, so test programs carry them
as `.long` words until a small assembler patch exists (out of tree).

The native track (S11 to S13, and the Marvel model in 21464 mode between
S11 and S12) is planned in [ev8-native.md](ev8-native.md), with its
decisions N1 to N31 (accepted by the owner on 2026-10-25) and the stages
refined.

The CPU oracle runs any row on the ES40 board (`cpu_oracle.py --cpu`,
[development.md](../development.md)), so the S1 to S10 cases run with
`--cpu ev8` once the row exists and has its model in `test/cpu/rows.py`.

## S1 and S2 (2026-10-24)

**The row** (S1). `ev8` in `cpu/CpuModels.cpp`, the part "21464 (EV8)":
IMPLVER 4 [IDS p. 76], AMASK 0x1307, chip ID 4 with type 15 minor 5 (D2),
L1 caches of 64 KB and a 3 MB L2 [IDS pp. 42-43]. Its family is
`CPU_FAMILY_EV7`: in EV7-compatible mode the address map, the start-up and
the interrupts are the EV7's, and those are what the family decides
(`cpu/CpuModel.hpp`). So the board check (`CAlphaCPU`'s constructor, and
the configurator's check mode) takes it on the ES47, ES80 and GS1280 and
refuses it on the Tsunami and Titan boards, with no test hook. What the
EV8 adds is a new field, `cpu_model::ext` = `CPU_EXT_EV8`, which the
decoders test; each later stage decodes more of the 21464's instructions
under it. A native EV8 (S11) would be a family of its own. Both JIT back
ends already took IMPLVER and AMASK from the row (`set_cpu_identity`).
`alphabox configure` offers `ev8` on the EV7 boards, at 1400 MHz by
default, the IDS's clock [IDS p. 42].

**AMASK, stage by stage** **[decision]**. The row reports only what it
executes, so that software testing AMASK never uses a missing feature:

| Bits | Feature | Reported |
| --- | --- | --- |
| 0-2, 8, 9, 12 | BWX, FIX, CIX, MVI, precise traps, prefetch with modify intent | now: the core's, 0x1307 |
| 10 | SMT: LDL_ARM, LDQ_ARM, QUIESCE | since S3 (0x1707), the instructions software tests it for [IDS pp. 668, 674]; S4 brings the TPUs its name describes |
| 11 | the 1C SIMD set and paired single (owner, 2026-10-24) | since S5 and S6 (0x1F07), once both executed |
| 3 | Tarantula (D14) | once its instructions execute, after S7 and S8 |

The IDS's full 0x1F07 is reached with bit 11 (S6); 0x1F0F, with
Tarantula, once bit 3 is on.

**The harmless additions** (S2), on the `ev8` row only, as
[ev8-isa.md](ev8-isa.md) 3.1 and 3.2 give them: CCB (18.EC00) is a no-op,
which no address faults (as ECB, **[inference]**); CMOV2 (11.68) copies Ra
to Rc, which for the CMOV2 Rc,Rb,Rc the chip makes is Rc to Rc; FCMOV2
(17.068) the same in the FP registers, with the FEN fault of an FP operate
instruction when the FPU is disabled (**[inference]**: the IDS says nothing
of it). WH64EN and MAP_ALIGN were no-ops on every row already. Both JITs
compile the three on the row only: `classify()` takes the row's `ext`,
and elsewhere the words stay the interpreter's. On the other rows nothing
changed: 11.68 and 17.068 raise OPCDEC, 18.EC00 is an unused code.

**LDL_ARM, LDQ_ARM and QUIESCE until S3** **[decision]** (S3 implements them, [below](#s3-2026-10-24)): no-ops. Until S3
they are no instructions of the row, so the row's rule for an unused
18.xxxx code applies, the 21264's [IDS p. 676]. AMASK bit 10 is clear and
software must test it before using them [IDS pp. 668, 674-675], so a
program that follows the IDS never runs them, and one that does not gets
what an EV7, or a real 21264, gives it (Ra is not written). The IDS's own
designers wanted the codes to trap on older processors and gave that up
because the 21264 ignores them [IDS pp. 675-676]; a trap here, for a row
that is a 21264 core underneath, would be neither the old processors'
behaviour nor the 21464's.

**What the console and the operating systems make of it.** The row looks
to all three like an EV7z, and IMPLVER 4 changes nothing they do. Where
each executes IMPLVER and AMASK was found with a build that printed every
such instruction (its PC, mode and result; not committed), on both rows.

- The ES47 console reaches `P00>>>`; `show config` prints each processor as
  "1.75 MB Cache EV7 rev 3.0, 1000 MHz" and `show cpu` "Type Major 15,
  Minor 5", as for an `ev7z` row; the ES80 and GS1280 the same; `init`
  restarts every processor (es47 2, es80 and gs1280 4). The row's
  reference listing is in `lab/runs/ev8-s1-reference/`. The console
  executes neither IMPLVER nor AMASK, as E1 found by reading
  ([marvel.md](marvel.md)).
- OpenVMS 8.4 (the M7 installed disk, a clone) boots to DCL on both
  processors. `SHOW CPU/FULL` reads "Type: EV7 (21364), Minor = 5", from
  the HWRPB the console wrote; on the `ev7` row it reads Minor = 3, and
  nothing else differs. OpenVMS executes IMPLVER at three places: in its
  primary bootstrap (VA 0x20078478), at 0x6A988 during the boot, and in
  the executive at 0xFFFFFFFF80012178, half a million times in a boot,
  beside an AMASK; every AMASK it executes tests BWX. On the `ev7` row the
  same places run about as often, so it takes no other path for 4 than
  for 3.
- Linux on the ES47 logs in; `uname -a` ends "alpha EV7 Marvel" and
  `/proc/cpuinfo` says "cpu model: EV7", "cpu variation: 7", "L2 cache:
  1792K, 7-way", the same as on the `ev7` row but for the BogoMIPS
  measured. Linux executes IMPLVER at one place, followed by an AMASK of
  CIX: `ELF_PLATFORM`, computed for each program it starts, which for any
  IMPLVER above 1 with CIX present is "ev67", on both rows; the other
  AMASK is `ELF_HWCAP`, 0x1307 on both. The FPU and xor paths that test
  for IMPLVER 2 did not run.

**The proofs**, on S11b (the Marvel model in 21464 mode), which S7 was
rebased onto. Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT). The CPU
oracle, on every one of them: `ev68cb` and
`ev7` PASS 20171, KNOWN 61, FAIL 0, with 211 results skipped (the EV8's
words); `ev8` PASS 20372, KNOWN 61, FAIL 0, with 10 skipped (the cases
that need 11.68, 17.068 and 18.EC00 unused); JIT_VERIFY 0 mismatches.
main's own oracle gives the same verdicts on this branch's binaries as on
main's (PASS 20138, KNOWN 61). A deliberately wrong CMOV2 in the JIT
failed 26 results of the compiled pass on both back ends. The SRM
console-log check is clean on the interpreter, JIT and JIT_VERIFY lanes
(JIT_VERIFY: 0 mismatches); `boards_check.sh` passes its 12 boards and
three `init` checks, with `--diff` against main the same on every board
(the DS15's measured clock once printed 787 MHz under host load, and 1000
MHz again on a second run). `guests_check.sh` passes `vms-es47`,
`linux-es47`, `vms-es40` and `linux` on the existing rows, and
`vms-es47` and `linux-es47` on the `ev8` row (`GUESTS_EV7_CPU=ev8`, with
`SHOW CPU/FULL` and `cat /proc/cpuinfo` added through
`GUESTS_EXTRA_CMDS`).

**Findings.**

- The IDS's decode tables use don't-care bits, so a 21464 has no unused
  18.xxxx code: 18.D000 decodes as RPCC, 18.E400 as RC, 18.F400 as RS
  [IDS p. 661]; 11.68 to 11.6B and 11.78 to 11.7B are CMOV2 [IDS p. 655];
  opcode 17 is decoded on the low bits of its function, so more codes than
  17.068 are FCMOV2 [IDS p. 660]. The row decodes the exact codes and keeps
  the core's rule for the rest **[decision]**; S3 has the same choice to
  make for LDx_ARM (18.B000 to 18.B3FF are LDL_ARM in that table).
- The probe D2 asked for, what the console does with a chip ID its table
  does not name (two builds not committed, the `ev8` row with chip ID 5 and
  0x3F, on the ES47): it still reaches `P00>>>`. Chip ID 5 prints "EV7,
  1000 MHz" with no revision and `show cpu` "Type Major 15, Minor 0"; 0x3F
  prints "EV79" and "Type Major 16, Minor 0", Linux's EV79_CPU. The row
  keeps chip ID 4, "EV7 rev 3.0", as D2 proposed; nothing in the console
  calls for another value.
- [ev8-isa.md](ev8-isa.md) sections 1 to 3 needed no correction for S1
  and S2: its example words are what `test/cpu/alpha_asm.py` assembles, and
  the oracle checks that they agree.

## S3 (2026-10-24)

LDL_ARM, LDQ_ARM and QUIESCE on the `ev8` row, one TPU per chip, across
processors and DMA (D16); AMASK bit 10 on, 0x1707. Every other row keeps
18.B000, 18.B400 and 18.B800 as unused codes, no-ops. The code is in
`cpu/cpu_memory.hpp` (`READ_VIRT_ARM`), `cpu/AlphaCPU.cpp` (`watch_arm`,
`quiesce`) and `system/System.cpp` (`cpu_arm_watch` and its neighbours);
every decision below is in [cpu-fidelity.md](../cpu-fidelity.md) too.

**What the IDS settles.**

- LDx_ARM writes Ra with the loaded value (sign-extended for LDL_ARM) and
  arms only "when [it] is executed without faulting" [IDS pp. 669-670]:
  a DTB miss, an access violation or an alignment fault leaves the watch as
  it was, and the fault is LDQ's, reported for va. Here that is this
  core's LDQ, which takes the alignment fault only across a page
  ([cpu-fidelity.md](../cpu-fidelity.md), the first known divergence).
- QUIESCE is not privileged: the instructions "may be used either in
  PALmode or in normal mode" [IDS p. 668], and QUIESCE has no exceptions
  [p. 672]. The Linux program below runs both in user mode.
- An interrupt directed to a quiesced processor clears watch_flag, and an
  LDx_ARM does not arm while one is pending [IDS pp. 677-678]; REI, rti,
  rfe, retsys and urti clear it, the other CALL_PALs may [p. 670]; the
  quiescent period ends with the flag clear [p. 673]; a second QUIESCE never
  sleeps [p. 671].

**What Alphabox decided or inferred** (the gaps the IDS leaves).

- The watched range is the 64-byte line the LDx_L lock watches, which the
  IDS requires it to be [p. 670].
- A processor's own store to its watched line does not clear its flag
  **[inference]**: the IDS names "Processor A ... in Processor B" [p. 670].
- LDx_ARM is performed whatever Ra, as LDx_L, and counts as a memory
  operation for a pending STx_C, as LDQ **[inference]**, "as similar as
  possible to the LDx_L instructions" [p. 669].
- "An interrupt pending" is one the processor would take outside PALmode
  (enabled in IER), in any mode **[decision]**; it ends a QUIESCE and
  refuses an arm. An LDx_ARM to an address outside memory does not arm
  **[decision]**: no store there can be seen.
- Every CALL_PAL clears the flag **[decision]**, so no PALcode's function
  codes have to be known (REI is OpenVMS's, rti, retsys and urti the
  UNIX PALcode's), and so does every exception or interrupt that clears
  the load lock, all but the TB-miss fills **[decision]**.
- The timeout is QUIESCE_TIMEOUT's reset value, 10240 cycles, at the
  processor's configured clock (10.2 us at the ES47's 1000 MHz, 7.3 us at
  the 1400 MHz `alphabox configure` gives the row) **[decision, D16]**.
  `ALPHABOX_QUIESCE_TIMEOUT` changes it, for tests ([headless.md](../headless.md)).

**How the wait works.** watch_flag is a bit per processor in
`CSystem::m_cpu_watch_flags`, beside the load-lock flags, with the
address it watches. LDx_ARM publishes the flag (a sequentially consistent
read-modify-write) and then reads the line, both inside the LL/SC reader
guard; every store from another processor (the interpreter's, the JIT's
helpers', a successful STx_C) fences and then looks at the flags, and a
DMA write looks at them inside the writer guard before the transfer. So a
release is either seen by the LDx_ARM's own read, and software does not
QUIESCE, or it clears the flag. A store that clears one wakes that
processor. QUIESCE, with the flag set, waits on the processor's condition
variable under its own mutex, the flag tested under the mutex and every
waker clearing the flag before it looks whether anyone sleeps: no wake-up
is lost. The wait ends at the timeout at the latest (a host that sleeps
makes it end late, never fail), and at once for an interrupt the
processor would take (`irq_h` notifies it: devices, the interval timer,
IPIs), a delayed interrupt on its way, `stop_threads` (exit, SIGTERM, a
snapshot, the BREAK menu, a reset) and a reset request, and a write by
another processor to its RBOX_SCRATCH1 (the start hand-off of `init`, M11).
Processor 0 drives the machine's interval timer from its own loop, so it
fires its ticks from inside the wait and is woken by its own. Anything
else is looked at within a millisecond.

Compiled code stores into memory through translations it caches, and
does not look at the flags. So a page an LDx_ARM has watched is, like a
page that holds compiled code, never cached as inline-writable; the first
watch on a page asks every processor to drop its cached translations, and
a QUIESCE does not sleep until every running processor has (it fails
instead, which the IDS allows, and the guest tries again). Pages stay
marked.

**The snapshot.** The flags are not in the state file. A machine stopped
for a snapshot ends every QUIESCE, and a restore leaves every flag clear:
a wake-up the IDS allows, since software tests its lock again after a
QUIESCE [p. 674]. No format change.

**The JIT.** Both back ends leave the three instructions to the
interpreter (a block ends before each; QUIESCE's wait is the
interpreter's). A compiled CALL_PAL on the row clears the flag, and on a
machine with an `ev8` processor the AArch64 HW_ST physical forms store
through the helper. JIT_VERIFY: 0 mismatches.

**The tests.**

- `test/cpu` group `quiesce` (`--cpu ev8`; skipped elsewhere but for AMASK
  bit 10's test): the loads, their faults, QUIESCE with the flag clear,
  QUIESCE to the timeout and a second one, the arm cleared by a CALL_PAL
  (compiled, in the last pass) and by an exception. A compiled CALL_PAL
  without the clear failed its case in the last pass, on both back ends.
- `test/cpu/quiesce_mp.py`: two `ev8` processors on the ES40 through the
  test hook, the image's own PALcode, processor 1 started through the
  dual-port RAM as the ES40's console starts one; timeout 2^28 cycles.
  Processor 1 waits on a lock the IDS's way while processor 0 releases it
  2^24 cycles (21 ms) later: woken by the store, by the STx_C, by an IPI
  (taken at 0x680), and by a sym53c810's SCRIPTS Memory Move into the lock
  (DMA, through a direct-mapped PCI window), each after the release and
  long before the timeout; with nobody writing, at the timeout (336 ms).
  `ALPHABOX_TRACE_QUIESCE` counts one wake-up of each cause. Passed on the
  interpreter, JIT, JIT_VERIFY and x86-64 lanes.
- Linux on the ES47 with two `ev8` processors (`test/tools/es47_quiesce.py`,
  a clone of the installed disk): `test/cpu/quiesce_lock.c`, built by the
  guest's gcc with the two instructions as `.long` words, runs two threads
  pinned to the two processors, each taking a lock 20000 times the IDS's
  way and holding it a little: the count is exact (40000), both finish,
  and the threads executed 4277 and 3792 QUIESCEs. `ALPHABOX_TRACE_QUIESCE`
  counted 3778 and 4266 wake-ups by the other processor's store, 6 and 4 by
  its STx_C, 3 by an interrupt and 2 by the timeout; `poweroff` halts.
  Run kept in `lab/runs/ev8-s3-linux/`.

**What the operating systems make of bit 10.** Linux reports it to
programs: the program's `AT_HWCAP`, Linux's `ELF_HWCAP` (the AMASK of
every bit), reads 0x1707, where S1 found 0x1307. Its other AMASK, of CIX
for `ELF_PLATFORM` (S1), does not look at bit 10, and Linux boots, logs
in and powers off as before. Every AMASK OpenVMS 8.4 executes tests BWX
(S1), so bit 10 changes nothing it does; it boots to DCL on the row as
before, and `SHOW CPU/FULL` still reads "EV7 (21364), Minor = 5".

**The proofs**, on S11b (the Marvel model in 21464 mode), which S7 was
rebased onto. Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT). The CPU
oracle on the interpreter, JIT, JIT_VERIFY and x86-64 lanes, alike on
all four: `ev68cb` and `ev7` PASS 20176, KNOWN 61, FAIL 0, SKIP 286 (S2's
20171 and 211, and group `quiesce`: AMASK bit 10's case and the harness's
checks run, its other 75 results skipped); `ev8` PASS 20428, KNOWN 73, FAIL
0, SKIP 22; JIT_VERIFY 0 mismatches. `quiesce_mp.py` passed on the same
four lanes. The SRM console-log check is clean on the interpreter, JIT and
JIT_VERIFY lanes (JIT_VERIFY: 0 mismatches); `boards_check.sh` passes its
12 boards and three `init` checks, the same as main's gate run on every
board (`--diff`); with `BOARDS_EV7_CPU=ev8` the ES47, ES80 and GS1280 and
their `init` checks (the GS1280's on four processors) pass, the same as
S1's run on the row but for the `show cpu` that run added;
`snap_roundtrip.sh` on the ES47 with the row: the state files identical,
the console answering after the restore. `guests_check.sh` passes
`vms-es47` and `linux-es47` on the `ev8` row, and `vms-es47`,
`linux-es47`, `vms-es40` and `linux` on the existing rows.

**Findings.**

- On the x86-64 JIT (under Rosetta), processor 0, spinning in one
  compiled loop, dropped its cached translations after the first watch on
  a page only when it left the loop, and processor 1's QUIESCEs failed
  until then (4952 times in one run of `quiesce_mp.py`; once on AArch64):
  the price of the first arm on a page, never a lost wake-up.
- [ev8-isa.md](ev8-isa.md) 3.1 needed no correction: its words are those
  `test/cpu/alpha_asm.py` assembles and the Linux program carries, and its
  notes are the IDS's.

## S4 (2026-10-25)

The 21464's thread processing units as processors of their own on the
Marvel boards, in EV7-compatible mode: `tpus = 2..4` in each `ev8`
section. The default stays one processor per chip. The code is in
`chipsets/marvel/Topology.*` (the numbering), `system/Configurator.cpp`
(`processor_tpus`: the key and its limits), `cpu/AlphaCPU.cpp` (a
processor per TPU) and `platforms/es47/Cmm.cpp` (what the management side
says of chips); every decision is in [cpu-fidelity.md](../cpu-fidelity.md)
too.

**What the console does with processors** (phase 1, from its code and
[marvel.md](marvel.md)). It learns them from the CMM: the partition
database lists each with its PID and its NS,EW coordinates, the MBM
configuration and the FRU EEPROMs describe modules and RIMMs by an ID it
makes from the coordinates (coord2id), and the memory assignment lists
chunks. It addresses a processor by its PID: the 4 MB register window,
RBOX_SCRATCH1 to start it, RBOX_IREQ to interrupt it. And it already
handles a processor with no memory: `find_nearest_memory` (0x2f2f20)
searches PID ^ {0, 1, 2, 3, 6, 7, 4, 5}, then the same ^ 8, ^ 16, ^ 24,
for a PID that has some, and `start_secondary` copies the PALcode there
and starts the processor at that memory + 0x30000. So D4 and D7 needed
nothing of the console.

**The owner's decisions of 2026-10-25.**

- **The numbering, in place of D6** **[decision]**: TPU 0 keeps its chip's
  PID, so its memory, its IO7 and the IDs the console makes from its
  coordinates are those of the EV7 it stands in for; TPU t is the chip's
  PID | t << s, s the lowest pair of PID bits no configured chip's PID
  uses (`CMarvelTopology::tpu_shift`). On the ES47 and the ES80, s = 1:
  the module field PID<2:1>, which their 2P drawers leave 0, so TPU t is
  PID + 2t; on a GS1280, whose drawers use every PID, s = 2 with four
  chips and 3 with eight. A chip of one TPU keeps today's number, every
  number stays below 32 within the 32-processor limit, and a machine whose
  chips leave no pair free is refused with the reason (none of the three
  rows can, within 32 processors).
- **The default, `tpus` absent, is 1** **[decision]**. On a real EV8 the
  four TPUs share one network address (R_WHOAMI [IDS p. 527]) and differ
  only in TPU_CNFG<57:56> [p. 488]; under the EV7 console and PALcode,
  which have no WAKEUP entry, TPUs 1-3 would most likely never start
  **[inference]**. So the faithful EV8 in EV7-compatible mode shows one
  processor per chip. TPUs listed as processors, each with a PID and a
  register window of its own, are an **Alphabox construction**, in the
  spirit of "what Alpha could have been": the PIDs and windows of TPUs
  1-3 are invented, while TPU 0 keeping the chip's PID matches R_WHOAMI.
  The native interface (S11-S13) is where TPUs are done the IDS's way:
  one network node and one register window per chip, the TPU from
  TPU_CNFG.
- **VCTL** (D11) is at offset 0x3F0000 of each processor's own window
  ([ev8-isa.md](ev8-isa.md), section 2, revised); on a one-TPU chip that
  is the address the first layout gave.

**Why D6 was replaced** (the probe, phase 1). A probe build listed TPUs
1-3 as processors with D6's numbers (PID x 4 + TPU) and no memory or IO7.

- ES47, two chips of four TPUs (PIDs 0-7): `P00>>>` in 41 s; `show config`
  lists PIDs 0-7, TPUs 1-3 with "No Local Memory" and "No Local I/O";
  `show cpu` CPUs 0-7; the TPUs started at 30000 and 2000030000, their own
  chip's memory, through RBOX_SCRATCH1; `init` twice brought all eight back
  each time, HWRPB slots 0-7 valid. OpenVMS 8.4 reached DCL with SHOW CPU
  "Active 0-7". Linux brought up eight CPUs, but with D6 TPU 0 of chip 1 is
  PID 4, whose memory is at 128 GB: past the 32 GB Linux uses on Marvel
  (`setup_memory`, "memory on their first node ... or 2 (marvel)"), and its
  memory map for 128 GB took 1 GB of chip 0's 2 GB ("Mem: total 1090" of 4
  GB). D6 cost Linux every chip's memory but the first.
- The ES80 layout (chips 0, 1, 8, 9: D6 numbers 0-7 and 32-39, memory at
  0, 128 GB, 1 TB and 1.125 TB): the console was content (16 processors,
  `init` twice, HWRPB slots 0-7 and 32-39); Linux was not. aboot halted
  right after "valid disklabel found", where the same disk boots on the
  same layout with `ev7` rows. Beyond that, Linux's NR_CPUS is 32
  (`kernel_max` 31), its MARVEL_MAX_PIDS is 32, and the console's GCT names
  the second drawer's IO7 by its coordinate ID, 8 (`build_iop_hw` uses
  coord2id; Linux takes an IO7's PE from the GCT, `marvel_io7_present`),
  while D6 put it at PID 32.
- The cause: D6 moves each chip's TPU 0 off the chip's own PID, so for
  every chip but PID 0 the addresses the console forms from a PID (memory
  at `pid2membase`, the IO7 and the register window at its PE) disagree
  with the IDs it forms from the coordinates. The numbering that replaced
  it, probed the same way on the ES80 layout (four chips of four TPUs):
  `show config` gives each TPU 0 its chip's PID, Hard ID and memory, `init`
  twice brought sixteen back (HWRPB slots 0-15), and Linux brought up 16
  CPUs, found the IO7s at PIDs 0 and 8, and ran 16 `xz` round trips with
  the file's MD5; OpenVMS on the ES47 reached "Active 0-7".

The probe's runs: `lab/runs/boards-ev8-s4-probe1` (ES47, D6),
`boards-ev8-s4-probe-es80` (ES80, D6), `guests-ev8-s4-probe-vms` (OpenVMS,
D6), `ev8-s4-probe-linux-es47-31` (Linux on the ES47, D6, 2 GB per chip),
`ev8-s4-probe-linux-es80-d6` (aboot's halt), `boards-ev8-s4-probe-es80-b`,
`ev8-s4-probe-linux-es80-b` and `guests-ev8-s4-probe-vms-b` (the numbering
that replaced it), and the probe build as `ev8-s4-probe/probe-build.patch`.

**What a TPU is** (with `tpus` above 1).

- **A processor**: a `CAlphaCPU` of its own, made from its chip's section
  (named `cpu0.tpu1(ev8)` and so on), so its registers, IPRs, TBs, cycle
  counter, LL/SC lock and LDx_ARM watch flag are a processor's. That is
  Mode 2, every TPU in a TB group of its own [IDS p. 306]: to an operating
  system four SMP processors whose only coupling is performance. Mode 3
  (shared TBs) is the native interface's (S11). The processors are
  numbered chip by chip, TPU 0 first.
- **Its PID** (above) is handed to the console's PALcode in r28 at entry,
  as an EV7's is (`ev7::xsrom_handoff`), and is what WHAMI returns and
  what every CSR address the console forms is made of.
- **Start-up** (D7): TPUs 1-3 are parked on their RBOX_SCRATCH1 like any
  Marvel secondary; the console starts them at their chip's memory, and
  `init` restarts them through the start hand-off (StartHandoff.hpp).
- **Interrupts** (D5): each TPU's register window has its own RBOX_INT,
  RBOX_IMASK, RBOX_IREQ and RBOX_IT, so the interval timer ticks each, an
  IPI is a write to the target TPU's RBOX_IREQ, and an IO7 LSI or MSI goes
  to the PID its control register names, which may be any TPU's. The GIO
  (management) interrupt goes to the TPUs whose GIO port enables it: the
  per-port interrupt enables in the PALcode's state word are the mask "a
  mask register will define which TPU(s) are to receive" [IDS p. 476]
  calls for **[decision]**.
- **Memory and I/O** (D4) are TPU 0's. The CMM's memory assignment lists
  a chunk per chip, and its MBM configuration and RIMM EEPROMs describe
  chips by the coordinate ID the console reads them with, which every TPU
  of a chip shares; TPUs 1-3 get "No Local Memory" and "No Local I/O". The
  route between two TPUs of a chip ends in their shared Cbox, as a route
  to itself does (bit 8, [marvel.md](marvel.md), M6a) **[decision]**.
- **Limits.** Every chip of a machine has the same count, 1 to 4; `tpus`
  above 1 is refused off the Marvel boards (where an `ev8` runs only
  through the ALPHABOX_CPU_ANY_BOARD test hook); and a machine has at most
  32 processors, refused at startup and by `alphabox run --check` with the
  reason (a 16-chip GS1280 takes two TPUs each, not four).
- **Raising the limit** would take: `CSystem::kMaxCPUs` and the arrays it
  sizes (`acCPUs`, the LDx_L sequence, value and address per processor);
  the two 32-bit masks, `m_cpu_lock_flags` and `m_cpu_watch_flags`, widened
  to 64-bit atomics with every `1u << cpu` that builds a bit; both JITs'
  compiled CALL_PAL, which clears the processor's watch bit with a 32-bit
  atomic AND (`jitemit_a64.hpp`, `jitengine.cpp`); CEv7Csr's 32-entry
  trace table. The state file keeps four processors' locks and would not
  change. And a guest would gain little: Linux on Alpha has at most 32
  CPUs (NR_CPUS) and Marvel PIDs below 32 (MARVEL_MAX_PIDS); OpenVMS takes
  64.
- **The JIT** needed nothing: compiled code reaches its own processor's
  state and its own bits of the lock and watch words by processor number.
- **The snapshot**: the state file holds a processor's state per TPU and
  a register block per TPU, in the existing format (version 2.3); a
  machine with TPUs saves and restores as one without.
- **The tools** know the count: `BOARDS_EV8_TPUS` (boards_check.sh,
  marvel_init_check.sh, snap_roundtrip.sh through them),
  `GUESTS_EV8_TPUS` (guests_check.sh: SHOW CPU's active set and Linux's
  "cpus active" must count 2 x TPUs), and es47_quiesce.py `--tpus`, which
  runs `test/cpu/quiesce_lock.c`'s threads on every processor. Unset, they
  write no `tpus` and expect one processor per chip.

**The proofs**, on S11b (the Marvel model in 21464 mode), which S7 was
rebased onto. Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT), and so
did every commit of the series. `test/unit/tpu_topology_test.cpp` checks
the numbering on the ES47, ES80 and GS1280 layouts (every number distinct,
below 32, TPU 0 its chip's PID, a one-TPU chip's unchanged), memory and
the IO7 with TPU 0 only, the route between TPUs of a chip, a register
block and an IO7 interrupt target per TPU, and the refusal of a layout
with no pair free (10103 checks). The configurator's check mode refuses a
16-chip GS1280 of four TPUs (64 processors) with the reason, at startup
too, and `tpus = 5`, chips with different counts, and `tpus = 2` on the
ES40 under the test hook; `tpus` on an `ev7` warns as an unknown key. The
CPU oracle on the interpreter, JIT, JIT_VERIFY and x86-64 lanes, alike on
all four: `ev68cb` and `ev7` PASS 20176, KNOWN 61, FAIL 0; `ev8` PASS
20428, KNOWN 73, FAIL 0, S3's verdicts; JIT_VERIFY 0 mismatches;
`quiesce_mp.py` passed on the same four lanes (`lab/runs/ev8-s4-oracle/`).
The SRM console-log check is clean on the interpreter, JIT and JIT_VERIFY
lanes (JIT_VERIFY: 0 mismatches, `lab/runs/ev8-s4-srm/`).
`boards_check.sh` passes its 12 boards and three `init` checks, the same
as main's gate run on every board (`--diff`, `lab/runs/boards-ev8-s4-boards/`).
With `BOARDS_EV7_CPU=ev8` and no `tpus` the ES47, ES80 and GS1280 and
their `init` checks pass, the same as S3's run on the row on all three
(`boards-ev8-s4-boards-ev8`). With `BOARDS_EV8_TPUS=4`: the three boards
with eight processors each in `show config` and `show cpu`, and `init`
twice bringing back 8 (ES47), 16 (ES80, chips 0, 1, 8, 9: PIDs 0-15) and
16 (GS1280, four chips: PIDs 0-15), every HWRPB slot valid
(`boards-ev8-s4-boards-tpus4`); the row's reference listing (`show config`,
`show fru`, `show cpu`, `show memory` on the three) is in
`lab/runs/ev8-s4-reference/`. `snap_roundtrip.sh` on the ES47 with four
TPUs per chip: the state files identical (552 MB), the console answering
after the restore (`snap-ev8-s4-tpus4`). `guests_check.sh` passes
`vms-es47`, `linux-es47`, `vms-es40` and `linux` on the existing rows
(`guests-ev8-s4-guests`, `guests-ev8-s4-guests2`), and `vms-es47` and
`linux-es47` on the `ev8` row with no `tpus`, two processors as in S3
(`guests-ev8-s4-guests-ev8`). With `GUESTS_EV8_TPUS=4`, the ES47's two
chips as eight processors:

- OpenVMS 8.4 boots to DCL with SHOW CPU "Active 0-7"; eight DCL loops
  spawned at once (a command procedure counting to 300000) run on all of
  them: SHOW SYSTEM shows seven CUR, on CPUs 1 to 7, and the eighth
  computable while CPU 0 runs the SHOW itself; SHOW CPU/FULL shows a
  process on every CPU; each loop had 20 s of CPU time ten seconds later;
  SHUTDOWN removes CPUs 1-7 from the active set and halts
  (`guests-ev8-s4-guests-tpus4-vms2`).
- Linux logs in with "cpus active: 8", runs eight `xz -1 | xz -d` round
  trips of an 8 MB random file at once, each with the file's MD5, and
  powers off (`guests-ev8-s4-guests-tpus4-linux`).
- `es47_quiesce.py --tpus 4`: the lock program's eight threads, one per
  TPU, take the lock 5000 times each the IDS's way: the count is exact
  (40000), every thread finishes, and they executed 37725 QUIESCEs, woken
  on every TPU mostly by another TPU's store (2340 to 4899 each) or STx_C,
  a few by an interrupt or the timeout (`lab/runs/ev8-s4-quiesce/`).

The ES80 layout (chips 0, 1, 8 and 9, four TPUs each, 2 GB per chip) ran
Linux with this numbering in phase 1, under the probe build: 16 CPUs, the
IO7s found at PIDs 0 and 8, 16 `xz` round trips with the file's MD5, and
`poweroff` (`ev8-s4-probe-linux-es80-b`). Its run on the committed code
waits for a host with the memory for it (swap above 12 GB when this was
written).

**Findings.**

- The Marvel console already knows processors without memory
  (`find_nearest_memory`, `start_secondary`), which is what made D4 cost
  nothing; and it reads its management side by coordinate ID but forms
  addresses from the PID, which is what made D6 wrong.
- [ev8-isa.md](ev8-isa.md) section 2 needed a correction, made in its own
  commit: VCTL was placed in the chip's window per TPU, which S4's
  processors (a PID and a window each) no longer have in EV7-compatible
  mode. The owner's layout (2026-10-25) puts it in each processor's own
  window; the chip-window layout stays for the native interface.

## S5 and S6 (2026-10-26)

The 21464's short-vector SIMD set under opcode 1C (S5, D8) and paired
single precision under opcode 07 (S6, D9), in the interpreter and then in
the JIT, on the `ev8` and `ev8n` rows (both carry `CPU_EXT_EV8`), and AMASK
bit 11, which reports both. The code is `cpu/Ev8Simd.cpp` (the 1C set, one
routine both engines call), `cpu/AlphaCPU_paired.cpp` (opcode 07,
`paired_operate`), the decode in `cpu/AlphaCPU.cpp`, two additions to
`cpu/AlphaCPU_ieeefloat.cpp` (`ieee_cvtfi_n`, CVTTQ's conversion with a
longword's range, for PCVTFI; `m_fp_no_ine`, which drops an estimate's
INE) and the helpers in `cpu/AlphaCPU_jit.cpp`; every decision and
inference below is in [cpu-fidelity.md](../cpu-fidelity.md) too.

**What is implemented, as the sheet gives it.**

- **S5**: every row of [ev8-isa.md](ev8-isa.md) 3.3, in the register and
  literal forms, with the sheet's results: CMPWGE and CMPLGE, PERMB8 with
  its sixteen selectors (13, reserved, reads 0), GPKBLB4, VADD and VSUB
  with SAT in the four zz sizes (the order agreed with nada) and the two
  L2 forms, VMINMAX's running pair, the packs and unpacks, the tree ops
  (TADD, TSUB, TABSERR, TSQERR, TMUL in four sizes, TMULUS in two) as
  exact 64-bit sums, VMULLUW4 and VMULHUW4, and the shifts, one count
  modulo the element width. Nothing traps or reads the FPCR. The other 1C
  codes are what they were on every row: 00, 01, 30-3F, 70 and 78 the
  core's, every other OPCDEC; the new words raise OPCDEC on every row
  without `CPU_EXT_EV8`.
- **S6**: every row of 3.4's table at the 0x080 | Fnc base, under the
  qualifiers its class accepts and OPCDEC for every other Trp and Rnd
  (the estimates take every Rnd alike). Each half is the scalar S
  instruction with the word's own Trp and Rnd and the FPCR, through the
  core's IEEE routines: ADDS, SUBS and MULS for P-ARITH, the operands by
  the table (the cross and horizontal forms), PMULLN and PMULHN negated
  after rounding; the compares as CMPTxx, NEQ and UN as EQ, writing IEEE
  1.0 or a true zero; PCVTFI as CVTTQ at 32 bits; the estimates as DIVS of
  1.0 by the operand (SQRTS for the root) but INE, exact and chopped to 14
  fraction bits whatever Rnd is, the H and L forms' other half 0 and
  unread; the bit manipulations with no checking. A half is read as LDS
  maps it and written as STS packs it. Each half's exceptions set their
  FPCR status bits; both halves are always computed and written; a trap is
  the scalar instruction's on this core, EXC_SUM the OR of both halves'
  reports with SWC for /S and REG = Fc (on `ev8n` the 21464's register
  fields), EXC_ADDR what a scalar ARITH gives (on `ev8` the trigger's PC,
  which the PALcode turns into the next; on `ev8n` the next, section 5).
  FEN with the FPU disabled, after OPCDEC for a code the sheet does not
  assign. Opcode 07 stays OPCDEC on every other row.
- **AMASK bit 11** on both rows, in the commit after S6's: 0x1F07, the
  IDS's value. Bit 3, Tarantula's, waits for S8.

**What Alphabox decided or inferred** (the gaps the sheet leaves).

- **CMPLGE** compares unsigned, as CMPWGE **[inference]**: the sheet
  gives it "by analogy".
- **The unpacks** read Rb only and ignore Ra, as this core's UNPKBW
  **[decision]**: "the operand as MVI's".
- **VMINMAX** reads only the element-size low bits of each of Rb's
  longwords **[inference]**: "each as an element-size value in the low
  bits of its longword".
- **The unary paired forms** (PARCPLx, PARSQRTx, PCVTFI, PEXTL, PEXTH)
  ignore Fa **[decision]**, as this core's CVTxx do; its SQRTx alone raise
  OPCDEC for Fa != 31.
- **PARSQRTx of +-0**: the infinity of the zero's sign, as 1.0 /
  SQRTS(+-0) gives it, and nothing raised, as SQRTS raises nothing for a
  zero **[inference]**: the sheet gives the case no value. IEEE 754's
  rSqrt signals division by zero there: a point for the owner and nada.
- **An estimate of a NaN** is the NaN quieted, its fraction not chopped
  **[inference]**: the chop is of "the exact reciprocal". After a
  denormal operand's trap the result is UNPREDICTABLE: the reciprocal
  what DIVS computes, the root +inf **[decision]**.
- **PFMAX and PFMIN**: "the reverse" is the same compare with the
  results swapped, (faX < fbX) ? faX : fbX, so for equal operands (+0 and
  -0) PFMAX gives faX and PFMIN fbX **[inference]**; an unordered pair
  gets the NaN quieted, Fb's first, ADDS's result for NaN operands
  **[inference]** ("the result the scalar rules give one"); an ordered
  one the operand's bits as they are.
- **PMULLN and PMULHN** flip the sign of whatever the rounded product is,
  a NaN's too, so an underflow's true zero becomes -0 **[inference]**
  ("negated after rounding").
- **Fc = F31** writes nothing and still raises the exceptions, as a
  scalar instruction does.

**The JIT.** First the check the brief asked for: on the AMASK commit
`classify()` compiled no new word on either back end (it returned OP_NONE
for every 1C code but the core's and for all of opcode 07), so a block
ended before each and the interpreter ran it; groups `simd` and `paired`
passed on the JIT_VERIFY lane so. Then both compiled, on the rows with
`CPU_EXT_EV8` and for the codes the sheet assigns only (`ev8simd::assigned`,
`ev8paired::assigned`): OP_SIMD calls `ev8simd::exec` through
`CAlphaCPU::jit_simd` with Rav and Rbv as compiled code holds them and
writes Rc; OP_PAIRED calls `paired_operate` through `jit_paired`, which
bails to the interpreter with the FPU disabled (FEN) and, after a trap,
leaves through the ARITH entry as a compiled FLTV does (the trap told by
`state.pc`, which only GO_PAL changes). Neither ends the block. A
deliberately wrong helper (the SIMD result's bit 0 flipped, Fc's bit 0
flipped after a paired instruction) failed 6744 and 6789 results of the
compiled pass on the AArch64 JIT (`ev8`, `ev8n`), 6739 and 6780 on the
x86-64 one, and none of the interpreted pass. Inline host SIMD is a later
stage; no performance claim and no switch.

**The tests.**

- `test/cpu` group `simd` (`simd_cases.py`, the expected values from the
  sheet's definitions): every 1C row in register and literal forms over a
  cross of edge values, saturation at both ends in every lane, signed and
  unsigned, every PERMB8 selector in every byte and the sign fill, the
  tree sums with carries out of every element width, VMINMAX's running
  pair from its start values and its unread bits, the shift counts, the
  value an operate leaves for the next instruction; the new words OPCDEC
  on the rows without `CPU_EXT_EV8` and the free codes OPCDEC on every
  row. 5,798 results on `ev8` and `ev8n`, 518 elsewhere.
- `test/cpu` group `paired` (`paired_cases.py`, the scalar rules per half
  in `sfloat.py`, exact arithmetic from the ARM and the 21264 HRM): a
  sweep of every instruction under every qualifier its class accepts, /D
  under each FPCR rounding mode, on pairs that do not trap; one-instruction
  cases with the trap record, Fc and the FPCR: each exception in one half,
  the other and both, NaNs and infinities per half, denormals (the
  unmaskable trap, DNZ with /S), underflow with /U and the disables (UNFD
  without UNDZ still traps), the estimates' DZE, underflow and unused
  halves, PCVTFI's overflow under the four rounding modes, the compares'
  NaN rules, the bit manipulations with every exception enabled, register
  aliasing and Fc = F31; FEN; OPCDEC for the free Fnc codes and for every
  qualifier a class does not accept, counted in batches, on every row, and
  for every code of the sheet on the rows without `CPU_EXT_EV8`. 3,702
  results on `ev8`, 6,162 on `ev8n` (its MT_FPCR traps are checked too),
  83 elsewhere.
- `alphabox_fptest` (`ctest`): paired single per half against the host
  FPU in every rounding mode, under every FPCR of the scalar sweep, the
  bit manipulations, and every one of the 2048 function codes' decode
  (OPCDEC or not, FEN): 7.2 million cases. A model made wrong on purpose
  (PMULLN not negated, the estimates rounded instead of chopped) failed
  412,158 of them.
- `test/tools/ev8_simd_guest.sh` and `ev8_simd.c`: a Linux program for the
  `linux-es47` guest on the `ev8` row, compiled by the guest's gcc with
  the new words as `.long`, running SIMD kernels (a saturating add, a sum
  of absolute differences, a dot product, a running min and max, packing,
  a byte reverse) and paired-single ones (y = a x + y, a horizontal sum,
  compares, a max, a conversion, a reciprocal) against scalar C.

**The proofs.** Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, JIT_STATS, a headless
JIT), and every commit of the series built (the interpreter, headless JIT
and JIT_VERIFY lanes, configured again at each commit for the source
list). The CPU oracle on the interpreter, JIT, JIT_VERIFY and x86-64
lanes, alike on all four: `ev68cb` and `ev7` PASS 20872, KNOWN 61, FAIL 0
(main's 20273 and the new groups' OPCDEC results, 518 of `simd` and 83 of
`paired`, less two harness results counted once); `ev8` PASS 50638, KNOWN
73, FAIL 0; `ev8n` PASS 53897, KNOWN 70, FAIL 0; the KNOWN counts are
main's; JIT_VERIFY 0 mismatches (4.5 million compiled-block executions on
each EV8 row). Of main's results only the three of group `misc`'s
"opcode 07 raises OPCDEC" no longer run on the EV8 rows, whose word,
0x1C000000, is PMUL/C there. `quiesce_mp.py`, `tpu_mp.py`,
`tarantula_vv.py` on `ev8` and `ev8n`, `codepage_mp.py` and
`marvel8_mp.py` passed on the same four lanes, with no MISMATCH line. The
SRM console-log check is clean on the interpreter, JIT and JIT_VERIFY
lanes (JIT_VERIFY: 113 million compiled-block executions, 0 mismatches).
`boards_check.sh` on the ES47 with the `ev8` row passes its board and its
`init` check, and `--diff` against `boards-gate-jit0508-ev8` gives the same
console: the console never executes AMASK (S1), so bit 11 shows nowhere in
it. `ev8_simd_guest.sh` on the AArch64 JIT: the guest logs in on both
processors, its gcc compiles the program, which reads IMPLVER 4 and AMASK
bit 11 present and makes 2101 checks against scalar C with 0 mismatches.
The runs are in `lab/runs/ev8-s5s6-proofs/`, `boards-ev8-s5s6-es47` and
`guests-ev8-s5s6-linux`.

**Findings.**

- [ev8-isa.md](ev8-isa.md) needed no correction for S5 and S6: its example
  words are what `alpha_asm.py` assembles, and groups `simd` and `paired`
  check them first. Its gaps are the inferences above; the zero's
  reciprocal square root is the one with no basis at all.
- The oracle image's code area is nearly full: on `ev8n` the cases take
  721 KB of the 736 KB `image.py` gives them. The next stage's cases need
  more room (a second image, or the code area moved).

## S7 (2026-10-25)

Tarantula's state and its operate and control instructions, in the
interpreter, on the `ev8` and `ev8n` rows: the vector state of
[ev8-isa.md](ev8-isa.md) 4.1, VCTL<VEN>, the VC group (4.7) and every VV
and VS function (4.4) with the rules of 4.3, and the vector state in the
state file. The memory groups SM and RM, DrainM and AMASK bit 3 are S8's.
The code is `cpu/AlphaCPU_vector.cpp` (the instructions, the window of a
board without EV7 windows), `cpu/Tarantula.*` (the state and its file
section), the decode in `cpu/AlphaCPU.cpp`, VCTL in
`chipsets/marvel/Ev7Csr.cpp`, and one test in `ieee_trap`
(`cpu/AlphaCPU_ieeefloat.cpp`); every decision and inference below is in
[cpu-fidelity.md](../cpu-fidelity.md) too.

**What is implemented, as the sheet gives it.**

- **The state** (4.1), per processor and so per TPU: $vr0-$vr31 of 128
  quadwords ($vr31 reads zero and discards writes), vl (0 to 128), vs (64
  bits, a signed byte stride), vm (128 bits), VCTL<VEN>. All are cleared
  at power-up and by a reset; the sheet leaves them UNPREDICTABLE but VEN,
  and clearing them is its Alphabox choice. A row has it when its
  `cpu_model::ext` has the new `CPU_EXT_TARANTULA`, which both EV8 rows
  have **[decision]**: a flag of its own beside `CPU_EXT_EV8`, since
  Tarantula was a follow-on candidate to the EV8 with an AMASK bit of its
  own. The state is allocated for those rows only and kept outside
  `state`, so no other row's processor, `state` or state file changes.
- **VEN** (section 2, D11). On `ev8` VCTL is the quadword CSR at offset
  0x3F0000 of the processor's own register window, VEN <0>, <63:1>
  RAZ/IGN, reset 0: `CEv7Csr` answers it from the processor's value, so
  any processor can write any processor's VCTL as the sheet places it,
  and TPUs listed as processors (S4) each have their own. On `ev8n` it is
  the IPR 0xAC that S11 stores (N8), and the CSR offset is not decoded.
  With VEN = 0 every word of opcodes 01-06 raises OPCDEC, EXC_ADDR its
  own PC, and changes nothing; it is not reported as an unknown opcode,
  since that is how an OS's lazy switching works (section 5).
- **VC**: SETVL (vl <- MIN(Rbv unsigned, 128), Rc <- vl), SETVS, SETVM
  (vm[i] <- Vb[i]<0> below vl, 0 above), GETVL, GETVS, GETVM (below vl;
  the rest unchanged), the literal forms of SETVL and SETVS.
- **VV and VS**, every function code of 4.4's table, in the register and
  literal forms the table gives: an element is active when i < vl and,
  with M, vm[i] = 1; an inactive destination element is left unchanged
  (the sheet's Alphabox choice, point 7); $vr31 as a destination
  discards the result; integer arithmetic wraps and raises nothing; every
  compare writes the quadword 1 or 0 (point 8); VVMRG and VSMRG select by
  vm for every i < vl.
- **FP** (4.3, D13, section 5, point 11): each active element is the
  scalar instruction with /SUI and dynamic rounding, computed by the
  core's own IEEE routines with that instruction's word: ADDT, SUBT,
  MULT, DIVT and SQRTT /SUID; the compares as CMPTxx/SU (NE as EQ, GT and
  GE as LT and LE); CVTQT/SUID, CVTTQ/SVID, VVCVTTQC as CVTTQ/SVIC; CPYSx
  raise nothing. The FPCR is updated element by element in element order,
  as that instruction would update it once per element. Every active
  element is computed and written, the IEEE default result where one
  trapped; then, if any element would have trapped, the instruction takes
  one arithmetic trap: EXC_SUM the OR of what every element would have
  reported, with SWC (a /S instruction's) and REG 31.
- **The field rules** (4.3): an SBZ field that is not zero, an unassigned
  function code, a literal on a VS code that reads Fb, M on VVMRG or
  VSMRG, M on VC: OPCDEC whatever VEN is. A register field an instruction
  does not use is ignored. Opcode 06 is all OPCDEC.
- **On every other row** opcodes 01-06 raise OPCDEC as before (the
  decode tests the row's state; the words reach `UNKNOWN1` as they did).

**What Alphabox decided or inferred** (the gaps the sheet leaves).

- **The trap's report** **[inference]**: the vector instruction traps as
  the scalar instruction it is computed as would, so EXC_ADDR is what
  this core gives a scalar ARITH: on `ev8` the trigger's own PC, which the
  EV7 PALcodes turn into the next PC (cpu_defs.hpp, ARITH_TRAP), and on
  `ev8n` the next instruction's [IDS p. 485]. The OS therefore sees the
  next PC (point 11) on both rows. The scalar words have Fa, Fb and Fc
  31, which gives REG 31 on `ev8` and the 21464's register fields 31 on
  `ev8n` **[decision]**.
- **"Would trap"** **[inference]**: the scalar instruction's rule on this
  core, the 21264's: an exception the instruction enables and the FPCR
  does not disable, or an FPCR status bit that is clear and has to be set
  (EXC_SUM<SET_x>, "PALcode should set FPCR[x]", 21264 HRM Table 5-9).
  `ieee_trap` collects each element's report instead of trapping while a
  vector instruction runs, and the instruction traps once at its end.
- **FEN** **[inference]**: with the FPU disabled the FP half of VV and VS
  (functions 20-3F, where VS reads Fb and the arithmetic uses the FPCR)
  takes FEN, as each element's scalar instruction would; the integer
  functions and VC do not. The order: OPCDEC for an unassigned encoding,
  then OPCDEC for VEN = 0, then FEN.
- **VV's <12>** **[inference]**: it "must be 0" (4.4) and is no register
  field, the only kind 4.3 lets Alphabox ignore, so it is SBZ: OPCDEC.
- **A literal on VC** **[decision]**: L on SETVM, GETVL, GETVS or GETVM
  raises OPCDEC, as a literal on VS's FP codes does; 4.7 gives L to SETVL
  and SETVS only.
- **SM and RM until S8** **[decision]**: opcodes 03 and 04 raise OPCDEC on
  every row until their stage, which AMASK bit 3, still clear, tells
  software to expect.
- **PALmode** **[decision]**: the instructions execute as in any mode;
  they are UNPREDICTABLE there (section 5).
- **VEN for the CPU oracle** **[decision]**: the oracle runs `--cpu ev8`
  on the ES40, which has no EV7 register windows. Under
  `ALPHABOX_CPU_ANY_BOARD` the processor answers its own VCTL address
  there (a memory range of its own), so the cases set and clear VEN with
  the quadword store an OS would use on a Marvel board, and the window
  layout is the sheet's. The Tsunami ignores PA<42:35> in its I/O space,
  so every PID's window is one address there; each processor reaches its
  own. Of the brief's choices this one needs no hook of its own and
  tests the address software uses; a reset hook would have left VEN = 0
  untestable within one run.
- **How the tests get values in and out** **[decision]**: S7 has no
  vector loads or stores, and the sheet moves no element between vector
  and scalar registers (point 13): a scalar enters through VS, an element
  leaves through memory only. Out therefore needs a test hook,
  `ALPHABOX_TARANTULA_PEEK=1` ([headless.md](../headless.md)): on a board
  without EV7 windows only, the vector registers at offset 0x3F8000 of
  the processor's window (element i of $vrN at 0x3F8000 + 0x400 x N + 8
  x i), the last 32 KB of the 64 KB that section 2 finds unclaimed. The
  boot-image test puts values in by the sheet's route (below); the
  oracle's cases fill registers through the window, so that each case
  tests one instruction.
- **Whose map the window follows** **[decision]**: the processor's own,
  S11b's `ev7::address_map`: the EV7's for `ev8` (PID 0's block at
  0xFFF_FFC0_0000), where the sheet puts VCTL, and the 21464 mode's (N18,
  PID 0's at 0xFFFF_FFC0_0000) for the hook's register window on
  `ev8n`, the map its Marvel boards use. The ES40 folds both into one
  address. On a Marvel board in 21464 mode offset 0x3F0000 of a chip's
  block is TPU 0's R_INT_MASK (N22), and VCTL is never decoded there;
  0x3F8000 is free in that block as in the EV7's.
- **The state file** **[decision]**: Tarantula's state is a section of
  its own after the processor's block (after the native EV8's IPRs on
  `ev8n`): a magic word, the payload's size, VCTL, vl, vs, the mask and
  the 32 registers, a second magic word (32,820 bytes). RestoreState
  reads it when its magic word is there; a file written before S7 has
  none, and restores with the vector state clear and VEN 0 (saying so).
  No version step: rows without Tarantula write and read what they did,
  and an `ev8` file of main's restores.

**The JIT.** Nothing changed: `classify()` compiles nothing of opcodes
01-06, on any row, so a block ends before every Tarantula word and the
interpreter runs it; JIT_VERIFY never meets one, and compares no vector
state. The FP elements call the same IEEE routines the JIT's helpers do;
`m_vec_defer` is set only while a vector instruction runs.

**The tests.**

- `test/cpu` group `tarantula` (`tarantula_cases.py`, the expected values
  from the sheet and, for FP, `tfloat.py`'s exact arithmetic): the sheet's
  example words of opcodes 01-06 raising OPCDEC on every row (VEN 0 on the
  EV8 rows outside the group); VCTL (its RAZ bits on `ev8`, the CSR not
  decoded on `ev8n`), OPCDEC with VEN = 0 for each group and nothing
  changed; every VC instruction in both forms, SETVL from 0 to 2^64 - 1,
  SETVM and GETVM at vl 0, 1, 64, 100, 128; every VV and VS integer
  function in register and literal forms over a rotation of vl 0, 1, 64,
  128 with and without the mask, and three functions at all eight
  combinations reading all 128 elements; SETVL 129, 255, 300; $vr31 as
  sources and destination; the field rules, 45 encodings; every FP
  function with the FPCR's disables set, the four rounding modes on the
  arithmetic and conversions, the FPCR across elements (SET traps once,
  then not), a trapping element with DZE enabled and disabled, past vl,
  under the mask and with its status already set; FEN. 20,674 results
  in 390 cases on `ev8`, 20,999 on `ev8n` (its MT_FPCR traps are checked
  too); 56 on every other row, the OPCDEC words.
- `test/cpu/tarantula_vv.py`, the boot image: an index vector built from
  SETVL, a broadcast, SETVM and VVMRG, values put in one element at a time
  with a one-element mask (VSCMPEQ, SETVM, VSMRG), results out through the
  hook; daxpy, a dot product in T and quadword (the reduction by scalar
  code over the products: the sheet has neither element moves nor a way to
  halve), a masked update merged with VVMRG, and a strip-mined loop over
  300 quadwords in three SETVL passes, each against scalar code on the
  same inputs. Passed on `ev8` and `ev8n`.
- `test/unit/tarantula_state_test.cpp`: the state file's section read
  back whole, absent from an older file (nothing read, the file where it
  was), refused when truncated or damaged, $vr31 zero after a restore.

**The proofs**, on S11b (the Marvel model in 21464 mode), which S7 was
rebased onto. Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT), and so
did every commit of the series. The CPU oracle on the interpreter, JIT,
JIT_VERIFY and x86-64 lanes, alike on all four: `ev68cb` and `ev7` PASS
20236, KNOWN 61, FAIL 0 (S11's 20180 and the 56 OPCDEC results of group
`tarantula`); `ev8` PASS 41106, KNOWN 73, FAIL 0; `ev8n` PASS 41905,
KNOWN 70, FAIL 0; JIT_VERIFY 0 mismatches. main's own oracle (35b8e15's
`test/cpu`) gives main's verdicts on this branch's binary on all four
rows (20180, 20180, 20432 and 20906 PASS). A model made wrong on
purpose (VVSRL, VVMULT under round-minus, a forgotten DZE) failed 333
results of each pass. `tarantula_vv.py` on `ev8` and `ev8n`,
`quiesce_mp.py`, `tpu_mp.py` and `marvel8_mp.py` (its three layouts, S11b)
passed on the same four lanes. The SRM
console-log check is clean on the interpreter, JIT and JIT_VERIFY lanes
(JIT_VERIFY: 0 mismatches). `boards_check.sh` on the ES47 with the `ev8`
row passes, its console and `init` the same as S11's gate run
(`--diff`). From the ES47's own console on the `ev8` row, `examine` and
`deposit` of PID 0's VCTL (0xFFF_FFFF_0000) read 0, then 1 after a
deposit of all ones, 0 after 2, and PID 1's (0xFF7_FFFF_0000) is its
own; on the `ev7` row both read 0, as before (`boards-ev8-s7-vctl-ev8`,
`-ev7`). `snap_roundtrip.sh` on the ES40 and the ES47, and on the ES47
with the `ev8` row: the state files identical, the console answering
after the restore; every part of the ES40's and the ES47's (`ev7`) files
has the size main's has, and on the `ev8` row each processor's part is
32,820 bytes longer, the section. State files written by main's
`build-jit/alphabox` at the console prompt (the ES40, the ES47, and the
ES47 on the `ev8` row) restore on this branch's binary and the console
answers; the `ev8` processors say they found no vector state. The
desktop snapshots `perf_ab.py` and `sect_mips.sh` resume
(`lab/ntsnap`, `lab/mips1/ntsnap`) resume on clones and run a section
to DONE. The runs are in `lab/runs/ev8-s7-proofs/`,
`boards-ev8-s7-*` and `snap-ev8-s7*`.

**Findings.**

- The sheet gives no way out of a vector register but memory (point 13),
  so until S8 any test of the operate instructions needs a hook or the
  FPCR as a side channel; the hook is test-only and dies with S8's
  VSTOREQ as the tests' route.
- The sheet's example words are what `alpha_asm.py` assembles; group
  `tarantula` checks them first. [ev8-isa.md](ev8-isa.md) needed no
  correction for S7.

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
