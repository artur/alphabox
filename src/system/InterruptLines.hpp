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
 * The chipset's device interrupt inputs as the wires they are.
 *
 * Every input (a Tsunami or Titan DRIR bit, an IO7 LSI on the Marvel) is a
 * level-sensitive line that the board ORs from everything wired to it
 * (21272 HRM 6.3, Table 6-9: DRIR<55:0> "PCI devices (level sensitive)").
 * Several sources can share one: two devices behind a PCI-PCI bridge whose
 * INTx rotate onto the same pin (devices d and d + 4 on the secondary bus,
 * PCI-to-PCI Bridge Architecture Specification 1.1 Table 9-1), the
 * functions of one device that a board wires together, a device and
 * anything else the board puts on that input. The line is asserted while
 * any of them asserts it. The chipset sees only the line, so the OR is
 * kept here, per input and per source, and the chipset is told the line's
 * level each time a source drives it.
 **/
#if !defined(INCLUDED_INTERRUPTLINES_HPP)
#define INCLUDED_INTERRUPTLINES_HPP

#include <cstddef>
#include <mutex>
#include <vector>

class CInterruptLines {
public:
  CInterruptLines() { m_on.reserve(32); }

  /// `source` drives `input` to `level`. A source is named by an address
  /// (the device) and `line`, which tells apart the several requests one
  /// device makes (its functions). The line's level, the OR of every source
  /// asserting it now, is handed to `deliver(input, level)` and returned.
  ///
  /// `deliver` runs under the lock, so the chipset sees the levels in the
  /// order they came about: two sources letting go and asserting at once
  /// cannot leave it holding the level of the one that lost the race. It
  /// must not call back into this object.
  template <class Deliver>
  bool drive(int input, const void *source, int line, bool level,
             Deliver &&deliver) {
    std::lock_guard<std::mutex> g(m_mx);
    const size_t i = find(input, source, line);
    if (level && i == m_on.size())
      m_on.push_back({input, source, line});
    else if (!level && i != m_on.size()) {
      m_on[i] = m_on.back();
      m_on.pop_back();
    }
    const bool high = asserted(input);
    deliver(input, high);
    return high;
  }

  /// Whether anything asserts `input` now.
  bool level(int input) const {
    std::lock_guard<std::mutex> g(m_mx);
    return asserted(input);
  }

  /// A machine reset: the chipset forgets its inputs, and so does this.
  void reset() {
    std::lock_guard<std::mutex> g(m_mx);
    m_on.clear();
  }

private:
  struct request {
    int input;
    const void *source;
    int line;
  };

  /// The requests asserted now: a handful at most, so a list it is.
  size_t find(int input, const void *source, int line) const {
    for (size_t i = 0; i < m_on.size(); i++)
      if (m_on[i].input == input && m_on[i].source == source &&
          m_on[i].line == line)
        return i;
    return m_on.size();
  }
  bool asserted(int input) const {
    for (const request &r : m_on)
      if (r.input == input)
        return true;
    return false;
  }

  mutable std::mutex m_mx;
  std::vector<request> m_on;
};

#endif // !defined(INCLUDED_INTERRUPTLINES_HPP)
