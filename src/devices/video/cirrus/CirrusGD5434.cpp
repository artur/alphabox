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
 * CL-GD5434 identity.
 **/

#include "CirrusGD5434.hpp"

using namespace cirrus;

static const cirrus_chip_config gd5434_config = {
    "CL-GD5434",       // part
    CHIP_GD5434,       // CR27
    PCI_DEVICE_GD5434, // PCI device id
    4u << 20,          // 4 MB VRAM
    16u << 20,         // 16 MB BAR0
    0x00,              // PCI revision
    0x98,              // SR0F: 64-bit DRAM bus, 4 MB
    0x20,              // SR17: PCI bus straps
    0x2d,              // SR1F: MCLK
    "gd5434.bin",      // default option ROM
};

CCirrusGD5434::CCirrusGD5434(CConfigurator *cfg, CSystem *c, int pcibus,
                             int pcidev)
    : CCirrusGD54xx(cfg, c, pcibus, pcidev, gd5434_config) {}
