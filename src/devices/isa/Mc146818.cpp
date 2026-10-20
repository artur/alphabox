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

/* The MC146818 time-of-year clock (see Mc146818.hpp).
 *
 * The registers, as Linux names them (include/linux/mc146818rtc.h):
 *   A (0Ah): UIP (bit 7, update in progress), DV2-0 (divider: 010 is the
 *            32.768 kHz time base), RS3-0 (periodic rate). SRM sets 0x26:
 *            32.768 kHz, 1024 Hz.
 *   B (0Bh): SET, PIE, AIE, UIE, SQWE, DM (1 binary, 0 BCD), 24/12, DSE.
 *            SRM sets 0x0e: SQWE, binary, 24 hour; Linux sets PIE later.
 *   C (0Ch): IRQF, PF, AF, UF; cleared by a read.
 *   D (0Dh): VRT, valid RAM and time.
 * See also NetBSD's sys/dev/ic/mc146818reg.h and sys/arch/alpha/alpha/
 * mcclock.c.
 */

#include "Mc146818.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#define MC_BASE_32_KHz 0x20
#define RTC_PF 0x40

static void utc_tm(time_t t, struct tm *out) {
#ifdef _WIN32
  gmtime_s(out, &t);
#else
  gmtime_r(&t, out);
#endif
}

static time_t utc_seconds(struct tm *t) {
#ifdef _WIN32
  return _mkgmtime(t);
#else
  return timegm(t);
#endif
}

/// The guest's time now, in whole seconds since the epoch.
static time_t host_seconds(s64 real_ns) {
  return (time_t)(real_ns >= 0 ? real_ns / 1000000000
                               : -((-real_ns + 999999999) / 1000000000));
}

// ALPHABOX_TRACE_TOY: the guest's writes to the clock's registers (00h-0Dh),
// selections with the NMI-disable bit set, and the first reads of register
// C, up to a few hundred lines in all.
static const bool trace_toy = getenv("ALPHABOX_TRACE_TOY") != nullptr;
static std::atomic<int> trace_lines{0};
#define TOY_TRACE(...)                                                         \
  do {                                                                         \
    if (trace_toy && trace_lines.fetch_add(1) < 400)                           \
      printf("%%TOY-I-TRACE: " __VA_ARGS__);                                   \
  } while (0)

u8 CMc146818::read(u32 port) {
  std::lock_guard<std::mutex> guard(lock);
  switch (port) {
  case 1: { // the register port 0 selects, as it is now
    const u8 index = ports[0] & 0x7f;
    const u8 v = register_locked(index);
    if (trace_toy && index == 0x0c && reg_c_reads++ < 32)
      TOY_TRACE("read reg 0c = %02x (index byte %02x)\n", v, ports[0]);
    ports[1] = v;
    return v;
  }
  case 3: // the upper 128 bytes of RAM
    ports[3] = ram[0x80 + (ports[2] & 0x7f)];
    return ports[3];
  default: // the index ports read back what was written
    return ports[port];
  }
}

/**
 * One of the clock's registers or RAM bytes, as it is at the time of the
 * read: the address strobe only latches the address, and the data strobe
 * of the read is when the chip drives the register onto the bus (data
 * sheet p.8, AS and DS); and reading register C clears its flags
 * (p.15-16, "All flag bits are cleared after Register C is read").
 **/
u8 CMc146818::register_locked(u8 index) {
  if (index < 0x0e) {
    if (!(ram[0x0b] & 0x80)) { // SET clear: the clock runs
      ram[0x0d] = 0x80;        // valid RAM and time
      generate_time_locked();
    }
    if (index == 0x0c) { // the flags, cleared by reading them
      update_pf_locked();
      const u8 v = ram[0x0c];
      ram[0x0c] = 0;
      return v;
    }
  }
  return ram[index];
}

/**
 * Write a port: an index on ports 0 and 2, the register or RAM byte it
 * selects on ports 1 and 3.
 **/
void CMc146818::write(u32 port, u8 data) {
  std::lock_guard<std::mutex> guard(lock);
  if (port == 1 && (ports[0] & 0x7f) < 0x0e)
    TOY_TRACE("write reg %02x = %02x (index byte %02x, B=%02x)\n",
              ports[0] & 0x7f, data, ports[0], ram[0x0b]);
  if (trace_toy && port == 0 && (data & 0x80) && nmi_selects++ < 32)
    TOY_TRACE("select %02x (NMI-disable bit set)\n", data);
  ports[port] = data;

  switch (port) {
  case 1: {
    const u8 index = ports[0] & 0x7f;
    if (index == 0x0b && (data & 0x40)) // If we're writing to register B, we
                                        // make register C look like it fired.
      ram[0x0c] = 0xf0;
    // On REG_B SET 1->0 transition, snapshot the just-written time/date
    // registers and compute an offset from host time so the value sticks
    // after SET goes low.  Without this, the time read next would overwrite
    // the user's writes.
    if (index == 0x0b && (ram[0x0b] & 0x80) && !(data & 0x80))
      set_time_from_registers(data);
    ram[index] = data;
    break;
  }

  case 3:
    ram[0x80 + (ports[2] & 0x7f)] = data;
    break;
  }
}

/**
 * The time and date registers for now, in the data mode and hour format
 * register B selects, and register A's UIP.
 **/
void CMc146818::generate_time_locked() {
  struct tm stime;
  utc_tm(host_seconds(clk.real_ns()) + offset, &stime);
  // 12-hour mode counts 1 to 12, PM in bit 7 (data sheet Table 3, p.12:
  // $01-$0C AM, $81-$8C PM): midnight is 12 AM and noon 12 PM, never 0.
  const u8 pm = (stime.tm_hour >= 12) ? 0x80 : 0;
  const int h12 = (stime.tm_hour % 12) ? (stime.tm_hour % 12) : 12;
  if (ram[0x0b] & 4) {

    // binary
    ram[0] = (u8)(stime.tm_sec);
    ram[2] = (u8)(stime.tm_min);
    if (ram[0x0b] & 2) // 24-hour
      ram[4] = (u8)(stime.tm_hour);
    else
      // 12-hour
      ram[4] = (u8)(pm | h12);
    ram[6] = (u8)(stime.tm_wday + 1);
    ram[7] = (u8)(stime.tm_mday);
    ram[8] = (u8)(stime.tm_mon + 1);
    ram[9] =
        (u8)(arc_year_compat ? (stime.tm_year - 80) : (stime.tm_year % 100));
  } else {

    // BCD
    ram[0] = (u8)(((stime.tm_sec / 10) << 4) | (stime.tm_sec % 10));
    ram[2] = (u8)(((stime.tm_min / 10) << 4) | (stime.tm_min % 10));
    if (ram[0x0b] & 2) // 24-hour
      ram[4] = (u8)(((stime.tm_hour / 10) << 4) | (stime.tm_hour % 10));
    else // 12-hour
      ram[4] = (u8)(pm | ((h12 / 10) << 4) | (h12 % 10));

    ram[6] = (u8)(stime.tm_wday + 1);
    ram[7] = (u8)(((stime.tm_mday / 10) << 4) | (stime.tm_mday % 10));
    ram[8] = (u8)((((stime.tm_mon + 1) / 10) << 4) | ((stime.tm_mon + 1) % 10));
    {
      int yy_dec =
          arc_year_compat ? (stime.tm_year - 80) : (stime.tm_year % 100);
      ram[9] = (u8)(((yy_dec / 10) << 4) | (yy_dec % 10));
    }
  }

  // MC146818 UIP (Update-In-Progress) bit at register A bit 7.
  // Real hardware pulses UIP high for ~244 µs every 1 second of
  // wall-clock when the internal RTC is about to advance, then
  // holds it low for the remainder of the second.
  const u64 now = clk.steady_ns();
  if (!uip_started) {
    uip_started = true;
    uip_epoch = now;
  }
  const u64 sub_sec_us = ((now - uip_epoch) / 1000) % 1000000ull;
  // UIP is high during the last 244 µs of each second.
  if (sub_sec_us >= (1000000ull - 244ull))
    ram[0x0a] |= 0x80;
  else
    ram[0x0a] &= ~0x80;
}

/**
 * Periodic flag (reg C PF), paced by wall-clock time: PF sets once per
 * elapsed rate period, counted against a fixed epoch, so a fast poller sees
 * the true cadence (976.5625 us at the SRM's 1024 Hz) and a slow one
 * coalesces missed periods into the single flag, as on the chip.
 **/
void CMc146818::update_pf_locked() {
  const int rate_pow = ram[0x0a] & 0x0f;
  u64 freq = rate_pow ? (65536ull >> rate_pow) : 0;
  if (ram[0x0a] & MC_BASE_32_KHz) {
    if (rate_pow == 0x1)
      freq = 256;
    else if (rate_pow == 0x2)
      freq = 128;
  }
  if (!freq)
    return;
  const u64 now = clk.steady_ns();
  if (freq != pf_freq) { // rate change or first use: restart
    pf_freq = freq;
    pf_epoch = now;
    pf_count = 0;
  }
  const u64 elapsed_ns = now - pf_epoch;
  // elapsed_ns * 32768 overflows 64 bits after ~6 days, so split off whole
  // seconds: floor((q * 1e9 + r) * f / 1e9) = q * f + r * f / 1e9 exactly,
  // and r * f stays below 2^45. (No __int128: 32-bit and MSVC builds have
  // none.)
  const u64 due = (elapsed_ns / 1000000000ull) * freq +
                  (elapsed_ns % 1000000000ull) * freq / 1000000000ull;
  if (due != pf_count) {
    ram[0x0c] |= RTC_PF;
    pf_count = due;
  }
}

/**
 * Take the time the guest wrote into the time and date registers (in the
 * data mode and hour format of `new_b`, register B as now written) as the
 * clock's time: the offset from the host's time is what is kept.
 **/
void CMc146818::set_time_from_registers(u8 new_b) {
  bool binary = (new_b & 0x04) != 0;
  bool h24 = (new_b & 0x02) != 0;

  u8 ss = ram[0];
  u8 mm = ram[2];
  u8 hh = ram[4];
  u8 dd = ram[7];
  u8 mo = ram[8];
  u8 yy = ram[9];

  bool pm = false;
  if (!h24) {
    pm = (hh & 0x80) != 0;
    hh &= 0x7f;
  }

  auto unbcd = [](u8 v) -> int { return ((v >> 4) & 0x0f) * 10 + (v & 0x0f); };

  struct tm st = {};
  st.tm_sec = binary ? ss : unbcd(ss);
  st.tm_min = binary ? mm : unbcd(mm);
  st.tm_hour = binary ? hh : unbcd(hh);
  st.tm_mday = binary ? dd : unbcd(dd);
  st.tm_mon = (binary ? mo : unbcd(mo)) - 1;
  if (arc_year_compat) {
    st.tm_year = (binary ? yy : unbcd(yy)) + 80; // 1980-base
  } else {
    st.tm_year = binary ? yy : unbcd(yy);
    if (st.tm_year < 70)
      st.tm_year += 100; // 20XX pivot
  }

  if (!h24 && pm && st.tm_hour < 12)
    st.tm_hour += 12;
  if (!h24 && !pm && st.tm_hour == 12)
    st.tm_hour = 0;
  const time_t set_time = utc_seconds(&st);
  if (set_time != (time_t)-1)
    offset = (long)(set_time - host_seconds(clk.real_ns()));
}

u64 CMc146818::interval_period_ns() const {
  const u8 reg_a = ram[0x0a];
  const int rate_pow = reg_a & 0x0f;
  if (rate_pow == 0)
    return 0;

  if (reg_a & 0x20) {
    if (rate_pow == 0x1)
      return 1000000000ull / 256;
    if (rate_pow == 0x2)
      return 1000000000ull / 128;
  }
  return ((1ull << rate_pow) * 1000000000ull) / 65536ull;
}
