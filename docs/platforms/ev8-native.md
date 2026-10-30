# EV8 native: the 21464's own interface, an Alphabox PALcode and console, Linux

The planning packet for the native track of the EV8 (Alpha 21464): the
processor on its own privileged interface (S11), the Marvel model as the
21464 would have used it, a PALcode and a minimal console written by the
project (S12), and Linux on them (S13). The research behind it, the
decisions D1 to D16 and the EV7-compatible track (S1 to S10) are in
[ev8.md](ev8.md); the instruction contract with the nada toolchain is
[ev8-isa.md](ev8-isa.md); the Marvel machines are [marvel.md](marvel.md).

**Status**: plan, 2026-10-25; N1 to N31 accepted as proposed by the owner
the same day. S11, the native interface on an `ev8n` row, done the same
day ([below](#s11-2026-10-25)); S11b, the Marvel model in 21464 mode,
done the same day ([below](#s11b-2026-10-25)); S12a, the PALcode, is
next. The owner's decisions that
frame it: option A first (an `ev8` row in EV7-compatible mode, S1 to S4
done), then the native interface as planned work, gaps
filled by inference and marked (2026-10-24); in EV7-compatible mode one
TPU per chip by default, `tpus = 2..4` an Alphabox construction, and the
native track the place where TPUs are done the IDS's way (2026-10-25).

**Markers**, as in [ev8.md](ev8.md): a bracketed source with a page is a
fact; **[inference]** is derived from sources but not stated in them;
**[decision]** is a choice of Alphabox's, numbered N1, N2, ... in
[section 7](#7-decisions-for-the-owner) (all accepted by the owner on
2026-10-25); **[conflict]** marks sources, or
parts of one, that disagree. Where a decision of [ev8.md](ev8.md) carries
over, it is cited as D1 to D16.

**Sources.** IDS is the *21464 Internal Design Specification*, Rev 1.1k
(`lab/datasheets/ev8-tarantula/`), cited by PDF page. Its text layer is
OCR: every register index, field boundary and table cell this packet
relies on was read on the rendered scan, not the OCR alone. ARM is the
*Alpha Architecture Reference Manual*, 4th edition, by PDF page; it has a
part on the Alpha Linux PALcode (chapters 20 and 21, pp. 535-562) beside
the Tru64 UNIX one. HRM is the 21264/EV68CB HRM (`lab/datasheets/alpha-cpu/`).
Linux files are `lab/docs-ev7/linux-*` (the Marvel code, from mainline)
and `lab/datasheets/ev8-native/linux/` (head.S, setup.h, srmcons.c,
Kconfig); qemu-palcode is `lab/datasheets/ev8-native/qemu-palcode/`
(GPL-2.0-or-later, read as a reference only, nothing copied). Rows 86 and
87 of `lab/datasheets/INDEX.md` describe the last two.

## The facts that matter most

1. **The IDS gives enough of the native interface to build it**: the
   indexes and fields of every Ibox and Mbox register, the HW_MFPR,
   HW_MTPR, HW_LD and HW_ST layouts, RET as the PALmode exit, the entry
   offsets, the shadow registers and the sixteen PALcode restrictions
   [IDS pp. 477-513, 569-582]. What it leaves out is the interrupt side:
   IER_CM, ISUM and SIRR have no index and no description, HW_INT_CLR
   "will be completely reworked", and the Rbox's interrupt registers are
   "TBD" [IDS pp. 479, 513, 527-528]. Those are the decisions this packet
   has most of.
2. **The EV8 has two PALcode shadow registers, S0 and S1, not none.**
   They replace R22/R23 when read as operand B and R24/R25 when read as
   operand A or written, in PALmode, with no enable bit [IDS p. 580].
   This settles the conflict [ev8.md](ev8.md) records ("the lack of
   shadow registers" [p. 500] means the 21264's eight are gone).
3. **The entry table exists twice and the two disagree** at 0x980, 0xA80
   and 0xB00: FAULT_RESET, IP_RESET and RESET in chapter 16 [IDS p. 499],
   FAST_RESET, TPU_RESTART ("another TPU has requested a restart") and
   MILD_RESET in chapter 22, which adds DST_NXM at 0xB80 and says Cold and
   Tepid reset enter at MILD_RESET [IDS pp. 631-634] **[conflict]**.
4. **The Rbox and Zbox "IPRs" have no index**: they are reached as I/O
   space, by RdIPR and WrIPR packets, and the IDS's own prose uses the
   21364's names for them (RBOX_INT, RBOX_IMASK, RBOX_IREQ, RBOX_INTQ,
   RBOX_INTA, RBOX_IO_CFG) [IDS pp. 75, 428, 464-465, 455]. So the EV7's
   4 MB register window, which Alphabox models, is where they live in
   21464 mode too **[inference]**, and the Marvel model keeps most of its
   code.
5. **The EV8's GIO registers and the CMM's register map agree with what
   M4 found by reverse engineering**: the IDS lists the Marvel CMM's GIO
   registers 00h CPUx_CTRL, 01h DMA data, 02h DMA address, 04h and 05h
   COM0 transmit and receive [IDS p. 474], the registers 0, 1, 2, 4 and 5
   of Alphabox's CMM model ([marvel.md](marvel.md), M4); the 21464 only
   narrows a GIO transaction to 32 bits, and Marvel's largest is 23
   [IDS p. 476].
6. **Linux needs a few lines changed, not a port**, if the 21464-mode
   address map keeps the EV7's shape with the inverted PID widened to the
   48-bit PA ([section 2.1](#21-the-address-map)): the kernel's own
   superpage arithmetic (`EV7_MASK40`, `EV7_KERN_ADDR`) then holds as it
   is, because the 21464's 43-bit superpage sign-extends VA<40> into
   PA<47:41> as the 21264's does into PA<43:41> [IDS pp. 688-689, 695].
   The changes are the PID mask, the CPU type, and WHAMI as TPU numbers.

## 1. The native privileged interface (S11)

### 1.1 Modes and the PC

The 21464 has three modes: normal mode, PALmode and SuperPALmode, and
the PC's low two bits carry them, as EXC_ADDR<1:0> and DTBMS_RET_ADDR<1:0>
report: 00 normal, 01 PALmode, 11 SuperPALmode [IDS pp. 484-485, 573].
On the 21264 only PC<0> marks PALmode.

| From | To normal | To PALmode | To SuperPALmode |
| --- | --- | --- | --- |
| Normal mode | RET | CALL_PAL, trap, interrupt | |
| PALmode | RET | RET | PNMI, RET |
| SuperPALmode | RET | RET | RET |

[IDS p. 574, Table 17-5]. A RET in normal mode cannot raise the mode
[IDS p. 573]. **[conflict]**: the text says "only a PNMI event can cause a
transition from Normal mode to SuperPALmode" [p. 573], the table puts PNMI
in the PALmode row [p. 574]. A PNMI is a non-maskable interrupt, "even by
PALcode": the two listed are TPU Restart and TPU PALmode Timeout ("a TPU
has been in PALmode too long") [IDS pp. 629, 632]. SuperPALmode is
therefore the mode a TPU stuck in PALmode is pulled out into
**[inference]**, entered at the INTERRUPT entry, which Table 22-2 gives
the PALmode timeout [IDS p. 632].

- In PALmode interrupts are blocked [IDS p. 625] (the 21264's rule,
  restated in the SROM section).
- **[decision, N1]**: the `ev8n` row implements the three mode encodings
  (RET to 11 enters SuperPALmode, EXC_ADDR and DTBMS_RET_ADDR report
  them) but raises no PNMI: neither TPU PALmode Timeout nor TPU Restart is
  modelled in S11. SuperPALmode behaves as PALmode. The table's row
  wins over the text: PNMI from PALmode only.
- **[decision, N2]**: in PALmode every jump-format instruction (JMP, JSR,
  RET, JSR_COROUTINE) takes the new mode from Rb<1:0>, and a BSR, JSR or
  JSR_COROUTINE in PALmode writes Ra with the next PC including its mode
  bits, as the 21264 keeps PC<0> in PALmode. The IDS names only RET
  [p. 573] and says PALmode JMPs always mispredict [p. 580]; a JMP to a
  PALmode target must carry 01 in Rb<1:0> to stay there. In normal mode
  Rb<1:0> are ignored, as the architecture requires.

### 1.2 The PALcode instructions

| | 21264 (what Alphabox implements) [HRM ch. 6.4] | 21464 [IDS] |
| --- | --- | --- |
| HW_MFPR | 0x19: Ra and Rb <25:16> (the same), index <15:8>, scoreboard <7:0> | 0x19: <25> 0, Rclass <24:21>, <20:13> 0, index <12:5>, **Rc <4:0>** the destination [p. 571]. **[conflict]**: the text above the figure says the destination is Ra [p. 571]; the figure, the field table [p. 571] and the decode appendix's IPR format [p. 650] say Rc <4:0>. The figure wins **[decision, N3]** |
| HW_MTPR | 0x1D: Ra = Rb <25:16>, index <15:8>, scoreboard <7:0> | 0x1D: <25> 0, Rclass <24:21>, Rb <20:16>, <15:13> 0, index <12:5>, Wclass <4:0> [p. 572] |
| Index | 8 bits, flat [HRM Table 5-1] | 8 bits; <12> (index MSB) 0 the Mbox, 1 the Ibox [pp. 571-572] |
| Classes | scoreboard bits | Rclass: <3> valid, <2:0> class; Wclass: <4> bubble, <3> valid, <2:0> class; classes 0 and 1 defined [pp. 571-573]. Ordering only: an emulator that retires in order ignores them **[inference]** |
| HW_LD / HW_ST | 0x1B / 0x1F: type <15:13>, LEN <12> (long or quad), disp <11:0> | 0x1B / 0x1F: type <15:13>, length <12:11> (00 byte, 01 word, 10 longword, 11 quadword), disp <10:0> signed [pp. 569-570] |
| HW_LD/ST types | 000 physical, 001 physical locked (HW_ST conditional), 010 VPTE, 100 virtual, 101 virtual write check, 110 alternate mode, 111 both | 000 physical, 010 virtual/PTE (HW_LD only; a TB miss goes straight to the double-miss flow), 100 virtual, 101 virtual/WrChk (HW_LD only), 110 virtual/Alt, 111 virtual/WrChk/Alt [p. 570]. **No locked or conditional form** |
| Alignment | | never checked; the address is forced to the data size [p. 569] |
| PALmode exit | HW_RET (0x1E), stall bit | RET (0x1A, function 2), the mode from Rb<1:0>; Ra receives the next PC [pp. 573-574]. HW_RET does not exist |
| 0x1E | HW_RET | IFETCHB, PALcode format: fetching stops until the IFETCHB retires; restarts at PC + 4 [pp. 107-108, 650] |
| Native CMOV | | CMOV2 and the CMOV pairing apply in PALmode too, with Restrictions 12 and 13 (below) [pp. 579-580] |

- **IFETCHB** is the 21464's barrier: Restriction 1 requires one after the
  last IPR write of every flow before it returns, Restriction 2 between
  two writes to the same IPR group, Restrictions 14 and 16 after a
  PALmode JMP and a PALmode MT_FPCR [IDS pp. 575-582]. In an emulator that
  executes in order and commits each write at once it is a no-op
  **[inference]**; an IPR write takes effect at the next instruction.
- **The HW_LD/HW_ST types 001 and 011** have no meaning on the 21464.
  **[decision, N4]**: OPCDEC, as an unassigned code, so that a PALcode
  written for the 21264 fails visibly; an atomic physical update is done
  with LDQ_L/STQ_C through a kernel superpage, which PALmode may use
  (it runs with kernel access checks, [IDS p. 308]).
- **Outside PALmode** the 0x19, 0x1B, 0x1D, 0x1F and 0x1E opcodes raise
  OPCDEC **[inference]**: the 21264 allows them only with I_CTL<HWE>, and
  the 21464's I_CTL has no such field [IDS pp. 489-490]. The disruption
  table's "Illegal Instruction: thread not allowed to execute this
  instruction" [IDS p. 630] is that OPCDEC.

### 1.3 Shadow registers

"The 21464 does not have a 'mode' bit to enable/disable shadow register
replacement. Instead, the 21464 implements the following
position-dependent replacement policy" [IDS p. 580]:

| In PALmode, register | R22 | R23 | R24 | R25 |
| --- | --- | --- | --- | --- |
| as operand A (Ra) | R22 | R23 | **S0** | **S1** |
| as operand B (Rb) | **S0** | **S1** | R24 | R25 |
| as destination | R22 | R23 | **S0** | **S1** |

- Operand A and the destination map alike so that STx_C, whose Ra is both,
  works; operand B maps differently "to make it easy for PALcode to read
  from any register, shadow or architectural" [IDS p. 580]. So PALcode
  reads the user's R24 and R25 through Rb, the shadow registers through
  Ra; it writes S0 and S1 by naming R24 and R25.
- The architectural R24 and R25 can be written in PALmode only by a
  legacy CMOV (one not followed by its CMOV2 placeholder), which the
  21464 special-cases to disable replacement of Rc [IDS pp. 574-575, 580].
  A native CMOV must not name R24 or R25 as destination (Restriction 13),
  and no CMOV may name a shadow register (Restriction 12) [IDS pp.
  579-580]. **[inference]**: by the same table a native CMOV must not
  name R22 or R23 as destination either. The 21464 replaces it with
  `CMOVx1 Ra, Rc -> Rc; CMOV2 Rc, Rb -> Rc` [IDS p. 580], whose first
  part reads the old Rc as operand B, so with Rc = 22 or 23 it reads S0
  or S1 where the destination is the architectural register: the failure
  Restriction 13 describes for R24 and R25, which the IDS does not list
  for R22 and R23. A legacy CMOV is safe for all four, since its Rc is not
  replaced at all. **[conflict]** of wording only: p. 574 says "R24/F24 and
  R25/F25", p. 580 R24 and R25 alone; no FP shadow register appears
  anywhere else. **[decision, N5]**: integer registers only.
- A CALL_PAL writes "ShadowReg1" implicitly [IDS p. 149, Table 4-3]: the
  return PC, which the PALcode then returns through with `RET (S1)`; the
  native MT_FPCR flow is `HW_MFPR EXC_ADDR -> S1; IFETCHB; RET S1`
  [IDS p. 581]. The 21264 does the same with a PALshadow register: it
  loads the PC after the CALL_PAL into R23, its "linkage register", with
  bit 0 set when the call came from PALmode [HRM pp. 196-197].
  **[inference]**: on the 21464, CALL_PAL writes S1 with the return PC
  and the caller's mode in <1:0>, and does not write EXC_ADDR (Table 22-3
  lists the IPRs each entry writes, and CALL_PAL is not among them
  [IDS p. 633]).
- S0 and S1 are per TPU, as every register file is [IDS p. 46]
  **[inference]**.

So the EV8 has two shadow registers where the 21264 has eight (R4-R7,
R20-R23 with I_CTL<SDE>); PAL_TEMP1 and PAL_TEMP2 take over what a
shadow register held across flows, such as the scratch area's address
[IDS p. 500].

### 1.4 The IPR table

From Table 16-1 [IDS pp. 477-481], every index read as the binary field
on the scan; fields and resets from the register's section (pages in the
last column). "TPU" is per TPU, "chip" one per chip; R and W are HW_MFPR
and HW_MTPR; I is written implicitly by hardware, E by an event. The
group column (I1-I3, M1-M3) is Restriction 2's speculative group; Init is
the reset class: All (every reset), Dbg (not the debug reset), No.

**Ibox (index MSB 1)**

| Index | Name | Scope | Access | Grp, Init | Fields | Pages |
| --- | --- | --- | --- | --- | --- | --- |
| 0x80-0x83 | I_CTL | TPU | RW; index<1:0>: 01 VPTE_BASE, 10 SUPERPAGE, VA_SIZE, PAGE_SIZE, 11 all | I1, Dbg | VPTE_BASE <51:33>, REDUCED_PT <27>, SPE<2:0> <26:24>, VA_SIZE <21> (0 43-bit, 1 52-bit), PAGE_SIZE <20> (0 8 KB, 1 64 KB) | 489-490 |
| 0x88-0x8B | I_MODE | TPU | RW; 01 CURRENT, 10 CONSOLE, 11 all | I1, Dbg | CONSOLE <5>, CURRENT <4:3> | 490-491 |
| 0x90-0x97 | I_PCTX | TPU | RW; index<2:0>: 001 ASN, 010 TPU_GRP, 100 FP_ENABLE, 111 all | I1, Dbg | FP_ENABLE <19>, TPU_GRP <17:16>, ASN <15:8> | 491-492 |
| 0xA0 | EXC_SUM | TPU | R, I | I1, Dbg | Ra <28:24>, Rb <23:19>, Rc <18:14> (the instruction's <25:21>, <20:16>, <4:0>), INT <13>, SET_IOV...SET_INV <12:7>, IOV, INE, UNF, OVF, DZE, INV <6:1>, SWC <0> | 485-486 |
| 0xA1 | EXC_ADDR | TPU | R, I | I1, Dbg | ADDR<63:2> = SEXT(PC<51:2>), MODE <1:0> | 484-485 |
| 0xA3 | IVA_FORM | TPU | R | | three formats by VA_SIZE, PAGE_SIZE, REDUCED_PT | 497-498 |
| 0xA6, 0xA7 | DTBMS_RET_ADDR | TPU | R, I; 0xA6 no side effect, 0xA7 also blocks loads and stores (for the DTB writer block) | I1, Dbg | as EXC_ADDR | 483-484 |
| 0xA8 | PAL_BASE | TPU | RW, I | I1, "?" | <51:15> | 498-499 |
| 0xA9 | PAL_TEMP1 | TPU | RW | I1, Dbg | anything | 499-500 |
| 0xAA | PAL_TEMP2 | TPU | RW | I1, Dbg | anything | 499-500 |
| 0xAA | CPU_BASE | chip | RW | I1, All | no section | 478 |
| 0xB8-0xBB | CC | TPU | RW (RPCC reads; MFPR "TBD"); index<1:0>: 01 OFFSET and ENABLE, 10 COUNT, 11 all | I1, "*" | OFFSET <63:32>, COUNT <31:4> (every 16th cycle), ENABLE <0> | 482-483 |
| 0xC0 | ITB_IM | TPU | W | I2 | MODE <4:0>: 00 all groups, 01 all of this group, 03 ASM of this group, 05 non-ASM of this group, 0D non-ASM of this group and ASN, 10 all and reset the write pointer, 16 reset the write pointer only | 494 |
| 0xC2 | ITB_IS | TPU | W | I2, No | VA<51:13> | 495 |
| 0xC4 | ITB_PTE | TPU | W | I3, No | PFN <63:32> (PA<44:13>, or PA<47:16> with 64 KB pages), URE, SRE, ERE, KRE <11:8>, GH <6:5>, ASM <4> | 495-496 |
| 0xC6 | ITB_TAG | TPU | W | I2, No | VA<51:13> | 496-497 |
| 0xC8 | CPU_CNFG | chip | RW | I1, All | CHIP_ID <63:56> (read-only, no value given), µITB_DIS, BPR_DIS, SLOT1_DIS, ... THRASH_LIMIT <24:0>: performance controls | 487-488 |
| 0xC9 | IC_STAT | TPU | R, W1C | I1, Dbg | INDEX <15:5>, MULTIPLE <1>, PAR_ERROR <0> | 492-493 |
| 0xCC | IC_FLUSH | TPU | W | | flush this group's Icache blocks | 493 |
| 0xCD | IC_FLUSH_ASM | TPU | W | | the same for ASM = 0 (implemented as IC_FLUSH) | 493-494 |
| 0xD8 | TPU_CNFG | TPU | RW | I1, All | TPU_ID <57:56> (read-only, 0-3), MCHK_EN <7>, PREF_RANGE <6:4> | 488-489 |
| 0xE0-0xE3, 0xE8-0xED, 0xF0-0xF7 | performance: IAGG_EVENT0-3, PR_INST_CTL, PR_TRIG_PC, PRn_PC, PR_I_INFO, PR_Q_INFO, PRn_TIMELINE0-3 | chip | R, E (two M) | | chapter 19 | 477-478 |

**Mbox (index MSB 0)**

| Index | Name | Scope | Access | Grp, Init | Fields | Pages |
| --- | --- | --- | --- | --- | --- | --- |
| 0x00-0x03 | M_CTL | TPU | RW (write-only on the 21264); index<1:0>: 01 VPTE_BASE, 10 SUPERPAGE, BIG_ENDIAN, VA_SIZE, PAGE_SIZE, 11 all | M1, Dbg | VPTE_BASE<51:33>, REDUCED_PT <27>, SPE<2:0> <26:24>, BIG_ENDIAN <23>, DBL_ALT <22> (which double-miss entry a VPTE miss takes), VA_SIZE <21>, PAGE_SIZE <20> | 506-507 |
| 0x08-0x0F | M_MODE | TPU | RW; index<2:0>: 001 CURRENT, 010 CONSOLE, 100 ALT, 111 all | M1, Dbg | ALT <7:6>, CONSOLE <5>, CURRENT <4:3> | 507-508 |
| 0x10-0x13 | M_PCTX | TPU | RW; 01 ASN, 10 TPU_GRP, 11 all | M1, Dbg | TPU_GRP <17:16>, ASN <15:8> | 508-509 |
| 0x20 | M_STAT | TPU | R, I | M1, Dbg | DCDAT_PERR <11>, DCTAG_PERR <10>, OPCODE <9:4>, FOW <3>, FOR <2>, ACV <1>, WR <0> | 509-510 |
| 0x21 | VA | TPU | R, I | M1, Dbg | VA<63:0>; va' (after the big-endian adjustment), not va | 511-512 |
| 0x23 | VA_FORM | TPU | R | | as IVA_FORM, from VA and M_CTL | 512-513 |
| 0x38 | QUIESCE_TIMEOUT | TPU | RW | M1, Dbg | TIMEOUT <19:4> (reset 0x280), WATCH_EN <1>, TIMEOUT_EN <0> | 510-511 |
| 0x40 | DTB_IM | TPU | W | M1 | MODE <4:0> as ITB_IM | 502-503 |
| 0x42 | DTB_IS | TPU | W | M2, No | VA<51:13> | 503 |
| 0x44-0x45 | DTB_PTE0, DTB_PTE1 | TPU | W | M3, No | PFN <63:32>, UWE, SWE, EWE, KWE <15:12>, URE, SRE, ERE, KRE <11:8>, GH <6:5>, ASM <4>, FOW <2>, FOR <1>, VALID <0> | 504-505 |
| 0x46-0x47 | DTB_TAG0, DTB_TAG1 | TPU | W | M2, No | VA<51:13> | 505-506 |
| 0x48 | DC_CTL | chip | RW | M1, All | DCDAT_PAR_EN <55>, DCTAG_PAR_EN <54>, F_BAD_DECC <53>, F_BAD_TPAR <52>, FLUSH <51>, F_HIT <50>, SET_EN <49:48> | 500-501 |
| 0x49 | DC_STAT | TPU | R, W1C | M1, Dbg | SEO <63>, ECC_ERR_ST <62>, ECC_ERR_LD <61>, TPERR_P2-P0 <60:58> | 501-502 |
| 0x60-0x63, 0x68, 0x6A-0x6C, 0x70-0x71 | performance: MAGG_EVENT0-3, PR_MEM_EVENT_CTL, PRn_MEM_INFO, PR_ST_LATENCY, PRn_DMISS_INFO | chip | R, E (one M) | | chapter 19 | 477-478 |
| none | WATCH_PHYS_ADDR | TPU | I (LDx_ARM), not readable | Dbg | PHYS_ADDR<47:4>, WATCH_FLAG <0> | 513 |

**The Cbox interrupt registers**, per TPU, listed with no index and only
HW_INT_CLR described [IDS pp. 479, 513-514]:

| Name | Access [p. 479] | What the IDS gives |
| --- | --- | --- |
| HW_INT_CLR | W | CR <31>, PC <30:29>, MCHK_ID <28:27>; "I believe this register is moving to the Cbox and will be completely reworked" [p. 513] |
| IER_CM | RW | nothing |
| ISUM | R | nothing |
| SIRR | RW | nothing |

**What the table does not settle, and the proposals:**

- **CPU_BASE and PAL_TEMP2 share index 0xAA** on the scan (both `1 0101
  010`) **[conflict]**, and CPU_BASE has no section. **[decision, N6]**:
  CPU_BASE moves to 0xAB, the free index of the same group, and holds the
  chip's node ID (its PID) in <9:0>, read-only to software, set at reset:
  the 21464 has no other register a chip learns its own PID from (the
  21364's CBOX_WHAMI is not in the table, and R_WHOAMI holds router
  coordinates, [section 2.2](#22-node-ids-pids-and-coordinates)). The
  name ("CPU Base", per chip, initialised on every reset) fits a value
  the chip is given rather than computes **[inference]**.
- **IER_CM, ISUM, SIRR, HW_INT_CLR**. **[decision, N7]**: indexes in the
  free Ibox block 0xB0-0xB7 (`1 0110 xxx`, beside CC): 0xB0 IER_CM, 0xB1
  SIRR, 0xB2 ISUM, 0xB3 HW_INT_CLR; layouts the 21264's [HRM Table 5-1
  and 5.2], which the IDS's interrupt levels follow ("the 21264 core
  allows for 6 interrupt wires into the core", [IDS p. 465]): IER_CM
  CM <4:3>, SIEN <28:14>, PCEN <30:29>, CREN <31>, SLEN <32>, EIEN
  <38:33> [HRM 5.2], without the 21264's ASTEN <13> (below); ISUM the
  same positions as pending bits;
  SIRR SIR <28:14>; HW_INT_CLR the IDS's CR, PC and MCHK_ID positions
  plus SL <32> as on the 21264. IER_CM<CM> is the same field as
  I_MODE<CURRENT> and M_MODE<CURRENT>: a write of any one changes all
  three **[decision]**, because the Mbox chapter's access check reads
  "IER_CM/CM" [IDS p. 308] and PALcode would otherwise write three
  registers on every mode change.
- **No AST registers.** I_PCTX has no ASTER or ASTRR, and no IPR holds
  them [IDS pp. 491-492]; the 21264 keeps both in PCTX and raises the AST
  interrupt from them. **[inference]**: on the 21464 a PALcode that needs
  ASTs (OpenVMS's) tests them itself on its return paths. Linux uses no
  ASTs, so the UNIX PALcode is unaffected; the bit IER_CM<ASTEN> of the
  21264 is not carried over.
- **The vector enable.** In EV7-compatible mode VCTL is a CSR at offset
  0x3F0000 of each processor's own window ([ev8-isa.md](ev8-isa.md),
  section 2; the owner, 2026-10-25). **[decision, N8]**: in the native interface VCTL becomes the
  Ibox IPR 0xAC (`1 0101 100`, the PAL_BASE group), per TPU, VEN <0>,
  reset 0, read and written by HW_MFPR and HW_MTPR; the CSR offset is not
  decoded on an `ev8n` row. The OS sets it through a PALcode call
  (section 3.1, `wrval`-style), since it cannot execute HW_MTPR.
- **DTB_PTE0/1 and DTB_TAG0/1** are one entry written twice, once per
  load/store bus (Restriction 3: Mbox IPRs of groups M2 and M3 have a
  copy per bus) [IDS pp. 504-506, 575-576]. **[decision, N9]**: the
  emulator keeps one copy: a TAG write at either index sets the pending
  tag, a PTE write at either index writes the entry, and the second
  write of a pair replaces the first's entry instead of adding one, so
  the DTB writer block (two TAGs, two PTEs) makes one entry, as it does
  on the chip.
- **A PTE with VALID clear**: "the 21464 hangs if an invalid TB entry is
  written"; PALcode must branch around it (Restriction 9) [IDS pp. 505,
  578]. **[decision, N10]**: the emulator drops the write and counts it
  under a trace, never hangs. Writing R31 completes without an entry
  [IDS p. 578]: the same.
- **The Mbox chapter's older names** (DTB_IA, DTB_IAP, DTB_IASN, TPUGRP,
  DTB_ALTMODE, VA_CTL, MM_STAT, CM [IDS pp. 305-311]) are the chapter-16
  registers' predecessors **[conflict]**; chapter 16 is followed.
- **MFPR of CC** is "TBD" [IDS p. 482]. **[decision]** (part of N7): it
  reads the register, as an MFPR of any readable IPR does; RPCC reads it
  as on the 21264.
- **PAL_BASE<51:15>** names VA<51:15> in its table and PA<51:15> in its
  figure, while PA is 48 bits [IDS p. 499]. **[decision]** (part of N6):
  a physical address; <51:48> MBZ.

### 1.5 The PALcode entry points

| Offset | Chapter 16 [IDS pp. 498-499] | Chapter 22 [IDS pp. 633-634] | Writes [IDS p. 633] | 21264 [HRM] |
| --- | --- | --- | --- | --- |
| 0x000 | reserved | reserved: the entry from SWPPAL or the SROM into the reset code | | |
| 0x080 | | available | | |
| 0x100 | DTBM_DOUBLE | DTBM_DOUBLE | EXC_ADDR | DTBM_DOUBLE_3 |
| 0x180 | DTBM_DOUBLE_ALT | DTBM_DOUBLE_ALT | EXC_ADDR | DTBM_DOUBLE_4 |
| 0x200 | FEN | FEN | EXC_ADDR | FEN |
| 0x280 | UNALIGN | UNALIGN | EXC_ADDR, EXC_SUM, VA, VA_FORM, M_STAT | UNALIGN |
| 0x300 | DTBM_SINGLE | DTBM_SINGLE | **DTBMS_RET_ADDR**, EXC_SUM, VA, VA_FORM, M_STAT | DTBM_SINGLE |
| 0x380 | DFAULT | DFAULT | EXC_ADDR, EXC_SUM, VA, VA_FORM, M_STAT | DFAULT |
| 0x400 | OPCDEC | OPCDEC | EXC_ADDR | OPCDEC |
| 0x480 | IACV | IACV | EXC_ADDR | IACV |
| 0x500 | MCHK | MCHK | EXC_ADDR, M_STAT | MCHK |
| 0x580 | ITB_MISS | ITB_MISS | EXC_ADDR, IVA_FORM | ITB_MISS |
| 0x600 | ARITH | ARITH | EXC_ADDR, EXC_SUM | ARITH |
| 0x680 | INTERRUPT | INTERRUPT | EXC_ADDR | INTERRUPT |
| 0x700 | MT_FPCR | MT_FPCR | EXC_ADDR | MT_FPCR |
| 0x780 | IMCHK | IMCHK | EXC_ADDR | RESET/WAKEUP |
| 0x800 | DTBM_SINGLE_CONS | DTBM_SINGLE_CONS | as DTBM_SINGLE | |
| 0x880 | ITB_MISS_CONS | ITB_MISS_CONS | EXC_ADDR, IVA_FORM | |
| 0x900 | BAD_JUMP_IVA | BAD_JUMP_IVA | EXC_ADDR, EXC_SUM | |
| 0x980 | FAULT_RESET | FAST_RESET | | |
| 0xA00 | WAKEUP | WAKEUP | | |
| 0xA80 | IP_RESET | TPU_RESTART | | |
| 0xB00 | RESET | MILD_RESET (and Cold and Tepid reset) | | |
| 0xB80 | | DST_NXM | EXC_ADDR | |
| 0xC00-0xF80 | | available | | |
| 0x2000 + | CALL_PAL | | S1 (above) | CALL_PAL |

- **CALL_PAL** goes to PC<51:15> = PAL_BASE<51:15>, PC<14> 0, PC<13> 1,
  PC<12> function<7>, PC<11:6> function<5:0>, PC<5:2> 0, PC<1> the
  current PC<1>, PC<0> 1 [IDS p. 499, read on the scan]: the 21264's
  0x2000 + function<7> x 0x1000 + function<5:0> x 0x40, the privileged
  functions 0x00-0x3F at 0x2000-0x2FC0 and the unprivileged 0x80-0xBF at
  0x3000-0x3FC0. PC<1> keeps SuperPALmode across a CALL_PAL from it.
  On the 21264 a privileged function (0x00-0x3F) outside kernel mode,
  and any function in 0x40-0x7F or above 0xBF, takes the OPCDEC entry
  instead [HRM p. 197]; the 21464 keeps that rule **[inference]**: the
  IDS does not repeat it, and keeps the rest of the computation.
- **The names at 0x980, 0xA80 and 0xB00** **[conflict]**. Chapter 22 is
  the later and the fuller (it gives each one's cause: fast reset after
  loss of lockstep, a restart requested by another TPU, mild reset after
  a core error, and overlays Cold and Tepid reset on MILD_RESET "since
  both ... execute from SROM code") [IDS pp. 631-633]. **[decision,
  N11]**: chapter 22's names and causes; power-up enters at 0xB00
  (MILD_RESET, "RESET" in chapter 16 and in this packet), and DST_NXM at
  0xB80 is not raised (no machine checks, as on every Alphabox row,
  [cpu-fidelity.md](../cpu-fidelity.md)).
- **DTBM_SINGLE** returns through DTBMS_RET_ADDR, not EXC_ADDR, so a
  fault inside the miss flow cannot lose the return PC [IDS p. 483]. The
  IDS's own writer block is `HW_MTPR S0 -> DTB_TAG` (twice), `HW_MTPR S1
  -> DTB_PTE` (twice), `HW_MFPR DTBMS_RET_ADDR -> S1`, two NOPs and an
  ALIGN_NOP in slot 7 [IDS p. 576]: the flow then returns with `RET (S1)`
  **[inference]**.
- **ITB miss and DTB miss in console mode** (I_MODE<CONSOLE>,
  M_MODE<CONSOLE> set) go to the _CONS entries, so a console's own
  translations can be handled apart from the OS's [IDS pp. 491, 508].
- **The double miss**: a HW_LD/VPTE that misses vectors to DTBM_DOUBLE or,
  with M_CTL<DBL_ALT>, to DTBM_DOUBLE_ALT; "the decision is the
  discretion of PALcode" [IDS p. 507]. Appendix C moves the bit to I_CTL
  and M_CTL both as DOUBLE_MISS_CONTROL and says it controls all double
  TB misses, I-stream and D-stream [IDS p. 688] **[conflict]** of
  placement; **[decision, N12]**: M_CTL<22> as chapter 16 has it.
- **BAD_JUMP_IVA**: a jump target outside the virtual address space
  [IDS p. 629]. **Bad VA sign** in the D-stream goes to DFAULT with
  M_STAT<ACV> ("this includes bad virtual addresses") and the IDS's
  margin note asks for a separate bit [IDS pp. 510, 630]; **[decision]**
  (part of N12): ACV, no new bit.

### 1.6 Memory management

- **52-bit VA, 48-bit PA, 8-bit ASN, 8 KB and 64 KB pages** [IDS p. 43].
  With 8 KB pages a PTE's 32-bit PFN reaches PA<44:13> ("the 21464
  cannot address more than 16TB when in 8K page mode" [IDS p. 504];
  45 bits are 32 TB **[conflict]** of arithmetic, harmless: the IDS also
  caps physical memory at 2^44 bytes [p. 44]); with 64 KB pages
  PA<47:16> [IDS pp. 307, 496, 504].
- **Valid combinations** [IDS p. 690, Table C-3]: 43-bit VA with 8 KB
  pages ("the addressing mode used by Tru64 UNIX and OpenVMS today"),
  52-bit VA with 64 KB pages, with or without reduced page tables. The
  others are invalid. **[conflict]**: the IVA_FORM and VA_FORM figures
  label their second format "VA_SIZE=1, PAGE_SIZE=0" [IDS pp. 497, 512],
  which Table C-3 and I_CTL's own field ("invalid if PAGE_SIZE = 0"
  [p. 490]) forbid: a label error. **[decision, N13]**: an invalid
  combination behaves as 43-bit VA, 8 KB pages for translation and
  formats; nothing raises an error.
- **Superpages** (I_CTL and M_CTL SPE<2:0>, kernel mode only, any
  combination) [IDS pp. 490, 507], in Appendix C's form [IDS pp. 688-689]:
  SPE<2> (SPE52) VA<63:50> = 0x3FFE maps VA<47:0> to PA<47:0>, VA<49:48>
  ignored; SPE<1> (SPE43) VA<63:41> = 0x7FFFFE maps PA<40:0> = VA<40:0>
  with PA<47:41> copies of VA<40>; SPE<0> (SPE32) VA<63:30> = 0x3FFFFFFFE
  maps PA<29:0>, PA<47:30> cleared. SPE43 is Linux's KSEG,
  0xfffffc0000000000: memory below 1 TB and, with VA<40> set, the top
  terabyte of the PA, PA<47:40> = 0xFF, where I/O sits
  ([section 2.1](#21-the-address-map)).
- **Sign-extension check, Appendix C's order** [IDS p. 695]: the
  superpage matches are tested first, each checking the address all the
  way to bit 63; only an address no superpage takes is checked against
  VA_SIZE (VA<63:52> or VA<63:43> the sign extension of VA<51> or
  VA<42>) and translated. So SPE52 works with 43-bit virtual addresses
  (ECO 129's mixed mode; the 21264 needed VA_48 for its 48-bit
  superpage) [IDS pp. 685, 695]. **[decision, N14]**: Appendix C is
  followed where chapter 16 is silent (chapter 16 already points to it
  for REDUCED_PT [p. 489]); its DOUBLE_MISS_CONTROL placement is N12's.
- **The TB match** [IDS pp. 305-307]: VA<51:13> (fewer bits with a
  granularity hint), the TPU group's valid bit, and ASN or ASM. Each DTB
  entry has four group valid bits, at most one set; a fill sets the
  writer's group's [IDS p. 306]. Granularity hints are 8x, 64x, 512x of
  either page size [IDS p. 306]; in 64 KB page mode "all granularity hint
  values are increased by one" [IDS p. 307]. The ITB the same, by
  I_PCTX's group [IDS p. 495].
- **Invalidation**: ITB_IM and DTB_IM by mode (all groups, this group,
  this group's ASM or non-ASM entries, this group and ASN, and the write
  pointer) [IDS pp. 494, 503]; ITB_IS and DTB_IS by VA in this group and
  ASN; IC_FLUSH and IC_FLUSH_ASM per group [IDS p. 493]. Superpage
  enables "only affect the group of TPUs belonging to the writer of
  M_CTL" [IDS p. 311] **[conflict]** with M_CTL being per TPU in Table
  16-1 [p. 479]; **[decision]** (part of N14): per TPU, as Table 16-1.
- **Groups and modes**: Mode 2, every TPU in its own group (SMP-like,
  "expected to be used by VMS"); Mode 3, all four in one group ("Unix")
  [IDS p. 306]. TPU_GRP is in both I_PCTX and M_PCTX, which PALcode
  writes alike [IDS pp. 492, 509]; "whenever a TPUGRP is reused the DTB
  must be flushed for that TPUGRP" [IDS p. 311].
- **Big-endian**: M_CTL<BIG_ENDIAN> inverts the low address bits by data
  size and the shift amount of EXTxx, INSxx and MSKxx [IDS p. 507]; VA
  reports va', which PALcode adjusts back [IDS p. 512]. **[decision,
  N15]**: not implemented in S11 (no Alphabox guest runs big-endian);
  the bit is kept and reads back.

What the emulator's TB model needs that it lacks: a group tag per entry
and per-group invalidation, 64 KB pages and their GH shift, 52-bit tags,
the three superpage forms, Appendix C's check order. The EV6 rows' TB code
stays as it is; the `ev8n` family gets its own lookup **[decision]**
(part of N16, [section 1.8](#18-the-ev8n-row)).

### 1.7 TPUs: start, sleep, interrupts

- **What each TPU has**: its integer and FP registers, PC, and the IPRs
  marked per TPU above; caches, TBs and predictors are shared [IDS p. 46].
  No TPU can read another's IPRs ("currently no mechanism exists"
  [IDS p. 477]).
- **Its number**: TPU_CNFG<57:56> [IDS p. 488]. The chip's network
  address: R_WHOAMI [IDS p. 527]. The processor number PALcode returns
  from WHAMI is PALcode's choice; in the native track the owner's
  original D6 is natural: PID x 4 + TPU ([section 3.4](#34-smp-whami-ipis-per-tpu-state)).
- **Start at reset.** Every reset class enters at a PALcode entry
  [IDS p. 631], and WAKEUP and TPU_RESTART are "a special class of
  interrupt ... the IBox only vectors into when a TPU is restarted or
  woken from sleep mode" [IDS p. 628]. Nothing says whether TPUs 1-3 run
  at reset. QUIESCE_TIMEOUT's section says "the background for booting
  document assumes the ability to define an infinite wait condition in
  the Suspending TPUs section. Disabling the timer should suffice"
  [IDS p. 511]: the boot flow suspends TPUs with LDx_ARM and QUIESCE,
  timer off **[inference]**. **[decision, N17]**: at power-up all four
  TPUs of every chip enter at 0xB00 with their TPU_ID; PALcode lets TPU 0
  go on and parks TPUs 1-3 with LDQ_ARM on a per-TPU mailbox and QUIESCE
  with QUIESCE_TIMEOUT<TIMEOUT_EN> = 0; a write to the mailbox (by the
  console, or the OS through PALcode) releases a TPU, which jumps where
  the mailbox says. No new register; the mechanism is the IDS's own.
  WAKEUP and TPU_RESTART are not raised in S11 (N1).
- **[conflict]** with ev8-isa.md 3.1's note: QUIESCE_TIMEOUT's enables
  reset to 0 [IDS p. 511], which in the native interface means a QUIESCE
  after reset sleeps until an interrupt: with WATCH_EN clear "a TPU will
  not awaken when an access to the WATCH_PHYS_ADDR is detected", and with
  TIMEOUT_EN clear it does not time out [p. 511]. The native row follows
  the register (enables 0, TIMEOUT 0x280) **[decision]** (part of N17):
  PALcode sets what it wants, WATCH_EN for the parking above; the
  EV7-compatible row keeps D16.
- **Interrupts**: six levels into each TPU [IDS p. 465], ISUM and IER_CM
  per TPU [IDS p. 479]; how a chip-level source reaches one TPU is
  [section 2.4](#24-interrupts-steering-to-tpus-ipis-the-timer).

### 1.8 The `ev8n` row

A native EV8 is a family of its own ([ev8.md](ev8.md), S1 and S2;
`cpu/CpuModel.hpp`): what the family decides (the address map, the
start-up, the interrupts, here also the privileged interface) all differ
from the EV7's. In `cpu_model` terms **[decision, N16]**:

| Field | `ev8n` | Why |
| --- | --- | --- |
| name, part | `ev8n`, "21464 (EV8), native interface" | beside `ev8` |
| implver | 4 | [IDS p. 76] |
| type, minor | 18, 0 | D3: the HWRPB's processor type is written by our console, and 18 is the next free value [ARM pp. 805-807] |
| chip_id | 0 | CPU_CNFG<CHIP_ID> has no value [IDS p. 487]; nothing reads it but our PALcode |
| amask | the `ev8` row's, stage by stage (0x1F07 since S5 and S6) | the same rule: report what executes |
| family | a new `CPU_FAMILY_EV8` | the PALcode instructions, IPRs, entries, TB model and per-TPU state are the family's; boards take it as they take EV7 rows (the ES47, ES80 and GS1280 in 21464 mode, [section 2](#2-the-marvel-model-in-21464-mode)) |
| ext | `CPU_EXT_EV8` | the user-visible instructions are those of the `ev8` row, shared code |
| caches | 64, 64, 3072 | as `ev8` |
| tpus | 4 (a new field, or the S4 `tpus` key with a default of 4) | the chip's four TPUs, each an emulated processor (a host thread) with its own IPRs; the shared TB is modelled per TPU with group tags (no cache model, [ev8.md](ev8.md) "How the OS sees them") |

The core already asks the family where rows may differ
(`cpu_family_unused_misc_is_nop`, the board check in `CAlphaCPU`'s
constructor); the EV8 family takes 0x19, 0x1B, 0x1D, 0x1F and 0x1E with
decoders of its own, a `cpu_pal_ev8.hpp` beside `cpu_pal.hpp` (557 lines
today). Both JITs compile the 21264's PALcode instructions through
helpers (HW_MFPR, HW_MTPR, HW_RET, `jit/jitemit_a64.hpp`); on an `ev8n`
row the 21464's forms are left to the interpreter at first, a block
ending before each, and compiled through helpers once the interpreter's
are proven **[decision]** (part of N16). The user-mode code is the `ev8`
row's, compiled as it is today.

## 2. The Marvel model in 21464 mode

The EV8 was designed for Marvel systems: its GIO port talks to the
Marvel CMM, its I/O bridge is the IO7, its network and coherence are the
21364's ([ev8.md](ev8.md), section 2). What the 21464 changes is the
address width, the node numbering, the interrupt registers and the GIO
word. This stage defines the Marvel model (`chipsets/marvel/`,
`platforms/es47/`, `cpu/ev7/`) in a **21464 mode**, selected by the
processor family (an `ev8n` processor on an ES47, ES80 or GS1280 row);
the EV7 rows and the `ev8` row keep the EV7's mode unchanged.

### 2.1 The address map

What the IDS gives:

- 48-bit physical addresses; a load "to I/O space (physical address bit
  47 equals 1)" [IDS pp. 43, 298]; the Mbox's MAF entry: "I/O space:
  PA<47> = 1" [IDS p. 354].
- 32 GB per processor, "35 address bits" [IDS p. 43]; at most 2^44 bytes
  of memory in a system [IDS p. 44].
- In packets, a memory address is a block address at the directory node
  DPID (10 bits): ADD<34:6>, with ADD<36:35> unused and ADD<34> used
  only "in 32GB/processor mode"; an I/O address is IOADD<37:6> at
  DESTPID (11 bits, <10> the I/O bit), IOADD<37:35> unused
  [IDS pp. 457-458]. So a node's memory and a node's I/O space each
  have a 35-bit offset, as on the EV7.
- The chapter "Physical Addressing and Input/Output" is an empty
  placeholder [IDS p. 645]: how a PA splits into node and offset is not
  given.
- The kernel superpage SPE43 (Linux's KSEG) maps VA<40:0> and copies
  VA<40> into PA<47:41> [IDS pp. 688-689]: it reaches memory below 1 TB
  and the top terabyte of the PA, PA<47:40> = 0xFF.

The EV7's map ([marvel.md](marvel.md), "Marvel's address map";
`cpu/ev7/Ev7.hpp`): I/O at PA<43>, the PID inverted in PA<43:35>
(`EV7_IPE(pe) = (~pe & 0x1ff) << 35`), so that small PIDs' I/O spaces sit
at the top of the 44-bit space, where the 21264's KSEG (VA<40> copied into
PA<43:41>) reaches them; a 4 MB register window per PID at `| 0x7FFC <<
20`; IO7 ports at `| (~port & 7) << 32`; memory of PID n at `((n & 3) |
(n & 0x7c) << 1) << 34`.

**[decision, N18]**: the 21464-mode map is the EV7's with the inverted
field widened to the top of the 48-bit PA:

| Space | 21464 mode | EV7 |
| --- | --- | --- |
| I/O of node n | `ipe8(n) = (~n & 0x1fff) << 35`: PA<47> = 1 (the I/O bit) and PA<46:45> = 11 for every 10-bit PID, PA<44:35> = ~PID | `(~n & 0x1ff) << 35` |
| Register window | `ipe8(n) \| 0x7FFC << 20 \| off`, 4 MB | the same with `ipe` |
| IO7 port p | `ipe8(n) \| (~p & 7) << 32` | the same with `ipe` |
| Memory of PID n | `((n & 3) \| (n & 0x3fc) << 1) << 34`: the EV7's for PIDs below 128, 32 GB per PID in two halves | `((n & 3) \| (n & 0x7c) << 1) << 34` |
| Physical mask | 48 bits | 44 bits |
| Node 0's window | 0xFFFF_FFC0_0000 | 0xFFF_FFC0_0000 |

- **Why**: for PIDs 0-31 the low 41 bits of every address are the EV7's,
  and PA<47:41> are all ones for I/O and all zeros for memory. Linux
  forms every Marvel address as `IDENT_ADDR | (pa & ((1UL << 41) - 1))`
  (`EV7_MASK40`, `EV7_KERN_ADDR` in `core_marvel.h`), which SPE43 maps
  back to the same PA on a 21464; so the kernel's Marvel code keeps its
  arithmetic, and only `EV7_PE_MASK` changes (0x1ff to 0x1fff)
  ([section 5](#5-linux-on-the-native-ev8-s13)). The same holds for our
  console and PALcode, and for Alphabox's decode (`csr_decode`,
  `io7_base`, `memory_pid`).
- **Every PID is reachable** through SPE52 (VA 0xFFF8_0000_0000_0000 |
  PA, which works with 43-bit virtual addresses under Appendix C's check
  [IDS p. 695]); through SPE43 only PIDs 0-31, as on the EV7. Alphabox's
  largest Marvel machine has 16.
- **The alternative**, PA<47> = 1 with the PID in PA<46:37> not inverted,
  is closer to a fresh design and reaches nothing through SPE43: every
  Linux macro and our console would need SPE52 addresses. Not proposed.
- **The memory formula** could instead be `n << 35` (32 GB contiguous per
  node, what ADD<34>'s "32GB/processor mode" suggests). The EV7's is
  proposed because Linux's `marvel_node_mem_start`, the console's
  memory descriptors and Alphabox's `CMarvel` keep it, and the IDS does
  not decide.

### 2.2 Node IDs, PIDs and coordinates

- R_WHOAMI "contains a 10-bit address defining the node's address": EW
  <9:5> and NS <4:0>, the torus coordinates [IDS p. 527].
- The routing table R_ROUT has 532 entries, "one for each of the 512
  nodes" and 20 for the directory's sharing-mask bits; each gives the
  destination's coordinates in 4 bits each, DNS <19:16> and DEW <15:12>,
  with direction and access bits [IDS pp. 526-527].
- A PID is 10 bits, 11 with the I/O bit [IDS p. 457], but the packet
  formats say RPID<9:8>, SPID<9:8>, DESTPID<9:8> and DPID<9:8> are
  unused, zeros [IDS p. 458].
- **[conflict]**: 512 processors [IDS p. 43] and a 16x32 torus [p. 44]
  need 9-bit PIDs and 5-bit coordinates; the route table's 4-bit
  coordinates and the packets' 8 used PID bits are 256 nodes, the 21364's
  numbers the chapters were copied from.
- Alphabox's EV7 model numbers PIDs from coordinates by the console's
  `coord2id` and keeps both (`chipsets/marvel/Topology.*`); its
  RBOX_WHOAMI holds the PID, a guess.

**[decision, N19]**: in 21464 mode the PIDs are **dense**, the
topology's processor index (0 to n - 1), not the EV7 console's
`coord2id` (which only an EV7 console needs, and which numbers an ES80's
eight processors 0, 1, 8, 9, 16, 17, 24, 25); R_WHOAMI holds the node's
coordinates in the IDS's layout; CPU_BASE<9:0> holds the PID (N6); the
route table is indexed by PID with the IDS's entry layout (DNS <19:16>,
DEW <15:12>, the flags of [IDS pp. 526-527]), which the OS does not read
(OpenVMS's walk of RBOX_ROUTE through cserve 0x4f, [marvel.md](marvel.md),
is the EV7 PALcode's; our PALcode has no such call). Dense PIDs keep
every Alphabox machine inside SPE43's reach (N18) and make WHAMI = PID x 4
+ TPU dense (N25): an eight-chip ES80 is CPUs 0-31, inside Linux's
NR_CPUS limit of 32. PIDs made of the coordinates (EW << 5 | NS, the
literal reading of R_WHOAMI) would put a GS1280's second column at PID 32
and beyond, out of both.

### 2.3 The Rbox and Zbox registers

They are listed as IPRs but have no index [IDS pp. 479-481]; another node
reaches them with RdIPR and WrIPR, "identical to RdQWs [and WrQWs] except
that the different opcode indicates that the reference was within the
range of the processor I/O space rather than the ASIC I/O space (i.e. the
address references an 21364 IPR)" [IDS pp. 428, 430], and the I/O ASIC
reaches any node's "system IPR's" the same way, which is how interrupts
arrive [IDS p. 463]. The chip's own PALcode reaches them as I/O space too
**[inference]**: no HW_MFPR index exists for them. So they live in each
node's register window, and **[decision, N20]** at the EV7's offsets
where the EV7 has the register, the IDS's own prose giving the mapping:

| 21464 [IDS pp. 479-480, 514-528] | EV7 window (Alphabox, `Ev7Csr.hpp`) | 21464 mode |
| --- | --- | --- |
| R_CFG1 | RBOX_CFG 0x000 | the same offset |
| R_CFG2 | none | 0x0E0 **[decision]** |
| R_WHOAMI | RBOX_WHOAMI 0x030 | the coordinates (N19) |
| R_OVER (overall timer control) | RBOX_TCTL 0x040 **[inference]**: the EV7's timer control | the same offset |
| R_INT_STAT | RBOX_INT 0x050 ("the RBOX_INT IPR" [IDS p. 464]) | the same offset; 32 bits [p. 464]; bits per N22 |
| R_INT_MASK | RBOX_IMASK 0x060 [IDS p. 464] | per TPU (N22): TPU 0's here and at 0x3F0000, TPU t's at 0x3F0000 + 0x40 x t |
| R_INT_REQ | RBOX_IREQ 0x070 [IDS p. 465] | the same offset |
| R_INT_QUE | RBOX_INTQ 0x080 [IDS p. 464] | the same; a read leaves the head, a write removes it [p. 528] |
| R_INT_QUEADD | RBOX_INTA 0x090 [IDS pp. 430, 464-465] | the same; full: WrIONack [p. 528] |
| R_INTER_TIM | RBOX_IT 0x0A0 | the same; ITV <7:0> [p. 528] |
| R_SCRATCH1, R_SCRATCH2 | RBOX_SCRATCH1 0x0B0, SCRATCH2 0x0C0 | the same (both "TBD: Do we need this?" [p. 528]) |
| R_LOC_ERR | RBOX_L_ERR 0x0D0 | the same |
| R_ROUT | RBOX_ROUTE 0x2000, 0x114 entries | 532 entries at the same base, the IDS's layout (N19) |
| R_n_CFG1, ERR, PERF, T1CFG, T2CFG (N, S, E, W) | the port blocks 0x4000, 0x6000, 0x8000, 0xA000, whose +0x40 is RBOX_n_T2CFG (`Ev7Csr.cpp`) | the same, R_n_T2CFG the EV7's at +0x40; R_n_CFG2 at a free offset of each block **[decision]** |
| R_IO_CFG1, ERR, PERF, T1CFG; R_IO_CFG2, T2CFG, BUFSIZ | the RBOX_IO_CFG block 0xC000 ("RBOX_IO_CFG", "RBOX_IO_BUF" [IDS pp. 455, 464]) | the same block, the new ones at free offsets |
| ZBOXn_* (two controllers) [IDS pp. 528-567] | Zbox0 0x10000-0x14110, Zbox1 0x16000-0x1A110 | the same blocks **[inference]**: the same controller family (Tarantula reuses the EV8's Zbox [TAR p. 284], and the EV8's register names are the 21364's) |
| GIO_CNFG, GIO_ADDR, GIO_DATA [IDS pp. 472-474] | GIO_CFG 0x100, GIO_CTL 0x120, GIO_DAT 0x110 | the same (section 2.5) |
| (Cbox, Bbox, pads, OCLA) | 0xE000, 0x1C000, 0x20000, 0x24000 | kept, unused by our PALcode (the EV7 PALcode's error path is what reads them) |
| VCTL (EV7-compatible mode, 0x3F0000 of each processor's window) | | not decoded: an IPR in the native interface (N8); the block holds the per-TPU masks |

The registers the model only stores (configuration, timers, errors,
performance, DRAM timing) answer as configured and clean, as the EV7's
do today; our console configures none of them.

### 2.4 Interrupts: steering to TPUs, IPIs, the timer

What the IDS gives:

- Requests arrive as network writes "to a block of processor-specific
  registers"; a requester receives WrIOAck, or WrIONack when it wrote the
  interrupt-ID queue and the queue is full, and may then "send the
  request to another processor, retry the same one, or wait"
  [IDS pp. 75, 465].
- Two mechanisms: a queue of 24-bit interrupt IDs (RBOX_INTQ, RBOX_INTA)
  and a 32-bit mask (RBOX_INT) with an enable mask (RBOX_IMASK); some
  mask bits are reserved (interrupt queue, performance counter, error,
  I/O error), "other bits will be available for general software use,
  e.g. interprocessor interrupt"; RBOX_IREQ launches one [IDS
  pp. 464-465].
- Six levels into the core: 0 correctable or performance count, 1 the
  interrupt queue, 2 interval timer, 3 interprocessor or software alert,
  4 halt or other, 5 uncorrectable or machine check [IDS p. 465].
- IPIs set a mask bit in the target's RBOX_INT through its RBOX_IREQ,
  and "will typically not use the interrupt queue method" [IDS p. 465].
- The queue holds four entries [IDS p. 464] and two [IDS p. 528]
  **[conflict]**.
- R_INT_STAT, R_INT_MASK and R_INT_REQ: "TBD" [IDS pp. 527-528].
- "The processor has multiple threads, each capable of running PAL
  code, interrupt-level service, or user code while the others are
  active. This implies a requirement for interlocking among the threads
  which might be servicing interrupts" [IDS p. 75]. For the GIO
  interrupt, "a mask register will define which TPU(s) are to receive"
  it, pre-set by software [IDS p. 476].

What the EV7 model does (`chipsets/marvel/Ev7Csr.cpp`): one RBOX_INT and
RBOX_IMASK per chip; RBOX_INT & RBOX_IMASK drives the six EI lines by the
console PALcode's decode (errors on EI0, the queue on EI1, the timer bit
15 on EI2, interprocessor bits 21-23 on EI4); an unbounded queue.

**[decision, N21]**: the queue is **four** entries deep, the chapter-13
design figure; a fifth IID is refused, and the IO7 model resends it, as
the IDS has the requester do. Four TPUs servicing one queue are the
reason to take the larger figure **[inference]**.

**[decision, N22]**: steering by a per-TPU mask over one chip-wide
status register:

- **R_INT_STAT** (0x050), one per chip, 32 bits, with a bit layout the
  21464 mode fixes, since our PALcode is the only reader:

  | Bits | Source | Level | Set by | Cleared by |
  | --- | --- | --- | --- | --- |
  | <3:0> | interprocessor interrupt to TPU 0-3 | 3 | R_INT_REQ writes (any node) | writing 1 to the bit |
  | <7:4> | interval timer tick for TPU 0-3 | 2 | each R_INTER_TIM period sets all four | each TPU its own |
  | <11:8> | halt request to TPU 0-3 | 4 | R_INT_REQ, the CMM's halt | writing 1 |
  | <12> | the interrupt queue holds an IID | 1 | R_INT_QUEADD | the queue emptying |
  | <13> | GIO interrupt (GIO_INT) | 4 | the CMM | writing 1 |
  | <14> | GIO high-priority interrupt (GIO_HINT) | 4 | the CMM | writing 1 |
  | <15> | performance counter | 0 | (not raised) | |
  | <17:16> | correctable and uncorrectable error | 0, 5 | (not raised) | |
  | <31:18> | software use | 3 | R_INT_REQ | writing 1 |

- **R_INT_MASK[t]**, one per TPU: TPU t's level L is asserted while some
  bit of level L is set in R_INT_STAT & R_INT_MASK[t]. Each TPU's level
  lines go into its own ISUM<EI>, gated by its own IER_CM<EIEN> (N7). The
  OS decides which TPU takes device interrupts by which masks enable
  bit 12 (Linux: one TPU per chip, [section 5](#5-linux-on-the-native-ev8-s13)).
  These masks are the GIO mask the IDS promises (bits 13 and 14), with no
  register of their own **[decision]**.
- **R_INT_REQ** (0x070): a write sets its bits in that chip's R_INT_STAT.
  An IPI to processor (PID p, TPU t) writes 1 << t to p's R_INT_REQ.
- **R_INTER_TIM** (0x0A0): ITV <7:0> [IDS p. 528], the EV7's RBOX_IT
  field; the period is the EV7's as M4 measured it, cpu_hz / ((ITV + 1) x
  2^17) ([marvel.md](marvel.md), "Interrupts") **[inference]**. One timer
  per chip, a tick per TPU (bits <7:4>), so each TPU, which Linux treats
  as a CPU with its own timer, gets its own.
- **The IO7** keeps its target field: LSI_CTL and MSI_CTL<22:14> name a
  PID, nine bits (`Io7.hpp`), and the IID goes into that chip's queue.

This **revises D5 for the native track**. D5 proposed that IO7 targets
name a PID and a TPU; the IO7 is the existing ASIC, its target field has
room for a PID only, and the IDS's queue is per chip [IDS p. 464], so the
TPU is chosen by the masks, inside the chip: the "interlocking among the
threads" the IDS anticipates. D5's per-TPU masks and request bits stand.

### 2.5 GIO and the CMM

- **The registers** [IDS pp. 472-474]: GIO_CNFG, DIVISOR <7:1> (zero
  stops GIO_CLK); GIO_ADDR, ADDR <7:1> and START_READ <0> (a write with
  START_READ starts a read); GIO_DATA, DONE <63> and DATA <31:0> (a write
  starts a write transaction; DONE says the port is idle and a read's data
  valid). This is what Alphabox's model implements for the EV7 (GIO_CTL
  <n:1> the far register, <0> go; GIO_DAT<63> done; [marvel.md](marvel.md),
  M2), except the width.
- **Differences from the 21364** [IDS p. 476]: no enable bit in GIO_CNFG;
  32-bit transactions where the 21364 has 64-bit writes and 63-bit reads
  ("the largest GIO operation defined in Marvel is currently 23 bits");
  a 4-bit datapath, invisible to software.
- **The CMM's registers** [IDS p. 474]: 00h CPUx_CTRL, 01h CPUx_DMA_Data,
  02h CPUx_DMA_Addr, 03h CPUx_RIMM_Serial_Port, 04h CPUx_Com0_Xmit,
  05h CPUx_Com0_Rcv, 06h and 07h Com1. Alphabox's CMM (`platforms/es47/Cmm.hpp`):
  register 0 status and control, 1 the byte window's 16-bit data, 2 the
  window's address, 4 and 5 the console terminal, with registers 8 (the
  PALcode's state word) and 9 (the terminal's pending interrupts) beyond
  the IDS's list. The IDS's "16-bit DMA transactions currently planned"
  [IDS p. 475] is the model's 16-bit window. **Nothing to change** but
  the width.
- **TPUs and GIO** [IDS pp. 475-476]: one TPU at a time may use the port;
  software keeps the others off. Our PALcode does it with a lock word in
  memory (LDQ_L/STQ_C) **[decision]** (part of N22); the EV7 console's
  lock at window offset 0x80000 stays in the model for the EV7 rows.

**[decision, N23]**: in 21464 mode GIO_DATA carries 32 bits: a write
passes DATA<31:0> on, a read returns DONE in <63> and the far register's
low 32 bits. The CMM model is unchanged.

### 2.6 The IO7

- The IO7 is the 21464's I/O ASIC: its PID is its processor's with the
  I/O bit set [IDS pp. 457, 463]; it reaches any node's system IPRs, and
  its interrupts are writes to RBOX_INTA [IDS pp. 463, 465].
- In 21464 mode its space is `ipe8(n) | (~port & 7) << 32 | off` (N18),
  its registers at the EV7's offsets; its DMA windows translate to PAs
  below 2^45, which is all memory can be (N18); its interrupt targets
  name a PID (N22).
- Nothing else of the IO7 changes: Linux's `core_marvel.c` and the
  console use its port and window registers as on the EV7.

### 2.7 What changes in the code

| Where | In 21464 mode |
| --- | --- |
| `cpu/ev7/Ev7.hpp` (the map; 21 uses in 5 files) | the map as a value chosen per machine: physical mask, I/O bit (47), `ipe` width (13 bits), `csr_decode`'s PID (10 bits) |
| `chipsets/marvel/Marvel.*` | `phys_mask()` 48 bits; decode through the map; memory per PID unchanged |
| `chipsets/marvel/Ev7Csr.*` | R_INT_STAT's layout and per-TPU masks (N22), the queue depth (N21), R_WHOAMI as coordinates, R_CFG2 and the other new registers, the route entry layout (N19); the interval tick to four TPUs; the start hand-off (M11) stays, unused by our console |
| `chipsets/marvel/Io7.*` | the hose base through the map; an IID refused when the queue is full is resent |
| `chipsets/marvel/Gio.*` | 32-bit data (N23) |
| `chipsets/marvel/Topology.*` | coordinates into R_WHOAMI; dense PIDs in 21464 mode (N19), `coord2id` kept for the EV7 mode; the CMM's per-module areas still follow PID >> 1, which dense PIDs keep for two-processor modules |
| `platforms/es47/Cmm.*` | nothing |
| `cpu/ev7/Ev7Reset.cpp` | an `ev8n` reset instead of the XSROM handoff: PAL_BASE and the PC at the PALcode's 0xB00, CPU_BASE, TPU_CNFG, the IPRs' reset values, on all four TPUs (N17, [section 3.6](#36-built-and-loaded)) |
| `platforms/Platforms.cpp` | the ES47, ES80 and GS1280 rows accept `ev8n` processors; the processor family selects the mode |

**Size**: about 600 to 900 lines, most in `Ev7Csr` and the map
**[guess]**, against 1.8 to 3.6 k estimated for M1 to M3, the EV7's
first model ([marvel.md](marvel.md), "Plan").

## 3. The Alphabox PALcode (S12)

No EV8 PALcode was ever published ([ev8.md](ev8.md), "Sources"), and the
EV7 PALcodes in the Marvel console cannot run on the native interface
(they use 21264 indexes and HW_RET, [ev8.md](ev8.md), "The PALcode
interface"). The project writes one: the UNIX interface Linux uses,
first and only, since an OpenVMS PALcode would also need SRM callbacks
OpenVMS's boot depends on ([ev8.md](ev8.md), option B).

### 3.1 The interface Linux uses

The ARM's Alpha Linux PALcode is the Tru64 UNIX one with fewer calls
[ARM pp. 535-562; codes, Tables C-13 and C-14, pp. 798-799]. What each
must do on the 21464's registers:

| CALL_PAL | Code | Linux uses it for | On the 21464 |
| --- | --- | --- | --- |
| halt | 00 | `halt`, `poweroff`, `reboot`: back to the console | record the halt in the per-CPU slot (reason, PC, PS) and enter the console's halt path; a TPU other than the console's parks (N17) |
| cflush | 01 | (not on Marvel) | nothing: no cache is modelled |
| draina | 02 | after machine-check handling | MB; machine checks are never raised |
| cserve | 09 | 0x48 PUT_TOY and 0x49 GET_TOY on Marvel, boot CPU only (`__marvel_access_rtc`, [marvel.md](marvel.md), "Interrupts") | the TOY bytes in the CMM's memory through the GIO byte window (registers 1 and 2), under the GIO lock word; any other function returns failure |
| swppal | 0A | (a bootstrap switching PALcode) | returns 1, "unknown PALcode variant": there is one PALcode **[decision, N24]** |
| wripir | 0D | IPIs | target n: write 1 << (n & 3) to R_INT_REQ of PID n >> 2, a physical HW_ST to `ipe8(n >> 2) \| 0x7FFC << 20 \| 0x70` (N22) |
| rdmces, wrmces | 10, 11 | machine-check summary | per TPU, in the impure area |
| wrfen | 2B | lazy FP | I_PCTX<FP_ENABLE> (index 0x94, FP_ENABLE only) and the PCB's FEN |
| wrvptptr | 2D | the virtual page table's base | I_CTL and M_CTL VPTE_BASE (indexes 0x81 and 0x01, VPTE_BASE only) |
| swpctx | 30 | context switch | save KSP, USP, the cycle count and UNIQUE in the old PCB; load the new: PTBR into the impure area, ASN into I_PCTX and M_PCTX (0x91, 0x11), FEN, CC<OFFSET> (0xB9) |
| wrval, rdval | 31, 32 | the per-CPU value | per TPU, impure area |
| tbi | 33 | TB invalidation | -2 tbia: ITB_IM and DTB_IM mode 01 (this group); -1 tbiap: mode 05 (non-ASM); 1, 2, 3: ITB_IS, DTB_IS, both; an IFETCHB after (Restriction 1) |
| wrent | 34 | entInt, entArith, entMM, entIF, entUna, entSys | six addresses in the impure area [ARM p. 493] |
| swpipl, rdps | 35, 36 | IPL | PS<IPL> through the IPL table (below) into IER_CM's enables |
| wrkgp, wrusp, rdusp | 37, 38, 3A | | impure area; USP in the PCB |
| wrperfmon | 39 | performance counters | returns 0: not modelled |
| whami | 3C | `hard_smp_processor_id()` | PID x 4 + TPU, from CPU_BASE and TPU_CNFG (N25) |
| retsys, rti | 3D, 3F | return to user or kernel | pop the frame [ARM pp. 527-528], set the mode and IPL, `RET` with Rb<1:0> = 00 |
| wtint | 3E | idle, if `CONFIG_ALPHA_WTINT` | a QUIESCE on the TPU's own wake word with the timer set to the requested ticks: the idle TPU gives its issue slots to the others, which is what QUIESCE is for [IDS p. 46] **[decision, N26]** |
| bpt, bugchk, gentrap | 80, 81, AA | | entIF with the type [ARM pp. 544-548] |
| callsys | 83 | system calls | entSys; from kernel mode, a bugcheck |
| imb | 86 | instruction-stream barrier | IC_FLUSH (0xCC) and IFETCHB |
| rdunique, wrunique | 9E, 9F | thread pointer | the PCB's UNIQUE |
| clrfen | AE | | FP_ENABLE off |

**The entry arguments** are the Tru64 UNIX ones [ARM p. 528]: entArith
(exception summary, register mask), entIF (type), entInt (type, vector,
parameter), entMM (VA, MMCSR, cause), entSys, entUna (VA, opcode,
register). **The IPLs** [ARM p. 526]: 0-2 software, 3 and 4 device, 5
clock and interprocessor, 6 real-time devices and correctable errors, 7
machine checks. **[decision]** (part of N22): the IPL table enables, per
TPU, EI1 (the queue) and EI4 (halt, GIO) below IPL 4, EI2 (timer) and EI3
(IPI) below IPL 5, EI0 below IPL 6, EI5 below IPL 7.

### 3.2 How each 21464 entry reaches the OS

| 21464 entry | The PALcode's work |
| --- | --- |
| 0xB00 RESET | TPU 0 of the boot chip: set up its impure area (its PA in PAL_TEMP1 [IDS p. 500]), I_CTL and M_CTL (8 KB pages, 43-bit VA, SPE43 and SPE52), TPU_GRP = TPU_ID (N25), R_INT_MASK[t] (N22), then jump to the console in kernel mode with the CONSOLE bit clear. Every other TPU: its own impure area, then park (N17) |
| 0x680 INTERRUPT | read ISUM: EI2 and this TPU's timer bit, entInt type 1; EI3 and this TPU's IPI bit, type 0; EI1, take the IID at R_INT_QUE's head, remove it, and pass the vector Linux decodes, `PE << 16 \| 0x800 + (LSI << 4)` or `0x1000 + (MSI << 4)` (`io7_device_interrupt`, `sys_marvel.c`), type 3; EI4, halt or the GIO terminal; each source cleared in R_INT_STAT before the OS runs |
| 0x300 DTBM_SINGLE, 0x800 _CONS | `HW_LD/PTE` of VA_FORM; if valid, the IDS's writer block and `RET` through DTBMS_RET_ADDR in S1; else entMM [IDS pp. 576-578] |
| 0x100, 0x180 DTBM_DOUBLE(_ALT) | walk the three-level table from PTBR physically, fill the VPTE's translation, return to retry |
| 0x580 ITB_MISS, 0x880 _CONS | `HW_LD/PTE` of IVA_FORM, ITB_TAG and ITB_PTE, IFETCHB, `RET` through EXC_ADDR; else entMM or entIF |
| 0x380 DFAULT, 0x480 IACV, 0x900 BAD_JUMP_IVA | entMM with VA (VA's va' adjusted back if big-endian, [IDS p. 512]), MMCSR from M_STAT, cause |
| 0x280 UNALIGN | entUna with VA, the opcode and register from EXC_SUM |
| 0x400 OPCDEC, 0x200 FEN | entIF, types 4 and 3 (an OPCDEC with VEN off is the OS's to recognise, [ev8-isa.md](ev8-isa.md), section 5) |
| 0x600 ARITH | entArith with EXC_SUM's bits and the register mask |
| 0x700 MT_FPCR | `HW_MFPR EXC_ADDR -> S1; IFETCHB; RET S1` [IDS p. 581] |
| 0x500 MCHK, 0x780 IMCHK, 0xB80 DST_NXM | not raised; halt the TPU with a message if ever entered |
| 0x980, 0xA00, 0xA80 | not raised (N1, N11) |

### 3.3 The HWRPB fields it owns, and per-TPU state

The PALcode owns, in each processor's per-CPU slot [ARM pp. 630-639]:
the HWPCB (KSP, USP, PTBR, cycle counter, ASN, UNIQUE, FEN), the PALcode
revision and variant, the sizes and PAs of the PALcode's memory and
scratch area, and the halt fields (PCBB, PC, PS, reason) written on a
halt; with the console it maintains the slot's state flags (present,
available, PALcode loaded and valid, which `smp.c` counts as 0x1cc). The
rest of the HWRPB is the console's ([section 4](#4-the-minimal-console-s12)).

Per TPU, in an impure area of its own: the six entry addresses, KGP,
PTBR, PCBB, MCES, the per-CPU value, the scratch the flows save into
(only two shadow registers, so the flows spill more than the 21264's
do), and the IPL. PAL_TEMP1 holds the area's PA, PAL_TEMP2 is free.

### 3.4 SMP: WHAMI, IPIs, per-TPU state

- **WHAMI** **[decision, N25]**: PID x 4 + TPU, the original D6, which
  the owner called natural for the native track (2026-10-25). With dense
  PIDs (N19) the numbers are dense too: an ES47's two chips are CPUs 0-7,
  an eight-chip ES80 0-31. The HWRPB has 4 x (highest PID + 1) slots,
  indexed by WHAMI, because Linux indexes them by `hard_smp_processor_id()`
  (`smp.c`).
- **TPU groups** **[decision]** (part of N25): every TPU in a group of
  its own (Mode 2), TPU_GRP = TPU_ID. Linux allocates ASNs per CPU
  (`mmu_context.h`, `cpu_last_asn`): two TPUs of one group (Mode 3) could
  hand the same ASN to two different address spaces and hit each other's
  TB entries **[inference]**. The IDS expected Unix to use Mode 3
  [IDS p. 306]; Tru64's ASN allocation is not known, and Linux's needs
  Mode 2.
- **IPIs**: wripir as above; the receiving TPU sees its own bit only
  (N22). Linux's IPI path (`handle_ipi`) is per CPU, so nothing else is
  needed.
- **Starting a secondary**: Linux writes the target slot's HWPCB, sets
  `CPU_restart` to `__smp_callin`, puts "START" in the slot's IPC buffer
  and sets the target's bit in `hwrpb->rxrdy` (`smp.c`,
  `send_secondary_console_msg`). A parked TPU (N17) watches exactly that
  quadword with LDQ_ARM and sleeps in QUIESCE; Linux's `set_bit` is a
  store to the watched line, which wakes it; it finds its bit, and
  starts at `CPU_restart` with the slot's HWPCB. qemu-palcode polls the
  same word every millisecond (`init.c`); on the 21464 the wait is the
  IDS's own mechanism.

### 3.5 How it is assembled

The 21464's PALcode forms are known to no toolchain: binutils has no EV8
([ev8.md](ev8.md), "What the software trees say"), and nada deliberately
leaves out every PALcode-only instruction (nada `doc/asm-spec.md` 6.6).
Two ways:

- **`test/cpu/alpha_asm.py`**, the Python assembler the oracle image and
  `quiesce_mp.py` build their PALcode with, extended with the 21464's
  forms: `hw_mfpr8(rc, ipr, rclass)`, `hw_mtpr8(rb, ipr, rclass, wclass)`,
  `hw_ld8`/`hw_st8(ra, rb, disp, type, length)`, `ifetchb()`, `ret` with a
  mode, and the IPR table of section 1.4. Enough for S11's tests and the
  boot images of section 6 at once; awkward for a 2000-line PALcode.
- **nada's assembler**, if nada takes the forms. **The proposal for the
  coordinator to relay to the nada session** (none of it is in
  [ev8-isa.md](ev8-isa.md), which covers user instructions only):
  - a switch that admits the PALcode forms only with `-mcpu=ev8` (as
    `-mtarantula` admits vectors), for example `-mpal`;
  - `hw_mfpr $rc, IPR[, rclass]`: 0x19, Rc <4:0>, index <12:5>, Rclass
    <24:21> (4 bits: valid and class) [IDS p. 571];
  - `hw_mtpr $rb, IPR[, rclass[, wclass]]`: 0x1D, Rb <20:16>, index
    <12:5>, Rclass <24:21>, Wclass <4:0> (bubble, valid, class) [IDS
    p. 572];
  - `hw_ld{b,w,l,q}` and `hw_st{b,w,l,q}` with a type qualifier, `/p`
    (000), `/pte` (010, loads only), `/v` (100), `/wchk` (101, loads
    only), `/a` (110), `/wchk/a` (111), and an 11-bit signed
    displacement [IDS pp. 569-570];
  - `ifetchb`: 0x1E, the other bits zero [IDS p. 650];
  - the IPR names of section 1.4 as symbols, with the field-select
    variants as names of their own (for example `i_ctl_vpte` = 0x81,
    `i_pctx_asn` = 0x91, `cc_offset` = 0xB9) and the Alphabox indexes of
    N6 to N8 marked as such;
  - `$s0` and `$s1`, resolved by position: 24 and 25 as Ra or the
    destination, 22 and 23 as Rb [IDS p. 580]; an error when a CMOV
    names one as destination (Restrictions 12 and 13);
  - nothing for `ret`, which is the ordinary instruction; nothing of the
    21264's PALcode forms.
  Optional: warnings for Restrictions 3 to 8 (the DTB writer block in one
  fetch block, the ALIGN_NOP in slot 7), which an emulator does not
  enforce but a hardware-faithful PALcode would keep.

**Agreed with nada, 2026-10-25** (nada accepted the forms for its first
batch after its item 3; Artur on both sides):

- **`-mpal` means the file is PALmode code**: it admits the forms and
  `$s0`/`$s1`, and it applies section 1.3's replacement table. A plain
  `$24` or `$25` as Ra or destination, or `$22` or `$23` as Rb, is
  refused with a message naming the shadow register it would be; the
  source writes `$s0`/`$s1` there. Plain `$24`/`$25` as Rb and `$22`/`$23`
  as Ra or destination are the architectural registers and are accepted.
  The one exception is a legacy CMOV, whose destination `$24`/`$25` is
  the architectural register. Outside `-mpal`, `$s0`/`$s1` are errors.
- **CMOV**: no CMOV may name `$s0`/`$s1` as destination (Restriction 12).
  A native CMOV (one followed by its `cmov2`) may not have Rc 24 or 25
  (Restriction 13), nor 22 or 23 (section 1.3, **[inference]**). A legacy
  CMOV may write any of the four architectural registers.
- **IPR names** live in a namespace of their own, valid only in the IPR
  operand of `hw_mfpr`/`hw_mtpr`, so a label of the same name does not
  clash; a bare number 0-255 is accepted there too. The list is below.
- **The macro** `__nada_alpha_pal__` is defined under `-mpal`.
- **Restrictions 3 to 8**: no warnings in the first version; S12a does not
  need them.

**The IPR names** (for assemblers). The names are Alphabox's: the IDS has
none for assemblers. A field-select variant is a name of its own. Combined
selects such as I_PCTX's 0x93 (ASN and TPU_GRP) and the performance
registers (0x60-0x71, 0xE0-0xF7) are written as numbers in the first
version. Indexes marked N6 to N8 are Alphabox decisions, not IDS facts.

| Name | Index | Name | Index | Name | Index |
| --- | --- | --- | --- | --- | --- |
| `i_ctl` | 0x83 | `pal_temp1` | 0xA9 | `m_ctl` | 0x03 |
| `i_ctl_vpte` | 0x81 | `pal_temp2` | 0xAA | `m_ctl_vpte` | 0x01 |
| `i_ctl_spe` | 0x82 | `cpu_base` (N6) | 0xAB | `m_ctl_spe` | 0x02 |
| `i_mode` | 0x8B | `vctl` (N8) | 0xAC | `m_mode` | 0x0F |
| `i_mode_cur` | 0x89 | `ier_cm` (N7) | 0xB0 | `m_mode_cur` | 0x09 |
| `i_mode_con` | 0x8A | `sirr` (N7) | 0xB1 | `m_mode_con` | 0x0A |
| `i_pctx` | 0x97 | `isum` (N7) | 0xB2 | `m_mode_alt` | 0x0C |
| `i_pctx_asn` | 0x91 | `hw_int_clr` (N7) | 0xB3 | `m_pctx` | 0x13 |
| `i_pctx_grp` | 0x92 | `cc` | 0xBB | `m_pctx_asn` | 0x11 |
| `i_pctx_fpe` | 0x94 | `cc_offset` | 0xB9 | `m_pctx_grp` | 0x12 |
| `exc_sum` | 0xA0 | `cc_count` | 0xBA | `m_stat` | 0x20 |
| `exc_addr` | 0xA1 | `itb_im` | 0xC0 | `va` | 0x21 |
| `iva_form` | 0xA3 | `itb_is` | 0xC2 | `va_form` | 0x23 |
| `dtbms_ret_addr` | 0xA6 | `itb_pte` | 0xC4 | `quiesce_timeout` | 0x38 |
| `dtbms_ret_addr_blk` | 0xA7 | `itb_tag` | 0xC6 | `dtb_im` | 0x40 |
| `pal_base` | 0xA8 | `cpu_cnfg` | 0xC8 | `dtb_is` | 0x42 |
| | | `ic_stat` | 0xC9 | `dtb_pte0` | 0x44 |
| | | `ic_flush` | 0xCC | `dtb_pte1` | 0x45 |
| | | `ic_flush_asm` | 0xCD | `dtb_tag0` | 0x46 |
| | | `tpu_cnfg` | 0xD8 | `dtb_tag1` | 0x47 |
| | | | | `dc_ctl` | 0x48 |
| | | | | `dc_stat` | 0x49 |

`_vpte`, `_spe`, `_cur`, `_con`, `_alt`, `_asn`, `_grp`, `_fpe`,
`_offset` and `_count` are the selects of section 1.4's tables (VPTE_BASE;
SUPERPAGE with VA_SIZE and PAGE_SIZE; CURRENT; CONSOLE; ALT; ASN; TPU_GRP;
FP_ENABLE; OFFSET and ENABLE; COUNT). `dtbms_ret_addr_blk` is 0xA7, the
form that also blocks loads and stores.

**[decision, N27]**: the PALcode is written in nada assembler source once
nada has the forms; until then S11's tests and S12's first boot images
use `alpha_asm.py`. The console is C (section 4), compiled by nada for
`alpha-none` or by the cross GCC of S13.

### 3.6 Built and loaded

- **The loader exists**: `CSystem::load_decompressed_console` reads a
  file of the entry PC, PAL_BASE, the board's console bytes (0x480000 on
  the ES47) placed at PA 0, and the tag `ALPHDC01`, and hands every
  processor to it (`hand_to_console`); the oracle image and
  `quiesce_mp.py` boot that way (`test/cpu/image.py`, `rom.decompressed`).
  An `ev8n` machine uses the same path, with the EV8 reset in place of
  the XSROM handoff: every TPU of every chip at PAL_BASE + 0xB00 in
  PALmode, CPU_BASE and TPU_CNFG set, the IPRs at their reset values
  ([section 2.7](#27-what-changes-in-the-code)).
- **The image** **[decision, N28]**: PAL_BASE 0; the entries at
  0x0000-0x0F80 and the CALL_PAL slots at 0x2000-0x3FC0; the impure areas
  after them; the console's code and data, the HWRPB, the GCT and the
  initial page tables below 2 MB, which the HWRPB's first memory cluster
  marks as the console's (usage 1, as qemu-palcode does, `init.c`).
- **The sources** **[decision]** (part of N28): a new top-level
  `firmware/ev8/` (PALcode and console), built by a script into a
  `rom.decompressed` file in `lab/`; no binary committed, nothing added
  to the CMake build (it needs no Alpha toolchain). It is Alphabox's own
  firmware under the project's licence; "firmware images are never
  downloaded" is untouched.

### 3.7 How big

| PALcode | Size | Source |
| --- | --- | --- |
| The Marvel console's UNIX PALcode V2.08-19 | 45 KB, 11712 instructions, 7513 reached from the entries | [marvel.md](marvel.md), E1 |
| Its OpenVMS PALcode V2.11-25 | 64 KB, 16384 instructions, 11168 reached | the same |
| qemu-palcode's `pal.S` | 1950 lines, for QEMU's own idealised processor (no TB fill flows: QEMU walks the page tables itself) | `lab/datasheets/ev8-native/qemu-palcode/` |
| The oracle image's PALcode | a few hundred instructions (`image.py`, `build_pal`) | `test/cpu/image.py` |
| **Ours** | **2000 to 3000 instructions** (8 to 12 KB) **[guess]**: the EV7's UNIX PALcode less its machine-check, error-logging and console-service code, plus the 21464's TB fill flows | |

## 4. The minimal console (S12)

### 4.1 What Linux needs from a console on Marvel

From the kernel's own code (`setup.c`, `smp.c`, `core_marvel.c`,
`sys_marvel.c`, `srmcons.c`, `head.S`, `setup.h`) and the ARM's console
chapter [ARM pp. 617-660]:

| Need | Detail | Reference |
| --- | --- | --- |
| The HWRPB, mapped where the kernel finds it | `INIT_HWRPB` (virtual 0x10000000) through the console's page tables, `phys_addr`, revision 5 or more (the DSR), SSN not starting "MILO" (`alpha_using_srm`), `sys_type` 39 (ST_DEC_MARVEL, the one Marvel vector), `max_asn` 255 (the 21464's 8 bits [IDS p. 43] and the Marvel vector's `MAX_ASN`, `EV6_MAX_ASN`, which `setup.c` checks), page size 8192, `pa_bits` 48, `cycle_freq`, `intr_freq` = 4096 x the R_INTER_TIM rate, `vptb` | `setup.c`; [ARM pp. 617-629] |
| Per-CPU slots | 4 x (highest PID + 1), indexed by WHAMI (N25); present ones with flags 0x1cc, type 18 (D3), the PALcode fields (section 3.3) | `smp.c`; [ARM pp. 630-639] |
| Memory descriptors | one cluster for the console and PALcode (usage 1), one or more for the OS (usage 0), per PID at the 21464-mode placement (N18) | `setup.c` (`mddt_offset`) |
| The GCT | at `frut_offset`: a header and one node per IO7, type 0x0E (hose), subtype 0x2C (I/O port module), `id` = PID << 8, magic 'GLXY', linked through `next` and `child` handles. Without it, `io7=` on the command line names the IO7 PIDs; a missing GCT is not an option, since `GCT_NODE_PTR(0)` reads whatever is at `frut_offset` | `gct.h`, `gct.c`, `core_marvel.c` |
| The CTB | the console terminal block: serial, so that `marvel_find_console_vga_hose` (TERM_TYPE 3 = graphics) leaves the VGA alone | `core_marvel.c`; [ARM pp. 625-626] |
| Console callbacks (the CRB) | GETC and PUTS for `srmcons` (`console=srm0`: Marvel has no 16550, [linux.md](../linux.md)), GET_ENV for the boot variables, and the FIXUP routine, which Linux calls to move the console's mapping into its own address space | `srmcons.c`, `console.h`; [ARM pp. 645-660] |
| PCI configured | `marvel_init_pci` sets PCI_PROBE_ONLY ("we trust the console to configure things properly") and `marvel_map_irq` takes each device's interrupt from INTERRUPT_LINE, which SRM sets to the LSI number | `sys_marvel.c` |
| The TOY | through `cserve` 0x48 and 0x49 (section 3.1) | `rtc.c`, `core_marvel.c` |
| Secondaries | parked until "START" (section 3.4) | `smp.c` |
| The command line and initrd | in the zero page: `COMMAND_LINE` at ZERO_PGE, `INITRD_START` and `INITRD_SIZE` at +0x100 and +0x108, ZERO_PGE = PAGE_OFFSET + KERNEL_START_PHYS + 0xA000; KERNEL_START_PHYS 0x1000000, "required: Wildfire/Titan/Marvel"; if the zero page is empty, `callback_getenv` | `setup.h`, `setup.c` |

### 4.2 Starting a kernel: two ways

**A. SRM callbacks enough for aboot.** The console boots like SRM:
`boot dqa0` reads the disk's boot block, loads aboot at 0x20000000, and
aboot reads the kernel and initrd through the CRB's OPEN, READ and CLOSE
on the console's device names, with BOOTED_DEV and BOOTED_OSFLAGS from
GET_ENV. Costs: disk drivers in the console (the CMD 649's ATA, or the
53C895's SCRIPTS, or both), the SRM device naming aboot parses, the boot
command and its environment, aboot's assumptions about the console's
memory, and every callback's error paths; 2 to 4 k lines more than B
**[guess]**. What it buys: the installed disks boot as on the EV7 rows,
unchanged.

**B. Load a kernel directly.** The emulator places the kernel (an ELF
`vmlinux`, at the physical addresses of its program headers) and an
optional initrd in memory, and the console writes the command line and
the initrd's place into the zero page, builds the HWRPB, GCT and page
tables, and enters the kernel at its entry in kernel mode, IPL 7, with
the boot PCB (KSP, PTBR) in place, as qemu-palcode does for QEMU's
`-kernel` (`init.c`, `do_start`). No disk driver, no aboot, no boot
command: GETC, PUTS, GET_ENV and FIXUP are the whole CRB. The root file
system is the installed disk, through the kernel's own drivers
(`root=/dev/sda2`). Costs: about 200 lines in the emulator (an ELF
reader and the keys `kernel`, `initrd`, `cmdline` on an `ev8n`
machine) and a console of 1.5 to 2.5 k lines of C **[guess]**: qemu-palcode's
`init.c`, `crb.c` and `console.c` together are 790 lines, for a machine
with no GCT, no PCI to configure and one CPU type.

**[decision, N29]**: **B**, with A as a later option if the owner wants
the boot command. Everything the native track exists to show (the
interface, the TPUs, the instructions under a real OS) needs only B.

**PCI** **[decision, N30]**: the Alphabox kernel clears PCI_PROBE_ONLY
and computes each device's LSI in `marvel_map_irq` from the IO7's rule
(port, slot and pin, the same `marvel_pci_interrupt` rule the board row
gives the IO7 model), so that the console configures no PCI device:
the kernel assigns BARs and bridge windows, as it does on the Tsunami
and the Titan. The alternative, PCI enumeration and resource assignment
in the console (the SRM's job), is 400 to 800 lines of C **[guess]** for
the same result.

**The console's other duties**, all small: a prompt that prints what it
built and waits for a key or boots at once (`auto_action`); `halt`
returning to it (Linux's `poweroff`); the CMM's terminal through GIO
registers 4 and 5 and the TOY through the byte window, both of which the
CMM model already answers.

## 5. Linux on the native EV8 (S13)

### 5.1 What the kernel must change

Measured against the Marvel sources (`lab/docs-ev7/linux-*`, mainline):

| File | Change | Why |
| --- | --- | --- |
| `include/asm/core_marvel.h` | `EV7_PE_MASK` 0x1ff to 0x1fff (or a variable set at boot from the CPU type) | `EV7_IPE` must produce the 21464-mode `ipe8` (N18); `EV7_MASK40` and `EV7_KERN_ADDR` stay: SPE43 maps them back to the same PA |
| `kernel/sys_marvel.c` | the PE of a CPU is `cpu >> 2` where the code uses `hard_smp_processor_id()` or `boot_cpuid` as a PE (`marvel_smp_callin`, `marvel_init_irq`: IO7 redirection, `init_one_io7_lsi`, the "Interrupts reported to CPU at PE" message); only TPU 0 of a chip redirects its IO7 | WHAMI = PID x 4 + TPU (N25) |
| `kernel/sys_marvel.c` | no PCI_PROBE_ONLY; `marvel_map_irq` computes the LSI | N30 |
| `kernel/setup.c` | "EV8" in `cpu_names` for type 18, and its caches in `determine_cpu_caches`: L1 64 KB 2-way each, L2 3 MB 6-way [IDS pp. 42-43] | D3; otherwise "Unknown" and no cache shape, harmless |
| `Kconfig` | NR_CPUS above 32 (64, the HWRPB's `rxrdy` and `txrdy` bitmasks being 64 bits) for a sixteen-chip GS1280 | "The kernel supports at most 32 CPUs" (Kconfig); 8 chips x 4 TPUs fit as it is |
| `kernel/core_marvel.c` | nothing for PIDs below 256 (the GCT's `(id >> 8) & 0xff`), nor the memory placement (N18) | |
| `kernel/time.c`, `rtc.c`, `smp.c`, `irq_alpha.c` | nothing: the timer arrives per CPU as entInt type 1, IPIs as type 0, the TOY through cserve | sections 2.4, 3.1 |
| `arch/alpha/mm` | nothing: 43-bit VA and 8 KB pages are a 21464 mode [IDS p. 690]; `MAX_ASN` 255 is its ASN width | |

About fifty changed lines **[guess]**, and a configuration: the MARVEL
machine vector only (or GENERIC), SMP, `CONFIG_SRM_CONSOLE` (srmcons),
the CMD 649 and USB drivers the ES47 has, and for S9 later the vector
context switch ([ev8-isa.md](ev8-isa.md), section 5). The new
instructions under the native interface (the S3, S5 and S6 programs)
need no kernel change beyond what the `ev8` row already runs: the
`AT_HWCAP` AMASK is the row's.

### 5.2 How Alphabox builds a kernel

The same need comes from three places: S13, Tarantula's OS stage (S9:
Linux enabling vectors and saving them on a switch,
[ev8.md](ev8.md)), and "a kernel built for the machine" in
[linux.md](../linux.md), "Open". So it is one tool, set up once.

| Where | For | Against |
| --- | --- | --- |
| **A Linux container** on the macOS host (Docker, Debian's `gcc-alpha-linux-gnu` cross compiler) | fast; a stock toolchain; the kernel tree is a release tarball | about 1.5 GB of kernel tree and 1 GB of build on a host with 9 GB free **[guess]**; Docker is installed on the host (`/usr/local/bin/docker`). The container mounts a `git archive` tarball of what it needs (the patch series), never the repository |
| **In the guest** (the ES40 or ES47 install has gcc, [ev8.md](ev8.md), S3) | no host toolchain; native | hours per build at emulated speed **[guess]**; the guest disk must hold the tree |
| A cross toolchain built on macOS (crosstool-ng) | no container | an evening of building binutils and gcc for a target nobody packages for macOS |

**[decision, N31]**: the container, with the patch series and the
configuration in the repository (beside the firmware, `firmware/linux/`
or under `test/`), the kernel tarball and the build in `lab/`, deleted
after a build if disk is short; the guest as the fallback when Docker is
absent. The kernel version is the one the CD and the installed disks
run (7.0.1), so that its modules on the installed disk still load.

## 6. Tests per stage

Every stage keeps the gates every packet keeps: all lanes build, the
ES40 console-log check clean, JIT_VERIFY 0, `boards_check.sh` and the
existing rows' guests unchanged.

| Stage | Tests, written from the sources before the code |
| --- | --- |
| S11, the native interface | `test/cpu` cases on the `ev8n` row (`--cpu ev8n`, a model in `test/cpu/rows.py`): per IPR, every index's read and write and its field-select low bits (I_CTL, I_MODE, I_PCTX, M_CTL, M_MODE, M_PCTX, CC), read-only and write-1-to-clear fields, the reset values of Table 16-1; HW_LD and HW_ST of each type and length, displacement sign, forced alignment; HW_MFPR's Rc; RET's mode transitions (Table 17-5), EXC_ADDR and DTBMS_RET_ADDR mode bits; the shadow-register table of p. 580 in every position; each entry: the offset, the IPRs it writes (Table 22-3), the restart PC (Table 22-2); CALL_PAL's target and S1; superpages and the sign check in Appendix C's order; TB groups (a fill in group 1 not seen in group 0; each ITB_IM and DTB_IM mode); 64 KB pages and their GH; OPCDEC for the PALcode forms outside PALmode and for HW_LD types 001 and 011. The image's own PALcode must be written for the 21464: `image.py` gets an EV8 PALcode beside its 21264 one |
| The Marvel model in 21464 mode | a boot image (as `quiesce_mp.py` builds its own) on the ES47 in 21464 mode: CSR window decode at the new `ipe8`, R_WHOAMI's coordinates and CPU_BASE's PID, R_INT_STAT and the per-TPU masks (an IPI to each TPU wakes only it), the queue's four entries and the IO7's resend, the per-TPU timer ticks, GIO at 32 bits with the CMM's terminal echoing; `boards_check.sh` and the EV7 rows unchanged (the mode is the family's) |
| S12, the PALcode | boot images that enter every PALcode entry from a test program and check what the OS entry receives (the registers of [ARM p. 528]); every CALL_PAL of section 3.1 against its ARM description; TB fills under a hand-built page table; the parked TPUs started through `rxrdy` and a QUIESCE that sleeps |
| S12, the console | the prompt on the CMM terminal; the HWRPB, slots, memory clusters, GCT and CRB checked field by field by a test program that walks them as Linux does (`setup.c`, `gct.c`); GETC and PUTS through the CRB after a FIXUP |
| S13, Linux | the Alphabox kernel to a shell on one chip, then all TPUs of two chips ("SMP: 8 CPUs probed"), `/proc/cpuinfo` (EV8, type 18), the timer on every CPU, IPIs (`taskset` across TPUs), the CMD 649 disk, `poweroff` back to the console; the S3 lock program and later the S5 and S6 programs in native mode; `guests_check.sh` grows a `linux-ev8n` check |

## 7. Decisions for the owner

Each is a gap, a conflict or a choice the native track needs. The
owner accepted all of them as proposed on 2026-10-25, N22's revision of
D5 for the native track included (D5 stands for EV7-compatible mode); a
stage that changes one says so. N8's PALcode call (the OS's way to set
VCTL) gets its function code when S12a starts.

| # | Area | What is missing or in conflict | Proposal | Why |
| --- | --- | --- | --- | --- |
| N1 | SuperPALmode, PNMI | PNMI's source, entry and the mode's use; the text and Table 17-5 disagree [IDS pp. 573-574] | model the mode encodings, raise no PNMI (no PALmode timeout, no TPU restart) | nothing Alphabox runs needs them; the table is the more specific |
| N2 | Jumps in PALmode | only RET's mode bits are described [IDS p. 573] | every jump takes the mode from Rb<1:0> in PALmode; BSR/JSR write the next PC with its mode | as the 21264 keeps PC<0>; one rule for all jumps |
| N3 | HW_MFPR's destination | Ra in the text, Rc <4:0> in the figure, table and decode appendix [IDS pp. 571, 650] | Rc <4:0> | three places against one |
| N4 | HW_LD/HW_ST types 001, 011 | unassigned on the 21464 [IDS p. 570] | OPCDEC | a 21264 PALcode fails visibly; atomics go through LDx_L/STx_C |
| N5 | FP shadow registers | "R24/F24 and R25/F25" once [IDS p. 574] | integer only | no other mention, p. 580's table is integer |
| N6 | CPU_BASE | shares 0xAA with PAL_TEMP2, no section [IDS p. 478] | 0xAB, the chip's PID <9:0>, read-only, set at reset; PAL_BASE<51:48> MBZ | nothing else tells a chip its PID |
| N7 | IER_CM, ISUM, SIRR, HW_INT_CLR | no index, three with no layout [IDS pp. 479, 513] | 0xB0-0xB3, the 21264's layouts without ASTEN; IER_CM<CM> aliases I_MODE and M_MODE CURRENT; MFPR of CC reads it | the IDS keeps the 21264's six levels [p. 465]; a free aligned block |
| N8 | VCTL | an IPR in the native interface, no index | Ibox 0xAC, per TPU, VEN <0>; the OS sets it through PALcode | the owner's 2026-10-24 statement; HW_MTPR is PALcode's |
| N9 | DTB_PTE0/1, DTB_TAG0/1 | two copies, one per bus [IDS pp. 504-506, 575-576] | one entry; the second write of a pair replaces the first | the copies are an implementation detail |
| N10 | An invalid PTE written | the chip hangs [IDS pp. 505, 578] | dropped and traced | an emulator never hangs |
| N11 | Entries 0x980, 0xA80, 0xB00, 0xB80 | two tables disagree [IDS pp. 499, 633-634] | chapter 22's names; power-up at 0xB00; DST_NXM not raised | the later, fuller table |
| N12 | Double-miss selector, bad-VA status | M_CTL<22> or I_CTL/M_CTL DOUBLE_MISS_CONTROL [pp. 507, 688]; no BAD_VA bit [p. 510] | M_CTL<22>; bad VA as ACV | chapter 16 where Appendix C only moves a bit |
| N13 | Invalid VA_SIZE/PAGE_SIZE combinations | figure labels against Table C-3 [IDS pp. 490, 497, 690] | treated as 43-bit VA, 8 KB pages | no error is specified |
| N14 | Appendix C | a proposal, "upon approval" [IDS p. 685] | followed where chapter 16 is silent: the superpage forms and the sign-check order; superpage enables per TPU | chapter 16 already points to it; SPE52 with 43-bit VAs is what reaches the 48-bit PA |
| N15 | Big-endian | M_CTL<BIG_ENDIAN> | kept, not implemented | no big-endian guest |
| N16 | The `ev8n` row | a family of its own | `CPU_FAMILY_EV8`, type 18, chip ID 0, the `ev8` row's instructions and AMASK, four TPUs, its own TB lookup; the 21464's PALcode forms interpreted at first, compiled through helpers later | [ev8.md](ev8.md), S1 and S2; D3 |
| N17 | TPU start and QUIESCE's reset | WAKEUP and TPU_RESTART with no requester; the boot document lost; enables reset 0 [IDS pp. 499, 511, 628] | all TPUs enter 0xB00; PALcode parks TPUs 1-3 with LDQ_ARM and QUIESCE, timer off; QUIESCE_TIMEOUT as the register says | "Suspending TPUs" with an infinite wait [p. 511]; no invented register |
| N18 | The 48-bit address map | the physical-addressing chapter is empty [IDS p. 645] | the EV7's map with the inverted PID in PA<47:35> (`ipe8`), the EV7's memory placement, 48-bit mask | Linux's KSEG arithmetic and Alphabox's decode keep working for PIDs 0-31 |
| N19 | PIDs and coordinates | 10-bit node address as coordinates [p. 527] against 256-node packets and route tables [pp. 458, 526] | dense PIDs (the topology's processor index), coordinates in R_WHOAMI, PID in CPU_BASE, R_ROUT in the IDS's layout | dense WHAMI numbers and SPE43's reach; `coord2id` is the EV7 console's rule, and no EV7 console runs here |
| N20 | Rbox and Zbox register offsets | no index, no offset [IDS pp. 479-481] | the EV7 window's offsets; new registers at free offsets | the IDS's prose uses the 21364 names; RdIPR/WrIPR reach them as I/O |
| N21 | Interrupt queue depth | four [p. 464] against two [p. 528] | four; a full queue refuses and the IO7 resends | the design section; four TPUs |
| N22 | Interrupt steering, IPIs, the timer, GIO's TPU mask | R_INT_* "TBD" [pp. 527-528]; "a mask register" for GIO [p. 476] | one R_INT_STAT per chip with per-TPU IPI, timer and halt bits; a mask per TPU (the GIO mask too); IO7 targets name a PID; one timer, a tick per TPU; the IPL table of section 3.1; a GIO lock word in memory | revises D5's IO7 part: the IO7's target field holds a PID only, the queue is per chip |
| N23 | GIO width | 32 bits [IDS p. 476] | GIO_DATA carries 32 bits in 21464 mode | the CMM's registers fit |
| N24 | SWPPAL | one PALcode | refuse (status 1) | nothing to swap to |
| N25 | WHAMI, groups | PALcode's choice | PID x 4 + TPU; HWRPB slots by WHAMI; each TPU its own group (Mode 2) | the owner's D6 for the native track; Linux's per-CPU ASNs need private groups |
| N26 | wtint | | QUIESCE with the timer | the idle TPU frees its issue slots |
| N27 | Assembler | no toolchain has the forms | nada with the forms of section 3.5 (proposal relayed); `alpha_asm.py` until then; the console in C | nada already targets bare Alpha |
| N28 | Image, sources | | `rom.decompressed` path, PAL_BASE 0, all below 2 MB; sources in `firmware/ev8/`, built by script into `lab/`, no binary committed | the loader exists; no CMake dependency on an Alpha toolchain |
| N29 | How Linux starts | aboot or direct | direct: the emulator loads `vmlinux` and an initrd, the console writes the zero page | no console disk drivers; aboot stays possible later |
| N30 | PCI setup | Linux trusts the console (PCI_PROBE_ONLY) | the Alphabox kernel assigns resources and computes LSIs | cheaper than a PCI allocator in the console |
| N31 | Building kernels | | a Debian cross-compiler container fed a `git archive` tarball; the guest as fallback; patches and configuration in the repository | shared with S9 and Linux phase 2; disk-tight host |

The three that matter most: **N18** (the address map: every later piece,
the console, the PALcode and the Linux changes, is written against it),
**N22** (interrupt steering: the largest invention, and where the
native track departs from D5), and **N17 with N25** (how TPUs start and
are numbered: what makes them four processors to Linux).

## 8. The stages refined

| Stage | Question | Rests on | Testable at its end | Size **[guess]** |
| --- | --- | --- | --- | --- |
| S11, the native interface | Does an `ev8n` row execute the IDS's privileged interface? | S1-S3 (the user instructions), N1-N17 | the section 6 cases in the interpreter, then the JIT (the 21464's PALcode forms interpreted, a block ending before each; JIT_VERIFY 0); no OS yet | 1.5 to 2.5 k lines of core (a `cpu_pal_ev8.hpp` beside the 557-line `cpu_pal.hpp`, the TB with groups and 64 KB pages, per-TPU state, the entries) and 1 k of tests; S3 was about 1.5 k with its tests, M1-M2 1.5 to 3 k |
| S11b, the Marvel model in 21464 mode (the stage the coordinator proposed) | Does the ES47 board run an `ev8n` machine: map, Rbox interrupts per TPU, GIO, IO7? | S11, N18-N23 | a boot image (own PALcode) on the ES47, ES80 and GS1280 layouts in 21464 mode; every EV7 board check unchanged | 600 to 900 lines, plus a 300-line test image; between M10 (250) and M5 (900) |
| S12a, the PALcode | Does a Linux-interface PALcode run on the 21464's registers? | S11b, N24-N28; nada's forms or `alpha_asm.py` | boot images entering every entry and CALL_PAL; TB fills; parked TPUs started | 2000 to 3000 instructions; nada's PALcode forms first, if nada takes them |
| S12b, the console | Does a console build what Linux reads (HWRPB, slots, memory, GCT, CRB) and start a kernel? | S12a, N29, N30 | a test program walking the HWRPB as Linux does; the prompt on the CMM terminal | 1.5 to 2.5 k lines of C, 200 in the emulator (ELF loader, keys) |
| S13, Linux | Does Linux run on the native EV8, SMP across TPUs? | S12b, N31, the kernel changes of section 5 | Linux to a shell on one chip, then two chips' eight TPUs; the S3 test in native mode; `linux-ev8n` in `guests_check.sh` | about 50 kernel lines, a configuration, the container; the most uncertain step is the first boot of a kernel nobody has run on this interface |

**Where the other stages fit.** S4 (TPUs in EV7-compatible mode) comes
first and gives the per-TPU processor state S11 needs (four host
threads per chip, per-TPU interrupt lines). S5 and S6 (the 1C SIMD set
and paired single) are user instructions, shared by both rows: they can
land before, between or after S11 to S13, and their tests run in native
mode once S13 boots. Tarantula's S7 and S8 are user-visible too; S9
(Linux saving vector state) needs the kernel-building of N31, so it
lands after S13's toolchain exists, on either row: on the `ev8` row the
enable is the CSR, on `ev8n` the IPR (N8). S10 (Tarantula in the JIT)
is independent of the native track.

Order: S4, then S11, S11b, S12a, S12b, S13, with S5 and S6 free to
interleave and S9 after S13's toolchain.

## S11 (2026-10-25)

The 21464's own privileged interface on a row of its own, `ev8n`
(`CPU_FAMILY_EV8`, N16), as section 1 describes it, interpreted, with
normal-mode code compiled. No board takes the row yet (that is S11b);
the CPU oracle runs it on the ES40 through `ALPHABOX_CPU_ANY_BOARD`. The
code is `cpu/cpu_pal_ev8.hpp` (the IPRs) and `cpu/AlphaCPU_ev8.cpp`
(modes, jumps, CALL_PAL, HW_LD/HW_ST, the entries, translation, the TB,
the reset); every decision and divergence is in
[cpu-fidelity.md](../cpu-fidelity.md), "The native EV8".

**What is implemented**, section by section:

- **1.1 Modes**: PC<1:0>; RET's transitions of Table 17-5; SuperPALmode
  as PALmode, no PNMI (N1); in PALmode every jump's mode from Rb<1:0> and
  every link, BR and BSR included, with the mode bits (N2). Two
  **[decision]**s beyond N2: Rb<1:0> = 10 is normal mode (no mode has the
  encoding), and a jump into normal mode whose target is outside the
  virtual address space takes BAD_JUMP_IVA at the jump, which does not
  complete, as Table 22-2 has it [IDS p. 629].
- **1.2 The PALcode instructions**: HW_MFPR's Rc (N3), HW_MTPR's Rb, the
  classes ignored; HW_LD and HW_ST with every type and length, the 11-bit
  displacement, forced alignment; 001 and 011 OPCDEC (N4), and the
  load-only types on a store OPCDEC **[decision]**; IFETCHB a no-op; the
  forms OPCDEC outside PALmode; MT_FPCR outside PALmode traps to its entry
  [IDS pp. 581, 630].
- **1.3 Shadow registers**: S0 and S1 by position, integer only (N5), per
  TPU (each TPU is a `CAlphaCPU`; state.r[56] and [57]); a native CMOV in
  PALmode executes as the chip splits it, a legacy CMOV's Rc unreplaced;
  CALL_PAL writes S1, not EXC_ADDR.
- **1.4 The IPRs**: every index of both tables with its selects,
  read-only and write-1-to-clear fields and reset values (zero where the
  IDS gives none, **[decision]**; DC_CTL<SET_EN> 11 **[inference]**);
  CPU_BASE, VCTL, IER_CM/SIRR/ISUM/HW_INT_CLR (N6-N8), DTB_PTE0/1 as one
  entry (N9), an invalid PTE dropped and traced (N10, `ALPHABOX_TRACE_EV8`),
  CC (N7); the performance registers read zero and ignore writes
  **[decision]**: nothing is counted. Any index of a register reads all of
  it **[inference]**; ITB_IS shares ITB_TAG's storage [IDS p. 495].
- **1.5 The entries**: chapter 22's (N11), power-up at 0xB00, the IPRs
  each writes (Table 22-3), DTBMS_RET_ADDR for the single miss, the _CONS
  entries, M_CTL<DBL_ALT> (N12), BAD_JUMP_IVA, CALL_PAL's target with
  PC<1> and the OPCDEC rule.
- **1.6 Memory management**: 52-bit VA and 48-bit PA, 8 KB and 64 KB
  pages, N13's fallback, Appendix C's superpages and sign-check order
  (N14), per-TPU enables, the TB match with a TPU group per entry, ITB_IM
  and DTB_IM by mode, ITB_IS and DTB_IS, IC_FLUSH (the whole I-cache,
  **[inference]**), granularity hints with the 64 KB shift, VA_FORM and
  IVA_FORM in the three formats, BIG_ENDIAN stored (N15). The modes' value
  list (00, 01, 03, 05, 0D, 10, 16) is read as bits **[inference]**, which
  gives the IDS's seven and a meaning to every other value; ITB_IM's "this
  group" is I_PCTX's, where its text says M_PCTX's [IDS p. 494,
  **conflict**] **[inference]**.
- **1.7 TPUs**: TPU_CNFG<TPU_ID>; per-chip CPU_CNFG, DC_CTL and CPU_BASE
  shared by a chip's TPUs, everything else per TPU; every TPU of every
  chip at PAL_BASE + 0xB00 at reset (N17); QUIESCE_TIMEOUT as the
  register says (N17): TIMEOUT cycles with TIMEOUT_EN, no timeout without;
  WATCH_EN clear, no store clears the flag [IDS p. 511]; WAKEUP and
  TPU_RESTART never raised. The `tpus` key defaults to 4 on the row; off
  the Marvel boards each TPU is one of the board's processors (the ES40's
  four).

**The TB-sharing decision** **[decision]**: each TPU keeps a TB of its
own, its entries tagged with the group of the fill, `valid` meaning
present and of the current group, so the lookup (`FindTBEntry`, the data
page cache, the JIT's helpers) is the other rows' and takes no lock. A
TB shared by four host threads would need a lock or cross-thread
invalidation on every lookup and fill, and on the page cache in front of
them. Mode 2 (N25's) is exact; Mode 3 sharing, a fill seen by every TPU
of the group and an "all groups" invalidation reaching the others, is a
divergence, recorded in [cpu-fidelity.md](../cpu-fidelity.md).

**The JIT** (part of N16): PALmode code is never recorded or compiled on
the row (a PALmode start is interpreted like an untranslated one), so the
21464's forms, shadow registers and mode bits are the interpreter's
alone. Normal-mode code compiles as on the `ev8` row but for CALL_PAL,
the jump-format instructions (their BAD_JUMP_IVA check) and MT_FPCR (its
trap), which end a block **[decision]**. The load and store helpers ask
`ev8_superpage` for Appendix C's superpages and sign check, and behind
it find the group's TB entries with their own masks: compiled loads and
stores translate by the 21464's rules, and do not merely bail (a
deliberately wrong SPE result in the helper failed 50 results of the
compiled pass). Traps from compiled code reach the 21464's entries
through the interpreter, which re-executes a bailed instruction.

**The tests**:

- `test/cpu/image.py` has an EV8 PALcode (`build_pal8`) beside the
  21264's, with the same services on the 21464's IPRs, RET, IFETCHB and
  two shadow registers (R1-R5 spilled into the trap record), a reset
  record of the IPRs and the entry, TPUs 1-3 parked; `alpha_asm.py` has
  the forms (`hw_mfpr8`, `hw_mtpr8`, `hw_ld8`, `hw_st8`, `ifetchb`, `ret`)
  and section 3.5's IPR names (`IPR8`). The existing groups run on `ev8n`
  as on `ev8`; those of the 21264's PALcode interface (PALshadow bank,
  SDE, ASTs: six cases, 52 results) are skipped there.
- Group `ev8n` (`test/cpu/ev8n_cases.py`, 524 results): the reset entry
  and 26 IPRs' reset values; every IPR's writes by select and read-back,
  read-only and W1C fields, CM shared by three registers, IER_CM, SIRR
  and ISUM; HW_MFPR's Rc and HW_MTPR's Rb with classes set; HW_LD/HW_ST
  of every length, physical and virtual types, displacements to -0x400,
  forced alignment, Alt with ALT = user (DFAULT), types 001, 011 and the
  load-only types on a store (OPCDEC), the double miss and DBL_ALT; RET
  and JSR through PALmode, SuperPALmode and back, CALL_PAL and OPCDEC in
  SuperPALmode (mode 11 in S1 and EXC_ADDR), RET with 10, a normal-mode
  RET that cannot raise the mode, BAD_JUMP_IVA three ways; S0 and S1 in
  every position, legacy and native CMOVs; CALL_PAL's S1; ARITH, DTBM_SINGLE
  from normal mode and PALmode, the _CONS entries, ITB_MISS's IVA_FORM,
  DFAULT for a bad sign, UNALIGN, MT_FPCR, OPCDEC of the forms, IFETCHB;
  the three superpages, SPE52 disabled, the 21264's SEG48 form and a
  52-bit address failing the sign check; a group-1 fill not seen from
  group 0, the seven DTB_IM and ITB_IM modes over four entries, DTB_IS,
  ITB_IS, invalid PTEs; a 64 KB page, GH 1 on 64 KB (512 KB) and on 8 KB
  (64 KB) pages, VA_FORM with 52-bit VA and with reduced page tables. The
  normal-mode loads and stores run compiled on a JIT lane's last pass.
- `test/cpu/tpu_mp.py`: one chip of four TPUs, each resetting at 0xB00
  with its TPU_ID, TPUs 1-3 parked and released by a store to their
  mailboxes; CPU_CNFG seen alike by all four, VCTL and PAL_TEMP1 each
  TPU's own.

**The proofs**. Every lane built and passed `ctest` (the interpreter,
the AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT), and
so did every commit of the series. The CPU oracle on the interpreter, JIT,
JIT_VERIFY and x86-64 lanes, alike on all four: `ev68cb` and `ev7` PASS
20180, KNOWN 61, FAIL 0; `ev8` PASS 20432, KNOWN 73, FAIL 0 (main's
verdicts and four results more, the harness's register check after the
new group); `ev8n` PASS 20906, KNOWN 70, FAIL 0, SKIP 74 (the 21264
PALcode interface's 52 and the 22 the `ev8` row skips; group `ev8n` is 528
of the results, and the MT_FPCR trap the other rows know as a divergence
passes). main's own oracle (8d6d95b's `test/cpu`) on this branch's
binaries gives main's verdicts exactly: PASS 20176 and KNOWN 61, PASS
20428 and KNOWN 73. JIT_VERIFY: 0 mismatches. `quiesce_mp.py` and
`tpu_mp.py` passed on the same four lanes (`lab/runs/ev8-s11-oracle/`).
The SRM console-log check is clean on the interpreter, JIT and JIT_VERIFY
lanes (JIT_VERIFY: 0 mismatches). `boards_check.sh` passes its 12 boards
and three `init` checks, with `--diff` against main and against main's
gate run (ev8s4) the same on every board; with `BOARDS_EV7_CPU=ev8` the
ES47, ES80 and GS1280 and their `init` checks pass, the same as the gate's
run on the row. `guests_check.sh` passes `vms-es47`, `linux-es47`,
`vms-es40` and `linux` (`lab/runs/ev8-s11-proof/`). No guest runs on
`ev8n` (none exists until S13).

## S11b (2026-10-25)

The ES47, ES80 and GS1280 in **21464 mode**: section 2 as implemented.
The board rows take `ev8n` processors beside their EV7 rows
(`platform_config::other_cpu_model`, `platforms/Platforms.cpp`), and the
processor family puts the machine in the mode: one family per machine,
refused otherwise **[decision]**. The EV7 rows and the `ev8` row keep the
EV7 mode exactly. The code is `cpu/ev7/Ev7.hpp` (the map),
`chipsets/marvel/` (`Marvel`, `Ev7Csr`, `Topology`, `Io7`, `Gio`) and
`platforms/es47/Es47.cpp`; every decision is in
[cpu-fidelity.md](../cpu-fidelity.md), "The Marvel model in 21464 mode",
and the board side in [marvel.md](marvel.md), "21464 mode".

**What is implemented**, section by section:

- **2.1 The map** (N18): `ev7::address_map`, a value per machine
  (`CMarvel::map()`): `kEv7Map` (44 bits, the PE inverted in PA<43:35>)
  and `k21464Map` (48 bits, ipe8, PA<47> the I/O bit, memory at ((n & 3) |
  (n & 0x3fc) << 1) << 34). Every one of Ev7.hpp's 21 uses goes through
  it; the decode refuses an inverted field that names no 10-bit PID
  (PA<46:45> not 11) **[inference]**. memory.bits is still at most 34:
  the second 16 GB of a PID, at + 2^36, is not modelled **[decision]**.
- **2.2 PIDs** (N19): `CMarvelTopology::build(..., dense)`: chip c is PID
  c in the configuration's order, its TPUs share its PID, register block
  and coordinates; each chip keeps the console's number for its place
  (`ev7_node::site`), by which the layouts cable their IO7s, so the ES80's
  IO7s are on PIDs 0, 2, 4 and 6 and its hoses (PID x 4 + port) are 0-3,
  8-11, 16-19 and 24-27 **[inference]**: N19 with the hose numbering kept.
  R_WHOAMI holds EW <9:5>, NS <4:0>; CPU_BASE is the chip's dense number
  (S11's, checked against the topology at start); R_ROUT has 532 entries,
  the route to PID n in entry n in the IDS's layout (`route21464`). Its
  flag bits are the IDS's text layer's order [IDS pp. 526-527]: every
  access bit set, CAD set, BOU, STF and PAR 0, the hops N/S first as on
  the EV7, North and East the higher coordinates, the sharing-mask entries
  0 **[decision]**. WHAMI = PID x 4 + TPU limits a machine to 32
  processors: an ES80 of eight chips of four TPUs fits, a GS1280 of more
  than eight chips needs `tpus` below 4, and more is refused at
  configuration with the reason.
- **2.3 The registers** (N20): at the EV7's offsets; R_CFG2 at 0x0E0, each
  port's R_n_CFG2 at +0x50, R_IO_CFG2 at 0xC050 and R_IO_T2CFG at 0xC060
  **[decision]**; R_n_T2CFG is the EV7's RBOX_n_T2CFG at +0x40 (a
  correction to the table, below). They hold what is written. The EV7
  console's GIO lock at 0x80000 is not decoded in 21464 mode (2.5).
- **2.4 Interrupts** (N21, N22): one R_INT_STAT per chip, 32 bits, the
  table's fields and levels, written with ones to clear (<12> clears only
  as the queue empties); R_INT_MASK per TPU, TPU 0's at 0x060 and 0x3F0000,
  TPU t's at 0x3F0000 + 0x40 x t; TPU t's level L asserted while a bit of
  level L is set in R_INT_STAT & R_INT_MASK[t], into its own ISUM<EI>,
  gated by its own IER_CM<EIEN>. R_INT_REQ sets only the fields N22 has it
  set, <3:0>, <11:8> and <31:18> **[inference]**, and reads back what was
  written, as the EV7's **[decision]**. The interval timer: ITV <7:0>, the
  EV7's period for its value **[inference]**, 0 off **[inference]**, the
  machine's tick schedule PID 0's, each tick setting <7:4> on every chip
  whose ITV is not 0. The queue: four IIDs; a processor's fifth write to
  R_INT_QUEADD is dropped and counted, since a store cannot see a WrIONack
  **[decision]**; R_INT_QUE reads the head with <24> valid and a write
  removes it. The management side's interrupt (GIO_INT) is <13>
  (`CEv7Csr::gio_interrupt`); halts (<11:8>) come from R_INT_REQ only, and
  <14>-<17> are never raised.
- **2.5 GIO** (N23): GIO_DATA carries 32 bits (a write passes <31:0> on, a
  read returns DONE and the far register's low 32 bits); DONE reads 1 at
  reset and GIO_CNFG's DIVISOR <7:1> runs the clock, which reset leaves
  stopped: a transaction begun while it is zero waits, DONE clear, until a
  divisor is written [IDS pp. 472-474]. The CMM is unchanged (one port per
  PID: a chip's TPUs share theirs).
- **2.6 The IO7**: its space through the map; LSI_CTL's target names a
  dense PID; an IID the target's full queue refuses is kept and sent again
  as soon as an entry of that queue is taken, the IDS's "retry the same
  one" [IDS p. 465] **[decision]** (when: not on a timer); a refusal is not
  in the state file **[decision]**. `ALPHABOX_TRACE_IO7` names both.
- **Reset and start** (N17, 2.7): no XSROM and no console. A machine in
  21464 mode starts from a boot image given as `rom.decompressed` (the
  oracle's loader: entry, PAL_BASE, the board's 0x480000 bytes, the tag),
  and refuses to start without one, since the EV7 console's decompressor
  would otherwise run on the 21464 **[decision]**; every TPU of every chip
  takes `ev8_reset` at the image's PAL_BASE + 0xB00 with its TPU_ID,
  CPU_BASE its chip's PID. The XSROM hand-off and RBOX_SCRATCH1's start
  protocol stay in the code, unused. `docs/headless.md` and `es40.cfg`
  say how such a machine is started.

**The tests**:

- `test/cpu/marvel8_mp.py`: one image (the EV8 PALcode, `build_pal8` with
  `by_chip`: each TPU numbered CPU_BASE x 4 + TPU_ID, its trap record and
  start word by that number, only number 0 going on) on the ES47 (2 chips
  of 4 TPUs), the ES80 (8 of 4) and the GS1280 (16 of 2), on the board
  rows with no test hook, output through GIO to the CMM's terminal. TPU 0
  of chip 0 checks GIO (DONE at reset, a write that does not complete with
  the divisor zero and does after one, 32 bits, a read of the CMM's
  register 0, a character typed at the terminal and echoed), the windows
  of PIDs 0 and 1 through Linux's KSEG form (SPE43), R_ROUT's entries,
  each chip's memory word at its PID's base, R_INT_REQ's fields, chip 0's
  four tick bits and its own cleared, the queue's four entries, a fifth
  refused, a sym53c810's SCRIPTS INT (LSI 4 of hose 0) refused by the full
  queue and resent by the IO7 once an entry is taken (the IO7 trace names
  both: an IO7 interrupt, not a register-level injection), the queue
  emptied. Every other TPU reads TPU_CNFG, CPU_BASE, its own chip's and the
  next chip's R_WHOAMI through ipe8, writes its R_INT_MASK (TPU 0 at
  0x060, read back at 0x3F0000), and waits for an IPI, in QUIESCE or (one
  in eight) a loop: TPU 0 sends one to each TPU in turn and counts the
  woken after each, which must be one more each time; then every TPU takes
  two timer interrupts with its own tick bit set. Then three
  configurations refused, each for its reason: a GS1280 of nine chips of
  four TPUs, `ev7` and `ev8n` mixed, no boot image. 129, 447 and 455
  results (JIT_VERIFY's mismatch count among them) and 3 refusals, 1034
  in all. A deliberately broken steering (every TPU's lines from all four
  masks) failed 19 of the ES47's. Since 2026-10-30 each of TPU 0's waits
  (for the others to be ready, each to wake, each to finish its ticks,
  its own IPI, chip 0's tick bits, the SCSI interrupt, the typed
  character) ends on elapsed time, a second of RPCC (two for the
  character), where it ended after 1 << 22 loop turns, 4.5 ms on the
  native JIT and 750 ms on JIT_VERIFY: three runs of `--layout es47` at
  once on the native JIT, 100 each, failed 18 of 300 at a load of 26 to
  46, every failure a TPU the host ran after TPU 0 had stopped waiting
  for it, and passed 300 of 300 with the time bound at 28 to 48.
- `test/unit/marvel21464_test.cpp`: the map for all 1024 PIDs (decode,
  memory, the KSEG property for PIDs 0-31, the fields no PID has, the
  EV7's map unchanged), the dense topology on the three layouts, the IO7
  placement, R_ROUT's entries and R_WHOAMI (27115 checks).

**The proofs**. Every lane built and passed `ctest` (the interpreter, the
AArch64 JIT with SDL, JIT_VERIFY, the x86-64 JIT, a headless JIT), and
every commit of the series built with its unit tests on an interpreter
lane. `marvel8_mp.py` passed on the interpreter, JIT, JIT_VERIFY and
x86-64 lanes, all three layouts, 1034 results each, JIT_VERIFY 0
mismatches. The CPU oracle on the same four lanes gave main's verdicts:
`ev68cb` and `ev7` PASS 20180, KNOWN 61; `ev8` PASS 20432, KNOWN 73;
`ev8n` PASS 20906, KNOWN 70, SKIP 74; FAIL 0 everywhere and JIT_VERIFY 0
mismatches. `quiesce_mp.py` and `tpu_mp.py` passed on the four lanes. The
SRM console-log check was clean on the interpreter, JIT and JIT_VERIFY
lanes (0 mismatches). `boards_check.sh` passed its 12 boards and three
`init` checks, `--diff` against main and against the gate's ev8s11 run
the same on every board; with `BOARDS_EV7_CPU=ev8` the ES47, ES80 and
GS1280 and their `init` checks passed, the same as the gate's ev8s4 run;
with `ev8` of four TPUs the three boards listed 8 processors each
(`lab/runs/ev8-s11b-proof/`, `boards-ev8-s11b-*`). On a host at load 300
the owner stopped the rest of the broad sweep, which the gate on main runs
after the merge: the four-TPU `init` checks and their `--diff`,
`marvel_init_check.sh gs1280:16`, and `guests_check.sh` (`vms-es47`,
`linux-es47`, with `ev7` and with `ev8`) were not run on this branch. At
the last commit, which only drops two functions nothing called, every
lane was built again with `ctest`, `marvel8_mp.py`, the oracle with
`--cpu ev8n` and `tpu_mp.py` passed again on the four lanes (the same
counts, JIT_VERIFY 0 mismatches), and the ES47 alone passed
`boards_check.sh` and its `init` check, its `--diff` against main the
same (`lab/runs/ev8-s11b-proof-final/`, `boards-ev8-s11b-es47`).

## Findings

On the way, against [ev8.md](ev8.md) and [ev8-isa.md](ev8-isa.md)
(applied to ev8.md by the coordinator on 2026-10-25):

- **The shadow-register conflict is settled** by the IDS itself: two
  shadow registers, S0 and S1, with a position-dependent overlay of
  R22-R25 [IDS p. 580] (section 1.3). [ev8.md](ev8.md)'s table ("Shadow
  registers ... **[conflict]**") can say so.
- **HW_MFPR's <4:0> is the destination Rc**, not a writer class
  [IDS pp. 571, 650]; [ev8.md](ev8.md)'s table gives "reader and writer
  classes in <24:21> and <4:0>" for both instructions, which is right for
  HW_MTPR only.
- **The entry table has a second version** in chapter 22 (FAST_RESET,
  TPU_RESTART, MILD_RESET, DST_NXM) [IDS pp. 633-634]; [ev8.md](ev8.md)'s
  list follows chapter 16 alone, and its "Starting TPUs" paragraph could
  name TPU_RESTART ("another TPU has requested a restart") and the
  "Suspending TPUs" evidence for QUIESCE-based parking [IDS pp. 511, 632].
- **An addition, not a correction**: [ev8.md](ev8.md)'s memory-management
  notes give I_CTL's three superpage enables; Appendix C names them SPE52,
  SPE43 and SPE32, gives SPE43 the sign extension of VA<40> into
  PA<47:41>, and makes SPE52 usable with 43-bit virtual addresses
  [IDS pp. 688, 695], which is how a Linux-style kernel reaches the whole
  48-bit PA, and what N18 rests on.
- **[ev8-isa.md](ev8-isa.md) needs no correction.** Its section 2 leaves
  VCTL's native index open, which N8 proposes to fill (0xAC); its note on
  QUIESCE_TIMEOUT's reset conflict [IDS pp. 511, 678] is what N17 follows
  for the native row.
- **Restriction 13 leaves out R22 and R23** (found with nada,
  2026-10-25): by p. 580's own table, a native CMOV with Rc 22 or 23
  reads S0 or S1 as its old value (section 1.3, **[inference]**). The
  assembler refuses it (section 3.5), so no Alphabox PALcode contains it.
- **D5's IO7 part does not carry over** to the native track (N22): the
  IO7's target field names a PID only. D5 as written stays right for the
  EV7-compatible row's S4, where it is Alphabox's construction anyway.

S11 (2026-10-25):

- **1.7 said a QUIESCE after the native reset sleeps until a store or an
  interrupt**: with WATCH_EN clear, as it resets, a store does not wake it
  [IDS p. 511]; corrected in its own commit. N17 ("QUIESCE_TIMEOUT as the
  register says") stands, and its parking sets WATCH_EN.
- **ITB_IM's modes say "the current TPUGRP field in M_PCTX"** where
  ITB_IS says I_PCTX's [IDS pp. 494-495] **[conflict]**: I_PCTX's is used
  for the ITB, which PALcode writes alike anyway.
- **The I_CTL and M_CTL selects** write REDUCED_PT (and M_CTL's DBL_ALT)
  only with 11, as their lists give the fields [IDS pp. 489, 506]; a
  PALcode that sets DBL_ALT with the 10 select does not.
- [ev8-isa.md](ev8-isa.md) needed no correction.

S11b (2026-10-25):

- **2.3's table gave R_n_T2CFG a free offset**: the EV7's port blocks
  already have RBOX_n_T2CFG at +0x40 (the console's table), so by N20's
  own rule it is that register; only R_n_CFG2 is new. Corrected in its own
  commit.
- **GIO_CNFG's divisor gates the port** [IDS pp. 472-473]: reset leaves
  the clock stopped, so our PALcode (S12a) must write a divisor before its
  first GIO transaction. 2.5 lists the field without saying so.
- **The hoses follow the dense PIDs**: with hose = PID x 4 + port (Linux's
  IO7_HOSE), an ES80 in 21464 mode has its drawers' hoses at 0-3, 8-11,
  16-19, 24-27, not the EV7 console's 0-3, 32-35, 64-67, 96-99; a
  configuration's `pciN.M` places follow.
- **R_ROUT's flags** (BOU, CAD, PME, PIO, PIP, IME, IIO, IIP, VAL in
  <8:0>, STF <9>, RNS <10>, REW <11>) were taken from the text layer, in
  its order; the packet checked only DNS and DEW on the scan. Nothing but
  our own code reads them.
- [ev8-isa.md](ev8-isa.md) needed no correction.

## Rules

- Documentation only until the owner decides; every value not taken
  from a source is an Alphabox decision, marked in the code and in
  [cpu-fidelity.md](../cpu-fidelity.md) when it is implemented.
- Existing machines must not change behaviour: the 21464 mode is chosen
  by the processor family, and the EV7 rows, the `ev8` row and their
  console keep everything they have.
- Firmware the project writes is built from source in the repository;
  firmware images are never downloaded.
- Speed claims go through `test/tools/perf_ab.py`.
