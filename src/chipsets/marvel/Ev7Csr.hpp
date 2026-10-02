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
 * One EV7's on-chip register block: the 4 MB window at its PID's CSR base
 * (cpu/ev7/Ev7.hpp) holding the Rbox (router, interrupts, interval timer,
 * scratch), the Cbox and Bbox, the two Zbox memory controllers, the pads,
 * the on-chip logic analysers and the GIO port.
 *
 * The register names and offsets are the console's own table (141 entries,
 * extracted from SRM V7.3-1: lab/docs-ev7/srm73-csr-tables.txt); Linux
 * names only the Rbox block. What a register DOES is modelled where the
 * console's code shows it (docs/platforms/marvel.md, M2): the rest hold what
 * is written to them and read back the state a configured machine would
 * have, which for the error registers is clean.
 **/
#if !defined(INCLUDED_EV7CSR_H_)
#define INCLUDED_EV7CSR_H_

#include "StdAfx.hpp"

#include "Gio.hpp"

#include <cstdio>
#include <map>
#include <mutex>

class CAlphaCPU;
class CSystem;

namespace ev7csr {
// Rbox
constexpr u32 RBOX_CFG = 0x000;
constexpr u32 RBOX_NSVC = 0x010;
constexpr u32 RBOX_EWVC = 0x020;
constexpr u32 RBOX_WHOAMI = 0x030;
constexpr u32 RBOX_TCTL = 0x040;
constexpr u32 RBOX_INT = 0x050;
constexpr u32 RBOX_IMASK = 0x060;
constexpr u32 RBOX_IREQ = 0x070;
constexpr u32 RBOX_INTQ = 0x080;
constexpr u32 RBOX_INTA = 0x090;
constexpr u32 RBOX_IT = 0x0a0;
constexpr u32 RBOX_SCRATCH1 = 0x0b0;
constexpr u32 RBOX_SCRATCH2 = 0x0c0; // Linux core_marvel.h; not in the table
constexpr u32 RBOX_L_ERR = 0x0d0;
// GIO
constexpr u32 GIO_CFG = 0x100;
constexpr u32 GIO_DAT = 0x110;
constexpr u32 GIO_CTL = 0x120;
constexpr u32 GIO_LOCK = 0x80000; // not in the table; the console's own use

/// RBOX_INT bits the console's PALcode names by what it does with them.
constexpr u64 INT_IT = U64(1) << 15; ///< interval timer (cleared by writing it)
} // namespace ev7csr

class CEv7Csr {
public:
  CEv7Csr(CSystem *sys, u32 pid, GioManagement *gio);

  u64 read(u32 off, int dsize);
  void write(u32 off, int dsize, u64 data);

  /// One interval-timer period has elapsed.
  void interval_tick();
  /// Another processor (or this one) sets request bits through this
  /// processor's RBOX_IREQ: they appear in its RBOX_INT.
  void request(u64 bits);

  void reset();
  void save_state(FILE *f);
  bool restore_state(FILE *f);

  u32 pid() const { return m_pid; }

private:
  /// This processor, once the CPUs exist (nullptr before).
  CAlphaCPU *cpu() const;
  /// Deliver RBOX_INT & RBOX_IMASK to the core's external interrupt lines.
  void update_irq();
  /// RBOX_SCRATCH1 written: the XSROM's wait for a start address, served
  /// for a processor that is still parked.
  void scratch_written(u64 v);
  u64 reg(u32 off) const;

  CSystem *m_sys;
  u32 m_pid;
  std::mutex m_lock;
  /// Every named register's contents (the table in Ev7Csr.cpp).
  std::map<u32, u64> m_regs;
  CGioPort m_gio;
  int m_lines = 0; ///< the EI lines this block asserts now
  u64 m_start_hi = 0;
  bool m_start_hi_valid = false;
};

#endif // !defined(INCLUDED_EV7CSR_H_)
