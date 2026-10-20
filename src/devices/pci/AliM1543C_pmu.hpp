/* ES40 emulator.
 * Copyright (C) 2007-2026 by the ES40 Emulator Project
 * Copyright (C) 2023-2026 gdwnldsKSC and the ES40-Emu contributors
 *
 * WWW    : https://github.com/gdwnldsKSC/es40
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
 * Stub Power Management Unit (M7101) on the ALi M1543C bridge.
 * Exists so the OS sees an ACPI PM block at PCI 0:17:0; reads/writes
 * are stored but not acted on.
 **/
#if !defined(INCLUDED_ALIM1543C_PMU_H_)
#define INCLUDED_ALIM1543C_PMU_H_

#include "PCIDevice.hpp"

class CAliM1543C_pmu : public CPCIDevice {
public:
  virtual int SaveState(FILE *f);
  virtual int RestoreState(FILE *f);

  CAliM1543C_pmu(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
  virtual ~CAliM1543C_pmu();
  virtual void WriteMem_Bar(int func, int bar, u32 address, int dsize,
                            u32 data);
  virtual u32 ReadMem_Bar(int func, int bar, u32 address, int dsize);

private:
  u32 pm_io_read(u32 address, int dsize);
  void pm_io_write(u32 address, int dsize, u32 data);
  u32 smb_io_read(u32 address, int dsize);
  void smb_io_write(u32 address, int dsize, u32 data);
  u32 pm_timer_value();
  u64 pm_timer_us();

  /// The PM timer: whether it has started, the microseconds it had run at
  /// m_pm_t0 (a time on this process's steady clock, in microseconds).
  std::mutex m_pm_lock;
  bool m_pm_running = false;
  u64 m_pm_base_us = 0;
  u64 m_pm_t0 = 0;

  struct SPMU_state {
    // 64-byte PM I/O block.  Holds the latched values of every read/write
    u8 pm_block[64];
    // 32-byte SMBus I/O block (host status/control + data).
    u8 smb_block[32];
    // The 3.579545 MHz PM timer in a state file: the microseconds it had
    // run, plus one; 0 if it had not started. Unused while running.
    u64 pm_timer_anchor_us;
  } state;
};

#endif // !defined(INCLUDED_ALIM1543C_PMU_H_)
