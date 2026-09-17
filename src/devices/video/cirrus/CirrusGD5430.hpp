/* AXPbox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/axpbox
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

/* Cirrus Logic CL-GD5430 ("Alpine"), PCI, 2 MB -- one of the two Cirrus
 * parts the ES40 SRM console names. */

#if !defined(INCLUDED_CIRRUS_GD5430_H)
#define INCLUDED_CIRRUS_GD5430_H

#include "CirrusGD54xx.hpp"

class CCirrusGD5430 : public CCirrusGD54xx {
public:
  CCirrusGD5430(CConfigurator *cfg, class CSystem *c, int pcibus, int pcidev);
};

#endif // !defined(INCLUDED_CIRRUS_GD5430_H)
