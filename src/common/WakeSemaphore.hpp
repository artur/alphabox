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

#if !defined(INCLUDED_WAKESEMAPHORE_H)
#define INCLUDED_WAKESEMAPHORE_H

#include <chrono>
#include <condition_variable>
#include <mutex>

/**
 * \brief A device thread's wake-up: a semaphore whose count is 0 or 1.
 *
 * set() makes it signalled -- once: a second set() before anyone waits is
 * lost, as "there is work" needs saying only once -- and wakes the thread
 * waiting for it. A wait takes the signal and clears it.
 *
 * It replaces CSemaphore(0, 1) from the Poco-derived src/base, which
 * saturated at its maximum the same way. No wait gives up on its own unless
 * it was asked to with a bound, and a bounded wait only reports "not
 * signalled": a host that sleeps, or a stopped process, turns into a late
 * wake-up, never into an error.
 **/
class WakeSemaphore {
public:
  WakeSemaphore() = default;
  WakeSemaphore(const WakeSemaphore &) = delete;
  WakeSemaphore &operator=(const WakeSemaphore &) = delete;

  /// Signal it (stays signalled if it was), and wake a waiter.
  void set() {
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      m_signalled = true;
    }
    m_cond.notify_one();
  }

  /// Wait as long as it takes for the signal, and take it.
  void wait() {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cond.wait(lock, [this] { return m_signalled; });
    m_signalled = false;
  }

  /// Take the signal if it comes within `timeout` (zero: if it is already
  /// there); returns whether it was taken.
  bool try_wait_for(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(m_mutex);
    if (!m_cond.wait_for(lock, timeout, [this] { return m_signalled; }))
      return false;
    m_signalled = false;
    return true;
  }

private:
  std::mutex m_mutex;
  std::condition_variable m_cond;
  bool m_signalled = false;
};

#endif // !defined(INCLUDED_WAKESEMAPHORE_H)
