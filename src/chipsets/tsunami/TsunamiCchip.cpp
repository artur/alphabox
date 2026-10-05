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
#include "StdAfx.hpp"
#include "System.hpp"
#include "Tsunami.hpp"

u64 CTsunami::cchip_csr_read(u32 a, CSystemComponent *source) {
  CAlphaCPU *cpu = (CAlphaCPU *)source;
  switch (a) {
  case 0x000:
    return state.cchip.csc;

  case 0x080:

    //    printf("MISC: %016" PRIx64 " from CPU %d (@%" PRIx64 ") (other @ %" LL
    //    "x).\n",state.cchip.misc | cpu->get_cpuid(),cpu->get_cpuid(),
    //    cpu->get_pc()-4, acCPUs[1-cpu->get_cpuid()]->get_pc());
    {
      // Consistent read of MISC against the concurrent RMW in
      // cchip_csr_write()/ack_interval_timer().
      std::lock_guard<std::mutex> g(m_lock);
      return state.cchip.misc | cpu->get_cpuid();
    }

  case 0x0c0: { // MPD: bit3=DR (SDA read), bit2=CKR (SCL read), bits1:0 read
                // as 0
    u8 v = 0;
    v |= (m_mpd_bus.scl() ? 1 : 0) << 2; // CKR
    v |= (m_mpd_bus.sda() ? 1 : 0) << 3; // DR
    return v;
  }

  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0: {
    // AAR0-3: memory as up to 4 arrays of at most 8 GB each (ASIZ 1010 is
    // the Typhoon maximum), the base in ADDR<34:24> and the size
    // 2^(ASIZ + 23) bytes in <15:12>. From the DIMM model the DPR reports
    // (init_spd): array n at n * 8 GB from memory.bits, or the arrays of
    // memory.arrays.
    const dimm_array &arr = m_dimms.array[(a >> 6) & 3];
    if (!arr.dimms)
      return 0; // array not populated
    unsigned int arr_bits = 0;
    while ((U64(1) << arr_bits) < arr.bytes())
      arr_bits++;
    return arr.base | ((u64)(arr_bits - 23) << 12);
  }

  case 0x200:
  case 0x240:
  case 0x600:
  case 0x640:
    return state.cchip.dim[((a >> 9) & 2) | ((a >> 6) & 1)];

  case 0x280:
  case 0x2c0:
  case 0x680:
  case 0x6c0:
    return state.cchip.drir & state.cchip.dim[((a >> 9) & 2) | ((a >> 6) & 1)];

  case 0x300:
    return state.cchip.drir;

  default:
    printf("Unknown CCHIP CSR %07x read attempted.\n", a);
    return 0;
  }
}

void CTsunami::cchip_csr_write(u32 a, u64 data, CSystemComponent *source) {
  CAlphaCPU *cpu = (CAlphaCPU *)source;
  switch (a) {
  case 0x000: // CSC
    state.cchip.csc &= ~U64(0x0777777fff3f0000);
    state.cchip.csc |= (data & U64(0x0777777fff3f0000));
    return;

  case 0x080: { // MISC
    // Interprocessor interrupt requests and arbitration live here.
    if (data & U64(0x0000000000000ff0))
      m_sys->trace_mp("Cchip MISC write", 0x080, data);
    // Serialize with interrupt()/ack_ipi()/ack_interval_timer(): an unlocked
    // IPI ack on one CPU thread could otherwise erase an IPI another CPU
    // thread is raising (irq_h's check-then-act), hanging the sender.
    std::lock_guard<std::mutex> g(m_lock);
    state.cchip.misc |= (data & U64(0x00000f0000f00000));  // W1S
    state.cchip.misc &= ~(data & U64(0x0000000010000ff0)); // W1C
    if (data & U64(0x0000000001000000)) {
      state.cchip.misc &= ~U64(0x0000000000ff0000); // Arbitration Clear
      printf("Arbitration clear from CPU %d (@%" PRIx64 ").\n",
             cpu->get_cpuid(), cpu->get_pc() - 4);
      // The console's election of its primary: a board whose processors
      // all run from reset starts the others now (CSystem).
      m_sys->arbitration_cleared(cpu->get_cpuid());
    }

    if (data & U64(0x00000000000f0000)) {
      printf("Arbitration %016" PRIx64 " from CPU %d (@%" PRIx64 ")... ", data,
             cpu->get_cpuid(), cpu->get_pc() - 4);
      if (!(state.cchip.misc & U64(0x00000000000f0000))) {
        state.cchip.misc |= (data & U64(0x00000000000f0000)); // Arbitration won
        printf("won  %016" PRIx64 "\n", state.cchip.misc);
      } else
        printf("lost %016" PRIx64 "\n", state.cchip.misc);
    }

    // stop interval timer interrupt
    if (data & U64(0x00000000000000f0)) {
      for (int i = 0; i < m_sys->get_cpu_num(); i++) {
        if (data & (U64(0x10) << i)) {
          m_sys->get_cpu(i)->irq_h(2, false, 0);

          // printf("*** TIMER interrupt cleared for CPU %d\n",i);
        }
      }
    }

    // stop inter processor interrupt
    if (data & U64(0x0000000000000f00)) {
      for (int i = 0; i < m_sys->get_cpu_num(); i++) {
        if (data & (U64(0x100) << i)) {
          m_sys->get_cpu(i)->irq_h(3, false, 0);
#ifdef DEBUG_IPI
          printf("*** IP interrupt cleared for CPU %d from CPU %d(@ %" PRIx64
                 ").\n",
                 i, cpu->get_cpuid(), cpu->get_pc() - 4);
#endif
        }
      }
    }

    // set inter processor interrupt
    if (data & U64(0x000000000000f000)) {
      for (int i = 0; i < m_sys->get_cpu_num(); i++) {
        if (data & (U64(0x1000) << i)) {
          state.cchip.misc |= U64(0x100) << i;
          m_sys->get_cpu(i)->irq_h(3, true, 0);
#ifdef DEBUG_IPI
          printf("*** IP interrupt set for CPU %d from CPU %d(@ %" PRIx64 ")\n",
                 i, cpu->get_cpuid(), cpu->get_pc() - 4);
#endif
        }
      }
    }

    return;
  }

  case 0x0c0: { // MPD
    // MPD: bit0=CKS (SCL driver: 1=release, 0=pull low)
    //      bit1=DS  (SDA driver: 1=release, 0=pull low)
    bool new_cks = (data & 0x1) != 0;
    bool new_ds = (data & 0x2) != 0;
    m_mpd.cks_out = new_cks;
    m_mpd.ds_out = new_ds;
    m_mpd_bus.drive_from_host(m_mpd.cks_out, m_mpd.ds_out); // SCL, SDA
    return;
  }

  case 0x200:
  case 0x240:
  case 0x600:
  case 0x640: {
    // DIMn: serialize with interrupt(), and re-drive that CPU's device lines
    // now -- the HAL may route (or mask) a device that is already asserted,
    // and the next interrupt() call could be a long way off.
    std::lock_guard<std::mutex> g(m_lock);
    const int n = ((a >> 9) & 2) | ((a >> 6) & 1);
    state.cchip.dim[n] = data;
    if (n < m_sys->get_cpu_num()) {
      if (state.cchip.drir & state.cchip.dim[n] & U64(0x00ffffffffffffff))
        m_sys->get_cpu(n)->irq_h(1, true, 100);
      else
        m_sys->get_cpu(n)->irq_h(1, false, 0);
      if (state.cchip.drir & state.cchip.dim[n] & U64(0xfc00000000000000))
        m_sys->get_cpu(n)->irq_h(0, true, 100);
      else
        m_sys->get_cpu(n)->irq_h(0, false, 0);
    }
    return;
  }

  default:
    printf("Unknown CCHIP CSR %07x write with %016" PRIx64 " attempted.\n", a,
           data);
  }
}

u8 CTsunami::dchip_csr_read(u32 a) {
  switch (a) {
  case 0x800: // DSC
    return state.dchip.dsc;
  case 0x840: // STR
    return state.dchip.str;
  case 0x880: // DREV
    return state.dchip.drev;
  case 0x8c0: // DSC2
    return state.dchip.dsc2;
  default:
    printf("Unknown DCHIP CSR %07x read attempted.\n", a);
    return 0;
  }
}

void CTsunami::dchip_csr_write(u32 a, u8 data) {
  printf("Unknown DCHIP CSR %07x write with %02x attempted.\n", a, data);
}
