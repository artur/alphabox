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
 * DECchip 21030 (TGA): the Brooktree Bt485 RAMDAC of the ZLXp-E1, reached
 * through the palette and DAC setup and data registers (4.8.2, 4.8.3).
 *
 * The 21030 drives the Bt485's MPU port from a 5-bit control field: bit 0
 * the strobe (0 the write strobe WR*, 1 the read strobe RD* on a Bt485 --
 * Table 8-1; Table 4-69 has the two the other way round) and bits 4:1 the
 * register select RS3..RS0. For a read the field is EPSR's and the byte
 * comes back in EPDR <23:16>. For a write the field travels in EPDR <12:8>
 * with the data in <7:0>: NetBSD's tga(4) writes the Bt485 that way and
 * notes it is "in spite of the 21030 documentation", whose EPDR write
 * format has <31:8> reserved; Linux's tgafb puts the same field in both
 * registers. EPDR's is the one taken.
 *
 * The Bt485: a 256-entry palette with 6- or 8-bit components (command
 * register 0 bit 1), the overscan and three cursor colours, and a 64x64
 * (or 32x32) two-plane cursor whose RAM is addressed by the palette write
 * address with command register 3's low two bits above it. Command
 * register 3 itself is reached through the status register's address
 * while command register 0 bit 7 is set and the write address is 1.
 **/

#include "StdAfx.hpp"
#include "Tga.hpp"

using namespace tga;

void CTga::ramdac_reset() {
  memset(&dac, 0, sizeof(dac));
  dac.pixmask = 0xff;
  m_generation++;
}

void CTga::epdr_write(u32 data) {
  r[EPDR] = data;
  const u32 ctl = (data >> 8) & 0x1f;
  if (ctl & 1) {
    // The read strobe: no write reaches the RAMDAC.
    trace("EPDR write %08x with the read strobe, ignored", data);
    return;
  }
  ramdac_write((ctl >> 1) & 0xf, u8(data));
}

u32 CTga::epdr_read() {
  const u32 ctl = r[EPSR] & 0x1f;
  const u8 v = ramdac_read((ctl >> 1) & 0xf);
  return u32(v) << 16;
}

/// The cursor RAM address: CR3<1:0> above the 8-bit address register.
static inline unsigned cursor_addr(const u8 *cmd, u8 lo) {
  return ((unsigned(cmd[3]) & 3) << 8) | lo;
}

static inline void cursor_addr_inc(u8 *cmd, u8 &lo) {
  if (++lo == 0)
    cmd[3] = u8((cmd[3] & ~3u) | ((cmd[3] + 1) & 3));
}

void CTga::ramdac_write(unsigned rs, u8 v) {
  trace("Bt485 write RS%x = %02x", rs, v);
  switch (rs) {
  case BT485_PAL_WRADDR:
    dac.wr_addr = v;
    dac.wr_sub = 0;
    break;
  case BT485_PAL_DATA:
    dac.latch[dac.wr_sub++] = v;
    if (dac.wr_sub == 3) {
      memcpy(dac.palette[dac.wr_addr], dac.latch, 3);
      dac.wr_addr++;
      dac.wr_sub = 0;
    }
    break;
  case BT485_PIXMASK:
    dac.pixmask = v;
    break;
  case BT485_PAL_RDADDR:
    dac.rd_addr = v;
    dac.rd_sub = 0;
    break;
  case BT485_COC_WRADDR:
    dac.coc_wr = v & 3;
    dac.coc_wsub = 0;
    break;
  case BT485_COC_DATA:
    dac.latch[dac.coc_wsub++] = v;
    if (dac.coc_wsub == 3) {
      memcpy(dac.coc[dac.coc_wr], dac.latch, 3);
      dac.coc_wr = (dac.coc_wr + 1) & 3;
      dac.coc_wsub = 0;
    }
    break;
  case BT485_CMD0:
    dac.cmd[0] = v;
    break;
  case BT485_COC_RDADDR:
    dac.coc_rd = v & 3;
    dac.coc_rsub = 0;
    break;
  case BT485_CMD1:
    dac.cmd[1] = v;
    break;
  case BT485_CMD2:
    dac.cmd[2] = v;
    break;
  case BT485_STATUS:
    if ((dac.cmd[0] & 0x80) && dac.wr_addr == 0x01)
      dac.cmd[3] = v;
    break;
  case BT485_CURSOR_RAM:
    dac.cursor[cursor_addr(dac.cmd, dac.wr_addr)] = v;
    cursor_addr_inc(dac.cmd, dac.wr_addr);
    break;
  case BT485_CURSOR_XLO:
    dac.cur_x = u16((dac.cur_x & 0x0f00) | v);
    break;
  case BT485_CURSOR_XHI:
    dac.cur_x = u16((dac.cur_x & 0x00ff) | ((v & 0x0f) << 8));
    break;
  case BT485_CURSOR_YLO:
    dac.cur_y = u16((dac.cur_y & 0x0f00) | v);
    break;
  case BT485_CURSOR_YHI:
    dac.cur_y = u16((dac.cur_y & 0x00ff) | ((v & 0x0f) << 8));
    break;
  }
  m_generation++;
}

u8 CTga::ramdac_read(unsigned rs) {
  u8 v = 0;
  switch (rs) {
  case BT485_PAL_WRADDR:
    v = dac.wr_addr;
    break;
  case BT485_PAL_DATA:
    v = dac.palette[dac.rd_addr][dac.rd_sub++];
    if (dac.rd_sub == 3) {
      dac.rd_addr++;
      dac.rd_sub = 0;
    }
    break;
  case BT485_PIXMASK:
    v = dac.pixmask;
    break;
  case BT485_PAL_RDADDR:
    v = dac.rd_addr;
    break;
  case BT485_COC_WRADDR:
    v = dac.coc_wr;
    break;
  case BT485_COC_DATA:
    v = dac.coc[dac.coc_rd][dac.coc_rsub++];
    if (dac.coc_rsub == 3) {
      dac.coc_rd = (dac.coc_rd + 1) & 3;
      dac.coc_rsub = 0;
    }
    break;
  case BT485_CMD0:
    v = dac.cmd[0];
    break;
  case BT485_COC_RDADDR:
    v = dac.coc_rd;
    break;
  case BT485_CMD1:
    v = dac.cmd[1];
    break;
  case BT485_CMD2:
    v = dac.cmd[2];
    break;
  case BT485_STATUS:
    // Command register 3 through its indirect address; otherwise the
    // status register, whose identification bits read 0x80 on a Bt485 (the
    // value XFree86's Brooktree probe tells the Bt485 by).
    if ((dac.cmd[0] & 0x80) && dac.wr_addr == 0x01)
      v = dac.cmd[3];
    else
      v = 0x80;
    break;
  case BT485_CURSOR_RAM:
    v = dac.cursor[cursor_addr(dac.cmd, dac.rd_addr)];
    cursor_addr_inc(dac.cmd, dac.rd_addr);
    break;
  case BT485_CURSOR_XLO:
    v = u8(dac.cur_x);
    break;
  case BT485_CURSOR_XHI:
    v = u8(dac.cur_x >> 8);
    break;
  case BT485_CURSOR_YLO:
    v = u8(dac.cur_y);
    break;
  case BT485_CURSOR_YHI:
    v = u8(dac.cur_y >> 8);
    break;
  }
  trace("Bt485 read  RS%x = %02x", rs, v);
  return v;
}

/// A palette entry as 0xffRRGGBB: through the pixel read mask, the
/// components 8-bit or 6-bit (command register 0 bit 1).
u32 CTga::ramdac_color(u8 index) const {
  const u8 *c = dac.palette[index & dac.pixmask];
  u32 rr = c[0], gg = c[1], bb = c[2];
  if (!(dac.cmd[0] & 0x02)) {
    rr = ((rr & 0x3f) << 2) | ((rr & 0x3f) >> 4);
    gg = ((gg & 0x3f) << 2) | ((gg & 0x3f) >> 4);
    bb = ((bb & 0x3f) << 2) | ((bb & 0x3f) >> 4);
  }
  return 0xff000000u | (rr << 16) | (gg << 8) | bb;
}
