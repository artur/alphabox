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
 * The CMD 649 PCI-IDE controller (Cmd649.hpp).
 **/
#include "Cmd649.hpp"
#include "StdAfx.hpp"

#include <atomic>

namespace {

// Configuration registers beyond the header.
constexpr u32 CFR = 0x50;       // <2> primary channel interrupt
constexpr u32 CNTRL = 0x51;     // <2>/<3> channel enables, <6>/<7> read-ahead
constexpr u32 ARTTIM23 = 0x57;  // <4> secondary channel interrupt
constexpr u32 MRDMODE = 0x71;   // <3:2> both interrupts, <5:4> blocked
constexpr u32 UDIDETCR0 = 0x73; // primary channel UDMA timing
constexpr u32 BMIDECSR = 0x79;  // bus master byte 9
constexpr u32 UDIDETCR1 = 0x7b; // secondary channel UDMA timing
constexpr u8 CFR_INTR_CH0 = 0x04;
constexpr u8 ARTTIM23_INTR_CH1 = 0x10;
constexpr u8 MRDMODE_INTR_CH0 = 0x04, MRDMODE_INTR_CH1 = 0x08;
constexpr u8 MRDMODE_BLK_CH0 = 0x10;

// ALPHABOX_IDETRACE: also the CMD registers a driver touches (the first
// few hundred accesses).
const bool g_trace = getenv("ALPHABOX_IDETRACE") != nullptr;
bool trace_more() {
  static std::atomic<int> n{0};
  return g_trace && n++ < 400;
}

u32 cmd649_cfg_data[64] = {
    /*00*/ 0x06491095, // CFID: CMD, PCI0649
    /*04*/ 0x02900000, // CFCS: DEVSEL medium, fast back-to-back, cap. list
    /*08*/ 0x01018f02, // CFRV: IDE, both channels native, revision 2
    /*0c*/ 0x00000000, // CFLT: latency timer + cache line size
    /*10*/ 0x00000001, // BAR0: primary command block, 8 bytes of I/O
    /*14*/ 0x00000001, // BAR1: primary control block, 4 bytes of I/O
    /*18*/ 0x00000001, // BAR2: secondary command block
    /*1c*/ 0x00000001, // BAR3: secondary control block
    /*20*/ 0x00000001, // BAR4: bus master block, 16 bytes of I/O
    /*24*/ 0x00000000, // BAR5
    /*28*/ 0x00000000, // CCIC: CardBus
    /*2c*/ 0x06491095, // CSID: subsystem = the part itself
    /*30*/ 0x00000000, // BAR6: no expansion ROM
    /*34*/ 0x00000060, // CCAP: power management at 0x60
    /*38*/ 0x00000000,
    /*3c*/ 0x040201ff, // CFIT: INTA, no line yet
    /*40*/ 0, 0, 0, 0,
    // 0x50 CFR, 0x51 CNTRL (both channels enabled), 0x52 CMDTIM,
    // 0x53 ARTTIM0, 0x54 DRWTIM0, 0x55 ARTTIM1, 0x56 DRWTIM1, 0x57 ARTTIM23
    /*50*/ 0x00000c00, 0x00000000,
    /*58*/ 0, 0,
    /*60*/ 0x00020001, // PMC: power management 1.1
    /*64*/ 0x00000000, // PMCSR: D0
    /*68*/ 0, 0,
    // 0x70 BMIDECR0, 0x71 MRDMODE, 0x72 BMIDESR0, 0x73 UDIDETCR0;
    // 0x78 BMIDECR1, 0x79 BMIDECSR, 0x7a BMIDESR1, 0x7b UDIDETCR1
    /*70*/ 0, 0, 0, 0};

u32 cmd649_cfg_mask[64] = {
    /*00*/ 0x00000000,
    /*04*/ 0x00000105, // CFCS: I/O, bus master, SERR
    /*08*/ 0x00000000,
    /*0c*/ 0x0000ffff, // CFLT
    /*10*/ 0xfffffff8, // BAR0
    /*14*/ 0xfffffffc, // BAR1
    /*18*/ 0xfffffff8, // BAR2
    /*1c*/ 0xfffffffc, // BAR3
    /*20*/ 0xfffffff0, // BAR4
    /*24*/ 0, 0, 0, 0, 0, 0,
    /*3c*/ 0x000000ff, // CFIT: interrupt line
    /*40*/ 0, 0, 0, 0,
    // CNTRL but the primary's enable, CMDTIM, the timings; ARTTIM23 but
    // its interrupt bit; DRWTIM2, BRST, DRWTIM3
    /*50*/ 0xffffcc00, 0xefffffff,
    /*58*/ 0xff00ffff, 0,
    /*60*/ 0x00000000,
    /*64*/ 0x00000003, // PMCSR: power state
    /*68*/ 0, 0,
    // MRDMODE <5:4> and <1:0>, UDIDETCR0; BMIDECSR, UDIDETCR1
    /*70*/ 0xff003300, 0, 0xff00ff00, 0};

} // namespace

CCmd649::CCmd649(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev)
    : CIdeController(cfg, c, pcibus, pcidev) {}

void CCmd649::add_functions() {
  add_function(0, cmd649_cfg_data, cmd649_cfg_mask);
  printf("%s: CMD 649 PCI-IDE controller.\n", devid_string);
}

u8 CCmd649::cfg8(u32 offset) const {
  return ((const u8 *)pci_state.config_data[0])[offset];
}

void CCmd649::set_cfg8(u32 offset, u8 value) {
  ((u8 *)pci_state.config_data[0])[offset] = value;
}

bool CCmd649::intr_latched(int ch) const {
  return ch ? (cfg8(ARTTIM23) & ARTTIM23_INTR_CH1) != 0
            : (cfg8(CFR) & CFR_INTR_CH0) != 0;
}

void CCmd649::set_intr_latched(int ch, bool on) {
  const u32 reg = ch ? ARTTIM23 : CFR;
  const u8 bit = ch ? ARTTIM23_INTR_CH1 : CFR_INTR_CH0;
  set_cfg8(reg, on ? (cfg8(reg) | bit) : (cfg8(reg) & ~bit));
}

void CCmd649::update_line() {
  std::lock_guard<std::mutex> lk(m_line_mx);
  const u8 blk = cfg8(MRDMODE);
  bool level = false;
  for (int ch = 0; ch < 2; ++ch)
    level |= irq_pending(ch) && !(blk & (MRDMODE_BLK_CH0 << ch));
  if (level != m_line) {
    m_line = level;
    do_pci_interrupt(0, level);
  }
}

void CCmd649::irq_raise(int ch) {
  set_intr_latched(ch, true);
  update_line();
}

void CCmd649::irq_lower(int) { update_line(); }

void CCmd649::ResetPCI() {
  CIdeController::ResetPCI(); // the latches are back at 0 with the rest
  update_line();
}

// The interrupt bits are latched in CFR and ARTTIM23; MRDMODE shows both.
u32 CCmd649::config_read_custom(int func, u32 address, int dsize, u32 data) {
  if (func != 0)
    return data;
  if (address >= 0x40 && trace_more())
    printf("CMDT config read @%02x/%d\n", address, dsize);
  const u32 n = (u32)dsize / 8;
  if (MRDMODE >= address && MRDMODE < address + n) {
    const u8 bits = (intr_latched(0) ? MRDMODE_INTR_CH0 : 0) |
                    (intr_latched(1) ? MRDMODE_INTR_CH1 : 0);
    data |= (u32)bits << (8 * (MRDMODE - address));
  }
  return data;
}

// Writing an interrupt bit back as 1 clears it, in whichever register.
void CCmd649::config_write_custom(int func, u32 address, int dsize, u32, u32,
                                  u32 raw) {
  if (func != 0)
    return;
  if (address >= 0x40 && trace_more())
    printf("CMDT config write @%02x/%d = %08x\n", address, dsize, raw);
  const u32 n = (u32)dsize / 8;
  auto byte = [&](u32 reg, u8 *v) {
    if (reg < address || reg >= address + n)
      return false;
    *v = (u8)(raw >> (8 * (reg - address)));
    return true;
  };
  u8 v;
  if (byte(CFR, &v) && (v & CFR_INTR_CH0))
    set_intr_latched(0, false);
  if (byte(ARTTIM23, &v) && (v & ARTTIM23_INTR_CH1))
    set_intr_latched(1, false);
  if (byte(MRDMODE, &v)) {
    if (v & MRDMODE_INTR_CH0)
      set_intr_latched(0, false);
    if (v & MRDMODE_INTR_CH1)
      set_intr_latched(1, false);
    update_line(); // the blocking bits may have changed
  }
}

u32 CCmd649::bm_alias(u32 address) {
  switch (address) {
  case 1:
    return MRDMODE;
  case 3:
    return UDIDETCR0;
  case 9:
    return BMIDECSR;
  case 11:
    return UDIDETCR1;
  }
  return 0;
}

// ALPHABOX_IDETRACE=2: every register access (a debugging aid, capped).
static bool reg_trace() {
  static const bool on =
      getenv("ALPHABOX_IDETRACE") && !strcmp(getenv("ALPHABOX_IDETRACE"), "2");
  static std::atomic<int> n{0};
  return on && n++ < 200000;
}

u32 CCmd649::ReadMem_Bar(int func, int bar, u32 address, int dsize) {
  if (bar != BAR_BUSMASTER) {
    const u32 v = CIdeController::ReadMem_Bar(func, bar, address, dsize);
    if (!(address == 0 && (bar == 0 || bar == 2)) && reg_trace())
      printf("CMDR bar%d+%x/%d -> %x\n", bar, address, dsize, v);
    return v;
  }
  if (dsize == 8) {
    if (const u32 reg = bm_alias(address)) {
      if (trace_more())
        printf("CMDT BM read @%x\n", address);
      return config_read(0, reg, 8);
    }
    return CIdeController::ReadMem_Bar(func, bar, address, 8);
  }
  // Wider: byte by byte, each from its own register.
  u32 v = 0;
  for (int i = 0; i < dsize / 8; ++i)
    v |= ReadMem_Bar(func, bar, address + i, 8) << (8 * i);
  return v;
}

void CCmd649::WriteMem_Bar(int func, int bar, u32 address, int dsize,
                           u32 data) {
  if (bar != BAR_BUSMASTER) {
    if (reg_trace())
      printf("CMDW bar%d+%x/%d = %x\n", bar, address, dsize, data);
    CIdeController::WriteMem_Bar(func, bar, address, dsize, data);
    return;
  }
  for (int i = 0; i < dsize / 8; ++i) {
    const u8 b = (u8)(data >> (8 * i));
    if (const u32 reg = bm_alias(address + i)) {
      if (trace_more())
        printf("CMDT BM write @%x = %02x\n", address + i, b);
      config_write(0, reg, 8, b);
    } else
      CIdeController::WriteMem_Bar(func, bar, address + i, 8, b);
  }
}
