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
 * The IO7: the bridge on an EV7's I/O port, with four PCI ports -- PCI or
 * PCI-X ports 0 to 2 and the AGP port 3 -- and its own registers on port 7.
 * Hose PID * 4 + port is port `port` of the IO7 on processor PID.
 *
 * Its space, for the IO7 on PE `pid` (Linux core_marvel.h, IO7_HOSE):
 *
 *   ipe(pid) | (~port & 7) << 32 | off
 *     off < 0xFE000000            PCI memory (dense)
 *     0xFE000000 - 0xFEFFFFFF     PCI configuration, bus << 16 | devfn << 8
 *     0xFF000000 - 0xFF7FFFFF     PCI I/O (dense)
 *     0xFF800000 + reg            the port's registers (64 bytes apart)
 *   port 7: 0xFF800000 + 0x300000 + reg, the IO7's own registers
 *
 * What a device registers in those spaces (CPCIDevice) is served by the
 * system's device ranges; what reaches here is the registers and the space
 * nobody claimed, which master-aborts.
 *
 * Modelled: the port and port-7 registers of the console's own tables
 * (SRM V7.3-1; lab/docs-ev7/srm73-csr-tables.txt), holding what is written,
 * with the identity and the presence bits a configured ES47 reads; the four
 * DMA windows per port (chipsets/PciWindows.hpp: the IO7's are the
 * Pchip's); and level-sensitive interrupts (LSIs), which go to the EV7 that
 * LSI_CTL names as IIDs and are re-sent after the PALcode's end-of-interrupt
 * write while the line is still asserted. The error registers read clean.
 * docs/platforms/marvel.md, "M5", has where each piece was found.
 **/
#if !defined(INCLUDED_IO7_H_)
#define INCLUDED_IO7_H_

#include "StdAfx.hpp"

#include <cstdio>
#include <map>
#include <mutex>

class CMarvel;
class CSystem;
class CSystemComponent;

namespace io7 {
constexpr int kPorts = 4;           ///< PCI-X 0-2 and AGP 3
constexpr int kAgpPort = 3;         ///< the AGP port
constexpr u32 kLsis = 128;          ///< LSI_CTL[128]
constexpr u32 kCsrOff = 0xFF800000; ///< a port's registers in its space

// Port registers (64 bytes apart).
constexpr u32 POx_CTRL = 0x0000;
constexpr u32 POx_CACHE_CTL = 0x0040;
constexpr u32 POx_WBASE = 0x1000; ///< [4]
constexpr u32 POx_WMASK = 0x1100; ///< [4]
constexpr u32 POx_TBASE = 0x1200; ///< [4]
constexpr u32 POx_SG_TBIA = 0x1300;
constexpr u32 POx_ERR_SUM = 0x2000;
constexpr u32 EOI_DAT = 0x2400; ///< Linux io7_ioport_csrs; not in the table
// The hot-plug controller of a PCI-X port (CIo7::hp_read).
constexpr u32 HP_MISC = 0x4000;
constexpr u32 HP_LED = 0x4040;
constexpr u32 HP_INTR_IN = 0x4080;
constexpr u32 HP_EVNT_MASK = 0x40c0;
constexpr u32 HP_DEV_CAP = 0x4100;
constexpr u32 HP_PWR = 0x4180;
constexpr u32 HP_CNTL = 0x41c0;
constexpr u32 HP_EVNT = 0x4200;
constexpr u32 kHpSlots = 6; ///< slots 1-6 (the console's 0-5)

// Port 7 registers (from kPort7).
constexpr u32 IO_ASIC_REV = 0x300000;
constexpr u32 IO_SYS_REV = 0x300040;
constexpr u32 POx_RST = 0x300140; ///< [4]
constexpr u32 PO7_SCRATCH = 0x300600;
constexpr u32 PO7_ERROR_SUM = 0x302000;
constexpr u32 LSI_CTL = 0x310000; ///< [128]
constexpr u32 MSI_CTL = 0x314000; ///< [16]

/// LSI_CTL/MSI_CTL: <24> enable, <22:14> the PID the interrupt goes to.
constexpr u64 CTL_ENABLE = U64(1) << 24;
constexpr u32 ctl_target(u64 ctl) { return (u32)(ctl >> 14) & 0x1ff; }

/// An LSI's number: port <7:5>, slot <4:2>, INTx <1:0> (core_marvel.h).
constexpr u32 lsi(u32 port, u32 slot, u32 intx) {
  return (port & 7) << 5 | (slot & 7) << 2 | (intx & 3);
}

/// IO_SYS_REV<7:4>, the I/O type, which show config names from a table at
/// 0x3ac378: 0 "3.3V PCI-X I/O" (the GS1280's standard I/O drawer, as a
/// real one lists it), 1 "Embedded I/O" (the 2P drawer's backplane, a real
/// ES47), 2 "X-Shelf I/O", 3 "Std PCI-X I/O".
constexpr u32 kIoTypeStdDrawer = 0;
constexpr u32 kIoTypeEmbedded = 1;
} // namespace io7

class CIo7 {
public:
  /// `backplane_rev`: the I/O backplane's revision, IO_SYS_REV<3:0>;
  /// `io_type`: what the IO7 sits in, IO_SYS_REV<7:4> (io7::kIoType*).
  CIo7(CSystem *sys, CMarvel *marvel, u32 pid, u8 backplane_rev, u8 io_type);

  u32 pid() const { return m_pid; }

  /// The I/O type IO_SYS_REV reports (io7::kIoType*), from the board row.
  u32 io_type() const { return m_io_type; }

  /// An access to the IO7's space that no device range claimed: `port` is
  /// 0-3 or 7, `off` the offset in that port's 4 GB.
  u64 read(u32 port, u32 off, int dsize, CSystemComponent *source);
  void write(u32 port, u32 off, int dsize, u64 data, CSystemComponent *source);

  /// A device's address as bus master on `port`, through the port's DMA
  /// windows, as a physical address.
  u64 pci_phys(u32 port, u32 address);

  /// A PCI interrupt line changes level: LSI `n` (io7::lsi).
  void lsi(u32 n, bool assert);

  void reset();
  void save_state(FILE *f);
  bool restore_state(FILE *f);

private:
  u64 reg(u32 port, u32 off) const;
  void set_reg(u32 port, u32 off, int dsize, u64 data);
  /// Send LSI n to its target if it is asserted, enabled and not in service.
  void deliver(u32 n);
  /// The PALcode's end of interrupt for `iid` (EOI_DAT).
  void end_of_interrupt(u64 iid);
  /// The hot-plug registers (HP_MISC .. HP_EVNT) of port `port`.
  u64 hp_read(u32 port, u32 r);
  bool hp_write(u32 port, u32 r, u64 data);
  /// Whether a card sits in slot (device) `dev` of bus 0 on `port`.
  bool slot_occupied(u32 port, u32 dev) const;

  CSystem *m_sys;
  CMarvel *m_marvel;
  u32 m_pid;
  u8 m_backplane_rev;
  u8 m_io_type;
  std::mutex m_lock;
  /// Every register's contents, ports 0-3 and port 7 (index 4).
  std::map<u32, u64> m_regs[io7::kPorts + 1];
  bool m_level[io7::kLsis] = {}; ///< the device lines
  /// Each PCI-X port's hot-plug state: slots powered and connected to the
  /// bus, and the completion events not yet cleared.
  struct HotPlug {
    u64 powered = 0;
    u64 connected = 0;
    u64 misc = 0; ///< HP_MISC's event bits
  } m_hp[io7::kPorts];
  bool m_in_service[io7::kLsis] = {}; ///< sent, no end of interrupt yet
};

#endif // !defined(INCLUDED_IO7_H_)
