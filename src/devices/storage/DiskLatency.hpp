/* Alphabox Alpha Emulator
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
 */

#if !defined(INCLUDED_DISKLATENCY_H)
#define INCLUDED_DISKLATENCY_H

#include <cstdint>
#include <cstdio>
#include <cstdlib>

/**
 * \brief How long a mechanical disk takes to serve an access (off by default).
 *
 * An image file answers in microseconds; a 1999 disk took milliseconds, and
 * a guest thread that waits for one gives the others the processor. The
 * model is deliberately small:
 *
 *   time = command_us
 *        + access_us   if the access does not start where the last one ended
 *        + bytes / rate
 *
 * access_us stands for the average seek plus half a rotation; it does not
 * grow with the distance. A drive's read-ahead and write cache are not
 * modelled beyond the sequential case being cheap.
 *
 * Configured per disk (latency.access_us, latency.command_us,
 * latency.mb_per_s), or for every hard disk from the environment:
 * ALPHABOX_DISK_LATENCY_US=<access>[,<command>[,<MB/s>]], which takes
 * precedence (command defaults to 300 us and the rate to 20 MB/s there).
 * ALPHABOX_DISK_LATENCY_US=0 turns the model off whatever the configuration
 * says. The controller, not this class, decides when the time is spent: on
 * its own thread, before it completes the command, never on a CPU thread.
 **/
class CDiskLatency {
public:
  void configure(uint64_t access_us, uint64_t command_us, uint64_t mb_per_s) {
    access = access_us;
    command = command_us;
    bytes_per_us = mb_per_s; // 1 MB/s = 1 byte/us (decimal megabytes)
  }

  /// Apply ALPHABOX_DISK_LATENCY_US if it is set; true if it was.
  bool configure_from_env() {
    const char *e = getenv("ALPHABOX_DISK_LATENCY_US");
    if (!e || !*e)
      return false;
    unsigned long long a = 0, c = 300, r = 20;
    int n = sscanf(e, "%llu,%llu,%llu", &a, &c, &r);
    if (n < 1 || a == 0)
      configure(0, 0, 0);
    else
      configure(a, c, r);
    return true;
  }

  bool enabled() const { return access || command || bytes_per_us; }

  /**
   * Service time of an access of \a blocks blocks at \a lba, in
   * microseconds; moves the head to its end. \a new_command is false for a
   * later block of a command already charged its overhead.
   **/
  uint64_t service_us(uint64_t lba, uint64_t blocks, uint64_t block_size,
                      bool new_command) {
    if (!enabled())
      return 0;
    uint64_t t = new_command ? command : 0;
    if (lba != next_lba)
      t += access;
    if (bytes_per_us)
      t += blocks * block_size / bytes_per_us;
    next_lba = lba + blocks;
    return t;
  }

  uint64_t access_us() const { return access; }
  uint64_t command_us() const { return command; }
  uint64_t mb_per_s() const { return bytes_per_us; }

private:
  uint64_t access = 0;
  uint64_t command = 0;
  uint64_t bytes_per_us = 0;
  uint64_t next_lba = ~uint64_t(0);
};

#endif
