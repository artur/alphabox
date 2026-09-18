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
 * Transparent PCI-PCI bridge: the type 1 header, bus numbering, and what
 * the forwarding windows let through to the devices on the secondary bus.
 **/
#include "PCIBridge.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

// Type 1 header offsets
#define PPB_BUS_NUMBERS 0x18 // primary, secondary, subordinate, sec. latency
#define PPB_SECONDARY 0x19
#define PPB_IO 0x1c       // I/O base, limit; secondary status
#define PPB_MEMORY 0x20   // memory base, limit
#define PPB_PREFETCH 0x24 // prefetchable base, limit
#define PPB_PREFETCH_BASE_UPPER 0x28
#define PPB_PREFETCH_LIMIT_UPPER 0x2c
#define PPB_IO_UPPER 0x30 // I/O base and limit, upper 16 bits
#define PPB_CAPABILITIES 0x34
#define PPB_INTERRUPT 0x3c // line, pin; bridge control
#define PPB_BRIDGE_CONTROL 0x3e
#define PPB_CHIP_REGS 0x40 // chip, diagnostic and arbiter control, ...
#define PPB_CHIP_REGS_END 0x70
#define PPB_PM_CAP 0xdc

#define BRIDGE_CONTROL_ISA 0x0004
#define BRIDGE_CONTROL_VGA 0x0008
#define BRIDGE_CONTROL_SECONDARY_RESET 0x0040

#define COMMAND_IO 0x0001
#define COMMAND_MEMORY 0x0002

// The windows the bridge claims on the primary side. Their range indices
// start well above anything CPCIDevice gives a BAR or a config-space range,
// so ReadMem/WriteMem can tell them apart by index alone.
#define PPB_WINDOW_BASE 0x1000
enum {
  PPB_WIN_IO,
  PPB_WIN_MEMORY,
  PPB_WIN_PREFETCH,
  PPB_WIN_VGA_MEMORY,
  PPB_WIN_VGA_IO_MONO,
  PPB_WIN_VGA_IO_COLOR,
  PPB_WIN_COUNT
};

CPCIBridge::CPCIBridge(CConfigurator *cfg, CSystem *c, int pcibus, int pcidev,
                       const pci_bridge_config &chip)
    : CPCIDevice(cfg, c, pcibus, pcidev), m_chip(chip),
      m_windows(PPB_WIN_COUNT) {}

CPCIBridge::~CPCIBridge() {}

void CPCIBridge::init() {
  u32 data[64] = {};
  u32 mask[64] = {};

  data[0x00 >> 2] = u32(m_chip.device_id) << 16 | m_chip.vendor_id;
  data[0x04 >> 2] = 0x02900000; // medium DEVSEL, fast back-to-back
  data[0x08 >> 2] = 0x06040000 | m_chip.revision; // PCI-PCI bridge
  data[0x0c >> 2] = 0x00010000;                   // type 1 header
  mask[0x04 >> 2] = 0x000003ff;
  mask[0x0c >> 2] = 0x0000ffff;
  mask[PPB_BUS_NUMBERS >> 2] = 0xffffffff;
  data[PPB_IO >> 2] = m_chip.io32 ? 0x00000101 : 0;
  mask[PPB_IO >> 2] = 0x0000f0f0;
  mask[PPB_MEMORY >> 2] = 0xfff0fff0;
  data[PPB_PREFETCH >> 2] = m_chip.pref64 ? 0x00010001 : 0;
  mask[PPB_PREFETCH >> 2] = 0xfff0fff0;
  if (m_chip.pref64) {
    mask[PPB_PREFETCH_BASE_UPPER >> 2] = 0xffffffff;
    mask[PPB_PREFETCH_LIMIT_UPPER >> 2] = 0xffffffff;
  }
  if (m_chip.io32)
    mask[PPB_IO_UPPER >> 2] = 0xffffffff;
  mask[PPB_INTERRUPT >> 2] = 0x0fff00ff; // line; bridge control
  for (int r = PPB_CHIP_REGS; r < PPB_CHIP_REGS_END; r += 4)
    mask[r >> 2] = 0xffffffff;
  if (m_chip.pm_capabilities) {
    data[0x04 >> 2] |= 0x00100000; // capability list
    data[PPB_CAPABILITIES >> 2] = PPB_PM_CAP;
    data[PPB_PM_CAP >> 2] = u32(m_chip.pm_capabilities) << 16 | 0x0001;
    mask[(PPB_PM_CAP + 4) >> 2] = 0x00000103;
  }
  add_function(0, data, mask);

  ResetPCI();

  if (m_chip.board)
    printf("%s: %s (%s, %d ports)\n", devid_string, m_chip.board, m_chip.part,
           m_chip.ports);
  else
    printf("%s: %s PCI-PCI bridge\n", devid_string, m_chip.part);
}

u32 CPCIBridge::cfg32(u32 address) const {
  return endian_32(pci_state.config_data[0][address >> 2]);
}

int CPCIBridge::secondary_bus() const {
  return (cfg32(PPB_BUS_NUMBERS) >> 8) & 0xff;
}

void CPCIBridge::attach(CPCIDevice *child) { m_children.push_back(child); }

void CPCIBridge::remap_children() {
  for (CPCIDevice *child : m_children)
    child->map_config_space();
}

void CPCIBridge::map_child_range(CPCIDevice *child, int index, bool is_io,
                                 u32 base, u64 length) {
  for (child_range &r : m_ranges) {
    if (r.dev == child && r.index == index) {
      r.io = is_io;
      r.base = base;
      r.length = length;
      return;
    }
  }
  m_ranges.push_back({child, index, is_io, base, length});
}

/**
 * Which device behind the bridge answers for `address`, if any. A bridge
 * claims its whole window whether or not something behind it decodes the
 * address; when nothing does, the access ends in a master abort.
 **/
const CPCIBridge::child_range *CPCIBridge::decode(int w, u32 address) const {
  const bool io = m_windows[w].io;

  // The ISA bit leaves the top 768 bytes of every kilobyte of the low 64 KB
  // of I/O space to the ISA bridge on the primary side, so that a card
  // behind this bridge can have the 256 ports of an ISA alias without
  // taking the legacy devices with them. Here the ISA bridge's own ranges
  // are registered before any window is, and the address never reaches us.
  if (io && w == PPB_WIN_IO &&
      (cfg32(PPB_INTERRUPT) >> 16) & BRIDGE_CONTROL_ISA && address < 0x10000 &&
      (address & 0x300))
    return nullptr;

  for (const child_range &r : m_ranges)
    if (r.io == io && r.length && address >= r.base &&
        address - r.base < r.length)
      return &r;
  return nullptr;
}

/**
 * Place (or, with a length of 0, withdraw) one window. A window is a range
 * like any other, so a bridge behind a bridge hands its windows to its
 * parent and is forwarded to in turn.
 **/
void CPCIBridge::set_window(int w, bool io, u32 base, u64 length,
                            const char *what) {
  window &win = m_windows[w];
  const bool changed = win.base != base || win.length != length;

  win.io = io;
  win.base = base;
  win.length = length;
  if (!length && !win.placed)
    return; // never claimed, still closed: nothing to tell anyone

  if (changed) {
    if (length)
      printf("%s: %s window %08x-%08" PRIx64 "\n", devid_string, what, base,
             base + length - 1);
    else
      printf("%s: %s window closed\n", devid_string, what);
  }
  win.placed = true;
  map_range(PPB_WINDOW_BASE + w, io, base, length);
}

/**
 * Work out the windows from the type 1 header. A window forwards when its
 * base is at or below its limit -- firmware closes one by writing a base
 * larger than the limit -- and when the matching space-enable bit in the
 * command register lets the bridge respond on the primary side at all,
 * which is what keeps a bridge that nobody has configured from swallowing
 * the low megabyte its reset values nominally describe.
 **/
void CPCIBridge::remap_windows() {
  const u32 command = cfg32(0x04);
  const u16 control = (u16)(cfg32(PPB_INTERRUPT) >> 16);
  const u32 io = cfg32(PPB_IO);
  const u32 io_upper = m_chip.io32 ? cfg32(PPB_IO_UPPER) : 0;
  const u32 memory = cfg32(PPB_MEMORY);
  const u32 prefetch = cfg32(PPB_PREFETCH);

  u32 base = ((io & 0x000000f0) << 8) | ((io_upper & 0x0000ffff) << 16);
  u32 limit = (io & 0x0000f000) | (io_upper & 0xffff0000) | 0xfff;
  bool on = (command & COMMAND_IO) && base <= limit;
  set_window(PPB_WIN_IO, true, on ? base : 0, on ? u64(limit) - base + 1 : 0,
             "I/O");

  base = memory << 16 & 0xfff00000;
  limit = (memory & 0xfff00000) | 0xfffff;
  on = (command & COMMAND_MEMORY) && base <= limit;
  set_window(PPB_WIN_MEMORY, false, on ? base : 0,
             on ? u64(limit) - base + 1 : 0, "memory");

  // A 64-bit prefetchable window that starts above 4 GB is out of reach of
  // this hose's 32-bit address space, so nothing of it is forwarded; one
  // that merely ends above 4 GB reaches as far as the space does.
  const u32 prefetch_base_upper =
      m_chip.pref64 ? cfg32(PPB_PREFETCH_BASE_UPPER) : 0;
  const u32 prefetch_limit_upper =
      m_chip.pref64 ? cfg32(PPB_PREFETCH_LIMIT_UPPER) : 0;
  base = prefetch << 16 & 0xfff00000;
  limit =
      prefetch_limit_upper ? 0xffffffff : ((prefetch & 0xfff00000) | 0xfffff);
  on = (command & COMMAND_MEMORY) && !prefetch_base_upper && base <= limit;
  set_window(PPB_WIN_PREFETCH, false, on ? base : 0,
             on ? u64(limit) - base + 1 : 0, "prefetchable");

  // The VGA bit forwards the graphics addresses whatever the windows say,
  // so that a VGA card can live behind a bridge while its registers stay
  // where every driver looks for them. The aliases of the I/O ranges (the
  // address bits above bit 9 are don't-cares on a real bridge) are not
  // claimed: nothing here has ever gone looking for them.
  const bool vga = (control & BRIDGE_CONTROL_VGA) != 0;
  set_window(PPB_WIN_VGA_MEMORY, false, 0xa0000,
             (vga && (command & COMMAND_MEMORY)) ? 0x20000 : 0, "VGA memory");
  set_window(PPB_WIN_VGA_IO_MONO, true, 0x3b0,
             (vga && (command & COMMAND_IO)) ? 0xc : 0, "VGA monochrome I/O");
  set_window(PPB_WIN_VGA_IO_COLOR, true, 0x3c0,
             (vga && (command & COMMAND_IO)) ? 0x20 : 0, "VGA colour I/O");
}

/**
 * Bus reset: the bus numbers clear, so everything behind the bridge drops
 * out of configuration space until the firmware numbers it again, and the
 * windows close with the command register.
 **/
void CPCIBridge::ResetPCI() {
  CPCIDevice::ResetPCI();
  remap_children();
  remap_windows();
}

/**
 * An access the hose has handed to one of our windows. Whatever is behind
 * the bridge sees the same address -- a transparent bridge changes none of
 * it -- so the window's own base turns the offset back into one.
 **/
u64 CPCIBridge::ReadMem(int index, u64 address, int dsize) {
  if (index < PPB_WINDOW_BASE)
    return CPCIDevice::ReadMem(index, address, dsize);

  const int w = index - PPB_WINDOW_BASE;
  const u32 addr = m_windows[w].base + (u32)address;
  const child_range *r = decode(w, addr);
  if (!r) {
    cSystem->trace_unknown("behind a PCI-PCI bridge",
                           bus_address(m_windows[w].io, addr), dsize, false, 0,
                           this);
    return 0; // master abort, answered as the hose answers for itself
  }
  return r->dev->ReadMem(r->index, addr - r->base, dsize);
}

void CPCIBridge::WriteMem(int index, u64 address, int dsize, u64 data) {
  if (index < PPB_WINDOW_BASE) {
    CPCIDevice::WriteMem(index, address, dsize, data);
    return;
  }

  const int w = index - PPB_WINDOW_BASE;
  const u32 addr = m_windows[w].base + (u32)address;
  const child_range *r = decode(w, addr);
  if (!r) {
    cSystem->trace_unknown("behind a PCI-PCI bridge",
                           bus_address(m_windows[w].io, addr), dsize, true,
                           data, this);
    return;
  }
  r->dev->WriteMem(r->index, addr - r->base, dsize, data);
}

void CPCIBridge::config_write_custom(int func, u32 address, int dsize,
                                     u32 old_data, u32 new_data, u32 data) {
  const u32 end = address + dsize / 8;

  if (address <= PPB_SECONDARY && PPB_SECONDARY < end) {
    if (secondary_bus())
      printf("%s: secondary bus is now %d\n", devid_string, secondary_bus());
    remap_children();
  }

  // Asserting the secondary reset resets every device behind the bridge.
  if (address <= PPB_BRIDGE_CONTROL && PPB_BRIDGE_CONTROL < end) {
    const int shift = 8 * (PPB_BRIDGE_CONTROL - address);
    const u32 was = old_data >> shift;
    const u32 now = new_data >> shift;
    if ((now & BRIDGE_CONTROL_SECONDARY_RESET) &&
        !(was & BRIDGE_CONTROL_SECONDARY_RESET))
      for (CPCIDevice *child : m_children)
        child->ResetPCI();
  }

  // Command register, windows, bridge control: firmware and guests move
  // them about, so work out where the bridge now answers after every write
  // rather than guess which of them mattered.
  remap_windows();
}

int CPCIBridge::RestoreState(FILE *f) {
  int res = CPCIDevice::RestoreState(f);
  if (!res) {
    remap_children();
    remap_windows();
  }
  return res;
}
