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
 * 93C46/93C66 Microwire serial EEPROM.
 *
 * Command format, MSB first: a start bit (1), two opcode bits, then
 * `abits` address bits. Opcodes: 10 read, 01 write, 11 erase, and 00 with
 * the top two address bits selecting 11 write enable, 00 write disable,
 * 10 erase all, 01 write all.
 **/
#include "Eeprom93cx6.hpp"

void CEeprom93cx6::init(int address_bits) {
  abits = address_bits;
  phase = IDLE;
  cs_prev = false;
  sk_prev = false;
  dout = true;
  write_enabled = false;
  opcode = 0;
  bits = 0;
  shift = 0;
  address = 0;
  out_word = 0;
}

void CEeprom93cx6::set_pins(bool cs, bool sk, bool di) {
  const bool rising = sk && !sk_prev;
  sk_prev = sk;

  if (!cs) {
    // Deselect ends the command; a write in progress completes now.
    if (cs_prev && phase == WRITING && bits == 16)
      command_complete();
    cs_prev = false;
    phase = IDLE;
    dout = true; // ready
    return;
  }
  if (!cs_prev) {
    cs_prev = true;
    phase = START;
  }
  if (!rising)
    return;

  switch (phase) {
  case START:
    if (di) {
      phase = OPCODE;
      bits = 0;
      shift = 0;
    }
    break;

  case OPCODE:
    shift = shift << 1 | di;
    if (++bits == 2) {
      opcode = (u8)shift;
      phase = ADDRESS;
      bits = 0;
      shift = 0;
    }
    break;

  case ADDRESS:
    shift = shift << 1 | di;
    if (++bits < abits)
      break;
    address = (u16)shift;
    bits = 0;
    shift = 0;
    if (opcode == 2) { // read: the dummy 0, then the word
      phase = READING;
      out_word = data[address % (1 << abits)];
      dout = false;
    } else if (opcode == 1 || (opcode == 0 && (address >> (abits - 2)) == 1)) {
      phase = WRITING; // write, or write all
    } else {
      command_complete(); // erase, erase all, enable, disable
      phase = IDLE;
    }
    break;

  case READING:
    // Sequential read: the next word follows without a new command.
    dout = (out_word & 0x8000) != 0;
    out_word <<= 1;
    if (++bits == 16) {
      bits = 0;
      address = (u16)((address + 1) % (1 << abits));
      out_word = data[address];
    }
    break;

  case WRITING:
    if (bits < 16) {
      shift = shift << 1 | di;
      bits++;
    }
    break;

  default:
    break;
  }
}

void CEeprom93cx6::command_complete() {
  const int words = 1 << abits;
  const int ext = address >> (abits - 2); // opcode 00 sub-command

  switch (opcode) {
  case 0:
    if (ext == 3)
      write_enabled = true;
    else if (ext == 0)
      write_enabled = false;
    else if (write_enabled)
      for (int i = 0; i < words; i++)
        data[i] = ext == 2 ? 0xffff : (u16)shift;
    break;
  case 1:
    if (write_enabled)
      data[address % words] = (u16)shift;
    break;
  case 3:
    if (write_enabled)
      data[address % words] = 0xffff;
    break;
  }
}
