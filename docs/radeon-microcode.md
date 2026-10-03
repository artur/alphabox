# The Radeon CP micro-engine and its microcode

The Radeon's command processor (CP) does not parse PM4 packets in fixed
logic: a small micro-engine (ME) does, running microcode the driver loads
at start-up. ATI never documented the micro-engine. This page collects
what the public sources say about it, every microcode image they
published, and what the images themselves show about the instruction
format -- for the Radeon 7500 (RV200) model in
`src/devices/video/radeon/` (the card is [radeon.md](radeon.md)) and for
the Radeons that will follow it.

Every statement carries its standing:

- **[doc]** documented: AMD's *Radeon R5xx Acceleration* v1.5 (2010; the
  CP chapter 5 and the PM4 chapter 6, which the R100 already followed),
  its register reference, or the drivers' sources;
- **[inf]** inferred: forced by evidence repeated across the images (a
  correlation with the documented packets that holds in every case
  looked at);
- **[spec]** speculative: a reading the evidence allows but does not
  force.

The microcode bytes are ATI's and are **not** in this repository; it
carries their CRCs, names and this analysis. To look at an image, take it
from linux-firmware (`radeon/R100_cp.bin`) or dump the one a guest loads
(`ALPHABOX_RADEON_ME_DUMP`, [headless.md](headless.md)) and run
`test/tools/radeon_me_dis.py` on it.

## What the micro-engine is

- The CP takes PM4 packets from its streams -- the ring (primary, bus
  master), the indirect buffers, or the PIO apertures -- into the
  command stream queue (CSQ), and the micro-engine parses them out of the
  CSQ [doc: R5xx 5.11, "the act of writing ... causes that data to be
  enqueued to the Command Stream Queue", and 6.1, "let the hardware
  (Microengine) do the rest of the job"].
- Its program lives in the ME RAM: 256 entries of 40 bits [doc: R5xx 5.12
  -- a 40-bit holding register, the low 8 bits of `CP_ME_RAM_DATAH` on
  top of the 32 of `CP_ME_RAM_DATAL`; `CP_ME_RAM_ADDR` <7:0>; Linux and the
  legacy DRM load 256 entries on every part from the R100 to the R5xx].
- Loading: write the start address to `CP_ME_RAM_ADDR` (0x7d4), then for
  each entry `DATAH` (0x7dc) and `DATAL` (0x7e0); the write of `DATAL`
  stores the entry and increments the address [doc: R5xx 5.12; Linux
  `r100_cp_load_microcode`, legacy `radeon_cp_load_microcode`].
  Reading: write `CP_ME_RAM_RADDR` (0x7d8), then read `DATAH`, `DATAL`
  pairs [doc: R5xx 5.13].
- `CP_ME_CNTL` (0x7d0): `ME_STAT` <15:0> shows internal registers chosen
  by `ME_STATMUX` <20:16>; `ME_BUSY` <29>; `ME_MODE` <30> (1 = free
  running, 0 = single step); `ME_STEP` <31> steps one instruction [doc:
  R5xx register reference]. Linux's `r100_cp_init` (and the legacy
  `radeon_do_cp_start`) never single-step it.
- The ME RAM doubles as data: on the R5xx, entries 0xfc-0xff are four
  semaphores the `WAIT_SEMAPHORE` packet tests and the host sets with
  `CP_ME_RAM_ADDR` + `CP_ME_RAM_DATAL` [doc: R5xx 6.2.4.1]. Every R300 and
  later image leaves 0xfc-0xff zero, as that requires [inf].
- A type-3 header's IT_OPCODE <15:8>: its MSB says the body starts with
  `GUI_CONTROL` (the 2D settings) [doc: R5xx 6.2]; the opcode list is
  R5xx 6.2.1 and Linux's legacy `radeon_drv.h` [doc].
- The legacy DRM loads the R200 image on the R200 family and checks the
  packets a client sends against it: the `_2` draws, `3D_CLEAR_HIZ` and
  `INDX_BUFFER` are "safe but r200 only" (`radeon_state.c`,
  `radeon_check_and_fixup_packet3`) [doc]. That is the only statement in
  any source about what one image does that another does not.

## The images

Every distinct CP microcode image of the family the open sources carry.
Collected 2026-10-03 from the full history of libdrm (the DRI CVS
history from 2001: `linux/`, `shared/`, `shared-core/radeon_cp.c` and
`shared-core/radeon_microcode.h`, 300 revisions), Linux
`drivers/char/drm/radeon_cp.c` (v2.6.12-v2.6.25) and
`drivers/gpu/drm/radeon/radeon_microcode.h` (v2.6.28-v2.6.31),
FreeBSD (`sys/dev/drm`, stable/5-9), NetBSD (`external/bsd/drm`),
linux-firmware `radeon/*_cp.bin` (each file has one revision,
2009-08-31, and is byte-identical to the 2008 C array), and the Rage
128's `r128_cce.c`. The download record (URLs, commits, CRCs) is
`lab/radeon-microcode/INDEX.md` in the main checkout (git-ignored, as are
the images).

CRC-32 is given two ways: as Alphabox logs it (each entry as its DATAH
word then its DATAL word, little-endian -- the model's `m_me_ram` in host
order) and of the linux-firmware style file (the same words big-endian).
All images are 256 entries.

| Name | Logged CRC | File CRC | Parts (Linux) | First published | Changed |
|---|---|---|---|---|---|
| **R100_cp** | **b86caa2a** | 92b7f8d8 | R100, RV100, RV200, RS100, RS200 | DRI CVS 2001-01-05 (ati-5-0-0 merge, as `radeon_cp_microcode`) | never: identical in every libdrm, Linux, BSD copy and linux-firmware |
| R200_cp (2002) | 7f059413 | 8297f6b1 | R200, RV250, RV280, RS300 | DRI CVS 2002-08-26 (r200-0-1-branch merge) | replaced 2008-03-19 |
| R200_cp | f71981e4 | f8f7c7c3 | as above | libdrm 2008-03-19 ("production microcode for all radeons, r1xx-r6xx", Alex Deucher, AMD) | -- (linux-firmware R200_cp.bin) |
| R300_cp (2004) | 236491cd | f483f895 | R300, R350, RV350, RV380, RS400, RS480 | DRI CVS 2004-10-23 ("r300 microcode patch") | replaced 2008-03-19 |
| R300_cp | 1f7252ce | 63b4be38 | as above | libdrm 2008-03-19 | -- (linux-firmware R300_cp.bin) |
| R420_cp | 8402ea16 | e73cc904 | R420, R423, RV410 | libdrm 2008-03-19 | -- |
| R520_cp | b9f09374 | abea771a | R520, RV515, RV530, RV560, RV570, R580 | libdrm 2008-03-19 | -- |
| RS600_cp | caaae480 | e5f4096a | RS600 | libdrm 2008-03-19 | -- |
| RS690_cp | 2c1c2fae | ff756426 | RS690, RS740 | libdrm 2008-03-19 | -- |
| R128_cce | 70c3e844 | fb2e59a2 | Rage 128 (not a Radeon; same ME RAM ports) | Linux `r128_cce.c` | identical in 2.6.12 and 2.6.25 |

**OpenVMS 8.4's DECwindows server loads `b86caa2a`: ATI's R100 image**,
the one Linux loads on the RV200 [inf: CRC-32 match; confirmed
byte-for-byte against linux-firmware's `R100_cp.bin` with
`ALPHABOX_RADEON_ME_DUMP`]. HP's server and the open drivers ran the
same microcode.

The model (`RadeonMicrocode.cpp`) carries this table. When a driver
completes a load it logs `CP microcode loaded, 256 entries, CRC-32
<crc>: <name>, for the <parts>`, or `unknown microcode` (still accepted, as
before). An image for another generation is accepted with a
`%RADEON-W-UCODE` warning (see below).

## Instruction word

The 40-bit word splits into four fields [inf]:

```
 39          (rb+21) (rb+20)     (rb+3) (rb+2) rb  (rb-1)      0
+---------------+-----------------------+---------+---------------+
|      OP       |           F           |    M    |       A       |
+---------------+-----------------------+---------+---------------+
 rb = 11 (Rage 128), 12 (R100/R200), 13 (R300 and later)
```

- **A**, the low field, is a register index (an MMIO offset / 4), a
  branch target (an ME address) or a small immediate [inf].
- The field **widens by one bit per generation** and everything above it
  moves up [inf]: the instruction every image has at entry 0 is
  `0x10803800` (Rage 128), `0x21007000` (R100/R200) and `0x4200e000`
  (R300+), the same bits shifted at bit 11 and again at bit 12; a jump is
  DATAH `0x02`, `0x04`, `0x08`; and the register indices fit the parts'
  register spaces (the Rage 128's to 0x1ffc, the R100's to 0x3ffc, the
  R300's 3D registers at 0x4000-0x4fff need the 13th bit). Once A is
  taken out, the other fields line up across all three generations: the
  same idioms, with the same OP, F and M values, are in each.
- **OP**: 7 bits (R100). **M**: 3 bits, which with OP says where a
  register write's data comes from. **F**: the rest.

The image the ME must have is therefore of its generation: an R300 image
on an RV200 would be decoded with every field one bit off [inf]. What
the chip then does is not known; the model accepts such an image, warns,
and keeps parsing the packets as the R100 microcode does.

## Layout of an image

Every image, from the Rage 128 to the RS690, has the same frame [inf]:

| Entries | Content |
|---|---|
| 0, 1 | Two instructions, the same in every image (but the RS690's, whose entry 0 is a jump): "the rest of the body to consecutive registers from the index register" and its non-incrementing twin -- the **type-0** packet, with and without `ONE_REG_WR` [inf: the same instruction ends 3D_LOAD_VBPNTR's handler after it sets the index to `SE_VTX_AOS` (0x20c0), and the R200's INDX_BUFFER; the two differ in one F bit, the type-0 header's bit 15 is the one difference between the two packet forms] |
| 2, 3 | Two jumps, in every image. The R100's: to 180 (two "index from the header, one dword" steps: the **type-1** packet [inf]) and to 184 (`DP_GUI_MASTER_CNTL` from the body, then `SRC_PITCH_OFFSET`, `DST_PITCH_OFFSET`, the source and destination scissors, the brush, `BRUSH_Y_X`, ending in returns: the **GUI_CONTROL settings** of a type-3 packet whose opcode has bit 7 set [inf: the fields and their order are R5xx 6.2.2's SETTINGS]) |
| 4 .. 12 (R100) | The **type-3 dispatch table**: one byte per opcode from 0x10, low byte first, the ME address of the opcode's handler [inf, below] |
| then | Handlers |
| 216-218 (R100) | ASCII: "Madd this" (`0x6464614d 0x69687420 0x73`), reached only through opcode 0x2b's table entry [inf that it is text; why it is there, spec] |
| 228-231 (R100) | Constants 0, 0, 8, 4 [spec: data the handlers read] |
| 232-255 (R100) | Zero. R300 and later: 252-255 zero, the semaphores [doc] |

### The dispatch table

Entries 4.. hold one byte per type-3 opcode, opcode 0x10 (NOP) first.
The evidence that this is the dispatch table [inf]:

- the table ends where the opcode set ends: 36 bytes (0x10-0x33) in the
  R100 and Rage 128 images, 40 (0x10-0x37, through `3D_CLEAR_HIZ`) in the
  R200's, 44 (to 0x3b, through `MPEG_INDEX`) in the R300's and later;
- every byte is a code address, and the opcodes that have no packet point
  at one shared entry -- NOP's -- which is a single instruction that drops
  the rest of the body;
- packets documented as variants share a handler: BITBLT and
  BITBLT_MULTI; 3D_DRAW_VBUF, _IMMD and _INDX; the three `_2` draws;
- each handler writes the registers its packet documents, in order:
  PAINT_MULTI loops writing `DST_X_Y`, `DST_WIDTH_HEIGHT` (its [X|Y]
  [W|H] rectangles); HOSTDATA_BLT writes `DP_SRC_FRGD_CLR`,
  `DP_SRC_BKGD_CLR`, `DST_Y_X`, `DST_HEIGHT_WIDTH`, then loops on
  `HOST_DATA0` and `HOST_DATA_LAST`; TRANS_BITBLT calls a routine writing
  `CLR_CMP_CNTL`, `CLR_CMP_CLR_SRC`, `CLR_CMP_CLR_DST` and jumps into
  BITBLT; SET_SCISSORS writes `SC_TOP_LEFT`, `SC_BOTTOM_RIGHT`; POLYLINE
  `DST_LINE_START`/`END`; POLYSCANLINES `DST_HEIGHT_Y`, `DST_WIDTH_X`;
  the 3D draws `SE_VTX_FMT`, `SE_VF_CNTL`, then the rest of the body to
  `SE_PORT_DATA0`; the MPEG IDCT packets `IDCT_RUNS`, `IDCT_LEVELS`,
  `IDCT_CONTROL`; INDX_BUFFER `CP_IB2_BASE` and `CP_IB2_BUFSZ`
  (0x730/0x734, the second indirect buffer R5xx 6.2.3.11 says it uses);
- the R200's draw handler drops the first body dword before
  `SE_VF_CNTL` -- the R5xx guide's "[VAP_VTX_FMT] Not Written to
  Hardware, Microcode Throws Away" [doc] -- and its `_2` handler starts at
  `SE_VF_CNTL`.

How an opcode reaches the table -- whether the hardware indexes it with
the opcode less 0x10, and what it does with an opcode past the table's
end -- is not known [spec]. The opcode's bit 7 does not index it: the
table has 0x11 PAINT where the packet is 0x91 `CNTL_PAINT` [inf].

### Handler addresses

The table of every image, the handler's ME address per opcode (`-` = the
NOP handler, the opcode is skipped; blank = past the table's end). The
R128 column uses the Rage 128's own opcode numbers, which differ from
0x20 on (names are the Radeons').

| op | packet | R128 | R100 | R200 2002 | R200 | R300 2004 | R300 | R420 | R520 | RS600 | RS690 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 10 | NOP | 35 | 76 | 73 | 93 | 79 | 74 | 74 | 74 | 74 | 74 |
| 11 | PAINT | 36 | 77 | 74 | 94 | 80 | 75 | 75 | 75 | 75 | 75 |
| 12 | BITBLT | 46 | 91 | 84 | 104 | 90 | 85 | 85 | 85 | 85 | 85 |
| 13 | SMALLTEXT | 64 | 111 | 102 | 122 | 108 | 103 | - | - | - | - |
| 14 | HOSTDATA_BLT | 79 | 127 | 116 | 136 | 122 | 117 | 103 | 103 | 103 | 103 |
| 15 | POLYLINE | 18 | 66 | 65 | 85 | 73 | 68 | 68 | 68 | 68 | 68 |
| 16 | ? | 98 | - | - | - | - | - | - | - | - | - |
| 17 | ? | 96 | - | - | - | - | - | - | - | - | - |
| 18 | POLYSCANLINES | 109 | 146 | 131 | 151 | 136 | 131 | 117 | 117 | 117 | 117 |
| 19 | NEXT_CHAR | 89 | 138 | 125 | 145 | 130 | 125 | 111 | 111 | 111 | 111 |
| 1a | PAINT_MULTI | 43 | 86 | 81 | 101 | 87 | 82 | 82 | 82 | 82 | 82 |
| 1b | BITBLT_MULTI | 46 | 91 | 84 | 104 | 90 | 85 | 85 | 85 | 85 | 85 |
| 1c | TRANS_BITBLT | 62 | 109 | 100 | 120 | 106 | 101 | 101 | 101 | 101 | 101 |
| 1d | PLY_NEXTSCAN | 117 | 156 | 139 | 159 | 144 | 139 | 125 | 125 | 125 | 125 |
| 1e | SET_SCISSORS | 119 | 160 | 141 | 161 | 145 | 140 | 126 | 126 | 126 | 126 |
| 1f | ? | 118 | - | - | - | - | - | - | - | - | - |
| 20 | PRED_EXEC (R5xx) | 130 | - | - | - | - | 246 | 246 | 246 | 246 | 246 |
| 21 | COND_EXEC (R5xx) | 132 | - | - | - | - | - | 223 | 230 | - | - |
| 22 | WAIT_SEMAPHORE (R5xx) | 137 | - | - | - | - | 239 | 211 | 218 | 231 | 215 |
| 23 | 3D_RNDR_GEN_INDX_PRIM (R100) / WAIT_MEM (R5xx) | 137 | 173 | - | - | - | - | 217 | 224 | - | - |
| 24 | LOAD_MICROCODE | 13 | 61 | 60 | 80 | 68 | - | - | - | - | - |
| 25 | ? | 139 | 175 | - | - | - | - | - | - | - | - |
| 26 | WAIT_FOR_IDLE | 188 | 225 | 218 | 238 | 226 | 225 | 197 | 197 | 211 | 201 |
| 27 | ? | - | - | - | - | - | - | - | - | - | - |
| 28 | 3D_DRAW_VBUF | - | 175 | 152 | 172 | 156 | 151 | 130 | 130 | 137 | 137 |
| 29 | 3D_DRAW_IMMD | - | 175 | 152 | 172 | 156 | 151 | 130 | 130 | 137 | 137 |
| 2a | 3D_DRAW_INDX | - | 175 | 152 | 172 | 156 | 151 | 130 | 130 | 137 | 137 |
| 2b | ? | - | 216 | - | 242 | - | 228 | 200 | 200 | 214 | 204 |
| 2c | LOAD_PALETTE | 180 | 220 | 213 | 233 | 222 | 221 | 193 | 193 | 207 | 197 |
| 2d | ? | 171 | - | - | 249 | - | 235 | 207 | 207 | 221 | 211 |
| 2e | ? | 141 | - | - | - | - | - | - | - | - | - |
| 2f | 3D_LOAD_VBPNTR | - | 214 | 211 | 231 | 220 | 219 | 191 | 191 | 205 | 195 |
| 30 | MPEG_IDCT_MACROBLOCK | - | 16 | 17 | 17 | - | - | - | 213 | 226 | - |
| 31 | MPEG_IDCT_MACROBLOCK_REV | - | 13 | 14 | 14 | - | - | - | - | - | - |
| 32 | 3D_CLEAR_ZMASK | - | 209 | 201 | 221 | 205 | 204 | 176 | 176 | 190 | - |
| 33 | INDX_BUFFER | - | - | 157 | 177 | 161 | 156 | 135 | 135 | 142 | 142 |
| 34 | 3D_DRAW_VBUF_2 |  |  | 155 | 175 | 157 | 152 | 131 | 131 | 138 | 138 |
| 35 | 3D_DRAW_IMMD_2 |  |  | 155 | 175 | 157 | 152 | 131 | 131 | 138 | 138 |
| 36 | 3D_DRAW_INDX_2 |  |  | 155 | 175 | 157 | 152 | 131 | 131 | 138 | 138 |
| 37 | 3D_CLEAR_HIZ |  |  | 206 | 226 | 210 | 209 | 181 | 181 | 195 | - |
| 38 | 3D_CLEAR_CMASK |  |  |  |  | 215 | 214 | 186 | 186 | 200 | - |
| 39 | 3D_DRAW_128 (R5xx) |  |  |  |  | 159 | 154 | 133 | 133 | 140 | 140 |
| 3a | MPEG_INDEX (R5xx) |  |  |  |  | 15 | 15 | 15 | 15 | 15 | 15 |
| 3b | ? |  |  |  |  | - | - | - | - | - | - |

### What the versions implement differently

From the table [inf], where the model takes it into account:

- **R100 vs R200**: the R100 image has no `INDX_BUFFER` (NOP's handler)
  and its table ends before the `_2` draws and `3D_CLEAR_HIZ` -- exactly
  the legacy DRM's "r200 only" list [doc]. The R200 image has them, and
  drops `3D_RNDR_GEN_INDX_PRIM` (0x23) and the unnamed 0x25, which on the
  R100 go into the 3D draw handler.
- The R200's 3D draws drop the `VTX_FMT` dword; the R100's write it to
  `SE_VTX_FMT` [inf]. (Not modelled: no guest loads the R200 image on the
  RV200, and the model's draws take the format from the packet.)
- **R200 2002 vs 2008**: the 2008 image adds 0x2b and 0x2d, unnamed
  (handlers writing six registers at 0x2310-0x2318 and 0x2490-0x2498,
  in a loop) [inf]; R300 2008 likewise.
- **R300 2004 vs 2008**: the 2008 image drops `LOAD_MICROCODE` (0x24) and
  adds `PRED_EXEC` and `WAIT_SEMAPHORE`, packets R5xx 6.2.1 documents.
- **R420 on**: `SMALLTEXT` gone; `COND_EXEC`/`WAIT_MEM` on the R420 and
  R520 only.
- Word for word (longest common subsequence of the 256 words, branch
  targets included): R200 2002/2008 share 164 words; R300 2004/2008
  176; R100/R200 2002 155; R420/R520 238 (224 at the same address);
  Rage 128/R100 26 (the width change moves every field).

The model's packet set: with a known image loaded, an opcode its table
sends to NOP's handler is skipped (`CP operation xx: the <name>
microcode has no handler for it, skipped as a NOP`), and the R200
packets run when the R200 image is loaded; opcodes past the table's
end, and every opcode of an unknown image, are as the part's row says
(the R100 set) [inf for the first two; the third is the model's choice].

## Instructions

The opcode values (R100 numbering) and what the evidence says of them.
`radeon_me_dis.py` prints these readings with their marks.

| OP | Count (R100) | Reading |
|---|---|---|
| 01 | 128 | A step. M=7: the next body dword to register A [inf: every one of them is a register the packet documents, in its order]. M=5, A=0: drop one body dword [inf: the R200's VTX_FMT, which R5xx says the microcode throws away]. M=2 or 6, F=0x00800: a computed value to register A [spec: e.g. PAINT's `DST_HEIGHT_WIDTH` from its corners, the scissors with an offset added]. Other forms (A an immediate such as 0xe6/0xe7 before a write of `DSTCACHE_CTLSTAT` / `RB3D_DSTCACHE_CTLSTAT`, which every 2D and 3D handler starts with) [spec: an ALU load, then the cache flush] |
| 00 | 60 | A packet's last step. M=7: the rest of the body to register A (no increment: `SE_PORT_DATA0` for the 3D draws, `SC_BOTTOM_RIGHT`) [inf]; M=7, A=0: to consecutive registers from the index register (entries 0/1: type 0) [inf]. M=5, A=0: drop the rest (NOP) [inf]. Also the encoding of the table and data words [inf] |
| 02 | 18 | Jump to A [inf: loops close on the handler's first register write; F/M set on a few -- conditional? [spec]] |
| 0c | 12 | Call A [inf: TRANS_BITBLT calls the `CLR_CMP` routine, which ends in an 03, then jumps to BITBLT; the settings routine calls 170/171] |
| 03 | 13 | Return, after an optional register write of 01's forms [inf] |
| 0b | 4 | Indexed jump through the next entry's four bytes, A selecting the byte [inf: the MPEG IDCT handlers; the inline tables hold code addresses] |
| 04, 06, 09, 0d, 0e | 2, 4, 2, 3, 2 | Conditional branches to A [inf: A is a code address and the fall-through is code]; the conditions [spec] |
| 0a, 0f, 10 | 1, 3, 4 | 10 with M=7: body dwords to `HOST_DATA0` or to registers from the index register, repeated [spec]; 0a, 0f unknown |

The F field's meaning is not established. Bits of it recur with the
same apparent role -- F bit 0x200 separates the incrementing type-0 form
from the `ONE_REG_WR` form [inf]; bit 0x10000 marks the settings
routine's writes, which happen only when their `GUI_CONTROL` bit is set
[spec] -- but no general decoding follows. A full instruction-level
semantics (the ALU, the conditions, the end-of-packet rule) is beyond
what 256 unlabelled 40-bit words support; `radeon_me_dis.py` gives the
structure and the readings above, not a disassembly of ALU operations.

## The model

`RadeonMicrocode.cpp` (`find_microcode`, `identify_microcode`,
`microcode_packet`) and its hook in `RadeonCP.cpp`:

- an image is identified when its load completes (and again when a later
  load ends): logged by name, or "unknown microcode";
- the word width comes from entries 2 and 3 (jumps in every known
  image). An image of another width than the part's (the chip row's
  `cp_microcode` family): `%RADEON-W-UCODE`, accepted, packets parsed as
  before -- what the silicon does is not known;
- a known image for another part of the same width (the R200 image on
  the RV200): `%RADEON-W-UCODE`, and its packet set is used, since the
  micro-engine would run it [inf];
- a known image's table decides which type-3 packets are skipped as NOPs
  (`microcode_packet`); the R200 packets run if it has them;
- the micro-engine still does not execute the microcode: the packets are
  interpreted natively ([radeon.md](radeon.md)).

`ALPHABOX_RADEON_SELFTEST` checks the table, synthetic images of each
width, and the R100/R200 packet sets (by CRC; no microcode bytes).
