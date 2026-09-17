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
 * 93C46/93C66 Microwire serial EEPROM, driven pin by pin.
 **/
#if !defined(INCLUDED_EEPROM93CX6_H_)
#define INCLUDED_EEPROM93CX6_H_

#include "StdAfx.hpp"

/**
 * \brief A 16-bit-organized Microwire EEPROM (93C46: 64 words, 93C66: 256).
 *
 * The host drives chip select, clock and data in; data out changes after
 * rising clock edges. Plain data (no pointers), so the owning device can
 * keep it in its state file structure.
 *
 * Data out reads 1 while the address is being clocked in and 0 (the dummy
 * bit) once the last address bit arrived; drivers such as Linux e100 size
 * the address from that transition.
 **/
struct CEeprom93cx6 {
  static constexpr int MAX_WORDS = 256;

  void init(int address_bits);
  void set_pins(bool cs, bool sk, bool di);
  bool data_out() const { return dout; }

  u16 data[MAX_WORDS];
  int abits;

private:
  enum phase_t { IDLE, START, OPCODE, ADDRESS, READING, WRITING };
  void command_complete();

  u8 phase;
  bool cs_prev;
  bool sk_prev;
  bool dout;
  bool write_enabled;
  u8 opcode;
  int bits;  // bits collected in the current phase
  u32 shift; // bits collected, MSB first
  u16 address;
  u16 out_word; // word being shifted out
};

#endif // !defined(INCLUDED_EEPROM93CX6_H_)
