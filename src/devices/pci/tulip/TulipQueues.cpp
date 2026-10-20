/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
 *
 * Contains code from ES40-Emu:
 * Copyright (C) 2025-2026 gdwnldsKSC and the ES40-Emu contributors
 * Copyright (C) 2026 Aleksander Czajczyński
 * Website: https://github.com/ES40-Emu/es40
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
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/**
 * \file
 * The descriptor rings: frames taken off the host backend into the
 * receive ring, the transmit ring put on the wire, the address filter
 * that decides which frames are ours, and the packet trace.
 **/
#include "StdAfx.hpp"

#include "Tulip.hpp"
#include "System.hpp"
#include <string.h>

static u16 pkt_be16(const u8 *p) { return (u16)(((u16)p[0] << 8) | p[1]); }

static void pkt_format_mac(char *out, const u8 *mac) {
  sprintf(out, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3],
          mac[4], mac[5]);
}

static void pkt_format_ip(char *out, const u8 *ip) {
  sprintf(out, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static const char *dhcp_type_name(int msg_type) {
  switch (msg_type) {
  case 1:
    return "discover";
  case 2:
    return "offer";
  case 3:
    return "request";
  case 4:
    return "decline";
  case 5:
    return "ack";
  case 6:
    return "nak";
  case 7:
    return "release";
  case 8:
    return "inform";
  default:
    return "unknown";
  }
}

static void append_text(char *out, int out_len, int *pos, const char *text) {
  if (*pos >= out_len - 1)
    return;
  int left = out_len - *pos;
  int wrote = snprintf(out + *pos, left, "%s", text);
  if (wrote < 0)
    return;
  if (wrote >= left)
    *pos = out_len - 1;
  else
    *pos += wrote;
}

static void append_dhcp_ip(char *out, int out_len, int *pos, const char *name,
                           const u8 *ip) {
  char addr[16];
  char text[40];

  pkt_format_ip(addr, ip);
  snprintf(text, sizeof(text), " %s=%s", name, addr);
  append_text(out, out_len, pos, text);
}

static void describe_dhcp(const u8 *bootp, int len, char *out, int out_len) {
  int pos = 0;
  int msg_type = -1;
  const u8 *subnet = NULL;
  const u8 *router = NULL;
  const u8 *server = NULL;
  const u8 *requested = NULL;
  char text[48];
  char yiaddr[16];

  out[0] = '\0';
  if (len < 240)
    return;
  if (bootp[236] != 0x63 || bootp[237] != 0x82 || bootp[238] != 0x53 ||
      bootp[239] != 0x63)
    return;

  for (int off = 240; off < len;) {
    int opt = bootp[off++];
    if (opt == 0)
      continue;
    if (opt == 255)
      break;
    if (off >= len)
      break;

    int opt_len = bootp[off++];
    if (opt_len < 0 || off + opt_len > len)
      break;

    switch (opt) {
    case 1:
      if (opt_len >= 4)
        subnet = &bootp[off];
      break;
    case 3:
      if (opt_len >= 4)
        router = &bootp[off];
      break;
    case 50:
      if (opt_len >= 4)
        requested = &bootp[off];
      break;
    case 53:
      if (opt_len >= 1)
        msg_type = bootp[off];
      break;
    case 54:
      if (opt_len >= 4)
        server = &bootp[off];
      break;
    default:
      break;
    }
    off += opt_len;
  }

  if (msg_type >= 0) {
    snprintf(text, sizeof(text), " DHCP=%s(%d)", dhcp_type_name(msg_type),
             msg_type);
    append_text(out, out_len, &pos, text);
  } else
    append_text(out, out_len, &pos, " DHCP");

  if (bootp[16] || bootp[17] || bootp[18] || bootp[19]) {
    pkt_format_ip(yiaddr, &bootp[16]);
    snprintf(text, sizeof(text), " yiaddr=%s", yiaddr);
    append_text(out, out_len, &pos, text);
  }
  if (subnet)
    append_dhcp_ip(out, out_len, &pos, "subnet", subnet);
  if (router)
    append_dhcp_ip(out, out_len, &pos, "router", router);
  if (server)
    append_dhcp_ip(out, out_len, &pos, "server", server);
  if (requested)
    append_dhcp_ip(out, out_len, &pos, "request", requested);
}

/**
 * Take what the back end has into the receive queue, then into the receive
 * ring. Only this thread reads the back end, so it does that without the
 * lock, and queues each frame under it.
 **/
void CTulip::receive_process() {
  bool listening;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    const u32 csr6 = state.reg[CSR_OPMODE / 8];
    // In internal loopback the receiver does not listen to the wire.
    listening = (csr6 & OPMODE_SR) && !(csr6 & OPMODE_OM_INTLOOP);
  }

  if (listening) {
    const u8 *packet_data = NULL;
    int packet_len = 0;
    while (net_backend->receive(&packet_data, &packet_len) > 0) {
      if (trace_packets)
        trace_packet("RX", packet_data, packet_len);
      std::lock_guard<std::mutex> lock(m_lock);
      rx_queue->add_tail(packet_data, packet_len, calc_crc, true);
      // Set 10bT activity -- but only where CSR12 is the SIA status. On
      // the 21140 that register is eight general purpose pins, and this
      // bit is one of them.
      if (m_chip.media != TULIP_MEDIA_GPR_21140)
        state.reg[CSR_SIASTAT / 8] |= SIASTAT_TRA;
    }
  }

  // Fill receive descriptors until the frames or the descriptors run out.
  while (dec21143_rx())
    ;
}

void CTulip::trace_packet(const char *dir, const u8 *frame, int len) {
  char dst[18];
  char src[18];
  u16 ether_type;

  if (len < 14) {
    printf("%s: %s len=%d short ethernet frame\n", devid_string, dir, len);
    return;
  }

  pkt_format_mac(dst, &frame[0]);
  pkt_format_mac(src, &frame[6]);
  ether_type = pkt_be16(&frame[12]);

  if (ether_type == 0x0806 && len >= 42) {
    char sha[18];
    char tha[18];
    char spa[16];
    char tpa[16];
    u16 op = pkt_be16(&frame[20]);

    pkt_format_mac(sha, &frame[22]);
    pkt_format_ip(spa, &frame[28]);
    pkt_format_mac(tha, &frame[32]);
    pkt_format_ip(tpa, &frame[38]);
    printf("%s: %s len=%d %s -> %s ARP op=%u sha=%s spa=%s tha=%s tpa=%s\n",
           devid_string, dir, len, src, dst, op, sha, spa, tha, tpa);
    return;
  }

  if (ether_type == 0x0800 && len >= 34) {
    const u8 *ip = &frame[14];
    int ihl = (ip[0] & 0x0f) * 4;
    char src_ip[16];
    char dst_ip[16];
    u8 proto;

    if (ihl < 20 || len < 14 + ihl) {
      printf("%s: %s len=%d %s -> %s IPv4 malformed\n", devid_string, dir, len,
             src, dst);
      return;
    }

    proto = ip[9];
    pkt_format_ip(src_ip, &ip[12]);
    pkt_format_ip(dst_ip, &ip[16]);

    if (proto == 17 && len >= 14 + ihl + 8) {
      const u8 *udp = ip + ihl;
      int udp_payload_len = len - 14 - ihl - 8;
      u16 sport = pkt_be16(&udp[0]);
      u16 dport = pkt_be16(&udp[2]);
      char dhcp[220];

      dhcp[0] = '\0';
      if ((sport == 67 || sport == 68 || dport == 67 || dport == 68) &&
          udp_payload_len > 0)
        describe_dhcp(udp + 8, udp_payload_len, dhcp, sizeof(dhcp));

      printf("%s: %s len=%d %s -> %s IPv4 %s:%u -> %s:%u UDP%s\n", devid_string,
             dir, len, src, dst, src_ip, sport, dst_ip, dport, dhcp);
      return;
    }

    if (proto == 1 && len >= 14 + ihl + 2) {
      const u8 *icmp = ip + ihl;
      printf("%s: %s len=%d %s -> %s IPv4 %s -> %s ICMP type=%u code=%u\n",
             devid_string, dir, len, src, dst, src_ip, dst_ip, icmp[0],
             icmp[1]);
      return;
    }

    printf("%s: %s len=%d %s -> %s IPv4 %s -> %s proto=%u\n", devid_string, dir,
           len, src, dst, src_ip, dst_ip, proto);
    return;
  }

  printf("%s: %s len=%d %s -> %s ethertype=0x%04x\n", devid_string, dir, len,
         src, dst, ether_type);
}

/**
 * Count one missed frame in CSR8: the 16-bit counter saturates and sets the
 * overflow bit (MFO).
 **/
static inline void count_missed_frame(u32 &csr8) {
  if ((csr8 & MISSED_MFC) == MISSED_MFC)
    csr8 |= MISSED_MFO;
  else
    csr8++;
}

/**
 * The status bits of the last descriptor of a received frame, besides the
 * frame length: multicast, filtering fail, data type, too long. Under
 * m_lock.
 **/
u32 CTulip::rx_last_status_locked(const u8 *frame, int len) const {
  u32 rdes0 = TDSTAT_Rx_LS;

  /*  Set the frame length: */
  rdes0 |= ((u32)len << 16) & TDSTAT_Rx_FL; /* include CRC like HW/QEMU */

  /* Set multicast / filter-fail flags. */
  const u8 *dst = frame;
  bool is_multicast = (dst[0] & 1) != 0;
  if (is_multicast) {
    rdes0 |= TDSTAT_Rx_MF;
  }
  /* Check perfect filter list programmed by setup-frame; fall back to our
   * MAC. */
  bool perfect = false;
  for (int k = 0; k < 16; k++) {
    const u8 *ent = &state.setup_filter[k * 12];
    u8 pa[6] = {ent[0], ent[1], ent[4], ent[5], ent[8], ent[9]};
    if ((pa[0] | pa[1] | pa[2] | pa[3] | pa[4] | pa[5]) == 0)
      continue;
    if (memcmp(dst, pa, 6) == 0) {
      perfect = true;
      break;
    }
  }
  if (!perfect && memcmp(dst, state.mac, 6) == 0) {
    perfect = true;
  }
  if (!perfect) {
    if (state.reg[CSR_OPMODE / 8] & (OPMODE_PR | OPMODE_RA)) {
      rdes0 |= TDSTAT_Rx_FF;
    }
  }

  /* Data type per OM (normal / internal / external loopback). */
  if (state.reg[CSR_OPMODE / 8] & OPMODE_OM_INTLOOP) {
    rdes0 |= TDSTAT_Rx_DT_IL;
  } else if (state.reg[CSR_OPMODE / 8] & OPMODE_OM_EXTLOOP) {
    rdes0 |= TDSTAT_Rx_DT_EL;
  } else {
    rdes0 |= TDSTAT_Rx_DT_SR;
  }

  /*  Frame too long? (1518 is max ethernet frame length)  */
  if (len > 1518)
    rdes0 |= TDSTAT_Rx_TL;
  // ^^ this is quite not possible in current code path...?

  return rdes0;
}

/**
 * Put the frame in hand, or its next part, into the current receive
 * descriptor; a new frame is taken from the receive queue when the last one
 * is done. Returns 1 when a descriptor was filled, 0 when there is nothing
 * to receive or nowhere to put it.
 *
 * The descriptor is fetched, filled and written back without the lock (see
 * "Threads and locking" in Tulip.cpp), and the move to the next one, the
 * receive interrupt and the process state committed under it.
 **/
int CTulip::dec21143_rx() {
  // The descriptor is this call's own: every Tulip runs this on its own
  // thread, so nothing here may be shared between them.
  u32 descr[4];
  u32 addr;
  u32 epoch;
  bool dbo;
  int len;
  int used;

  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!(state.reg[CSR_OPMODE / 8] & OPMODE_SR))
      return 0;
    /*  Is current packet finished? Then check for new ones.  */
    if (state.rx.current.used >= state.rx.current.len &&
        !rx_queue->get_head(state.rx.current))
      return 0; // nothing to receive
    addr = state.rx.cur_addr;
    epoch = m_rx_epoch;
    dbo = (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO) != 0;
    len = state.rx.current.len;
    used = state.rx.current.used;
  }

  // read current descriptor
  do_pci_read(addr, descr, 4, 4);
  // respect DBO bit - swap descriptor byte order if needed
  if (dbo) {
    for (int i = 0; i < 4; i++)
      descr[i] = bswap32_local(descr[i]);
  }
  const u32 rdes1 = descr[1];
  const u32 rdes2 = descr[2];
  const u32 rdes3 = descr[3];

  /*  Only use descriptors owned by the 21143:  */
  if (!(descr[0] & TDSTAT_OWN)) {
    std::lock_guard<std::mutex> lock(m_lock);
    if (epoch != m_rx_epoch)
      return 0;
    // No receive buffer: like the chip, discard the arriving frame and count
    // it in CSR8 instead of holding it back until the driver refills the
    // ring. RU is raised only on the change into the suspended state;
    // raising it on every poll re-asserts the interrupt the driver has just
    // acknowledged.
    const bool was_suspended =
        (state.reg[CSR_STATUS / 8] & STATUS_RS) == STATUS_RS_SUSPENDED;
    if (state.rx.current.used < state.rx.current.len) {
      state.rx.current.used = state.rx.current.len;
      count_missed_frame(state.reg[CSR_MISSED / 8]);
    }
    if (state.reg[CSR_OPMODE / 8] & OPMODE_SR)
      state.reg[CSR_STATUS / 8] = (state.reg[CSR_STATUS / 8] & ~STATUS_RS) |
                                  (was_suspended ? 0 : STATUS_RU) |
                                  STATUS_RS_SUSPENDED;
    return 0; // indicate nothing was processed
  }

  const int buf1_size = rdes1 & TDCTL_SIZE1;
  const int buf2_size = (rdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;

  //  Turn off all status bits, and give up ownership
  u32 rdes0 = 0;

  //  Is this the first buffer of the frame?
  if (used == 0)
    rdes0 |= TDSTAT_Rx_FS;

  // The frame's bytes are written by this thread alone (get_head above), so
  // they are read here without the lock.
  u8 *frame = state.rx.current.frame;

  // use buffer 1, if length is non-zero
  if (buf1_size > 0) {
    const int to_xfer = std::min(len - used, buf1_size);
    // DMA bytes from the packet into buffer 1
    do_pci_write(rdes2, frame + used, 1, to_xfer);
    used += to_xfer;
  }

  // use buffer 2, if length is non-zero and not a chain buffer
  if ((buf2_size > 0) && (!(rdes1 & TDCTL_CH))) {
    const int to_xfer = std::min(len - used, buf2_size);
    // DMA bytes from the packet into buffer 2
    do_pci_write(rdes3, frame + used, 1, to_xfer);
    used += to_xfer;
  }

  //  Frame completed?
  const bool last = used >= len;
  if (last) {
    std::lock_guard<std::mutex> lock(m_lock);
    rdes0 |= rx_last_status_locked(frame, len);
  }

  // Writeback rdes0, others are read-only
  {
    u32 w0 = dbo ? bswap32_local(rdes0) : rdes0;
    do_pci_write(addr, &w0, 4, 1);
  }

  std::lock_guard<std::mutex> lock(m_lock);
  if (epoch != m_rx_epoch)
    return 0; // reset or re-pointed meanwhile: the frame went with the ring
  state.rx.current.used = used;
  // set receive interrupt once the frame is in memory
  if (last)
    state.reg[CSR_STATUS / 8] |= STATUS_RI;
  if (state.reg[CSR_OPMODE / 8] & OPMODE_SR)
    set_rx_state(STATUS_RS_CLOSE);

  // move to next descriptor
  if (rdes1 & TDCTL_ER) // end-of-ring, return to base
    state.rx.cur_addr = state.reg[CSR_RXLIST / 8];
  else if (rdes1 & TDCTL_CH) // explicit chain, use chain address
    state.rx.cur_addr = rdes3;
  else // implicit chain
    state.rx.cur_addr = addr + (4 * sizeof(uint32_t)) + state.descr_skip;

  return 1; // indicate processing has occurred
}

/**
 * Transmit a packet, if the guest OS has marked a descriptor as containing
 * data to transmit. Returns 1 when a descriptor was processed.
 *
 * Like the receive side, the descriptor and its buffers are read and the
 * status written back without the lock, and the move to the next descriptor
 * committed under it.
 **/
int CTulip::dec21143_tx() {
  u32 descr[4];
  u32 addr;
  u32 epoch;
  u32 txlist;
  u32 opmode;
  int skip;
  int buf_len;
  bool dbo;

  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!(state.reg[CSR_OPMODE / 8] & OPMODE_ST) || state.tx.suspend)
      return 0;
    set_tx_state(STATUS_TS_FETCH);
    addr = state.tx.cur_addr;
    epoch = m_tx_epoch;
    dbo = (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO) != 0;
    txlist = state.reg[CSR_TXLIST / 8];
    skip = state.descr_skip;
    buf_len = state.tx.cur_buf_len;
  }

  do_pci_read(addr, descr, 4, 4);
  if (dbo) {
    for (int i = 0; i < 4; i++)
      descr[i] = bswap32_local(descr[i]);
  }
  u32 tdes0 = descr[0];
  const u32 tdes1 = descr[1];
  const u32 tdes2 = descr[2];
  const u32 tdes3 = descr[3];

  const int buf1_size = tdes1 & TDCTL_SIZE1;
  const int buf2_size = (tdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;
  const u32 bufaddr = buf1_size ? tdes2 : tdes3;
  const int bufsize = buf1_size ? buf1_size : buf2_size;

  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (epoch != m_tx_epoch)
      return 0;
    const bool running = (state.reg[CSR_OPMODE / 8] & OPMODE_ST) != 0;
    /*  Only process packets owned by the 21143:  */
    if (!(tdes0 & TDSTAT_OWN)) {
      /* HRM 4.3.6.2: on fetching an unowned descriptor, the chip raises
       * STATUS_TU and moves to suspended state. Do this immediately, no
       * idle threshold. */
      state.reg[CSR_STATUS / 8] |= STATUS_TU;
      if (running)
        set_tx_state(STATUS_TS_SUSPENDED);
      state.tx.suspend = true;
      return 0;
    }
    if (running)
      set_tx_state((tdes1 & TDCTL_Tx_SET) ? STATUS_TS_SETUP
                                          : STATUS_TS_READING);
    opmode = state.reg[CSR_OPMODE / 8];
  }

  u32 next;
  if (tdes1 & TDCTL_ER) // end-of-ring, return to base
    next = txlist;
  else if (tdes1 & TDCTL_CH) // explicit chain, use chain address
    next = tdes3;
  else // implicit chain
    next = addr + (4 * sizeof(uint32_t)) + skip;

  /*  Assume no error:  */
  tdes0 &= ~(TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC | TDSTAT_Tx_NC |
             TDSTAT_Tx_LO | TDSTAT_Tx_TO | TDSTAT_ES);
  bool ti = false;

  if (tdes1 & TDCTL_Tx_SET) {
    /*
     *  Setup Packet.
     */
    u8 setup[192];
    if (bufsize != 192)
      printf("[ dec21143: setup packet len = %i, should be 192! ]\n",
             (int)bufsize);
    do_pci_read(bufaddr, setup, 1, 192);
    {
      std::lock_guard<std::mutex> lock(m_lock);
      if (epoch != m_tx_epoch)
        return 0;
      memcpy(state.setup_filter, setup, sizeof(state.setup_filter));
      m_filter_dirty = true;
    }

    ti = (tdes1 & TDCTL_Tx_IC) != 0;

    /*  Setup frame complete (HRM 4.2.3): clear OWN, set all other
        TDES0 bits to 1. TDES1/2/3 are driver-owned -- do not touch
        (matches QEMU tulip.c and 86Box net_tulip.c). */
    tdes0 = 0x7fffffff;
  } else {
    /*
     *  Data Packet. The first segment starts a new frame in the 2 KB
     *  scratch buffer; only this thread touches its bytes.
     */
    if (tdes1 & TDCTL_Tx_FS)
      buf_len = 0;

    /* Safely DMA data from guest memory, without exceeding TX scratch capacity.
     */
    {
      /* 2KB scratch (matches allocation in init()), large enough for legal
       * ethernet frames */
      const int tx_cap = 2048; /* aligned with QEMU tulip's 2KB frame buffers */
      /* If guest tried to exceed scratch capacity, we will mark error at LS. */

      /* Buffer 1 */
      if (buf1_size > 0) {
        int avail = tx_cap - buf_len;
        int copy = (buf1_size < avail) ? buf1_size : (avail > 0 ? avail : 0);
        if (copy > 0) {
          do_pci_read(tdes2, state.tx.cur_buf + buf_len, 1, copy);
          buf_len += copy;
        }
      }

      /* Buffer 2 is valid unless the second address is chained.
         TER only controls descriptor wraparound after this descriptor. */
      if (buf2_size > 0 && !(tdes1 & TDCTL_CH)) {
        int avail = tx_cap - buf_len;
        int copy = (buf2_size < avail) ? buf2_size : (avail > 0 ? avail : 0);
        if (copy > 0) {
          do_pci_read(tdes3, state.tx.cur_buf + buf_len, 1, copy);
          buf_len += copy;
        }
      }
    }

    /*  Last segment? Then actually transmit it:  */
    if (tdes1 & TDCTL_Tx_LS) {
      /* Enforce ethernet max frame length for delivery (without CRC = 1514
       * bytes). */
      bool frame_too_long = (buf_len > ETH_MAX_PACKET_RAW);

      /* Only transmit to wire if no loopback is active and frame size is legal.
       */
      if (!frame_too_long && !(opmode & OPMODE_OM)) {
        if (trace_packets)
          trace_packet("TX", state.tx.cur_buf, buf_len);
        net_backend->send(state.tx.cur_buf, buf_len);
      }

      /* In internal or external loopback, inject into RX queue iff RX is
       * running and size ok. */
      if (!frame_too_long && (opmode & OPMODE_OM) && (opmode & OPMODE_SR)) {
        bool crc = !(tdes1 & TDCTL_Tx_AC);
        std::lock_guard<std::mutex> lock(m_lock);
        (void)rx_queue->add_tail(state.tx.cur_buf, buf_len, calc_crc, crc);
      }

      /* Oversize frames: signal jabber and error summary, and do not deliver.
       */
      if (frame_too_long) {
        tdes0 |= TDSTAT_Tx_TO; /* transmit jabber timeout */
        tdes0 |= TDSTAT_ES;    /* error summary */
      }

      buf_len = 0;

      /*  Interrupt, if Tx_IC is set:  */
      ti = (tdes1 & TDCTL_Tx_IC) != 0;
    }

    /*  We are done with this segment.  */
    tdes0 &= ~TDSTAT_OWN;
  }

  /*  Error summary:  */
  if (tdes0 & (TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC | TDSTAT_Tx_NC |
               TDSTAT_Tx_LO | TDSTAT_Tx_TO))
    tdes0 |= TDSTAT_ES;

  /*  Descriptor writeback:
      only write back tdes0 - the status word
      tdes1/tdes2/tdes3 are read-only from NIC's perspective */
  descr[0] = dbo ? bswap32_local(tdes0) : tdes0;
  do_pci_write(addr, descr, 1, 4);

  std::lock_guard<std::mutex> lock(m_lock);
  if (epoch != m_tx_epoch)
    return 0; // reset or re-pointed meanwhile
  state.tx.cur_addr = next;
  state.tx.cur_buf_len = buf_len;
  // TI follows the status in memory (HRM 4.3.6.1).
  if (ti)
    state.reg[CSR_STATUS / 8] |= STATUS_TI;
  if (state.reg[CSR_OPMODE / 8] & OPMODE_ST)
    set_tx_state(STATUS_TS_CLOSE);

  return 1;
}

/**
 * The host filter: what the back end may pass up. Under m_lock.
 **/
NetworkFilter CTulip::host_filter_locked() const {
  NetworkFilter nf;
  memset(&nf, 0, sizeof(nf));
  for (int i = 0; i < 16; i++) {
    nf.mac_list[i][0] = state.setup_filter[i * 12];
    nf.mac_list[i][1] = state.setup_filter[i * 12 + 1];
    nf.mac_list[i][2] = state.setup_filter[i * 12 + 4];
    nf.mac_list[i][3] = state.setup_filter[i * 12 + 5];
    nf.mac_list[i][4] = state.setup_filter[i * 12 + 8];
    nf.mac_list[i][5] = state.setup_filter[i * 12 + 9];
#if defined(DEBUG_NIC_FILTER)
    printf("MAC[%d] = %02x:%02x:%02x:%02x:%02x:%02x. \n", i, nf.mac_list[i][0],
           nf.mac_list[i][1], nf.mac_list[i][2], nf.mac_list[i][3],
           nf.mac_list[i][4], nf.mac_list[i][5]);
#endif
  }

  /* Hand the CSR6 filtering semantics to the backend; pcap builds a BPF
   * expression from these, tap relies on the guest driver's own filtering. */
  nf.promiscuous = (state.reg[CSR_OPMODE / 8] & OPMODE_PR) != 0;
  nf.receive_all = (state.reg[CSR_OPMODE / 8] & OPMODE_RA) != 0;
  nf.pass_multicast = (state.reg[CSR_OPMODE / 8] & OPMODE_PM) != 0;
  nf.inverse = (state.reg[CSR_OPMODE / 8] & OPMODE_IF) != 0;
  memcpy(nf.own_mac, state.mac, sizeof(nf.own_mac));
  return nf;
}

/**
 * Hand the back end the host filter, if the NIC's has changed since. On the
 * NIC thread, or in init before there is one: the back ends are not thread
 * safe, and pcap's filter belongs to the handle the thread reads.
 **/
void CTulip::apply_filter() {
  NetworkFilter nf;
  {
    std::lock_guard<std::mutex> lock(m_lock);
    if (!m_filter_dirty)
      return;
    m_filter_dirty = false;
    nf = host_filter_locked();
  }
  net_backend->set_filter(nf);
}
