/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

/* Register definitions and access helpers shared by the Sym53C8xx units.
 * Macros refer to members of CSym53C8xx (state, m_chip); include this only
 * from the family's own .cpp files, after Sym53C8xx.hpp. */

#if !defined(INCLUDED_SYM53C8XX_REGS_H)
#define INCLUDED_SYM53C8XX_REGS_H

// SCRIPTS runaway guard — maximum instructions per guest-initiated start
// (DSP write, DCNTL.STD, SIGP resume). Real drivers never approach this.
#define SYM_MAX_INSN_PER_BURST 100000

// Maximum instructions run inline on the register-writing (CPU) thread
// before handing SCRIPTS to the device thread; see run_scripts_inline().
#define SYM_INLINE_INSN_LIMIT 256

// Build with -DDEBUG_SYM_START to log SIGP writes that find no WAIT RESELECT
// and inline->thread handoffs (the header adds counters for it).

/// Register 00: SCNTL0: SCSI Control 0
#define R_SCNTL0 0x00
#define R_SCNTL0_ARB1 0x80
#define R_SCNTL0_ARB0 0x40
#define R_SCNTL0_START 0x20
#define R_SCNTL0_WATN 0x10
#define R_SCNTL0_EPC 0x08
#define R_SCNTL0_AAP 0x02
#define R_SCNTL0_TRG 0x01
#define SCNTL0_MASK 0xFB

/// Register 01: SCNTL1: SCSI Control 1
#define R_SCNTL1 0x01
#define R_SCNTL1_CON 0x10
#define R_SCNTL1_RST 0x08
#define R_SCNTL1_IARB 0x02

/// Register 02: SCNTL2: SCSI Control 2
#define R_SCNTL2 0x02
#define R_SCNTL2_SDU 0x80
#define R_SCNTL2_WSS 0x08
#define R_SCNTL2_WSR 0x01
#define SCNTL2_MASK (m_chip.scntl2_mask)
#define SCNTL2_W1C (m_chip.scntl2_w1c)

/// Register 03: SCNTL3: SCSI Control 3
#define R_SCNTL3 0x03
#define R_SCNTL3_EWS 0x08
#define SCNTL3_MASK (m_chip.scntl3_mask)

/// Register 04: SCID: SCSI Chip ID
#define R_SCID 0x04
#define R_SCID_ID (m_chip.id_mask)
#define SCID_MASK (m_chip.scid_mask)

/// Register 05: SXFER: SCSI Transfer
#define R_SXFER 0x05

/// Register 06: SDID: SCSI Destination ID
#define R_SDID 0x06
#define R_SDID_ID (m_chip.id_mask)
#define SDID_MASK (m_chip.id_mask)

/// Register 07: GPREG: General Purpose
#define R_GPREG 0x07
#define GPREG_MASK (m_chip.gpreg_mask)

/// Register 08: SFBR: SCSI First Byte REceived
#define R_SFBR 0x08

/// Register 09: SOCL: SCSI Output Control Latch
#define R_SOCL 0x09
#define R_SOCL_ACK 0x40
#define R_SOCL_ATN 0x20

/// Register 0A: SSID: SCSI Selector ID
#define R_SSID 0x0A
#define R_SSID_VAL 0x80
#define R_SSID_ID (m_chip.id_mask)

/// Register 0B: SBCL: SCSI Bus Control Lines
#define R_SBCL 0x0B
#define R_SBCL_REQ 0x80
#define R_SBCL_ACK 0x40
#define R_SBCL_BSY 0x20
#define R_SBCL_SEL 0x10
#define R_SBCL_ATN 0x08
#define R_SBCL_MSG 0x04
#define R_SBCL_CD 0x02
#define R_SBCL_IO 0x01
#define R_SBCL_PHASE 0x07

/// Register 0C: DSTAT: DMA Status
#define R_DSTAT 0x0C
#define R_DSTAT_DFE 0x80
#define R_DSTAT_MDPE 0x40
#define R_DSTAT_BF 0x20
#define R_DSTAT_ABRT 0x10
#define R_DSTAT_SSI 0x08
#define R_DSTAT_SIR 0x04
#define R_DSTAT_IID 0x01
#define DSTAT_RC 0x7D
#define DSTAT_FATAL 0x7D

/// Register 0D: SSTAT0: SCSI Status 0
#define R_SSTAT0 0x0D
#define R_SSTAT0_RST 0x02
#define R_SSTAT0_SDP0 0x01
#define R_SSTAT0_ILF 0x80
#define R_SSTAT0_ORF 0x40
#define R_SSTAT0_OLF 0x20
#define R_SSTAT0_AIP 0x10
#define R_SSTAT0_LOA 0x08
#define R_SSTAT0_WOA 0x04

/// Register 0E: SSTAT1: SCSI Status 1
#define R_SSTAT1 0x0E
#define R_SSTAT1_SDP1 0x01
#define R_SSTAT1_PHASE 0x07

/// Register 0F: SSTAT2: SCSI Status 2
#define R_SSTAT2 0x0F
#define R_SSTAT2_LDSC 0x02

/// Register 10..13: DSA: Data Structure Address
#define R_DSA 0x10

/// Register 14: ISTAT: Interrupt Status
#define R_ISTAT 0x14
#define R_ISTAT_ABRT 0x80
#define R_ISTAT_SRST 0x40
#define R_ISTAT_SIGP 0x20
#define R_ISTAT_SEM 0x10
#define R_ISTAT_CON 0x08
#define R_ISTAT_INTF 0x04
#define R_ISTAT_SIP 0x02
#define R_ISTAT_DIP 0x01
#define ISTAT_MASK 0xF0
#define ISTAT_W1C 0x04

/// Register 18: CTEST0: Chip Test 0
#define R_CTEST0 0x18

/// Register 19: CTEST1: Chip Test 1
#define R_CTEST1 0x19
#define R_CTEST1_FMT 0xF0
#define R_CTEST1_FFL 0x0F

/// Register 1A: CTEST2: Chip Test 2
#define R_CTEST2 0x1A
#define R_CTEST2_DDIR 0x80
#define R_CTEST2_SIGP 0x40
#define R_CTEST2_CIO 0x20
#define R_CTEST2_CM 0x10
#define R_CTEST2_SRTCH 0x08
#define R_CTEST2_TEOP 0x04
#define R_CTEST2_DREQ 0x02
#define R_CTEST2_DACK 0x01

/// Register 1B: CTEST3: Chip Test 3
#define R_CTEST3 0x1B
#define R_CTEST3_REV 0xf0
#define R_CTEST3_FLF 0x08
#define R_CTEST3_CLF 0x04
#define R_CTEST3_FM 0x02
#define CTEST3_MASK 0x0B

/// Register 1C..1F: TEMP: Temporary
#define R_TEMP 0x1C

/// Register 20: DFIFO: DMA FIFO
#define R_DFIFO 0x20

/// Register 21: CTEST4: Chip Test 4
#define R_CTEST4 0x21

/// Register 22: CTEST5: Chip Test 5
#define R_CTEST5 0x22
#define R_CTEST5_ADCK 0x80
#define R_CTEST5_BBCK 0x40
#define CTEST5_MASK (m_chip.ctest5_mask)

/// Register 24..26: DBC: DMA Byte Counter
#define R_DBC 0x24

/// Register 27: DCMD: DMA Command
#define R_DCMD 0x27

/// Register 28..2B: DNAD: DMA Next Address
#define R_DNAD 0x28

/// Register 2C..2F: DSP: DMA SCRIPTS Pointer
#define R_DSP 0x2C

/// Register 30..33: DSPS: DMA SCRIPTS Pointer Save
#define R_DSPS 0x30

/// Register 34..37: SCRATCHA: Scratch Register A
#define R_SCRATCHA 0x34

/// Register 38: DMODE: DMA Mode
#define R_DMODE 0x38
#define R_DMODE_MAN 0x01

/// Register 39: DIEN: DMA Interrupt Enable
#define R_DIEN 0x39
#define DIEN_MASK 0x7D

/// Register 3A: SBR: Scratch Byte Register
#define R_SBR 0x3A

/// Register 3B: DCNTL: DMA Control
#define R_DCNTL 0x3B
#define R_DCNTL_SSM 0x10
#define R_DCNTL_STD 0x04
#define R_DCNTL_IRQD 0x02
#define R_DCNTL_COM 0x01
#define DCNTL_MASK 0xFB

/// Register 3C..37: ADDER: Adder Sum Output
#define R_ADDER 0x3C

/// Register 40: SIEN0: SCSI Interrupt Enable 0
#define R_SIEN0 0x40
#define SIEN0_MASK 0xFF

/// Register 41: SIEN1: SCSI Interrupt Enable 1
#define R_SIEN1 0x41
#define SIEN1_MASK (m_chip.sien1_mask)

/// Register 42: SIST0: SCSI Interrupt Status 0
#define R_SIST0 0x42
#define R_SIST0_MA 0x80
#define R_SIST0_CMP 0x40
#define R_SIST0_SEL 0x20
#define R_SIST0_RSL 0x10
#define R_SIST0_SGE 0x08
#define R_SIST0_UDC 0x04
#define R_SIST0_RST 0x02
#define R_SIST0_PAR 0x01
#define SIST0_RC 0xFF
#define SIST0_FATAL 0x8F

/// Register 43: SIST1: SCSI Interrupt Status 1
#define R_SIST1 0x43
#define R_SIST1_STO 0x04
#define R_SIST1_GEN 0x02
#define R_SIST1_HTH 0x01
#define SIST1_RC (m_chip.sist1_rc)
#define SIST1_FATAL (m_chip.sist1_fatal)

/// Register 46: MACNTL: Memory Access Control
#define R_MACNTL 0x46
#define MACNTL_MASK 0x0F

/// Register 47: GPCNTL: General Purpose Pin Control
#define R_GPCNTL 0x47

/// Register 48: STIME0: SCSI Timer 0
#define R_STIME0 0x48

/// Register 49: STIME1: SCSI Timer 1
#define R_STIME1 0x49
#define R_STIME1_GEN 0x0F
#define STIME1_MASK (m_chip.stime1_mask)

/// Register 4A: RESPID: SCSI Response ID
#define R_RESPID 0x4A

/// Register 4C: STEST0: SCSI Test 0
#define R_STEST0 0x4C

/// Register 4D: STEST1: SCSI Test 1
#define R_STEST1 0x4D
#define STEST1_MASK (m_chip.stest1_mask)

/// Register 4E: STEST2: SCSI Test 2
#define R_STEST2 0x4E
#define R_STEST2_SCE 0x80
#define R_STEST2_ROF 0x40
#define R_STEST2_SLB 0x10
#define R_STEST2_SZM 0x08
#define R_STEST2_EXT 0x02
#define R_STEST2_LOW 0x01
#define STEST2_MASK (m_chip.stest2_mask)

/// Register 4F: STEST3: SCSI Test 3
#define R_STEST3 0x4F
#define R_STEST3_TE 0x80
#define R_STEST3_STR 0x40
#define R_STEST3_HSC 0x20
#define R_STEST3_DSI 0x10
#define R_STEST3_TTM 0x04
#define R_STEST3_CSF 0x02
#define R_STEST3_STW 0x01
#define STEST3_MASK (m_chip.stest3_mask)

/// Register 50: SIDL
#define R_SIDL 0x50

/// Register 52: STEST4: SCSI Test 4 (Ultra2 parts)
#define R_STEST4 0x52

/// Register 54: SODL
#define R_SODL 0x54

/// Register 58: SBDL: SCSI Bus Data Lines
#define R_SBDL 0x58

/// Registers 5C..5F: SCRATCHB: Scratch Register B
#define R_SCRATCHB 0x5C

/// Acces an 8-byte register
#define R8(a) state.regs.reg8[R_##a]

/// Acces a 16-byte register
#define R16(a) state.regs.reg16[R_##a / 2]

/// Access a 32-byte register
#define R32(a) state.regs.reg32[R_##a / 4]

/**
 * Test bit in register
 *
 * \param a is the name of the register
 * \param b is the name of the bit
 **/
#define TB_R8(a, b) ((R8(a) & R_##a##_##b) == R_##a##_##b)

/**
 * Set bit in register.
 *
 * \param a is the name of the register
 * \param b is the name of the bit
 * \param c is the value for the bit
 **/
#define SB_R8(a, b, c) R8(a) = (R8(a) & ~R_##a##_##b) | (c ? R_##a##_##b : 0)

/**
 * Write to a register, using a mask
 *
 * \param a is the name of the register
 * \param b is the value to write.
 *
 * Only those bits that are set to 1 in <regname>_MASK will be changed.
 **/
#define WRM_R8(a, b) R8(a) = (R8(a) & ~a##_MASK) | ((b) & a##_MASK)

/**
 * Write to a register, using a mask, and using write-1-to-clear bits
 *
 * \param a is the name of the register
 * \param b is the value to write.
 *
 * Only those bits that are set to 1 in <regname>_MASK will be changed.
 * In addition, bits that are set to 1 in <regname>_W1C will be cleared
 * in the register if they are set to 1 in the value.
 **/
#define WRMW1C_R8(a, b)                                                        \
  R8(a) = (R8(a) & ~a##_MASK & ~a##_W1C) | ((b) & a##_MASK) |                  \
          (R8(a) & ~(b) & a##_W1C)

/**
 * Raise an interrupt
 *
 * \param a is the name of the interrupt register
 * \param b is the name of the bit to set
 **/
#define RAISE(a, b) set_interrupt(R_##a, R_##a##_##b)

/**
 * Clear read-to-clear-bits
 *
 * \param a is the name of the register
 *
 * Clear bits that are set to 1 in <regname>_RC
 **/
#define RDCLR_R8(a) R8(a) &= ~a##_RC

/**
 * Get the SCSI destination ID from the SDID register
 **/
#define GET_DEST() (R8(SDID) & R_SCID_ID)

/**
 * Set the SCSI destination ID in the SDID register
 **/
#define SET_DEST(a) R8(SDID) = (a) & R_SCID_ID

/**
 * Get the value of the DBC register (24-bits)
 **/
#define GET_DBC() (R32(DBC) & 0x00ffffff)

/**
 * Set the value of the DBC register (24-bits)
 **/
#define SET_DBC(a) R32(DBC) = (R32(DBC) & 0xff000000) | ((a) & 0x00ffffff)

#endif // !defined(INCLUDED_SYM53C8XX_REGS_H)
