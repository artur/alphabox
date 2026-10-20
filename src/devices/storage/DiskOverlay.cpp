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
 * A copy-on-write overlay over a disk image: see DiskOverlay.hpp.
 **/

#include "DiskOverlay.hpp"
#include <algorithm>
#include <cerrno>
#include <cstring>

namespace {

const char kMagic[8] = {'A', 'B', 'X', 'C', 'O', 'W', '0', '1'};
const u32 kVersion = 1;
const off_t_large kMapStart = 4096;

void put32(u8 *p, u32 v) {
  for (int i = 0; i < 4; i++)
    p[i] = u8(v >> (8 * i));
}
void put64(u8 *p, u64 v) {
  for (int i = 0; i < 8; i++)
    p[i] = u8(v >> (8 * i));
}
u32 get32(const u8 *p) {
  return u32(p[0]) | u32(p[1]) << 8 | u32(p[2]) << 16 | u32(p[3]) << 24;
}
u64 get64(const u8 *p) { return u64(get32(p)) | u64(get32(p + 4)) << 32; }

bool read_at(FILE *f, off_t_large pos, void *dest, size_t n) {
  return fseek_large(f, pos, SEEK_SET) == 0 && fread(dest, 1, n, f) == n;
}
bool write_at(FILE *f, off_t_large pos, const void *src, size_t n) {
  return fseek_large(f, pos, SEEK_SET) == 0 && fwrite(src, 1, n, f) == n;
}

/// FNV-1a over the base's first and last cluster: enough to tell one base
/// image from another without reading all of it.
bool base_tag(FILE *base, off_t_large size, u64 &tag) {
  std::vector<u8> buf(CDiskOverlay::kCluster);
  u64 h = 0xcbf29ce484222325ull;
  const off_t_large last = size > (off_t_large)CDiskOverlay::kCluster
                               ? size - CDiskOverlay::kCluster
                               : 0;
  for (off_t_large at : {(off_t_large)0, last}) {
    const size_t n = (size_t)std::min<off_t_large>(size - at, buf.size());
    if (!read_at(base, at, buf.data(), n))
      return false;
    for (size_t i = 0; i < n; i++)
      h = (h ^ buf[i]) * 0x100000001b3ull;
  }
  tag = h;
  return true;
}

} // namespace

std::unique_ptr<CDiskOverlay>
CDiskOverlay::open(FILE *base, off_t_large base_size, const std::string &path,
                   bool discard, std::string &error) {
  std::unique_ptr<CDiskOverlay> o(new CDiskOverlay());
  o->base = base;
  o->base_size = base_size;
  o->file_path = path;
  const u32 clusters = u32((base_size + kCluster - 1) / kCluster);
  o->map.assign(clusters, 0);
  o->data_start = (kMapStart + (off_t_large)clusters * 4 + kCluster - 1) /
                  kCluster * kCluster;
  u64 tag;
  if (!base_tag(base, base_size, tag)) {
    error = "cannot read the base image";
    return nullptr;
  }

  u8 hdr[40];
  o->f = fopen_large(path.c_str(), "rb+");
  if (o->f) {
    if (!read_at(o->f, 0, hdr, sizeof(hdr)) || memcmp(hdr, kMagic, 8) != 0 ||
        get32(hdr + 8) != kVersion || get32(hdr + 12) != kCluster) {
      error = path + " is not an Alphabox overlay";
      return nullptr;
    }
    if ((off_t_large)get64(hdr + 16) != base_size || get64(hdr + 24) != tag ||
        get32(hdr + 32) != clusters) {
      error = path + " was made over another base image";
      return nullptr;
    }
    std::vector<u8> raw((size_t)clusters * 4);
    if (!read_at(o->f, kMapStart, raw.data(), raw.size())) {
      error = path + ": cluster map cut short";
      return nullptr;
    }
    for (u32 c = 0; c < clusters; c++) {
      o->map[c] = get32(&raw[(size_t)c * 4]);
      o->used = std::max(o->used, o->map[c]);
    }
  } else {
    if (errno != ENOENT) {
      error = "cannot open " + path + ": " + strerror(errno);
      return nullptr;
    }
    o->f = fopen_large(path.c_str(), "wb+");
    if (!o->f) {
      error = "cannot create " + path + ": " + strerror(errno);
      return nullptr;
    }
    std::vector<u8> head((size_t)(o->data_start), 0);
    memcpy(head.data(), kMagic, 8);
    put32(&head[8], kVersion);
    put32(&head[12], kCluster);
    put64(&head[16], (u64)base_size);
    put64(&head[24], tag);
    put32(&head[32], clusters);
    if (!write_at(o->f, 0, head.data(), head.size()) || fflush(o->f) != 0) {
      error = "cannot write " + path + ": " + strerror(errno);
      return nullptr;
    }
  }
  o->discard = discard;
  return o;
}

CDiskOverlay::~CDiskOverlay() {
  if (f)
    fclose(f);
  if (discard)
    remove(file_path.c_str());
}

/**
 * Give \a cluster a slot of its own, holding the base's bytes: the copy
 * first, then the map entry.
 **/
bool CDiskOverlay::claim(u32 cluster) {
  std::vector<u8> buf(kCluster, 0);
  const off_t_large at = (off_t_large)cluster * kCluster;
  const size_t n = (size_t)std::min<off_t_large>(base_size - at, kCluster);
  if (!read_at(base, at, buf.data(), n))
    return false;
  const u32 slot = used;
  if (!write_at(f, slot_offset(slot), buf.data(), kCluster))
    return false;
  u8 entry[4];
  put32(entry, slot + 1);
  if (!write_at(f, kMapStart + (off_t_large)cluster * 4, entry, 4))
    return false;
  map[cluster] = slot + 1;
  used = slot + 1;
  return true;
}

size_t CDiskOverlay::read(off_t_large pos, void *dest, size_t n) {
  u8 *out = static_cast<u8 *>(dest);
  size_t done = 0;
  while (done < n && pos < base_size) {
    const u32 c = u32(pos / kCluster);
    const u32 off = u32(pos % kCluster);
    const size_t len = (size_t)std::min<off_t_large>(
        std::min<size_t>(n - done, kCluster - off), base_size - pos);
    const bool ok =
        map[c] ? read_at(f, slot_offset(map[c] - 1) + off, out + done, len)
               : read_at(base, pos, out + done, len);
    if (!ok)
      break;
    done += len;
    pos += len;
  }
  return done;
}

size_t CDiskOverlay::write(off_t_large pos, const void *src, size_t n) {
  const u8 *in = static_cast<const u8 *>(src);
  size_t done = 0;
  while (done < n && pos < base_size) {
    const u32 c = u32(pos / kCluster);
    const u32 off = u32(pos % kCluster);
    const size_t len = (size_t)std::min<off_t_large>(
        std::min<size_t>(n - done, kCluster - off), base_size - pos);
    if (!map[c] && !claim(c))
      break;
    if (!write_at(f, slot_offset(map[c] - 1) + off, in + done, len))
      break;
    done += len;
    pos += len;
  }
  return done;
}

void CDiskOverlay::flush() { fflush(f); }
