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
 * The Radeon's command processor (CP): the ring buffer, indirect buffers
 * and the programmed-I/O command queue, and the packets they carry.
 *
 * A driver can program the engine through the registers (RadeonEngine.cpp)
 * or hand the CP a stream of packets: through the ring buffer in memory
 * the chip reads by bus mastering (CP_RB_BASE, CP_RB_CNTL's size, the
 * driver's CP_RB_WPTR and the chip's CP_RB_RPTR, written back to
 * CP_RB_RPTR_ADDR), through indirect buffers the ring points at
 * (CP_IB_BASE, CP_IB_BUFSZ) or by writing them to CP_CSQ_APER_PRIMARY.
 * OpenVMS's DECwindows server uses the ring.
 *
 * The packets (Linux's r100d.h and radeon_reg.h):
 *   type 0: <29:16> count less one, <15> one register, <12:0> register
 *           index: register writes;
 *   type 1: two registers, <10:0> and <21:11>, one dword each;
 *   type 2: a filler;
 *   type 3: <29:16> count less one, <15:8> the operation: the 2D
 *           operations (see cp_packet3), NOP and WAIT_FOR_IDLE, and the
 *           3D draws (RadeonR100_3D.cpp).
 * (AMD's "Radeon R5xx Acceleration" v1.5, 6.1, documents the four
 * types, as the R100 microcode already had them.)
 * A 2D operation starts with DP_GUI_MASTER_CNTL; its bits say which
 * fields follow (source and destination pitch/offset, the clip
 * rectangles, the brush and source colours), then the operation's own.
 *
 * The CP runs on the engine's thread (RadeonQueue.cpp): a write to
 * CP_RB_WPTR wakes it and returns; the engine reads the ring a packet at
 * a time when its FIFO is empty, the read pointer advancing as it goes,
 * and writes its copy back to CP_RB_RPTR_ADDR. Which streams run is the
 * CSQ mode's (CP_CSQ_CNTL, CP_CSQ_MODE): the ring when the primary
 * stream bus masters, the PIO queue when it is pushed, indirect buffers
 * when the indirect stream bus masters.
 *
 * The micro-engine. The CP parses packets with microcode a driver loads
 * into its RAM (CP_ME_RAM_ADDR, then DATAH/DATAL pairs; R5xx
 * Acceleration 5.12). ATI never documented the micro-engine's
 * instruction set, so the packets are interpreted here natively; what
 * the model keeps of the micro-engine is that without microcode there is
 * no CP: until a complete image has been loaded (every entry written, not
 * all zero) neither the ring nor the PIO queue is read, and the read
 * pointer stays where it is. The image is kept and reads back.
 *
 * Memory the CP reads is addressed in the memory controller's space
 * (RadeonGart.cpp): the framebuffer, the AGP window or the PCI GART.
 **/

#include "Radeon.hpp"
#include "System.hpp"

#include <algorithm>

using namespace radeon;

namespace {
constexpr u32 CP_RB_BASE = 0x0700;
constexpr u32 CP_RB_CNTL = 0x0704;
constexpr u32 CP_RB_RPTR_ADDR = 0x070c;
constexpr u32 CP_RB_RPTR = 0x0710;
constexpr u32 CP_RB_WPTR = 0x0714;
constexpr u32 CP_RB_RPTR_WR = 0x071c;
constexpr u32 CP_IB_BASE = 0x0738;
constexpr u32 CP_IB_BUFSZ = 0x073c;
constexpr u32 SCRATCH_UMSK = 0x0770;
constexpr u32 SCRATCH_ADDR = 0x0774;
constexpr u32 CP_ME_RAM_ADDR = 0x07d4;
constexpr u32 CP_ME_RAM_RADDR = 0x07d8;
constexpr u32 CP_ME_RAM_DATAH = 0x07dc;
constexpr u32 CP_ME_RAM_DATAL = 0x07e0;
constexpr u32 CP_CSQ_MODE = 0x0744;
constexpr u32 CP_STAT = 0x07c0;
constexpr u32 CP_CSQ_APER_PRIMARY = 0x1000;
constexpr u32 CP_CSQ_APER_END = 0x1200;
constexpr u32 SCRATCH_REG0 = 0x15e0;
constexpr u32 SCRATCH_REG5 = 0x15f4;

constexpr u32 RB_NO_UPDATE = 1u << 27;
constexpr u32 RB_RPTR_WR_ENA = 1u << 31;

// engine registers a packet loads
constexpr u32 DP_GUI_MASTER_CNTL = 0x146c;
constexpr u32 SRC_PITCH_OFFSET = 0x1428;
constexpr u32 DST_PITCH_OFFSET = 0x142c;
constexpr u32 SRC_SC_BOTTOM_RIGHT = 0x16f4;
constexpr u32 SC_TOP_LEFT = 0x16ec;
constexpr u32 SC_BOTTOM_RIGHT = 0x16f0;
constexpr u32 DP_BRUSH_BKGD_CLR = 0x1478;
constexpr u32 DP_BRUSH_FRGD_CLR = 0x147c;
constexpr u32 BRUSH_DATA0 = 0x1480;
constexpr u32 DP_SRC_FRGD_CLR = 0x15d8;
constexpr u32 DP_SRC_BKGD_CLR = 0x15dc;
constexpr u32 DST_Y_X = 0x1438;
constexpr u32 DST_HEIGHT_WIDTH = 0x143c;
constexpr u32 SRC_X_Y = 0x1590;
constexpr u32 DST_X_Y = 0x1594;
constexpr u32 DST_WIDTH_HEIGHT = 0x1598;
constexpr u32 DST_LINE_START = 0x1600;
constexpr u32 DST_LINE_END = 0x1604;
constexpr u32 HOST_DATA0 = 0x17c0;
constexpr u32 HOST_DATA_LAST = 0x17e0;
constexpr u32 BRUSH_Y_X = 0x1474;
constexpr u32 CLR_CMP_CNTL = 0x15c0;
constexpr u32 CLR_CMP_CLR_SRC = 0x15c4;
constexpr u32 CLR_CMP_CLR_DST = 0x15c8;

// type-3 operations
enum : u8 {
  OP_NOP = 0x10,
  OP_NEXT_CHAR = 0x19,
  OP_WAIT_FOR_IDLE = 0x26,
  OP_LOAD_MICROCODE = 0x24,
  OP_CNTL_PAINT = 0x91,
  OP_CNTL_BITBLT = 0x92,
  OP_CNTL_SMALLTEXT = 0x93,
  OP_CNTL_HOSTDATA_BLT = 0x94,
  OP_CNTL_POLYLINE = 0x95,
  OP_CNTL_POLYSCANLINES = 0x98,
  OP_CNTL_PAINT_MULTI = 0x9a,
  OP_CNTL_BITBLT_MULTI = 0x9b,
  OP_CNTL_TRANS_BITBLT = 0x9c,
  OP_PLY_NEXTSCAN = 0x1d,
  OP_SET_SCISSORS = 0x1e
};

/// How many brush dwords a 2D packet carries, by brush type, and which
/// registers they load: (background,) foreground, then pattern data.
int brush_fields(u32 type, u32 *regs) {
  int n = 0;
  auto add = [&](u32 r) { regs[n++] = r; };
  switch (type) {
  case 0: // 8x8 mono, foreground and background
    add(DP_BRUSH_BKGD_CLR);
    add(DP_BRUSH_FRGD_CLR);
    add(BRUSH_DATA0);
    add(BRUSH_DATA0 + 4);
    break;
  case 1: // 8x8 mono, foreground, background left alone
    add(DP_BRUSH_FRGD_CLR);
    add(BRUSH_DATA0);
    add(BRUSH_DATA0 + 4);
    break;
  case 4: // 1x8 mono
  case 6: // 32x1 mono
    add(DP_BRUSH_BKGD_CLR);
    add(DP_BRUSH_FRGD_CLR);
    add(BRUSH_DATA0);
    break;
  case 5:
  case 7:
    add(DP_BRUSH_FRGD_CLR);
    add(BRUSH_DATA0);
    break;
  case 8: // 32x32 mono
  case 9:
    if (type == 8)
      add(DP_BRUSH_BKGD_CLR);
    add(DP_BRUSH_FRGD_CLR);
    for (int i = 0; i < 32; i++)
      add(BRUSH_DATA0 + 4 * u32(i));
    break;
  case 13: // solid
  case 14: // solid (R5xx Acceleration 6.2.2, BRUSH_PACKET table)
    add(DP_BRUSH_FRGD_CLR);
    break;
  default: // none; the 8x8 colour brush is sized by the destination
    break;
  }
  return n;
}

/// The bytes a pixel of a DP_DATATYPE destination type takes (24 bpp
/// pixels count as 4 in a colour brush packet).
int datatype_bytes(u32 t) {
  switch (t & 0xf) {
  case 3:
  case 4:
  case 11:
  case 12:
  case 15:
    return 2;
  case 5:
  case 6:
  case 14:
    return 4;
  default:
    return 1;
  }
}
} // namespace

u32 CRadeon::cp_read32(u32 mc) {
  bool vram;
  u32 a;
  if (!cp_translate(mc, &vram, &a)) {
    if (m_cp_bad_reads++ < 8)
      printf("%s: CP read outside the framebuffer, AGP window and GART: "
             "%08x\n",
             devid_string, mc);
    return 0x80000000u; // a type-2 filler
  }
  if (vram)
    return vram_read(a, 4);
  u32 v = 0;
  do_pci_read(a, &v, 4, 1);
  return v;
}

void CRadeon::cp_write32(u32 mc, u32 data) {
  bool vram;
  u32 a;
  if (!cp_translate(mc, &vram, &a))
    return;
  if (vram) {
    vram_write(a, 4, data);
    return;
  }
  do_pci_write(a, &data, 4, 1);
}

/// The swaps of CP_RB_CNTL BUF_SWAP, CP_RB_RPTR_ADDR RB_RPTR_SWAP and
/// SCRATCH_UMSK SCRATCH_SWAP (R5xx Acceleration, CP_RB_CNTL): 1 the bytes
/// of each half, 2 all four, 3 the halves.
static u32 cp_swap(u32 v, u32 mode) {
  switch (mode & 3) {
  case 1:
    return ((v & 0x00ff00ffu) << 8) | ((v >> 8) & 0x00ff00ffu);
  case 2:
    return (v << 24) | ((v & 0xff00) << 8) | ((v >> 8) & 0xff00) | (v >> 24);
  case 3:
    return (v << 16) | (v >> 16);
  }
  return v;
}

u32 CRadeon::cp_fetch(u32 mc) {
  bool vram;
  u32 a;
  const u32 v = cp_read32(mc);
  if (cp_translate(mc, &vram, &a) && !vram)
    return cp_swap(v, R(CP_RB_CNTL) >> 16);
  return v;
}

/**
 * The CSQ mode: CP_CSQ_CNTL<31:28> (0 off; 1, 3, 5, 7 primary by PIO; 2,
 * 4, 6, 8 primary by bus mastering; 3-8 indirect buffers by bus
 * mastering; 15 both by PIO), unless CP_CSQ_MODE's enables <31>, <29>
 * override it (R5xx Acceleration, CP_CSQ_CNTL and CP_CSQ_MODE).
 **/
u32 CRadeon::cp_csq_mode() const { return R(CP_CSQ_CNTL) >> 28; }

bool CRadeon::cp_primary_bm() const {
  const u32 m = R(CP_CSQ_MODE);
  if (m & (1u << 31))
    return (m >> 30) & 1;
  const u32 c = cp_csq_mode();
  return c == 2 || c == 4 || c == 6 || c == 8;
}

bool CRadeon::cp_primary_pio() const {
  const u32 m = R(CP_CSQ_MODE);
  if (m & (1u << 31))
    return !((m >> 30) & 1);
  const u32 c = cp_csq_mode();
  return c == 1 || c == 3 || c == 5 || c == 7 || c == 15;
}

bool CRadeon::cp_indirect_bm() const {
  const u32 m = R(CP_CSQ_MODE);
  if (m & (1u << 29))
    return (m >> 28) & 1;
  const u32 c = cp_csq_mode();
  return c >= 3 && c <= 8;
}

/// The ring's size in dwords: 2^(RB_BUFSZ+1), RB_BUFSZ clamped to 2..22.
static u32 ring_dwords(u32 cntl) {
  const u32 b = std::min(22u, std::max(2u, cntl & 0x3f));
  return 2u << b;
}

bool CRadeon::cp_ring_pending() const {
  if (!cp_primary_bm())
    return false;
  const u32 mask = ring_dwords(R(CP_RB_CNTL)) - 1;
  return (R(CP_RB_WPTR) & mask) != (R(CP_RB_RPTR) & mask);
}

bool CRadeon::cp_ring_has_work() const {
  return cp_microcode_ok() && cp_ring_pending();
}

/**
 * The CP's registers. Returns true when the register is the CP's and
 * the write was handled.
 **/
bool CRadeon::cp_reg_write(u32 reg, u32 data) {
  if (reg >= CP_CSQ_APER_PRIMARY && reg < CP_CSQ_APER_END) {
    // The primary PIO queue: the dwords go to the CP in the order written
    // (R5xx Acceleration 5.11), while the CSQ is in a primary PIO mode.
    if (!cp_primary_pio()) {
      if (!m_cp_warned_pio) {
        m_cp_warned_pio = true;
        printf("%s: CP PIO write with the primary queue not in PIO mode "
               "(CP_CSQ_CNTL %08x): ignored\n",
               devid_string, R(CP_CSQ_CNTL));
      }
      return true;
    }
    if (in_engine() || m_sync) {
      m_csq.push_back(data);
      engine_kick();
      return true;
    }
    {
      std::unique_lock<std::mutex> l(m_q_mx);
      while (m_csq.size() >= m_chip->csq_primary_dwords && m_eng_thread &&
             !m_eng_stop && cp_microcode_ok())
        m_q_space.wait_for(l, std::chrono::milliseconds(100));
      m_csq.push_back(data);
    }
    engine_kick();
    return true;
  }
  switch (reg) {
  case CP_RB_WPTR: {
    // RB_RPTR_WR_ENA: the writable read pointer becomes the read pointer
    // when the write pointer is written (R5xx Acceleration 5.8).
    if (R(CP_RB_CNTL) & RB_RPTR_WR_ENA) {
      std::lock_guard<std::mutex> x(m_exec_mx);
      R(CP_RB_RPTR) = R(CP_RB_RPTR_WR);
      m_rptr_since_wb = 0;
    }
    R(reg) = data;
    engine_kick();
    return true;
  }
  case CP_RB_RPTR_WR:
    R(reg) = data;
    return true;
  case CP_RB_RPTR:
    return true; // the chip's
  case CP_CSQ_CNTL:
  case CP_CSQ_MODE:
    R(reg) = data;
    engine_kick();
    return true;
  case CP_IB_BUFSZ:
    R(reg) = data;
    if (!in_engine()) {
      // a CPU's write (PIO mode): the buffer is read now, by the engine
      std::lock_guard<std::mutex> x(m_exec_mx);
      engine_scope scope;
      cp_run_buffer(R(CP_IB_BASE), data & 0x7fffff);
      return true;
    }
    cp_run_buffer(R(CP_IB_BASE), data & 0x7fffff);
    return true;
  case CP_ME_RAM_ADDR:
  case CP_ME_RAM_RADDR:
    m_me_index = data % m_chip->me_ram_entries;
    R(reg) = data;
    return true;
  case CP_ME_RAM_DATAH:
    // the high 8 bits of a 40-bit micro-engine word (R5xx 5.12)
    m_me_ram[m_me_index][0] = data & 0xff;
    return true;
  case CP_ME_RAM_DATAL: {
    m_me_ram[m_me_index][1] = data;
    m_me_written[m_me_index >> 5] |= 1u << (m_me_index & 31);
    m_me_index = (m_me_index + 1) % m_chip->me_ram_entries;
    // A complete image: every entry loaded, and not all of them zero.
    // The R100 microcode is a firmware blob (Linux radeon/R100_cp.bin);
    // nothing in the sources identifies it by content, so any complete
    // load is taken [inference]. Its CRC is printed to tell images apart.
    bool all = true, any = false;
    for (u32 i = 0; i < m_chip->me_ram_entries; i++) {
      all = all && ((m_me_written[i >> 5] >> (i & 31)) & 1);
      any = any || m_me_ram[i][0] || m_me_ram[i][1];
    }
    const bool was = m_me_loaded;
    m_me_loaded = all && any;
    if (m_me_loaded && !was) {
      u32 crc = 0xffffffffu;
      for (u32 i = 0; i < m_chip->me_ram_entries; i++)
        for (int k = 0; k < 2; k++)
          for (int b = 0; b < 32; b += 8) {
            crc ^= (m_me_ram[i][k] >> b) & 0xff;
            for (int j = 0; j < 8; j++)
              crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
          }
      printf("%s: CP microcode loaded, %u entries, CRC-32 %08x\n", devid_string,
             m_chip->me_ram_entries, ~crc);
      engine_kick();
    }
    return true;
  }
  }
  if (reg >= SCRATCH_REG0 && reg <= SCRATCH_REG5) {
    // A scratch register is a fence: written back to memory when its
    // SCRATCH_UMSK bit is set, swapped as SCRATCH_SWAP <17:16> says.
    R(reg) = data;
    const u32 n = (reg - SCRATCH_REG0) / 4;
    const u32 umsk = R(SCRATCH_UMSK);
    if (umsk & (1u << n)) {
      bool vram;
      u32 a;
      const u32 mc = R(SCRATCH_ADDR) + 4 * n;
      const bool host = cp_translate(mc, &vram, &a) && !vram;
      cp_write32(mc, host ? cp_swap(data, umsk >> 16) : data);
    }
    return true;
  }
  return false;
}

bool CRadeon::cp_reg_read(u32 reg, u32 *v) {
  switch (reg) {
  case CP_ME_RAM_DATAH:
    *v = m_me_ram[m_me_index][0];
    return true;
  case CP_ME_RAM_DATAL:
    *v = m_me_ram[m_me_index][1];
    m_me_index = (m_me_index + 1) % m_chip->me_ram_entries;
    return true;
  case CP_CSQ_CNTL: {
    // CSQ_CNT_PRIMARY <7:0>: the dwords the primary PIO queue has room
    // for (r100d.h)
    size_t q;
    {
      std::lock_guard<std::mutex> l(m_q_mx);
      q = m_csq.size();
    }
    const u32 room =
        cp_primary_pio()
            ? m_chip->csq_primary_dwords -
                  u32(std::min<size_t>(q, m_chip->csq_primary_dwords))
            : 0;
    *v = (R(reg) & ~0xffu) | std::min(room, 0xffu);
    return true;
  }
  case CP_CSQ_STAT: {
    // the primary queue's read and write pointers <7:0>, <15:8>
    // (radeon_reg.h): equal when it is drained
    size_t q;
    {
      std::lock_guard<std::mutex> l(m_q_mx);
      q = m_csq.size();
    }
    *v = u32(q & 0xff) << 8;
    return true;
  }
  case 0x07fc: // CP_CSQ2_STAT
    *v = 0;
    return true;
  case CP_STAT: {
    // CP_BUSY <31>, CMDSTRM_BUSY <30>, CSF_PRIMARY_BUSY <9> (r100d.h)
    bool pio;
    {
      std::lock_guard<std::mutex> l(m_q_mx);
      pio = !m_csq.empty();
    }
    *v = (cp_ring_pending() || pio) ? 0xc0000200u : 0;
    return true;
  }
  }
  return false;
}

/**
 * The ring, as the engine reads it: from the read pointer to the end of
 * the next packet (or the write pointer, if the packet is not all there
 * yet). The read pointer is the chip's and moves as dwords are read; the
 * host's copy at CP_RB_RPTR_ADDR is written every 2^RB_BLKSZ quadwords
 * and when the ring runs dry, unless RB_NO_UPDATE (R5xx Acceleration,
 * CP_RB_CNTL).
 **/
void CRadeon::cp_ring_step() {
  if (!cp_ring_has_work())
    return;
  const u32 cntl = R(CP_RB_CNTL);
  const u32 mask = ring_dwords(cntl) - 1;
  const u32 block = 2u << std::min(21u, (cntl >> 8) & 0x3f); // dwords
  u32 rptr = R(CP_RB_RPTR) & mask;
  for (int n = 0; n < 65536; n++) {
    const u32 wptr = R(CP_RB_WPTR) & mask;
    if (rptr == wptr)
      break;
    const u32 d = cp_fetch(R(CP_RB_BASE) + rptr * 4);
    rptr = (rptr + 1) & mask;
    R(CP_RB_RPTR) = rptr;
    cp_feed(d);
    if (++m_rptr_since_wb >= block)
      cp_rptr_writeback();
    if (m_cp.remaining == 0)
      break; // a packet's end
  }
  if ((R(CP_RB_WPTR) & mask) == rptr)
    cp_rptr_writeback();
}

void CRadeon::cp_rptr_writeback() {
  m_rptr_since_wb = 0;
  if (R(CP_RB_CNTL) & RB_NO_UPDATE)
    return;
  const u32 addr = R(CP_RB_RPTR_ADDR);
  bool vram;
  u32 a;
  const u32 mc = addr & ~3u;
  const bool host = cp_translate(mc, &vram, &a) && !vram;
  const u32 v = R(CP_RB_RPTR);
  cp_write32(mc, host ? cp_swap(v, addr) : v);
}

/**
 * RBBM_SOFT_RESET's CP bit: the parser forgets the packet in progress and
 * the read pointer returns to zero (R5xx Acceleration 5.8: "the read
 * pointer still resets to zero"). The microcode stays loaded.
 **/
void CRadeon::cp_soft_reset() {
  m_cp = cp_parser{};
  R(CP_RB_RPTR) = 0;
  m_rptr_since_wb = 0;
  std::lock_guard<std::mutex> l(m_q_mx);
  m_csq.clear();
}

/**
 * An indirect buffer: `dwords` dwords from `mc`, read when the write to
 * CP_IB_BUFSZ that ends a type-0 packet reaches the CP (R5xx Acceleration
 * 5.10), while the CSQ mode lets the indirect stream bus master.
 **/
void CRadeon::cp_run_buffer(u32 mc, u32 dwords) {
  if (!cp_indirect_bm()) {
    if (!m_cp_warned_ib) {
      m_cp_warned_ib = true;
      printf("%s: indirect buffer with the indirect stream disabled "
             "(CP_CSQ_CNTL %08x): not read\n",
             devid_string, R(CP_CSQ_CNTL));
    }
    return;
  }
  if (m_cp_ib_depth > 2)
    return;
  m_cp_ib_depth++;
  // The ring's packet in progress (the one that wrote CP_IB_BUFSZ) is
  // complete; the buffer's packets have a parser of their own.
  const cp_parser saved = m_cp;
  m_cp = cp_parser{};
  for (u32 i = 0; i < dwords; i++)
    cp_feed(cp_fetch(mc + i * 4));
  m_cp = saved;
  m_cp_ib_depth--;
}

/**
 * One dword of the packet stream.
 **/
void CRadeon::cp_feed(u32 d) {
  cp_parser &p = m_cp;
  if (p.remaining == 0) {
    p.header = d;
    switch (d >> 30) {
    case 0:
      p.remaining = ((d >> 16) & 0x3fff) + 1;
      p.reg = (d & 0x1fff) << 2;
      return;
    case 1:
      p.remaining = 2;
      p.reg = (d & 0x7ff) << 2;
      p.reg1 = ((d >> 11) & 0x7ff) << 2;
      return;
    case 2:
      return;
    default:
      p.remaining = ((d >> 16) & 0x3fff) + 1;
      p.payload.clear();
      return;
    }
  }
  p.remaining--;
  switch (p.header >> 30) {
  case 0:
    if (m_trace && m_trace_budget > 0) {
      m_trace_budget--;
      printf("%s: cp    reg   %04x <- %08x\n", devid_string, p.reg, d);
    }
    reg_write(p.reg, 4, d);
    if (!(p.header & 0x8000))
      p.reg += 4;
    return;
  case 1:
    reg_write(p.remaining == 1 ? p.reg : p.reg1, 4, d);
    return;
  default:
    p.payload.push_back(d);
    if (p.remaining == 0)
      cp_packet3(u8(p.header >> 8), p.payload);
    return;
  }
}

/**
 * A type-3 packet. The 2D operations load the engine's registers as the
 * fields say and start it through the same registers a driver would
 * write, so the two ways of driving the engine draw the same. The 3D
 * operations go to the 3D engine (RadeonR100_3D.cpp).
 *
 * The packet layouts are AMD's "Radeon R5xx Acceleration" v1.5, 6.2.2
 * (the PM4 2D packets the R100 microcode already understood):
 *   SETTINGS   GUI_CONTROL (DP_GUI_MASTER_CNTL), then as its bits say
 *              SRC_PITCH_OFFSET <0>, DST_PITCH_OFFSET <1>,
 *              SRC_SC_BOT_RITE <2>, SC_TOP_LEFT + SC_BOT_RITE <3>, the
 *              brush packet of BRUSH_TYPE <7:4>, BRUSH_Y_X <31>;
 *   PAINT          [TOP|LEFT] [BOTM|RITE] per rectangle (y high);
 *   PAINT_MULTI    [X|Y] [W|H] per rectangle (x high);
 *   BITBLT(_MULTI) [SRC_X|SRC_Y] [DST_X|DST_Y] [W|H] (x high);
 *   TRANS_BITBLT   CLR_CMP_CNTL, the source and destination reference
 *                  colours, then BITBLT's rectangles;
 *   HOSTDATA_BLT   FRGD, BKGD, then per bitmap [Y|X] [H|W] NUMBER<13:0>
 *                  and the data;
 *   POLYLINE       [Y|X] per vertex (y high);
 *   POLYSCANLINES  SCAN_COUNT, then per scan NUM_LINE, [HEIGHT|TOP] and
 *                  NUM_LINE [END|START] pairs;
 *   NEXTCHAR       [Y|X] [H|W] and the bitmap, no settings;
 *   PLY_NEXTSCAN   [HEIGHT|TOP] and [END|START] pairs, no settings;
 *   SET_SCISSORS   [TOP_LEFT] [BOTTOM_RIGHT].
 * The guide does not say whether a rectangle's bottom-right corner or a
 * span's end is inclusive; they are taken as exclusive, as the Windows
 * RECTL these packets were designed for has them [inference].
 **/
void CRadeon::cp_packet3(u8 op, const std::vector<u32> &d) {
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: cp    op %02x n=%zu:", devid_string, op, d.size());
    for (size_t i = 0; i < d.size() && i < 12; i++)
      printf(" %08x", d[i]);
    printf("%s\n", d.size() > 12 ? " ..." : "");
  }
  auto s16 = [](u32 v) { return int(int16_t(v & 0xffff)); };
  // A rectangle filled with the current settings.
  auto paint = [&](int x, int y, int w, int h) {
    if (w <= 0 || h <= 0)
      return;
    eng.dp_cntl |= 3;
    reg_write(DST_Y_X, 4, (u32(y & 0xffff) << 16) | u32(x & 0xffff));
    reg_write(DST_HEIGHT_WIDTH, 4, (u32(h) << 16) | u32(w));
  };
  switch (op) {
  case OP_NOP:
  case OP_WAIT_FOR_IDLE:
  case OP_LOAD_MICROCODE:
    return;
  case OP_SET_SCISSORS:
    if (d.size() >= 2) {
      reg_write(SC_TOP_LEFT, 4, d[0]);
      reg_write(SC_BOTTOM_RIGHT, 4, d[1]);
    }
    return;
  case OP_NEXT_CHAR:
    // A character in the current colours, as HOSTDATA_BLT's bitmaps.
    if (d.size() >= 2) {
      eng.dp_cntl |= 3;
      reg_write(DST_Y_X, 4, d[0]);
      reg_write(DST_HEIGHT_WIDTH, 4, d[1]);
      for (size_t k = 2; k < d.size(); k++)
        reg_write(k + 1 == d.size() ? HOST_DATA_LAST : HOST_DATA0, 4, d[k]);
    }
    return;
  case OP_PLY_NEXTSCAN:
    if (!d.empty()) {
      const int top = s16(d[0]), h = int(d[0] >> 16);
      for (size_t k = 1; k < d.size(); k++)
        paint(s16(d[k]), top, s16(d[k] >> 16) - s16(d[k]), h);
    }
    return;
  case OP_CNTL_PAINT:
  case OP_CNTL_PAINT_MULTI:
  case OP_CNTL_BITBLT:
  case OP_CNTL_BITBLT_MULTI:
  case OP_CNTL_TRANS_BITBLT:
  case OP_CNTL_HOSTDATA_BLT:
  case OP_CNTL_POLYLINE:
  case OP_CNTL_POLYSCANLINES:
    break;
  default:
    if (r3d_packet3(op, d))
      return;
    if (!m_cp_unknown_seen[op]) {
      m_cp_unknown_seen[op] = true;
      printf("%s: CP operation %02x (%zu dwords) not modelled\n", devid_string,
             op, d.size());
    }
    return;
  }
  if (d.empty())
    return;

  // The fields DP_GUI_MASTER_CNTL announces.
  size_t i = 0;
  const u32 gmc = d[i++];
  auto field = [&](u32 reg) {
    if (i < d.size())
      reg_write(reg, 4, d[i++]);
  };
  reg_write(DP_GUI_MASTER_CNTL, 4, gmc);
  if (gmc & 1)
    field(SRC_PITCH_OFFSET);
  if (gmc & 2)
    field(DST_PITCH_OFFSET);
  if (gmc & 4)
    field(SRC_SC_BOTTOM_RIGHT);
  if (gmc & 8) {
    field(SC_TOP_LEFT);
    field(SC_BOTTOM_RIGHT);
  }
  const u32 btype = (gmc >> 4) & 0xf;
  if (btype == 10) {
    // The 8x8 colour brush: 64 pixels of the destination type, 16 dwords
    // per byte of pixel.
    const int n = 16 * datatype_bytes(gmc >> 8);
    for (int b = 0; b < n && b < 64; b++)
      field(BRUSH_DATA0 + 4 * u32(b));
  } else {
    u32 brush_regs[40];
    const int nb = brush_fields(btype, brush_regs);
    for (int b = 0; b < nb; b++)
      field(brush_regs[b]);
  }
  if (gmc & 0x80000000u)
    field(BRUSH_Y_X);

  // source x/y, destination x/y, width/height (x high, y low), copied in
  // whichever direction keeps an overlapping copy intact
  auto blits = [&]() {
    while (i + 3 <= d.size()) {
      const int sx = int(d[i] >> 16) & 0x3fff, sy = int(d[i]) & 0x3fff;
      const int dx = int(d[i + 1] >> 16) & 0x3fff, dy = int(d[i + 1]) & 0x3fff;
      const int w = int(d[i + 2] >> 16) & 0x3fff, h = int(d[i + 2]) & 0x3fff;
      const bool ttb = !(dy > sy), ltr = !(dy == sy && dx > sx);
      eng.dp_cntl = (eng.dp_cntl & ~3u) | (ltr ? 1 : 0) | (ttb ? 2 : 0);
      const int ox = ltr ? 0 : w - 1, oy = ttb ? 0 : h - 1;
      reg_write(SRC_X_Y, 4, (u32(sx + ox) << 16) | u32(sy + oy));
      reg_write(DST_X_Y, 4, (u32(dx + ox) << 16) | u32(dy + oy));
      reg_write(DST_WIDTH_HEIGHT, 4, d[i + 2]);
      eng.dp_cntl |= 3;
      i += 3;
    }
  };

  switch (op) {
  case OP_CNTL_PAINT:
    // rectangles by their corners: [top|left] [bottom|right]
    while (i + 2 <= d.size()) {
      const int l = s16(d[i]), t = s16(d[i] >> 16);
      const int r = s16(d[i + 1]), b = s16(d[i + 1] >> 16);
      paint(l, t, r - l, b - t);
      i += 2;
    }
    return;

  case OP_CNTL_PAINT_MULTI:
    // rectangles: x <31:16> y <15:0>, width <31:16> height <15:0>
    while (i + 2 <= d.size()) {
      eng.dp_cntl |= 3;
      reg_write(DST_X_Y, 4, d[i]);
      reg_write(DST_WIDTH_HEIGHT, 4, d[i + 1]);
      i += 2;
    }
    return;

  case OP_CNTL_BITBLT:
  case OP_CNTL_BITBLT_MULTI:
    blits();
    return;

  case OP_CNTL_TRANS_BITBLT:
    field(CLR_CMP_CNTL);
    field(CLR_CMP_CLR_SRC);
    field(CLR_CMP_CLR_DST);
    blits();
    return;

  case OP_CNTL_HOSTDATA_BLT:
    // The source colours, then one or more rectangles, each y <31:16>
    // x <15:0>, height/width, the number of data dwords <13:0> and the
    // data: DECwindows sends a run of glyphs in one packet this way.
    field(DP_SRC_FRGD_CLR);
    field(DP_SRC_BKGD_CLR);
    eng.dp_cntl |= 3;
    while (i + 3 <= d.size()) {
      reg_write(DST_Y_X, 4, d[i]);
      reg_write(DST_HEIGHT_WIDTH, 4, d[i + 1]);
      size_t n = d[i + 2] & 0x3fff;
      i += 3;
      if (n > d.size() - i)
        n = d.size() - i;
      for (size_t k = 0; k < n; k++)
        reg_write(k + 1 == n ? HOST_DATA_LAST : HOST_DATA0, 4, d[i + k]);
      i += n;
    }
    return;

  case OP_CNTL_POLYLINE:
    // the brush colour drawn through the vertices, [y|x] each: the layout
    // of DST_LINE_START/END
    if (i < d.size())
      reg_write(DST_LINE_START, 4, d[i++]);
    while (i < d.size())
      reg_write(DST_LINE_END, 4, d[i++]);
    return;

  case OP_CNTL_POLYSCANLINES:
    if (i < d.size()) {
      u32 scans = d[i++];
      while (scans-- && i + 2 <= d.size()) {
        u32 n = d[i] & 0x3fff;
        const int top = s16(d[i + 1]), h = int(d[i + 1] >> 16);
        i += 2;
        for (; n && i < d.size(); n--, i++)
          paint(s16(d[i]), top, s16(d[i] >> 16) - s16(d[i]), h);
      }
    }
    return;
  }
}
