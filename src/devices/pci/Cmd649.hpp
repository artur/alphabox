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
 * The CMD (Silicon Image) PCI0649 PCI-IDE controller.
 **/
#if !defined(INCLUDED_CMD649_H_)
#define INCLUDED_CMD649_H_

#include "IdeController.hpp"

/**
 * \brief The CMD 649 UltraDMA/100 PCI-IDE controller: the ATA/ATAPI core
 * (CIdeController) as a PCI card or on-board part, both channels native.
 *
 * PCI 1095:0649, class 0101 with programming interface 8F (both channels
 * native, the mode bits read-only), one function on INTA. BARs 0-3 are the
 * channels' command and control blocks, BAR 4 the bus master block, in
 * I/O space; there are no legacy ports.
 *
 * What the CMD parts add to SFF-8038i is an interrupt status per channel
 * that a driver tests to find which channel interrupted, latched when the
 * channel raises its interrupt and cleared by writing it back with the bit
 * set: CFR (0x50) bit 2 for the primary channel, ARTTIM23 (0x57) bit 4 for
 * the secondary, and both in MRDMODE (0x71, also bus master byte 1) bits 2
 * and 3. MRDMODE bits 4 and 5 keep a channel's interrupt off INTA. The
 * timing registers (0x51-0x5b, UDIDETCR0/1 at 0x73/0x7b, bus master bytes
 * 3 and 11) hold what is written to them and do nothing else: a transfer
 * here takes no time whatever the mode.
 *
 * `disk<channel>.<drive>` as on the ALi; a CD-ROM is an ATAPI device.
 *
 * Documentation consulted: the register layout the Linux cmd64x driver
 * (drivers/ide/cmd64x.c, include/linux/... cmd64x definitions) programs,
 * and the CMD 646/648/649 programming conventions it records; SFF-8038i
 * for the bus master registers. No CMD data sheet was at hand.
 **/
class CCmd649 : public CIdeController {
public:
  CCmd649(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);

  u32 ReadMem_Bar(int func, int bar, u32 address, int dsize) override;
  void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                    u32 data) override;
  u32 config_read_custom(int func, u32 address, int dsize, u32 data) override;
  void config_write_custom(int func, u32 address, int dsize, u32 old_data,
                           u32 new_data, u32 raw) override;
  void ResetPCI() override;

protected:
  void add_functions() override;
  void irq_raise(int channel) override;
  void irq_lower(int channel) override;

private:
  u8 cfg8(u32 offset) const;
  void set_cfg8(u32 offset, u8 value);
  /// The latched interrupt status of a channel (CFR / ARTTIM23).
  bool intr_latched(int channel) const;
  void set_intr_latched(int channel, bool on);
  /// INTA: a channel whose interrupt is pending and not blocked in MRDMODE.
  void update_line();
  /// Bus master bytes 1, 3, 9 and 11: the configuration registers they
  /// mirror, or 0 for the SFF-8038i bytes.
  static u32 bm_alias(u32 address);
  std::mutex m_line_mx;
  bool m_line = false;
};

#endif
