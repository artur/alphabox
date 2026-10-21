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

#include "StdAfx.hpp"

#include "ToySeed.hpp"

#include <cerrno>
#include <cstdlib>
#include <ctime>
#include <string>

/// The TOY's initial offset from the host clock, in seconds. `faketime` (the
/// system block's `time`, may be null) is an absolute UTC date, "YYYY-MM-DD"
/// or "YYYY-MM-DD HH:MM:SS", and wins; otherwise `tz` is "local" or "utc"
/// with an optional "+<n>" or "-<n>" and a unit s|m|h|d|M|y. A bad value is
/// a configuration error; `current` is returned if the time is unrepresentable.
long toy_seed_offset(const char *faketime, const char *tz, long current) {
  if (faketime) {
    struct tm ft = {};
    int n = sscanf(faketime, "%d-%d-%d %d:%d:%d", &ft.tm_year, &ft.tm_mon,
                   &ft.tm_mday, &ft.tm_hour, &ft.tm_min, &ft.tm_sec);
    if (n < 3)
      FAILURE_1(Configuration,
                "Invalid time format: %s (use YYYY-MM-DD or "
                "YYYY-MM-DD HH:MM:SS)",
                faketime);
    ft.tm_year -= 1900;
    ft.tm_mon -= 1;
#ifdef _WIN32
    time_t set_time = _mkgmtime(&ft);
#else
    time_t set_time = timegm(&ft);
#endif
    if (set_time == (time_t)-1)
      FAILURE_1(Configuration, "Invalid time value: %s", faketime);
    time_t host_now;
    time(&host_now);
    return (long)(set_time - host_now);
  } else {
    // Alphabox 'timezone' option: seed the TOY offset so the guest clock
    // shows host local time by default ("local"), UTC ("utc"), or either
    // with an offset appended: "+<n>" or "-<n>", then s|m|h|d|M|y
    // (e.g. "local+2h", "local-20M", "utc+20y"). Guest TOY writes and the
    // sys0 'time' override still take precedence over this initial seed.
    std::string timezone{tz};
    std::string timebase;
    bool offset_present = false;
    long offset = 0;

    if (timezone.rfind("local", 0) == 0)
      timebase = "local";
    else if (timezone.rfind("utc", 0) == 0)
      timebase = "utc";
    else
      FAILURE_1(Configuration, "Invalid timezone %s", timezone.c_str());

    // The offset: a sign, digits and one unit letter, nothing else.
    const char *rest = timezone.c_str() + timebase.length();
    if (*rest) {
      char *end = nullptr;
      errno = 0;
      if (*rest == '+' || *rest == '-')
        offset = strtol(rest, &end, 10);
      if (!end || end == rest + 1 || errno || !end[0] || end[1])
        FAILURE_1(Configuration, "Invalid timezone %s", timezone.c_str());
      offset_present = true;
    }

    time_t host_now;
    time(&host_now);
    time_t shifted = host_now;

    if (offset_present) {
      switch (timezone.at(timezone.length() - 1)) {
      case 's':
        shifted += offset;
        break;
      case 'm':
        shifted += offset * 60;
        break;
      case 'h':
        shifted += offset * 3600;
        break;
      case 'd':
        shifted += offset * 86400;
        break;
      case 'M':
      case 'y':
        break;
      default:
        FAILURE_1(Configuration, "Invalid timezone offset type %c",
                  timezone.at(timezone.length() - 1));
      }
    }

    struct tm target = {};
    if (timebase == "local") {
#ifdef _WIN32
      localtime_s(&target, &shifted);
#else
      localtime_r(&shifted, &target);
#endif
    } else {
#ifdef _WIN32
      gmtime_s(&target, &shifted);
#else
      gmtime_r(&shifted, &target);
#endif
    }

    if (offset_present) {
      switch (timezone.at(timezone.length() - 1)) {
      case 'M':
        target.tm_year += (int)(offset / 12);
        target.tm_mon += (int)(offset % 12);
        break;
      case 'y':
        target.tm_year += (int)offset;
        break;
      }
    }

    // The TOY itself runs in UTC + toy_offset, so express the target
    // wall-clock as a UTC delta against the host clock.
#ifdef _WIN32
    time_t seeded = _mkgmtime(&target);
#else
    time_t seeded = timegm(&target);
#endif
    if (seeded != (time_t)-1)
      return (long)(seeded - host_now);
  }
  return current;
}
