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

/* Unit test of the start hand-off through an EV7's RBOX_SCRATCH1
 * (src/chipsets/marvel/StartHandoff.hpp), without the console: two host
 * threads play the console's start_secondary (write 0xf1, read back at most
 * 50 times for the 0xf2 echo, write 0xf3: SRM V7.3-1 at 0x2dd600) and the
 * secondary's PALcode loop (poll for 0xf1, echo 0xf2, poll for 0xf3: 0x390e4).
 * The secondary is made slow, as a host thread delayed by the host is: a
 * pause between its polls far longer than the console's 50 reads take. With
 * the hand-off's order the start is taken every time; without it (the
 * control) the console gives up, as it did on four-processor Marvel layouts
 * after `init`. Built with every lane as `start_handoff_test`; exit status 0
 * when every check passes.
 */

#include "StartHandoff.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

static int checks = 0, failures = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                   \
    }                                                                          \
  } while (0)

using namespace std::chrono;

/// One EV7's RBOX_SCRATCH1 and its lock, as CEv7Csr holds them. `progress`
/// stands for the owner's instruction count.
struct Scratch {
  std::mutex lock;
  uint64_t value = 0;
  uint64_t progress = 0;
  CStartHandoff handoff;
  bool with_handoff = true;
  explicit Scratch(milliseconds bound = milliseconds(1000)) : handoff(bound) {}

  uint64_t owner_read() {
    std::lock_guard<std::mutex> g(lock);
    progress += 6; // a turn of the PALcode's loop
    if (with_handoff)
      handoff.owner_read(value, progress);
    return value;
  }
  void owner_write(uint64_t v) {
    std::lock_guard<std::mutex> g(lock);
    value = v;
    if (with_handoff)
      handoff.owner_wrote();
  }
  uint64_t remote_read(int reader, bool *in_time = nullptr) {
    std::unique_lock<std::mutex> lk(lock);
    bool ok = true;
    if (with_handoff)
      ok = handoff.remote_read(lk, reader, [] { return true; });
    if (in_time)
      *in_time = ok;
    return value;
  }
  void remote_write(uint64_t v) {
    std::lock_guard<std::mutex> g(lock);
    value = v;
    if (with_handoff)
      handoff.remote_wrote(progress);
  }
};

static uint32_t tag(uint64_t v) { return (uint32_t)(v >> 24) & 0xff; }

/// The secondary's PALcode loop, `pause` between polls and a `nap` once
/// after its second (a host thread the host delays). Returns the address it
/// was given, or 0 when told to stop.
static uint64_t secondary(Scratch &s, microseconds pause, microseconds nap,
                          std::atomic<int> &polls, std::atomic<bool> &stop) {
  uint64_t hi = 0;
  for (;;) {
    if (stop)
      return 0;
    const uint64_t v = s.owner_read();
    if (tag(v) == 0xf1) {
      hi = v & 0xffffff;
      s.owner_write((uint64_t(0xf2) << 24) | hi);
    } else if (tag(v) == 0xf3) {
      return (hi << 24) | (v & 0xffffff);
    }
    if (++polls == 2 && nap.count())
      std::this_thread::sleep_for(nap);
    else if (pause.count())
      std::this_thread::sleep_for(pause);
  }
}

/// The console's start_secondary: true if the echo came within its 50 reads.
static bool start(Scratch &s, uint64_t addr) {
  s.remote_write((uint64_t(0xf1) << 24) | ((addr >> 24) & 0xffffff));
  int n = 0;
  while (n < 50 && tag(s.remote_read(0)) != 0xf2)
    n++;
  if (n == 50)
    return false;
  s.remote_write((uint64_t(0xf3) << 24) | (addr & 0xffffff));
  return true;
}

/// A start of a secondary that pauses `pause` between polls and naps `nap`
/// once, the console writing while it naps; the hand-off on or off.
static bool run(bool handoff, microseconds pause, microseconds nap,
                uint64_t *got = nullptr) {
  Scratch s;
  s.with_handoff = handoff;
  std::atomic<bool> stop{false};
  std::atomic<int> polls{0};
  uint64_t taken = 0;
  std::thread t([&] { taken = secondary(s, pause, nap, polls, stop); });
  while (polls < 2) // polling: its loop has gone round twice
    std::this_thread::sleep_for(microseconds(50));
  const uint64_t addr = 0x800030000ULL;
  const bool ok = start(s, addr);
  if (!ok)
    stop = true;
  t.join();
  if (got)
    *got = taken;
  return ok;
}

int main() {
  // A secondary the host holds up for 200 ms just as the console writes,
  // and one pausing 2 ms between polls: the console's 50 reads take
  // microseconds. With the hand-off every start is taken, at the address.
  for (int i = 0; i < 5; i++) {
    uint64_t got = 0;
    CHECK(run(true, microseconds(0), milliseconds(200), &got));
    CHECK(got == 0x800030000ULL);
  }
  for (int i = 0; i < 10; i++) {
    uint64_t got = 0;
    CHECK(run(true, microseconds(2000), microseconds(0), &got));
    CHECK(got == 0x800030000ULL);
  }
  // A secondary polling flat out, the common case: also taken.
  for (int i = 0; i < 20; i++)
    CHECK(run(true, microseconds(0), microseconds(0)));
  // The control: the held-up secondary without the hand-off loses.
  int lost = 0;
  for (int i = 0; i < 3; i++)
    lost += run(false, microseconds(0), milliseconds(200)) ? 0 : 1;
  CHECK(lost == 3);

  // An owner that is not polling opens no hand-off: a read does not wait.
  {
    Scratch s;
    s.remote_write(0xf1000800);
    const auto t0 = steady_clock::now();
    bool in_time = false;
    CHECK(s.remote_read(0, &in_time) == 0xf1000800);
    CHECK(in_time);
    CHECK(steady_clock::now() - t0 < milliseconds(50));
  }
  // An owner that read twice and then ran on elsewhere is not polling.
  {
    Scratch s;
    s.owner_read();
    s.owner_read();
    s.progress += 2000000; // its instructions since: far from a loop's turn
    s.remote_write(0xf1000800);
    const auto t0 = steady_clock::now();
    bool in_time = false;
    s.remote_read(0, &in_time);
    s.remote_read(0, &in_time);
    CHECK(in_time);
    CHECK(steady_clock::now() - t0 < milliseconds(50));
  }
  // An owner that polled and then stopped looking holds a reader for the
  // bound in all, not once per read.
  {
    Scratch s(milliseconds(100));
    s.owner_read();
    s.owner_read(); // polling
    s.remote_write(0xf1000800);
    const auto t0 = steady_clock::now();
    bool in_time = true;
    s.remote_read(0, &in_time);
    CHECK(!in_time);
    for (int i = 0; i < 49; i++)
      s.remote_read(0, &in_time);
    CHECK(in_time); // the time was up: the later reads did not wait
    const auto took = steady_clock::now() - t0;
    CHECK(took >= milliseconds(100));
    CHECK(took < milliseconds(1000));
  }
  // An owner that looked once and went on (the 0xf3): a later reader waits
  // for the bound and no more, and it does not count as never looked.
  {
    Scratch s(milliseconds(100));
    s.owner_read();
    s.owner_read();
    s.remote_write(0xf3030000);
    CHECK(s.owner_read() == 0xf3030000); // taken: it leaves its loop
    bool in_time = false;
    const auto t0 = steady_clock::now();
    s.remote_read(0, &in_time); // the owner has looked: no wait
    CHECK(in_time);
    CHECK(steady_clock::now() - t0 < milliseconds(50));
    s.remote_read(0, &in_time);
    CHECK(in_time);
    CHECK(steady_clock::now() - t0 < milliseconds(1000));
  }
  // The owner's answer releases a waiting reader at once.
  {
    Scratch s;
    s.owner_read();
    s.owner_read();
    s.remote_write(0xf1000800);
    std::thread owner([&] {
      std::this_thread::sleep_for(milliseconds(20));
      s.owner_write(0xf2000800);
    });
    bool in_time = false;
    const uint64_t v = s.remote_read(0, &in_time);
    owner.join();
    CHECK(in_time);
    CHECK(v == 0xf2000800);
  }

  printf("start_handoff: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
