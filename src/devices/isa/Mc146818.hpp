/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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

/* The ALi M1543C's MC146818-compatible time-of-year clock and its 256 bytes
 * of CMOS RAM (ports 70h-73h), apart from the bridge: CAliM1543C owns one,
 * keeps its registers in its state file and reads the periodic rate for the
 * interval timer, and test/unit/mc146818_test.cpp drives one on clocks of
 * its own.
 *
 * The clock is the host's wall clock plus an offset in whole seconds (the
 * guest's setting of the time): the time registers are generated from it,
 * not counted.
 *
 * Reference: Motorola MC146818A Real-Time Clock Plus RAM (Advance
 * Information).
 */

#if !defined(INCLUDED_MC146818_H)
#define INCLUDED_MC146818_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>
#include <mutex>

/**
 * \brief The TOY clock: ports 0 and 1 (70h, 71h) select and access the
 * clock's registers and the first 128 bytes of RAM, ports 2 and 3 (72h,
 * 73h) the second 128.
 *
 * The registers live in the owner's state (it saves them): `ram` holds the
 * 256 bytes (time, alarm and control registers A-D at 00h-0Dh), `ports` the
 * last value written to or latched for each port, `offset` the seconds
 * between the guest's time and the host's.
 *
 * Thread safe: every port access takes the clock's lock.
 **/
class CMc146818 {
public:
  struct Clocks {
    /// The host's wall clock: nanoseconds since the Unix epoch, UTC.
    std::function<s64()> real_ns;
    /// Nanoseconds on a monotonic clock.
    std::function<u64()> steady_ns;
  };

  CMc146818(u8 (&ram)[256], u8 (&ports)[4], long &offset, Clocks clocks)
      : ram(ram), ports(ports), offset(offset), clk(std::move(clocks)) {}

  /// The ARC console counts years from 1980 (see AliM1543C.hpp).
  void set_arc_year_compat(bool on) { arc_year_compat = on; }

  u8 read(u32 port);
  void write(u32 port, u8 data);

  /// Period in ns of the periodic interrupt / square wave rate in register
  /// A, 0 when none is selected. Lock-free: a processor reads it every
  /// batch.
  u64 interval_period_ns() const;

private:
  u8 register_locked(u8 index);
  void generate_time_locked();
  void update_pf_locked();
  void set_time_from_registers(u8 new_b);

  std::mutex lock;
  u8 (&ram)[256];
  u8 (&ports)[4];
  long &offset;
  Clocks clk;
  bool arc_year_compat = false;

  /// Register C's periodic flag (PF), paced by the monotonic clock: the
  /// time the rate was last chosen, periods counted since, and the rate.
  u64 pf_epoch = 0;
  u64 pf_count = 0;
  u64 pf_freq = 0;
  /// ALPHABOX_TRACE_TOY's counts of what it reports only the first of.
  int reg_c_reads = 0;
  int nmi_selects = 0;
};

#endif // !defined(INCLUDED_MC146818_H)
