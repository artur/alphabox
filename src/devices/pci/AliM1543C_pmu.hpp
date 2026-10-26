/* Alphabox Alpha Emulator
 *
 * Ported from: the ALi M7101 power management unit of ES40-Emu
 * Copyright (C) 2026 gdwnldsKSC and the ES40-Emu contributors
 * Website: https://github.com/ES40-Emu/es40
 *
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * SPDX-License-Identifier: BSD-1-Clause
 *
 * ES40-Emu distributes this module under the BSD 1-Clause licence, and this
 * fork's changes to it are distributed under the same licence, so that they
 * can go back to ES40-Emu:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS AND CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
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
