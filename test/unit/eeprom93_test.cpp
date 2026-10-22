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

/* Unit test of the Microwire EEPROM (src/devices/common/Eeprom93cx6.cpp),
 * 93C46 and 93C66 in x16 organization, driven pin by pin as the 8255x NIC,
 * the ISP adapters and the Mach64 drivers do, against the 93C46 data sheet
 * (Microchip DS21749F: Table 1-3, PDF p. 5; 2.7 Read). Built with every lane
 * as `eeprom93_test`; exit status 0 when every check passes.
 */

#include "Eeprom93cx6.hpp"

#include <cstdio>

static int checks = 0, failures = 0;

static void check(const char *what, unsigned got, unsigned want) {
  checks++;
  if (got == want)
    return;
  failures++;
  printf("FAIL %s: got %04x, want %04x\n", what, got, want);
}

static CEeprom93cx6 e;

static void clk(bool di) {
  e.set_pins(true, false, di);
  e.set_pins(true, true, di); // rising edge
}
static void select() {
  e.set_pins(false, false, false);
  e.set_pins(true, false, false);
}
static void deselect() { e.set_pins(false, false, false); }
static void bits(unsigned v, int n) {
  for (int i = n - 1; i >= 0; i--)
    clk((v >> i) & 1);
}
/// Start bit, opcode, address: the opening of every instruction.
static void instruction(unsigned opcode, unsigned address) {
  select();
  clk(1);
  bits(opcode, 2);
  bits(address, e.abits);
}
static unsigned word() {
  unsigned w = 0;
  for (int i = 0; i < 16; i++) {
    clk(0);
    w = w << 1 | (e.data_out() ? 1 : 0);
  }
  return w;
}
static unsigned read(unsigned address) {
  instruction(2, address);
  const unsigned w = word();
  deselect();
  return w;
}
/// The 00 opcode's commands, chosen by the top two address bits.
static void special(unsigned sub, int data = -1) {
  instruction(0, sub << (e.abits - 2));
  if (data >= 0)
    bits((unsigned)data, 16);
  deselect();
}
static void write(unsigned address, unsigned v) {
  instruction(1, address);
  bits(v, 16);
  deselect();
}

int main() {
  e.init(6); // 93C46: 64 words
  for (int i = 0; i < 64; i++)
    e.data[i] = (u16)(0x1100 + i);

  // READ: the address clocked in with DO high, then the dummy 0 that tells
  // a driver the address is complete, then D15-D0 (2.7).
  select();
  clk(1);
  bits(2, 2);
  bits(5 >> 1, 5);
  check("DO high while the address comes in", e.data_out(), 1);
  clk(5 & 1);
  check("dummy zero after the last address bit", e.data_out(), 0);
  check("read word 5", word(), 0x1105);
  check("sequential read with CS held high", word(), 0x1106);
  deselect();
  check("DO ready (high) once deselected", e.data_out(), 1);
  instruction(2, 63);
  word();
  check("sequential read wraps to word 0", word(), 0x1100);
  deselect();

  // Writes need EWEN first and stop after EWDS.
  write(7, 0x1234);
  check("write before EWEN ignored", read(7), 0x1107);
  special(3); // EWEN
  write(7, 0x1234);
  check("write after EWEN", read(7), 0x1234);
  instruction(3, 7); // ERASE
  deselect();
  check("erase sets the word to ffff", read(7), 0xffff);
  special(1, 0xabcd); // WRAL
  check("write all, word 0", read(0), 0xabcd);
  check("write all, word 63", read(63), 0xabcd);
  special(2); // ERAL
  check("erase all", read(40), 0xffff);
  special(0); // EWDS
  write(8, 0x0000);
  check("write after EWDS ignored", read(8), 0xffff);
  // A write cut short (CS dropped before the 16th data bit) writes nothing.
  special(3);
  instruction(1, 9);
  bits(0x12, 8);
  deselect();
  check("short write ignored", read(9), 0xffff);

  e.init(8); // 93C66: 256 words
  for (int i = 0; i < 256; i++)
    e.data[i] = (u16)(0x6600 + i);
  check("93C66 read word 200", read(200), 0x66c8);

  printf("eeprom93_test: %d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
