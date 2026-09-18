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
 * QLogic ISP1020/1040 registers, mailbox commands and queue entries
 * (family units only). Names follow the QLogic firmware interface
 * specification, as the NetBSD, Linux and FreeBSD drivers use them.
 **/
#if !defined(INCLUDED_ISP1040REGS_H_)
#define INCLUDED_ISP1040REGS_H_

// Register map as a PCI card presents it: the bus interface unit at the
// bottom, the mailboxes at 0x70, and the RISC block at 0x80.
#define ISP_BIU_ID_LO 0x00
#define ISP_BIU_ID_HI 0x02
#define ISP_BIU_CONF0 0x04
#define ISP_BIU_CONF1 0x06
#define ISP_BIU_ICR 0x08  ///< interrupt control
#define ISP_BIU_ISR 0x0a  ///< interrupt status
#define ISP_BIU_SEMA 0x0c ///< semaphore: a mailbox result is waiting
#define ISP_BIU_NVRAM 0x0e
#define ISP_BIU_REQINP 0x10  ///< request queue in (written by the driver)
#define ISP_BIU_REQOUTP 0x12 ///< request queue out (written by us)
#define ISP_BIU_RSPINP 0x14  ///< response queue in (written by us)
#define ISP_BIU_RSPOUTP 0x16 ///< response queue out (written by the driver)
#define ISP_MBOX(n) (0x70 + (n) * 2)
#define ISP_MBOX_COUNT 8
#define ISP_HCCR 0xc0 ///< host command and control
#define ISP_REG_SIZE 0x100

// The identity the driver checks before it believes there is a card here.
#define ISP_BIU_ID_MAGIC_LO 0x4953 ///< "IS"
#define ISP_BIU_ID_MAGIC_HI 0x5000 ///< "P"

// ISP_BIU_ICR / ISP_BIU_ISR
#define ISP_ICR_SOFT_RESET 0x0001
#define ISP_ICR_ENABLE_ALL 0x0002
#define ISP_ICR_ENABLE_RISC 0x0004
#define ISP_ISR_IPEND 0x0002
#define ISP_ISR_RISC_INT 0x0004

// ISP_BIU_SEMA
#define ISP_SEMA_LOCK 0x0001
#define ISP_SEMA_STATUS 0x0002

// ISP_BIU_NVRAM: the serial EEPROM's pins
#define ISP_NVRAM_CLOCK 0x0001
#define ISP_NVRAM_CHIP_SELECT 0x0002
#define ISP_NVRAM_DATA_OUT 0x0004 ///< towards the EEPROM
#define ISP_NVRAM_DATA_IN 0x0008  ///< from the EEPROM
#define ISP_NVRAM_BYTES 128
#define ISP_NVRAM_ADDRESS_BITS 6 ///< a 93C46, addressed in words

// ISP_HCCR commands (the top nibble)
#define ISP_HCCR_CMD_MASK 0xf000
#define ISP_HCCR_CMD_NOP 0x0000
#define ISP_HCCR_CMD_RESET 0x1000
#define ISP_HCCR_CMD_PAUSE 0x2000
#define ISP_HCCR_CMD_RELEASE 0x3000
#define ISP_HCCR_CMD_STEP 0x4000
#define ISP_HCCR_CMD_SET_HOST_INT 0x5000
#define ISP_HCCR_CMD_CLEAR_HOST_INT 0x6000
#define ISP_HCCR_CMD_CLEAR_RISC_INT 0x7000
#define ISP_HCCR_CMD_BREAKPOINT 0x8000
#define ISP_HCCR_CMD_WRITE_BIAS 0xa000
#define ISP_HCCR_PAUSE 0x0020 ///< read back: the RISC is paused

// Mailbox commands the driver uses on this family.
#define ISP_MBOX_NO_OP 0x0000
#define ISP_MBOX_LOAD_RAM 0x0001
#define ISP_MBOX_EXEC_FIRMWARE 0x0002
#define ISP_MBOX_WRITE_RAM_WORD 0x0004
#define ISP_MBOX_READ_RAM_WORD 0x0005
#define ISP_MBOX_MAILBOX_REG_TEST 0x0006
#define ISP_MBOX_VERIFY_CHECKSUM 0x0007
#define ISP_MBOX_ABOUT_FIRMWARE 0x0008
#define ISP_MBOX_INIT_REQ_QUEUE 0x0010
#define ISP_MBOX_INIT_RES_QUEUE 0x0011
#define ISP_MBOX_STOP_FIRMWARE 0x0014
#define ISP_MBOX_ABORT 0x0015
#define ISP_MBOX_ABORT_DEVICE 0x0016
#define ISP_MBOX_ABORT_TARGET 0x0017
#define ISP_MBOX_BUS_RESET 0x0018
#define ISP_MBOX_STOP_QUEUE 0x0019
#define ISP_MBOX_START_QUEUE 0x001a
#define ISP_MBOX_GET_FIRMWARE_STATUS 0x001f
#define ISP_MBOX_GET_INIT_SCSI_ID 0x0020
#define ISP_MBOX_GET_SELECT_TIMEOUT 0x0021
#define ISP_MBOX_GET_RETRY_COUNT 0x0022
#define ISP_MBOX_GET_TAG_AGE_LIMIT 0x0023
#define ISP_MBOX_GET_CLOCK_RATE 0x0024
#define ISP_MBOX_GET_ACT_NEG_STATE 0x0025
#define ISP_MBOX_GET_ASYNC_DATA_SETUP 0x0026
#define ISP_MBOX_GET_PCI_PARAMS 0x0027
#define ISP_MBOX_GET_TARGET_PARAMS 0x0028
#define ISP_MBOX_GET_DEV_QUEUE_PARAMS 0x0029
#define ISP_MBOX_SET_INIT_SCSI_ID 0x0030
#define ISP_MBOX_SET_SELECT_TIMEOUT 0x0031
#define ISP_MBOX_SET_RETRY_COUNT 0x0032
#define ISP_MBOX_SET_TAG_AGE_LIMIT 0x0033
#define ISP_MBOX_SET_CLOCK_RATE 0x0034
#define ISP_MBOX_SET_ACT_NEG_STATE 0x0035
#define ISP_MBOX_SET_ASYNC_DATA_SETUP 0x0036
#define ISP_MBOX_SET_PCI_PARAMS 0x0037
#define ISP_MBOX_SET_TARGET_PARAMS 0x0038
#define ISP_MBOX_SET_DEV_QUEUE_PARAMS 0x0039
#define ISP_MBOX_SET_SYSTEM_PARAMETER 0x0045
#define ISP_MBOX_SET_FIRMWARE_FEATURES 0x004a
/// Issued at the end of initialisation by QLogic's own drivers (the
/// AlphaBIOS one and Windows' QL10WNT), always with mailbox 1 set to 1.
/// No public documentation names it, and the ES40 console never issues
/// it and works regardless -- but those drivers give up when it is
/// refused, so it is accepted. See Isp1040Mailbox.cpp.
#define ISP_MBOX_UNDOCUMENTED_5A 0x005a
#define ISP_MBOX_INIT_REQ_QUEUE_A64 0x0052
#define ISP_MBOX_INIT_RES_QUEUE_A64 0x0053

// What the firmware leaves in the mailboxes after a RISC reset: the part
// identifying itself, "ISP  ", and the interface version. Drivers check it
// to decide whether there is a working adapter here at all -- Windows'
// does, and refuses the device when it does not match.
#define ISP_PRODUCT_ID_1 0x4953 ///< "IS"
#define ISP_PRODUCT_ID_2 0x5020 ///< "P "
#define ISP_PRODUCT_ID_3 0x2020 ///< "  "
#define ISP_PRODUCT_ID_4 0x0001

// Mailbox 0 on completion.
#define ISP_MBOX_BUSY 0x0004
#define ISP_MBOX_COMMAND_COMPLETE 0x4000
#define ISP_MBOX_INVALID_COMMAND 0x4001
#define ISP_MBOX_HOST_INTERFACE_ERROR 0x4002
#define ISP_MBOX_COMMAND_PARAM_ERROR 0x4006

// Asynchronous events, also reported in mailbox 0.
#define ISP_ASYNC_BUS_RESET 0x8001
#define ISP_ASYNC_SYSTEM_ERROR 0x8002
#define ISP_ASYNC_RQS_XFER_ERR 0x8003
#define ISP_ASYNC_RSP_XFER_ERR 0x8004
#define ISP_ASYNC_TIMEOUT_RESET 0x8005

// Queue entries: a header, then the entry's own fields.
#define ISP_QUEUE_ENTRY_SIZE 64
#define ISP_ENTRY_TYPE_REQUEST 0x01  ///< a SCSI command, up to 4 segments
#define ISP_ENTRY_TYPE_DATASEG 0x02  ///< continuation: 7 more segments
#define ISP_ENTRY_TYPE_RESPONSE 0x03 ///< what we post back
#define ISP_ENTRY_TYPE_MARKER 0x04
#define ISP_ENTRY_TYPE_CMDONLY 0x05
#define ISP_ENTRY_TYPE_A64 0x09      ///< 64-bit addressing, 2 segments
#define ISP_ENTRY_TYPE_A64_CONT 0x0a ///< continuation: 5 more segments

// Request entry (type 1) field offsets.
#define ISP_REQ_HANDLE 0x04
#define ISP_REQ_LUN 0x08
#define ISP_REQ_TARGET 0x09
#define ISP_REQ_CDBLEN 0x0a
#define ISP_REQ_FLAGS 0x0c
#define ISP_REQ_TIMEOUT 0x10
#define ISP_REQ_SEG_COUNT 0x12
#define ISP_REQ_CDB 0x14
#define ISP_REQ_CDB_LEN 12
#define ISP_REQ_DATASEG 0x20 ///< four {address, count} pairs
#define ISP_REQ_SEGMENTS 4
// A continuation entry has a reserved word after its header, so its seven
// segments start at 8; the 64-bit form has none and starts at 4.
#define ISP_CONT_DATASEG 0x08 ///< seven pairs in a continuation entry
#define ISP_CONT_SEGMENTS 7
#define ISP_A64_DATASEG 0x20 ///< two {address, address high, count}
#define ISP_A64_SEGMENTS 2
#define ISP_A64_CONT_DATASEG 0x04
#define ISP_A64_CONT_SEGMENTS 5

// Direction, in the request's flags.
#define ISP_REQ_FLAG_DATA_IN 0x0040
#define ISP_REQ_FLAG_DATA_OUT 0x0080

// Status entry (type 3) field offsets.
#define ISP_RSP_HANDLE 0x04
#define ISP_RSP_SCSI_STATUS 0x08
#define ISP_RSP_COMPLETION 0x0a
#define ISP_RSP_STATE_FLAGS 0x0c
#define ISP_RSP_STATUS_FLAGS 0x0e
#define ISP_RSP_TIME 0x10
#define ISP_RSP_SENSE_LEN 0x12
#define ISP_RSP_RESID 0x14
#define ISP_RSP_SENSE 0x20
#define ISP_RSP_SENSE_MAX 32

// Completion status.
#define ISP_STATUS_COMPLETE 0x0000
#define ISP_STATUS_DMA_ERROR 0x0002
#define ISP_STATUS_RESET 0x0004
#define ISP_STATUS_ABORTED 0x0005
#define ISP_STATUS_TIMEOUT 0x0006
#define ISP_STATUS_DATA_OVERRUN 0x0007
#define ISP_STATUS_DATA_UNDERRUN 0x0015
#define ISP_STATUS_SELECTION_TIMEOUT 0x0010
#define ISP_STATUS_BUS_RESET 0x0017

// State flags, which drivers check to see how far a command got.
#define ISP_STATE_GOT_BUS 0x0100
#define ISP_STATE_GOT_TARGET 0x0200
#define ISP_STATE_SENT_CDB 0x0400
#define ISP_STATE_XFRD_DATA 0x0800
#define ISP_STATE_GOT_STATUS 0x1000
#define ISP_STATE_GOT_SENSE 0x2000

// Status flags.
#define ISP_STATUS_FLAG_SENSE_VALID 0x0200

#endif // !defined(INCLUDED_ISP1040REGS_H_)
