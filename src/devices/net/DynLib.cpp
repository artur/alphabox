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

#include "StdAfx.hpp"

#if !defined(_WIN32)
#include "DynLib.hpp"

#include <dlfcn.h>
#include <string>
#include <vector>

void *dynlib_open(const char *const *names, const char *const *dirs,
                  const char *fallback_dir) {
  // The soname first, so the system's own search (and any rpath or
  // LD_LIBRARY_PATH) decides; then the usual package-manager directories;
  // then where pkg-config found the library when this binary was built.
  for (const char *const *n = names; *n; n++)
    if (void *h = dlopen(*n, RTLD_NOW | RTLD_LOCAL))
      return h;
  std::vector<std::string> dir_list;
  for (const char *const *d = dirs; d && *d; d++)
    dir_list.push_back(*d);
  if (fallback_dir && *fallback_dir)
    dir_list.push_back(fallback_dir);
  for (const std::string &dir : dir_list)
    for (const char *const *n = names; *n; n++)
      if (void *h = dlopen((dir + "/" + *n).c_str(), RTLD_NOW | RTLD_LOCAL))
        return h;
  return nullptr;
}

void *dynlib_sym(void *handle, const char *name) {
  return dlsym(handle, name);
}
#endif
