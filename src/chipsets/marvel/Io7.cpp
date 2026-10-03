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
 * The IO7 (Io7.hpp).
 *
 * The interrupt protocol is the console PALcode's (SRM V7.3-1, decompressed
 * image), since no document describes it:
 *
 *  - its EI1 handler (0x398b4) reads the processor's RBOX_INTQ while <24>
 *    is set, keeps each IID in a ring in memory and writes the value back
 *    to RBOX_INTQ, then clears RBOX_INT<12> and <14> by writing them;
 *  - the IID it takes apart is Linux's (core_marvel.h, IO7_IID): int_num
 *    <8:0>, msi <13>, the IO7's PE <23:14>; it reads that IO7's PO7_SCRATCH
 *    <7:0> as the vector block (x 0x2800) and hands the operating system
 *    0x800 + (LSI << 4), or 0x1000 + (MSI << 4), plus that block;
 *  - when the interrupt is done (0x38bc4, as the IPL drops) it writes the
 *    IID to the EOI_DAT register of the port its <6:5> names, and if
 *    another IID is waiting in its ring it raises RBOX_INT<14> on itself
 *    through RBOX_IREQ.
 *
 * So an LSI is sent once when its line is asserted and its LSI_CTL enabled,
 * and again after its end of interrupt if the line is still asserted: a
 * level-sensitive interrupt as a message [inference].
 **/
#include "StdAfx.hpp"

#include "AlphaCPU.hpp"
#include "Ev7.hpp"
#include "Io7.hpp"
#include "Marvel.hpp"
#include "PciWindows.hpp"
#include "System.hpp"

using namespace io7;

namespace {

struct named_reg {
  u32 off;
  const char *name;
};

/// The console's table of a PCI-X port's registers (SRM V7.3-1 at
/// 0x3ad1c8), plus the AGP port's three AGP registers (0x3ad5c8) and
/// Linux's EOI_DAT.
const named_reg kPortRegs[] = {
    {0x0000, "POx_CTRL"},        {0x0040, "POx_CACHE_CTL"},
    {0x0080, "POx_TIMER"},       {0x00c0, "POx_IO_ADR_EXT"},
    {0x0100, "POx_MEM_ADR_EXT"}, {0x0140, "POx_XCAL_CTRL"},
    {0x0200, "POx_DM_SOURCE"},   {0x0240, "POx_DM_DEST"},
    {0x0280, "POx_DM_SIZE"},     {0x02c0, "POx_DM_STAT"},
    {0x0400, "AGP_CAP_ID"},      {0x0440, "AGP_STAT"},
    {0x0480, "AGP_CMD"},         {0x0500, "POx_MONCTL"},
    {0x0540, "POx_CTRA"},        {0x0580, "POx_CTRB"},
    {0x05c0, "POx_CTR56"},       {0x0600, "POx_SCRATCH"},
    {0x0640, "POx_XTRA_A"},      {0x0680, "POx_XTRA_TS"},
    {0x06c0, "POx_XTRA_Z"},      {0x0740, "POx_THRESHA"},
    {0x0780, "POx_THRESHB"},     {0x1000, "POx_WBASE0"},
    {0x1040, "POx_WBASE1"},      {0x1080, "POx_WBASE2"},
    {0x10c0, "POx_WBASE3"},      {0x1100, "POx_WMASK0"},
    {0x1140, "POx_WMASK1"},      {0x1180, "POx_WMASK2"},
    {0x11c0, "POx_WMASK3"},      {0x1200, "POx_TBASE0"},
    {0x1240, "POx_TBASE1"},      {0x1280, "POx_TBASE2"},
    {0x12c0, "POx_TBASE3"},      {0x1300, "POx_SG_TBIA"},
    {0x1340, "POx_MSI_WBASE"},   {0x2000, "POx_ERR_SUM"},
    {0x2040, "POx_FIRST_ERR"},   {0x2080, "POx_MSK_HEI"},
    {0x20c0, "POx_TLB_ERR"},     {0x2100, "POx_SPL_COMPLT"},
    {0x2140, "POx_TRANS_SUM"},   {0x2180, "POx_FRC_PCI_ERR"},
    {0x21c0, "POx_MULT_ERR"},    {0x2400, "EOI_DAT"},
    {0x4000, "POx_HP_MISC"},     {0x4040, "POx_HP_LED"},
    {0x4080, "POx_HP_INTR_IN"},  {0x40c0, "POx_HP_EVNT_MASK"},
    {0x4100, "POx_HP_DEV_CAP"},  {0x4180, "POx_HP_PWR"},
    {0x41c0, "POx_HP_CNTL"},     {0x4200, "POx_HP_EVNT"},
};

/// The console's table of port 7's registers (SRM V7.3-1 at 0x3ad960),
/// less the LSI_CTL array, and Linux's MSI_CTL.
const named_reg kPort7Regs[] = {
    {0x300000, "IO_ASIC_REV"},   {0x300040, "IO_SYS_REV"},
    {0x300080, "SER_CHAIN3"},    {0x3000c0, "PO7_RST1"},
    {0x300100, "PO7_RST2"},      {0x300140, "POx_RST0"},
    {0x300180, "POx_RST1"},      {0x3001c0, "POx_RST2"},
    {0x300200, "POx_RST3"},      {0x300240, "IO7_DWNH"},
    {0x300280, "IO7_MAF"},       {0x3002c0, "IO7_MAF_TO"},
    {0x300300, "IO7_ACR0"},      {0x300340, "IO7_ACR1"},
    {0x300380, "IO7_ACR2"},      {0x3003c0, "IO7_UPH"},
    {0x300400, "IO7_UPH_TO"},    {0x300440, "IO7_IREQ_OFF"},
    {0x300480, "IO7_INTA_OFF"},  {0x3004c0, "IO7_RTY"},
    {0x300500, "PO7_MONCTL"},    {0x300540, "PO7_CTRA"},
    {0x300580, "PO7_CTRB"},      {0x3005c0, "PO7_CTR56"},
    {0x300600, "PO7_SCRATCH"},   {0x300640, "PO7_XTRA_A"},
    {0x300680, "PO7_XTRA_TS"},   {0x3006c0, "PO7_XTRA_Z"},
    {0x300700, "PO7_PMASK"},     {0x300740, "PO7_THRESHA"},
    {0x300780, "PO7_THRESHB"},   {0x302000, "PO7_ERROR_SUM"},
    {0x302040, "PO7_BHOLE_MSK"}, {0x302080, "PO7_HEI_MSK"},
    {0x3020c0, "PO7_CRD_MSK"},   {0x302100, "PO7_UNCRR_SYM"},
    {0x302140, "PO7_CRRCT_SYM"}, {0x302180, "PO7_ERR_PKT0"},
    {0x3021c0, "PO7_ERR_PKT1"},  {0x302200, "PO7_UGBGE_SYM"},
    {0x313ec0, "HLT_CTL"},       {0x313f00, "HPI_CTL"},
    {0x313f40, "CRD_CTL"},       {0x313f80, "STV_CTL"},
    {0x313fc0, "HEI_CTL"},       {0x318000, "INT_PND0"},
    {0x318040, "INT_CLR0"},      {0x318080, "INT_EOI0"},
    {0x318800, "INT_PND1"},      {0x318840, "INT_CLR1"},
    {0x318880, "INT_EOI1"},      {0x319000, "INT_PND2"},
    {0x319040, "INT_CLR2"},      {0x319080, "INT_EOI2"},
    {0x319800, "INT_PND3"},      {0x319840, "INT_CLR3"},
    {0x319880, "INT_EOI3"},      {0x31b800, "MISC_PND"},
    {0x31b840, "MISC_CLR"},
};

bool known(u32 port, u32 off) {
  if (port < kPorts) {
    for (const named_reg &r : kPortRegs)
      if (r.off == off)
        return true;
    return false;
  }
  if (off >= LSI_CTL && off < LSI_CTL + kLsis * 0x40 && !(off & 0x3f))
    return true;
  if (off >= MSI_CTL && off < MSI_CTL + 16 * 0x40 && !(off & 0x3f))
    return true;
  for (const named_reg &r : kPort7Regs)
    if (r.off == off)
      return true;
  return false;
}

const char *reg_name(u32 port, u32 off, char *buf, size_t n) {
  const named_reg *t = port < kPorts ? kPortRegs : kPort7Regs;
  const size_t count = port < kPorts
                           ? sizeof(kPortRegs) / sizeof(kPortRegs[0])
                           : sizeof(kPort7Regs) / sizeof(kPort7Regs[0]);
  for (size_t i = 0; i < count; i++)
    if (t[i].off == off)
      return t[i].name;
  if (port >= kPorts && off >= LSI_CTL && off < LSI_CTL + kLsis * 0x40)
    snprintf(buf, n, "LSI_CTL%u", (off - LSI_CTL) / 0x40);
  else if (port >= kPorts && off >= MSI_CTL && off < MSI_CTL + 16 * 0x40)
    snprintf(buf, n, "MSI_CTL%u", (off - MSI_CTL) / 0x40);
  else
    snprintf(buf, n, "+%06x", off);
  return buf;
}

/// ALPHABOX_TRACE_IO7=1: every access to an IO7 register, named, with the
/// instruction that made it and its return address. The console reaches
/// them all through one pair of helpers, so an access repeats when the
/// register, direction and value do: the first three of each, then every
/// power of two, as the CSR trace does.
bool trace_on() {
  static const bool on = getenv("ALPHABOX_TRACE_IO7") != nullptr;
  return on;
}

void trace(u32 pid, u32 port, u32 off, bool write, u64 v) {
  static std::mutex m;
  static std::map<std::pair<u64, u64>, u64> seen;
  const u64 pc = t_running_cpu ? t_running_cpu->get_pc() : 0;
  const u64 ra = t_running_cpu ? t_running_cpu->get_r(26, true) : 0;
  u64 n;
  {
    std::lock_guard<std::mutex> g(m);
    if (seen.size() > 100000)
      seen.clear();
    n = ++seen[{((u64)pid << 40) ^ ((u64)port << 32) ^ ((u64)off << 1) ^
                    (write ? 1 : 0),
                v}];
  }
  // Writes of the interrupt controls are few and decide whether a device is
  // heard at all: every one is shown.
  const bool ctl =
      write && port == 7 && off >= LSI_CTL && off < MSI_CTL + 16 * 0x40;
  if (n > 3 && (n & (n - 1)) && !ctl)
    return;
  char buf[32];
  printf("%%MVL-T-IO7: pid %u port %u %s %-16s %s %016" PRIx64 "%s pc=%" PRIx64
         " ra=%" PRIx64 "\n",
         pid, port, write ? "write" : "read ", reg_name(port, off, buf, 32),
         write ? "=" : "->", v, n > 3 ? " (repeated)" : "", pc, ra);
}

/// The index into m_regs: ports 0-3, and port 7 at 4.
int slot_of(u32 port) { return port < kPorts ? (int)port : kPorts; }

} // namespace

CIo7::CIo7(CSystem *sys, CMarvel *marvel, u32 pid, u8 backplane_rev)
    : m_sys(sys), m_marvel(marvel), m_pid(pid),
      m_backplane_rev(backplane_rev & 0xf) {
  reset();
}

/**
 * The state the XSROM leaves the IO7 in [inference: nothing documents the
 * reset values; these are what the console and Linux test]:
 *
 *  - every port present: POx_CACHE_CTL reads 8, the test Linux makes
 *    (marvel_init_io7);
 *  - IO_ASIC_REV: the chip's pass; the console prints "IO7 pass" as <n> + 1
 *    and a real ES47 says pass 3 [inference from our runs: 0 printed pass 1];
 *  - IO_SYS_REV: <16> valid and <7:4> the I/O type, which the console's
 *    get_io_type (0x2ea0f0) returns and show config names from a table
 *    ("3.3V PCI-X I/O", "Embedded I/O", "X-Shelf I/O", "Std PCI-X I/O" for 2
 *    to 5 [inference from the table's layout]); the ES47's is the embedded
 *    I/O. <3:0> is the I/O backplane's revision, from the board row:
 *    show_core_system (0x2dcd3c) prints it as "Backplane rev";
 *  - POx_RST3 <8:6>: the AGP PLL range, 6 for 1x/4x (Linux
 *    marvel_agp_configure), which the real listing prints;
 *  - the error registers, the windows and the interrupt controls 0.
 */
void CIo7::reset() {
  std::lock_guard<std::mutex> g(m_lock);
  for (auto &r : m_regs)
    r.clear();
  for (int p = 0; p < kPorts; p++)
    m_regs[p][POx_CACHE_CTL] = 8;
  m_regs[kPorts][IO_ASIC_REV] = 0x12;
  m_regs[kPorts][IO_SYS_REV] =
      (U64(1) << 16) | (io_type() << 4) | m_backplane_rev;
  m_regs[kPorts][POx_RST + 3 * 0x40] = U64(6) << 6;
  // AGP_CAP_ID: the AGP capability, revision 2.0 in <23:16> as in a PCI
  // AGP capability header [inference: the console prints "AGP rev %d.%d"
  // from it, and a real ES47 says 2.0].
  m_regs[kAgpPort][0x0400] = U64(0x00200002);
  for (u32 i = 0; i < kLsis; i++) {
    m_level[i] = false;
    m_in_service[i] = false;
  }
  // Every slot powered and on its bus, as the XSROM leaves them: the
  // console powers the hot-plug slots of hoses 0 and 1 off and on again
  // itself, but takes the embedded devices on hose 2 as they are (it reads
  // their HP_PWR without writing it first) [inference].
  for (auto &hp : m_hp) {
    hp = HotPlug();
    hp.powered = hp.connected = (U64(1) << kHpSlots) - 1;
  }
}

u64 CIo7::reg(u32 port, u32 off) const {
  const auto &m = m_regs[slot_of(port)];
  auto it = m.find(off);
  return it == m.end() ? 0 : it->second;
}

void CIo7::set_reg(u32 port, u32 off, int dsize, u64 data) {
  u64 &r = m_regs[slot_of(port)][off];
  r = (dsize == 32) ? ((r & ~U64(0xffffffff)) | (data & U64(0xffffffff)))
                    : data;
}

u64 CIo7::read(u32 port, u32 off, int dsize, CSystemComponent *source) {
  const u64 a = ev7::io7_base(m_pid, port) | off;
  if (port < kPorts && off < kCsrOff) {
    // PCI space nothing answers: a configuration or I/O read master-aborts
    // and reads as all ones; memory reads as zero, as on the Tsunami.
    const bool cfg = off >= 0xFE000000 && off < 0xFF000000;
    m_sys->trace_unknown(cfg                 ? "IO7 PCI configuration"
                         : off >= 0xFF000000 ? "IO7 PCI I/O"
                                             : "IO7 PCI memory",
                         a, dsize, false, 0, source);
    if (off < 0xFE000000)
      return 0;
    return (dsize >= 64) ? ~U64(0) : ((U64(1) << dsize) - 1);
  }
  if ((port < kPorts || port == 7) && off >= kCsrOff) {
    const u32 r = off - kCsrOff;
    std::lock_guard<std::mutex> g(m_lock);
    if (!known(port, r))
      m_sys->trace_unknown("IO7 register", a, dsize, false, 0, source);
    u64 v = (port < kPorts && r >= HP_MISC && r <= HP_EVNT) ? hp_read(port, r)
                                                            : reg(port, r);
    if (dsize == 32)
      v &= 0xffffffff;
    if (trace_on())
      trace(m_pid, port, r, false, v);
    return v;
  }
  m_sys->trace_unknown("IO7 port", a, dsize, false, 0, source);
  return 0;
}

void CIo7::write(u32 port, u32 off, int dsize, u64 data,
                 CSystemComponent *source) {
  const u64 a = ev7::io7_base(m_pid, port) | off;
  if (port < kPorts && off < kCsrOff) {
    m_sys->trace_unknown(off >= 0xFE000000 && off < 0xFF000000
                             ? "IO7 PCI configuration"
                         : off >= 0xFF000000 ? "IO7 PCI I/O"
                                             : "IO7 PCI memory",
                         a, dsize, true, data, source);
    return;
  }
  if (!((port < kPorts || port == 7) && off >= kCsrOff)) {
    m_sys->trace_unknown("IO7 port", a, dsize, true, data, source);
    return;
  }
  const u32 r = off - kCsrOff;
  std::lock_guard<std::mutex> g(m_lock);
  if (!known(port, r))
    m_sys->trace_unknown("IO7 register", a, dsize, true, data, source);
  if (trace_on())
    trace(m_pid, port, r, true, data);
  if (port < kPorts) {
    if (r >= HP_MISC && r <= HP_EVNT && hp_write(port, r, data))
      return;
    switch (r) {
    case POx_ERR_SUM:
    case 0x20c0: // POx_TLB_ERR
    case 0x2100: // POx_SPL_COMPLT
    case 0x2140: // POx_TRANS_SUM
      // Error summaries: written with ones to clear (Linux
      // io7_clear_errors); nothing ever sets them.
      return;
    case POx_SG_TBIA:
      // The scatter-gather TLB: pci_phys reads the page tables every time,
      // so there is nothing to invalidate.
      set_reg(port, r, dsize, data);
      return;
    case EOI_DAT:
      set_reg(port, r, dsize, data);
      end_of_interrupt(data);
      return;
    }
    set_reg(port, r, dsize, data);
    return;
  }
  switch (r) {
  case IO_ASIC_REV:
  case IO_SYS_REV:
    return; // identity [read-only here]
  case PO7_ERROR_SUM:
  case 0x302100: // PO7_UNCRR_SYM
  case 0x302140: // PO7_CRRCT_SYM
    return;      // errors, cleared by writing ones; never set
  }
  set_reg(port, r, dsize, data);
  if (r >= LSI_CTL && r < LSI_CTL + kLsis * 0x40)
    deliver((r - LSI_CTL) / 0x40); // newly enabled while asserted
}

/**
 * DMA: the port's four windows, the Tsunami Pchip's scheme (Linux
 * io7_init_hose: WBASE <0> enable, <1> scatter-gather, <31:20> the base;
 * WMASK <31:20>; TBASE the translated base or the page table; PTEs of
 * (pa >> 12) | 1 for 8 KB pages, as on the Tsunami). Window 3's DAC bit
 * is not modelled: devices here address 32 bits. An address no window
 * takes stays on the hose.
 */
u64 CIo7::pci_phys(u32 port, u32 address) {
  u64 wbase[4], wmask[4], tbase[4];
  {
    std::lock_guard<std::mutex> g(m_lock);
    for (int j = 0; j < 4; j++) {
      wbase[j] = reg(port, POx_WBASE + j * 0x40);
      wmask[j] = reg(port, POx_WMASK + j * 0x40);
      tbase[j] = reg(port, POx_TBASE + j * 0x40);
    }
  }
  for (int j = 0; j < 4; j++) {
    if (!pci_window::hit(wbase[j], wmask[j], address))
      continue;
    if (!(wbase[j] & 2))
      return pci_window::direct(address, wmask[j], tbase[j]);
    const u64 pte = m_sys->ReadMem(
        pci_window::pte_address(address, wmask[j], tbase[j]), 64, nullptr);
    if (pte & 1)
      return ((pte << pci_window::PTE_SHIFT) & pci_window::PTE_MASK) |
             (address & pci_window::PTE_ADD2_MASK);
    break; // an invalid entry: not translated
  }
  return ev7::io7_base(m_pid, port) | address;
}

/**
 * The hot-plug controller, as the console drives it (php_disconnect_all,
 * php_pwr_on_all, php_connect_all and the slot routines around 0x2e5490-
 * 0x2e5e80; no document describes it). Per port, slots 1-6 are the console's
 * 0-5, bit s of each field:
 *
 *  - HP_PWR: a write's <13:8> powers slots on and <5:0> powers them off
 *    [inference: the console writes 0x3f00 to power all on]; a read gives
 *    the slots powered in <5:0>, <31> set when the state is invalid;
 *  - HP_CNTL: the same for connecting slots to the bus (0x3f00 connect all,
 *    0x3f disconnect all) [inference];
 *  - HP_MISC: completion events, written with ones to clear: <5:0> a power
 *    change done (the console waits for them after HP_PWR), <21:16> a
 *    connection change done (after HP_CNTL);
 *  - HP_INTR_IN: the slot inputs: <s> the interlock switch open, <8+s> a
 *    power fault, <16+s> and <24+s> both set for an empty slot (the
 *    console powers a slot only when not both are) [inference: PRSNT1# and
 *    PRSNT2#, high with no card];
 *  - HP_DEV_CAP: what each slot supports: <s> 66 MHz, <8+s> PCI-X, <16+s>
 *    PCI-X 133 [inference from the console's "%s MHz PCI-X" strings];
 *  - HP_EVNT: events (presence, user request), written with ones to clear;
 *    none happen here. HP_LED and HP_EVNT_MASK hold what is written.
 */
u64 CIo7::hp_read(u32 port, u32 r) {
  const HotPlug &hp = m_hp[port];
  switch (r) {
  case HP_MISC:
    return hp.misc;
  case HP_PWR:
    return hp.powered;
  case HP_INTR_IN: {
    u64 v = 0;
    for (u32 s = 0; s < kHpSlots; s++)
      if (!slot_occupied(port, s + 1))
        v |= (U64(1) << (16 + s)) | (U64(1) << (24 + s));
    return v;
  }
  case HP_DEV_CAP:
    // The ES47's hose 0 is its 66 MHz PCI-X slot, the others 33 MHz PCI
    // [the slots' PCI-X capabilities are not known].
    return port == 0 ? 0x3f : 0;
  case HP_EVNT:
    return 0;
  }
  return reg(port, r);
}

/// With m_lock held; false when the register simply holds what is written.
bool CIo7::hp_write(u32 port, u32 r, u64 data) {
  HotPlug &hp = m_hp[port];
  const u64 on = (data >> 8) & 0x3f, off = data & 0x3f;
  switch (r) {
  case HP_MISC:
    hp.misc &= ~data;
    return true;
  case HP_PWR:
    hp.powered = (hp.powered | on) & ~off;
    hp.misc |= on | off;
    set_reg(port, r, 64, data);
    return true;
  case HP_CNTL:
    hp.connected = (hp.connected | on) & ~off;
    hp.misc |= (on | off) << 16;
    set_reg(port, r, 64, data);
    return true;
  case HP_EVNT:
  case HP_INTR_IN:
  case HP_DEV_CAP:
    return true;
  }
  return false;
}

/// A card in a slot is a device answering configuration space there.
bool CIo7::slot_occupied(u32 port, u32 dev) const {
  return m_sys->device_at(ev7::io7_base(m_pid, port) + 0xFE000000 +
                          (u64)dev * 0x800);
}

void CIo7::lsi(u32 n, bool assert) {
  if (n >= kLsis)
    return;
  std::lock_guard<std::mutex> g(m_lock);
  m_level[n] = assert;
  if (assert)
    deliver(n);
}

/// With m_lock held.
void CIo7::deliver(u32 n) {
  if (!m_level[n] || m_in_service[n])
    return;
  const u64 ctl = reg(7, LSI_CTL + n * 0x40);
  if (!(ctl & CTL_ENABLE))
    return;
  CEv7Csr *target = m_marvel->csr(ctl_target(ctl));
  if (!target)
    return;
  m_in_service[n] = true;
  // The IID: int_num <8:0>, msi <13> clear, this IO7's PE <23:14>.
  target->post_iid(((u64)m_pid << 14) | n);
}

/// With m_lock held.
void CIo7::end_of_interrupt(u64 iid) {
  if (iid & (U64(1) << 13))
    return; // an MSI [not modelled: no device here sends one]
  const u32 n = (u32)iid & 0xff;
  if (n >= kLsis)
    return;
  m_in_service[n] = false;
  deliver(n);
}

void CIo7::save_state(FILE *f) {
  std::lock_guard<std::mutex> g(m_lock);
  for (const auto &m : m_regs) {
    const u32 n = (u32)m.size();
    fwrite(&n, sizeof(n), 1, f);
    for (const auto &kv : m) {
      fwrite(&kv.first, sizeof(kv.first), 1, f);
      fwrite(&kv.second, sizeof(kv.second), 1, f);
    }
  }
  fwrite(m_level, sizeof(m_level), 1, f);
  fwrite(m_in_service, sizeof(m_in_service), 1, f);
  fwrite(m_hp, sizeof(m_hp), 1, f);
}

bool CIo7::restore_state(FILE *f) {
  std::lock_guard<std::mutex> g(m_lock);
  for (auto &m : m_regs) {
    u32 n = 0;
    if (fread(&n, sizeof(n), 1, f) != 1)
      return false;
    m.clear();
    for (u32 i = 0; i < n; i++) {
      u32 off;
      u64 v;
      if (fread(&off, sizeof(off), 1, f) != 1 ||
          fread(&v, sizeof(v), 1, f) != 1)
        return false;
      m[off] = v;
    }
  }
  return fread(m_level, sizeof(m_level), 1, f) == 1 &&
         fread(m_in_service, sizeof(m_in_service), 1, f) == 1 &&
         fread(m_hp, sizeof(m_hp), 1, f) == 1;
}
