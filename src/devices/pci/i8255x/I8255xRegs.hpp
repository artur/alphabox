/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
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
 */

/**
 * \file
 * Intel 8255x register, command block and frame descriptor definitions
 * (family units only). Names follow the Intel 8255x 10/100 Mbps Ethernet
 * Controller Family Open Source Software Developer Manual.
 **/
#if !defined(INCLUDED_I8255XREGS_H_)
#define INCLUDED_I8255XREGS_H_

// System Control Block (SCB) and the other CSRs, byte offsets.
#define SCB_STATUS 0x00   // CU and RU status
#define SCB_STATACK 0x01  // interrupt causes, write 1 to acknowledge
#define SCB_COMMAND 0x02  // RU (3..0) and CU (7..4) commands
#define SCB_INTMASK 0x03  // M, SI, and the per-cause masks (82558+)
#define SCB_POINTER 0x04  // general pointer (32 bits)
#define CSR_PORT 0x08     // PORT interface (32 bits)
#define CSR_FLASH 0x0c    // flash control (16 bits)
#define CSR_EEPROM 0x0e   // EEPROM control (16 bits)
#define CSR_MDI 0x10      // MDI control (32 bits)
#define CSR_EARLY_RX 0x14 // early receive byte count (32 bits)
#define CSR_PMDR 0x1b     // power management driver register (82558+)
#define CSR_GCTRL 0x1c    // general control (82558+)
#define CSR_GSTAT 0x1d    // general status (82558+)
#define CSR_SIZE 0x20     // I/O BAR size; the memory BAR mirrors it

// SCB_STATUS
#define SCB_CUS_SHIFT 6
#define SCB_CUS_MASK 0xc0
#define SCB_RUS_SHIFT 2
#define SCB_RUS_MASK 0x3c

enum i8255x_cu_state { CU_IDLE = 0, CU_SUSPENDED = 1, CU_ACTIVE = 2 };

enum i8255x_ru_state {
  RU_IDLE = 0,
  RU_SUSPENDED = 1,
  RU_NO_RESOURCES = 2,
  RU_READY = 4,
  RU_SUSPENDED_NO_RBDS = 9,
  RU_NO_RESOURCES_NO_RBDS = 10,
  RU_READY_NO_RBDS = 12,
};

// SCB_STATACK
#define STAT_FCP 0x01 // flow control pause (82558+)
#define STAT_ER 0x02  // early receive (82558+)
#define STAT_SWI 0x04 // software interrupt
#define STAT_MDI 0x08 // MDI read/write cycle done
#define STAT_RNR 0x10 // RU left the ready state
#define STAT_CNA 0x20 // CU left the active state
#define STAT_FR 0x40  // frame received
#define STAT_CX 0x80  // command with I bit completed (CX/TNO)

// SCB_INTMASK
#define INTMASK_M 0x01  // mask every interrupt
#define INTMASK_SI 0x02 // request a software interrupt
#define INTMASK_FCP 0x04
#define INTMASK_ER 0x08
#define INTMASK_RNR 0x10
#define INTMASK_CNA 0x20
#define INTMASK_FR 0x40
#define INTMASK_CX 0x80

// SCB_COMMAND, low nibble: RU commands
#define RUC_MASK 0x07
#define RUC_NOP 0
#define RUC_START 1
#define RUC_RESUME 2
#define RUC_DMA_REDIRECT 3
#define RUC_ABORT 4
#define RUC_LOAD_HDS 5
#define RUC_LOAD_BASE 6
#define RUC_RBD_RESUME 7

// SCB_COMMAND, high nibble: CU commands
#define CUC_MASK 0xf0
#define CUC_NOP 0x00
#define CUC_START 0x10
#define CUC_RESUME 0x20
#define CUC_HQ_START 0x30 // 82558+: unused by the drivers we know
#define CUC_LOAD_DUMP_ADDR 0x40
#define CUC_DUMP_STATS 0x50
#define CUC_LOAD_BASE 0x60
#define CUC_DUMP_RESET_STATS 0x70
#define CUC_STATIC_RESUME 0xa0
#define CUC_HQ_RESUME 0xb0

// CSR_PORT, selection in the low two bits, an address above them
#define PORT_SOFTWARE_RESET 0
#define PORT_SELF_TEST 1
#define PORT_SELECTIVE_RESET 2
#define PORT_DUMP 3

// CSR_EEPROM
#define EEPROM_SK 0x01
#define EEPROM_CS 0x02
#define EEPROM_DI 0x04
#define EEPROM_DO 0x08

// CSR_MDI
#define MDI_DATA_MASK 0x0000ffff
#define MDI_REG_SHIFT 16
#define MDI_PHY_SHIFT 21
#define MDI_OP_SHIFT 26
#define MDI_OP_WRITE 1
#define MDI_OP_READ 2
#define MDI_READY 0x10000000
#define MDI_IE 0x20000000

// CSR_GSTAT
#define GSTAT_LINK 0x01
#define GSTAT_100 0x02
#define GSTAT_FDX 0x04

// Action command block: status, command, link, parameters.
#define CB_STATUS 0x00
#define CB_COMMAND 0x02
#define CB_LINK 0x04
#define CB_PARAM 0x08

#define CB_STATUS_C 0x8000  // complete
#define CB_STATUS_B 0x4000  // busy (never seen: commands finish at once)
#define CB_STATUS_OK 0x2000 // completed without error
#define CB_STATUS_U 0x1000  // transmit underrun

#define CB_CMD_EL 0x8000 // end of list: CU goes idle
#define CB_CMD_S 0x4000  // suspend: CU goes suspended
#define CB_CMD_I 0x2000  // interrupt (CX) on completion
#define CB_CMD_NC 0x0010 // transmit: no CRC/source address insertion
#define CB_CMD_SF 0x0008 // transmit: flexible mode
#define CB_CMD_OP 0x0007

enum i8255x_cb_op {
  CB_NOP = 0,
  CB_IA_SETUP = 1,
  CB_CONFIGURE = 2,
  CB_MC_SETUP = 3,
  CB_TRANSMIT = 4,
  CB_TDR = 5, // 82557; microcode load on the 82558 and later
  CB_DUMP = 6,
  CB_DIAGNOSE = 7,
};

// Transmit command block, after the common header.
#define TCB_TBD_ARRAY 0x08
#define TCB_BYTE_COUNT 0x0c // 14 bits; bit 15 EOF
#define TCB_THRESHOLD 0x0e
#define TCB_TBD_COUNT 0x0f
#define TCB_DATA 0x10 // simplified data, or the extended TBDs
#define TCB_COUNT_MASK 0x3fff

// Transmit buffer descriptor: address, then 15-bit size with bit 16 EL.
#define TBD_SIZE 8
#define TBD_SIZE_MASK 0x7fff
#define TBD_EL 0x00010000

// Receive frame descriptor.
#define RFD_STATUS 0x00
#define RFD_COMMAND 0x02
#define RFD_LINK 0x04
#define RFD_RBD 0x08
#define RFD_COUNT 0x0c
#define RFD_SIZE 0x0e
#define RFD_DATA 0x10

#define RFD_STATUS_C 0x8000
#define RFD_STATUS_OK 0x2000
#define RFD_STATUS_CRC 0x0800
#define RFD_STATUS_ALIGN 0x0400
#define RFD_STATUS_NO_RESOURCES 0x0200 // frame did not fit the buffers
#define RFD_STATUS_OVERRUN 0x0100
#define RFD_STATUS_SHORT 0x0080
#define RFD_STATUS_TYPE 0x0020 // type field, not a length
#define RFD_STATUS_RX_ERROR 0x0010
#define RFD_STATUS_NO_MATCH 0x0004 // matched no address (promiscuous)
#define RFD_STATUS_IA_MISMATCH 0x0002

#define RFD_CMD_EL 0x8000
#define RFD_CMD_S 0x4000
#define RFD_CMD_H 0x0010
#define RFD_CMD_SF 0x0008 // flexible: data beyond RFD_SIZE goes to RBDs

// Actual count words (RFD and RBD): 14-bit count, F and EOF.
#define COUNT_EOF 0x8000
#define COUNT_F 0x4000
#define COUNT_MASK 0x3fff

// Receive buffer descriptor.
#define RBD_COUNT 0x00
#define RBD_LINK 0x04
#define RBD_BUFFER 0x08
#define RBD_SIZE 0x0c // 14 bits; bit 15 EL
#define RBD_EL 0x8000

// Configure command bytes that the emulation looks at.
#define CFG_BYTES 22
#define CFG0_COUNT_MASK 0x3f
#define CFG6_TCO_STATS 0x04  // 82559: TCO statistics
#define CFG6_CI_INT 0x08     // CNA only when the CU goes idle
#define CFG6_STD_TCB 0x10    // 82558+: 1 = standard TCB, 0 = extended
#define CFG6_STD_STATS 0x20  // 82558+: 1 = the 82557 counter set
#define CFG8_CSMA_DIS 0x80   // 82558+: no carrier sense: nothing received
#define CFG10_NSAI 0x08      // no source address insertion
#define CFG10_LOOPBACK 0xc0  // 00 normal, 01 internal, 1x external
#define CFG15_PROMISC 0x01   // receive everything
#define CFG15_BCAST_DIS 0x02 // refuse broadcasts
#define CFG18_PADDING 0x02   // pad short transmit frames
#define CFG18_RX_CRC 0x04    // hand the CRC over with received frames
#define CFG18_LONG_RX 0x08   // 82558+: accept frames longer than 1518
#define CFG20_MULTI_IA 0x40  // hash filter applies to unicast too
#define CFG21_MC_ALL 0x08    // receive every multicast frame

// Statistical counters, offsets into the dump area.
#define STAT_TX_GOOD 0
#define STAT_RX_GOOD 36
#define STAT_RX_RESOURCE 48
#define STAT_RX_SHORT 60
#define STATS_WORDS 20 // counters (82557: 16, 82558: 19) plus TCO

// PCI identity
#define PCI_VENDOR_INTEL 0x8086
#define PCI_VENDOR_COMPAQ 0x0e11

#endif // !defined(INCLUDED_I8255XREGS_H_)
