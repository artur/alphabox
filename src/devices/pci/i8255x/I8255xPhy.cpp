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
 * Intel 82555 PHY registers.
 **/
#include "I8255xPhy.hpp"

// MII registers
#define MII_BMCR 0 // control
#define MII_BMSR 1 // status
#define MII_PHYID1 2
#define MII_PHYID2 3
#define MII_ANAR 4     // autonegotiation advertisement
#define MII_ANLPAR 5   // link partner ability
#define MII_ANER 6     // autonegotiation expansion
#define PHY_STATCTL 16 // 82555 status and control

#define BMCR_RESET 0x8000
#define BMCR_LOOPBACK 0x4000
#define BMCR_SPEED100 0x2000
#define BMCR_ANENABLE 0x1000
#define BMCR_ANRESTART 0x0200
#define BMCR_FULLDPLX 0x0100

// 100BASE-TX FD/HD, 10BASE-T FD/HD, autonegotiation able, extended
// register set; link up and autonegotiation complete.
#define BMSR_VALUE 0x782d

// Link partner: every 10/100 mode, acknowledging our page.
#define ANLPAR_VALUE 0x45e1

void CI8255xPhy::reset() {
  memset(regs, 0, sizeof(regs));
  regs[MII_BMCR] = BMCR_SPEED100 | BMCR_ANENABLE;
  regs[MII_BMSR] = BMSR_VALUE;
  regs[MII_PHYID1] = 0x02a8; // Intel OUI
  regs[MII_PHYID2] = 0x0154; // 82555, revision 4
  regs[MII_ANAR] = 0x05e1;   // every 10/100 mode, IEEE 802.3 selector
  regs[MII_ANLPAR] = ANLPAR_VALUE;
  regs[MII_ANER] = 0x0001;    // partner can autonegotiate
  regs[PHY_STATCTL] = 0x0003; // 100 Mbit/s, full duplex
}

u16 CI8255xPhy::read(int reg) { return regs[reg & 0x1f]; }

void CI8255xPhy::write(int reg, u16 value) {
  switch (reg & 0x1f) {
  case MII_BMCR:
    if (value & BMCR_RESET) {
      reset();
      return;
    }
    // Restart completes at once; the link never drops.
    regs[MII_BMCR] = value & ~BMCR_ANRESTART;
    return;
  case MII_ANAR:
    regs[MII_ANAR] = (value & 0x3fe0) | 0x0001;
    return;
  case PHY_STATCTL:
    // Speed and duplex (bits 1..0) are status; the rest is control.
    regs[PHY_STATCTL] = (value & ~0x0003) | (regs[PHY_STATCTL] & 0x0003);
    return;
  default:
    // Identification and status are read-only; the vendor registers
    // beyond 16 hold whatever was written.
    if (reg > PHY_STATCTL)
      regs[reg & 0x1f] = value;
    return;
  }
}
