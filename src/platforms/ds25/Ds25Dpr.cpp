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
#include "Ds25Dpr.hpp"
#include "StdAfx.hpp"

void CDs25Dpr::board_init() {
  // The SROM's version, which the SROM leaves here: the DS25's is V1.4-G
  // (FWREADME.TXT, and what the update utility installs) [the SROM's own
  // spelling of it is not known; the update utility's is used].
  const char srom[9] = "V1.4-G";
  for (int i = 0; i < 9; i++)
    state.ram[0x3000 + i] = srom[i];

  // RMC firmware revisions, as the firmware CD V7.3's FWREADME.TXT lists
  // them for the DS25 (RMC V1.3) [the on-chip and flash code are given the
  // same revision: assumed].
  state.ram[0x3009] = 'V';
  state.ram[0x300a] = '1';
  state.ram[0x300b] = '3';
  state.ram[0x300c] = 'V';
  state.ram[0x300d] = '1';
  state.ram[0x300e] = '3';
}

bool CDs25Dpr::board_command(u8 command) {
  switch (command) {
  case 0x09:
    // The ES45 console's command 9 (one byte from an environment variable's
    // write action); the DS25's update utility sends it the same way. What
    // the RMC does with it is not known; it is accepted [a value chosen to
    // get past "RMC command failure", as on the ES45].
    return true;
  default:
    return false;
  }
}
