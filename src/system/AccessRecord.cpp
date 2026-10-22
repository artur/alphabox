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

#include "AccessRecord.hpp"
#include "AlphaCPU.hpp"
#include "StdAfx.hpp"

#include <map>
#include <mutex>
#include <set>
#include <string>

namespace access_record {

static bool record_selected() {
  const char *v = getenv("ALPHABOX_RECORD");
  return v && *v;
}

const bool g_on = record_selected();

namespace {

std::set<std::string> &tokens() {
  static std::set<std::string> t = [] {
    std::set<std::string> s;
    const char *v = getenv("ALPHABOX_RECORD");
    std::string all = v ? v : "";
    size_t at = 0;
    while (at <= all.size()) {
      size_t end = all.find(',', at);
      if (end == std::string::npos)
        end = all.size();
      if (end > at)
        s.insert(all.substr(at, end - at));
      at = end + 1;
    }
    return s;
  }();
  return t;
}

/// The last eight records of one device and processor, to see a loop in.
struct Stream {
  std::string hist[8];
  unsigned n = 0;
  const std::string &back(unsigned j) const { return hist[(n - j) % 8]; }
};

/// Whether r continues a loop of period k <= 4: the record k back is r, and
/// the k - 1 records since then match the k - 1 before it.
bool repeats(const Stream &s, const std::string &r) {
  for (unsigned k = 1; k <= 4 && s.n >= 2 * k - 1; k++) {
    if (s.back(k) != r)
      continue;
    bool loop = true;
    for (unsigned j = 1; j < k && loop; j++)
      loop = s.back(j) == s.back(j + k);
    if (loop)
      return true;
  }
  return false;
}

struct Recorder {
  std::mutex m;
  FILE *f = nullptr;
  bool raw = false;
  std::map<std::string, Stream> streams;
  u64 written = 0;
  u64 limit = 256 << 20; // bytes; the host's disk is shared
  bool full = false;

  Recorder() {
    const char *path = getenv("ALPHABOX_RECORD_FILE");
    if (!path || !*path)
      path = "alphabox-record.txt";
    if (const char *mb = getenv("ALPHABOX_RECORD_MAX_MB"))
      limit = strtoull(mb, nullptr, 10) << 20;
    f = fopen(path, "w");
    if (!f) {
      printf("%%REC-W-OPEN: cannot write the access record to %s\n", path);
      return;
    }
    setvbuf(f, nullptr, _IOFBF, 1 << 20);
    raw = tokens().count("raw") != 0;
    atexit([] { flush(); });
  }

  /// Never destroyed: the flush at exit runs after static destructors.
  static Recorder &get() {
    static Recorder *r = new Recorder;
    return *r;
  }

  static void flush() {
    Recorder &r = get();
    std::lock_guard<std::mutex> g(r.m);
    if (r.f)
      fflush(r.f);
  }
};

} // namespace

bool selected(const char *name) {
  if (!g_on || !name)
    return false;
  const std::set<std::string> &t = tokens();
  const std::string s = name;
  if (t.count(s))
    return true;
  const size_t open = s.find('(');
  if (open == std::string::npos || s.back() != ')')
    return false;
  return t.count(s.substr(0, open)) ||
         t.count(s.substr(open + 1, s.size() - open - 2));
}

void note(const char *device, const char *space, u64 address, int bits,
          bool write, u64 value) {
  Recorder &r = Recorder::get();
  if (!r.f)
    return;
  char who[16] = "dev";
  if (t_running_cpu)
    snprintf(who, sizeof(who), "cpu%d", t_running_cpu->get_cpuid());
  char body[96];
  snprintf(body, sizeof(body), "%c %s %" PRIx64 " %d %" PRIx64,
           write ? 'w' : 'r', space, address, bits, value);

  std::lock_guard<std::mutex> g(r.m);
  if (r.full)
    return;
  Stream &s = r.streams[std::string(device) + " " + who];
  const bool drop = !r.raw && repeats(s, body);
  s.hist[s.n++ % 8] = body;
  if (drop)
    return;
  const int n = fprintf(r.f, "%s %s %s\n", device, who, body);
  r.written += n > 0 ? (u64)n : 0;
  if (r.written >= r.limit) {
    r.full = true;
    fprintf(r.f,
            "# stopped at the size limit, %" PRIu64
            " MB (ALPHABOX_RECORD_MAX_MB)\n",
            r.limit >> 20);
    printf("%%REC-W-FULL: the access record reached %" PRIu64
           " MB and stopped (ALPHABOX_RECORD_MAX_MB)\n",
           r.limit >> 20);
  }
}

void note(const char *device, int index, u64 address, int bits, bool write,
          u64 value) {
  char space[16];
  snprintf(space, sizeof(space), "%d", index);
  note(device, space, address, bits, write, value);
}

} // namespace access_record
