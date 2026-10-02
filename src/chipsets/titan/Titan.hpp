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
 * The 21274 Titan chipset, as the AlphaServer ES45, DS25 and DS15 have it
 * (docs/platforms/titan.md). One class, split over files by chip:
 *
 *  - Titan.cpp: the physical address decode, interrupts, reset and state;
 *  - TitanCchip.cpp: the Cchip's CSRs (MISC, DIM/DIR for four processors,
 *    DRIR, AARn, MPD, IICn, ...) and the Dchips';
 *  - TitanPachip.cpp: the two PA-chips, each a G-port and an A-port, which
 *    are the four PCI hoses, and their DMA windows (PciWindows.hpp, shared
 *    with the Tsunami's Pchips);
 *  - TitanTig.cpp: the TIG bus registers.
 *
 * Sources: Linux arch/alpha/include/asm/core_titan.h (register layout, from
 * the "Titan Chipset Engineering Specification" rev 0.12), core_titan.c
 * (hose numbering, windows), sys_titan.c (interrupts); and what the ES45
 * console does, which is the specification wherever they differ.
 *
 * What a Titan is next to a Typhoon, as far as this model goes:
 *  - the same physical map for the Cchip (801.A000.0000), the Dchips
 *    (801.B000.0000) and the TIG bus (801.0000.0000), and the same register
 *    layout for MISC, DIM0-3/DIR0-3/DRIR and the interval timer;
 *  - four PCI hoses instead of two. Each PA-chip has two ports, a G-port
 *    (PCI or PCI-X) and an A-port (PCI or AGP), each with its own windows
 *    and control register: hose 0 = PA-chip 0 G-port, 1 = PA-chip 1 G-port,
 *    2 = PA-chip 0 A-port, 3 = PA-chip 1 A-port. Hose h's spaces are at
 *    800.0000.0000 + h * 2.0000.0000, laid out as the Tsunami's hose 0 and
 *    1: so hoses 0 and 1, and the legacy I/O at 801.FC00.0000, are where the
 *    Tsunami has them;
 *  - a port's CSRs are 0x1000 bytes: the G-port at the PA-chip's base
 *    (801.8000.0000, 803.8000.0000), the A-port right after it.
 **/
#if !defined(INCLUDED_TITAN_H_)
#define INCLUDED_TITAN_H_

#include "Chipset.hpp"
#include "DimmModel.hpp"
#include "i2c_spd.hpp"
#include <mutex>

class CTitan : public CChipset {
public:
  explicit CTitan(CSystem *sys);
  ~CTitan() override = default;

  chipset_kind kind() const override { return CHIPSET_TITAN; }
  const char *name() const override { return "Titan 21274"; }

  /// PA<43> selects I/O and PA<34:33> the hose, as on the Tsunami; the
  /// bits between are not decoded.
  u64 phys_mask() const override { return U64(0x00000807ffffffff); }
  u64 read_io(u64 a, u64 raw, int dsize, CSystemComponent *source) override;
  void write_io(u64 a, u64 raw, int dsize, u64 data,
                CSystemComponent *source) override;

  void interrupt(int number, bool assert) override;
  void interval_tick() override { interrupt(-1, true); }
  void ack_interval_timer(int cpu) override;
  void ack_ipi(int cpu) override;

  u64 pci_space_base(int hose, pci_space space) const override;
  u64 pci_phys(int hose, u32 address) override;

  const dimm_population *dimms() const override { return &m_dimms; }

  void reset() override;
  void save_state(FILE *f) override;
  bool restore_state(FILE *f) override;

  static const int HOSES = 4;

private:
  u64 cchip_read(u32 a, CSystemComponent *source);
  void cchip_write(u32 a, u64 data, CSystemComponent *source);
  u64 dchip_read(u32 a);
  u64 port_read(int hose, u32 a);
  void port_write(int hose, u32 a, u64 data);
  u8 tig_read(u32 a);
  void tig_write(u32 a, u8 data);
  void drive_lines(int cpu); ///< b_irq<1:0> from DRIR & DIMn; m_lock held
  void update_halt_lines();
  void power_on_state();

  dimm_population m_dimms;
  struct {
    bool cks_out = true; ///< SCL driver (1 = released)
    bool ds_out = true;  ///< SDA driver
  } m_mpd;
  I2CBus m_mpd_bus;

  /// Serializes DRIR, MISC, DIMn and the halt lines against device threads
  /// and the other processors; not part of the saved state.
  std::mutex m_lock;

public:
  struct SState {
    struct {
      u64 csc, mtr, misc, prben;
      u64 dim[4];
      u64 drir;
      u64 iic[4];
      u64 mpr[4];
      u64 ttr, tdr, pwr;
      u64 cmonctla, cmonctlb, cpen;
    } cchip;
    struct {
      u64 dsc, str, drev, dsc2;
    } dchip;
    /// A PA-chip port (a hose): its CSRs by offset / 0x40, 0x000-0x8c0.
    struct SPort {
      u64 csr[0x24];
    } port[HOSES];
    struct {
      u8 smir, mod_info, ttcr, ev6_halt;
      u8 ipcr[5];
      u8 srcr[2];
      u8 other[64]; ///< TIG registers 0x000-0xfc0 not named above
    } tig;
  } state;
};

#endif // !defined(INCLUDED_TITAN_H_)
