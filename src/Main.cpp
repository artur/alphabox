/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2020 Remy van Elst
 * Website: https://github.com/lenticularis39/axpbox
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 */

#include "config.hpp"
#include <cstdio>
#include <cstring>
#include <iostream>

int main_sim(int argc, char *argv[]);
int main_cfg(int argc, char *argv[]);

/**
 * Print the version, the commit and the optional features compiled in.
 **/
static void print_version() {
#ifdef VERSION
  printf("Alphabox %s", VERSION);
#else
  printf("Alphabox (unknown version)");
#endif
#ifdef PACKAGE_GITSHA
  printf(" (commit %s)", PACKAGE_GITSHA);
#endif
  printf("\nFeatures:");
  int features = 0;
#if defined(ES40_JIT)
#if defined(__aarch64__) || defined(_M_ARM64)
  printf(" JIT(AArch64)");
#else
  printf(" JIT(x86-64)");
#endif
  features++;
#endif
#if defined(HAVE_SDL3)
  printf(" SDL3");
  features++;
#endif
#if defined(HAVE_PCAP)
  printf(" PCap");
  features++;
#endif
#if defined(__linux__)
  printf(" TAP");
  features++;
#endif
  printf("%s\n", features ? "" : " (none)");
}

int main(int argc, char **argv) {
  if (argc == 2 &&
      (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0)) {
    print_version();
    return 0;
  }

  if (argc <= 1 || (strcmp(argv[1], "run") && strcmp(argv[1], "configure"))) {
    std::cerr << "Alphabox Alpha Emulator";
#ifdef PACKAGE_GITSHA
    std::cerr << " (commit " << std::string(PACKAGE_GITSHA) << ")";
#endif
    std::cerr << std::endl;
    std::cerr << "Usage: " << argv[0] << " run|configure <options>" << std::endl;
    std::cerr << "       " << argv[0] << " --version" << std::endl;
    return 0;
  }

  if (strcmp(argv[1], "run") == 0) {
    return main_sim(argc - 1, ++argv);
  }

  if (strcmp(argv[1], "configure") == 0) {
    return main_cfg(argc - 1, ++argv);
  }
}
