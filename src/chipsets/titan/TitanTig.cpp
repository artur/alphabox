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
 * The Titan's TIG bus registers (801.3000.0000 + offset << 6), what is not
 * the flash or the DPR (board parts, platforms/<board>/, which register
 * their own ranges of the TIG bus).
 *
 * The ES45's TIG (TIG Rev 2.6 in its show config) is a different part from
 * the ES40's, programmed differently; no document names its registers. The
 * offsets the ES45 console was seen to use are those of the ES40's TIG
 * (Tru64 dc104x.h, as in chipsets/tsunami/TsunamiTig.cpp), so they are given
 * the same meaning here; every other register is traced, and keeps what is
 * written to it.
 **/
#include "AlphaCPU.hpp"
#include "Flash.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "Titan.hpp"

/// The halt / MP-work line of each processor (b_irq<4>), from the two halt
/// registers, as on the ES40.
void CTitan::update_halt_lines() {
  std::lock_guard<std::mutex> g(m_lock);
  const u8 lines = state.tig.ttcr | state.tig.ev6_halt;
  for (int i = 0; i < m_sys->get_cpu_num(); i++)
    m_sys->get_cpu(i)->irq_h(4, (lines >> i) & 1, 0);
}

u8 CTitan::tig_read(u32 a) {
  switch (a) {
  case 0x30000000: // trr
    return TIG_REV;
  case 0x30000040: // smir
    return state.tig.smir;
  case 0x30000100: // mod_info
    return state.tig.mod_info;
  case 0x300003c0: // ttcr
    return state.tig.ttcr;
  case 0x30000440: // clr_irq4, clr_pwr_flt_det: read as 0, as on the ES40
  case 0x30000480:
    return 0;
  case 0x300005c0: // ev6_halt
    return state.tig.ev6_halt;
  case 0x30000a00: // ipcr0-4: the PALcode's MP restart handshake
  case 0x30000a40:
  case 0x30000a80:
  case 0x30000ac0:
  case 0x30000b00:
    return state.tig.ipcr[(a - 0x30000a00) >> 6];
  case 0x30000600:
  case 0x30000640:
    return state.tig.srcr[(a >> 6) & 1];
  default:
    m_sys->trace_unknown("TIG register", U64(0x80100000000) | a, 8, false, 0,
                         nullptr);
    if (a >= 0x30000000 && a < 0x30001000)
      return state.tig.other[(a >> 6) & 63];
    return 0;
  }
}

void CTitan::tig_write(u32 a, u8 data) {
  if (a == 0x300003c0 || a == 0x300005c0 ||
      (a >= 0x30000a00 && a <= 0x30000b00))
    m_sys->trace_mp("TIG write", a, data);
  switch (a) {
  case 0x30000000: // trr
  case 0x30000440: // clr_irq4 (the PALcode writes it): the halt lines are
  case 0x30000480: // level-driven here; clr_pwr_flt_det: no power faults
    return;
  case 0x30000040:
    state.tig.smir = data;
    return;
  case 0x30000100:
    state.tig.mod_info = data;
    return;
  case 0x300003c0:
    state.tig.ttcr = data;
    update_halt_lines();
    return;
  case 0x300005c0:
    state.tig.ev6_halt = data;
    update_halt_lines();
    return;
  case 0x30000a00:
  case 0x30000a40:
  case 0x30000a80:
  case 0x30000ac0:
  case 0x30000b00:
    state.tig.ipcr[(a - 0x30000a00) >> 6] = data;
    return;
  case 0x30000600: // srcr0-1: the update utility asks for a reset here
  case 0x30000640:
    state.tig.srcr[(a >> 6) & 1] = data;
    if (data & 0x30) {
      printf("%%SYS-I-RESETREQ: TIG SRCR write %07x=%02x\n", a, data);
      if (theSROM)
        theSROM->FlushIfDirty();
      m_sys->RequestSystemReset();
    }
    return;
  default:
    m_sys->trace_unknown("TIG register", U64(0x80100000000) | a, 8, true, data,
                         nullptr);
    if (a >= 0x30000000 && a < 0x30001000)
      state.tig.other[(a >> 6) & 63] = data;
  }
}
