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
#include "Ds15Dpr.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

namespace {
/// The DS15 console's RMC command mailbox (update utility V7.3, 0x9ccc0 and
/// 0x9cde0): the ES40's layout at 0xf9-0xff moved to 0xb00, with the
/// command's data at 0xb10.
enum {
  MBX_SIZE = 0xb00,
  MBX_QUALIFIER = 0xb02, // two bytes
  MBX_STATUS = 0xb04,    // completion code: 0 done, 0x81 unknown command
  MBX_RESPONSE_ID = 0xb05,
  MBX_COMMAND = 0xb06,
  MBX_COMMAND_ID = 0xb07,
  MBX_DATA = 0xb10,
};
} // namespace

CDs15Dpr::CDs15Dpr(CConfigurator *cfg, CSystem *c) : CDPR(cfg, c) {}

void CDs15Dpr::board_init() {
  // Bytes the DS15 console reads after the ES40's console does (its powerup
  // check at 0x91960 in the update utility reads DPR 0xb8-0xff):
  //  0xb9: nonzero = "RMC detected: DPR Test Failure";
  //  0xba bits 0-2: fault LEDs ("WARNING: - %s Fault LED illuminated"; the
  //        ES40's 0xba is 0xba, "i2c finished", which lights bit 1,
  //        "Over-Temperature");
  //  0xca: nonzero = errors listed from a table;
  //  0xdb: 0xdb when the RMC found the TIG good ("RMC detected: TIG Status
  //        Failure" otherwise; the ES40 keeps a power supply's ID there).
  state.ram[0xb9] = 0;
  state.ram[0xba] = 0;
  state.ram[0xca] = 0;
  state.ram[0xdb] = 0xdb;
}

void CDs15Dpr::WriteMem(int index, u64 address, int dsize, u64 data) {
  CDPR::WriteMem(index, address, dsize, data);
  if ((address >> 6) != MBX_COMMAND_ID)
    return;
  // A new command id: carry the command out and answer with the same id.
  // The commands are the ES40 RMC's (1 update an EEPROM, 2 baud rate, 3
  // operator panel text) and command 9, which the ES45 console also sends
  // (two data bytes here, one there); they are accepted without effect
  // [values chosen to get past "RMC command failure": the DS15's RMC is not
  // modelled further].
  switch (state.ram[MBX_COMMAND]) {
  case 1:
  case 2:
  case 3:
  case 9:
    state.ram[MBX_STATUS] = 0;
    break;
  default:
    state.ram[MBX_STATUS] = 0x81;
    break;
  }
  state.ram[MBX_RESPONSE_ID] = state.ram[MBX_COMMAND_ID];
}
