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
 * Intel 82555 10/100 PHY, as the MII management interface sees it.
 **/
#if !defined(INCLUDED_I8255XPHY_H_)
#define INCLUDED_I8255XPHY_H_

#include "StdAfx.hpp"

/**
 * \brief The 82555 PHY that sits next to the 82558 on the DE600 and Intel
 * PRO/100+ boards (the 82559 carries the same PHY on-chip), always linked
 * at 100 Mbit/s full duplex after autonegotiation.
 *
 * Plain data, kept in the owning device's state file structure.
 **/
struct CI8255xPhy {
  static constexpr int ADDRESS = 1; ///< MII address, as EEPROM word 6 says

  void reset();
  u16 read(int reg);
  void write(int reg, u16 value);

  /// MII control loopback: transmitted frames come straight back.
  bool loopback() const { return (regs[0] & 0x4000) != 0; }

  u16 regs[32];
};

#endif // !defined(INCLUDED_I8255XPHY_H_)
