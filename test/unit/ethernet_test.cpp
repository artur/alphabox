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
 * Unit test of the receive queue's frame check sequence (Ethernet.cpp).
 *
 * A NIC configured with `crc = true` hands its guest each received frame
 * with the FCS computed and appended (CPacketQueue::add_tail). The FCS
 * follows the frame least significant byte first (IEEE 802.3 3.2.9: the
 * CRC's bits go out x^31 first, which a receiver that shifts in the octet's
 * low bit first sees as the complemented CRC-32's low byte first). The
 * checks: the CRC-32 itself against its check value, the four bytes
 * appended, and the property a receiver tests, that the CRC-32 of the frame
 * with its FCS is the residue 2144DF1Ch.
 **/
#include "Ethernet.hpp"

#include <cstdio>
#include <cstring>

static int checks = 0, failures = 0;

#define CHECK(cond, what)                                                      \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      printf("FAIL line %d: %s\n", __LINE__, what);                            \
    }                                                                          \
  } while (0)

int main() {
  // The CRC-32 check value (ITU-T V.42, ISO 3309): "123456789".
  const u8 digits[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(eth_crc32(0, digits, 9) == 0xCBF43926u, "the CRC-32 of 123456789");

  CPacketQueue q("test", 4);
  eth_packet p;

  CHECK(q.add_tail(digits, 9, true, true), "a frame is queued");
  CHECK(q.get_head(p), "and comes back");
  CHECK(p.len == 13, "four bytes longer");
  const u8 want[4] = {0x26, 0x39, 0xF4, 0xCB};
  CHECK(memcmp(p.frame + 9, want, 4) == 0,
        "the FCS follows least significant byte first");
  CHECK(eth_crc32(0, p.frame, p.len) == 0x2144DF1Cu,
        "the frame with its FCS has the CRC-32 residue");

  // A frame of the least length a wire carries, every byte different.
  u8 f[ETH_MIN_PACKET_RAW];
  for (int i = 0; i < (int)sizeof(f); i++)
    f[i] = (u8)(i * 37 + 11);
  CHECK(q.add_tail(f, sizeof(f), true, true), "a 60-byte frame is queued");
  CHECK(q.get_head(p) && p.len == 64, "64 bytes with its FCS");
  CHECK(memcmp(p.frame, f, sizeof(f)) == 0, "the frame's bytes unchanged");
  CHECK(eth_crc32(0, p.frame, p.len) == 0x2144DF1Cu, "and the residue again");

  // Without `crc = true` the four bytes are there and zero; with no FCS
  // wanted the frame is as given.
  CHECK(q.add_tail(f, sizeof(f), false, true) && q.get_head(p) && p.len == 64 &&
            !p.frame[60] && !p.frame[61] && !p.frame[62] && !p.frame[63],
        "an FCS not computed is four zero bytes");
  CHECK(q.add_tail(f, sizeof(f), true, false) && q.get_head(p) && p.len == 60,
        "a frame without an FCS keeps its length");

  printf("ethernet_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
