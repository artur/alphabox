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
 * The order of a hand-off through a register that one processor polls and
 * another writes and then reads back: an EV7's RBOX_SCRATCH1 (Ev7Csr.cpp,
 * "The start hand-off").
 *
 * The console starts a secondary by writing 0xf1 and part of an address to
 * the secondary's RBOX_SCRATCH1 and reading the register back until the
 * secondary has answered 0xf2, which the secondary's PALcode does from a
 * loop polling the register. The console gives up after 50 reads, with no
 * delay between them (0x2dd628 in SRM V7.3-1, "CPU %d failed to take jump
 * address part 0"). On the machine that is enough by a wide margin: the
 * owner's read is a local one, a few cycles round its loop, and the other
 * processor's goes out through the router and back. Here both are the same
 * call on two host threads, and a thread the host delays by a few
 * microseconds (taking the register's lock, being scheduled) loses the race.
 *
 * So the order the hardware's timing gives is kept: while the owner polls
 * the register, a value another processor writes there is seen by the
 * owner's next look before that processor's next read of it returns. A
 * remote read waits, releasing the lock, until the owner has looked at the
 * register since that reader's last read, or has written it. The wait is
 * bounded by `bound` in all for one written value (an owner that has
 * stopped polling cannot hold the reader), and by `go_on`, which says
 * whether the owner is still running and nobody is stopping. The hand-off
 * ends at the owner's next write; an owner that read the value and went on
 * without writing (a secondary taking the 0xf3 that sends it to its
 * address) leaves it open until then or until the bound, which costs
 * nothing as long as nobody reads: the console reads no more after its
 * 0xf3, and its spinlocks write their processor's register while they wait
 * (0x423918 in SRM V7.3-1).
 *
 * The owner counts as polling when its last two reads returned the same
 * value, with no write of its own since, and it has gone on less than
 * `recent` of its own instructions since the last one: a processor still in
 * its loop, or held up by the host in it, and not one that read twice and
 * went elsewhere. A write to a register its owner is not polling changes
 * nothing. The owner's read takes no clock: it is every turn of a polling
 * loop, under the lock CPU 0 also takes to deliver each processor's clock
 * tick, and a host clock read there starved CPU 0 with sixteen secondaries
 * polling (M11).
 *
 * Every member function is called with the register block's lock held.
 **/
#if !defined(INCLUDED_STARTHANDOFF_HPP)
#define INCLUDED_STARTHANDOFF_HPP

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class CStartHandoff {
public:
  using clock = std::chrono::steady_clock;
  /// Readers told apart (the processors' numbers).
  static constexpr int kReaders = 64;

  explicit CStartHandoff(
      std::chrono::milliseconds bound = std::chrono::milliseconds(1000),
      uint64_t recent = 1000000)
      : m_bound(bound), m_recent(recent) {}

  /// The owner has read `v` from its register, at `progress` of its own
  /// instructions.
  void owner_read(uint64_t v, uint64_t progress) {
    ++m_touches;
    m_polling = m_last_valid && v == m_last;
    m_last = v;
    m_last_valid = true;
    m_last_progress = progress;
    if (m_waiters)
      m_cv.notify_all();
  }

  /// The owner has written its register: it answered, or left its loop.
  void owner_wrote() {
    ++m_touches;
    m_polling = false;
    m_last_valid = false;
    m_open = false;
    if (m_waiters)
      m_cv.notify_all();
  }

  /// Another processor has written the register; the owner is at
  /// `owner_progress` of its own instructions.
  void remote_wrote(uint64_t owner_progress) {
    if (!m_polling || owner_progress - m_last_progress >= m_recent)
      return;
    m_open = true;
    m_end = clock::now() + m_bound;
    m_at_write = m_touches;
    for (uint64_t &s : m_seen)
      s = m_touches; // every reader waits for the owner's next look
  }

  /// Processor `reader` (not the owner) reads the register: while a
  /// hand-off is open, wait until the owner has looked at the register since
  /// this reader's last read. `lk` holds the register block's lock and is
  /// released while waiting. Returns false when the wait ran into `bound`
  /// with the owner never having looked at the value (an owner that looked
  /// and then went on without writing ends the hand-off there silently).
  template <class GoOn>
  bool remote_read(std::unique_lock<std::mutex> &lk, int reader, GoOn &&go_on) {
    if (!m_open || reader < 0 || reader >= kReaders)
      return true;
    bool ok = true;
    ++m_waiters;
    while (m_open && m_touches == m_seen[reader] && go_on()) {
      const clock::time_point now = clock::now();
      if (now >= m_end) {
        m_open = false; // this value's time is up: no reader waits again
        ok = m_touches != m_at_write;
        break;
      }
      // Wake at least every millisecond to look at go_on() again.
      const clock::time_point slice = now + std::chrono::milliseconds(1);
      m_cv.wait_until(lk, slice < m_end ? slice : m_end);
    }
    --m_waiters;
    m_seen[reader] = m_touches;
    return ok;
  }

  /// Whether a remote read may wait now (for traces).
  bool open() const { return m_open; }

  void reset() {
    m_touches = 0;
    m_last = 0;
    m_last_valid = m_polling = m_open = false;
    for (uint64_t &s : m_seen)
      s = 0;
    if (m_waiters)
      m_cv.notify_all();
  }

private:
  const std::chrono::milliseconds m_bound;
  const uint64_t m_recent; ///< in the owner's instructions
  std::condition_variable m_cv;
  uint64_t m_touches = 0; ///< the owner's reads and writes of the register
  uint64_t m_last = 0;    ///< what the owner's last read returned
  bool m_last_valid = false;
  bool m_polling = false; ///< the owner reads the register in a loop
  uint64_t m_last_progress = 0; ///< the owner's instructions at its last read
  bool m_open = false; ///< a value written by another processor is not seen yet
  clock::time_point m_end; ///< when an open hand-off stops holding readers
  uint64_t m_at_write = 0; ///< m_touches when the other processor wrote
  uint64_t m_seen[kReaders] = {}; ///< m_touches at each reader's last read
  int m_waiters = 0;
};

#endif // !defined(INCLUDED_STARTHANDOFF_HPP)
