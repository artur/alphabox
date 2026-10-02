/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
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
 */

#include "AlphaCPU.hpp"
#include "Flash.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "Tsunami.hpp"

/**
 * Read a byte from the TIGbus
 *
 * What information we have is sketchy at best. Following is extracted from T64,
 * Although we're not 100% sure that this is actually for ES40:
 *
 * \code
 * +-----------------+--------+
 * | register        | offset |
 * +-----------------+--------+
 * | trr             | 000    |
 * | smir            | 040    | system management IR
 * | cpuir           | 080    | CPU IR
 * | psir            | 0c0    | powe supply IR
 * | mod_info        | 100    |
 * | clk_info        | 140    |
 * | chip_info       | 180    |
 * | tpcr            | 200    |
 * | pll_data        | 280    |
 * | pll_clk         | 2c0    |
 * | ev6_init        | 300    |
 * | csleep          | 340    |
 * | smcr            | 380    |
 * | ttcr            | 3c0    |
 * | clr_irq5        | 400    |
 * | clr_irq4        | 440    |
 * | clr_pwr_flt_det | 480    |
 * | clr_temp_warn   | 4c0    |
 * | clr_temp_fail   | 500    |
 * | ev6_halt        | 5c0    |
 * | srcr0           | 600    |
 * | srcr1           | 640    |
 * | frar0           | 700    |
 * | frar1           | 740    |
 * | fwmr0           | 800    |
 * | fwmr1           | 840    |
 * | fwmr2           | 880    |
 * | fwmr3           | 8c0    |
 * | ipcr0           | a00    | inter-processor communications register for
 *arbiter (?) | ipcr1           | a40    | | ipcr2           | a80    | | ipcr3
 *| ac0    | | ipcr4           | b00    |
 * +-----------------+--------+
 * \endcode
 **/
u8 CTsunami::tig_read(u32 a) {
  switch (a) {
  case 0x30000000: // trr
    return 0;
  case 0x30000040: // smir
    return state.tig.FwWrite;
  case 0x30000100: // mod_info
    return state.tig.ModInfo;
  case 0x300003c0: // ttcr
    return state.tig.HaltA;
  case 0x30000440: // clr_irq4: latch clear; our IRQ4 is level-driven
  case 0x30000480: // clr_pwr_flt_det
    return 0;
  case 0x300005c0: // ev6_halt
    return state.tig.HaltB;
  case 0x30000a00: // ipcr0-4: PALcode MP restart handshake
  case 0x30000a40:
  case 0x30000a80:
  case 0x30000ac0:
  case 0x30000b00:
    return state.tig.ipcr[(a - 0x30000a00) >> 6];
  case 0x38000180: // Arbiter revision
    return 0xfe;
  default:
    printf("Unknown TIG %08x read attempted.\n", a);
    m_sys->trace_unknown("TIG register", a, 8, false, 0, nullptr);
    return 0;
  }
}

/**
 * Drive each CPU's IRQ4 (halt / MP work request) line from the TIG halt
 * registers: bit n of (ttcr | ev6_halt) is CPU n's line, level-triggered. The
 * PALcode's MP work request sets the target's bit, and the target's halt
 * interrupt handler clears it again, which drops the line. Serialized with
 * interrupt(): the writing CPU changes other CPUs' interrupt state.
 **/
void CTsunami::tig_update_halt_lines() {
  std::lock_guard<std::mutex> g(m_lock);
  const u8 lines = state.tig.HaltA | state.tig.HaltB;
  for (int i = 0; i < m_sys->get_cpu_num(); i++)
    m_sys->get_cpu(i)->irq_h(4, (lines >> i) & 1, 0);
}

void CTsunami::tig_write(u32 a, u8 data) {
  // The registers a console uses to wake and hand work to other processors.
  if (a == 0x300003c0 || a == 0x300005c0 ||
      (a >= 0x30000a00 && a <= 0x30000b00))
    m_sys->trace_mp("TIG write", a, data);
  switch (a) {
  case 0x30000000: // trr
    return;
  case 0x30000040: // smir
    state.tig.FwWrite = data;
    return;
  case 0x30000100: // mod_info
    state.tig.ModInfo = data;
    return;
  case 0x300003c0: // ttcr
    state.tig.HaltA = data;
    tig_update_halt_lines();
    return;
  case 0x30000440: // clr_irq4: latch clear; our IRQ4 is level-driven
  case 0x30000480: // clr_pwr_flt_det
    return;
  case 0x300005c0: // ev6_halt
    state.tig.HaltB = data;
    tig_update_halt_lines();
    return;
  case 0x30000a00: // ipcr0-4: PALcode MP restart handshake
  case 0x30000a40:
  case 0x30000a80:
  case 0x30000ac0:
  case 0x30000b00:
    state.tig.ipcr[(a - 0x30000a00) >> 6] = data;
    return;
  case 0x30000600: // srcr0
  case 0x30000640: // srcr1
    // Empirical: LFU writes 0x30 here when exiting after an update.
    if (data & 0x30) {
      printf("%%SYS-I-RESETREQ: TIG SRCR write %07x=%02x\n", a, data);
      if (theSROM)
        theSROM->FlushIfDirty();
      m_sys->RequestSystemReset();
    }
    return;
  default:
    printf("Unknown TIG %07x write with %02x attempted.\n", a, data);
    m_sys->trace_unknown("TIG register", a, 8, true, data, nullptr);
  }
}
