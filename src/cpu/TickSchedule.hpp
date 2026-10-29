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
 * One processor's interval-timer schedule, which its own thread keeps
 * (CAlphaCPU::interval_timer_poll) and nothing else reads.
 *
 * The machine broadcasts one tick and every processor takes it within
 * microseconds. Here each processor is a host thread the host may not run
 * for milliseconds, so each keeps its own schedule and repays the ticks its
 * thread was too late to fire: a tick delivered from another processor's
 * thread found the last one not yet taken and was lost
 * (docs/cpu-fidelity.md, "Every processor keeps its own interval timer").
 * The rules, the same on every processor:
 *
 *  - Count-preserving: the schedule advances one period per tick, so ticks
 *    a busy or descheduled thread missed are repaid.
 *  - Paced: repaid at least half a period apart, 0.7 of one on average,
 *    never in a burst (ticks crowded together skew a guest's calibration
 *    of its cycle counter against them), the gaps modulated so that the
 *    console's calibration windows (100 ticks against the cycle counter,
 *    repeated until two agree) never agree during a repay stretch; an
 *    eighth of that gap once more than 32 periods behind (a thread
 *    starved, not stalled for a moment).
 *  - A backlog over a second (a debugger stop, a host sleep, a thread that
 *    gets almost nothing) is dropped.
 *  - The first tick is due at once.
 *  - Processor 0's phase: at every tick of a processor other than 0 its
 *    next tick moves to the nearest instant of processor 0's schedule
 *    (`phase_ns`), at most half a period away, so that every processor's
 *    ticks are due together, as the machine's are. In step it stays there
 *    (both advance by the period), and it follows processor 0's schedule
 *    when that moves (a backlog dropped, its thread started again).
 *
 * A tick that was fired and not taken (the guest's interrupts off) is not
 * owed: it merges with the one pending, as on the machine.
 *
 * Header-only, with no emulator types, so that test/unit/
 * tick_schedule_test.cpp can drive it with a made-up clock.
 **/
#if !defined(INCLUDED_TICKSCHEDULE_H)
#define INCLUDED_TICKSCHEDULE_H

#include <chrono>
#include <cstdint>

class TickSchedule {
public:
  using clock = std::chrono::steady_clock;
  using time_point = clock::time_point;

  /// The schedule begins at `now`, with its first tick due at once.
  void start(time_point now) {
    m_next = m_last = now;
    m_gap_ns = 0;
  }

  /// Time the schedule did not run (a debugger's stop) does not count.
  void shift(clock::duration d) {
    m_next += d;
    m_last += d;
    m_fired_due += d;
  }

  /// When the next tick is due by the count, not counting the spacing.
  time_point next() const { return m_next; }

  /// The earliest instant poll() fires at: whoever sleeps on this
  /// processor's thread wakes by then.
  time_point due() const {
    const time_point paced = m_last + std::chrono::nanoseconds(m_gap_ns);
    return paced > m_next ? paced : m_next;
  }

  /// How many ticks the schedule has fired.
  uint32_t fired() const { return m_fired; }

  /// The instant the last tick fired was due at, by the count.
  time_point fired_due() const { return m_fired_due; }

  /// The backlog the last poll that fired dropped (0: none).
  clock::duration dropped() const { return m_dropped; }

  /// Whether a tick is due at `now`, taking it off the schedule if so. The
  /// period is `period_ns`; with none set yet (0) a tick comes every
  /// `unset_ns`. `phase_ns` is an instant of processor 0's schedule (the
  /// clock's nanoseconds) for the others to keep, or 0 for none.
  bool poll(time_point now, uint64_t period_ns, uint64_t unset_ns,
            int64_t phase_ns) {
    using std::chrono::nanoseconds;
    if (now < m_next)
      return false;
    if (!period_ns) {
      m_fired_due = m_next;
      m_dropped = clock::duration::zero();
      m_last = now;
      m_next = now + nanoseconds(unset_ns);
      ++m_fired;
      return true;
    }
    if (now - m_last < nanoseconds(m_gap_ns))
      return false;
    m_fired_due = m_next;
    m_dropped = clock::duration::zero();
    m_last = now;
    m_gap_ns = next_gap(period_ns);
    m_next += nanoseconds(period_ns);
    // A thread the host gives less than about 70% of a core cannot repay
    // at those gaps: the backlog grows until it passes a second and is
    // dropped, and a guest counting ticks loses the time (at a 60% share
    // the console's timer ran at 810-860 Hz instead of 1024). More than 32
    // periods behind, the thread is starved, not stalled for a moment:
    // repay eight times as fast, still modulated, which keeps up down to
    // about a tenth of a core (four times, until 2026-10-28, kept up down
    // to a fifth: a secondary the host gave 18% of a core then measured
    // its speed at 1.1 to 1.8 times the configured one, after 18 to 31 s).
    // A thread that keeps up never gets here.
    if (now - m_next > nanoseconds(32 * period_ns))
      m_gap_ns /= 8;
    if (now - m_next > std::chrono::seconds(1)) {
      m_dropped = now - m_next;
      m_next = now;
    }
    if (phase_ns)
      m_next = nearest(m_next, period_ns, phase_ns);
    ++m_fired;
    return true;
  }

private:
  /// The instant of `phase_ns`'s schedule nearest to `t`: at most half a
  /// period away.
  static time_point nearest(time_point t, uint64_t period_ns,
                            int64_t phase_ns) {
    using std::chrono::nanoseconds;
    const int64_t p = (int64_t)period_ns;
    const int64_t at =
        std::chrono::duration_cast<nanoseconds>(t.time_since_epoch()).count();
    int64_t r = (phase_ns - at) % p;
    if (r < 0)
      r += p;
    if (r > p / 2)
      r -= p;
    return t + nanoseconds(r);
  }

  /// The minimum spacing before the next catch-up tick. Per-tick noise
  /// alone averages out over a window of ~100 ticks; the triangle wave's
  /// ~2.8-window wavelength survives the averaging, and the noise breaks
  /// symmetric-alignment ties. Between 0.5 and 0.91 of a period, 0.7 on
  /// average: a thread that runs at all repays at most 1.4 times the rate
  /// on average (poll's starved case goes faster).
  uint64_t next_gap(uint64_t period_ns) {
    m_idx++;
    const uint32_t ph = m_idx % 277;
    const uint32_t tri = (ph <= 138) ? ph : (277 - ph);
    m_lcg = m_lcg * 1664525u + 1013904223u;
    return period_ns / 2 + period_ns * tri / 400 +
           period_ns * ((m_lcg >> 24) & 0x3f) / 1024;
  }

  time_point m_next{};         ///< due by the count (+= period per tick)
  time_point m_last{};         ///< the last tick, for the spacing
  time_point m_fired_due{};    ///< when the last tick fired was due
  clock::duration m_dropped{}; ///< the backlog the last tick dropped
  uint64_t m_gap_ns = 0;       ///< the spacing after m_last (0: none)
  uint32_t m_lcg = 0x9e3779b9; ///< the spacing's noise
  uint32_t m_idx = 0;          ///< the spacing's triangle phase
  uint32_t m_fired = 0;
};

/**
 * How far a processor's cycle counter may run, set by its interval timer
 * after each tick (docs/cpu-fidelity.md, "The cycle counter keeps to the
 * interval timer").
 *
 * On the machine both are hardware, and the cycles between two ticks are
 * one period's. Here the counter follows the host's clock while a tick
 * waits for the processor's thread, so a thread the host does not run
 * counts many periods of cycles for one tick, and the console, which
 * measures the processor's speed in cycles per tick, read 2.2 to 3.7 GHz
 * for 833 MHz; from 2^31 Hz OpenVMS cannot start ("The console's frequency
 * is a longword"). So after each tick the counter may run:
 *
 *  - behind the host's clock by how late the tick was, less a quarter
 *    period (`lag`): read right after a tick, the counter is what it was at
 *    the tick's due instant, to the quarter period;
 *  - no further than a quarter period past the next tick's due instant
 *    (`limit`): a thread that is not run does not count the cycles its
 *    ticks do not;
 *  - not over a backlog the schedule dropped (`skip`): the time the count
 *    gives up, the counter gives up too.
 *
 * A thread that fires each tick within a quarter period of its due instant
 * (one the host runs: a dispatch batch is microseconds) has no lag and
 * never reaches the limit: its counter is the host's clock times the speed,
 * as before. The quarter period is that margin.
 **/
struct CcBound {
  TickSchedule::clock::duration lag{};  ///< behind the host's clock
  TickSchedule::time_point limit{};     ///< never past this instant
  TickSchedule::clock::duration skip{}; ///< not billed (a dropped backlog)
  bool bounded = false;                 ///< false: no period, no bound
};

/// The bound after `s` fired a tick at `now`, the period `period_ns`.
inline CcBound cc_bound_after_tick(const TickSchedule &s,
                                   TickSchedule::time_point now,
                                   uint64_t period_ns) {
  using std::chrono::nanoseconds;
  CcBound b;
  if (!period_ns)
    return b; // no period yet: nothing to keep to
  const auto margin = nanoseconds(period_ns / 4);
  b.bounded = true;
  b.limit = s.next() + margin;
  const auto late = now - s.fired_due();
  if (late > margin)
    b.lag = late - margin;
  b.skip = s.dropped();
  return b;
}

#endif
