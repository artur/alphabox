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

#if !defined(INCLUDED_SAVEFILE_H)
#define INCLUDED_SAVEFILE_H

#include <cstdio>
#include <string>

/**
 * Replace the file at `path` with what `write(FILE *)` puts in it, or leave
 * it as it was. The new contents go to `path` + ".tmp", which is renamed
 * over `path` only once it is complete, so an emulator stopped in the middle
 * of a save -- killed, or out of disk -- leaves the old file behind instead
 * of a truncated one that the next start refuses.
 *
 * `write` returns false on a short write. Returns true when `path` holds the
 * new contents.
 **/
template <typename F> bool save_file(const char *path, F write) {
  const std::string tmp = std::string(path) + ".tmp";
  FILE *f = fopen(tmp.c_str(), "wb");
  if (!f)
    return false;
  bool ok = write(f);
  ok = (fclose(f) == 0) && ok;
#if defined(_WIN32)
  // rename() does not replace an existing file on Windows.
  if (ok)
    remove(path);
#endif
  if (!ok || rename(tmp.c_str(), path) != 0) {
    remove(tmp.c_str());
    return false;
  }
  return true;
}

#endif // !defined(INCLUDED_SAVEFILE_H)
