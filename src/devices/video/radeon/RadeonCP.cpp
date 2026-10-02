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
 *           operations (CNTL_PAINT_MULTI, CNTL_BITBLT_MULTI,
 *           CNTL_HOSTDATA_BLT, CNTL_POLYLINE), NOP and WAIT_FOR_IDLE.
 *           3D operations are not modelled.
 * A 2D operation starts with DP_GUI_MASTER_CNTL; its bits say which
 * fields follow (source and destination pitch/offset, the clip
 * rectangles, the brush and source colours), then the operation's own.
 *
 * The CP works synchronously: the packets up to the new write pointer are
 * executed inside the register write that publishes them, and the read
 * pointer is written back before the write returns. The microcode a
 * driver loads (CP_ME_RAM) is kept and read back, not executed.
 *
 * Memory the CP reads is addressed in the memory controller's space:
 * MC_FB_LOCATION is the framebuffer, MC_AGP_LOCATION a window onto the
 * bus starting at AGP_BASE (on a PCI card, and on this AGP card without
 * a GART, the way a driver reaches host memory).
 **/

#include "Radeon.hpp"
#include "System.hpp"

using namespace radeon;

namespace {
constexpr u32 AGP_BASE = 0x0170;
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
  OP_CNTL_TRANS_BITBLT = 0x9c
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
    add(DP_BRUSH_FRGD_CLR);
    break;
  default: // none, and the colour brushes (not carried in a packet here)
    break;
  }
  return n;
}
} // namespace

/**
 * A memory-controller address as the CP reads it: the framebuffer, or the
 * bus through the AGP window. Returns false when it is neither.
 **/
bool CRadeon::cp_translate(u32 mc, bool *is_vram, u32 *addr) const {
  const u32 fb = R(MC_FB_LOCATION), agp = R(MC_AGP_LOCATION);
  const u32 fb_start = (fb & 0xffff) << 16,
            fb_end = (fb & 0xffff0000u) | 0xffff;
  const u32 agp_start = (agp & 0xffff) << 16,
            agp_end = (agp & 0xffff0000u) | 0xffff;
  if (mc >= fb_start && mc <= fb_end) {
    *is_vram = true;
    *addr = (mc - fb_start) & vram_mask();
    return true;
  }
  if (agp_end > agp_start && mc >= agp_start && mc <= agp_end) {
    *is_vram = false;
    *addr = R(AGP_BASE) + (mc - agp_start);
    return true;
  }
  return false;
}

u32 CRadeon::cp_read32(u32 mc) {
  bool vram;
  u32 a;
  if (!cp_translate(mc, &vram, &a)) {
    if (m_cp_bad_reads++ < 8)
      printf("%s: CP read outside the framebuffer and AGP window: %08x\n",
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

/**
 * The CP's registers. Returns true when the register is the CP's and
 * the write was handled.
 **/
bool CRadeon::cp_reg_write(u32 reg, u32 data) {
  if (reg >= CP_CSQ_APER_PRIMARY && reg < CP_CSQ_APER_END) {
    cp_feed(data);
    return true;
  }
  switch (reg) {
  case CP_RB_WPTR:
    R(reg) = data;
    cp_run_ring();
    return true;
  case CP_RB_RPTR_WR:
    R(reg) = data;
    if (R(CP_RB_CNTL) & RB_RPTR_WR_ENA)
      R(CP_RB_RPTR) = data;
    return true;
  case CP_RB_RPTR:
    return true; // the chip's
  case CP_IB_BUFSZ:
    R(reg) = data;
    cp_run_buffer(R(CP_IB_BASE), data & 0x7fffff);
    return true;
  case CP_ME_RAM_ADDR:
    m_me_index = data & 0xff;
    R(reg) = data;
    return true;
  case CP_ME_RAM_RADDR:
    m_me_index = data & 0xff;
    R(reg) = data;
    return true;
  case CP_ME_RAM_DATAH:
    m_me_ram[m_me_index][0] = data;
    return true;
  case CP_ME_RAM_DATAL:
    m_me_ram[m_me_index][1] = data;
    m_me_index = (m_me_index + 1) & 0xff;
    return true;
  }
  if (reg >= SCRATCH_REG0 && reg <= SCRATCH_REG5) {
    // A scratch register is a fence: written back to memory when its
    // SCRATCH_UMSK bit is set.
    R(reg) = data;
    const u32 n = (reg - SCRATCH_REG0) / 4;
    if (R(SCRATCH_UMSK) & (1u << n))
      cp_write32(R(SCRATCH_ADDR) + 4 * n, data);
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
    m_me_index = (m_me_index + 1) & 0xff;
    return true;
  case 0x07f8: // CP_CSQ_STAT: the queues are always drained
  case 0x07fc: // CP_CSQ2_STAT
  case 0x07c0: // CP_STAT: idle
    *v = 0;
    return true;
  }
  return false;
}

/**
 * Execute the ring from the read pointer to the write pointer, then write
 * the read pointer back where the driver asked for it.
 **/
void CRadeon::cp_run_ring() {
  if (m_cp_depth > 0)
    return; // the ring publishing itself from inside a packet
  const u32 cntl = R(CP_RB_CNTL);
  const u32 dwords = 2u << (cntl & 0x3f);
  const u32 mask = dwords - 1;
  const u32 wptr = R(CP_RB_WPTR) & mask;
  u32 rptr = R(CP_RB_RPTR) & mask;
  m_cp_depth++;
  int guard = 0;
  while (rptr != wptr && guard++ < (1 << 22)) {
    cp_feed(cp_read32(R(CP_RB_BASE) + rptr * 4));
    rptr = (rptr + 1) & mask;
  }
  m_cp_depth--;
  R(CP_RB_RPTR) = rptr;
  if (!(cntl & RB_NO_UPDATE))
    cp_write32(R(CP_RB_RPTR_ADDR) & ~3u, rptr);
}

/// An indirect buffer: `dwords` dwords from `mc`.
void CRadeon::cp_run_buffer(u32 mc, u32 dwords) {
  if (m_cp_ib_depth > 2)
    return;
  m_cp_ib_depth++;
  // The ring's packet in progress (the one that wrote CP_IB_BUFSZ) is
  // complete; the buffer's packets have a parser of their own.
  const cp_parser saved = m_cp;
  m_cp = cp_parser{};
  for (u32 i = 0; i < dwords; i++)
    cp_feed(cp_read32(mc + i * 4));
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
 * write, so the two ways of driving the engine draw the same.
 **/
void CRadeon::cp_packet3(u8 op, const std::vector<u32> &d) {
  if (m_trace && m_trace_budget > 0) {
    m_trace_budget--;
    printf("%s: cp    op %02x n=%zu:", devid_string, op, d.size());
    for (size_t i = 0; i < d.size() && i < 12; i++)
      printf(" %08x", d[i]);
    printf("%s\n", d.size() > 12 ? " ..." : "");
  }
  switch (op) {
  case OP_NOP:
  case OP_WAIT_FOR_IDLE:
  case OP_LOAD_MICROCODE:
    return;
  case OP_CNTL_PAINT:
  case OP_CNTL_PAINT_MULTI:
  case OP_CNTL_BITBLT:
  case OP_CNTL_BITBLT_MULTI:
  case OP_CNTL_HOSTDATA_BLT:
  case OP_CNTL_POLYLINE:
    break;
  default:
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
  u32 brush_regs[40];
  const int nb = brush_fields((gmc >> 4) & 0xf, brush_regs);
  for (int b = 0; b < nb; b++)
    field(brush_regs[b]);
  const u32 src_type = (gmc >> 12) & 3;
  const u32 source = (gmc >> 24) & 7;

  switch (op) {
  case OP_CNTL_PAINT:
  case OP_CNTL_PAINT_MULTI:
    // rectangles: x <31:16> y <15:0>, width <31:16> height <15:0>
    while (i + 2 <= d.size()) {
      reg_write(DST_X_Y, 4, d[i]);
      reg_write(DST_WIDTH_HEIGHT, 4, d[i + 1]);
      i += 2;
    }
    return;

  case OP_CNTL_BITBLT:
  case OP_CNTL_BITBLT_MULTI:
    if (src_type != 3) { // mono source: its colours
      field(DP_SRC_FRGD_CLR);
      field(DP_SRC_BKGD_CLR);
    }
    // source x/y, destination x/y, width/height (x high, y low), copied
    // in whichever direction keeps an overlapping copy intact
    while (i + 3 <= d.size()) {
      const int sx = int(d[i] >> 16) & 0x3fff, sy = int(d[i]) & 0x3fff;
      const int dx = int(d[i + 1] >> 16) & 0x3fff, dy = int(d[i + 1]) & 0x3fff;
      const int w = int(d[i + 2] >> 16) & 0x3fff, h = int(d[i + 2]) & 0x3fff;
      const bool ttb = !(dy > sy), ltr = !(dy == sy && dx > sx);
      eng.dp_cntl = (ltr ? 1 : 0) | (ttb ? 2 : 0);
      const int ox = ltr ? 0 : w - 1, oy = ttb ? 0 : h - 1;
      reg_write(SRC_X_Y, 4, (u32(sx + ox) << 16) | u32(sy + oy));
      reg_write(DST_X_Y, 4, (u32(dx + ox) << 16) | u32(dy + oy));
      reg_write(DST_WIDTH_HEIGHT, 4, d[i + 2]);
      eng.dp_cntl = 3;
      i += 3;
    }
    return;

  case OP_CNTL_HOSTDATA_BLT: {
    // The source colours, then one or more rectangles, each y <31:16>
    // x <15:0>, height/width, the number of data dwords and the data:
    // DECwindows sends a run of glyphs in one packet this way.
    field(DP_SRC_FRGD_CLR);
    field(DP_SRC_BKGD_CLR);
    eng.dp_cntl = 3;
    while (i + 3 <= d.size()) {
      reg_write(DST_Y_X, 4, d[i]);
      reg_write(DST_HEIGHT_WIDTH, 4, d[i + 1]);
      size_t n = d[i + 2];
      i += 3;
      if (n > d.size() - i)
        n = d.size() - i;
      for (size_t k = 0; k < n; k++)
        reg_write(k + 1 == n ? HOST_DATA_LAST : HOST_DATA0, 4, d[i + k]);
      i += n;
    }
    (void)source;
    return;
  }

  case OP_CNTL_POLYLINE:
    // the brush colour drawn through the vertices (x high, y low)
    if (i < d.size()) {
      const u32 v = d[i++];
      reg_write(DST_LINE_START, 4, (v << 16) | (v >> 16));
    }
    while (i < d.size()) {
      const u32 v = d[i++];
      reg_write(DST_LINE_END, 4, (v << 16) | (v >> 16));
    }
    return;
  }
}
