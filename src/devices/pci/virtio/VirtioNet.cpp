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
 * virtio-net, legacy interface (OASIS virtio 1.0, 5.1). See docs/virtio.md.
 **/

#include "VirtioNet.hpp"
#include "NetworkBackend.hpp"
#include "NicAddress.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include <cstring>

CVirtioNet::CVirtioNet(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVirtioPci(cfg, c, pcibus, pcidev, 1, 0x020000, 2, "virtio-net") {
  // struct virtio_net_config, legacy: mac[6], status.
  m_config.assign(8, 0);
  nic_station_address(myCfg, devid_string, &m_config[0]);
  const u16 st = S_LINK_UP;
  memcpy(&m_config[6], &st, 2);
  m_backend = create_network_backend(myCfg);
  if (!m_backend)
    FAILURE(Runtime, "Failed to create network backend");
  if (!m_backend->init(devid_string, myCfg))
    FAILURE(Runtime, "Failed to initialize network backend");
  printf("%s: virtio-net (legacy), receive queue 0, transmit queue 1.\n",
         devid_string);
}

CVirtioNet::~CVirtioNet() {
  stop_threads();
  if (m_backend) {
    m_backend->close();
    delete m_backend;
  }
}

u32 CVirtioNet::device_features() { return F_MAC | F_STATUS | F_INDIRECT_DESC; }

void CVirtioNet::device_reset() { m_pending.clear(); }

// A driver may write the MAC (legacy, without a control queue): the
// backend's filter follows, applied on the device thread.
void CVirtioNet::config_written(u32 off, int size) {
  if (off < 6)
    m_filter_dirty = true;
}

void CVirtioNet::apply_filter() {
  NetworkFilter f = {};
  u8 mac[6];
  {
    std::lock_guard<std::mutex> lk(m_mx);
    memcpy(mac, &m_config[0], 6);
  }
  memcpy(f.mac_list[0], mac, 6);
  memcpy(f.own_mac, mac, 6);
  f.pass_multicast = true; // no control queue: all multicast (and broadcast)
  m_backend->set_filter(f);
}

// Transmit: each buffer is the header (ignored: no offloads were offered)
// and the frame.
void CVirtioNet::process_queue(int q) {
  if (q != TXQ)
    return; // receive buffers are taken by poll()
  Chain c;
  bool any = false;
  while (pop_chain(q, c)) {
    if (c.readable > kHdrLen) {
      const size_t len = c.readable - kHdrLen;
      m_buf.resize(len);
      if (chain_read(c, kHdrLen, m_buf.data(), len) == len)
        m_backend->send(m_buf.data(), (int)len);
    }
    push_used(q, c.head, 0);
    any = true;
  }
  if (any)
    queue_interrupt(q);
}

// Receive: frames from the backend into the driver's buffers, a zeroed
// header first. Before the driver is ready, frames are dropped; while it
// has no buffer, one frame waits (the rest wait in the backend).
void CVirtioNet::poll() {
  if (m_filter_dirty.exchange(false))
    apply_filter();
  const u8 *frame;
  int flen;
  if (!driver_ok() || !queue_ready(RXQ)) {
    while (m_backend->receive(&frame, &flen) > 0) {
    }
    m_pending.clear();
    return;
  }
  Chain c;
  bool any = false;
  for (;;) {
    if (m_pending.empty()) {
      if (m_backend->receive(&frame, &flen) <= 0)
        break;
      m_pending.assign(frame, frame + flen);
    }
    if (!pop_chain(RXQ, c))
      break; // no buffer: keep the frame for the next notify
    const size_t need = kHdrLen + m_pending.size();
    u32 used = 0;
    if (c.writable >= need) {
      const u8 hdr[kHdrLen] = {};
      chain_write(c, 0, hdr, kHdrLen);
      chain_write(c, kHdrLen, m_pending.data(), m_pending.size());
      used = (u32)need;
    } else if (!m_dropped++) {
      printf("%s: receive buffer of %zu bytes too small for a %zu-byte "
             "frame (dropped)\n",
             devid_string, c.writable, need);
    }
    push_used(RXQ, c.head, used);
    m_pending.clear();
    any = true;
  }
  if (any)
    queue_interrupt(RXQ);
}

// ALPHABOX_VIRTIO_SELFTEST=1: transmit a broadcast frame and receive one.
// It needs a backend that brings a frame back: `type = "udp"` with
// udp_local and udp_remote the same address (a loopback), or two virtio_net
// devices wired to each other.
void CVirtioNet::selftest() {
  const u32 base = st_base();
  const u32 RXQUEUE = base, TXQUEUE = base + 0x4000, TXH = base + 0x8000,
            TXF = base + 0x8100, RXBUF = base + 0x10000;
  const u32 kRxLen = kHdrLen + 1514;
  bool pass = true;
  StQueue rx, tx;
  u32 id, len;

  st_reg_write(REG_STATUS, 8, 0);
  pass &= st_say("reset: status 0", st_reg_read(REG_STATUS, 8) == 0);
  st_reg_write(REG_STATUS, 8, S_ACKNOWLEDGE | S_DRIVER);
  const u32 host = st_reg_read(REG_HOST_FEATURES, 32);
  pass &= st_say("features: MAC STATUS",
                 (host & (F_MAC | F_STATUS)) == (F_MAC | F_STATUS));
  st_reg_write(REG_GUEST_FEATURES, 32, F_MAC | F_STATUS);
  u8 mac[6];
  for (int i = 0; i < 6; ++i)
    mac[i] = (u8)st_reg_read(REG_CONFIG + i, 8);
  printf("%%VIRTIO-I-SELFTEST: virtio-net: MAC "
         "%02X-%02X-%02X-%02X-%02X-%02X\n",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  pass &= st_say("config: MAC", !memcmp(mac, &m_config[0], 6));
  pass &= st_say("config: link up",
                 (st_reg_read(REG_CONFIG + 6, 16) & S_LINK_UP) != 0);
  pass &= st_say("queue 0 (receive): 256 entries",
                 st_setup_queue(rx, RXQ, RXQUEUE));
  pass &= st_say("queue 1 (transmit): 256 entries",
                 st_setup_queue(tx, TXQ, TXQUEUE));
  st_reg_write(REG_STATUS, 8, S_ACKNOWLEDGE | S_DRIVER | S_DRIVER_OK);

  // Receive buffers: eight, each header and a full frame in one.
  for (u32 i = 0; i < 8; ++i) {
    memset(st_mem(RXBUF + i * 0x800), 0xee, kRxLen);
    st_submit(rx, {{RXBUF + i * 0x800, kRxLen, true}}, i == 7);
  }

  // A broadcast frame, ethertype 88B5 (local experimental).
  u8 f[64] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
  memcpy(f + 6, mac, 6);
  f[12] = 0x88;
  f[13] = 0xb5;
  const char msg[] = "Alphabox virtio-net self-test";
  memcpy(f + 14, msg, sizeof(msg));
  for (u32 i = 14 + sizeof(msg); i < sizeof(f); ++i)
    f[i] = u8(i);
  memset(st_mem(TXH), 0, kHdrLen);
  memcpy(st_mem(TXF), f, sizeof(f));
  st_submit(tx, {{TXH, kHdrLen, false}, {TXF, sizeof(f), false}});
  bool got = st_wait_used(tx, id, len);
  pass &= st_say("transmit: used, id = head", got && id == 0 && len == 0);
  pass &= st_say("ISR bit 0, INTA asserted",
                 st_wait_irq() && (st_isr_peek() & ISR_QUEUE));

  got = st_wait_used(rx, id, len, 3000);
  const u8 *b = st_mem(RXBUF + id * 0x800);
  pass &= st_say("receive: a frame arrived", got);
  if (got) {
    pass &= st_say("receive: header zeroed, length 10 + 64",
                   len == kHdrLen + sizeof(f) &&
                       !memcmp(b, "\0\0\0\0\0\0\0\0\0\0", kHdrLen));
    pass &= st_say("receive: it is the self-test frame",
                   !memcmp(b + kHdrLen + 12, f + 12, sizeof(f) - 12));
    st_wait_ms(20); // the interrupt follows the used ring
    pass &= st_say("ISR read clears it, INTA drops",
                   (st_reg_read(REG_ISR, 8) & ISR_QUEUE) &&
                       st_reg_read(REG_ISR, 8) == 0 && !st_irq());
  } else {
    printf("%%VIRTIO-I-SELFTEST: virtio-net: nothing came back -- the "
           "backend must return frames (type = \"udp\", udp_local = "
           "udp_remote)\n");
  }

  st_reg_write(REG_STATUS, 8, 0);
  st_reg_write(REG_QUEUE_SEL, 16, 1);
  pass &= st_say("reset: queue PFN 0", st_reg_read(REG_QUEUE_PFN, 32) == 0);
  printf("%%VIRTIO-I-SELFTEST: virtio-net: %s\n", pass ? "PASS" : "FAIL");
}
