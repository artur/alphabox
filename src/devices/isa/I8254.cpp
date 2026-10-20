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

/* The 8254 interval timer (see I8254.hpp).
 *
 * Here's the PIT traffic during SRM and Linux startup:
 *
 * SRM
 * PIT Write:  3, 36  = counter 0, load lsb + msb, mode 3
 * PIT Write:  0, 00
 * PIT Write:  0, 00  = 65536 = 18.2 Hz = timer interrupt.
 * PIT Write:  3, 54  = counter 1, msb only, mode 2
 * PIT Write:  1, 12  = 0x1200 = memory refresh?
 * PIT Write:  3, b6  = counter 2, load lsb + msb, mode 3
 * PIT Write:  3, 00
 * PIT Write:  0, 00
 * PIT Write:  0, 00
 *
 * Linux Startup
 * PIT Write:  3, b0  = counter 2, load lsb+msb, mode 0
 * PIT Write:  2, a4
 * PIT Write:  2, ec  = eca4
 * PIT Write:  3, 36  = counter 0, load lsb+msb, mode 3
 * PIT Write:  0, 00
 * PIT Write:  0, 00  = 65536
 * PIT Write:  3, b6  = counter 2, load lsb+msb, mode 3
 * PIT Write:  2, 31
 * PIT Write:  2, 13  = 1331
 */

#include "I8254.hpp"

// The latch word, counter[c + PIT_OFFSET_LATCH]: a latched count in the low
// 16 bits, these flags, and a read-back's status byte in the top byte.
#define PIT_LATCH_VALID 0x00010000U     // a count is latched
#define PIT_LATCH_HIGH_NEXT 0x00020000U // ...and its LSB has been read
#define PIT_LATCH_STATUS 0x00040000U    // a read-back latched the status
#define PIT_LATCH_STATUS_SHIFT 24       // ...which sits in the top byte

u64 CI8254::clocks_between(u64 from, u64 to) {
  if (to <= from)
    return 0;
  const u64 ns = to - from;
  // ns * PIT_CLOCK_HZ overflows 64 bits after about four hours, so split
  // off whole seconds: floor((q * 1e9 + r) * f / 1e9) = q * f + r * f / 1e9
  // exactly, and r * f stays below 2^51.
  return (ns / 1000000000ull) * PIT_CLOCK_HZ +
         (ns % 1000000000ull) * PIT_CLOCK_HZ / 1000000000ull;
}

u64 CI8254::ns_for_clocks(u64 clocks) {
  return (clocks / PIT_CLOCK_HZ) * 1000000000ull +
         (clocks % PIT_CLOCK_HZ) * 1000000000ull / PIT_CLOCK_HZ;
}

void CI8254::reset() {
  std::lock_guard<std::mutex> guard(lock);
  for (int i = 0; i < 3; i++)
    s.status[i] = 0x40; // invalid/null counter
  for (int i = 0; i < 9; i++)
    s.counter[i] = 0;
  last = now_ns();
  for (int i = 0; i < 3; i++)
    epoch[i] = last;
  acc = 0;
}

void CI8254::load_gate(int c, bool high) {
  std::lock_guard<std::mutex> guard(lock);
  gate[c] = high;
}

u8 CI8254::read(u32 port) {
  if (port >= 3)
    return 0; // the control word is write-only

  std::lock_guard<std::mutex> guard(lock);
  u32 &latch = s.counter[port + PIT_OFFSET_LATCH];
  if (latch & PIT_LATCH_STATUS) { // a read-back's status comes out first
    const u8 st = (u8)(latch >> PIT_LATCH_STATUS_SHIFT);
    latch &= 0x00ffffffU & ~PIT_LATCH_STATUS;
    return st;
  }
  const bool latched = (latch & PIT_LATCH_VALID) != 0;
  const u16 count =
      latched ? (u16)latch : count_now_locked((int)port, now_ns());
  const int access = (s.status[port] & 0x30) >> 4;

  switch (access) {
  case 1: // LSB only
    if (latched)
      latch = 0;
    return (u8)count;

  case 2: // MSB only
    if (latched)
      latch = 0;
    return (u8)(count >> 8);

  case 3: // LSB then MSB: hold the count until its MSB has been read
    if ((latch & PIT_LATCH_HIGH_NEXT) == 0) {
      latch = (u32)count | PIT_LATCH_VALID | PIT_LATCH_HIGH_NEXT;
      return (u8)count;
    }
    latch = 0;
    return (u8)(count >> 8);
  }
  return 0;
}

/**
 * The counter's current count, derived from the wall-clock phase since its
 * last load (as out_locked derives OUT), so a guest reading or latching the
 * count -- delay-calibration loops do -- sees it move between clock() steps.
 **/
u16 CI8254::count_now_locked(int c, u64 now) {
  const u32 n = s.counter[c + PIT_OFFSET_MAX];
  if ((s.status[c] & 0x40) || !n || !gate[c]) // unloaded, or gated off
    return (u16)s.counter[c];
  const u64 clocks = clocks_between(epoch[c], now);
  switch (mode_of(c)) {
  case 0: // counts down from n and keeps wrapping past zero
    return (u16)((u64)n - clocks);
  case 2: // rate generator: n .. 1, reloading every n clocks
    return (u16)(n - (u32)(clocks % n));
  case 3: // square wave: counts down by two, reloading each half period
    return (u16)((n - (u32)((clocks * 2) % n)) & ~1u);
  default:
    return (u16)s.counter[c];
  }
}

void CI8254::write(u32 port, u8 data) {
  std::lock_guard<std::mutex> guard(lock);
  const u64 now = now_ns();
  if (port == 3) { // control word
    const int counter = (data >> 6) & 3;
    if (counter == 3) { // read-back command (8254 only; the 8253 has none)
      // Bit 5 clear latches the count, bit 4 clear latches the status, for
      // every counter whose select bit (1 << (c + 1)) is set. A latch that
      // is already held is not replaced. The status byte is what the next
      // read of that counter returns, before any latched count: OUT (bit
      // 7), NULL COUNT (bit 6), the read/write mode, the mode and BCD.
      for (int c = 0; c < 3; c++) {
        if (!(data & (2 << c)))
          continue;
        u32 &latch = s.counter[c + PIT_OFFSET_LATCH];
        if (!(data & 0x10) && !(latch & PIT_LATCH_STATUS)) {
          const u8 st =
              (u8)((out_locked(c, now) ? 0x80 : 0) | (s.status[c] & 0x7f));
          latch |= PIT_LATCH_STATUS | ((u32)st << PIT_LATCH_STATUS_SHIFT);
        }
        if (!(data & 0x20) && !(latch & PIT_LATCH_VALID))
          latch = (latch & (PIT_LATCH_STATUS | 0xff000000U)) |
                  (u32)count_now_locked(c, now) | PIT_LATCH_VALID;
      }
      return;
    }
    const int access = (data >> 4) & 3;
    if (access == 0) { // counter latch command; never replace an unread latch
      u32 &latch = s.counter[counter + PIT_OFFSET_LATCH];
      if ((latch & PIT_LATCH_VALID) == 0)
        latch = (latch & (PIT_LATCH_STATUS | 0xff000000U)) |
                (u32)count_now_locked(counter, now) | PIT_LATCH_VALID;
      return;
    }
    int mode = (data >> 1) & 7;
    if (mode >= 6)
      mode -= 4; // modes 6 and 7 alias 2 and 3
    // status: access + BCD bits, mode, NULL COUNT (0x40); OUT (0x80) goes
    // high after a control word in every mode but 0
    s.status[counter] = (u8)((data & 0x31) | (mode << 1) | 0x40);
    if (mode != 0)
      s.status[counter] |= 0x80;
    s.mode[counter] = (u8)access; // write sequencing state
    s.counter[counter + PIT_OFFSET_LATCH] = 0;
    return;
  }

  if (port < 3) { // count register
    const int access = (s.status[port] & 0x30) >> 4;
    u32 count = 0;
    switch (access) {
    case 1: // LSB only
      count = data;
      break;
    case 2: // MSB only
      count = (u32)data << 8;
      break;
    case 3: // LSB then MSB: stash the LSB until the MSB arrives
      if (s.mode[port] == 3) {
        s.counter[port + PIT_OFFSET_LATCH] = data;
        s.mode[port] = 2;
        return;
      }
      count = (s.counter[port + PIT_OFFSET_LATCH] & 0xFFU) | ((u32)data << 8);
      s.mode[port] = 3;
      break;
    default:
      return;
    }
    if (count == 0)
      count = 65536; // a zero count means 2^16
    s.counter[port] = count;
    s.counter[port + PIT_OFFSET_MAX] = count;
    s.counter[port + PIT_OFFSET_LATCH] = 0;
    s.status[port] &= ~0x40; // count loaded
    if (mode_of((int)port) == 0)
      s.status[port] &= ~0x80; // mode 0: OUT low while counting
    else
      s.status[port] |= 0x80;
    epoch[port] = now; // phase reference
  }
}

bool CI8254::out(int c) {
  std::lock_guard<std::mutex> guard(lock);
  return out_locked(c, now_ns());
}

/**
 * Derive a counter's output pin from wall-clock phase since its last load.
 * Analytic on purpose: a poller (port 61h bit 5) sees exact edges instead of
 * edges quantized to the owner thread's wakeup cadence.
 **/
bool CI8254::out_locked(int c, u64 now) {
  const int mode = mode_of(c);
  if (s.status[c] & 0x40) // control word written, no count loaded yet
    return mode != 0;     // mode 0 starts low, the others high
  if (!gate[c])           // a low GATE inhibits the counter
    return mode == 0 ? (s.status[c] & 0x80) != 0 : true;
  const u32 n = s.counter[c + PIT_OFFSET_MAX];
  if (!n)
    return true;

  const u64 clocks = clocks_between(epoch[c], now);
  switch (mode) {
  case 0: // interrupt on terminal count: low while counting, high at terminal
    return clocks >= n;

  case 3: // square wave: high for the first half of each period
    return ((clocks % n) * 2) < n;

  default: // unmodelled mode: fall back to the decrement machinery's view
    return (s.status[c] & 0x80) != 0;
  }
}

/**
 * Write a counter's analytic state at `now` -- its count and OUT -- into
 * the words clock() keeps (`into` is the state or a copy of it), so that
 * phase_locked() finds the same point of the count again.
 **/
void CI8254::freeze_locked(int c, u64 now, SPit_state &into) {
  const u32 n = s.counter[c + PIT_OFFSET_MAX];
  if ((s.status[c] & 0x40) || !n || !gate[c]) // not counting: already held
    return;
  const u64 clocks = clocks_between(epoch[c], now);
  u32 count;
  bool high;
  switch (mode_of(c)) {
  case 0: // counts down from n, wrapping past zero; OUT high from terminal
    count = (u32)(((u64)n - clocks) & 0xffffu);
    high = clocks >= n;
    break;
  case 2: // n .. 1
    count = n - (u32)(clocks % n);
    high = true;
    break;
  case 3: { // down by two from n in each half; OUT high in the first
    const u32 p = (u32)(clocks % n);
    const u32 first = (n + 1) / 2;
    high = p < first;
    count = n - 2 * (high ? p : p - first);
    break;
  }
  default:
    return;
  }
  into.counter[c] = count;
  if (high)
    into.status[c] |= 0x80;
  else
    into.status[c] &= ~0x80;
}

/**
 * How many input clocks into its count the words clock() keeps put a
 * counter: the inverse of freeze_locked().
 **/
u64 CI8254::phase_locked(int c) const {
  const u32 n = s.counter[c + PIT_OFFSET_MAX];
  const u32 count = s.counter[c];
  if ((s.status[c] & 0x40) || !n)
    return 0;
  switch (mode_of(c)) {
  case 0:
    if (!(s.status[c] & 0x80)) // still counting down from n
      return count <= n ? n - count : 0;
    return (u64)n + ((0x10000u - count) & 0xffffu); // wrapped past zero
  case 2:
    return count >= 1 && count <= n ? n - count : 0;
  case 3: {
    const u32 half = count <= n ? (n - count) / 2 : 0;
    return (s.status[c] & 0x80) ? half : (n + 1) / 2 + half;
  }
  default:
    return 0;
  }
}

void CI8254::set_gate(int c, bool high) {
  std::lock_guard<std::mutex> guard(lock);
  const bool was_high = gate[c];
  const int mode = mode_of(c);
  const u64 now = now_ns();
  if (was_high && !high) {
    if (mode == 0) // counting stops where it is; OUT keeps its level
      freeze_locked(c, now, s);
    else
      s.status[c] |= 0x80; // gate low forces OUT high in modes 1-5
  }
  gate[c] = high;
  if (!was_high && high) {
    if (mode == 2 || mode == 3) { // rising gate restarts the count
      s.counter[c] = s.counter[c + PIT_OFFSET_MAX];
      s.status[c] |= 0x80;
      epoch[c] = now;
    } else if (mode == 0) {
      // mode 0 resumes where it was suspended: rebase the phase reference
      epoch[c] = now - ns_for_clocks(phase_locked(c));
    }
  }
}

/**
 * Advance the counters by elapsed wall-clock time.
 *
 *  - counter 0 is the 18.2Hz time counter.
 *  - counter 1 is the ram refresh, we don't care.
 *  - counter 2 is the speaker and/or generic timer
 *  .
 **/
int CI8254::clock() {
  std::lock_guard<std::mutex> guard(lock);
  // Convert elapsed host time to 1.193182 MHz input clocks. The pacing must
  // be wall-clock: SRM calibrates the boot CPU's speed against this timer,
  // and the guest RPCC it compares against is wall-clock true.
  const u64 now = now_ns();
  u64 elapsed_ns = now > last ? now - last : 0;
  last = now;
  if (elapsed_ns > 30000000ull) // stall/restore catch-up cap: under one ch0
    elapsed_ns = 30000000ull;   // half-period, so toggles can never bunch
                                // into one call
  acc += elapsed_ns * PIT_CLOCK_HZ;
  const u64 clocks = acc / 1000000000ull;
  acc %= 1000000000ull;
  if (!clocks)
    return 0;

  int irq0 = 0;
  for (int i = 0; i < 3; i++) {
    if (s.status[i] & 0x40)
      continue;
    if (!gate[i]) // a low GATE holds the counter
      continue;
    switch (mode_of(i)) {
    case 0: // interrupt on terminal count
      // OUT goes high at terminal count and stays high until a new count or
      // control word (data sheet p.10); the counter does not stop, it wraps
      // to FFFF and counts on (p.17). NULL COUNT is not touched: it only
      // reports a written count not yet loaded (p.9).
      if (s.counter[i] <= clocks)
        s.status[i] |= 0x80;
      s.counter[i] = (u32)(((u64)s.counter[i] - clocks) & 0xffffu);
      break;

    case 3: // square wave generator: counts down by two per input clock,
            // output toggles at terminal, so one output period = one reload
            // value
    {
      u64 dec = clocks * 2;
      const u32 reload = s.counter[i + PIT_OFFSET_MAX];
      while (dec >= s.counter[i]) {
        if (!reload) { // half-programmed counter: don't spin on a zero reload
          s.counter[i] = 0;
          dec = 0;
          break;
        }
        dec -= s.counter[i];
        s.counter[i] = reload;
        if (s.status[i] & 0x80) {
          s.status[i] &= ~0x80; // lower output;
        } else {
          s.status[i] |= 0x80; // raise output
          if (i == 0)
            irq0++; // counter 0 is tied to irq 0.
        }
      }
      s.counter[i] -= (u32)dec;
    } break;

    default:
      break; // we don't care to handle it.
    }
  }
  return irq0;
}
