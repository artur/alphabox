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

#include "Es45Dpr.hpp"
#include "StdAfx.hpp"

void CEs45Dpr::board_init() {
  // The PCI backplane's FRU EEPROM, which the RMC copies to 0x2900: the
  // console's build_dsrdb reads byte 6 and takes <1:0> as the backplane
  // type -- 1 for the Model 3's, 2 for the Model 2's ten-slot backplane
  // (ES45 V7.3-2 console, 0x8f9d0). The Model 2 is the machine of the
  // owner's guide's show config listing.
  state.ram[0x2906] = 2;

  // RMC firmware revisions, as the firmware CD V7.3's FWREADME.TXT lists
  // them for the ES45 (RMC V2.4) [the on-chip and flash code are given the
  // same revision: assumed].
  state.ram[0x3009] = 'V';
  state.ram[0x300a] = '2';
  state.ram[0x300b] = '4';
  state.ram[0x300c] = 'V';
  state.ram[0x300d] = '2';
  state.ram[0x300e] = '4';
}

bool CEs45Dpr::board_command(u8 command) {
  switch (command) {
  case 0x09:
    // Sent with one data byte, 0 or 1, from an environment variable's
    // write action (the console tests whether its value begins with 'F',
    // 0x51f40). What the RMC does with it is not known; it is accepted
    // [a value chosen to get past the console's "RMC command failure"].
    return true;
  default:
    return false;
  }
}
