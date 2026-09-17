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
 * Intel 8255x family: the CSRs (System Control Block, PORT, EEPROM and MDI
 * control) and the interrupt line.
 *
 * Both BARs map the same CSRs. An access of any width is a run of byte
 * accesses; side effects fire once per access, after all its bytes are
 * stored, keyed by the bytes it touched (a 32-bit PORT or MDI write acts
 * when its top byte is written, as the chip latches it).
 **/
#include "I8255x.hpp"
#include "StdAfx.hpp"

#include "I8255xRegs.hpp"

#if defined(DEBUG_NIC)
#define TRACE_CSR(...) printf(__VA_ARGS__)
#else
#define TRACE_CSR(...)
#endif

u32 CI8255x::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar == 2)
    return 0xffffffff >> (32 - dsize); // no flash fitted: erased

  std::lock_guard<std::mutex> lock(myLock);
  u32 data = 0;
  for (int i = 0; i < dsize / 8; i++)
    data |= u32(read_csr((address + i) & (CSR_SIZE - 1))) << (8 * i);
  TRACE_CSR("%s: read  %02x/%d = %08x\n", m_chip.name, address, dsize, data);
  return data;
}

void CI8255x::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                           u32 data) {
  if (bar == 2)
    return;

  std::lock_guard<std::mutex> lock(myLock);
  TRACE_CSR("%s: write %02x/%d = %08x\n", m_chip.name, address, dsize, data);
  address &= CSR_SIZE - 1;
  const int bytes = dsize / 8;
  for (int i = 0; i < bytes; i++)
    write_csr((address + i) & (CSR_SIZE - 1), u8(data >> (8 * i)));
  csr_written(address, address + bytes - 1);
  update_irq();
}

u8 CI8255x::read_csr(u32 offset) {
  switch (offset) {
  case CSR_EEPROM:
    return u8((state.csr[offset] & ~EEPROM_DO) |
              (state.eeprom.data_out() ? EEPROM_DO : 0));
  case CSR_FLASH:
  case CSR_FLASH + 1:
    return 0;
  case CSR_PMDR:
    return m_chip.generation >= 8 ? state.csr[offset] : 0;
  default:
    return state.csr[offset];
  }
}

void CI8255x::write_csr(u32 offset, u8 value) {
  switch (offset) {
  case SCB_STATUS: // read-only
  case CSR_GSTAT:
    return;
  case SCB_STATACK: // write 1 to acknowledge
    state.csr[offset] &= ~value;
    return;
  case CSR_PMDR: // write 1 to clear
    state.csr[offset] &= ~value;
    return;
  case CSR_MDI + 3: // ready is status
    state.csr[offset] = u8((value & ~(MDI_READY >> 24)) |
                           (state.csr[offset] & (MDI_READY >> 24)));
    return;
  default:
    state.csr[offset] = value;
    return;
  }
}

/**
 * Side effects of an access that wrote bytes first..last.
 **/
void CI8255x::csr_written(u32 first, u32 last) {
  auto touched = [first, last](u32 offset) {
    return first <= offset && offset <= last;
  };

  if (touched(SCB_INTMASK)) {
    u8 &mask = state.csr[SCB_INTMASK];
    if (m_chip.generation < 8)
      mask &= INTMASK_M | INTMASK_SI; // no per-cause masks on the 82557
    if (mask & INTMASK_SI) {
      mask &= ~INTMASK_SI;
      state.csr[SCB_STATACK] |= STAT_SWI;
    }
  }
  if (touched(SCB_COMMAND)) {
    const u8 command = state.csr[SCB_COMMAND];
    state.csr[SCB_COMMAND] = 0; // accepted
    scb_command(command);
  }
  if (touched(CSR_PORT + 3))
    port_command();
  if (touched(CSR_EEPROM))
    eeprom_pins();
  if (touched(CSR_MDI + 3))
    mdi_command();
}

/// A 32-bit CSR, assembled from its bytes.
u32 CI8255x::csr_long(u32 offset) const {
  return u32(state.csr[offset]) | u32(state.csr[offset + 1]) << 8 |
         u32(state.csr[offset + 2]) << 16 | u32(state.csr[offset + 3]) << 24;
}

void CI8255x::scb_command(u8 value) {
  const u32 pointer = csr_long(SCB_POINTER);

  if (value & RUC_MASK)
    ru_command(value & RUC_MASK, pointer);

  switch (value & CUC_MASK) {
  case CUC_NOP:
    break;
  case CUC_START:
    cu_start(pointer);
    break;
  case CUC_RESUME:
  case CUC_STATIC_RESUME:
    cu_resume();
    break;
  case CUC_LOAD_DUMP_ADDR:
    state.stats_addr = pointer;
    break;
  case CUC_DUMP_STATS:
    dump_statistics(false);
    break;
  case CUC_LOAD_BASE:
    state.cu_base = pointer;
    break;
  case CUC_DUMP_RESET_STATS:
    dump_statistics(true);
    break;
  default:
    printf("%s: unsupported CU command %02x\n", m_chip.name, value & CUC_MASK);
    break;
  }
}

void CI8255x::port_command() {
  const u32 port = csr_long(CSR_PORT);
  const u32 address = port & ~u32(3);

  switch (port & 3) {
  case PORT_SOFTWARE_RESET:
    software_reset();
    break;
  case PORT_SELF_TEST:
    // Signature (the ROM checksum, never zero) and a clean result, then
    // the reset every self-test ends with.
    dma_write32(address, 0x00ff00ff);
    dma_write32(address + 4, 0);
    software_reset();
    break;
  case PORT_SELECTIVE_RESET:
    selective_reset();
    break;
  case PORT_DUMP:
    // The internal register dump has no documented layout; leave the
    // buffer alone.
    break;
  }
  memset(state.csr + CSR_PORT, 0, 4);
}

void CI8255x::eeprom_pins() {
  const u8 pins = state.csr[CSR_EEPROM];
  state.eeprom.set_pins((pins & EEPROM_CS) != 0, (pins & EEPROM_SK) != 0,
                        (pins & EEPROM_DI) != 0);
}

/**
 * An MDI read or write cycle, finished at once. Addresses without a PHY
 * read as all ones, as the pulled-up management bus does.
 **/
void CI8255x::mdi_command() {
  u32 mdi = csr_long(CSR_MDI);
  const int op = (mdi >> MDI_OP_SHIFT) & 3;
  const int phy = (mdi >> MDI_PHY_SHIFT) & 0x1f;
  const int reg = (mdi >> MDI_REG_SHIFT) & 0x1f;

  if (op == MDI_OP_WRITE) {
    if (phy == CI8255xPhy::ADDRESS)
      state.phy.write(reg, u16(mdi & MDI_DATA_MASK));
  } else if (op == MDI_OP_READ) {
    const u16 value =
        phy == CI8255xPhy::ADDRESS ? state.phy.read(reg) : u16(0xffff);
    mdi = (mdi & ~u32(MDI_DATA_MASK)) | value;
  } else {
    return; // not a cycle
  }

  mdi |= MDI_READY;
  for (int i = 0; i < 4; i++)
    state.csr[CSR_MDI + i] = u8(mdi >> (8 * i));
  if (mdi & MDI_IE)
    raise(STAT_MDI);
}

void CI8255x::raise(u8 causes) {
  state.csr[SCB_STATACK] |= causes;
  update_irq();
}

/**
 * The interrupt line: any cause not masked, unless M masks them all. The
 * 82558 and later mask CX, FR, CNA, RNR, ER and FCP one by one; MDI and
 * SWI have no mask of their own.
 **/
void CI8255x::update_irq() {
  const u8 mask = state.csr[SCB_INTMASK];
  const u8 causes = state.csr[SCB_STATACK];
  bool asserted = false;

  if (!(mask & INTMASK_M)) {
    u8 masked = mask & (INTMASK_CX | INTMASK_FR | INTMASK_CNA | INTMASK_RNR);
    if (mask & INTMASK_ER)
      masked |= STAT_ER;
    if (mask & INTMASK_FCP)
      masked |= STAT_FCP;
    asserted = (causes & ~masked) != 0;
  }

  if (asserted != state.irq_asserted && do_pci_interrupt(0, asserted))
    state.irq_asserted = asserted;
}

int CI8255x::cu_state() const {
  return (state.csr[SCB_STATUS] & SCB_CUS_MASK) >> SCB_CUS_SHIFT;
}

int CI8255x::ru_state() const {
  return (state.csr[SCB_STATUS] & SCB_RUS_MASK) >> SCB_RUS_SHIFT;
}

void CI8255x::set_cu_state(int s) {
  state.csr[SCB_STATUS] =
      u8((state.csr[SCB_STATUS] & ~SCB_CUS_MASK) | s << SCB_CUS_SHIFT);
}

void CI8255x::set_ru_state(int s) {
  state.csr[SCB_STATUS] =
      u8((state.csr[SCB_STATUS] & ~SCB_RUS_MASK) | s << SCB_RUS_SHIFT);
}
