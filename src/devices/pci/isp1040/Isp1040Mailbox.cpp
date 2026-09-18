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
 * QLogic ISP10x0: the mailbox commands.
 *
 * The driver fills the mailbox registers and asks the RISC to read them;
 * the answer comes back in the same registers with the semaphore set and
 * an interrupt raised. Mailbox 0 carries the command going in and the
 * result coming out.
 *
 * Firmware loading is accepted and discarded: the emulation implements
 * what the firmware would have done, so there is nothing to load it into,
 * and no driver reads it back.
 **/
#include "Isp1040.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "Isp1040Regs.hpp"

#if defined(DEBUG_ISP)
#define TRACE_MBOX(...) printf(__VA_ARGS__)
#else
#define TRACE_MBOX(...)
#endif

/// Finish a mailbox command: the result in mailbox 0, the semaphore set,
/// and an interrupt for the driver waiting on it.
void CIsp1040::mailbox_done(u16 status) {
  state.mailbox_out[0] = status;
  state.sema = ISP_SEMA_LOCK;
  state.isr |= ISP_ISR_RISC_INT | ISP_ISR_IPEND;
  update_irq();
}

void CIsp1040::mailbox_command() {
  const u16 command = state.mailbox[0];

  // Unless a command says otherwise, it answers with the registers it was
  // given, so a driver reading them back sees its own values.
  memcpy(state.mailbox_out, state.mailbox, sizeof(state.mailbox_out));

  TRACE_MBOX("%s: mailbox %04x (%04x %04x %04x %04x)\n", devid_string, command,
             state.mailbox[1], state.mailbox[2], state.mailbox[3],
             state.mailbox[4]);

  switch (command) {
  case ISP_MBOX_NO_OP:
    break;

  case ISP_MBOX_LOAD_RAM:
  case ISP_MBOX_WRITE_RAM_WORD:
    // Firmware, accepted and discarded (see the file comment).
    break;

  case ISP_MBOX_READ_RAM_WORD:
    state.mailbox_out[2] = 0;
    break;

  case ISP_MBOX_MAILBOX_REG_TEST:
    // The driver writes patterns and expects them back, which the copy
    // above already did.
    break;

  case ISP_MBOX_VERIFY_CHECKSUM:
    // The firmware it did not load is sound.
    break;

  case ISP_MBOX_EXEC_FIRMWARE:
    state.firmware_running = true;
    state.risc_reset = false;
    break;

  case ISP_MBOX_STOP_FIRMWARE:
    state.firmware_running = false;
    break;

  case ISP_MBOX_ABOUT_FIRMWARE:
    state.mailbox_out[1] = m_chip.firmware_major;
    state.mailbox_out[2] = m_chip.firmware_minor;
    state.mailbox_out[3] = m_chip.firmware_micro;
    break;

  case ISP_MBOX_GET_FIRMWARE_STATUS:
    state.mailbox_out[1] = 0; // no commands outstanding
    break;

  case ISP_MBOX_INIT_REQ_QUEUE:
  case ISP_MBOX_INIT_REQ_QUEUE_A64:
    // Mailbox 1: entries, 2 and 3: the address, 4: where to start.
    state.request_length = state.mailbox[1];
    state.request_base = u32(state.mailbox[2]) << 16 | state.mailbox[3];
    state.request_in = state.mailbox[4];
    state.request_out = state.mailbox[4];
    break;

  case ISP_MBOX_INIT_RES_QUEUE:
  case ISP_MBOX_INIT_RES_QUEUE_A64:
    state.response_length = state.mailbox[1];
    state.response_base = u32(state.mailbox[2]) << 16 | state.mailbox[3];
    state.response_in = state.mailbox[5];
    state.response_out = state.mailbox[5];
    break;

  case ISP_MBOX_BUS_RESET:
    scsi_bus[0]->reset_bus();
    mailbox_done(ISP_MBOX_COMMAND_COMPLETE);
    raise_async(ISP_ASYNC_BUS_RESET);
    return;

  case ISP_MBOX_ABORT:
  case ISP_MBOX_ABORT_DEVICE:
  case ISP_MBOX_ABORT_TARGET:
  case ISP_MBOX_STOP_QUEUE:
  case ISP_MBOX_START_QUEUE:
    // Commands complete as they are issued here, so there is never
    // anything outstanding to abort or to hold back.
    break;

  case ISP_MBOX_SET_INIT_SCSI_ID:
    state.initiator_id = u8(state.mailbox[1] & 0x0f);
    break;

  case ISP_MBOX_GET_INIT_SCSI_ID:
    state.mailbox_out[1] = state.initiator_id;
    break;

  case ISP_MBOX_GET_CLOCK_RATE:
    state.mailbox_out[1] = m_chip.ultra ? 60 : 40; // MHz
    break;

  case ISP_MBOX_GET_SELECT_TIMEOUT:
    state.mailbox_out[1] = 250;
    break;

  case ISP_MBOX_GET_RETRY_COUNT:
    state.mailbox_out[1] = 4;
    state.mailbox_out[2] = 5;
    break;

  case ISP_MBOX_GET_TAG_AGE_LIMIT:
    state.mailbox_out[1] = 8;
    break;

  case ISP_MBOX_GET_ACT_NEG_STATE:
    state.mailbox_out[1] = 0;
    break;

  case ISP_MBOX_GET_ASYNC_DATA_SETUP:
    state.mailbox_out[1] = 6;
    break;

  case ISP_MBOX_GET_PCI_PARAMS:
    state.mailbox_out[1] = state.conf1;
    break;

  case ISP_MBOX_GET_TARGET_PARAMS:
    // Everything this part can do, for every target: synchronous transfer
    // at the part's rate, wide where it is wide.
    state.mailbox_out[2] = u16(0x00c0 | (m_chip.wide ? 0x0020 : 0));
    state.mailbox_out[3] = u16((0x0f << 8) | (m_chip.ultra ? 0x0c : 0x19));
    break;

  case ISP_MBOX_GET_DEV_QUEUE_PARAMS:
    state.mailbox_out[2] = 32; // queue depth
    state.mailbox_out[3] = 0;
    break;

  case ISP_MBOX_SET_SELECT_TIMEOUT:
  case ISP_MBOX_SET_RETRY_COUNT:
  case ISP_MBOX_SET_TAG_AGE_LIMIT:
  case ISP_MBOX_SET_CLOCK_RATE:
  case ISP_MBOX_SET_ACT_NEG_STATE:
  case ISP_MBOX_SET_ASYNC_DATA_SETUP:
  case ISP_MBOX_SET_PCI_PARAMS:
  case ISP_MBOX_SET_TARGET_PARAMS:
  case ISP_MBOX_SET_DEV_QUEUE_PARAMS:
  case ISP_MBOX_SET_SYSTEM_PARAMETER:
  case ISP_MBOX_SET_FIRMWARE_FEATURES:
  case ISP_MBOX_UNDOCUMENTED_5A:
    // Timing and queueing parameters: accepted. What they describe --
    // transfer rates, retries, tag ages -- has no counterpart here, where
    // a transfer takes no time on a bus that never disconnects.
    //
    // The last of these (0x5a) is accepted on weaker grounds: it is not in
    // any documentation or open-source driver, but QLogic's own drivers
    // end their initialisation with it and give up if it is refused, and
    // the ES40 console never issues it and works. So it is taken to be a
    // parameter this emulation has no counterpart for. If it turns out to
    // mean something that must be done, this is where it goes.
    break;

  default:
    printf("%s: unsupported mailbox command %04x\n", devid_string, command);
    mailbox_done(ISP_MBOX_INVALID_COMMAND);
    return;
  }

  mailbox_done(ISP_MBOX_COMMAND_COMPLETE);
}
