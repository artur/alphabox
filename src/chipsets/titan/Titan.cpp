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
 * The Titan's physical address decode, interrupt delivery, PCI hose spaces,
 * reset and state (Titan.hpp).
 **/
#include "Titan.hpp"
#include "AlphaCPU.hpp"
#include "PciWindows.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

CTitan::CTitan(CSystem *sys) : CChipset(sys) {
  const uint32_t *arrays = sys->memory_arrays();
  m_dimms = arrays ? model_dimm_arrays(arrays)
                   : model_dimms(static_cast<uint32_t>(
                         (1ULL << sys->get_memory_bits()) >> 20));
  attach_dimm_spd(m_mpd_bus, m_dimms);

  const titan_layout *board = sys->platform().titan;
  if (board) {
    m_cchip_rev = board->cchip_rev;
    m_agp = board->agp;
  }
  const unsigned bits = sys->get_memory_bits();
  if (arrays) {
    // The arrays as the configuration gives them (memory.arrays).
    for (int n = 0; n < 4; n++)
      m_array[n] = {m_dimms.array[n].base, m_dimms.array[n].bytes()};
  } else if (board && board->paired_arrays && bits > 30) {
    // Two equal arrays, 0 then 2, as the DS15's listing of 2048 MB shows
    // them: array 0 at 0, array 2 at half the memory.
    const u64 half = U64(1) << (bits - 1);
    m_array[0] = {0, half};
    m_array[2] = {half, half};
  } else {
    // Arrays of up to 8 GB from 0, in order, as the Typhoon's AARs [assumed
    // for the ES45 and DS25: no listing of theirs is of a power of two
    // above 8 GB].
    const unsigned arr_bits = bits > 33 ? 33 : bits;
    for (int n = 0; n < (1 << (bits - arr_bits)) && n < 4; n++)
      m_array[n] = {(u64)n << arr_bits, U64(1) << arr_bits};
  }
  power_on_state();
}

/**
 * Whether memory is interleaved: arrays 0 and 2 populated alike. The
 * consoles print "1-Way" when CSC<51> is set, which the real listings show
 * for one array (DS15 example 2-7, the DS25's show memory) and for arrays 0
 * and 2 of different sizes (the DS25's 512 + 1024 MB), and print 2-Way (or
 * 4-Way on the DS25 and ES45, when arrays 0 and 1 match too) otherwise:
 * arrays 0 and 2 of one size (DS15 example 2-5, the ES45's 10 GB). With
 * CSC<51> clear and one array the DS15 console printed an uninitialised
 * register as the mode. CSC<51> is what the SROM, which is not emulated,
 * leaves after configuring memory.
 **/
bool CTitan::interleaved() const {
  return m_array[0].size && m_array[0].size == m_array[2].size;
}

/**
 * The registers' power-on values. Where neither Linux nor the console says
 * what a register holds at reset, the Typhoon's value is used and marked.
 **/
void CTitan::power_on_state() {
  memset(&state, 0, sizeof(state));
  // CSC: the Typhoon's value [assumed]. Bit 14 (P1P) says PA-chip 1 is
  // there, which is how Linux decides whether hoses 1 and 3 exist.
  state.cchip.csc = U64(0x3142444014157803);
  if (!interleaved())
    state.cchip.csc |= U64(1) << 51;
  // The chip revisions: the console's show config prints MISC<39:32>, the
  // Dchips' DREV and each PA-chip's SCTL<7:0> (ES45 V7.3-2 console, 0x96fc0);
  // the real listings (owner's guides) say 17 for all three on the ES45,
  // and a Cchip of 18 on the DS25 and DS15 (the board row's cchip_rev).
  state.cchip.misc = (u64)m_cchip_rev << 32;
  state.dchip.drev = TITAN_REV;

  // The other Dchip registers: the Typhoon's values [assumed].
  state.dchip.dsc = 0x43;
  state.dchip.dsc2 = 0x03;
  state.dchip.str = 0x25;

  for (int h = 0; h < HOSES; h++) {
    // PCTL<17> PCISPD66 is a strap: the ES45's hose 0 runs at 33 MHz and the
    // others at 66 MHz (its owner's guide, show config) [board fact kept here
    // until a second Titan board needs otherwise].
    state.port[h].csr[0x300 >> 6] = (h == 0) ? 0 : U64(0x20000);
  }
  // APCTL<57> AGP_PRESENT, on PA-chip 0's A-port: the board's AGP bus
  // (titan_layout::agp). The ES45 console never sets it on hardware (its
  // setup_io, 0x900f0, sets it only when it runs in its simulator) and
  // keeps it when it clears the AGP rate and enable fields <55:52> and the
  // queue depths <63:58>: a strap, like PCISPD66 [inferred from that code].
  if (m_agp)
    state.port[2].csr[0x300 >> 6] |= U64(1) << 57;
  // SCTL is the G-port's; its low byte is the PA-chip's revision.
  state.port[0].csr[0x700 >> 6] = TITAN_REV;
  state.port[1].csr[0x700 >> 6] = TITAN_REV;
}

void CTitan::reset() {
  std::lock_guard<std::mutex> g(m_lock);
  power_on_state();
}

void CTitan::save_state(FILE *f) { fwrite(&state, sizeof(state), 1, f); }

bool CTitan::restore_state(FILE *f) {
  return fread(&state, sizeof(state), 1, f) == 1;
}

/**
 * Hose h's spaces (Linux core_titan.h): memory at 800.0000.0000, I/O at
 * 801.FC00.0000 and configuration at 801.FE00.0000, each plus h * 2.0000.0000.
 **/
u64 CTitan::pci_space_base(int hose, pci_space space) const {
  const u64 h = U64(0x0000000200000000) * (u64)(hose & 3);
  switch (space) {
  case PCI_SPACE_IO:
    return U64(0x00000801fc000000) + h;
  case PCI_SPACE_CONFIG:
    return U64(0x00000801fe000000) + h;
  case PCI_SPACE_MEM:
  default:
    return U64(0x0000080000000000) + h;
  }
}

/// Which hose's space `a` is in, and which space; false for CSR space.
static bool hose_space(u64 a, int *hose, pci_space *space) {
  const u64 off = a - U64(0x80000000000);
  *hose = (int)((off >> 33) & 3);
  const u64 in = off & U64(0x1ffffffff);
  if (in < U64(0x100000000)) {
    *space = PCI_SPACE_MEM;
    return true;
  }
  if (in >= U64(0x1fc000000) && in < U64(0x1fe000000)) {
    *space = PCI_SPACE_IO;
    return true;
  }
  if (in >= U64(0x1fe000000) && in < U64(0x1ff000000)) {
    *space = PCI_SPACE_CONFIG;
    return true;
  }
  return false;
}

u64 CTitan::read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) {
  if (a >= U64(0x801a0000000) && a < U64(0x801b0000000))
    return cchip_read((u32)a & 0xfffffff, source);
  if (a >= U64(0x801b0000000) && a < U64(0x801c0000000))
    return dchip_read((u32)a & 0xfffffff);
  if (a >= U64(0x80180000000) && a < U64(0x80190000000))
    return port_read(((a >> 12) & 1) ? 2 : 0, (u32)a & 0xfff);
  if (a >= U64(0x80380000000) && a < U64(0x80390000000))
    return port_read(((a >> 12) & 1) ? 3 : 1, (u32)a & 0xfff);
  if (a >= U64(0x80100000000) && a < U64(0x80140000000))
    return tig_read((u32)a & 0x3fffffff);

  int hose;
  pci_space space;
  if (a < U64(0x80800000000) && hose_space(a, &hose, &space)) {
    static const char *const what[3][4] = {
        {"PCI 0 memory", "PCI 1 memory", "PCI 2 memory", "PCI 3 memory"},
        {"PCI 0 I/O", "PCI 1 I/O", "PCI 2 I/O", "PCI 3 I/O"},
        {"PCI 0 configuration", "PCI 1 configuration", "PCI 2 configuration",
         "PCI 3 configuration"}};
    m_sys->trace_unknown(what[space][hose], a, dsize, false, 0, source);
    // Nothing answers: a configuration or I/O read is master-aborted and
    // reads as all ones. Memory reads as zero, as on the Tsunami.
    if (space == PCI_SPACE_MEM)
      return 0;
    return (dsize >= 64) ? ~U64(0) : ((U64(1) << dsize) - 1);
  }

  m_sys->trace_unknown("no device", raw, dsize, false, 0, source);
  return 0;
}

void CTitan::write_io(u64 a, u64 raw, int dsize, u64 data,
                      CSystemComponent *source) {
  if (a >= U64(0x801a0000000) && a < U64(0x801b0000000)) {
    cchip_write((u32)a & 0xfffffff, data, source);
    return;
  }
  if (a >= U64(0x801b0000000) && a < U64(0x801c0000000)) {
    // The Dchips' registers are set by the SROM; a console write is noted.
    m_sys->trace_unknown("Dchip CSR", a, dsize, true, data, source);
    return;
  }
  if (a >= U64(0x80180000000) && a < U64(0x80190000000)) {
    port_write(((a >> 12) & 1) ? 2 : 0, (u32)a & 0xfff, data);
    return;
  }
  if (a >= U64(0x80380000000) && a < U64(0x80390000000)) {
    port_write(((a >> 12) & 1) ? 3 : 1, (u32)a & 0xfff, data);
    return;
  }
  if (a >= U64(0x80100000000) && a < U64(0x80140000000)) {
    tig_write((u32)a & 0x3fffffff, (u8)data);
    return;
  }

  int hose;
  pci_space space;
  if (a < U64(0x80800000000) && hose_space(a, &hose, &space)) {
    static const char *const what[3] = {"PCI memory", "PCI I/O",
                                        "PCI configuration"};
    m_sys->trace_unknown(what[space], a, dsize, true, data, source);
    return;
  }
  m_sys->trace_unknown("no device", raw, dsize, true, data, source);
}

/**
 * b_irq<1> (device interrupts, DRIR<55:0>) and b_irq<0> (errors,
 * DRIR<63:58>) for one processor, as on the Typhoon: the Titan keeps the
 * DIM/DIR scheme and gives each of four processors its own mask (Linux
 * sys_titan.c titan_update_irq_hw). Device interrupts are delayed by 100
 * clocks, as the Tsunami model does.
 **/
void CTitan::drive_lines(int i) {
  if (state.cchip.drir & state.cchip.dim[i] & U64(0x00ffffffffffffff))
    m_sys->get_cpu(i)->irq_h(1, true, 100);
  else
    m_sys->get_cpu(i)->irq_h(1, false, 0);
  if (state.cchip.drir & state.cchip.dim[i] & U64(0xfc00000000000000))
    m_sys->get_cpu(i)->irq_h(0, true, 100);
  else
    m_sys->get_cpu(i)->irq_h(0, false, 0);
}

void CTitan::interrupt(int number, bool assert) {
  std::lock_guard<std::mutex> g(m_lock);
  if (number == -1) {
    // The interval timer: MISC<ITINTR> and b_irq<2> on every processor.
    g_irqstats.cchip_timer.fetch_add(1, std::memory_order_relaxed);
    state.cchip.misc |= 0xf0;
    for (int i = 0; i < m_sys->get_cpu_num(); i++)
      m_sys->get_cpu(i)->irq_h(2, true, 0);
    m_sys->note_interval_tick();
  } else if (assert) {
    if (!(state.cchip.drir & (U64(1) << number)))
      g_irqstats.drir_rise[number].fetch_add(1, std::memory_order_relaxed);
    state.cchip.drir |= U64(1) << number;
  } else {
    state.cchip.drir &= ~(U64(1) << number);
  }
  for (int i = 0; i < m_sys->get_cpu_num(); i++)
    drive_lines(i);
}

void CTitan::ack_interval_timer(int cpu) {
  std::lock_guard<std::mutex> g(m_lock);
  state.cchip.misc &= ~(U64(0x10) << cpu);
  m_sys->get_cpu(cpu)->irq_h(2, false, 0);
}

void CTitan::ack_ipi(int cpu) {
  std::lock_guard<std::mutex> g(m_lock);
  state.cchip.misc &= ~(U64(0x100) << cpu);
  m_sys->get_cpu(cpu)->irq_h(3, false, 0);
}

/**
 * DMA from a device on `hose`: the port's four windows (PciWindows.hpp),
 * subject to the 512 KB-1 MB hole when PCTL<HOLE> is set. An address no
 * window takes stays on the hose (peer to peer), as on the Tsunami.
 **/
u64 CTitan::pci_phys(int hose, u32 address) {
  const SState::SPort &p = state.port[hose & 3];
  const u64 pctl = p.csr[0x300 >> 6];
  if (!(pctl & 0x20) || address < 0x80000 || address > 0xfffff) {
    for (int j = 0; j < 4; j++) {
      const u64 wsba = p.csr[j], wsm = p.csr[4 + j], tba = p.csr[8 + j];
      if (!pci_window::hit(wsba, wsm, address))
        continue;
      if (!(wsba & 2))
        return pci_window::direct(address, wsm, tba);
      const u64 pte =
          m_sys->ReadMem(pci_window::pte_address(address, wsm, tba), 64, 0);
      if (pte & 1)
        return pci_window::from_pte(pte, address);
      break; // an invalid entry: not translated
    }
  }
  return pci_space_base(hose, PCI_SPACE_MEM) | (u64)address;
}
