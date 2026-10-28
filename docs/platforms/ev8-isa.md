# EV8 and Tarantula: the instruction-set contract

This sheet fixes the instructions the Alpha 21464 (EV8) adds to the
architecture, and the Tarantula vector extension, as Alphabox will execute
them and as the nada toolchain will assemble them. The research behind it,
its sources and the owner's plan are in [ev8.md](ev8.md); this sheet
settles that packet's D8, D10, D13 and D14, and the instruction-level
parts of D9, D11, D12 and D15.

**Status**: revision 1, 2026-10-24. The owner's decisions of that day and
nada's review are applied: the vector enable is a CSR, not an instruction,
AMASK bits 3 and 11 are decided, the 1C size order is agreed. No code yet
([ev8.md](ev8.md), plan S2 to S10).
Revision 1.1, 2026-10-26: one result rule in 3.4, no encoding: the
estimate of the root of +-0 raises division by zero (the owner).
Revision 1.2, 2026-10-28: two things section 5 left out, no encoding, as
stage S8 found them (the owner): a vector memory fault's EXC_SUM<REG>,
and what keeping a faulting instruction moving on through its restarts
requires of an implementation.

## 1. How to read it

The sheet is the contract between two readers: the emulator implementer,
who needs every encoding's effect including each UNPREDICTABLE case
Alphabox makes deterministic, and the assembler writer, who needs the
mnemonic, the operand syntax, the bits and the valid qualifiers.

**Status of each instruction row:**

- **final**: the encoding (opcode, function, format, operand fields) is
  fixed by the 21464 Internal Design Specification and was checked against
  the page cited, on the scan and not only its OCR text. **nada implements
  the final rows in its next batch** (`-mcpu=ev8`).
- **agreed**: an Alphabox choice that nada reviewed and accepted
  (Alphabox and nada, 2026-10-24): the size order inside the 1C "zz"
  blocks, PARCPL's code 07.08, the P-EST qualifier class, the RM syntax.
  The assembler and the emulator implement the same thing.
- **proposed**: an Alphabox choice nobody has accepted yet: all of
  Tarantula, DrainM's code, the results of the 1C instructions (D8), every
  opcode 07 qualifier rule the IDS does not state per instruction. Proposed
  rows wait for the owner (nada has no stake in them).
- A final encoding may carry an operation marked **[inference]** or
  **[decision]**: the bits stand; the note says how sure the meaning is.

**What nada's assembler does with the sheet** (agreed, 2026-10-24):

- `-mcpu=ev8` accepts every EV8 mnemonic; `-mtarantula`, only together with
  `ev8`, accepts every Tarantula one, `drainm` included.
- `ldl_arm` and `ldq_arm` refuse a displacement.
- On every `-mcpu` nada also assembles `amask` (11.61) and `implver`
  (11.6C, with the literal 1 in Rb) [ARM pp. 780, 782].
- RM syntax stays `vgathq $vrt, $vrx($rb)`; PARCPL is at 07.08 with no
  alias for the IDS's 07.0A; the P-EST qualifier class is accepted.
- For readers only, no renaming: some 1C names sit close to Tarantula VS
  names (`vsrl2` and `vssrl`, `vsll2` and `vssll`, `vsral2` and `vssra`).
  They are different opcodes and cannot be confused by an assembler.

**Markers**, as in [ev8.md](ev8.md): a bracketed source with a page is a
fact; **[inference]** is derived from the sources but not stated in them;
**[decision]** is Alphabox's choice; **[conflict]** marks sources, or parts
of one, that disagree. IDS pages are PDF pages of the scan, TAR pages the
printed ISCA pages, ARM pages PDF pages of the 4th edition ([ev8.md](ev8.md),
"Sources"). "HRM" is the 21264 EV68CB/EV68DC hardware reference manual in
`lab/datasheets/alpha-cpu/`, by PDF page.

**Notation.**

- Codes are written as the ARM's Table C-1 writes them [ARM p. 779]:
  `oo.ff` an operate instruction's 7-bit function <11:5>, `oo.fff` a
  floating operate instruction's 11-bit function <15:5>, `oo.ffff` a
  memory-function instruction's 16-bit function <15:0>.
- `Rav`, `Rbv`, `Fav`, `Fbv` are operand values as in the ARM; `x.b[i]`,
  `x.w[i]`, `x.l[i]` are byte, word and longword i of a quadword, i = 0 the
  least significant; `V[i]` is element i of a vector register.
- Syntax is GNU as for Alpha, the way nada reads it (nada's
  `doc/asm-spec.md` 6.6): registers written with `$`, the destination last
  in the operate formats and first in the memory formats, a number 0 to
  255 in place of Rb in the operate formats. "Words" are 32-bit
  instruction values in hex, as an assembler stores them (little-endian).

**Unused codes trap.** Opcodes 01 to 07 and 1C were reserved in Version 5
of the architecture, so their unused function codes must raise an illegal
instruction trap (OPCDEC); unused 18.xxxx codes are UNPREDICTABLE [ARM p.
803]. Every code this sheet leaves free in 01 to 07 and 1C therefore
traps, which is what keeps the spare space spare.

## 2. Identification

| | EV8 | Source | Status |
| --- | --- | --- | --- |
| IMPLVER | 4 | [IDS p. 76] | final |
| AMASK feature mask | 0x1F07; 0x1F0F with Tarantula | 0x1F07 [IDS p. 76]; bit 3 **[decision, D14]**, taken by the owner on 2026-10-24 | final; bit 3 decided (owner, 2026-10-24) |

| AMASK bit | Meaning | Source |
| --- | --- | --- |
| 0 | BWX | [ARM p. 808] |
| 1 | FIX | [ARM p. 808] |
| 2 | CIX | [ARM p. 808] |
| 3 | **Tarantula**: opcodes 01 to 05 and DrainM | **[decision, D14; owner, 2026-10-24]**: the first free bit of the low byte, which the ARM reserves for standard extensions "so they can be tested with a literal" [ARM p. 204] |
| 8 | MVI | [ARM p. 808] |
| 9 | precise arithmetic traps: "the trap PC is the same as the instruction PC after the trapping instruction is executed" | [ARM p. 808] |
| 10 | SMT: LDL_ARM, LDQ_ARM, QUIESCE | [IDS p. 668] |
| 11 | set in 0x1F07, meaning not stated; "Not available" in the ARM | [IDS p. 76]; [ARM p. 808]. **[inference]**: it marks one of the EV8's new sets. **[decision, owner, 2026-10-24]**: bit 11 covers both the 1C short-vector SIMD set and opcode 07 paired single, since the value has one unexplained bit and the EV8 implements both |
| 12 | prefetch with modify intent | [ARM p. 808] |

AMASK clears the bits of the extensions present: `Rc ← Rbv AND NOT mask`,
so a zero result means every requested feature is there [ARM p. 203; IDS
p. 76]. The test for Tarantula is `amask 8, $t0` then `bne $t0, no_vectors`.
**[conflict]**: the IDS's own QUIESCE example tests SMT with `LDA R2,
0x400(R31)`, `AMASK R2, R2`, `BEQ R2, CheckLock` "if no SMT, skip quiesce"
[IDS p. 674], which skips the QUIESCE exactly when SMT is present; the
branch must be BNE.

AMASK bit 3 reports that the processor has Tarantula, not that the
operating system lets a program use it: under an OS that never sets the
vector enable (below) every vector instruction raises OPCDEC. Whether the
OS supports vectors is the OS's to say (Linux: a hardware-capability bit).

**The vector enable, `VCTL<VEN>`** **[decision, D11]**, the owner's
(2026-10-24): a per-processor CSR in the processor's register window, not
an instruction and not an IPR index. Where in the window is the owner's
decision of 2026-10-25 **[decision]**: at offset 0x3F0000 of each
processor's own window, the 4 MB block of its own PID. It replaces the
first layout (2026-10-24), one window per chip with a VCTL per TPU at
0x3F0000 + 0x40 × tpu, which S4 made wrong for EV7-compatible mode: there
each TPU listed as a processor has a PID and a window of its own
([ev8.md](ev8.md), S4).

| Register | Physical address | Fields | Access |
| --- | --- | --- | --- |
| VCTL | `(~pid & 0x1ff) << 35 \| 0x7FFC << 20 \| 0x3F0000`, pid the processor's own PID (what WHAMI returns): on a chip of several TPUs, each TPU's own | 64 bits. <0> VEN: 1, the Tarantula instructions execute; 0, they raise OPCDEC (section 5). <63:1> RAZ/IGN | ordinary quadword loads and stores to that address from kernel mode. Reset 0 |

- **The window** is the EV7's: 4 MB per PID, `(~n & 0x1ff) << 35 | 0x7FFC
  << 20 | off`; PID 0 is at 0xFFF_FFC0_0000 ([marvel.md](marvel.md),
  "Marvel's address map"). VCTL of PID 0 is therefore at 0xFFF_FFFF_0000
  and VCTL of PID 2 at 0xFFF_FFFF_0000 - 2 × 0x8_0000_0000 =
  0xFEF_FFFF_0000; on the ES47 with `tpus = 4` PID 2 is TPU 1 of the chip
  with PID 0 ([ev8.md](ev8.md), S4), so each TPU's VCTL is in a window of
  its own. A processor finds its own from its WHAMI alone, as it finds its
  RBOX registers. The OS reaches it the way it reaches the Rbox and Zbox
  controls that EV7 systems already expose as CSRs; the EV7 PALcodes never
  touch it.
- **On a one-TPU chip** (the `ev8` row's default) the processor's own
  window is its chip's window and its TPU is TPU 0, so this layout and the
  chip-window layout of 2026-10-24 give the same address, 0x3F0000 in the
  chip's window.
- **The offset 0x3F0000** **[decision]**: the console's CSR table
  (`lab/docs-ev7/srm73-csr-tables.txt`, 141 rows) uses offsets below
  0x30000 only (the highest is 0x26120) and the console takes one more
  word, GIO_LOCK, at 0x80000; Linux names only the Rbox block
  ([marvel.md](marvel.md)). The last 64 KB of the window is therefore
  unclaimed.
- **The 0x40 × tpu spacing** stays for the native interface (S11) only,
  where a chip has one window for its four TPUs: VCTL[tpu] at 0x3F0000 +
  0x40 × tpu there, the 64 bytes between the IO7's port registers, unless
  S11 makes VCTL an IPR (below).
- **Lazy switching** (D11): an OS that knows vectors leaves VEN at 0 until
  a thread first uses one; the OPCDEC tells it (section 5), it stores 1 to
  the VCTL in its own processor's window (the PID WHAMI returns, offset
  0x3F0000) and returns. An OS that never stores (OpenVMS) keeps VEN at 0
  for every process, so no vector state ever passes between its processes.
- **Not an I_CTL bit**, because I_CTL has none free: the 21264 assigns
  every bit, <63:48> the sign extension of VPTB, <47:30> VPTB, <29:24>
  CHIP_ID and each of <23:0> a named field [HRM pp. 158-160], and the EV7
  PALcodes use <47:30>, <29:24>, <23>, <19:18>, <16:15>, <14>, <9:8>, <7>
  and <0> of it ([marvel.md](marvel.md), "The EV7 PALcodes' internal registers").
- In the native interface (S11) VCTL becomes an IPR in the IDS's Ibox map,
  with an index of its own; open.

## 3. EV8 instructions fixed by the IDS

### 3.1 Opcode 18

| Mnemonic | Syntax | Code | Format | Operation | IDS | Status |
| --- | --- | --- | --- | --- | --- | --- |
| LDL_ARM | `ldl_arm $ra, ($rb)` | 18.B000 | Mfc | Ra ← SEXT(MEM_L[Rbv]); watch_physical_address ← PA; watch_flag ← 1 | pp. 669, 675, 661 | final |
| LDQ_ARM | `ldq_arm $ra, ($rb)` | 18.B400 | Mfc | Ra ← MEM_Q[Rbv]; arms as LDL_ARM | pp. 669, 675, 661 | final |
| QUIESCE | `quiesce` (Ra = Rb = $31) | 18.B800 | Mfc | if watch_flag is set: start the quiesce timer and suspend until watch_flag clears | pp. 672-673, 675, 661 | final |
| CCB | `ccb ($rb)` (Ra = $31) | 18.EC00 | Mfc | CleanCacheBlk: give up ownership of the block, keeping a shared copy; a hint with no architectural effect on data | pp. 661, 411 | final; the syntax **[inference]** from ECB, whose predecode (IIX) it shares [IDS p. 661; ARM p. 206] |
| WH64EN | `wh64en ($rb)` (Ra = $31) | 18.FC00 | Mfc | write hint, 64 bytes, evict next | p. 661; [ARM pp. 218, 785] | final |

- **LDx_ARM has no displacement**: the 16-bit field is the function
  [IDS p. 669], so an assembler must refuse `ldq_arm $1, 8($2)`. It faults
  as LDQ does (access violation, alignment, fault on read, translation not
  valid), with the fault reported for va [IDS p. 669]; a big-endian
  LDL_ARM inverts va<2> [IDS p. 669].
- **The watched range** is the implementation's LDx_L lock range, at
  least 16 bytes and at most a page [IDS p. 670]. A store, a successful
  STx_C or a data-changing WH64 by another processor in the range clears
  watch_flag; so do the quiesce timeout, CALL_PAL REI, rti, rfe, retsys
  and urti [IDS p. 670]; an interrupt to a quiesced TPU clears it, and an
  LDx_ARM fails to arm while an interrupt is pending [IDS pp. 677-678]. A
  vector store (section 4) is a store for this purpose and for LDx_L's
  lock **[decision]**.
- **Detection**: test AMASK bit 10 first; "Software will not emulate any
  LDx_ARM instruction" [IDS p. 671]; on a 21264 unused 18.xxxx codes are
  NOPs [IDS p. 676].
- Words: `ldq_arm $1, ($2)` = 6022B400, `ldl_arm $1, ($2)` = 6022B000,
  `quiesce` = 63FFB800, `ccb ($3)` = 63E3EC00, `wh64en ($3)` = 63E3FC00.

**Note: the quiesce registers** (native-mode IPRs; no EV7 PALcode knows
them):

- QUIESCE_TIMEOUT[tpu], index 0x38: TIMEOUT <19:4>, CPU cycles, reset
  0x280 in the field; WATCH_EN <1>, wake on a write to the watched
  address; TIMEOUT_EN <0>, wake on the timeout; both enables reset 0;
  written by HW_MTPR, read by HW_MFPR [IDS pp. 510-511]. Appendix B calls
  it QUIESCE_TIMEOUT_VALUE<19:0>, 10K cycles at power-up, at most 2^20
  [IDS p. 678]: the field value 0x280 is the register value 0x2800, 10240
  cycles **[inference]**.
- WATCH_PHYS_ADDR[tpu]: PA<47:4> in <47:4> and WATCH_FLAG <0>; written
  only by LDx_ARM, not readable, no index [IDS p. 513].
- **[conflict]**: with both enables reset to 0 [IDS p. 511], a QUIESCE
  after reset would sleep until an interrupt, while Appendix B has the
  timer working from power-up [IDS p. 678]. In EV7-compatible mode neither
  register has an index (the EV7 PALcodes never write them), and Alphabox
  behaves as WATCH_EN = TIMEOUT_EN = 1 with 10240 cycles at the row's
  clock **[decision, D16]**.

### 3.2 CMOV2, FCMOV2, MAP_ALIGN

| Mnemonic | Syntax | Code | Format | Operation | IDS | Status |
| --- | --- | --- | --- | --- | --- | --- |
| CMOV2 | `cmov2 $ra, $rb, $rc` | 11.68 | Opr (Rb or literal) | executed from memory its predicate is always clear: Rc ← Rav | pp. 73-74, 655 | final; operation **[inference]** |
| FCMOV2 | `fcmov2 $fa, $fb, $fc` | 17.068 | F-P | Fc ← Fav | pp. 73-74, 660 | final; operation **[inference]** |
| MAP_ALIGN | `map_align`, which is `xor $31, $31, $31` | 11.40 | Opr | a NOP; in the last slot of an aligned eight-instruction block it starts a new map chunk | p. 75 | final |

The 21464 splits each CMOVxx into CMOVxx1 and CMOV2 at Icache fill time,
putting the CMOV2 in place of a `bis $31, $31, $31` (11.20) that follows
the CMOV, or of a `cpys $f31, $f31, $f31` (17.020) after an FCMOV
[IDS pp. 72-74]. CMOV2 Rc, Rb, Rc copies Rbv when the predicate bit of
its first operand is set and that operand otherwise; the predicate is
never set outside the pair, so "the CMOV2 instruction is effectively a NOP
that just copies Rc to Rc" [IDS p. 74]. For a word with Ra ≠ Rc the same
rule gives Rc ← Rav. Software has no reason to write CMOV2: the rows are
for an emulator (an 11.68 or 17.068 word in memory must execute as
defined) and a disassembler. A compiler for a 21464 can follow each
CMOVxx with `nop` (each FCMOVxx with `fnop`) so four CMOVs map per cycle;
Alphabox gains nothing from it. Words: `cmov2 $1, $2, $1` = 44220D01,
`fcmov2 $f1, $f2, $f1` = 5C220D01, `map_align` = 47FF081F.

### 3.3 Opcode 1C: the short-vector SIMD set

All are operate format with Rb or a literal (predecode I?I, [IDS pp. 652,
662-664]), so `vaddub8 $1, 8, $3` is legal. The syntax is
`mnemonic $ra, $rb, $rc` wherever a row gives none. The existing codes keep their
places: 00 SEXTB, 01 SEXTW, 30 to 3F (CTPOP, PERR, CTLZ, CTTZ, the MVI
packs, MIN and MAX), 70 FTOIT, 78 FTOIS [IDS pp. 662-664]. No new code is
one of these, so nada's use of 1C (00, 01, 30 to 3F, 70, 78) is untouched.

**The sizes in "zz" blocks** **[inference, agreed]**. The IDS writes four blocks
as `VADDzzz` and the like with the size in the low two bits, and writes
MAX's existing block 3C to 3F the same way (`MAXzzz`) [IDS p. 663]. The
ARM gives MAX as 3C MAXUB8, 3D MAXUW4, 3E MAXSB8, 3F MAXSW4 [ARM p. 783],
so zz = 00 UB8, 01 UW4, 10 SB8, 11 SW4: bit 0 picks the wider element,
bit 1 the signed form. Every explicitly coded EV8 pair follows the same
rule: PKUWB8 28, PKULW4 29, PKSWB8 2A, PKSLW4 2B; UPKUBW4 2C to UPKSWL2
2F; VADDUL2 14 and VADDSL2 16; VSRB8 60, VSRW4 61, VSRAB8 62, VSRAW4 63;
TMULUSB8 54 and TMULUSW4 55 [IDS pp. 662-664]. Only the 21164PC's MIN
block, 38 MINSB8 to 3B MINUW4 [ARM p. 783], orders them otherwise. A zz
row's block is the IDS's; which mnemonic has which code inside it is the
sheet's inference from MAX at 1C.3C to 1C.3F, **agreed** (Alphabox and
nada, 2026-10-24): the assembler and the emulator implement this order.
Only the results below stay proposed.

**The results** **[decision, D8]**. The IDS names each instruction
(Table 2-16, [IDS pp. 70-72]) and describes the multimedia unit's datapath
[IDS pp. 212-221], but it states no architectural result, and the ECO
that defined them is lost. The definitions below are proposed; where the
datapath settles a point the row cites it. SAT clamps to the element's
range (unsigned 0 to 2^n − 1, signed −2^(n−1) to 2^(n−1) − 1); U and S
name unsigned and signed elements.

| Mnemonic | Syntax | Code | Operation | Basis | Status |
| --- | --- | --- | --- | --- | --- |
| CMPWGE | `cmpwge $ra, $rb, $rc` | 1C.04 | for word i: Rc<2i+1:2i> ← 11 if Rav.w[i] ≥ Rbv.w[i] unsigned, else 00; Rc<63:8> ← 0 | "4 unsigned word subtracts ... forms 8 bits to be output in bits<7:0>" [IDS p. 219] **[inference]**; two bits per word, a byte mask for ZAP as CMPBGE's is **[decision]** | final |
| CMPLGE | `cmplge $ra, $rb, $rc` | 1C.05 | for longword i: Rc<4i+3:4i> ← 1111 or 0000; Rc<63:8> ← 0 | by analogy **[decision]** | final |
| PERMB8 | `permb8 $ra, $rb, $rc` | 1C.08 | byte i of Rc chosen by s = Rbv<4i+3:4i>: 0-7 Rav.b[s]; 8-11 Rbv.b[s−4] (Rb's high longword); 12 sign fill, every bit = bit 7 of Rc.b[i−1] (0 for byte 0); 13 reserved (Alphabox: 0); 14 0xFF; 15 0x00 | the shifter's 16-way byte mux takes the 8 bytes of A, the 4 high bytes of B, sign extend, saturate, ones and zeros [IDS p. 221]; PERMB8 "decodes the B register inputs" [IDS p. 220] **[inference]**; the selector numbering in the figure's order **[decision]** | final |
| GPKBLB4 | `gpkblb4 $ra, $rb, $rc` | 1C.09 | Rc.b[0..3] ← each of Rav.l[0], Rav.l[1], Rbv.l[0], Rbv.l[1] as a signed longword clamped to 0..255; Rc<63:32> ← 0 | only the name, "Graphics Pack Byte" [IDS p. 70] **[decision]** | final |
| VADDUB8, VADDUW4, VADDSB8, VADDSW4 | `vaddub8 $ra, $rb, $rc` | 1C.10, .11, .12, .13 | Rc.e[i] ← SAT(Rav.e[i] + Rbv.e[i]) | "Saturation is required for the PADD and PSUB operations" [IDS p. 219], the Ebox chapter's names for the parallel add and subtract **[inference]** | agreed (size) |
| VADDUL2, VADDSL2 | as above | 1C.14, 1C.16 | the same on longwords | as above | final |
| VSUBUB8, VSUBUW4, VSUBSB8, VSUBSW4 | | 1C.18, .19, .1A, .1B | Rc.e[i] ← SAT(Rav.e[i] − Rbv.e[i]) | as above | agreed (size) |
| VSUBUL2, VSUBSL2 | | 1C.1C, 1C.1E | the same on longwords | as above | final |
| VMINMAXUB8, VMINMAXUW4, VMINMAXSB8, VMINMAXSW4 | | 1C.20, .21, .22, .23 | running minimum and maximum, below | [IDS pp. 215-216, 219] | agreed (size) |
| VMINMAXUL2, VMINMAXSL2 | | 1C.24, 1C.26 | the same on longwords | as above | final |
| PKUWB8 | `pkuwb8 $ra, $rb, $rc` | 1C.28 | Rc.b[i] ← SAT(Rav.w[i]), Rc.b[4+i] ← SAT(Rbv.w[i]), i = 0..3, unsigned | "Pack must detect when word->byte or longword->word overflow ... saturate" [IDS p. 220] **[inference]**; Ra fills the low half **[decision]** | final |
| PKULW4 | | 1C.29 | Rc.w[i] ← SAT(Rav.l[i]), Rc.w[2+i] ← SAT(Rbv.l[i]), unsigned | as above | final |
| PKSWB8 | | 1C.2A | as PKUWB8, signed | as above | final |
| PKSLW4 | | 1C.2B | as PKULW4, signed | as above | final |
| UPKUBW4 | `upkubw4 $rb, $rc` (Ra = $31) | 1C.2C | Rc.w[i] ← ZEXT(Rbv.b[i]), i = 0..3; the same result as MVI's UNPKBW [ARM p. 229] | "Unpack must sign extend the result for signed data types" [IDS p. 220]; the operand as MVI's **[decision]** | final |
| UPKUWL2 | `upkuwl2 $rb, $rc` | 1C.2D | Rc.l[i] ← ZEXT(Rbv.w[i]), i = 0..1 | as above | final |
| UPKSBW4 | `upksbw4 $rb, $rc` | 1C.2E | as UPKUBW4, sign-extended | as above | final |
| UPKSWL2 | `upkswl2 $rb, $rc` | 1C.2F | as UPKUWL2, sign-extended | as above | final |
| TADDUB8, TADDUW4, TADDSB8, TADDSW4 | | 1C.40, .41, .42, .43 | Rc ← Σ (Rav.e[i] + Rbv.e[i]) | tree ops, below | agreed (size) |
| TSUBUB8, TSUBUW4, TSUBSB8, TSUBSW4 | | 1C.44, .45, .46, .47 | Rc ← Σ (Rav.e[i] − Rbv.e[i]) | | agreed (size) |
| TABSERRUB8, TABSERRUW4, TABSERRSB8, TABSERRSW4 | | 1C.48, .49, .4A, .4B | Rc ← Σ \|Rav.e[i] − Rbv.e[i]\|; TABSERRUB8 equals PERR [ARM p. 227] | | agreed (size) |
| TSQERRUB8, TSQERRUW4, TSQERRSB8, TSQERRSW4 | | 1C.4C, .4D, .4E, .4F | Rc ← Σ (Rav.e[i] − Rbv.e[i])² | | agreed (size) |
| TMULUB8, TMULUW4, TMULSB8, TMULSW4 | | 1C.50, .51, .52, .53 | Rc ← Σ Rav.e[i] × Rbv.e[i] | | agreed (size) |
| TMULUSB8, TMULUSW4 | | 1C.54, 1C.55 | the same, Ra's elements unsigned and Rb's signed | "Unsigned Times Signed" [IDS p. 70]; which operand is unsigned follows the name's order **[decision]** | final |
| VMULLUW4 | `vmulluw4 $ra, $rb, $rc` | 1C.59 | Rc.w[i] ← (Rav.w[i] × Rbv.w[i])<15:0>, unsigned | "the high or low 16 bits of product from each multiplier are selected" [IDS p. 218] **[inference]** | final |
| VMULHUW4 | | 1C.5B | Rc.w[i] ← (Rav.w[i] × Rbv.w[i])<31:16>, unsigned | as above | final |
| VSRB8, VSRW4 | | 1C.60, 1C.61 | Rc.e[i] ← Rav.e[i] shifted right, logical | shift rule below | final |
| VSRAB8, VSRAW4 | | 1C.62, 1C.63 | the same, arithmetic | | final |
| VSRL2, VSRAL2 | | 1C.64, 1C.66 | longwords, logical and arithmetic | | final |
| VSLB8, VSLW4, VSLL2 | | 1C.68, 1C.69, 1C.6C | the same, left | | final |

- **Tree ops** **[inference, IDS pp. 213-218]**: each element pair goes
  through the ALU as a + b (TADD), a − b (TSUB), the positive one of a − b
  and b − a (TABSERR) [IDS p. 215], or through the multipliers as a × b
  (TMUL) or (a − b)² (TSQERR) [IDS pp. 215-216], and the tree adder
  combines the eight byte results 8 to 4 to 2 to 1, or the four word
  results 4 to 2 to 1, into one sum [IDS p. 217]. The sum is exact and is
  written as a 64-bit two's-complement integer **[decision]**.
- **VMINMAX** **[inference, IDS pp. 215-216, 219]**: it finds the
  minimum and the maximum element of Ra (bytes in two groups of four,
  Table 6-14), compares them with "the previously found minimums and
  maximums in the B register", and writes the results "sign extended to
  longwords". **[decision]**: Rb<31:0> holds the running minimum and
  Rb<63:32> the running maximum, each as an element-size value in the low
  bits of its longword; Rc<31:0> ← the minimum of Rb's minimum and every
  element of Ra, Rc<63:32> ← the maximum likewise; results sign-extended
  to 32 bits for the S forms, zero-extended for the U forms. A loop starts
  with the minimum at the type's largest value and the maximum at its
  smallest.
- **Shifts** **[decision]**: one count for all elements, from Rbv or the
  literal, taken modulo the element width (Rbv<2:0> for bytes, <3:0> for
  words, <4:0> for longwords).
- Words: `pkuwb8 $1, $2, $3` = 70220503, `upksbw4 $2, $3` = 73E205C3,
  `vaddub8 $1, $2, $3` = 70220203 (zz as agreed).

### 3.4 Opcode 07: paired single precision

The FP operate format with a 6-bit function: "we will use the source
datatype (2 bits) field to expand the function field to 6 bits. All other
bits of the FP operate instruction remain the same" [IDS p. 280]:

```
 31    26 25   21 20   16 15 13 12 11 10      5 4    0
+--------+-------+-------+-----+-----+---------+------+
| 000111 |  Fa   |  Fb   | Trp | Rnd |   Fnc   |  Fc  |
+--------+-------+-------+-----+-----+---------+------+
```

- **Trp and Rnd keep their scalar meaning** [IDS p. 280; ARM pp.
  155-156]: Rnd 00 /C, 01 /M, 10 normal, 11 /D; Trp 000 none, 001 /U (/V
  for an integer result), 101 /SU (/SV), 111 /SUI (/SVI). The IDS's
  two-digit code (07.10) is Fnc alone; the 11-bit function at <15:5> is
  Trp<<8 | Rnd<<6 | Fnc, and the no-qualifier form, as for every IEEE
  instruction, has Rnd = 10: **0x080 | Fnc** **[inference]**. The IDS
  gives Fnc only: Table 8-18 splits <10:5> as the function, bits <10:9>
  "used to be SRC", and Table 8-20 prints two-digit codes [IDS pp.
  280-283]. No full 11-bit opcode 07 code is stated anywhere in it: the
  Fbox chapter (pp. 279-283) and the decode appendix (pp. 652-664) were
  searched, and the appendix has no opcode 07 table, only the sentence that
  opcodes 01 to 07 are reserved (A.6.2, p. 653). The base is the scalar
  IEEE convention carried over to every qualifier class, P-BIT and P-EST
  included; the example words below depend on it. It is the base nada
  packs; its qualifier letters then add and subtract as for the scalar
  instructions (`/s` +0x400, `/u` and `/v` +0x100, `/i` +0x200, `/c`
  −0x80, `/m` −0x40, `/d` +0x40).
- **Register format** (Table 8-19): the high single in <63:32>, the low
  in <31:0>, each an IEEE single: low half sign <31>, exponent <30:23>,
  fraction <22:0> [IDS p. 280]. **[conflict]**: the table prints the high
  half as exponent <62:54>, fraction <53:32>, nine and 22 bits; the high
  half is sign <63>, exponent <62:55>, fraction <54:32>, as the low half's
  layout and PCVTSP's packing (note 5) require.
- **Rounding and exceptions** [IDS p. 280]: all four rounding modes;
  IEEE exceptions are detected per half and ORed into one report, so one
  instruction can raise two different ones; each half's result follows
  the scalar rules; all 64 bits of Fc are always written; FPCR status and
  trap control as for scalar FP. The estimates (PARCPLx, PARSQRTx) use
  chopped rounding only and ignore Rnd.
- **[conflict]**: the decode appendix still lists 01 to 07 as reserved,
  raising OPCDEC [IDS p. 653]; the Fbox chapter assigns 07 [IDS pp.
  279-280], and the Ebox chapter's reserved-opcode table (Table 6-17) has
  a row for opcode 07 reading "Codes not defined by SIMD FP extension"
  [IDS p. 229; the row alignment is read from the OCR text]. This sheet
  follows the Fbox chapter.

**The qualifier classes** **[decision]**. The IDS gives the rounding rule
for the estimates and the "no arithmetic exceptions" rule for the bit
manipulations (notes 1, 5, 6, 7), but no per-instruction list. Proposed,
each class matching a scalar instruction's set in the ARM's Table C-3
[ARM pp. 785-786]:

| Class | Instructions | Trap qualifiers | Rounding | Like (nada's class) |
| --- | --- | --- | --- | --- |
| P-ARITH | PADD, PSUB, PCADD, PSUBC, PHADD, PHSUB, PHSUBR, PMUL, PMULH, PMULL, PMULHN, PMULLN | none, /U, /SU, /SUI | /C, /M, none, /D | ADDT (AXQ_ARITH) |
| P-CMP | PCMPEQ, PCMPNEQ, PCMPLT, PCMPLE, PCMPUN, PFMAX, PFMIN | none, /SU | none only | CMPTEQ (AXQ_CMP) |
| P-EST (agreed) | PARCPL, PARCPLH, PARCPLL, PARSQRT, PARSQRTH, PARSQRTL | none, /U, /SU | none (the field is ignored, note 1) | new: the arithmetic set less /SUI, since an estimate never sets INE |
| P-CVT | PCVTFI | none, /V, /SV, /SVI | /C, /M, none, /D | CVTTQ (AXQ_CVTTQ) |
| P-BIT | PMOVHH, PMOVHL, PMOVLH, PMOVLL, PCPYS, PCPYSN, PCPYSE, PCVTSP, PEXTH, PEXTL | none | none | CPYS (AXQ_NONE), but with base 0x080 \| Fnc |

Because opcode 07 was reserved in Version 5, every other Trp and Rnd
combination is an unused function code and raises OPCDEC [ARM p. 803]
**[decision]**, except that the estimates accept all four Rnd values and
treat them alike, as the IDS says.

**The instructions** (Table 8-20, [IDS pp. 281-283]; H and L are the
high and low halves). Status is the Fnc code's; every Class entry is
proposed, as above, except P-EST (agreed with nada, 2026-10-24); the Base
column is an **[inference]** in every row, from the IDS's statement that
the other fields keep their scalar meaning (see Trp and Rnd above).

| Mnemonic | Syntax | Fnc | Base [inference] | Class | Operation | Status |
| --- | --- | --- | --- | --- | --- | --- |
| PMUL | `pmul $fa, $fb, $fc` | 00 | 0x080 | ARITH | fcH ← faH × fbH, fcL ← faL × fbL | final |
| PMULL | `pmull $fa, $fb, $fc` | 01 | 0x081 | ARITH | fcH ← faH × fbL, fcL ← faL × fbL | final |
| PMULH | `pmulh $fa, $fb, $fc` | 02 | 0x082 | ARITH | fcH ← faH × fbH, fcL ← faL × fbH | final |
| PMULLN | `pmulln $fa, $fb, $fc` | 05 | 0x085 | ARITH | fcH ← faH × fbL, fcL ← −(faL × fbL), negated after rounding (note 8) | final |
| PMULHN | `pmulhn $fa, $fb, $fc` | 06 | 0x086 | ARITH | fcH ← −(faH × fbH), fcL ← faL × fbH, negated after rounding (note 8) | final |
| PARCPL | `parcpl $fb, $fc` (Fa = $f31) | 08 | 0x088 | EST | fcH ← 1/fbH, fcL ← 1/fbL | **agreed** (Alphabox and nada, 2026-10-24, no alias): the table gives 07.0A, also PARCPLL's code **[conflict]**; 08 completes the pattern of PARSQRT 0C, PARSQRTH 0D, PARSQRTL 0E |
| PARCPLH | `parcplh $fb, $fc` | 09 | 0x089 | EST | fcH ← 1/fbH, fcL ← 0 | final; the name **[decision]**: the table prints "PARCPH", note 1 and section 8.11.3 say PARCPLH and PARCPLx |
| PARCPLL | `parcpll $fb, $fc` | 0A | 0x08A | EST | fcH ← 0, fcL ← 1/fbL | final |
| PARSQRT | `parsqrt $fb, $fc` | 0C | 0x08C | EST | fcH ← 1/√fbH, fcL ← 1/√fbL | final |
| PARSQRTH | `parsqrth $fb, $fc` | 0D | 0x08D | EST | fcH ← 1/√fbH, fcL ← 0 | final |
| PARSQRTL | `parsqrtl $fb, $fc` | 0E | 0x08E | EST | fcH ← 0, fcL ← 1/√fbL | final |
| PADD | `padd $fa, $fb, $fc` | 10 | 0x090 | ARITH | fcH ← faH + fbH, fcL ← faL + fbL | final |
| PCADD | `pcadd $fa, $fb, $fc` | 11 | 0x091 | ARITH | fcH ← faH + fbL, fcL ← faL + fbH | final |
| PHADD | `phadd $fa, $fb, $fc` | 12 | 0x092 | ARITH | fcH ← faH + faL, fcL ← fbH + fbL | final |
| PSUB | `psub $fa, $fb, $fc` | 14 | 0x094 | ARITH | fcH ← faH − fbH, fcL ← faL − fbL | final |
| PSUBC | `psubc $fa, $fb, $fc` | 15 | 0x095 | ARITH | fcH ← faH − fbL, fcL ← faL − fbH | final |
| PHSUB | `phsub $fa, $fb, $fc` | 16 | 0x096 | ARITH | fcH ← faH − faL, fcL ← fbH − fbL | final |
| PMOVLL | `pmovll $fa, $fb, $fc` | 18 | 0x098 | BIT | fcH ← faL, fcL ← fbL | final |
| PMOVLH | `pmovlh $fa, $fb, $fc` | 19 | 0x099 | BIT | fcH ← faL, fcL ← fbH | final |
| PMOVHL | `pmovhl $fa, $fb, $fc` | 1A | 0x09A | BIT | fcH ← faH, fcL ← fbL | final |
| PMOVHH | `pmovhh $fa, $fb, $fc` | 1B | 0x09B | BIT | fcH ← faH, fcL ← fbH | final |
| PHSUBR | `phsubr $fa, $fb, $fc` | 1F | 0x09F | ARITH | fcH ← faL − faH, fcL ← fbL − fbH | final |
| PCPYS | `pcpys $fa, $fb, $fc` | 20 | 0x0A0 | BIT | each half: fa's sign, fb's exponent and fraction | final |
| PCPYSN | `pcpysn $fa, $fb, $fc` | 21 | 0x0A1 | BIT | each half: fa's sign inverted, fb's exponent and fraction | final |
| PCPYSE | `pcpyse $fa, $fb, $fc` | 22 | 0x0A2 | BIT | each half: fa's sign and exponent, fb's fraction | final |
| PCMPEQ | `pcmpeq $fa, $fb, $fc` | 28 | 0x0A8 | CMP | per half: IEEE 1.0 (0x3F800000) if faX = fbX, else a true zero | final |
| PCMPNEQ | `pcmpneq $fa, $fb, $fc` | 29 | 0x0A9 | CMP | per half: 1.0 if not equal (true when unordered) | final |
| PCMPUN | `pcmpun $fa, $fb, $fc` | 2A | 0x0AA | CMP | per half: 1.0 if unordered | final |
| PCMPLT | `pcmplt $fa, $fb, $fc` | 2C | 0x0AC | CMP | per half: 1.0 if faX < fbX | final |
| PCMPLE | `pcmple $fa, $fb, $fc` | 2D | 0x0AD | CMP | per half: 1.0 if faX ≤ fbX | final |
| PFMAX | `pfmax $fa, $fb, $fc` | 2E | 0x0AE | CMP | per half: MAX(faX, fbX) | final |
| PFMIN | `pfmin $fa, $fb, $fc` | 2F | 0x0AF | CMP | per half: MIN(faX, fbX) | final |
| PCVTSP | `pcvtsp $fa, $fb, $fc` | 30 | 0x0B0 | BIT | fcH ← fa<63:62> ‖ fa<58:29>, fcL ← the same of fb: two register-format singles packed as STS packs them, no checking (note 5) | final |
| PCVTFI | `pcvtfi $fb, $fc` (Fa = $f31) | 39 | 0x0B9 | CVT | each half converted to a 32-bit integer in that half | final; the direction and the format **[inference]** from the name (Alpha's CVTxy converts x to y) and "paired SP/LW integer" [IDS p. 280] |
| PEXTL | `pextl $fb, $fc` (Fa = $f31) | 3A | 0x0BA | BIT | Fc ← fbL in register format, mapped as LDS maps it (MAP_S), no checking (note 6) | final |
| PEXTH | `pexth $fb, $fc` (Fa = $f31) | 3C | 0x0BC | BIT | Fc ← fbH in register format, likewise | final |

Notes the IDS gives with the table [IDS pp. 282-283]: the estimates are
accurate to at least 14 bits, and the H and L forms neither check nor
raise exceptions for the unused half (note 1); with Fb = $f31 a PMOV
clears the low half, with Fa = $f31 the high half (note 2); there is no
paired branch: `pcmplt $fa, $f31, $fc` then `fbeq $fc` branches when
neither half is negative (note 3); the compares write IEEE 1.0, not the
scalar compares' 2.0, so a compare then a PMUL clears the halves where the
condition is false (note 4).

The Fnc codes are the IDS's except PARCPL's. The definitions the IDS
leaves open, all **[decision]**:

- **Estimates**: the exact reciprocal or reciprocal square root, chopped
  to 14 fraction bits (fraction <8:0> of the single zero), so the
  emulator gives the guaranteed precision and no more. Exceptions as the
  scalar DIVS of 1.0 by the operand (SQRTS for the root) would raise them,
  except INE, which an estimate never sets. The root of +-0 is the
  infinity of the zero's sign and raises division by zero, as IEEE 754's
  rSqrt and PARCPL of a zero do (the owner, 2026-10-26).
- **PFMAX and PFMIN**: per half `(faX < fbX) ? fbX : faX` and the
  reverse, comparing as PCMPLT does; an unordered pair is an invalid
  operation, and the half gets the result the scalar rules give one.
- **PCVTFI**: each half rounds by Rnd and is written as a 32-bit two's
  complement integer; out of range, the half takes the low 32 bits of the
  integer and the integer overflow is reported, as CVTTQ does at 64 bits.
- **Compares**: exceptions as CMPTxx raises them, NEQ and UN as EQ.
- Words, all depending on the 0x080 base **[inference]** (the `pmovhl` and
  `parsqrt` ones too, whose classes have no qualifier to vary):
  `padd $f1, $f2, $f3` = 1C221203, `padd/su $f1, $f2, $f3` =
  1C22B203, `pcmpeq $f1, $f2, $f3` = 1C221503, `pmovhl $f1, $f2, $f3` =
  1C221343, `parsqrt $f2, $f3` = 1FE21183.

## 4. Tarantula

Everything in this section is **proposed** unless a row cites the paper:
Alphabox's design, since no Tarantula encoding, register number or
detection method was ever published ([ev8.md](ev8.md), "3. Tarantula").
The owner and nada agreed it on 2026-10-25 (section 6).

### 4.1 State

| State | Size | Source |
| --- | --- | --- |
| $vr0 to $vr31 | 128 elements of 64 bits each; $vr31 reads as zero and discards writes | [TAR pp. 282-283] |
| $vl | vector length, 0 to 128; an 8-bit register | [TAR p. 282]; the range **[inference]** from Fig. 1's loop bound |
| $vs | stride, 64 bits, a signed byte distance | [TAR p. 282]; bytes **[inference]**: Fig. 1 adds `i * vs` to the byte address `Rb + off` [TAR p. 283]; signed **[decision]** |
| $vm | mask, 128 bits, one per element | [TAR p. 282] |
| `VCTL<VEN>` | the vector enable, a CSR per processor (section 2); not instruction-visible state | **[decision]** |

The register names are the ones agreed with nada: `$v0` is the integer
return register in GNU as and nada, so vector registers are `$vr`. `$vl`
and `$vs` never appear as operands (the mnemonic names them); `$vm`
appears only as the mask operand. All of this exists per TPU: the Vbox
"was also multithreaded" [TAR p. 284]. The vector registers are untyped:
an element is a quadword integer or a T float as the instruction reads
it. Values at power-up are UNPREDICTABLE except VEN = 0 (Alphabox clears
them all).

### 4.2 The opcode map

| Opcode | Group | Format | Instructions |
| --- | --- | --- | --- |
| 01 | VV, vector-vector operate | operate: Va, Vb, function, Vc | 42 |
| 02 | VS, vector-scalar operate | operate: Va, Rb, Fb or a literal, function, Vc | 38 |
| 03 | SM, strided memory | memory-function: Vt, Rb, function | VLOADQ, VSTOREQ |
| 04 | RM, random memory (gather and scatter) | memory-function: Vt, Rb, function, Vx in <4:0> | VGATHQ, VSCATQ |
| 05 | VC, vector control | operate | SETVL, SETVS, SETVM, GETVL, GETVS, GETVM |
| 06 | spare | | all codes OPCDEC |
| 18.5000 | DrainM | memory-function | DRAINM |

Opcodes 01 to 06 are free in the EV8 (07 went to paired single) [IDS pp.
653, 280] and "Reserved for Compaq" in the ARM [ARM pp. 788-789, 803];
nada uses none of them. One group per opcode, and the spare, is D10's
proposal.

### 4.3 Rules every group shares

- **Active elements.** An instruction acts on element i only when
  i < vl and, if its mask bit M is set, vm[i] = 1. That is the paper's
  Fig. 1 [TAR p. 283] with the "under-mask" specifier [TAR p. 282].
- **Inactive elements of a destination register are UNPREDICTABLE**:
  those at or above vl as Fig. 1 says [TAR p. 283], and those masked off
  below vl **[decision]**. The second follows the lanes' port budget: each
  lane's register slice has four read and two write ports for its two
  functional units [TAR p. 284], which leaves no port to read an old
  destination and merge it. Merging is done explicitly with VVMRG.
  Alphabox leaves inactive elements unchanged, so the interpreter, the JIT
  and JIT_VERIFY agree; software must not rely on it.
- **Inactive elements do nothing else**: no memory access, no fault, no
  arithmetic exception.
- **Element order** within one memory instruction is unspecified: Fig. 1
  visits a random permutation [TAR p. 283]. When two active elements of a
  store or scatter write one address, which value remains is
  UNPREDICTABLE **[inference]**; Alphabox writes in increasing i.
- **$vr31 as a destination**: an operate result is discarded; a load or
  gather into it is a prefetch, which takes no fault and has no effect
  [TAR p. 283], as a scalar load to R31 never signals one [ARM p. 57].
  The paper's "gather and scatter prefetches" made "by using v31 as the
  destination register" [TAR p. 283] are gathers into $vr31 ahead of a
  scatter **[inference]**: a store or scatter of $vr31 stores zeros, as
  STQ R31 does.
- **Integer arithmetic** wraps; no vector instruction raises integer
  overflow **[decision]**.
- **Floating point** (D13, **[decision]**): T format; each active
  element is computed as the scalar T instruction with /SUI and dynamic
  rounding (/SUID) computes it, with the same FPCR: rounding from
  `FPCR<DYN>`, and the FPCR's trap disable and denormal controls applying as
  they do to a /S instruction. There are no qualifier bits. The FPCR is
  updated as that scalar instruction would update it, once per active
  element. If any element would trap, the instruction traps (section 5).
- **Booleans**: a vector compare writes the quadword 1 for true and 0 for
  false, for the T compares too (scalar CMPTxx writes 2.0)
  **[decision]**: the result feeds SETVM and the logical instructions.
- **Register fields an instruction does not use** must be 31 (Va for the
  unary VV instructions, Rc where VC writes no integer register). Any
  other value is UNPREDICTABLE; Alphabox ignores the field.
- **SBZ fields and unassigned codes** raise OPCDEC (section 1), whatever
  VEN is.

### 4.4 VV and VS: operate

```
VV (01) and VS (02), register form:
 31    26 25   21 20   16 15 13 12 11 10      5 4    0
+--------+-------+-------+-----+--+--+---------+------+
| opcode |  Va   |  Vb   | 000 | 0| M|   fnc   |  Vc  |   VV: Vb a vector register
|        |       | Rb/Fb |     |  |  |         |      |   VS: Rb (fnc 00-1F) or Fb (fnc 20-3F)
+--------+-------+-------+-----+--+--+---------+------+

VS (02), literal form (fnc 00-1F only):
 31    26 25   21 20          13 12 11 10      5 4    0
+--------+-------+--------------+--+--+---------+------+
| 000010 |  Va   |   literal    | 1| M|   fnc   |  Vc  |
+--------+-------+--------------+--+--+---------+------+
```

The integer operate layout throughout: <11:5> is the 7-bit function, M
(under mask) its top bit, <12> the literal flag, <20:13> an 8-bit
zero-extended literal; in the register form <15:13> are SBZ. In VV, <12>
must be 0. The function code fixes the kind of VS's scalar: codes 00 to
1F read Rb or the literal, codes 20 to 3F read Fb, and a literal with
codes 20 to 3F raises OPCDEC. So the kind follows from the mnemonic, as
agreed with nada, never from a separate bit.

```
VV:  for i in 0..127:  if active(i): Vc[i] ← Va[i] op Vb[i]
VS:  for i in 0..127:  if active(i): Vc[i] ← Va[i] op S      (S = Rbv, the literal or Fbv)
     Vc[i] for i not active: UNPREDICTABLE (Alphabox: unchanged)
```

Mnemonics are `vv` or `vs` and the scalar Alpha mnemonic, as the paper's
VVADDQ and VSMULQ are [TAR p. 283]: its instructions are "straightforward
extensions of the existing scalar Alpha instructions" [TAR p. 282].
Syntax: `vvaddq $vra, $vrb, $vrc`, `vsaddq $vra, $rb, $vrc`,
`vsaddq $vra, 8, $vrc`, `vsaddt $vra, $fb, $vrc`, the unary ones
`vvsqrtt $vrb, $vrc`; under mask a last operand, `vvaddq $vra, $vrb,
$vrc, $vm`. B below is Vb[i] in VV and S in VS.

| fnc | VV | VS | Element operation | Notes |
| --- | --- | --- | --- | --- |
| 00 | vvaddq | vsaddq | Va[i] + B | the paper's VVADDQ |
| 01 | vvsubq | vssubq | Va[i] − B | |
| 02 | vvmulq | vsmulq | (Va[i] × B)<63:0> | the paper's VSMULQ, written there with Fb **[conflict]**; here the scalar of a quadword op is Rb |
| 03 | vvumulh | vsumulh | (Va[i] × B)<127:64>, unsigned | |
| 04 | vvand | vsand | Va[i] AND B | the paper's "vand" |
| 05 | vvbic | vsbic | Va[i] AND NOT B | |
| 06 | vvbis | vsbis | Va[i] OR B | `vsbis $vr31, $rb, $vrc` broadcasts Rb |
| 07 | vvornot | vsornot | Va[i] OR NOT B | |
| 08 | vvxor | vsxor | Va[i] XOR B | |
| 09 | vveqv | vseqv | Va[i] XOR NOT B | |
| 0A | vvsll | vssll | Va[i] << B<5:0> | |
| 0B | vvsrl | vssrl | Va[i] >> B<5:0>, logical | |
| 0C | vvsra | vssra | Va[i] >> B<5:0>, arithmetic | |
| 0D | vvmrg | vsmrg | vm[i] ? B : Va[i], for i < vl | reads vm as a selector; M must be 0 (OPCDEC otherwise) |
| 10 | vvcmpeq | vscmpeq | Va[i] = B | |
| 11 | vvcmpne | vscmpne | Va[i] ≠ B | the paper's "vcmpne v0, #0" is `vscmpne $vr0, 0, $vr6` |
| 12 | vvcmplt | vscmplt | Va[i] < B, signed | |
| 13 | vvcmple | vscmple | Va[i] ≤ B, signed | |
| 14 | vvcmpgt | vscmpgt | Va[i] > B, signed | the paper's "vcmpgt v1, #2" is `vscmpgt $vr1, 2, $vr7` |
| 15 | vvcmpge | vscmpge | Va[i] ≥ B, signed | |
| 16 | vvcmpult | vscmpult | Va[i] < B, unsigned | |
| 17 | vvcmpule | vscmpule | Va[i] ≤ B, unsigned | |
| 18 | vvcmpugt | vscmpugt | Va[i] > B, unsigned | |
| 19 | vvcmpuge | vscmpuge | Va[i] ≥ B, unsigned | |
| 20 | vvaddt | vsaddt | Va[i] + B | |
| 21 | vvsubt | vssubt | Va[i] − B | |
| 22 | vvmult | vsmult | Va[i] × B | |
| 23 | vvdivt | vsdivt | Va[i] / B | a divide exists **[inference]**: the paper's example trap is "divide-by-0" [TAR p. 283] |
| 24 | vvsqrtt | (OPCDEC) | √Vb[i]; Va = $vr31 | **[decision]** |
| 25 | vvcmpteq | vscmpteq | Va[i] = B | |
| 26 | vvcmptlt | vscmptlt | Va[i] < B | |
| 27 | vvcmptle | vscmptle | Va[i] ≤ B | |
| 28 | vvcmptun | vscmptun | unordered | |
| 29 | vvcmptne | vscmptne | ≠ (true when unordered) | |
| 2A | vvcmptgt | vscmptgt | Va[i] > B | |
| 2B | vvcmptge | vscmptge | Va[i] ≥ B | |
| 2C | vvcvtqt | (OPCDEC) | Vb[i] quadword to T, dynamic rounding; Va = $vr31 | |
| 2D | vvcvttq | (OPCDEC) | Vb[i] T to quadword, dynamic rounding | |
| 2E | vvcvttqc | (OPCDEC) | Vb[i] T to quadword, chopped (C's conversion) | a separate operation, not a qualifier |
| 30 | vvcpys | vscpys | Va[i]<63> ‖ B<62:0> | |
| 31 | vvcpysn | vscpysn | NOT Va[i]<63> ‖ B<62:0> | |
| 32 | vvcpyse | vscpyse | Va[i]<63:52> ‖ B<51:0> | |

Codes 0E, 0F, 1A to 1F, 2F and 33 to 3F are free. The T compares raise
exceptions as CMPTxx/SU does, NE as EQ, GT and GE as LT and LE; the
conversions as CVTQT and CVTTQ/SVI do; CPYSx none.

Words: `vvaddq $vr1, $vr2, $vr3` = 04220003, with `$vm` 04220803;
`vsmulq $vr1, $2, $vr3` = 08220043; `vsmult $vr1, $f2, $vr3` = 08220443;
`vscmpne $vr0, 0, $vr6` = 08001226; `vscmpgt $vr1, 2, $vr7` = 08205287;
`vvand $vr6, $vr7, $vr8` = 04C70088.

### 4.5 SM: strided memory

```
 31    26 25   21 20   16 15  12 11 10      5 4    0
+--------+-------+-------+------+--+---------+------+
| 000011 |  Vt   |  Rb   | 0000 | M|   fnc   | 00000|
+--------+-------+-------+------+--+---------+------+
```

The memory-function format with no displacement, as LDx_ARM has, and the
vector register in Ra's place, as the loaded or stored register of every
memory instruction. The function keeps the operate layout (M at <11>,
fnc at <10:5>) so one decoder serves 01 to 05.

| fnc | Mnemonic | Syntax | Operation |
| --- | --- | --- | --- |
| 00 | vloadq | `vloadq $vrt, ($rb)` | for active i: Vt[i] ← MEM_Q[Rbv + i × vs] |
| 01 | vstoreq | `vstoreq $vrt, ($rb)` | for active i: MEM_Q[Rbv + i × vs] ← Vt[i] |

Codes 02 to 3F are free (for example narrower elements, or a unit-stride
form that ignores vs). The paper's VLOADQ has a displacement, `VLOADQ Vc,
off(Rb)` [TAR p. 283] **[conflict]**: the agreed format has none, and a
program adds the offset to Rb first. There are no T forms: a T element's
memory image is its register image, as for LDT and LDQ. Each element
access is an aligned quadword access, single-copy atomic as LDQ's; an
element address that is not a multiple of 8 raises the alignment fault
(section 5). With `$vm`: `vloadq $vrt, ($rb), $vm`. Words: `vloadq $vr0,
($16)` = 0C100000, `vstoreq $vr0, ($16), $vm` = 0C100820.

### 4.6 RM: gather and scatter

```
 31    26 25   21 20   16 15  12 11 10      5 4    0
+--------+-------+-------+------+--+---------+------+
| 000100 |  Vt   |  Rb   | 0000 | M|   fnc   |  Vx  |
+--------+-------+-------+------+--+---------+------+
```

As SM, with the vector of byte offsets in the function field's low five
bits. The paper writes `VSCATQ Va, Rb, Vc`, Va the offsets and Vc the
data [TAR p. 283]; here the data register is Vt, first as in every memory
format, and the offsets Vx are written where a displacement goes, since
they play its part: `ea = Vx[i] + Rbv`.

| fnc | Mnemonic | Syntax | Operation |
| --- | --- | --- | --- |
| 00 | vgathq | `vgathq $vrt, $vrx($rb)` | for active i: Vt[i] ← MEM_Q[Vx[i] + Rbv] |
| 01 | vscatq | `vscatq $vrt, $vrx($rb)` | for active i: MEM_Q[Vx[i] + Rbv] ← Vt[i]; the paper's VSCATQ |

Codes 02 to 3F are free. Element alignment, atomicity and order as for
SM. Words: `vgathq $vr3, $vr1($2)` = 10620001, `vscatq $vr3, $vr1($2)` =
10620021.

### 4.7 VC: vector control

```
 31    26 25   21 20   16 15 13 12 11 10      5 4    0
+--------+-------+-------+-----+--+--+---------+------+
| 000101 |  Ra   |Rb / Vb| 000 | L| 0|   fnc   |Rc/Vc |
+--------+-------+-------+-----+--+--+---------+------+
```

The integer operate layout, as VV and VS: L (<12>) selects the literal
in <20:13> for SETVL and SETVS; M (<11>) must be 0. Ra is always
$31; the instruction says whether <20:16> and <4:0> name integer or
vector registers.

| fnc | Mnemonic | Syntax | Operation |
| --- | --- | --- | --- |
| 00 | setvl | `setvl $rb, $rc` or `setvl 64, $rc` | vl ← MIN(Rbv unsigned, 128); Rc ← vl (so a strip-mined loop gets this pass's length) |
| 01 | setvs | `setvs $rb` or `setvs 8` (Ra = Rc = $31) | vs ← Rbv |
| 02 | setvm | `setvm $vrb` (Ra = Rc = $31) | vm[i] ← Vb[i]<0> for i < vl; vm[i] ← 0 for i ≥ vl |
| 04 | getvl | `getvl $rc` | Rc ← vl |
| 05 | getvs | `getvs $rc` | Rc ← vs |
| 06 | getvm | `getvm $vrc` | Vc[i] ← vm[i] (0 or 1) for i < vl; the rest UNPREDICTABLE |

Only SETVM is named in the paper ("setvm v8" copies v8 into vm [TAR p.
282]); SETVM's low-bit test follows Alpha's BLBS **[decision]**.
The paper names no instruction that sets vl or vs and none that moves a
vector element to or from a scalar register. Its core interface carries
scalar data only toward the Vbox [TAR p. 284], and its mask design exists
to avoid "data transfers back and forth" with a 20-cycle round trip [TAR
p. 282], so no element move is defined: a scalar enters a vector through
VS (`vsbis $vr31, $rb, $vrc` broadcasts it), and an element leaves through
memory (a vector store is visible to later scalar loads, section 4.8).
Codes 03, 07 and 08 to 3F are free for reductions or element moves if a
later stage wants them. Words: `setvm $vr8` = 17E8005F, `setvl $16, $1` =
17F00001, `setvs 8` = 17E1103F, `getvl $1` = 17FF0081.

The paper's mask example [TAR p. 282] in this syntax:

```
        vloadq  $vr0, ($a0)          # A(i)
        vloadq  $vr1, ($a1)          # B(i)
        vscmpne $vr0, 0, $vr6        # A(i) .ne. 0
        vscmpgt $vr1, 2, $vr7        # B(i) .gt. 2
        vvand   $vr6, $vr7, $vr8
        setvm   $vr8
```

### 4.8 DrainM and memory ordering

| Mnemonic | Syntax | Code | Format | Status |
| --- | --- | --- | --- | --- |
| DRAINM | `drainm` (Ra = Rb = $31) | **18.5000** | Mfc | proposed |

```
 31    26 25   21 20   16 15                            0
+--------+-------+-------+--------------------------------+
| 011000 | 11111 | 11111 |      0101 0000 0000 0000       |   word 63FF5000
+--------+-------+-------+--------------------------------+
```

**The code**: outside nada's 18.xxxx set (0000 0400 4000 4400 C000) and
every code the ARM or the IDS assigns (4800 4C00 8000 A000 B000 B400 B800
E000 E800 EC00 F000 F800 FC00) [ARM pp. 780-785, 803; IDS p. 661]. It lies in
the barrier group, <15:14> = 01, where the 21464's decoder maps every code
with <11:10> = 00 to MB [IDS p. 661]: an EV8 without Tarantula executes
it as an MB, the strongest ordering it has, and a 21264 treats unused
18.xxxx codes as NOPs [IDS p. 676]. Both are harmless where there is no
vector unit. Software issues DRAINM only where AMASK bit 3 says Tarantula
is there. With VEN = 0 it executes all the same **[decision]**.

**Ordering** (D12):

- DRAINM orders as MB does, and in addition makes every earlier store of
  the processor visible to its later vector loads and gathers. The paper
  requires it between a scalar store and a later vector load, the one case
  its coherence hardware does not cover: on the EV8 the store waits in the
  store queue or write buffer, where the vector load, which goes to the
  L2, cannot see it; DrainM purges both and replay-traps the next
  instruction [TAR p. 287].
- A scalar store followed by a vector store, a vector store followed by
  a scalar or vector load: ordered by the hardware [TAR p. 287 and note
  4] **[inference]** for the vector-to-vector case.
- MB and WMB order vector memory accesses with respect to other
  processors as they order scalar ones **[decision]**: the paper's
  extension "fully integrates into a virtual-memory cache-coherent system
  without changes to its coherency protocol" [TAR p. 281] and says no
  more.
- Alphabox executes memory in program order, so DRAINM changes nothing
  in the emulator; a missing DRAINM goes unnoticed there. A trace that
  reports a vector load reading an address the same processor stored to
  since its last DRAINM would make the omission visible (a test idea, not
  part of the contract).

### 4.9 Spare encoding space

- opcode 06, whole;
- VV and VS: function codes 0E, 0F, 1A to 1F, 2F, 33 to 3F, and bits
  <15:13> of the register form (room for narrower element types);
- SM and RM: function codes 02 to 3F and bits <15:12>;
- VC: function codes 03, 07, 08 to 3F;
- opcode 18: every unassigned code (the barrier group's 5400 to 7C00 for
  further barriers).

### 4.10 Count

Counting an operation once across its VV and VS forms and its Q and T
types, the set has 44: 33 operate operations, 4 memory instructions, 6
control instructions and DrainM. The paper counts "45 new instructions
(not counting data-type variations)" [TAR p. 282]; with the vector enable
a CSR and not an instruction (section 2) the sheet is one short, and the
paper never published its list, so neither a match nor a miss proves
anything. As mnemonics there are 91: 42 VV, 38 VS, 2 SM, 2 RM, 6 VC and
DRAINM.

## 5. Exceptions and context

**Precision.** "Tarantula provides a precise exception model at the
instruction level granularity": system software gets "the PC of the
faulting instruction, but no extra information on which element"
[TAR p. 283]. In Alpha terms:

- **Memory faults** (TB miss, access violation, fault on read or write,
  alignment) are faults: EXC_ADDR is the vector instruction's own PC and
  nothing has changed, neither memory nor any register. An emulator
  translates and checks every active element before it writes anything
  **[decision]**. After the handler returns, the instruction restarts from
  its first element (and moves on: **Forward progress**, below).
- **Arithmetic traps** are traps, as on every Alpha: the EV8 reports them
  precisely, "the trap PC is the same as the instruction PC after the
  trapping instruction is executed" (AMASK bit 9, [ARM p. 808]), which
  identifies the vector instruction exactly. Its destination is written:
  correct elements where no exception trapped, UNPREDICTABLE ones where
  one did (Alphabox: the IEEE default result). EXC_SUM carries the OR of
  the trapping elements' exception bits with SWC set, as for a /S
  instruction, and REG = 31, since no scalar register was written
  **[decision, D13]**. No OS's completion code knows a vector
  instruction, so the OS delivers the signal.

**What a TB miss reports** (D15) **[decision]**. The emulator keeps no
lane TLBs [TAR p. 286]. A vector access that misses raises the ordinary
DTB miss of the mode (in EV7-compatible mode the 21264's DTBM_SINGLE
flow) for the lowest-numbered active element whose translation misses:
VA holds that element's address, MM_STAT its opcode (03 or 04) and WR for
a store or scatter, EXC_SUM<REG> the instruction's Vt field <25:21>, where
a scalar memory instruction's Ra is (on the native interface EXC_SUM
carries the instruction's register fields as for any instruction, Vt
among them), EXC_ADDR the instruction's PC. The PALcode fills the
entry and returns; the instruction restarts. Other faults are reported the
same way, for the lowest-numbered faulting element, and reach the OS
through its memory-management entry (on Linux, the cause is 0 for a load
and 1 for a store). An element address that is not a multiple of 8 raises
the alignment fault with that VA; no OS's alignment fixup knows opcodes
03 and 04, so the program gets the signal. The paper's
alternative, PALcode refilling every mapping the instruction may need by
reading vs [TAR p. 286], stays open to a PALcode of our own (S12).
EXC_SUM<REG> **[decision]** (the owner, 2026-10-28; S8 had inferred it
from the memory format) holds for every memory fault above, the alignment
fault included.

**Forward progress** is the emulator's to guarantee: a strided access
can touch 128 pages, more than a small TB holds, and the paper made each
lane TLB at least 8-way for that reason [TAR p. 286].

- **What an implementation must do.** An instruction whose every active
  element can be mapped completes: each fault its handler resolves moves
  it on, so that restarting from the first element never loses the
  progress already made. This must hold when the instruction's pages,
  with the TB entries the PALcode's own miss handling takes (an EV7
  PALcode reads the page table through a virtual address), outnumber the
  TB's entries, and whatever the TB evicts between two restarts. What the
  OS and the PALcode see is unchanged: the faults above, the TB they
  filled, and no miss repeated for a translation the PALcode has already
  written, unless a TB invalidation has dropped it since.
- **How Alphabox does it** **[decision]** (S8): the elements before a
  faulting one keep their translations for the instruction's restart,
  used only when the same word runs again at the same PC, in the same
  mode and address space; every TB invalidation drops them. They are
  neither lane TLBs nor architectural state. So an access to 128 pages
  completes whatever the TB evicts. The choice and its reasons are
  recorded in [ev8.md](ev8.md), "S8" ("Forward progress"), and
  [cpu-fidelity.md](../cpu-fidelity.md), the `ev8` row's table;
  `test/cpu/tarantula_mem.py` shows it.

**OPCDEC with VEN = 0.** Every Tarantula instruction, all of opcodes 01
to 05, raises OPCDEC with VEN = 0, with no exception, and EXC_ADDR is the
instruction's PC. An OS that knows vectors reads the opcode at that PC: 01
to 05 with VEN off means "vector unit disabled", so it gives the thread its
vector state, stores 1 to its processor's VCTL (section 2) with a quadword
store and returns to the instruction; with VEN on, an OPCDEC there is a
genuinely unused code and the program gets SIGILL. An OS that does not know
vectors never sets VEN and delivers the illegal-instruction signal on the
first vector instruction, so no vector state ever passes between its
processes. DrainM, which is not in opcodes 01 to 05, executes whatever VEN
is (section 4.8). Vector instructions in PALmode are UNPREDICTABLE: PALcode
must not use them.

**What must be saved** (D11). PALcode saves nothing: no HWPCB field holds
vector state, the EV7 PALcodes never touch VCTL, and SWPCTX leaves VEN as
the kernel set it. The OS saves, per thread that uses vectors, $vr0 to
$vr30 (all 128 elements, whatever vl is, since a later instruction may
use elements the current vl hides), vl, vs and vm: 31,744 bytes of
registers, the mask, and two quadwords. Interrupt and exception handlers
need not save any of it as long as the kernel uses vector instructions
only in its context-switch path. A sequence that saves to the area at
$a0 (32,784 bytes: vl and vs, the registers, the mask as 128 quadwords):

```
        getvl   $t0
        getvs   $t1
        stq     $t0, 0($a0)
        stq     $t1, 8($a0)
        setvl   128, $31             # the whole register, whatever vl was
        setvs   8
        lda     $t2, 16($a0)
        vstoreq $vr0, ($t2)
        lda     $t2, 1024($t2)
        vstoreq $vr1, ($t2)
        # ... the same for $vr2 to $vr30
        getvm   $vr0                 # $vr0 is saved: it carries the mask
        lda     $t2, 1024($t2)       # offset 31760
        vstoreq $vr0, ($t2)
```

Restoring runs it backwards: vl = 128 and vs = 8, `drainm` if the area was
written by scalar stores (a copy, a signal frame), load the mask image
into $vr0 and `setvm $vr0`, load $vr0 to $vr30, then `setvs` and `setvl`
from the saved quadwords. Alphabox's SaveState and RestoreState carry the
same state per TPU, VEN included.

**The calling standard.** All vector state, $vr0 to $vr30, vl, vs and vm,
is volatile across calls in both calling standards (Linux and NT), and
never carries an argument or a result. A function that uses vectors sets
vl, vs and vm itself before it relies on them. VEN belongs to the OS, not
to the calling standard.

## 6. The points agreed

Settled by the owner and nada on 2026-10-24 (points 1 to 5, 9), by the
owner alone on 2026-10-25 (7, 8 and 11, in which nada has no stake, each
as proposed), and by the owner and nada on 2026-10-25 as proposed (6,
10, 12, 13); 14 is a record, with nothing to decide. Nothing is open.
nada assembles point 12's additions under `-mtarantula` and marks them
as Alphabox's in its own specification. Each point is given with the
proposal it would replace:

1. **RM syntax** (agreed): `vgathq $vrt, $vrx($rb)` puts a vector register
   where a displacement goes. The alternative, three operands `vgathq
   $vrt, $vrx, $rb`, has the same encoding; nada keeps the first.
2. **Run-time detection in nada** (settled): nada assembles `amask` (11.61)
   and `implver` (11.6C, the literal 1 in Rb) on every `-mcpu`, so a program
   that must run on other Alphas tests bits 3, 10 and 11 first [ARM pp.
   780, 782].
3. **PARCPL's code** (agreed): 07.08, against the IDS's 07.0A, which the
   table also gives PARCPLL; PARCPLH for the table's "PARCPH". No alias.
4. **The P-EST qualifier class** (agreed): none, /U, /SU, no rounding
   letter.
5. **AMASK bit 11** for both new EV8 sets, and bit 3 for Tarantula: decided
   by the owner, 2026-10-24.
6. **The 1C results** (D8; agreed, 2026-10-25) are all proposed; GPKBLB4
   and PERMB8's selector values rest on the least evidence, VMINMAX's
   layout on a datapath description. The size order inside the zz blocks is
   agreed.
7. **Masked-off elements** (decided by the owner, 2026-10-25):
   UNPREDICTABLE, merged explicitly with VVMRG (section 4.3), against
   leaving them unchanged as Alphabox implements anyway. The second is
   easier to program and costs a third read port in hardware that will
   never exist.
8. **Vector booleans** (decided by the owner, 2026-10-25) are the integer
   1, not the scalar compares' 2.0 nor paired single's 1.0.
9. **VCTL's offset** (decided): 0x3F0000 in each processor's own EV7 window
   (section 2; the owner, 2026-10-25, in place of 0x3F0000 + 0x40 × tpu in
   the chip's, which stays for the native interface), chosen because no
   console table or Linux name uses it. It is listed so that a later look
   at a console's CSR table can reopen it.
10. **The quiesce registers** in EV7-compatible mode (agreed, 2026-10-25):
    not reachable, the timer fixed at 10240 cycles with both enables on
    (section 3.1). A test that wants another timeout would need a register
    of its own (a CSR beside VCTL's would do).
11. **Vector arithmetic traps** (decided by the owner, 2026-10-25) report
    the next PC (trap style, as AMASK bit 9 defines; section 5); the paper
    says only that the faulting instruction's PC is provided. A fault-style
    report (own PC, nothing written) would let an OS re-execute, but Linux
    under AMASK bit 9 decodes the instruction before the trap PC.
12. **Instructions beyond the paper's evidence** (agreed as Alphabox's
    additions, 2026-10-25): VVSQRTT, VVCVTTQC, VVUMULH and the unsigned
    compares are Alphabox's additions. The operations the paper names are
    add, multiply, the NE and GT compares, AND, SETVM, the strided load,
    the scatter and DrainM, and its divide-by-zero example implies a
    divide.
13. **No element moves** between vector and scalar registers (agreed,
    2026-10-25); reductions and extraction go through memory. VC function
    space is free if a stage shows the cost matters.
14. **The IDS's own errors** this sheet works around, for the record:
    the AMASK test branching the wrong way [IDS p. 674], the paired single
    high half's field boundaries [IDS p. 280], the decode appendix calling
    opcode 07 reserved [IDS p. 653], PARCPL's duplicate code
    [IDS p. 281], and the conflicting reset of QUIESCE_TIMEOUT's enables
    [IDS pp. 511, 678].
