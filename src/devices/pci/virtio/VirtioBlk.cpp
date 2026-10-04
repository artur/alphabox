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
 * virtio-blk, legacy interface (OASIS virtio 1.0, 5.2). See docs/virtio.md.
 **/

#include "VirtioBlk.hpp"
#include "Disk.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include <algorithm>
#include <cstring>

static constexpr u32 kSector = 512;

CVirtioBlk::CVirtioBlk(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CVirtioPci(cfg, c, pcibus, pcidev, 2, 0x018000, 1, "virtio-blk"),
      CDiskController(1, 1) {
  fill_config();
  number_disk_serials(false); // GET_ID shows the guest the serial
  printf("%s: virtio-blk (legacy), 1 queue of %d.\n", devid_string, kQueueSize);
}

void CVirtioBlk::register_disk(CDisk *dsk, int bus, int dev) {
  if (bus != 0 || dev != 0)
    FAILURE(Configuration, "virtio_blk has one disk: disk0.0");
  CDiskController::register_disk(dsk, bus, dev);
  std::lock_guard<std::mutex> work(m_work);
  std::lock_guard<std::mutex> lk(m_mx);
  m_disk = dsk; // mounted after this: the size is read when asked for
  printf("%s: disk0.0%s.\n", devid_string, dsk->ro() ? ", read-only" : "");
}

// struct virtio_blk_config, legacy: capacity, size_max, seg_max, geometry,
// blk_size -- 24 bytes, little-endian.
void CVirtioBlk::fill_config() {
  m_config.assign(24, 0);
  const u64 cap = m_disk ? u64(m_disk->get_byte_size()) / kSector : 0;
  const u32 size_max = kSizeMax, seg_max = kSegMax;
  const u32 blk = m_disk ? u32(m_disk->get_block_size()) : kSector;
  const u16 cyl =
      m_disk ? (u16)std::min<off_t_large>(m_disk->get_cylinders(), 65535) : 0;
  const u8 heads = m_disk ? (u8)std::min<long>(m_disk->get_heads(), 255) : 0;
  const u8 secs = m_disk ? (u8)std::min<long>(m_disk->get_sectors(), 255) : 0;
  memcpy(&m_config[0], &cap, 8);
  memcpy(&m_config[8], &size_max, 4);
  memcpy(&m_config[12], &seg_max, 4);
  memcpy(&m_config[16], &cyl, 2);
  m_config[18] = heads;
  m_config[19] = secs;
  memcpy(&m_config[20], &blk, 4);
}

u32 CVirtioBlk::device_features() {
  u32 f = F_SIZE_MAX | F_SEG_MAX | F_GEOMETRY | F_BLK_SIZE | F_FLUSH |
          F_INDIRECT_DESC;
  if (m_disk && m_disk->ro())
    f |= F_RO;
  return f;
}

void CVirtioBlk::process_queue(int q) {
  Chain c;
  bool any = false;
  while (pop_chain(q, c)) {
    u32 written = 0;
    if (!c.segs.empty() && c.writable) {
      const u8 st = request(c, written);
      chain_write(c, c.writable - 1, &st, 1); // the status byte is last
      written += 1;
    }
    push_used(q, c.head, written);
    any = true;
  }
  if (any)
    queue_interrupt(q);
}

// One request: a 16-byte header (type, reserved, sector) the device reads,
// data, and a status byte the device writes. `written` counts the data
// bytes put in the driver's buffers.
u8 CVirtioBlk::request(const Chain &c, u32 &written) {
  u8 hdr[16];
  if (chain_read(c, 0, hdr, 16) != 16)
    return S_IOERR;
  u32 type;
  u64 sector;
  memcpy(&type, hdr, 4);
  memcpy(&sector, hdr + 8, 8);
  const u64 disk_bytes = m_disk ? u64(m_disk->get_byte_size()) : 0;

  switch (type) {
  case T_IN: {
    const size_t len = c.writable - 1;
    if (len % kSector || !m_disk || sector > disk_bytes / kSector ||
        len > disk_bytes - sector * kSector)
      return S_IOERR;
    m_buf.resize(len);
    if (!m_disk->seek_byte(sector * kSector) ||
        m_disk->read_bytes(m_buf.data(), len) != len)
      return S_IOERR;
    written = (u32)chain_write(c, 0, m_buf.data(), len);
    return written == len ? S_OK : S_IOERR;
  }
  case T_OUT: {
    const size_t len = c.readable - 16;
    if (!m_disk || m_disk->ro() || len % kSector ||
        sector > disk_bytes / kSector || len > disk_bytes - sector * kSector)
      return S_IOERR;
    m_buf.resize(len);
    if (chain_read(c, 16, m_buf.data(), len) != len)
      return S_IOERR;
    if (!m_disk->seek_byte(sector * kSector) ||
        m_disk->write_bytes(m_buf.data(), len) != len)
      return S_IOERR;
    return S_OK;
  }
  case T_FLUSH:
    if (m_disk)
      m_disk->flush();
    return S_OK;
  case T_GET_ID: {
    // VIRTIO_BLK_ID_BYTES: the serial number, NUL-padded (not terminated
    // when it fills all 20).
    char id[20] = {};
    if (m_disk)
      strncpy(id, m_disk->get_serial(), sizeof(id));
    const size_t n = std::min<size_t>(sizeof(id), c.writable - 1);
    written = (u32)chain_write(c, 0, id, n);
    return S_OK;
  }
  default:
    return S_UNSUPP;
  }
}

// ALPHABOX_VIRTIO_SELFTEST=1: the device driven through its registers by a
// minimal legacy driver, before any firmware runs. Needs a disk whose
// sector 0 ends in 55 AA; the last sector is written and put back.
void CVirtioBlk::selftest() {
  const u32 base = st_base();
  const u32 QUEUE = base, HDR = base + 0x8000, STAT = base + 0x8100,
            DATA = base + 0x10000, SAVE = base + 0x20000, INDIR = base + 0x9000;
  bool pass = true;
  auto hdr = [&](u32 at, u32 type, u64 sector) {
    u8 h[16] = {};
    memcpy(h, &type, 4);
    memcpy(h + 8, &sector, 8);
    memcpy(st_mem(at), h, 16);
  };
  StQueue sq;
  u32 id, len;
  u16 last_head = 0;
  // One request: header, optional data, status; returns the status byte,
  // or -1 when the device never answered.
  auto req = [&](u32 type, u64 sector, u32 data, u32 dlen, bool dev_writes,
                 u32 *used_len = nullptr) {
    hdr(HDR, type, sector);
    *st_mem(STAT) = 0xff;
    std::vector<StDesc> d = {{HDR, 16, false}};
    if (dlen)
      d.push_back({data, dlen, dev_writes});
    d.push_back({STAT, 1, true});
    last_head = st_submit(sq, d);
    if (!st_wait_used(sq, id, len) || id != last_head)
      return -1;
    if (used_len)
      *used_len = len;
    return (int)*st_mem(STAT);
  };

  // Reset and feature negotiation.
  st_reg_write(REG_STATUS, 8, 0);
  pass &= st_say("reset: status 0", st_reg_read(REG_STATUS, 8) == 0);
  st_reg_write(REG_STATUS, 8, S_ACKNOWLEDGE | S_DRIVER);
  const u32 host = st_reg_read(REG_HOST_FEATURES, 32);
  const u32 want = F_SIZE_MAX | F_SEG_MAX | F_BLK_SIZE | F_FLUSH;
  pass &= st_say("features: SIZE_MAX SEG_MAX BLK_SIZE FLUSH",
                 (host & want) == want);
  st_reg_write(REG_GUEST_FEATURES, 32, host);
  pass &=
      st_say("features accepted", st_reg_read(REG_GUEST_FEATURES, 32) == host);
  const bool ro = (host & F_RO) != 0;
  u64 cap = st_reg_read(REG_CONFIG, 32) |
            (u64(st_reg_read(REG_CONFIG + 4, 32)) << 32);
  pass &= st_say("config: capacity", cap > 0);
  pass &= st_say("config: blk_size 512",
                 st_reg_read(REG_CONFIG + 20, 32) == kSector);
  pass &= st_say("queue 0: 256 entries", st_setup_queue(sq, 0, QUEUE));
  st_reg_write(REG_QUEUE_SEL, 16, 1);
  pass &= st_say("queue 1: absent", st_reg_read(REG_QUEUE_SIZE, 16) == 0);
  st_reg_write(REG_STATUS, 8, S_ACKNOWLEDGE | S_DRIVER | S_DRIVER_OK);

  // GET_ID.
  u32 ulen = 0;
  memset(st_mem(DATA), 0, 20);
  int st = req(T_GET_ID, 0, DATA, 20, true, &ulen);
  char idbuf[21] = {};
  memcpy(idbuf, st_mem(DATA), 20);
  printf("%%VIRTIO-I-SELFTEST: virtio-blk: id \"%s\", %llu sectors%s\n", idbuf,
         (unsigned long long)cap, ro ? ", read-only" : "");
  pass &= st_say("GET_ID", st == S_OK && idbuf[0] && ulen == 21);

  // Sector 0, and the interrupt it raises.
  memset(st_mem(DATA), 0, kSector);
  st = req(T_IN, 0, DATA, kSector, true, &ulen);
  pass &=
      st_say("IN sector 0 (55 AA)", st == S_OK && st_mem(DATA)[510] == 0x55 &&
                                        st_mem(DATA)[511] == 0xaa);
  pass &= st_say("used: id = head, len = 513", id == last_head && ulen == 513);
  pass &= st_say("ISR bit 0, INTA asserted",
                 st_wait_irq() && (st_isr_peek() & ISR_QUEUE));
  pass &= st_say("ISR read clears it", (st_reg_read(REG_ISR, 8) & ISR_QUEUE) &&
                                           st_reg_read(REG_ISR, 8) == 0 &&
                                           !st_irq());

  // The same sector through an indirect table.
  {
    u8 t[48] = {};
    const u64 a0 = st_bus(HDR), a1 = st_bus(DATA + 0x1000), a2 = st_bus(STAT);
    const u32 l0 = 16, l1 = kSector, l2 = 1;
    const u16 f0 = D_NEXT, f1 = D_NEXT | D_WRITE, f2 = D_WRITE;
    const u16 n0 = 1, n1 = 2;
    memcpy(t, &a0, 8), memcpy(t + 8, &l0, 4), memcpy(t + 12, &f0, 2),
        memcpy(t + 14, &n0, 2);
    memcpy(t + 16, &a1, 8), memcpy(t + 24, &l1, 4), memcpy(t + 28, &f1, 2),
        memcpy(t + 30, &n1, 2);
    memcpy(t + 32, &a2, 8), memcpy(t + 40, &l2, 4), memcpy(t + 44, &f2, 2);
    memcpy(st_mem(INDIR), t, 48);
    hdr(HDR, T_IN, 0);
    *st_mem(STAT) = 0xff;
    memset(st_mem(DATA + 0x1000), 0, kSector);
    // One descriptor flagged INDIRECT, written by hand.
    u8 e[16] = {};
    const u64 ia = st_bus(INDIR);
    const u32 il = 48;
    const u16 fl = D_INDIRECT;
    memcpy(e, &ia, 8), memcpy(e + 8, &il, 4), memcpy(e + 12, &fl, 2);
    const u16 head = sq.next_desc;
    memcpy(st_mem(sq.desc + 16 * head), e, 16);
    sq.next_desc = u16((head + 1) % kQueueSize);
    memcpy(st_mem(sq.avail + 4 + 2 * (sq.avail_idx % kQueueSize)), &head, 2);
    std::atomic_thread_fence(std::memory_order_release);
    sq.avail_idx++;
    memcpy(st_mem(sq.avail + 2), &sq.avail_idx, 2);
    st_reg_write(REG_QUEUE_NOTIFY, 16, 0);
    const bool got = st_wait_used(sq, id, len);
    pass &= st_say("IN sector 0 via an indirect table",
                   got && id == head && len == 513 && *st_mem(STAT) == S_OK &&
                       !memcmp(st_mem(DATA + 0x1000), st_mem(DATA), kSector));
    st_wait_irq();
    st_reg_read(REG_ISR, 8);
  }

  // Write the last sector, read it back, put it back, flush.
  const u64 last = cap - 1;
  st = req(T_IN, last, SAVE, kSector, true);
  pass &= st_say("IN last sector", st == S_OK);
  for (u32 i = 0; i < kSector; ++i)
    st_mem(DATA)[i] = u8(i * 7 + 0x5a);
  st = req(T_OUT, last, DATA, kSector, false, &ulen);
  if (ro) {
    pass &= st_say("OUT refused (read-only image)", st == S_IOERR);
  } else {
    pass &= st_say("OUT last sector", st == S_OK && ulen == 1);
    memset(st_mem(DATA + 0x1000), 0, kSector);
    st = req(T_IN, last, DATA + 0x1000, kSector, true);
    pass &= st_say("IN it again: same data",
                   st == S_OK &&
                       !memcmp(st_mem(DATA), st_mem(DATA + 0x1000), kSector));
    st = req(T_OUT, last, SAVE, kSector, false);
    pass &= st_say("OUT: original put back", st == S_OK);
  }
  st = req(T_FLUSH, 0, 0, 0, false, &ulen);
  pass &= st_say("FLUSH", st == S_OK && ulen == 1);

  // Errors: past the end, an unknown type.
  st = req(T_IN, cap, DATA, kSector, true);
  pass &= st_say("IN past the end: IOERR", st == S_IOERR);
  st = req(99, 0, 0, 0, false);
  pass &= st_say("unknown type: UNSUPP", st == S_UNSUPP);
  st_wait_irq();
  st_reg_read(REG_ISR, 8);

  // The driver asks for no interrupts: none comes.
  const u16 noint = AVAIL_F_NO_INTERRUPT;
  memcpy(st_mem(sq.avail), &noint, 2);
  st = req(T_GET_ID, 0, DATA, 20, true);
  st_wait_ms(20);
  pass &= st_say("NO_INTERRUPT honoured",
                 st == S_OK && !st_irq() && st_reg_read(REG_ISR, 8) == 0);

  // Reset: queues forgotten.
  st_reg_write(REG_STATUS, 8, 0);
  st_reg_write(REG_QUEUE_SEL, 16, 0);
  pass &= st_say("reset: queue PFN 0", st_reg_read(REG_QUEUE_PFN, 32) == 0);
  printf("%%VIRTIO-I-SELFTEST: virtio-blk: %s\n", pass ? "PASS" : "FAIL");
}
