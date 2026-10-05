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

#include "Tsunami.hpp"
#include "AlphaCPU.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

CTsunami::CTsunami(CSystem *sys) : CChipset(sys) {
  // The SPD EEPROMs on the MPD pins describe the configured memory.
  if (const uint32_t *arrays = sys->memory_arrays()) {
    m_dimms = model_dimm_arrays(arrays);
    attach_dimm_spd(m_mpd_bus, m_dimms);
  } else {
    init_spd(static_cast<uint32_t>((1ULL << sys->get_memory_bits()) >> 20));
  }
  power_on_state();
}

/**
 * The power-on values of every register, for the constructor and for a
 * firmware-requested system reset.
 **/
void CTsunami::power_on_state() {
  for (int i = 0; i < 4; i++)
    state.cchip.dim[i] = 0;
  state.cchip.drir = 0;
  state.cchip.misc = U64(0x0000000800000000);
  state.cchip.csc = U64(0x3142444014157803);

  state.dchip.drev = 0x01;
  state.dchip.dsc = 0x43;
  state.dchip.dsc2 = 0x03;
  state.dchip.str = 0x25;

  for (int i = 0; i < 2; i++) {
    memset(&state.pchip[i], 0, sizeof(struct SState::SSys_pchip));
    state.pchip[i].wsba[3] = 2;
  }

  state.pchip[0].pctl = U64(0x0000104401440081);
  state.pchip[1].pctl = U64(0x0000504401440081);

  state.tig.FwWrite = 0;
  state.tig.HaltA = 0;
  state.tig.HaltB = 0;
  memset(state.tig.ipcr, 0, sizeof(state.tig.ipcr));
  state.tig.ModInfo = 0;

  memset(state.cf8_address, 0, sizeof(state.cf8_address));
}

void CTsunami::reset() { power_on_state(); }

void CTsunami::save_state(FILE *f) { fwrite(&state, sizeof(state), 1, f); }

bool CTsunami::restore_state(FILE *f) {
  return fread(&state, sizeof(state), 1, f) == 1;
}

/**
 * Where a hose's spaces are (HRM 10.1.1): Pchip n's PCI memory at
 * 800.0000.0000 + n * 2.0000.0000, its I/O at 801.FC00.0000 and its
 * configuration space at 801.FE00.0000 on the same stride.
 **/
u64 CTsunami::pci_space_base(int hose, pci_space space) const {
  const u64 stride = U64(0x0000000200000000) * hose;
  switch (space) {
  case PCI_SPACE_IO:
    return U64(0x00000801fc000000) + stride;
  case PCI_SPACE_CONFIG:
    return U64(0x00000801fe000000) + stride;
  case PCI_SPACE_MEM:
  default:
    return U64(0x0000080000000000) + stride;
  }
}

void CTsunami::set_dim(int ProcNum, u64 value) {
  std::lock_guard<std::mutex> g(m_lock);
  state.cchip.dim[ProcNum] = value;
  if (ProcNum >= m_sys->get_cpu_num())
    return;
  if (state.cchip.drir & value & U64(0x00ffffffffffffff))
    m_sys->get_cpu(ProcNum)->irq_h(1, true, 100);
  else
    m_sys->get_cpu(ProcNum)->irq_h(1, false, 0);
  if (state.cchip.drir & value & U64(0xfc00000000000000))
    m_sys->get_cpu(ProcNum)->irq_h(0, true, 100);
  else
    m_sys->get_cpu(ProcNum)->irq_h(0, false, 0);
}

/**
 * Clear the clock interrupt for a specific CPU. Used by the CPU to acknowledge
 *the interrupt.
 **/
void CTsunami::ack_interval_timer(int ProcNum) {
  std::lock_guard<std::mutex> g(m_lock);
  state.cchip.misc &= ~(U64(0x10) << ProcNum);
  m_sys->get_cpu(ProcNum)->irq_h(2, false, 0);
}

/**
 * Acknowledge an interprocessor interrupt: clear MISC<IPINTR>, drop b_irq<3>.
 **/
void CTsunami::ack_ipi(int ProcNum) {
  std::lock_guard<std::mutex> g(m_lock);
  state.cchip.misc &= ~(U64(0x100) << ProcNum);
  m_sys->get_cpu(ProcNum)->irq_h(3, false, 0);
#ifdef DEBUG_IPI
  printf("*** IP interrupt cleared for CPU %d (PALcode dispatch ack).\n",
         ProcNum);
#endif
}

/**
 * \brief Write 8, 4, 2 or 1 byte(s) to a 64-bit system address. This could be
 *memory, internal chipset registers, nothing or some device.
 *
 * Source: HRM, 10.1.1:
 *
 * System Space and Address Map
 *
 * The system address space is divided into two parts: system memory and PIO.
 *This division is indicated by physical memory bit <43> = 1 for PIO accesses
 *from the CPU, and by the PTP bit in the window registers for PTP accesses from
 *the Pchip. While the operating system may choose bit <40> instead of bit <43>
 *to represent PIO space, bit <43> is used throughout this chapter. In general,
 *bits <42:35> are don�t cares if bit <43> is asserted.
 *
 * There is 16GB of PIO space available on the 21272 chipset with 8GB assigned
 *to each Pchip. The Pchip supports up to bit <34> (35 bits total) of system
 *address. However, the Version 1 Cchip only supports 4GB of system memory (32
 *bits total). As described in Chapter 6, the CAPbus protocol between the Pchip
 *and Cchip does support up to bit <34>, as does the Cchip�s interface to the
 *CPU. The Typhoon Cchip supports 32GB of system memory (35 bits total).
 *
 * The system address space is divided as shown in the following table:
 *
 * \code
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Space             | Size   | System Address <43:0>         | Comments |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | System memory     |    4GB | 000.0000.0000 - 000.FFFF.FFFF | Cacheable and
 *prefetchable.     |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          | 8188GB | 001.0000.0000 - 7FF.FFFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI memory |    4GB | 800.0000.0000 - 800.FFFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | TIGbus            |    1GB | 801.0000.0000 - 801.3FFF.FFFF | addr<5:0> = 0.
 *Single byte      | |                   |        | | valid in quadword access.
 *| |                   |        |                               | 16MB
 *accessible.                |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |    1GB | 801.4000.0000 - 801.7FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 CSRs       |  256MB | 801.8000.0000 - 801.8FFF.FFFF | addr<5:0> = 0.
 *Quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |  256MB | 801.9000.0000 - 801.9FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Cchip CSRs        |  256MB | 801.A000.0000 - 801.AFFF.FFFF | addr<5:0> = 0.
 *Quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Dchip CSRs        |  256MB | 801.B000.0000 - 801.BFFF.FFFF | addr<5:0> = 0.
 *All eight bytes  | |                   |        | | in quadword access must be
 *| |                   |        |                               | identical. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |  768MB | 801.C000.0000 - 801.EFFF.FFFF | � | | Reserved
 *|  128MB | 801.F000.0000 - 801.F7FF.FFFF | �                               |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip 0 PCI IACK  |   64MB | 801.F800.0000 - 801.FBFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI I/O    |   32MB | 801.FC00.0000 - 801.FDFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI conf   |   16MB | 801.FE00.0000 - 801.FEFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |   16MB | 801.FF00.0000 - 801.FFFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI memory |    4GB | 802.0000.0000 - 802.FFFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |    2GB | 803.0000.0000 - 803.7FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 CSRs       |  256MB | 803.8000.0000 - 803.8FFF.FFFF | addr<5:0> = 0,
 *quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          | 1536MB | 803.9000.0000 - 803.EFFF.FFFF | � | | Reserved
 *|  128MB | 803.F000.0000 - 803.F7FF.FFFF | �                               |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip 1 PCI IACK  |   64MB | 803.F800.0000 - 803.FBFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI I/O    |   32MB | 803.FC00.0000 - 803.FDFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI conf   |   16MB | 803.FE00.0000 - 803.FEFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |   16MB | 803.FF00.0000 - 803.FFFF.FFFF | � | | Reserved
 *| 8172GB | 804.0000.0000 - FFF.FFFF.FFFF | Bits <42:35> are don�t cares if |
 * |                   |        |                               | bit <43> is
 *asserted.           |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * \endcode
 **/
void CTsunami::write_io(u64 a, u64 address, int dsize, u64 data,
                        CSystemComponent *source) {
  if ((a == U64(0x00000801FC000CF8)) && (dsize == 32)) {
    state.cf8_address[0] = (u32)data & 0x00ffffff;
    return;
  }

  if ((a == U64(0x00000803FC000CF8)) && (dsize == 32)) {
    state.cf8_address[1] = (u32)data & 0x00ffffff;
    return;
  }

  if ((a == U64(0x00000801FC000CFC)) && (dsize == 32)) {
    printf("PCI 0 config space write through CF8/CFC mechanism.   \n");
    getc(stdin);
    m_sys->WriteMem(U64(0x00000801FE000000) | state.cf8_address[0], dsize, data,
                    source);
    return;
  }

  if ((a == U64(0x00000803FC000CFC)) && (dsize == 32)) {
    printf("PCI 1 config space write through CF8/CFC mechanism.   \n");
    getc(stdin);
    m_sys->WriteMem(U64(0x00000803FE000000) | state.cf8_address[1], dsize, data,
                    source);
    return;
  }

  if (a >= U64(0x00000801A0000000) && a <= U64(0x00000801AFFFFFFF)) {
    cchip_csr_write((u32)a & 0xFFFFFFF, data, source);
    return;
  }

  if (a >= U64(0x0000080180000000) && a <= U64(0x000008018FFFFFFF)) {
    pchip_csr_write(0, (u32)a & 0xFFFFFFF, data);
    return;
  }

  if (a >= U64(0x0000080380000000) && a <= U64(0x000008038FFFFFFF)) {
    pchip_csr_write(1, (u32)a & 0xFFFFFFF, data);
    return;
  }

  if (a >= U64(0x00000801B0000000) && a <= U64(0x00000801BFFFFFFF)) {
    dchip_csr_write((u32)a & 0xFFFFFFF, (u8)data & 0xff);
    return;
  }

  if (a >= U64(0x0000080100000000) && a <= U64(0x000008013FFFFFFF)) {
    tig_write((u32)a & 0x3FFFFFFF, (u8)data);
    return;
  }

  if (a >= U64(0x801fc000000) && a < U64(0x801fe000000)) {

    // Unused PCI I/O space
    m_sys->trace_unknown("PCI 0 I/O", a, dsize, true, data, source);
    return;
  }

  if (a >= U64(0x803fc000000) && a < U64(0x803fe000000)) {

    // Unused PCI I/O space
    m_sys->trace_unknown("PCI 1 I/O", a, dsize, true, data, source);
    if (source) {
      printf("Write to unknown IO port %" PRIx64 " on PCI 1 from %s   \n",
             a & U64(0x1ffffff), source->devid_string);
    } else
      printf("Write to unknown IO port %" PRIx64 " on PCI 1   \n",
             a & U64(0x1ffffff));
    return;
  }

  if (a >= U64(0x80000000000) && a < U64(0x80100000000)) {

    // Unused PCI memory space
    m_sys->trace_unknown("PCI 0 memory", a, dsize, true, data, source);
    u64 paddr = a & U64(0xffffffff);
    if (paddr > 0xb8fff || paddr < 0xb8000) { // skip legacy video
      if (source) {
        printf("Write to unknown memory %" PRIx64 " on PCI 0 from %s   \n",
               a & U64(0xffffffff), source->devid_string);
      } else
        printf("Write to unknown memory %" PRIx64 " on PCI 0   \n",
               a & U64(0xffffffff));
    }
  }

  if (a >= U64(0x80200000000) && a < U64(0x80300000000)) {

    // Unused PCI memory space
    if (source) {
      printf("Write to unknown memory %" PRIx64 " on PCI 1 from %s   \n",
             a & U64(0xffffffff), source->devid_string);
    } else
      printf("Write to unknown memory %" PRIx64 " on PCI 1   \n",
             a & U64(0xffffffff));
    return;
  }

#ifdef DEBUG_UNKMEM
  if (source)
    printf("Write to unknown memory %" PRIx64 " from %s   \n", a,
           source->devid_string);
  else
    printf("Write to unknown memory %" PRIx64 "   \n", a);
#endif // defined(DEBUG_UNKMEM)
  // Outside every space the Tsunami decodes. Traced with the address as
  // the processor issued it, before CSystem's mask folded it into the
  // ES40's map: another machine's console (Marvel's EV7 CSRs at
  // 0xfff'ffc0'0000) is recognisable only from that.
  m_sys->trace_unknown("no device", address, dsize, true, data, source);
  return;
}

/**
 * \brief Read 8, 4, 2 or 1 byte(s) from a 64-bit system address. This could be
 *memory, internal chipset registers, nothing or some device.
 *
 * Source: HRM, 10.1.1:
 *
 * System Space and Address Map
 *
 * The system address space is divided into two parts: system memory and PIO.
 *This division is indicated by physical memory bit <43> = 1 for PIO accesses
 *from the CPU, and by the PTP bit in the window registers for PTP accesses from
 *the Pchip. While the operating system may choose bit <40> instead of bit <43>
 *to represent PIO space, bit <43> is used throughout this chapter. In general,
 *bits <42:35> are don�t cares if bit <43> is asserted.
 *
 * There is 16GB of PIO space available on the 21272 chipset with 8GB assigned
 *to each Pchip. The Pchip supports up to bit <34> (35 bits total) of system
 *address. However, the Version 1 Cchip only supports 4GB of system memory (32
 *bits total). As described in Chapter 6, the CAPbus protocol between the Pchip
 *and Cchip does support up to bit <34>, as does the Cchip�s interface to the
 *CPU. The Typhoon Cchip supports 32GB of system memory (35 bits total).
 *
 * The system address space is divided as shown in the following table:
 *
 * \code
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Space             | Size   | System Address <43:0>         | Comments |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | System memory     |    4GB | 000.0000.0000 - 000.FFFF.FFFF | Cacheable and
 *prefetchable.     |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          | 8188GB | 001.0000.0000 - 7FF.FFFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI memory |    4GB | 800.0000.0000 - 800.FFFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | TIGbus            |    1GB | 801.0000.0000 - 801.3FFF.FFFF | addr<5:0> = 0.
 *Single byte      | |                   |        | | valid in quadword access.
 *| |                   |        |                               | 16MB
 *accessible.                |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |    1GB | 801.4000.0000 - 801.7FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 CSRs       |  256MB | 801.8000.0000 - 801.8FFF.FFFF | addr<5:0> = 0.
 *Quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |  256MB | 801.9000.0000 - 801.9FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Cchip CSRs        |  256MB | 801.A000.0000 - 801.AFFF.FFFF | addr<5:0> = 0.
 *Quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Dchip CSRs        |  256MB | 801.B000.0000 - 801.BFFF.FFFF | addr<5:0> = 0.
 *All eight bytes  | |                   |        | | in quadword access must be
 *| |                   |        |                               | identical. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |  768MB | 801.C000.0000 - 801.EFFF.FFFF | � | | Reserved
 *|  128MB | 801.F000.0000 - 801.F7FF.FFFF | �                               |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip 0 PCI IACK  |   64MB | 801.F800.0000 - 801.FBFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI I/O    |   32MB | 801.FC00.0000 - 801.FDFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip0 PCI conf   |   16MB | 801.FE00.0000 - 801.FEFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |   16MB | 801.FF00.0000 - 801.FFFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI memory |    4GB | 802.0000.0000 - 802.FFFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |    2GB | 803.0000.0000 - 803.7FFF.FFFF | � |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 CSRs       |  256MB | 803.8000.0000 - 803.8FFF.FFFF | addr<5:0> = 0,
 *quadword access. |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          | 1536MB | 803.9000.0000 - 803.EFFF.FFFF | � | | Reserved
 *|  128MB | 803.F000.0000 - 803.F7FF.FFFF | �                               |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip 1 PCI IACK  |   64MB | 803.F800.0000 - 803.FBFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI I/O    |   32MB | 803.FC00.0000 - 803.FDFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Pchip1 PCI conf   |   16MB | 803.FE00.0000 - 803.FEFF.FFFF | Linear
 *addressing.              |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * | Reserved          |   16MB | 803.FF00.0000 - 803.FFFF.FFFF | � | | Reserved
 *| 8172GB | 804.0000.0000 - FFF.FFFF.FFFF | Bits <42:35> are don�t cares if |
 * |                   |        |                               | bit <43> is
 *asserted.           |
 * +-------------------+--------+-------------------------------+---------------------------------+
 * \endcode
 **/
u64 CTsunami::read_io(u64 a, u64 address, int dsize, CSystemComponent *source) {
  if ((a == U64(0x00000801FC000CFC)) && (dsize == 32)) {
    printf("PCI 0 config space read through CF8/CFC mechanism.   \n");
    getc(stdin);
    return m_sys->ReadMem(U64(0x00000801FE000000) | state.cf8_address[0], dsize,
                          source);
  }

  if ((a == U64(0x00000803FC000CFC)) && (dsize == 32)) {
    printf("PCI 1 config space read through CF8/CFC mechanism.   \n");
    getc(stdin);
    return m_sys->ReadMem(U64(0x00000803FE000000) | state.cf8_address[1], dsize,
                          source);
  }

  if (a >= U64(0x00000801A0000000) && a <= U64(0x00000801AFFFFFFF))
    return cchip_csr_read((u32)a & 0xFFFFFFF, source);

  if (a >= U64(0x0000080180000000) && a <= U64(0x000008018FFFFFFF))
    return pchip_csr_read(0, (u32)a & 0xFFFFFFF);

  if (a >= U64(0x0000080380000000) && a <= U64(0x000008038FFFFFFF))
    return pchip_csr_read(1, (u32)a & 0xFFFFFFF);

  if (a >= U64(0x00000801B0000000) && a <= U64(0x00000801BFFFFFFF))
    return dchip_csr_read((u32)a & 0xFFFFFFF) * U64(0x0101010101010101);

  if (a >= U64(0x0000080100000000) && a <= U64(0x000008013FFFFFFF))
    return tig_read((u32)a & 0x3FFFFFFF);

  if ((a >= U64(0x801fe000000) && a < U64(0x801ff000000)) ||
      (a >= U64(0x803fe000000) && a < U64(0x803ff000000))) {

    // Unused PCI configuration space
    m_sys->trace_unknown("PCI configuration", a, dsize, false, 0, source);
    switch (dsize) {
    case 8:
      return X64_BYTE;
    case 16:
      return X64_WORD;
    case 32:
      return X64_LONG;
    case 64:
      return X64_QUAD;
    }
  }

  if (a >= U64(0x800000c0000) && a < U64(0x800000e0000)) {

    // The option ROM window with nothing shadowed into it. It reads as
    // zero, but it is worth tracing: this is where the console looks for
    // a video BIOS, and a card that fails to answer here is a card the
    // console will never bring up.
    m_sys->trace_unknown("PCI ROM BIOS", a, dsize, false, 0, source);
    return 0;
  }

  if (a >= U64(0x801fc000000) && a < U64(0x801fe000000)) {

    // Unused PCI I/O space: nothing answers, the read is master-aborted
    // and the data returned is all ones, as on any PCI host. It used to
    // read as zero, and zero is the worst value an ISA status port can
    // show: Windows 2000, told by the firmware tree of a parallel port at
    // 0x3BC that no device here models, read its status as "printer
    // attached, busy, acknowledging" and ran its IEEE-1284 negotiation to
    // every timeout -- 18M status reads per boot.
    m_sys->trace_unknown("PCI 0 I/O", a, dsize, false, 0, source);
    return (dsize >= 64) ? ~U64(0) : ((U64(1) << dsize) - 1);
  }

  if (a >= U64(0x803fc000000) && a < U64(0x803fe000000)) {

    // Unused PCI I/O space
    m_sys->trace_unknown("PCI 1 I/O", a, dsize, false, 0, source);
    if (source) {
      printf("Read from unknown IO port %" PRIx64 " on PCI 1 from %s   \n",
             a & U64(0x1ffffff), source->devid_string);
    } else
      printf("Read from unknown IO port %" PRIx64 " on PCI 1   \n",
             a & U64(0x1ffffff));
    return 0;
  }

  if (a >= U64(0x80000000000) && a < U64(0x80100000000)) {

    // Unused PCI memory space
    m_sys->trace_unknown("PCI 0 memory", a, dsize, false, 0, source);
    u64 paddr = a & U64(0xffffffff);
    if (paddr > 0xb8fff || paddr < 0xb8000) { // skip legacy video
      if (source) {
        printf("Read from unknown memory %" PRIx64 " on PCI 0 from %s   \n",
               a & U64(0xffffffff), source->devid_string);
      } else
        printf("Read from unknown memory %" PRIx64 " on PCI 0   \n",
               a & U64(0xffffffff));
    }

    return 0;
  }

  if (a >= U64(0x80200000000) && a < U64(0x80300000000)) {

    // Unused PCI memory space
    if (source) {
      printf("Read from unknown memory %" PRIx64 " on PCI 1 from %s   \n",
             a & U64(0xffffffff), source->devid_string);
    } else
      printf("Read from unknown memory %" PRIx64 " on PCI 1   \n",
             a & U64(0xffffffff));
    return 0;
  }

#if defined(DEBUG_UNKMEM)
  if (source)
    printf("Read from unknown memory %" PRIx64 " from %s   \n", a,
           source->devid_string);
  else
    printf("Read from unknown memory %" PRIx64 "   \n", a);
#endif // defined(DEBUG_UNKMEM)
  // As in WriteMem: the unmasked address, for another machine's console.
  m_sys->trace_unknown("no device", address, dsize, false, 0, source);
  return 0x00;
}

/**
 * \brief Assert or deassert one of 64 possible interrupt lines on the Tsunami
 *chipset.
 *
 * Source: HRM, 6.3:
 *
 * TIGbus and Interrupts
 *
 * The TIGbus supports miscellaneous system logic such as flash ROM and
 * interrupt inputs. The Cchip TIG controller polls interrupts continuously
 * except when a read or write to flash is requested. The 64 possible interrupt
 *inputs are polled eight at a time by selecting a byte with the b_tia<2:0>
 *pins, and asserting b_toe_l to allow the selected byte to be driven onto
 *b_td<7:0>. Using the polled interrupts, the Cchip calculates the b_irq values
 *that should be delivered to the CPUs. When any change occurs in these b_irq
 *values, the Cchip drives the b_irq<3:0> data for both CPUs onto b_td<7:0>, and
 *asserts signal b_tis to strobe it into a register on the module. If there is
 *no flash read or write outstanding, the polling process is repeated. If there
 * is a flash read or write outstanding, it is serviced between interrupt reads
 *after any pending b_irq updates. Thus, the rounds of interrupt polling are not
 *atomic, but the b_irq values reflect the most recently polled interrupts.
 *Furthermore, b_irq<1> may be artificially suppressed for one full polling loop
 *using the CSR bit MISC<DEVSUP>.
 * [...]
 *
 * Device and Error Interrupt Delivery - b_irq<1:0>
 *
 * As interrupts are read into the Cchip through the TIGbus, the corresponding
 *bits are set in DRIR. These bits are ANDed with the mask bits in DIMn and then
 *placed in DIRn. If any bits are set in DIRn<55:0>, then CPUn is interrupted
 *using CPU pin b_irq<1>.
 *
 * Interrupt bits <62:58> cause b_irq<0> to be asserted and are intended for use
 *as error signals. Assertion of interrupt bits <62:58> causes b_irq<0> to be
 *asserted. Interrupt bits <62:61> can be used for Pchip 0 and Pchip 1 errors,
 *respectively. Interrupt bit <63> is special because it is not read from the
 *TIGbus, but is internally generated as the Cchip detected error interrupt
 *(currently used only for NXM requests). Assertion of interrupt bit <63> causes
 *b_irq<0> to be asserted. See Chapter 10 for descriptions of the
 *interrupt-related CSRs (DRIR, DIMn, DIRn, and MISC). A full mask register for
 * each CPU allows software to decide whether to send each of the 64 possible
 *interrupts to either or both CPUs.
 *
 * After handling all known outstanding interrupts, software may suppress
 *b_irq<1> device interrupts to allow the Cchip�s polling mechanism to detect
 *the updated (deasserted) value of the interrupt lines from the PCI devices and
 *thereby avoid giving the CPU �stale� interrupts, which require passive
 *release. The field MISC<DEVSUP> is provided for this purpose. When a CPU
 *writes a one to its bit in MISC<DEVSY> the Cchip deasserts b_irq<1> to that
 *CPU (regardless of the value in the DIRn) until it has completed an entire
 *polling loop. When the Cchip has completed an entire polling loop, b_irq<1>
 *will again reflect the value of DIRn<55:00>.
 *
 * Interval Timer Interrupts - b_irq<2>
 *
 * The interval timer interrupts the Cchip through a dedicated pin, i_intim_l,
 *and is asserted low. When the Cchip sees an asserting (falling) edge of this
 *pin, it asserts MISC<ITINTR> for both CPUs. Pin b_irq<2> remains asserted for
 *each CPU <ITINTR>. When the CPU has finished handling the interrupt, it writes
 *a one to its MISC<ITINTR> bit to clear it. Software can suppress interval
 *timer interrupts for n cycles by writing n into IICn.
 *
 * This table shows TIG Interrupts and IRQ Lines
 *
 * \code
 * +---------------+-----------------+--------+----------------------------------------+
 * | TIG Interrupt | Assertion Level | irq<n> | Use |
 * +---------------+-----------------+--------+----------------------------------------+
 * |            63 | N/A             | irq<0> | N/C (internally generated Cchip
 *error) | |               |                 |        |     (currently NXM only)
 *|
 * +---------------+-----------------+--------+----------------------------------------+
 * |         62:58 | High            | irq<0> | Errors (Pchips, and so on) | |
 *|                 |        | Recommended:                           | | | | |
 ** Bit <62> - Pchip0 error            | |               |                 | |
 ** Bit <61> - Pchip1 error            |
 * +---------------+-----------------+--------+----------------------------------------+
 * |         57:56 | N/A             | N/A    | Reserved |
 * +---------------+-----------------+--------+----------------------------------------+
 * |          55:0 | Low             | irq<1> | PCI devices (level sensitive) |
 * +---------------+-----------------+--------+----------------------------------------+
 * \endcode
 *
 * It is not clear from the documentation how exactly the interval timer is
 *connected to the CChip. It looks like this is tied to the interrupt-line from
 *the real-time clock (TOY), as the periodic interval rate for the TOY is set to
 *1024 Hz by SRM. 1024 Hz is the frequency of the system timer interrupt
 *according to the OpenVMS Alpha Internals and Data Structures Handbook.
 *
 **/
void CTsunami::interrupt(int number, bool assert) {
  int i;

  // Serialize drir RMW + delivery against other device threads; irq_h() is
  // lock-free and never re-enters here, so this is deadlock-free.
  std::lock_guard<std::mutex> drirGuard(m_lock);

  if (number == -1) {

    // timer int...
    g_irqstats.cchip_timer.fetch_add(1, std::memory_order_relaxed);
    state.cchip.misc |= 0xf0;
    for (i = 0; i < m_sys->get_cpu_num(); i++)
      m_sys->get_cpu(i)->irq_h(2, true, 0); // timer interrupt is immediate
    m_sys->note_interval_tick();
  } else if (assert) {

    //    if (!(state.cchip.drir & (1i64<<number)))
    //      printf("%%TYP-I-INTERRUPT: Interrupt %d asserted.\n",number);
    if (!(state.cchip.drir & (U64(0x1) << number)))
      g_irqstats.drir_rise[number].fetch_add(1, std::memory_order_relaxed);
    state.cchip.drir |= (U64(0x1) << number);
  } else {

    //    if (state.cchip.drir & (1i64<<number))
    //      printf("%%TYP-I-INTERRUPT: Interrupt %d deasserted.\n",number);
    state.cchip.drir &= ~(U64(0x1) << number);
  }

  // Device interrupts are delayed by 100 clocks.
  for (i = 0; i < m_sys->get_cpu_num(); i++) {
    if (state.cchip.drir & state.cchip.dim[i] & U64(0x00ffffffffffffff))
      m_sys->get_cpu(i)->irq_h(1, true, 100);
    else
      m_sys->get_cpu(i)->irq_h(1, false, 0);

    if (state.cchip.drir & state.cchip.dim[i] & U64(0xfc00000000000000))
      m_sys->get_cpu(i)->irq_h(0, true, 100);
    else
      m_sys->get_cpu(i)->irq_h(0, false, 0);
  }
}
