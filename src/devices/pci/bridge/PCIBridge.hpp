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

/* Transparent PCI-PCI bridges: the DECchip 21050/21052/21152/21153/21154
 * family (and Intel's 21154), alone or as the heart of a multi-port board.
 *
 *   PCIBridge.cpp       the type 1 header, bus numbering, the forwarding
 *                       windows, the secondary bus and its devices, state
 *                       file
 *   PCIBridgeChips.cpp  the parts, and the boards built on them
 *
 * Devices behind a bridge are declared inside its block as pci.<device>
 * (device 0-15: the secondary IDSEL lines). They are reached through the
 * Tsunami's type 1 configuration space at the bus number the firmware
 * gives the bridge, and their INTx pins rotate onto the bridge's slot.
 *
 * The bridge decodes addresses. Its I/O, memory and prefetchable-memory
 * windows are what it claims on the hose, and an access inside one is
 * handed to whichever device behind the bridge has a range there; nothing
 * behind a bridge is reachable on the hose by itself. The bridge-control
 * register's ISA bit keeps the top 768 bytes of every 1 KB of the low 64 KB
 * of I/O space on the primary side, and its VGA bit forwards the VGA
 * addresses whatever the windows say.
 *
 * Documentation consulted:
 *  - PCI-to-PCI Bridge Architecture Specification, revision 1.1
 *  - DIGITAL Semiconductor 21152 / 21154 PCI-to-PCI Bridge data sheets
 *  - the ES40 SRM console: its PCI table ("DECchip 21152-AA", ...) and
 *    "probing PCI-to-PCI bridge, hose %d bus %d"
 */
#if !defined(INCLUDED_PCIBRIDGE_H_)
#define INCLUDED_PCIBRIDGE_H_

#include "PCIDevice.hpp"

#include <vector>

/**
 * \brief What distinguishes one bridge part or bridge-based board.
 **/
struct pci_bridge_config {
  const char *name;    ///< config class, e.g. "dec21152" or "de602"
  const char *part;    ///< the bridge chip, e.g. "DECchip 21152"
  u16 vendor_id;       ///< PCI config 0x00
  u16 device_id;       ///< PCI config 0x02
  u8 revision;         ///< PCI config 0x08
  bool io32;           ///< 32-bit I/O forwarding (21152 and later)
  bool pref64;         ///< 64-bit prefetchable window (21154)
  u16 pm_capabilities; ///< PCI power management PMC (0: none)
  /// Boards: the class of the devices on the secondary bus (at devices
  /// 0..ports-1), synthesized when the configuration leaves them out.
  const char *port_class;
  int ports;
  const char *board; ///< board name for messages, e.g. "DE602-AA"
};

/**
 * \brief Emulated transparent PCI-PCI bridge.
 **/
class CPCIBridge : public CPCIDevice {
public:
  CPCIBridge(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev,
             const pci_bridge_config &chip);
  virtual ~CPCIBridge();

  virtual void init();
  virtual void ResetPCI();
  virtual int RestoreState(FILE *f);
  virtual void config_write_custom(int func, u32 address, int dsize,
                                   u32 old_data, u32 new_data, u32 data);
  virtual u64 ReadMem(int index, u64 address, int dsize);
  virtual void WriteMem(int index, u64 address, int dsize, u64 data);

  /// The secondary bus number (0 until the firmware numbers the bus).
  int secondary_bus() const;

  /// A device on the secondary bus announces itself.
  void attach(CPCIDevice *child);

  /// A device on the secondary bus places one of its ranges there, or
  /// withdraws it with a length of 0. Only what a window covers is reached.
  void map_child_range(CPCIDevice *child, int index, bool is_io, u32 base,
                       u64 length);

  /// The part or board named `name`, or nullptr.
  static const pci_bridge_config *find_chip(const char *name);

private:
  /// One range a device behind the bridge answers on the secondary bus.
  struct child_range {
    CPCIDevice *dev;
    int index; ///< the device's own name for it, as ReadMem/WriteMem take it
    bool io;
    u32 base;
    u64 length;
  };

  /// One address range the bridge claims on the primary side.
  struct window {
    bool io = false;
    u32 base = 0;
    u64 length = 0;
    bool placed = false; ///< whether the range exists upstream yet
  };

  void remap_children();
  void remap_windows();
  void set_window(int w, bool io, u32 base, u64 length, const char *what);
  u32 cfg32(u32 address) const;
  const child_range *decode(int window, u32 address) const;

  const pci_bridge_config m_chip;
  std::vector<CPCIDevice *> m_children;
  std::vector<child_range> m_ranges;
  std::vector<window> m_windows;
};

#endif // !defined(INCLUDED_PCIBRIDGE_H_)
