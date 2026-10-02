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
 * The chipsets this emulator knows, built by the kind a board row names.
 **/
#include "StdAfx.hpp"

#include "Chipset.hpp"
#include "Marvel.hpp"
#include "Titan.hpp"
#include "Tsunami.hpp"
#ifdef ALPHABOX_HVF
#include "HvRuntime.hpp"
#endif

CChipset *create_chipset(chipset_kind kind, CSystem *sys) {
  switch (kind) {
  case CHIPSET_TSUNAMI:
    return new CTsunami(sys);
  case CHIPSET_MARVEL:
    return new CMarvel(sys);
  case CHIPSET_TITAN:
    return new CTitan(sys);
  }
  FAILURE(Configuration, "unknown chipset");
}

#ifdef ALPHABOX_HVF
// As CSystem's (System.cpp): under ALPHABOX_HV=1 the dispatch loop inside
// the VM reaches the chipset, so it is placed in memory the two share.
void *CChipset::operator new(size_t n) {
  if (hv::enabled()) {
    if (void *p = hv::alloc(n))
      return p;
  }
  return ::operator new(n);
}

void CChipset::operator delete(void *p) noexcept {
  if (hv::enabled())
    return; // the VM's allocator releases everything at exit
  ::operator delete(p);
}
#endif
