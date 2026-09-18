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
 * QLogic ISP10x0: the request and response queues, and running a queue
 * entry against a SCSI target.
 *
 * Both queues are rings of 64-byte entries in host memory. The driver
 * writes commands at its producer index and tells the adapter; the
 * adapter answers with a status entry in the response queue and raises an
 * interrupt. A command entry carries the target, the command block and up
 * to four buffer segments, with more segments in continuation entries
 * that follow it.
 *
 * A command is run to completion here, as the adapter's firmware would:
 * select the target, hand over the command block, move the data and
 * collect the status. Nothing disconnects, so there is no reselection to
 * model.
 *
 * The parts with two SCSI buses have one pair of queues all the same:
 * there is one RISC serving both buses, and a command says which bus it is
 * for in the top bit of its target byte.
 **/
#include "Isp1040.hpp"
#include "SCSIBus.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

#include "Isp1040Regs.hpp"

#include <algorithm>

#if defined(DEBUG_ISP)
#define TRACE_QUEUE(...) printf(__VA_ARGS__)
#else
#define TRACE_QUEUE(...)
#endif

/// Entries run before the rest is left to the thread, so a driver that
/// queues a great many commands never waits inside one register write.
static const int ISP_QUEUE_SLICE = 32;

/// The driver reads the queue pointers from mailboxes 4 and 5, so they
/// follow every move of the queues.
static void publish(u16 *mailbox_out, u16 request_out, u16 response_in) {
  mailbox_out[4] = request_out;
  mailbox_out[5] = response_in;
}

void CIsp1040::run_request_queue() {
  state.queue_pending = false;

  for (int n = 0; n < ISP_QUEUE_SLICE; n++) {
    if (!state.request_length || state.request_out == state.request_in)
      return; // nothing queued

    const u32 address =
        state.request_base + u32(state.request_out) * ISP_QUEUE_ENTRY_SIZE;
    u8 entry[ISP_QUEUE_ENTRY_SIZE];
    do_pci_read(address, entry, 1, sizeof(entry));

    // Whatever the entry turns out to be, it has been taken.
    state.request_out = u16((state.request_out + (entry[1] ? entry[1] : 1)) %
                            state.request_length);
    publish(state.mailbox_out, state.request_out, state.response_in);
    execute_entry(address, entry);
  }

  state.queue_pending = state.request_out != state.request_in;
  if (state.queue_pending)
    myWake.notify_all();
}

/**
 * Collect the buffer segments of a command: those in the entry itself,
 * then those in the continuation entries that follow it. Returns how many
 * were found.
 **/
int CIsp1040::gather_segments(u32 entry_address, const u8 *entry, u32 *address,
                              u32 *count, int max_segments) {
  const bool a64 = entry[0] == ISP_ENTRY_TYPE_A64;
  const int in_entry = a64 ? ISP_A64_SEGMENTS : ISP_REQ_SEGMENTS;
  const int stride = a64 ? 12 : 8;
  const int base = a64 ? ISP_A64_DATASEG : ISP_REQ_DATASEG;
  const int wanted = dma_read16(entry_address + ISP_REQ_SEG_COUNT);
  int found = 0;

  for (int i = 0; i < in_entry && found < wanted && found < max_segments; i++) {
    const int at = base + i * stride;
    address[found] = u32(entry[at]) | u32(entry[at + 1]) << 8 |
                     u32(entry[at + 2]) << 16 | u32(entry[at + 3]) << 24;
    const int c = a64 ? at + 8 : at + 4;
    count[found] = u32(entry[c]) | u32(entry[c + 1]) << 8 |
                   u32(entry[c + 2]) << 16 | u32(entry[c + 3]) << 24;
    found++;
  }

  // Continuation entries hold the rest. They follow the command entry in
  // the queue, one slot each -- which is why the command's own slot is
  // where the walk starts, not where the queue has been consumed to.
  u16 slot =
      u16((((entry_address - state.request_base) / ISP_QUEUE_ENTRY_SIZE) + 1) %
          state.request_length);
  while (found < wanted && found < max_segments) {
    const u32 at = state.request_base + u32(slot) * ISP_QUEUE_ENTRY_SIZE;
    u8 cont[ISP_QUEUE_ENTRY_SIZE];
    do_pci_read(at, cont, 1, sizeof(cont));
    if (cont[0] != ISP_ENTRY_TYPE_DATASEG && cont[0] != ISP_ENTRY_TYPE_A64_CONT)
      break;

    const bool c64 = cont[0] == ISP_ENTRY_TYPE_A64_CONT;
    const int n = c64 ? ISP_A64_CONT_SEGMENTS : ISP_CONT_SEGMENTS;
    const int cstride = c64 ? 12 : 8;
    const int cbase = c64 ? ISP_A64_CONT_DATASEG : ISP_CONT_DATASEG;
    for (int i = 0; i < n && found < wanted && found < max_segments; i++) {
      const int a = cbase + i * cstride;
      address[found] = u32(cont[a]) | u32(cont[a + 1]) << 8 |
                       u32(cont[a + 2]) << 16 | u32(cont[a + 3]) << 24;
      const int c = c64 ? a + 8 : a + 4;
      count[found] = u32(cont[c]) | u32(cont[c + 1]) << 8 |
                     u32(cont[c + 2]) << 16 | u32(cont[c + 3]) << 24;
      found++;
    }
    slot = u16((slot + 1) % state.request_length);
  }

  return found;
}

/**
 * Run one queue entry. Commands are executed in place; anything else is
 * acknowledged.
 **/
bool CIsp1040::execute_entry(u32 entry_address, u8 *entry) {
  const u8 type = entry[0];

  if (type == ISP_ENTRY_TYPE_MARKER || type == ISP_ENTRY_TYPE_DATASEG ||
      type == ISP_ENTRY_TYPE_A64_CONT)
    return true; // a marker, or segments belonging to the entry before

  if (type != ISP_ENTRY_TYPE_REQUEST && type != ISP_ENTRY_TYPE_CMDONLY &&
      type != ISP_ENTRY_TYPE_A64) {
    printf("%s: unsupported queue entry type %02x\n", devid_string, type);
    post_response(entry, ISP_STATUS_DMA_ERROR, 0, 0, 0, nullptr, 0);
    return true;
  }

  // On a part with two of them, the top bit of the target byte is the bus.
  const int bus =
      m_chip.buses > 1 && (entry[ISP_REQ_TARGET] & ISP_REQ_TARGET_BUS) ? 1 : 0;
  const int target = m_chip.buses > 1
                         ? entry[ISP_REQ_TARGET] & ISP_REQ_TARGET_ID
                         : entry[ISP_REQ_TARGET];
  const int lun = entry[ISP_REQ_LUN];
  const u16 flags = dma_read16(entry_address + ISP_REQ_FLAGS);
  int cdb_length = dma_read16(entry_address + ISP_REQ_CDBLEN);
  if (cdb_length <= 0 || cdb_length > ISP_REQ_CDB_LEN)
    cdb_length = ISP_REQ_CDB_LEN;

  u32 seg_address[64];
  u32 seg_count[64];
  const int segments =
      gather_segments(entry_address, entry, seg_address, seg_count, 64);
  u32 wanted = 0;
  for (int i = 0; i < segments; i++)
    wanted += seg_count[i];

  TRACE_QUEUE("%s: command %02x to %d:%d.%d, type %02x count %d, %d segments, "
              "%u bytes\n",
              devid_string, entry[ISP_REQ_CDB], bus, target, lun, type,
              entry[1], segments, wanted);
#if defined(DEBUG_ISP)
  for (int i = 0; i < segments; i++)
    printf("    segment %d: %08x + %u\n", i, seg_address[i], seg_count[i]);
#endif

  if (!scsi_arbitrate(bus)) {
    post_response(entry, ISP_STATUS_BUS_RESET, 0, 0, wanted, nullptr, 0);
    return true;
  }
  if (!scsi_select(bus, target)) {
    scsi_free(bus);
    post_response(entry, ISP_STATUS_SELECTION_TIMEOUT, 0, ISP_STATE_GOT_BUS,
                  wanted, nullptr, 0);
    return true;
  }

  u16 state_flags = ISP_STATE_GOT_BUS | ISP_STATE_GOT_TARGET;
  u16 scsi_status = 0;
  u16 completion = ISP_STATUS_COMPLETE;
  u32 moved = 0;
  u8 sense[ISP_RSP_SENSE_MAX];
  int sense_length = 0;
  int segment = 0;
  u32 offset_in_segment = 0;

  for (bool done = false; !done;) {
    switch (scsi_get_phase(bus)) {
    case SCSI_PHASE_COMMAND: {
      u8 *p = (u8 *)scsi_xfer_ptr(bus, cdb_length);
      // The command block, with the logical unit the entry names.
      memcpy(p, entry + ISP_REQ_CDB, cdb_length);
      p[1] = u8((p[1] & 0x1f) | ((lun & 7) << 5));
      scsi_xfer_done(bus);
      state_flags |= ISP_STATE_SENT_CDB;
      break;
    }

    case SCSI_PHASE_DATA_IN:
    case SCSI_PHASE_DATA_OUT: {
      const bool in = scsi_get_phase(bus) == SCSI_PHASE_DATA_IN;
      size_t chunk = scsi_expected_xfer(bus);
      u8 *p = (u8 *)scsi_xfer_ptr(bus, chunk);

      // Spread the transfer over the entry's segments, in order.
      size_t left = chunk;
      while (left && segment < segments) {
        const u32 room = seg_count[segment] - offset_in_segment;
        const size_t now = std::min<size_t>(left, room);
        const u32 at = seg_address[segment] + offset_in_segment;
        if (in)
          do_pci_write(at, p, 1, now);
        else
          do_pci_read(at, p, 1, now);
        p += now;
        left -= now;
        moved += (u32)now;
        offset_in_segment += (u32)now;
        if (offset_in_segment == seg_count[segment]) {
          segment++;
          offset_in_segment = 0;
        }
      }
      if (left) {
        // The target offered more than the entry has room for.
        completion = in ? ISP_STATUS_DATA_OVERRUN : ISP_STATUS_COMPLETE;
        memset(p, 0, left);
      }
      scsi_xfer_done(bus);
      state_flags |= ISP_STATE_XFRD_DATA;
      break;
    }

    case SCSI_PHASE_STATUS: {
      u8 *p = (u8 *)scsi_xfer_ptr(bus, 1);
      scsi_status = *p;
      scsi_xfer_done(bus);
      state_flags |= ISP_STATE_GOT_STATUS;
      break;
    }

    case SCSI_PHASE_MSG_IN: {
      size_t n = scsi_expected_xfer(bus);
      u8 *p = (u8 *)scsi_xfer_ptr(bus, n);
      const bool complete = n && p[0] == 0x00; // command complete
      scsi_xfer_done(bus);
      if (complete)
        done = true;
      break;
    }

    case SCSI_PHASE_MSG_OUT: {
      // Identify, and nothing more: which logical unit the command is
      // for. The target offers room for a whole message stream, so ask
      // for one byte -- anything more is read as further messages.
      // Disconnection is not offered, because nothing here disconnects.
      u8 *p = (u8 *)scsi_xfer_ptr(bus, 1);
      p[0] = u8(0x80 | (lun & 7));
      scsi_xfer_done(bus);
      break;
    }

    default:
      done = true;
      break;
    }
  }

  scsi_free(bus);

  if (wanted > moved && completion == ISP_STATUS_COMPLETE)
    completion = ISP_STATUS_DATA_UNDERRUN;

  post_response(entry, completion, scsi_status, state_flags, wanted - moved,
                sense_length ? sense : nullptr, sense_length);
  return true;
}

/**
 * Answer a command with a status entry in the response queue, and tell
 * the driver there is something there.
 **/
void CIsp1040::post_response(const u8 *request, u16 completion, u16 scsi_status,
                             u16 state_flags, u32 residual, const u8 *sense,
                             int sense_length) {
  if (!state.response_length)
    return;

  const u32 at =
      state.response_base + u32(state.response_in) * ISP_QUEUE_ENTRY_SIZE;
  u8 entry[ISP_QUEUE_ENTRY_SIZE];
  memset(entry, 0, sizeof(entry));

  entry[0] = ISP_ENTRY_TYPE_RESPONSE;
  entry[1] = 1; // one entry
  memcpy(entry + ISP_RSP_HANDLE, request + ISP_REQ_HANDLE, 4);
  entry[ISP_RSP_SCSI_STATUS] = u8(scsi_status);
  entry[ISP_RSP_COMPLETION] = u8(completion);
  entry[ISP_RSP_COMPLETION + 1] = u8(completion >> 8);
  entry[ISP_RSP_STATE_FLAGS] = u8(state_flags);
  entry[ISP_RSP_STATE_FLAGS + 1] = u8(state_flags >> 8);
  if (sense && sense_length) {
    const int n = std::min(sense_length, ISP_RSP_SENSE_MAX);
    entry[ISP_RSP_STATUS_FLAGS] = u8(ISP_STATUS_FLAG_SENSE_VALID);
    entry[ISP_RSP_STATUS_FLAGS + 1] = u8(ISP_STATUS_FLAG_SENSE_VALID >> 8);
    entry[ISP_RSP_SENSE_LEN] = u8(n);
    memcpy(entry + ISP_RSP_SENSE, sense, n);
    state_flags |= ISP_STATE_GOT_SENSE;
  }
  entry[ISP_RSP_RESID] = u8(residual);
  entry[ISP_RSP_RESID + 1] = u8(residual >> 8);
  entry[ISP_RSP_RESID + 2] = u8(residual >> 16);
  entry[ISP_RSP_RESID + 3] = u8(residual >> 24);

  do_pci_write(at, entry, 1, sizeof(entry));
  state.response_in = u16((state.response_in + 1) % state.response_length);
  publish(state.mailbox_out, state.request_out, state.response_in);

  // A completion carries no mailbox result, so the semaphore stays clear:
  // that is how the driver tells the two apart.
  state.isr |= ISP_ISR_RISC_INT | ISP_ISR_IPEND;
  update_irq();
}
