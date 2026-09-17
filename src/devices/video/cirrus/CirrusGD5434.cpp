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

/**
 * \file
 * CL-GD5434 identity: chip id, straps and PCI header.
 **/

#include "CirrusGD5434.hpp"

using namespace cirrus;

static const cirrus_chip_config gd5434_config = {
    "CL-GD5434",       // part
    CHIP_GD5434,       // CR27
    PCI_DEVICE_GD5434, // PCI device id
    4u << 20,          // 4 MB VRAM
    32u << 20,         // 32 MB BAR0
    0x98,              // SR0F: 2 MB per bank, second bank fitted (4 MB)
    0x20,              // SR17: PCI bus straps
    0x2d,              // SR1F: MCLK
    "gd5434.bin",      // default option ROM
};

/** PCI configuration space: power-on values */
static u32 gd5434_cfg_data[64] = {
    /*00*/ (u32(PCI_DEVICE_GD5434) << 16) | PCI_VENDOR_CIRRUS,
    /*04*/ 0x02000000, // status: medium DEVSEL
    /*08*/ 0x03000000, // class: VGA display controller, revision 0
    /*0c*/ 0x00000000,
    /*10*/ 0x00000008, // BAR0: 32 MB prefetchable memory (linear aperture)
    /*14*/ 0x00000000,
    /*18*/ 0x00000000,
    /*1c*/ 0x00000000,
    /*20*/ 0x00000000,
    /*24*/ 0x00000000,
    /*28*/ 0x00000000,
    /*2c*/ 0x00000000,
    /*30*/ 0x00000000, // no expansion ROM BAR: the BIOS is at 0xc0000
    /*34*/ 0x00000000,
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff, // no interrupt pin
};

/** PCI configuration space: writable bits */
static u32 gd5434_cfg_mask[64] = {
    /*00*/ 0x00000000,
    /*04*/ 0x0000ffff,
    /*08*/ 0x00000000,
    /*0c*/ 0x0000ffff,
    /*10*/ 0xfe000000,
    /*14*/ 0x00000000,
    /*18*/ 0x00000000,
    /*1c*/ 0x00000000,
    /*20*/ 0x00000000,
    /*24*/ 0x00000000,
    /*28*/ 0x00000000,
    /*2c*/ 0x00000000,
    /*30*/ 0x00000000,
    /*34*/ 0x00000000,
    /*38*/ 0x00000000,
    /*3c*/ 0x000000ff,
};

CCirrusGD5434::CCirrusGD5434(CConfigurator *cfg, CSystem *c, int pcibus,
                             int pcidev)
    : CCirrusGD54xx(cfg, c, pcibus, pcidev, gd5434_config, gd5434_cfg_data,
                    gd5434_cfg_mask) {}
