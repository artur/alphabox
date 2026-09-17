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
 * Intel 8255x family: the receive unit (RU).
 *
 * The RU fills the receive frame area, a list of RFDs. A simplified RFD
 * holds the whole frame; a flexible one (SF set) holds up to its size and
 * the rest goes to the receive buffer descriptor list, whose head the
 * first RFD names at RU start. RFD and RBD links are offsets from the RU
 * base; RBD buffer addresses are absolute, as TBD buffers are.
 *
 * Frames that arrive while the RU is not ready are dropped and counted as
 * resource errors, as on the chip.
 **/
#include "I8255x.hpp"
#include "StdAfx.hpp"

#include "I8255xRegs.hpp"

#include <algorithm>

#if defined(DEBUG_NIC)
#define TRACE_RU(...) printf(__VA_ARGS__)
#else
#define TRACE_RU(...)
#endif

static const int ETH_MIN_FRAME = 60;   // without CRC
static const int ETH_MAX_FRAME = 1514; // without CRC
static const int RX_FRAME_MAX = 2600;

static bool rbd_mode(int ru) {
  return ru == RU_SUSPENDED_NO_RBDS || ru == RU_NO_RESOURCES_NO_RBDS ||
         ru == RU_READY_NO_RBDS;
}

void CI8255x::ru_command(u8 command, u32 pointer) {
  const int ru = ru_state();

  switch (command) {
  case RUC_START: {
    state.ru_offset = pointer;
    const u32 rfd = state.ru_base + pointer;
    const u32 rbd = dma_read32(rfd + RFD_RBD);
    state.rbd_valid = rbd != 0xffffffff;
    state.rbd_offset = rbd;
    set_ru_state(RU_READY);
    break;
  }
  case RUC_RESUME:
    if (ru == RU_SUSPENDED || ru == RU_SUSPENDED_NO_RBDS)
      set_ru_state(RU_READY);
    break;
  case RUC_RBD_RESUME:
    // New buffers were linked on after the one that ran out.
    if (rbd_mode(ru)) {
      if (!state.rbd_valid) {
        state.rbd_offset = pointer;
        state.rbd_valid = true;
      }
      set_ru_state(RU_READY);
    }
    break;
  case RUC_ABORT:
    if (ru == RU_READY || ru == RU_READY_NO_RBDS)
      raise(STAT_RNR);
    set_ru_state(RU_IDLE);
    break;
  case RUC_LOAD_BASE:
    state.ru_base = pointer;
    break;
  case RUC_LOAD_HDS:
  case RUC_DMA_REDIRECT:
    break; // no header RFDs or early receive DMA in the emulation
  default:
    break;
  }
}

/**
 * The address filter. Fills the RFD status bits that describe the match.
 **/
bool CI8255x::accept_frame(const u8 *frame, int len, u16 *status) {
  static const u8 broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  const bool promiscuous = (state.config[15] & CFG15_PROMISC) != 0;
  const u8 *dst = frame;

  if (m_chip.generation >= 8 && (state.config[8] & CFG8_CSMA_DIS))
    return false;
  if (len > ETH_MAX_FRAME &&
      !(m_chip.generation >= 8 && (state.config[18] & CFG18_LONG_RX)))
    return false;

  if (!memcmp(dst, state.ia, 6))
    return true;

  *status |= RFD_STATUS_IA_MISMATCH;
  bool match;
  if (!memcmp(dst, broadcast, 6)) {
    match = !(state.config[15] & CFG15_BCAST_DIS);
  } else if (dst[0] & 1) {
    const int h = hash_index(dst);
    match = (state.config[21] & CFG21_MC_ALL) ||
            (state.hash[h >> 3] & (1 << (h & 7)));
  } else {
    const int h = hash_index(dst);
    match = (state.config[20] & CFG20_MULTI_IA) &&
            (state.hash[h >> 3] & (1 << (h & 7)));
  }
  if (match)
    return true;
  if (promiscuous) {
    *status |= RFD_STATUS_NO_MATCH;
    return true;
  }
  return false;
}

/**
 * Index into the 64-bit hash filter: the six low-order bits of the
 * destination address's CRC.
 **/
int CI8255x::hash_index(const u8 *address) {
  return int(eth_crc32(0, address, 6) & 0x3f);
}

void CI8255x::receive_frame(const u8 *in, int in_len) {
  u8 frame[RX_FRAME_MAX + 4];
  u16 status = RFD_STATUS_C | RFD_STATUS_OK;

  if (in_len < 14 || in_len > RX_FRAME_MAX)
    return;
  if (!accept_frame(in, in_len, &status))
    return;

  int ru = ru_state();
  if (ru != RU_READY && ru != RU_READY_NO_RBDS) {
    state.stats[STAT_RX_RESOURCE / 4]++;
    return;
  }

  // A host frame may be shorter than the wire allows: pad it as the
  // sender's MAC would have.
  int len = in_len;
  memcpy(frame, in, len);
  if (len < ETH_MIN_FRAME) {
    memset(frame + len, 0, ETH_MIN_FRAME - len);
    len = ETH_MIN_FRAME;
  }
  if (state.config[18] & CFG18_RX_CRC) {
    const u32 crc = eth_crc32(0, frame, len);
    for (int i = 0; i < 4; i++)
      frame[len++] = u8(crc >> (8 * i));
  }
  if (((frame[12] << 8) | frame[13]) > 1500)
    status |= RFD_STATUS_TYPE;

  const u32 rfd = state.ru_base + state.ru_offset;
  const u16 command = dma_read16(rfd + RFD_COMMAND);
  const u32 link = dma_read32(rfd + RFD_LINK);
  const int size = dma_read16(rfd + RFD_SIZE) & COUNT_MASK;
  const bool flexible = (command & RFD_CMD_SF) != 0;

  TRACE_RU("%s: RX %d bytes into RFD %08x (cmd %04x size %d)\n", m_chip.name,
           len, rfd, command, size);

  // The part that fits the RFD itself.
  const int in_rfd = std::min(len, size);
  do_pci_write(rfd + RFD_DATA, frame, 1, in_rfd);
  int stored = in_rfd;
  bool rbds_exhausted = false;

  if (flexible && stored < len) {
    u32 first_rbd = 0xffffffff;
    stored += store_frame_rbds(frame + stored, len - stored, &first_rbd);
    dma_write32(rfd + RFD_RBD, first_rbd);
    rbds_exhausted = !state.rbd_valid;
  }
  if (stored < len)
    status |= RFD_STATUS_NO_RESOURCES; // truncated
  if (status & RFD_STATUS_NO_RESOURCES)
    status &= ~RFD_STATUS_OK;

  u16 count = u16(in_rfd | COUNT_F);
  if (in_rfd == len)
    count |= COUNT_EOF;
  dma_write16(rfd + RFD_COUNT, count);
  dma_write16(rfd + RFD_STATUS, status);
  state.stats[STAT_RX_GOOD / 4]++;
  state.ru_offset = link;

  u8 causes = STAT_FR;
  if (command & RFD_CMD_EL) {
    set_ru_state(RU_NO_RESOURCES);
    causes |= STAT_RNR;
  } else if (command & RFD_CMD_S) {
    set_ru_state(RU_SUSPENDED);
    causes |= STAT_RNR;
  } else if (rbds_exhausted) {
    set_ru_state(RU_NO_RESOURCES_NO_RBDS);
    causes |= STAT_RNR;
  }
  raise(causes);
}

/**
 * Store the rest of a frame into the RBD list; returns the bytes stored.
 * Each RBD used gets its actual count (F, and EOF on the frame's last);
 * an RBD with EL ends the list, leaving no RBD for the next frame.
 **/
int CI8255x::store_frame_rbds(const u8 *data, int len, u32 *first_rbd) {
  int stored = 0;
  while (stored < len && state.rbd_valid) {
    const u32 rbd = state.ru_base + state.rbd_offset;
    const u32 link = dma_read32(rbd + RBD_LINK);
    const u32 buffer = dma_read32(rbd + RBD_BUFFER);
    const u16 size_word = dma_read16(rbd + RBD_SIZE);
    const int n = std::min(len - stored, int(size_word & COUNT_MASK));

    if (*first_rbd == 0xffffffff)
      *first_rbd = state.rbd_offset;
    do_pci_write(buffer, const_cast<u8 *>(data + stored), 1, n);
    stored += n;
    dma_write16(rbd + RBD_COUNT,
                u16(n | COUNT_F | (stored == len ? COUNT_EOF : 0)));

    if (size_word & RBD_EL)
      state.rbd_valid = false;
    else
      state.rbd_offset = link;
  }
  return stored;
}
