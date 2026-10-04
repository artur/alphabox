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
 * The Titan Cchip's and Dchips' CSRs (Titan.hpp). Layout from Linux
 * core_titan.h (titan_cchip), at 801.A000.0000 + offset:
 *
 * \code
 * 000 CSC     040 MTR     080 MISC    0C0 MPD
 * 100-1C0 AAR0-3
 * 200 DIM0    240 DIM1    280 DIR0    2C0 DIR1    300 DRIR    340 PRBEN
 * 380 IIC0    3C0 IIC1    400-4C0 MPR0-3          580 TTR     5C0 TDR
 * 600 DIM2    640 DIM3    680 DIR2    6C0 DIR3    700 IIC2    740 IIC3
 * 780 PWR     C00 CMONCTLA C40 CMONCTLB C80 CMONCNT01 CC0 CMONCNT23 D00 CPEN
 * \endcode
 *
 * MISC, the DIMs and DIRs and the MPD pins behave as the Typhoon's
 * (chipsets/tsunami/TsunamiCchip.cpp), which their identical layout in
 * Linux supports; the console is the check.
 **/
#include "AlphaCPU.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "Titan.hpp"

static int dim_index(u32 a) { return ((a >> 9) & 2) | ((a >> 6) & 1); }

u64 CTitan::cchip_read(u32 a, CSystemComponent *source) {
  switch (a) {
  case 0x000:
    return state.cchip.csc;
  case 0x040:
    return state.cchip.mtr;
  case 0x080: {
    std::lock_guard<std::mutex> g(m_lock);
    const int id = source ? ((CAlphaCPU *)source)->get_cpuid() : 0;
    return state.cchip.misc | (u64)(id & 3);
  }
  case 0x0c0: { // MPD: <3> DR (SDA in), <2> CKR (SCL in)
    u64 v = 0;
    v |= (m_mpd_bus.scl() ? 1 : 0) << 2;
    v |= (m_mpd_bus.sda() ? 1 : 0) << 3;
    return v;
  }
  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0: {
    // AAR0-3: the Typhoon's encoding, as the consoles read it: the base
    // in <34:24>, the size 2^(<15:12> + 23) bytes, 0 when not populated.
    const auto &arr = m_array[(a >> 6) & 3];
    if (!arr.size)
      return 0;
    unsigned log2 = 0;
    while ((U64(1) << log2) < arr.size)
      log2++;
    return (arr.base & U64(0x7ff000000)) | ((u64)(log2 - 23) << 12);
  }
  case 0x200:
  case 0x240:
  case 0x600:
  case 0x640:
    return state.cchip.dim[dim_index(a)];
  case 0x280:
  case 0x2c0:
  case 0x680:
  case 0x6c0:
    return state.cchip.drir & state.cchip.dim[dim_index(a)];
  case 0x300:
    return state.cchip.drir;
  case 0x340:
    return state.cchip.prben;
  case 0x380:
  case 0x3c0:
    return state.cchip.iic[(a >> 6) & 1];
  case 0x700:
  case 0x740:
    return state.cchip.iic[2 + ((a >> 6) & 1)];
  case 0x400:
  case 0x440:
  case 0x480:
  case 0x4c0:
    return state.cchip.mpr[(a >> 6) & 3];
  case 0x580:
    return state.cchip.ttr;
  case 0x5c0:
    return state.cchip.tdr;
  case 0x780:
    return state.cchip.pwr;
  case 0xc00:
    return state.cchip.cmonctla;
  case 0xc40:
    return state.cchip.cmonctlb;
  case 0xc80:
  case 0xcc0:
    return 0; // performance counters: not counted
  case 0xd00:
    return state.cchip.cpen;
  default:
    m_sys->trace_unknown("Cchip CSR", U64(0x801a0000000) | a, 64, false, 0,
                         source);
    return 0;
  }
}

void CTitan::cchip_write(u32 a, u64 data, CSystemComponent *source) {
  CAlphaCPU *cpu = (CAlphaCPU *)source;
  switch (a) {
  case 0x000:
    state.cchip.csc = data;
    return;
  case 0x040:
    state.cchip.mtr = data;
    return;
  case 0x080: { // MISC, as the Typhoon's
    if (data & U64(0x0000000000000ff0))
      m_sys->trace_mp("Cchip MISC write", 0x080, data);
    std::lock_guard<std::mutex> g(m_lock);
    state.cchip.misc |= (data & U64(0x00000f0000f00000));  // W1S
    state.cchip.misc &= ~(data & U64(0x0000000010000ff0)); // W1C
    if (data & U64(0x0000000001000000)) {                  // arbitration clear
      state.cchip.misc &= ~U64(0x0000000000ff0000);
      if (cpu)
        m_sys->arbitration_cleared(cpu->get_cpuid());
    }
    if ((data & U64(0x00000000000f0000)) &&
        !(state.cchip.misc & U64(0x00000000000f0000)))
      state.cchip.misc |= data & U64(0x00000000000f0000); // arbitration won
    for (int i = 0; i < m_sys->get_cpu_num(); i++) {
      if (data & (U64(0x10) << i)) // ITINTR cleared
        m_sys->get_cpu(i)->irq_h(2, false, 0);
      if (data & (U64(0x100) << i)) // IPINTR cleared
        m_sys->get_cpu(i)->irq_h(3, false, 0);
      if (data & (U64(0x1000) << i)) { // IPREQ
        state.cchip.misc |= U64(0x100) << i;
        m_sys->get_cpu(i)->irq_h(3, true, 0);
      }
    }
    return;
  }
  case 0x0c0: // MPD: <0> CKS (SCL driver), <1> DS (SDA driver)
    m_mpd.cks_out = (data & 1) != 0;
    m_mpd.ds_out = (data & 2) != 0;
    m_mpd_bus.drive_from_host(m_mpd.cks_out, m_mpd.ds_out);
    return;
  case 0x200:
  case 0x240:
  case 0x600:
  case 0x640: {
    std::lock_guard<std::mutex> g(m_lock);
    const int n = dim_index(a);
    state.cchip.dim[n] = data;
    if (n < m_sys->get_cpu_num())
      drive_lines(n);
    return;
  }
  case 0x340:
    state.cchip.prben = data;
    return;
  case 0x380:
  case 0x3c0:
    state.cchip.iic[(a >> 6) & 1] = data;
    return;
  case 0x700:
  case 0x740:
    state.cchip.iic[2 + ((a >> 6) & 1)] = data;
    return;
  case 0x400:
  case 0x440:
  case 0x480:
  case 0x4c0:
    state.cchip.mpr[(a >> 6) & 3] = data;
    return;
  case 0x580:
    state.cchip.ttr = data;
    return;
  case 0x5c0:
    state.cchip.tdr = data;
    return;
  case 0x780:
    state.cchip.pwr = data;
    return;
  case 0xc00:
    state.cchip.cmonctla = data;
    return;
  case 0xc40:
    state.cchip.cmonctlb = data;
    return;
  case 0xd00:
    state.cchip.cpen = data;
    return;
  default:
    m_sys->trace_unknown("Cchip CSR", U64(0x801a0000000) | a, 64, true, data,
                         source);
  }
}

/// Dchip CSRs (801.B000.0800): DSC, STR, DREV, DSC2, a byte each,
/// replicated across the quadword as on the Typhoon.
u64 CTitan::dchip_read(u32 a) {
  u64 v;
  switch (a) {
  case 0x800:
    v = state.dchip.dsc;
    break;
  case 0x840:
    v = state.dchip.str;
    break;
  case 0x880:
    v = state.dchip.drev;
    break;
  case 0x8c0:
    v = state.dchip.dsc2;
    break;
  default:
    m_sys->trace_unknown("Dchip CSR", U64(0x801b0000000) | a, 64, false, 0,
                         nullptr);
    return 0;
  }
  return (v & 0xff) * U64(0x0101010101010101);
}
