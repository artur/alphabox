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

/* The 8254 programmable interval timer of the ALi M1543C (ports 40h-43h),
 * apart from the bridge: CAliM1543C owns one and connects counter 0's output
 * to IRQ0 and counter 2's gate and output to port 61h, and
 * test/unit/i8254_test.cpp drives one on a clock of its own.
 *
 * Time is wall-clock: the clock the owner passes in gives nanoseconds, and
 * elapsed time is turned into 1.193182 MHz input clocks. A counter's output
 * and count are derived from the time since its phase reference (its last
 * load), so a poller sees exact edges; clock(), called about once a
 * millisecond, steps the decrementing counters that raise IRQ0.
 *
 * Reference: Intel 8254 Programmable Interval Timer, 231164-005 (Sep 1993).
 */

#if !defined(INCLUDED_I8254_H)
#define INCLUDED_I8254_H

#include <cstdint> // datatypes.hpp needs the fixed-width types

#include "datatypes.hpp"
#include <functional>
#include <mutex>

#define PIT_CLOCK_HZ 1193182ULL // 8254 input clock

/// Offsets into SPit_state::counter of a counter's latch word and of its
/// initial count (the value it reloads from).
#define PIT_OFFSET_LATCH 3
#define PIT_OFFSET_MAX 6

/**
 * The three counters as the bridge's state file stores them (the ALi writes
 * its state struct, this included, as it is): any change to the layout makes
 * old state files unreadable.
 **/
struct SPit_state {
  /// [c] the count as clock() decrements it; [c + PIT_OFFSET_LATCH] the
  /// latch word (count, flags, a latched status byte; see I8254.cpp);
  /// [c + PIT_OFFSET_MAX] the initial count, 1..65536.
  u32 counter[9];
  /// [c] the status byte a read-back returns: OUT (bit 7), NULL COUNT
  /// (bit 6), read/write mode, mode, BCD.
  u8 status[4];
  /// [c] write sequencing: the read/write mode of the control word, and 2
  /// while a two-byte count waits for its MSB.
  u8 mode[4];
};
static_assert(sizeof(SPit_state) == 44,
              "SPit_state is in the state file: its layout must not change");

/**
 * \brief An 8254: three counters, their control word register, latches and
 * read-back, over an SPit_state that its owner keeps (and saves).
 *
 * Thread safe: the owner's timer thread (clock()) and the processors
 * (read(), write(), out(), set_gate()) may run at once; every entry takes
 * the timer's lock.
 **/
class CI8254 {
public:
  /// Nanoseconds on a monotonic clock.
  using Clock = std::function<u64()>;

  CI8254(SPit_state &state, Clock now) : s(state), now_ns(std::move(now)) {}

  /// Power-on: every counter unprogrammed, the phase references now.
  void reset();

  /// The ports: 0-2 the counters, 3 the control word (write only).
  u8 read(u32 port);
  void write(u32 port, u8 data);

  /// A counter's OUT pin now.
  bool out(int c);

  /// A counter's GATE input (counter 2's is port 61h bit 0; the others are
  /// tied high). A rising edge restarts a mode 2 or 3 counter.
  void set_gate(int c, bool high);
  /// Set a GATE input as it already is, with no edge (after a restore).
  void load_gate(int c, bool high);

  /// Step the decrementing counters to now. Returns how many rising edges
  /// counter 0's output made, each one an IRQ0 for the owner to raise.
  int clock();

private:
  bool out_locked(int c, u64 now);
  u16 count_now_locked(int c, u64 now);
  void freeze_locked(int c, u64 now, SPit_state &into);
  u64 phase_locked(int c) const;
  /// Input clocks from `from` to `to` (0 if `to` is earlier).
  static u64 clocks_between(u64 from, u64 to);
  static u64 ns_for_clocks(u64 clocks);
  int mode_of(int c) const { return (s.status[c] & 0x0e) >> 1; }

  std::mutex lock;
  SPit_state &s;
  Clock now_ns;
  /// Each counter's phase reference: the time of its last load.
  u64 epoch[3] = {};
  /// clock()'s last time, and the remainder below one input clock it
  /// carries (in ns * PIT_CLOCK_HZ units).
  u64 last = 0;
  u64 acc = 0;
  /// The GATE inputs. Not in the state file: the owner sets counter 2's
  /// from port 61h after a restore.
  bool gate[3] = {true, true, false};
};

#endif // !defined(INCLUDED_I8254_H)
