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

/* Unit test of one processor's interval-timer schedule
 * (src/cpu/TickSchedule.hpp), on a made-up clock: a thread that polls at
 * every dispatch batch is played by a loop that advances the clock and
 * polls, and a thread the host starves by one that polls only in bursts.
 * Checked: the first tick at once; the count kept to wall time for a
 * thread that runs, and down to about a tenth of a core; the spacing of
 * repaid ticks (half a period, a sixteenth when starved) and its modulation
 * across the console's 100-tick windows; a backlog over a second dropped;
 * a processor other than 0 keeping processor 0's phase after its first
 * tick, after a drop and after a period change, and following it when
 * processor 0's schedule moves; the unset period; a debugger's shift;
 * the cycle counter's bound (CcBound): the console's 100-tick calibration
 * windows on a starved thread read one period a tick, a thread that runs
 * is untouched, a dropped backlog is reported and skipped.
 * Built with every lane as `tick_schedule_test`; exit
 * status 0 when every check passes.
 */

#include "TickSchedule.hpp"

#include <cstdio>
#include <cstdlib>
#include <vector>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

using std::chrono::nanoseconds;
using tp = TickSchedule::time_point;

static const uint64_t P = 976562;         // 1024 Hz, as the ES40's
static const uint64_t UNSET = 1000000000; // the ALi's unset period
static const int64_t SEC = 1000000000;    // a second, in nanoseconds
static const int64_t T0 = 1000 * SEC;     // the made-up clock's start

static tp at(int64_t ns) { return tp(nanoseconds(ns)); }
static int64_t ns_of(tp t) {
  return std::chrono::duration_cast<nanoseconds>(t.time_since_epoch()).count();
}

/// Poll `s` every `step` ns from `from` to `to`, in bursts: `run` ns of
/// polling in every `cycle` (the thread's share of a core; run == cycle is
/// a thread that always runs). Records each tick's instant.
static void drive(TickSchedule &s, int64_t from, int64_t to, int64_t step,
                  int64_t run, int64_t cycle, std::vector<int64_t> &ticks,
                  int64_t phase = 0, uint64_t period = P) {
  for (int64_t t = from; t < to; t += step) {
    if ((t - from) % cycle >= run)
      continue;
    if (s.poll(at(t), period, UNSET, phase))
      ticks.push_back(t);
  }
}

static void first_tick_at_once() {
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.due() == at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  CHECK(!s.poll(at(T0), P, UNSET, 0));
  CHECK(s.next() == at(T0 + (int64_t)P));
  CHECK(s.fired() == 1);
}

static void keeps_count_running() {
  // A thread that polls every 2 us for 10 s: one tick per period, each a
  // period after the last (to the polling's grain), none owed at the end.
  TickSchedule s;
  s.start(at(T0));
  std::vector<int64_t> ticks;
  drive(s, T0, T0 + 10 * SEC, 2000, 1, 1, ticks);
  const int64_t want = 10 * SEC / (int64_t)P + 1;
  CHECK((int64_t)ticks.size() >= want - 1 && (int64_t)ticks.size() <= want);
  int64_t worst = 0;
  for (size_t i = 1; i < ticks.size(); i++) {
    const int64_t d = ticks[i] - ticks[i - 1] - (int64_t)P;
    worst = d < 0 ? (-d > worst ? -d : worst) : (d > worst ? d : worst);
  }
  CHECK(worst <= 2000);
}

/// Over 20 s, a thread that runs `run` us of every `cycle`: the ticks it
/// fired plus those it still owes at the end (nothing dropped: the periods
/// in 20 s), and how many it owes.
static int64_t share_ticks(int64_t run_us, int64_t cycle_us, int64_t *owed) {
  TickSchedule s;
  s.start(at(T0));
  std::vector<int64_t> ticks;
  const int64_t end = T0 + 20 * SEC;
  drive(s, T0, end, 2000, run_us * 1000, cycle_us * 1000, ticks);
  *owed = end > ns_of(s.next()) ? (end - ns_of(s.next())) / (int64_t)P : 0;
  return (int64_t)ticks.size() + *owed;
}

static void keeps_count_starved() {
  const int64_t want = 20 * SEC / (int64_t)P;
  int64_t owed;
  // 60% of a core (the share the 3a0b44f measurement used): every period
  // fired or still owed, the backlog held near 32 periods.
  int64_t n = share_ticks(30000, 50000, &owed);
  printf("  60%% of a core: %lld ticks of %lld, %lld owed\n", (long long)n,
         (long long)want, (long long)owed);
  CHECK(n >= want - 1 && n <= want + 1 && owed <= 64);
  // An eighth of a core, 1 ms in 8: still kept.
  n = share_ticks(1000, 8000, &owed);
  printf("  12.5%% of a core: %lld ticks of %lld, %lld owed\n", (long long)n,
         (long long)want, (long long)owed);
  CHECK(n >= want - 1 && n <= want + 1 && owed <= 64);
  // 5% (1 ms in 20): the catch-up falls behind and backlogs are dropped,
  // as the documentation says of less than about a tenth of a core.
  n = share_ticks(1000, 20000, &owed);
  printf("  5%% of a core: %lld ticks of %lld, %lld owed\n", (long long)n,
         (long long)want, (long long)owed);
  CHECK(n < want * 9 / 10);
}

static void repay_spacing() {
  // Stalled 20 periods (under the starved threshold), then polled every
  // microsecond: repaid ticks at least half a period apart.
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  std::vector<int64_t> ticks;
  const int64_t from = T0 + 20 * (int64_t)P;
  drive(s, from, from + 30 * (int64_t)P, 1000, 1, 1, ticks);
  CHECK(ticks.size() > 20);
  int64_t least = INT64_MAX;
  for (size_t i = 1; i < ticks.size(); i++)
    if (ticks[i] - ticks[i - 1] < least)
      least = ticks[i] - ticks[i - 1];
  CHECK(least >= (int64_t)P / 2);
  // Stalled 400 periods: repaid eight times as fast while more than 32
  // behind, never closer than a sixteenth of a period.
  TickSchedule q;
  q.start(at(T0));
  CHECK(q.poll(at(T0), P, UNSET, 0));
  ticks.clear();
  const int64_t from2 = T0 + 400 * (int64_t)P;
  drive(q, from2, from2 + 200 * (int64_t)P, 1000, 1, 1, ticks);
  least = INT64_MAX;
  int64_t first100 = ticks.size() > 100 ? ticks[100] - ticks[0] : 0;
  for (size_t i = 1; i < ticks.size(); i++)
    if (ticks[i] - ticks[i - 1] < least)
      least = ticks[i] - ticks[i - 1];
  CHECK(least >= (int64_t)P / 16);
  CHECK(least < (int64_t)P / 2);
  CHECK(first100 > 0 && first100 < 50 * (int64_t)P);
}

static void repay_windows_disagree() {
  // During a long repay stretch, consecutive windows of 100 ticks (the
  // console's calibration) never take the same time: what keeps it from
  // agreeing on a wrong speed. Stalled 900 ms, then polled every 1 us.
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  std::vector<int64_t> ticks;
  const int64_t from = T0 + 900 * 1000000;
  drive(s, from, from + 2 * SEC, 1000, 1, 1, ticks);
  int windows = 0, agree = 0;
  for (size_t i = 200; i < ticks.size(); i += 100) {
    // only while still repaying: ticks closer than a period apart
    if (ticks[i] - ticks[i - 1] >= (int64_t)P)
      break;
    const int64_t a = ticks[i - 100] - ticks[i - 200];
    const int64_t b = ticks[i] - ticks[i - 100];
    windows++;
    if ((a > b ? a - b : b - a) * 200 < a) // within 0.5%
      agree++;
  }
  printf("  repay windows: %d, agreeing within 0.5%%: %d\n", windows, agree);
  CHECK(windows >= 3);
  CHECK(agree == 0);
}

static void backlog_dropped() {
  // Stalled 3 s: no more than a second's worth repaid, then on time again.
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  std::vector<int64_t> ticks;
  const int64_t from = T0 + 3 * SEC;
  drive(s, from, from + 3 * SEC, 2000, 1, 1, ticks);
  const int64_t owed = 6 * SEC / (int64_t)P;
  printf("  3 s stall: %zu ticks in the 3 s after it, %lld owed\n",
         ticks.size(), (long long)owed);
  CHECK((int64_t)ticks.size() < owed - 1000);
  CHECK((int64_t)ticks.size() >= 3 * SEC / (int64_t)P);
  CHECK(s.next() >= at(from + 3 * SEC - 2 * (int64_t)P));
}

static void joins_phase() {
  // Processor 0 started at T0; processor 1 released 0.3 periods later,
  // which takes its first tick at once and then ticks with processor 0.
  TickSchedule p0, p1;
  p0.start(at(T0));
  const int64_t rel =
      T0 + (10 * (int64_t)P + 3 * (int64_t)P / 10) / 1000 * 1000;
  std::vector<int64_t> t0, t1;
  int64_t phase = 0;
  for (int64_t t = T0; t < T0 + SEC; t += 1000) {
    if (p0.poll(at(t), P, UNSET, 0)) {
      t0.push_back(t);
      phase = ns_of(p0.next());
    }
    if (t == rel)
      p1.start(at(t));
    if (t >= rel && p1.poll(at(t), P, UNSET, phase))
      t1.push_back(t);
  }
  CHECK(!t1.empty() && t1[0] == rel); // the first at once
  // Joining may put its next tick inside the spacing after the first, and
  // the spacing then holds a few ticks late until it has caught up (at
  // least 0.09 of a period a tick). From the tenth on, each of processor
  // 1's ticks is one of processor 0's.
  int off = 0;
  size_t j = 0;
  for (size_t i = 10; i < t1.size(); i++) {
    while (j < t0.size() && t0[j] < t1[i])
      j++;
    if (j >= t0.size() || t0[j] != t1[i])
      off++;
  }
  printf("  joined: %zu ticks on processor 1, %d off processor 0's\n",
         t1.size(), off);
  CHECK(t1.size() > 1000);
  CHECK(off == 0);
  // Processor 0 itself never moves (phase 0).
  TickSchedule q;
  q.start(at(T0 + 123));
  CHECK(q.poll(at(T0 + 123), P, UNSET, 0));
  CHECK(q.next() == at(T0 + 123 + (int64_t)P));
}

static void rejoins_after_drop_and_period_change() {
  const int64_t phase = T0 + 777; // processor 0's instants: phase + k * P
  auto on_phase = [&](tp t, uint64_t period) {
    int64_t r = (ns_of(t) - phase) % (int64_t)period;
    return r == 0;
  };
  TickSchedule s;
  s.start(at(T0 + 5000));
  CHECK(s.poll(at(T0 + 5000), P, UNSET, phase));
  CHECK(on_phase(s.next(), P));
  // A two-second stall: dropped, and back on the phase.
  const int64_t later = T0 + 2 * SEC + 31337;
  CHECK(s.poll(at(later), P, UNSET, phase));
  CHECK(on_phase(s.next(), P));
  // Moved off it by hand (a debugger's stop), then the period changes: on
  // the phase of the new period.
  s.shift(nanoseconds(1234));
  CHECK(!on_phase(s.next(), P));
  const uint64_t P2 = P / 2;
  const tp due = s.due();
  CHECK(s.poll(due, P2, UNSET, phase));
  CHECK(on_phase(s.next(), P2));
  // Joining moves the schedule by at most half a period.
  CHECK(s.next() - due <= nanoseconds(P2 + P2 / 2));
  // Processor 0's schedule moves (it dropped a backlog, or its thread was
  // started again): the next tick follows it.
  const int64_t moved = phase + (int64_t)P2 / 3;
  const tp due2 = s.due();
  CHECK(s.poll(due2, P2, UNSET, moved));
  CHECK((ns_of(s.next()) - moved) % (int64_t)P2 == 0);
}

static void unset_period() {
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), 0, UNSET, 0));
  CHECK(!s.poll(at(T0 + SEC - 1), 0, UNSET, 0));
  CHECK(s.poll(at(T0 + SEC), 0, UNSET, 0));
  CHECK(s.next() == at(T0 + 2 * SEC));
  CHECK(s.fired() == 2);
}

static void debugger_shift() {
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  s.shift(nanoseconds(5 * SEC));
  CHECK(s.next() == at(T0 + 5 * SEC + (int64_t)P));
  CHECK(!s.poll(at(T0 + 5 * SEC), P, UNSET, 0));
  CHECK(s.poll(at(T0 + 5 * SEC + (int64_t)P), P, UNSET, 0));
}

/// The console's speed calibration on a thread the host runs `run` ns of
/// every `cycle` for 20 s: the guest reads its cycle counter at each of its
/// ticks (the interrupt) and takes 100 ticks' cycles over 100 periods. The
/// counter is the host's clock in nanoseconds (1 GHz), billed at every poll
/// as sync_cc_wallclock bills it, with the interval timer's bound
/// (cc_bound_after_tick) or, `bound` false, without. Returns the window
/// furthest from one period a tick, as a fraction (0.01: 1% fast or slow).
static double worst_window(int64_t run, int64_t cycle, bool bound) {
  TickSchedule s;
  s.start(at(T0));
  int64_t billed = T0, lag = 0, lim = INT64_MAX, cc = 0;
  std::vector<int64_t> reads;
  const auto sync = [&](int64_t now) {
    int64_t to = now - lag;
    if (to > lim)
      to = lim;
    if (to < billed)
      to = billed;
    cc += to - billed;
    billed = to;
  };
  const auto ns = [](TickSchedule::clock::duration d) {
    return (int64_t)std::chrono::duration_cast<nanoseconds>(d).count();
  };
  for (int64_t t = T0; t < T0 + 20 * SEC; t += 2000) {
    if ((t - T0) % cycle >= run)
      continue;
    sync(t); // the dispatch batch
    if (!s.poll(at(t), P, UNSET, 0))
      continue;
    if (bound) {
      const CcBound b = cc_bound_after_tick(s, at(t), P);
      CHECK(b.bounded);
      lag = ns(b.lag);
      lim = ns_of(b.limit);
      billed += ns(b.skip);
    }
    sync(t); // the guest's interrupt reads RPCC
    reads.push_back(cc);
  }
  double worst = 0;
  for (size_t i = 0; i + 100 < reads.size(); i++) {
    double e = (double)(reads[i + 100] - reads[i]) / (100.0 * (double)P) - 1;
    e = e < 0 ? -e : e;
    worst = e > worst ? e : worst;
  }
  return worst;
}

static void counter_keeps_to_ticks() {
  // The console's cycle time is 1200 ps at 833 MHz, rounded to 10 ps: a
  // window within 0.4% reads the right speed.
  static const struct {
    int64_t run_us, cycle_us;
    const char *what;
  } shares[] = {{1, 1, "a thread that runs"}, {30000, 50000, "60% of a core"},
                {25000, 100000, "25%"},       {10000, 100000, "10%"},
                {5000, 100000, "5%"},         {1000, 20000, "5%, 1 ms in 20"},
                {2000, 100000, "2%"}};
  for (const auto &sh : shares) {
    const double with =
        worst_window(sh.run_us * 1000, sh.cycle_us * 1000, true);
    const double without =
        worst_window(sh.run_us * 1000, sh.cycle_us * 1000, false);
    printf("  %s: worst 100-tick window %.3f%% off with the bound, %.3f%% "
           "without\n",
           sh.what, with * 100, without * 100);
    CHECK(with < 0.004);
  }
  // A thread that runs is untouched: no lag, never at the limit.
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0 + 3000), P, UNSET, 0));
  const CcBound b = cc_bound_after_tick(s, at(T0 + 3000), P);
  CHECK(b.lag == TickSchedule::clock::duration::zero());
  CHECK(b.limit == at(T0 + (int64_t)P + (int64_t)P / 4));
  CHECK(b.skip == TickSchedule::clock::duration::zero());
  // No period yet: no bound.
  TickSchedule u;
  u.start(at(T0));
  CHECK(u.poll(at(T0), 0, UNSET, 0));
  CHECK(!cc_bound_after_tick(u, at(T0), 0).bounded);
}

static void dropped_backlog_reported() {
  TickSchedule s;
  s.start(at(T0));
  CHECK(s.poll(at(T0), P, UNSET, 0));
  CHECK(s.dropped() == TickSchedule::clock::duration::zero());
  // Five seconds late: the tick due at T0 + P is fired, and the backlog
  // after it given up.
  const tp late = at(T0 + 5 * SEC);
  CHECK(s.poll(late, P, UNSET, 0));
  CHECK(s.fired_due() == at(T0 + (int64_t)P));
  CHECK(s.dropped() == late - at(T0 + 2 * (int64_t)P));
  CHECK(s.next() == late);
  const CcBound b = cc_bound_after_tick(s, late, P);
  CHECK(b.skip == s.dropped());
  CHECK(b.lag == late - at(T0 + (int64_t)P) - nanoseconds(P / 4));
}

int main() {
  first_tick_at_once();
  keeps_count_running();
  keeps_count_starved();
  repay_spacing();
  repay_windows_disagree();
  backlog_dropped();
  joins_phase();
  rejoins_after_drop_and_period_change();
  unset_period();
  debugger_shift();
  counter_keeps_to_ticks();
  dropped_backlog_reported();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
