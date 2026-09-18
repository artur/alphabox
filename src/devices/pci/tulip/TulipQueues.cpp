/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
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
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 */

/**
 * \file
 * The descriptor rings: frames taken off the host backend into the
 * receive ring, the transmit ring put on the wire, the address filter
 * that decides which frames are ours, and the packet trace.
 **/
#include "StdAfx.hpp"

#if defined(HAVE_PCAP) || defined(__linux__)
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

void CTulip::receive_process() {
  const u8 *packet_data = NULL;
  int packet_len = 0;

  // if receive process active
  if (state.reg[CSR_OPMODE / 8] & OPMODE_SR) {

    // get packets from host nic if not in internal loopback mode
    if (!(state.reg[CSR_OPMODE / 8] & OPMODE_OM_INTLOOP)) {
      while (net_backend->receive(&packet_data, &packet_len) > 0) {
        if (trace_packets)
          trace_packet("RX", packet_data, packet_len);
        rx_queue->add_tail(packet_data, packet_len, calc_crc, true);
        // Set 10bT activity -- but only where CSR12 is the SIA status. On
        // the 21140 that register is eight general purpose pins, and this
        // bit is one of them.
        if (m_chip.media != TULIP_MEDIA_GPR_21140)
          state.reg[CSR_SIASTAT / 8] |= SIASTAT_TRA;
      }
    }

    // process a receive descriptor until we run out of
    // descriptors or packets to process
    while (dec21143_rx())
      ;
  }
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
 *  Receive a packet. (If there is no current packet, then check for newly
 *  arrived ones. If the current packet couldn't be fully transfered the
 *  last time, then continue on that packet.)
 **/
int CTulip::dec21143_rx() {
  static u32 descr[4];
  static u32 &rdes0 = descr[0];
  static u32 &rdes1 = descr[1];
  static u32 &rdes2 = descr[2];
  static u32 &rdes3 = descr[3];

  u32 addr = state.rx.cur_addr;
  u32 bufaddr;

  // unsigned char descr[16];
  // u32 rdes0, rdes1, rdes2, rdes3;
  int bufsize;

  // unsigned char descr[16];
  int buf1_size;

  // unsigned char descr[16];
  int buf2_size;

  // unsigned char descr[16];
  // int           writeback_len = 4;

  // unsigned char descr[16];
  int to_xfer;

  // struct pcap_pkthdr * packet_header;
  // const u_char * packet_data = NULL;

  /*  Is current packet finished? Then check for new ones.  */
  if (state.rx.current.used >= state.rx.current.len) {

    /*  Nothing available? Then abort.  */
    if (rx_queue->count() == 0)
      return 0; // indicate nothing was processed

    //      printf("pcap recv: %d bytes (%d captured) for
    //      %02x:%02x:%02x:%02x:%02x:%02x  \n",packet_header->len,
    //      packet_header->caplen,packet_data[0],packet_data[1],packet_data[2],packet_data[3],packet_data[4],packet_data[5]);
    // get next packet from receive queue
    rx_queue->get_head(state.rx.current);

    /*  Append a 4 byte CRC:  */

    // state.rx.cur_buf_len += 4;
    // CHECK_REALLOCATION(state.rx.cur_buf, realloc(state.rx.cur_buf,
    // state.rx.cur_buf_len), unsigned char);

    /*  Get the next packet into our buffer:  */

    // memcpy(state.rx.cur_buf, packet_data, state.rx.cur_buf_len);

    /*  Well... the CRC is just zeros, for now.  */

    // memset(state.rx.cur_buf + state.rx.cur_buf_len - 4, 0, 4);
    // state.rx.cur_offset = 0;
  }

  // read current descriptor
  do_pci_read(state.rx.cur_addr, descr, 4, 4);
  // respect DBO bit - swap descriptor byte order if needed
  if (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO) {
    for (int i = 0; i < 4; i++)
      descr[i] = bswap32_local(descr[i]);
  }

  rdes0 = descr[0];
  rdes1 = descr[1];
  rdes2 = descr[2];
  rdes3 = descr[3];

  /*  Only use descriptors owned by the 21143:  */
  if (!(rdes0 & TDSTAT_OWN)) {
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
    state.reg[CSR_STATUS / 8] = (state.reg[CSR_STATUS / 8] & ~STATUS_RS) |
                                (was_suspended ? 0 : STATUS_RU) |
                                STATUS_RS_SUSPENDED;
    return 0; // indicate nothing was processed
  }

  buf1_size = rdes1 & TDCTL_SIZE1;
  buf2_size = (rdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;
  bufaddr = buf1_size ? rdes2 : rdes3;
  bufsize = buf1_size ? buf1_size : buf2_size;

  // state.reg[CSR_STATUS/8] &= ~STATUS_RS; // dth: wrong, this is receive state
  // stopped
  //   printf("{ dec21143_rx: base = 0x%08x }\n", addr);
  //       debug("{ RX (%08x): 0x%08x 0x%08x 0x%x 0x%x: buf %d bytes at 0x%x
  //       }\n",
  //           addr, rdes0, rdes1, rdes2, rdes3, bufsize, (int)bufaddr);
  //  Turn off all status bits, and give up ownership
  rdes0 = 0x00000000;

  //  Is this the first buffer of the frame?
  if (state.rx.current.used == 0)
    rdes0 |= TDSTAT_Rx_FS;

  // use buffer 1, if length is non-zero
  if (buf1_size > 0) {
    to_xfer = state.rx.current.len - state.rx.current.used;
    if (to_xfer > buf1_size)
      to_xfer = buf1_size;

    // DMA bytes from the packet into buffer 1
    do_pci_write(rdes2, &state.rx.current.frame[state.rx.current.used], 1,
                 to_xfer);

    // update used
    state.rx.current.used += to_xfer;
  }

  // use buffer 2, if length is non-zero and not a chain buffer
  if ((buf2_size > 0) && (!(rdes1 & TDCTL_CH))) {
    to_xfer = state.rx.current.len - state.rx.current.used;
    if (to_xfer > buf2_size)
      to_xfer = buf2_size;

    // DMA bytes from the packet into buffer 2
    do_pci_write(rdes3, state.rx.current.frame + state.rx.current.used, 1,
                 to_xfer);

    // update used
    state.rx.current.used += to_xfer;
  }

  //  Frame completed?
  if (state.rx.current.used >= state.rx.current.len) {

    //        debug("frame complete.\n");
    rdes0 |= TDSTAT_Rx_LS;

    /*  Set the frame length: */
    rdes0 |= ((state.rx.current.len) << 16) &
             TDSTAT_Rx_FL; /* include CRC like HW/QEMU */

    /* Set multicast / filter-fail flags. */
    const u8 *dst = state.rx.current.frame;
    bool is_multicast = (dst[0] & 1) != 0;
    if (is_multicast) {
      rdes0 |= TDSTAT_Rx_MF;
    }
    /* Check perfect filter list programmed by setup-frame; fall back to our
     * MAC. */
    bool perfect = false;
    for (int k = 0; k < 16; k++) {
      u8 *ent = &state.setup_filter[k * 12];
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
    if (state.rx.current.len > 1518)
      rdes0 |= TDSTAT_Rx_TL;
    // ^^ this is quite not possible in current code path...?

    // set receive interrupt and receive state to waiting-for-packet
    state.reg[CSR_STATUS / 8] =
        (state.reg[CSR_STATUS / 8] & ~STATUS_RS) | STATUS_RI | STATUS_RS_WAIT;
  }

  // Writeback rdes0, others are read-only
  state.reg[CSR_STATUS / 8] =
      (state.reg[CSR_STATUS / 8] & ~STATUS_RS) | STATUS_RS_CLOSE;
  // respect DBO bit - swap descriptor byte order if needed
  {
    u32 w0 = rdes0;
    if (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO)
      w0 = bswap32_local(w0);
    do_pci_write(state.rx.cur_addr, &w0, 4, 1);
  }

  // move to next descriptor
  if (rdes1 & TDCTL_ER) // end-of-ring, return to base
    state.rx.cur_addr = state.reg[CSR_RXLIST / 8];
  else {
    if (rdes1 & TDCTL_CH) // explicit chain, use chain address
      state.rx.cur_addr = rdes3;
    else
      // implicit chain
      state.rx.cur_addr += (4 * sizeof(uint32_t)) + state.descr_skip;
  }

  return 1; // indicate processing has occurred
}

/**
 *  Transmit a packet, if the guest OS has marked a descriptor as containing
 *  data to transmit.
 **/
int CTulip::dec21143_tx() {
  u32 addr = state.tx.cur_addr;

  u32 bufaddr;
  u32 descr[4];
  u32 tdes0;
  u32 tdes1;
  u32 tdes2;
  u32 tdes3;
  int bufsize;
  int buf1_size;
  int buf2_size;

  if (state.tx.suspend)
    return 0;

  set_tx_state(STATUS_TS_FETCH);
  do_pci_read(addr, descr, 4, 4);

  if (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO) {
    for (int i = 0; i < 4; i++)
      descr[i] = bswap32_local(descr[i]);
  }
  tdes0 = descr[0];
  tdes1 = descr[1];
  tdes2 = descr[2];
  tdes3 = descr[3];

  /*  printf("{ dec21143_tx: base=0x%08x, tdes0=0x%08x }\n", (int)addr,
   * (int)tdes0);  */

  /*  Only process packets owned by the 21143:  */
  if (!(tdes0 & TDSTAT_OWN)) {
    /* HRM 4.3.7.1: on fetching an unowned descriptor, the chip raises
     * STATUS_TU and moves to suspended state. Do this immediately, no
     * idle threshold. */
    state.reg[CSR_STATUS / 8] |= STATUS_TU;
    set_tx_state(STATUS_TS_SUSPENDED);
    state.tx.suspend = true;
    update_irq();
    return 0;
  }

  buf1_size = tdes1 & TDCTL_SIZE1;
  buf2_size = (tdes1 & TDCTL_SIZE2) >> TDCTL_SIZE2_SHIFT;
  bufaddr = buf1_size ? tdes2 : tdes3;
  bufsize = buf1_size ? buf1_size : buf2_size;

  if (tdes1 & TDCTL_ER) // end-of-ring, return to base
    state.tx.cur_addr = state.reg[CSR_TXLIST / 8];
  else {
    if (tdes1 & TDCTL_CH) // explicit chain, use chain address
      state.tx.cur_addr = tdes3;
    else
      // implicit chain
      state.tx.cur_addr += (4 * sizeof(uint32_t)) + state.descr_skip;
  }

  /*
     printf("{ TX (%llx): 0x%08x 0x%08x 0x%x 0x%x: buf %i bytes at 0x%x }\n",
     (long long)addr, tdes0, tdes1, tdes2, tdes3, bufsize, (int)bufaddr);
   */

  /*  Assume no error:  */
  tdes0 &= ~(TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC | TDSTAT_Tx_NC |
             TDSTAT_Tx_LO | TDSTAT_Tx_TO | TDSTAT_ES);

  if (tdes1 & TDCTL_Tx_SET) {
    set_tx_state(STATUS_TS_SETUP);

    /*
     *  Setup Packet.
     *
     *  TODO. For now, just ignore it, and pretend it worked.
     */

    //              printf("{ TX: setup packet }\n");
    if (bufsize != 192)
      printf("[ dec21143: setup packet len = %i, should be 192! ]\n",
             (int)bufsize);
    do_pci_read(bufaddr, state.setup_filter, 1, 192);
    SetupFilter();

    if (tdes1 & TDCTL_Tx_IC)
      state.reg[CSR_STATUS / 8] |= STATUS_TI;

    /*  Setup frame complete (HRM 4.2.3): clear OWN, set all other
        TDES0 bits to 1. TDES1/2/3 are driver-owned -- do not touch
        (matches QEMU tulip.c and 86Box net_tulip.c). */
    tdes0 = 0x7fffffff;
  } else {
    set_tx_state(STATUS_TS_READING);

    /*
     *  Data Packet.
     */

    //              printf("{ TX: data packet: ");
    if (tdes1 & TDCTL_Tx_FS) {

      /*  First segment. Let's allocate a new buffer:  */

      /*  printf("new frame }\n");  */

      // CHECK_ALLOCATION(state.tx.cur_buf = (unsigned char *)malloc(bufsize));
      state.tx.cur_buf_len = 0;
    } else {

      /*  Not first segment. Increase the length of the current buffer:  */

      /*  printf("continuing last frame }\n");  */
      if (state.tx.cur_buf == NULL)
        printf("[ dec21143: WARNING! tx: middle segment, but no first "
               "segment?! ]\n");

      // CHECK_REALLOCATION(state.tx.cur_buf, realloc(state.tx.cur_buf,
      // state.tx.cur_buf_len + bufsize), unsigned char);
    }

    /* Safely DMA data from guest memory, without exceeding TX scratch capacity.
     */
    {
      /* 2KB scratch (matches allocation in init()), large enough for legal
       * ethernet frames */
      const int tx_cap = 2048; /* aligned with QEMU tulip's 2KB frame buffers */
      /* If guest tried to exceed scratch capacity, we will mark error at LS. */

      /* Buffer 1 */
      if (buf1_size > 0) {
        int avail = tx_cap - state.tx.cur_buf_len;
        int copy = (buf1_size < avail) ? buf1_size : (avail > 0 ? avail : 0);
        if (copy > 0) {
          do_pci_read(tdes2, state.tx.cur_buf + state.tx.cur_buf_len, 1, copy);
          state.tx.cur_buf_len += copy;
        }
      }

      /* Buffer 2 is valid unless the second address is chained.
         TER only controls descriptor wraparound after this descriptor. */
      if (buf2_size > 0 && !(tdes1 & TDCTL_CH)) {
        int avail = tx_cap - state.tx.cur_buf_len;
        int copy = (buf2_size < avail) ? buf2_size : (avail > 0 ? avail : 0);
        if (copy > 0) {
          do_pci_read(tdes3, state.tx.cur_buf + state.tx.cur_buf_len, 1, copy);
          state.tx.cur_buf_len += copy;
        }
      }
    }

    /*  Last segment? Then actually transmit it:  */
    if (tdes1 & TDCTL_Tx_LS) {

      /*  printf("{ TX: data frame complete. }\n");  */

      /* Enforce ethernet max frame length for delivery (without CRC = 1514
       * bytes). */
      bool frame_too_long = (state.tx.cur_buf_len > ETH_MAX_PACKET_RAW);

      /* Only transmit to wire if no loopback is active and frame size is legal.
       */
      if (!frame_too_long && !(state.reg[CSR_OPMODE / 8] & OPMODE_OM)) {
        if (trace_packets)
          trace_packet("TX", state.tx.cur_buf, state.tx.cur_buf_len);
        net_backend->send(state.tx.cur_buf, state.tx.cur_buf_len);
      }

      /* In internal or external loopback, inject into RX queue iff RX is
       * running and size ok. */
      if (!frame_too_long && (state.reg[CSR_OPMODE / 8] & OPMODE_OM) &&
          (state.reg[CSR_OPMODE / 8] & OPMODE_SR)) {
        bool crc = !(tdes1 & TDCTL_Tx_AC);

        // printf("21143: %s packet, AC: %d\n", (state.reg[CSR_OPMODE / 8] &
        // OPMODE_OM_INTLOOP) ? "IL" : "EL", !crc); printf("21143: TX enabled:
        // %d, RX enabled: %d\n", state.reg[CSR_OPMODE /8] & OPMODE_ST,
        // state.reg[CSR_OPMODE /8] & OPMODE_SR); printf("21143: tx(), len=%d,
        // addr=%08x, mode=%s\n", state.tx.cur_buf_len, (int) state.tx.cur_buf,
        // (state.reg[CSR_OPMODE / 8] & OPMODE_OM_INTLOOP) ? "IL" : "EL");
        // printf("21143: tx(), data=|");
        // unsigned char* aptr = state.tx.cur_buf;
        // for(int i=0; i<state.tx.cur_buf_len; i++) {
        //       printf("%02x-",*aptr++);
        // }
        // printf("|\n");
        (void)rx_queue->add_tail(state.tx.cur_buf, state.tx.cur_buf_len,
                                 calc_crc, crc);
      }

      /* Oversize frames: signal jabber and error summary, and do not deliver.
       */
      if (frame_too_long) {
        tdes0 |= TDSTAT_Tx_TO; /* transmit jabber timeout */
        tdes0 |= TDSTAT_ES;    /* error summary */
      }

      // free(state.tx.cur_buf);
      // state.tx.cur_buf = NULL;
      state.tx.cur_buf_len = 0;

      /*  Interrupt, if Tx_IC is set:  */
      if (tdes1 & TDCTL_Tx_IC)
        state.reg[CSR_STATUS / 8] |= STATUS_TI;
    }

    /*  We are done with this segment.  */
    tdes0 &= ~TDSTAT_OWN;
  }

  /*  Error summary:  */
  if (tdes0 & (TDSTAT_Tx_UF | TDSTAT_Tx_EC | TDSTAT_Tx_LC | TDSTAT_Tx_NC |
               TDSTAT_Tx_LO | TDSTAT_Tx_TO))
    tdes0 |= TDSTAT_ES;

  set_tx_state(STATUS_TS_CLOSE);

  /*  Descriptor writeback:
      only write back tdes0 - the status word
      tdes1/tdes2/tdes3 are read-only from NIC's perspective */

  descr[0] = tdes0;
  if (state.reg[CSR_BUSMODE / 8] & BUSMODE_DBO)
    descr[0] = bswap32_local(descr[0]);
  do_pci_write(addr, descr, 1, 4);

  return 1;
}

void CTulip::SetupFilter() {
  u8 mac[16][6];
  int i;
#if defined(DEBUG_NIC_FILTER)
  printf("Building a filter...\n");
#endif
  for (i = 0; i < 16; i++) {
    mac[i][0] = state.setup_filter[i * 12];
    mac[i][1] = state.setup_filter[i * 12 + 1];
    mac[i][2] = state.setup_filter[i * 12 + 4];
    mac[i][3] = state.setup_filter[i * 12 + 5];
    mac[i][4] = state.setup_filter[i * 12 + 8];
    mac[i][5] = state.setup_filter[i * 12 + 9];
#if defined(DEBUG_NIC_FILTER)
    printf("MAC[%d] = %02x:%02x:%02x:%02x:%02x:%02x. \n", i, mac[i][0],
           mac[i][1], mac[i][2], mac[i][3], mac[i][4], mac[i][5]);
#endif
  }

#if defined(DEBUG_NIC_FILTER)
  printf("Filter mode: ");
  if (state.reg[CSR_OPMODE / 8] & OPMODE_PR)
    printf("promiscuous.\n");
  else {
    if (state.reg[CSR_OPMODE / 8] & OPMODE_IF)
      printf("inverse ");
    if (state.reg[CSR_OPMODE / 8] & OPMODE_HP) {
      printf("hash ");
      if (state.reg[CSR_OPMODE / 8] & OPMODE_HO)
        printf("only ");
    } else
      printf("perfect ");
    printf("filtering.\n");
  }
#endif

  /* Hand the CSR6 filtering semantics to the backend; pcap builds a BPF
   * expression from these, tap relies on the guest driver's own filtering. */
  NetworkFilter nf;
  memcpy(nf.mac_list, mac, sizeof(nf.mac_list));
  nf.promiscuous = (state.reg[CSR_OPMODE / 8] & OPMODE_PR) != 0;
  nf.receive_all = (state.reg[CSR_OPMODE / 8] & OPMODE_RA) != 0;
  nf.pass_multicast = (state.reg[CSR_OPMODE / 8] & OPMODE_PM) != 0;
  nf.inverse = (state.reg[CSR_OPMODE / 8] & OPMODE_IF) != 0;
  memcpy(nf.own_mac, state.mac, sizeof(nf.own_mac));
  net_backend->set_filter(nf);
}

#endif // defined(HAVE_PCAP) || defined(__linux__)
