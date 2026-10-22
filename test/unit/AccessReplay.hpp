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

/* An ALPHABOX_RECORD file read back for a unit test to replay
 * (src/system/AccessRecord.hpp): the test feeds the writes to the device and
 * checks that every read answers what the recorded run was told. Record with
 * "raw" in the list, so no access is left out. Lines starting with '#' are
 * comments, for where the record came from.
 */
#if !defined(INCLUDED_ACCESSREPLAY_H)
#define INCLUDED_ACCESSREPLAY_H

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

struct RecordedAccess {
  std::string who;   ///< cpuN, or dev
  bool write;        ///< else a read
  std::string space; ///< range index, f<n> or a chip
  uint64_t address;  ///< offset into the range
  int bits;
  uint64_t value; ///< written, or what the read returned
  int line;       ///< in the file, for messages
};

/// The accesses to `device` in the record at `path`, in order. Empty (and
/// a message) if the file cannot be read.
inline std::vector<RecordedAccess> load_record(const char *path,
                                               const char *device) {
  std::vector<RecordedAccess> out;
  FILE *f = fopen(path, "r");
  if (!f) {
    printf("cannot read the record %s\n", path);
    return out;
  }
  char buf[512];
  int line = 0;
  while (fgets(buf, sizeof(buf), f)) {
    line++;
    if (buf[0] == '#')
      continue;
    char dev[128], who[32], op[4], space[32];
    uint64_t address, value;
    int bits;
    if (sscanf(buf, "%127s %31s %3s %31s %" SCNx64 " %d %" SCNx64, dev, who, op,
               space, &address, &bits, &value) != 7 ||
        strcmp(dev, device) != 0)
      continue;
    out.push_back({who, op[0] == 'w', space, address, bits, value, line});
  }
  fclose(f);
  return out;
}

#endif // !defined(INCLUDED_ACCESSREPLAY_H)
