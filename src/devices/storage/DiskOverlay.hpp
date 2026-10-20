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
 * A copy-on-write overlay over a disk image (`overlay = "<file>"`).
 **/

#if !defined(__DISKOVERLAY_H__)
#define __DISKOVERLAY_H__

#include "StdAfx.hpp"
#include <memory>
#include <string>
#include <vector>

/**
 * \brief The writes of a disk kept in a file of their own, over a base
 * image that is never written.
 *
 * The base is split into 64 KiB clusters. The first write to a cluster
 * copies it from the base into the next free slot of the overlay; every
 * later read and write of that cluster goes to the slot. Clusters never
 * written are read from the base.
 *
 * The overlay file:
 *   - a 4 KiB header: "ABXCOW01", the format version, the cluster size,
 *     the base's size in bytes, a hash of the base's first and last
 *     cluster (an overlay over a different base is refused), the number of
 *     clusters; little-endian;
 *   - the cluster map at 4 KiB: a 32-bit entry per base cluster, 0 for
 *     "in the base", n for slot n - 1;
 *   - the slots, from the first 64 KiB boundary after the map, in the order
 *     the clusters were first written.
 * A first write puts the copied cluster in its slot before the map entry
 * that points at it, so a host crash loses at most the writes in flight.
 **/
class CDiskOverlay {
public:
  static constexpr u32 kCluster = 0x10000;

  /// Open \a path over \a base (opened for reading, \a base_size bytes),
  /// creating it when it does not exist. On failure returns nullptr and
  /// sets \a error. With \a discard the file is removed when closed.
  static std::unique_ptr<CDiskOverlay> open(FILE *base, off_t_large base_size,
                                            const std::string &path,
                                            bool discard, std::string &error);
  ~CDiskOverlay();

  /// Read or write \a n bytes at \a pos (within the base's size); return
  /// how many were moved.
  size_t read(off_t_large pos, void *dest, size_t n);
  size_t write(off_t_large pos, const void *src, size_t n);
  void flush();

  const std::string &path() const { return file_path; }
  u32 clusters_written() const { return used; }

private:
  CDiskOverlay() = default;
  bool claim(u32 cluster);
  off_t_large slot_offset(u32 slot) const {
    return data_start + (off_t_large)slot * kCluster;
  }

  FILE *base = nullptr;
  FILE *f = nullptr;
  off_t_large base_size = 0;
  off_t_large data_start = 0;
  std::vector<u32> map; ///< per cluster: 0 = base, n = slot n - 1
  u32 used = 0;         ///< slots in use
  std::string file_path;
  bool discard = false;
};

#endif // !defined(__DISKOVERLAY_H__)
