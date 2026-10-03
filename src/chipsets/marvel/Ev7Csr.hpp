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

#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>

class CAlphaCPU;
class CMarvelTopology;
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
constexpr u32 RBOX_IO_CFG = 0xc000;
/// The route table: 0x114 registers 0x10 apart (the console's table of
/// arrays, "RBOX_ROUTE" at 0x3ac688: offset 0x2000, 0x114 entries, stride
/// 0x10). Entry n < 0x100 is the route to PID n; what the last 0x14 route
/// is not known.
constexpr u32 RBOX_ROUTE = 0x2000;
constexpr u32 RBOX_ROUTE_ENTRIES = 0x114;
// GIO
constexpr u32 GIO_CFG = 0x100;
constexpr u32 GIO_DAT = 0x110;
constexpr u32 GIO_CTL = 0x120;
constexpr u32 GIO_LOCK = 0x80000; // not in the table; the console's own use

/// RBOX_INT bits the console's PALcode names by what it does with them.
constexpr u64 INT_IT = U64(1) << 15; ///< interval timer (cleared by writing it)
constexpr u64 INT_IOQ = U64(1) << 12; ///< an IID waits in RBOX_INTQ
/// The primary's clock-window broadcast (Ev7Csr.cpp, "The clock
/// rendezvous"): masked in RBOX_IMASK, polled by the secondaries.
constexpr u64 INT_SYNC = U64(1) << 23;
/// RBOX_INTQ <24>: the IID at the head of the queue is valid.
constexpr u64 INTQ_VALID = U64(1) << 24;
} // namespace ev7csr

class CEv7Csr {
public:
  CEv7Csr(CSystem *sys, u32 pid, GioManagement *gio);

  u64 read(u32 off, int dsize);
  void write(u32 off, int dsize, u64 data);

  /// One interval-timer period has elapsed.
  void interval_tick();
  /// The period RBOX_IT sets on a processor clocked at `cpu_hz`, or 0 when
  /// the timer is off.
  u64 interval_period_ns(u64 cpu_hz) const;
  /// Another processor (or this one) sets request bits through this
  /// processor's RBOX_IREQ: they appear in its RBOX_INT.
  void request(u64 bits);
  /// An IO7 sends this processor an interrupt identifier (Io7.hpp): it
  /// waits in RBOX_INTQ and raises RBOX_INT<12> until the PALcode takes it.
  void post_iid(u64 iid);
  /// Whether an IO7 hangs on this processor's I/O port (RBOX_IO_CFG).
  void set_io7_attached(bool on);
  /// The route table as the XSROM leaves it: a route to each of the first
  /// `present` processors of `topology`.
  void load_routes(const CMarvelTopology &topology, int present);

  void reset();
  void save_state(FILE *f);
  bool restore_state(FILE *f);

  u32 pid() const { return m_pid; }

  /// ALPHABOX_TRACE_RBOX: report an enabled interrupt (RBOX_INT & RBOX_IMASK)
  /// still pending after `ms` milliseconds, with this processor's state, and
  /// when it is finally taken.
  void check_pending(int ms);
  /// ALPHABOX_TRACE_RBOX: interval ticks delivered, and how many found the
  /// previous one still pending.
  void tick_stats();

  /// The far side of this processor's GIO port (CMarvel::set_management).
  void set_gio_management(GioManagement *far) { m_gio.set_far(far); }

private:
  /// This processor, once the CPUs exist (nullptr before).
  CAlphaCPU *cpu() const;
  /// Deliver RBOX_INT & RBOX_IMASK to the core's external interrupt lines.
  void update_irq();
  void note_pending(u64 pending);
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
  /// RBOX_IT, for the dispatch loop's schedule without taking m_lock.
  std::atomic<u64> m_it{0};
  u64 m_start_hi = 0;
  bool m_start_hi_valid = false;
  bool m_io7 = false;     ///< an IO7 is on the I/O port
  std::deque<u64> m_intq; ///< IIDs the IO7s sent, oldest first
  // ALPHABOX_TRACE_RBOX (check_pending).
  bool m_pend = false, m_pend_reported = false;
  // The clock rendezvous (RBOX_INT<23>): this processor has cleared the bit
  // to wait for the next broadcast; a broadcast that came first is held.
  bool m_sync_waiting = false, m_sync_held = false;
  u64 m_ticks = 0, m_ticks_merged = 0;
  std::chrono::steady_clock::time_point m_pend_since;
};

#endif // !defined(INCLUDED_EV7CSR_H_)
