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
 * Intel 8255x family: the command unit (CU).
 *
 * The CU walks a linked list of action command blocks, each completing
 * before the next starts. Commands finish at once, so a start or resume
 * runs the list inline until it ends (EL: idle) or pauses (S: suspended).
 * A list that does neither -- a ring the driver keeps appending to -- is
 * run in bounded slices, the rest continuing on the device thread, so a
 * guest can never wedge the CPU thread inside a write.
 *
 * Structure addresses the CU is given -- links, the TBD array, the counter
 * dump area -- are offsets from the CU base; data buffer addresses in the
 * TBDs are absolute. The ES40 console depends on both: it loads the base
 * with its PCI DMA window, 0x80000000, and writes offsets from it, but its
 * TBDs hold full bus addresses.
 **/
#include "I8255x.hpp"
#include "StdAfx.hpp"

#include "I8255xRegs.hpp"

#include <algorithm>

#if defined(DEBUG_NIC)
#define TRACE_CU(...) printf(__VA_ARGS__)
#else
#define TRACE_CU(...)
#endif

static const int CU_SLICE = 64;       // commands per inline slice
static const int TX_FRAME_MAX = 2600; // longest frame the chip will send
static const int ETH_MIN_FRAME = 60;  // without CRC
static const int MC_SETUP_MAX_BYTES = 6 * 64;

void CI8255x::cu_start(u32 offset) {
  if (cu_state() == CU_ACTIVE)
    printf("%s: CU start while active\n", m_chip.name);
  state.cu_offset = offset;
  set_cu_state(CU_ACTIVE);
  run_command_list();
}

/**
 * Resume continues after the block that suspended. Resuming an idle CU is
 * accepted too: the 82557 does, and old Linux eepro100 drivers rely on it.
 **/
void CI8255x::cu_resume() {
  if (cu_state() == CU_ACTIVE)
    return;
  set_cu_state(CU_ACTIVE);
  run_command_list();
}

void CI8255x::run_command_list() {
  state.cu_pending = false;
  for (int n = 0; n < CU_SLICE; n++) {
    if (cu_state() != CU_ACTIVE)
      return;
    const u32 cb = state.cu_base + state.cu_offset;
    if (!action_command(cb))
      return;
  }
  // Still going: continue on the device thread.
  state.cu_pending = true;
  myWake.notify_all();
}

/**
 * Execute the block at `cb`; false when the CU left the active state.
 **/
bool CI8255x::action_command(u32 cb) {
  const u16 command = dma_read16(cb + CB_COMMAND);
  const u32 link = dma_read32(cb + CB_LINK);
  const u16 status = CB_STATUS_OK;

  TRACE_CU("%s: CB %08x cmd %04x link %08x\n", m_chip.name, cb, command, link);

  switch (command & CB_CMD_OP) {
  case CB_NOP:
    break;

  case CB_IA_SETUP:
    do_pci_read(cb + CB_PARAM, state.ia, 1, 6);
    update_host_filter();
    break;

  case CB_CONFIGURE: {
    u8 bytes[CFG_BYTES];
    do_pci_read(cb + CB_PARAM, bytes, 1, CFG_BYTES);
    int count = bytes[0] & CFG0_COUNT_MASK;
    if (count < 8)
      count = 8;
    if (count > CFG_BYTES)
      count = CFG_BYTES;
    // Bytes beyond the count keep their values.
    memcpy(state.config, bytes, count);
    TRACE_CU("%s: configure %d:", m_chip.name, count);
    for (int i = 0; i < CFG_BYTES; i++)
      TRACE_CU(" %02x", state.config[i]);
    TRACE_CU("\n");
    update_host_filter();
    break;
  }

  case CB_MC_SETUP:
    setup_multicast(cb);
    update_host_filter();
    break;

  case CB_TRANSMIT:
    transmit(cb, command);
    break;

  case CB_TDR:
    // 82557: time-domain reflectometry, reporting a good link. Later
    // parts load microcode here, which the emulation does not need.
    if (m_chip.generation < 8)
      dma_write16(cb + CB_PARAM, 0x8000);
    break;

  case CB_DUMP:
  case CB_DIAGNOSE:
    // Nothing to report: the diagnose passes (F clear).
    break;
  }

  dma_write16(cb + CB_STATUS, CB_STATUS_C | status);
  state.cu_offset = link;

  u8 causes = 0;
  if (command & CB_CMD_I)
    causes |= STAT_CX;
  bool active = true;
  if (command & CB_CMD_EL) {
    set_cu_state(CU_IDLE);
    causes |= STAT_CNA;
    active = false;
  } else if (command & CB_CMD_S) {
    set_cu_state(CU_SUSPENDED);
    if (!(state.config[6] & CFG6_CI_INT))
      causes |= STAT_CNA;
    active = false;
  }
  if (causes)
    raise(causes);
  return active;
}

bool CI8255x::extended_tcb() const {
  return m_chip.generation >= 8 && !(state.config[6] & CFG6_STD_TCB);
}

/**
 * Transmit: gather the frame (simplified: data in the TCB; flexible: TCB
 * data, then the TBDs -- on the 82558 and later with extended TCBs, the
 * first two TBDs live in the TCB and the array holds the rest), insert
 * the source address unless told not to, pad, and send it -- or, in
 * loopback, hand it straight to the receive unit.
 **/
void CI8255x::transmit(u32 cb, u16 command) {
  u8 frame[TX_FRAME_MAX];
  int len = 0;

  const u32 tbd_array = state.cu_base + dma_read32(cb + TCB_TBD_ARRAY);
  const int tcb_bytes = dma_read16(cb + TCB_BYTE_COUNT) & TCB_COUNT_MASK;
  const int tbd_count = dma_read8(cb + TCB_TBD_COUNT);
  const bool flexible = (command & CB_CMD_SF) != 0;
  TRACE_CU("%s: TCB tbd_array %08x bytes %d tbds %d %s\n", m_chip.name,
           tbd_array, tcb_bytes, tbd_count,
           !flexible        ? "simplified"
           : extended_tcb() ? "extended"
                            : "flexible");

  auto append = [&](u32 address, int size) {
    size = std::min(size, TX_FRAME_MAX - len);
    if (size > 0) {
      do_pci_read(address, frame + len, 1, size);
      len += size;
    }
  };
  auto append_tbd = [&](u32 tbd) {
    const u32 size = dma_read32(tbd + 4);
    const u32 buffer = dma_read32(tbd);
    TRACE_CU("%s: TBD %08x: %08x size %08x\n", m_chip.name, tbd, buffer, size);
    append(buffer, int(size & TBD_SIZE_MASK));
    return (size & TBD_EL) != 0;
  };

  if (!flexible) {
    append(cb + TCB_DATA, tcb_bytes);
  } else if (extended_tcb()) {
    int n = 0;
    bool last = false;
    for (; n < tbd_count && n < 2 && !last; n++)
      last = append_tbd(cb + TCB_DATA + n * TBD_SIZE);
    for (int i = 0; n < tbd_count && !last; n++, i++)
      last = append_tbd(tbd_array + i * TBD_SIZE);
  } else {
    append(cb + TCB_DATA, tcb_bytes);
    bool last = false;
    for (int i = 0; i < tbd_count && !last; i++)
      last = append_tbd(tbd_array + i * TBD_SIZE);
  }

  if (command & CB_CMD_NC) {
    len = std::max(0, len - 4); // the driver supplied the CRC
  } else if (!(state.config[10] & CFG10_NSAI) && len >= 12) {
    memcpy(frame + 6, state.ia, 6);
  }
  if (len < ETH_MIN_FRAME && (state.config[18] & CFG18_PADDING)) {
    memset(frame + len, 0, ETH_MIN_FRAME - len);
    len = ETH_MIN_FRAME;
  }
  if (len < 14)
    return;

  TRACE_CU("%s: TX %d bytes to %02x:%02x:%02x:%02x:%02x:%02x\n", m_chip.name,
           len, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5]);

  if ((state.config[10] & CFG10_LOOPBACK) || state.phy.loopback())
    receive_frame(frame, len);
  else
    net_backend->send(frame, len);
  state.stats[STAT_TX_GOOD / 4]++;
}

/**
 * Multicast setup: a byte count (14 bits) and that many bytes of
 * addresses, hashed into the 64-bit filter.
 **/
void CI8255x::setup_multicast(u32 cb) {
  int bytes = dma_read16(cb + CB_PARAM) & 0x3fff;
  bytes = std::min(bytes, MC_SETUP_MAX_BYTES);
  memset(state.hash, 0, sizeof(state.hash));
  for (int i = 0; i + 6 <= bytes; i += 6) {
    u8 address[6];
    do_pci_read(cb + CB_PARAM + 2 + i, address, 1, 6);
    const int h = hash_index(address);
    state.hash[h >> 3] |= u8(1 << (h & 7));
  }
}

/**
 * How many bytes of counters a dump writes: 16 counters on the 82557 (and
 * on later parts configured for the standard set), 19 with the 82558's
 * flow control counters, plus the two TCO words on the 82559.
 **/
int CI8255x::statistics_size() const {
  if (m_chip.generation < 8 || (state.config[6] & CFG6_STD_STATS))
    return 64;
  if (m_chip.generation >= 9 && (state.config[6] & CFG6_TCO_STATS))
    return 80;
  return 76;
}

/**
 * Dump (and optionally clear) the counters. Every frame the emulation
 * sends or receives is good, so only the good-frame and resource counters
 * move. On completion the chip writes a marker after the counters, 0xA005
 * (dump) or 0xA007 (dump and reset); drivers poll for it.
 **/
void CI8255x::dump_statistics(bool reset) {
  const int size = statistics_size();
  const u32 base = state.cu_base + state.stats_addr;
  int words = 16;
  if (size >= 76)
    words = 19;
  for (int i = 0; i < words; i++)
    dma_write32(base + 4 * i, state.stats[i]);
  if (size == 80) {
    dma_write16(base + 76, 0);
    dma_write16(base + 78, 0);
  }
  dma_write32(base + size, reset ? 0xa007 : 0xa005);
  if (reset)
    memset(state.stats, 0, sizeof(state.stats));
}
