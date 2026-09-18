/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2021 Dietmar M. Zettl
 * Website: https://github.com/lenticularis39/axpbox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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
 *
 * Although this is not required, the author would appreciate being notified of,
 * and receiving any modifications you may make to the source code that might
 * serve the general public.
 *
 * Parts of this file based upon GXemul, which is Copyright (C) 2004-2007
 * Anders Gavare.  All rights reserved.
 */

/**
 * \file
 * How a Tulip learns what it is plugged into: the serial ROM that holds
 * its station address and describes its media, the MII management
 * interface to an external PHY, and the SIA's own autonegotiation.
 **/
#include "StdAfx.hpp"

#if defined(HAVE_PCAP) || defined(__linux__)
#include "Tulip.hpp"
#include "System.hpp"
#include <string.h>

/*  Internal states during MII data stream decode:  */
#define MII_STATE_RESET 0
#define MII_STATE_START_WAIT 1
#define MII_STATE_READ_OP 2
#define MII_STATE_READ_PHYADDR_REGADDR 3
#define MII_STATE_A 4
#define MII_STATE_D 5
#define MII_STATE_IDLE 6

/**
 *  This function handles accesses to the MII. Data streams seem to be of the
 *  following format:
 *
 *      vv---- starting delimiter
 *  ... 01 xx yyyyy zzzzz a[a] dddddddddddddddd
 *         ^---- I am starting with mii_bit = 0 here
 *
 *  where x = opcode (10 = read, 01 = write)
 *        y = PHY address
 *        z = register address
 *        a = on Reads: ACK bit (returned, should be 0)
 *            on Writes: _TWO_ dummy bits (10)
 *        d = 16 bits of data (MSB first)
 **/
void CTulip::mii_access(uint32_t oldreg, uint32_t idata) {
  int obit;

  int ibit = 0;
  uint16_t tmp;

  /*  Only care about data during clock cycles:  */
  if (!(idata & MIIROM_MDC))
    return;

  if (idata & MIIROM_MDC && oldreg & MIIROM_MDC)
    return;

  /*  printf("[ mii_access(): 0x%08x ]\n", (int)idata);  */
  if (idata & MIIROM_BR) {
    printf("[ mii_access(): MIIROM_BR: TODO ]\n");
    return;
  }

  obit = idata & MIIROM_MDO ? 1 : 0;

  if (state.mii.state >= MII_STATE_START_WAIT &&
      state.mii.state <= MII_STATE_READ_PHYADDR_REGADDR &&
      idata & MIIROM_MIIDIR)
    printf("[ mii_access(): bad dir? ]\n");

  switch (state.mii.state) {
  case MII_STATE_RESET:

    /*  Wait for a starting delimiter (0 followed by 1).  */
    if (obit)
      return;
    if (idata & MIIROM_MIIDIR)
      return;

    /*  printf("[ mii_access(): got a 0 delimiter ]\n");  */
    state.mii.state = MII_STATE_START_WAIT;
    state.mii.opcode = 0;
    state.mii.phyaddr = 0;
    state.mii.regaddr = 0;
    break;

  case MII_STATE_START_WAIT:

    /*  Wait for a starting delimiter (0 followed by 1).  */
    if (!obit)
      return;
    if (idata & MIIROM_MIIDIR) {
      state.mii.state = MII_STATE_RESET;
      return;
    }

    /*  printf("[ mii_access(): got a 1 delimiter ]\n");  */
    state.mii.state = MII_STATE_READ_OP;
    state.mii.bit = 0;
    break;

  case MII_STATE_READ_OP:
    if (state.mii.bit == 0) {
      state.mii.opcode = obit << 1;

      /*  printf("[ mii_access(): got first opcode bit (%i) ]\n", obit);  */
    } else {
      state.mii.opcode |= obit;

      /*  printf("[ mii_access(): got opcode = %i ]\n", state.mii.opcode);  */
      state.mii.state = MII_STATE_READ_PHYADDR_REGADDR;
    }

    state.mii.bit++;
    break;

  case MII_STATE_READ_PHYADDR_REGADDR:

    /*  printf("[ mii_access(): got phy/reg addr bit nr %i (%i)"
       " ]\n", state.mii.bit - 2, obit);  */
    if (state.mii.bit <= 6)
      state.mii.phyaddr |= obit << (6 - state.mii.bit);
    else
      state.mii.regaddr |= obit << (11 - state.mii.bit);
    state.mii.bit++;
    if (state.mii.bit >= 12) {

      /*  printf("[ mii_access(): phyaddr=0x%x regaddr=0x"
         "%x ]\n", state.mii.phyaddr, state.mii.regaddr);  */
      state.mii.state = MII_STATE_A;
    }
    break;

  case MII_STATE_A:
    switch (state.mii.opcode) {
    case MII_COMMAND_WRITE:
      if (state.mii.bit >= 13)
        state.mii.state = MII_STATE_D;
      break;

    case MII_COMMAND_READ:
      ibit = 0;
      state.mii.state = MII_STATE_D;
      break;

    case 3: /* Some stacks tickle the line so we see '11b' treat as READ */
      ibit = 0;
      state.mii.state = MII_STATE_D;
      state.mii.opcode = MII_COMMAND_READ;
      break;

    default:
      printf("[ mii_access(): UNIMPLEMENTED MII opcode %i (probably just a bug "
             "in GXemul's MII data stream handling) ]\n",
             state.mii.opcode);
      state.mii.state = MII_STATE_RESET;
    }

    state.mii.bit++;
    break;

  case MII_STATE_D:
    switch (state.mii.opcode) {
    case MII_COMMAND_WRITE:
      if (idata & MIIROM_MIIDIR)
        printf("[ mii_access(): write: bad dir? ]\n");
      obit = obit ? (0x8000 >> (state.mii.bit - 14)) : 0;
      tmp = state.mii.phy_reg[(state.mii.phyaddr << 5) + state.mii.regaddr] |
            obit;
      if (state.mii.bit >= 29) {
        state.mii.state = MII_STATE_IDLE;

        //                              debug("[ mii_access(): WRITE to
        //                              phyaddr=0x%x regaddr=0x%x: 0x%04x ]\n",
        //                              state.mii.phyaddr, state.mii.regaddr,
        //                              tmp);
      }
      break;

    case MII_COMMAND_READ:
      if (!(idata & MIIROM_MIIDIR))
        break;
      tmp = state.mii.phy_reg[(state.mii.phyaddr << 5) + state.mii.regaddr];

      //                      if (state.mii.bit == 13)
      //                              debug("[ mii_access(): READ phyaddr=0x%x
      //                              regaddr=0x%x: 0x%04x ]\n",
      //                              state.mii.phyaddr, state.mii.regaddr,
      //                              tmp);
      ibit = tmp & (0x8000 >> (state.mii.bit - 13));
      if (state.mii.bit >= 28)
        state.mii.state = MII_STATE_IDLE;
      break;
    }

    state.mii.bit++;
    break;

  case MII_STATE_IDLE:
    state.mii.bit++;
    if (state.mii.bit >= 31)
      state.mii.state = MII_STATE_RESET;
    break;
  }

  state.reg[CSR_MIIROM / 8] &= ~MIIROM_MDI;
  if (ibit)
    state.reg[CSR_MIIROM / 8] |= MIIROM_MDI;
}

void CTulip::complete_sia_autoneg() {
  /* What the emulated link partner answers with. The 21041's SIA
     negotiates at 10 Mb and knows nothing of 100, so it hears only the two
     10 Mb abilities; the 21143 hears all four. */
  u32 abilities = ANLPAR_ACK | ANLPAR_10_FD | ANLPAR_10 | ANLPAR_CSMA;
  if (m_chip.media == TULIP_MEDIA_SIA_21143)
    abilities |= ANLPAR_TX_FD | ANLPAR_TX;
  const u32 link_partner = abilities << 16;

  /* Autonegotiation completes immediately against the emulated link partner.
     Report a stable link without remote-fault bits. On the 21041 the two
     link-status bits cleared here are that part's network-connection-error
     and link-fail bits, which is the same statement in its own words. */
  state.reg[CSR_SIASTAT / 8] &= ~(SIASTAT_ANS | SIASTAT_LPC | SIASTAT_LS100 |
                                  SIASTAT_LS10 | SIASTAT_NSN | SIASTAT_TRF);
  state.reg[CSR_SIASTAT / 8] |=
      SIASTAT_ANS_FLPGOOD | SIASTAT_LPN | link_partner;

  state.reg[CSR_STATUS / 8] &= ~STATUS_LNF;
  state.reg[CSR_STATUS / 8] |= STATUS_LNPANC;

  if (m_chip.media == TULIP_MEDIA_SIA_21143) {
    /* The 100 Mb result, which only the 21143 has anywhere to put. */
    state.reg[CSR_OPMODE / 8] &= ~OPMODE_TTM;
    state.reg[CSR_OPMODE / 8] |=
        OPMODE_PS | OPMODE_PCS | OPMODE_SCR | OPMODE_FD | OPMODE_HBD;

    state.reg[CSR_SIATXRX / 8] &= ~(SIATXRX_TH | SIATXRX_THX | SIATXRX_T4);
    state.reg[CSR_SIATXRX / 8] |= SIATXRX_TXF;
  }

  update_irq();
}

/**
 * The driver has written CSR13, which on the older parts is where it takes
 * the serial interface out of reset. A real port would now look for its
 * link partner; the emulated one is plugged into a backend that is already
 * there, so the link comes up at once -- or goes away again if the driver
 * has just put the port back into reset.
 **/
void CTulip::sia_connect(u32 csr13) {
  if (!(csr13 & SIACONN_SRL)) {
    state.reg[CSR_SIASTAT / 8] |= SIASTAT_LKF | SIASTAT_NCR;
    state.reg[CSR_STATUS / 8] |= STATUS_LNF;
    update_irq();
    return;
  }

  state.reg[CSR_SIASTAT / 8] &= ~(SIASTAT_LKF | SIASTAT_NCR);
  state.reg[CSR_STATUS / 8] &= ~STATUS_LNF;
  state.reg[CSR_STATUS / 8] |= STATUS_LNPANC;

  /* A 21041 whose driver asked for autonegotiation gets it now; a 21040
     has none to give. */
  if (m_chip.media == TULIP_MEDIA_SIA_21041 &&
      (state.reg[CSR_SIATXRX / 8] & SIATXRX_ANE))
    complete_sia_autoneg();
  else
    update_irq();
}

/**
 * Read the 21140's general purpose port. A pin the chip drives reads back
 * what it drives; a pin it does not drive reads back whatever the board
 * puts there, and the board this part's serial ROM describes wires nothing
 * to those pins -- which is why its media blocks tell the driver there is
 * no link indicator to watch.
 **/
u32 CTulip::gpr_read() { return (u32)(state.gpr.out & state.gpr.dir); }

/**
 * Write it. A write with bit 8 set carries no data: it says which of the
 * eight pins the chip is to drive from now on.
 **/
void CTulip::gpr_write(u32 data) {
  if (data & 0x00000100)
    state.gpr.dir = (u8)(data & 0xff);
  else
    state.gpr.out = (u8)(data & 0xff);
}

/**
 * Hand the driver the next byte of the 21040's address ROM. The chip
 * presents one byte at a time in the low eight bits of CSR9 and holds bit
 * 31 up while the byte is not ready yet; ours is always ready, so that bit
 * stays down. The pointer runs over all 128 bytes the chip serialises,
 * which is the 32-byte ROM image four times over.
 **/
u32 CTulip::address_rom_read() {
  u32 data = state.srom.data[state.srom.addr & (sizeof(state.srom.data) - 1)];
  state.srom.addr++;
  return data;
}

/**
 *  This function handles reads from the Ethernet Address ROM. This is not a
 *  100% correct implementation, as it was reverse-engineered from OpenBSD
 *  sources; it seems to work with OpenBSD, NetBSD, and Linux, though.
 *
 *  Each transfer (if I understood this correctly) is of the following format:
 *
 *	1xx yyyyyy zzzzzzzzzzzzzzzz
 *
 *  where 1xx    = operation (6 means a Read),
 *        yyyyyy = ROM address
 *        zz...z = data
 *
 *  y and z are _both_ read and written to at the same time; this enables the
 *  operating system to sense the number of bits in y (when reading, all y bits
 *  are 1 except the last one).
 **/
void CTulip::srom_access(uint32_t oldreg, uint32_t idata) {
  int obit;

  int ibit;

  /*  debug("CSR9 WRITE! 0x%08x\n", (int)idata);  */

  /*  New selection? Then reset internal state.  */
  if (idata & MIIROM_SR && !(oldreg & MIIROM_SR)) {
    state.srom.curbit = 0;
    state.srom.opcode = 0;
    state.srom.opcode_has_started = 0;
    state.srom.addr = 0;
  }

  /*  Only care about data during clock cycles:  */
  if (!(idata & MIIROM_SROMSK))
    return;

  obit = 0;
  ibit = idata & MIIROM_SROMDI ? 1 : 0;

  /*  debug("CLOCK CYCLE! (bit %i): ", state.srom.curbit);  */

  /*
   *  Linux sends more zeroes before starting the actual opcode, than
   *  OpenBSD and NetBSD. Hopefully this is correct. (I'm just guessing
   *  that all opcodes should start with a 1, perhaps that's not really
   *  the case.)
   */
  if (!ibit && !state.srom.opcode_has_started)
    return;

  if (state.srom.curbit < 3) {
    state.srom.opcode_has_started = 1;
    state.srom.opcode <<= 1;
    state.srom.opcode |= ibit;

    /*  debug("opcode input '%i'\n", ibit);  */
  } else {
    switch (state.srom.opcode) {
    case TULIP_SROM_OPC_READ:
      if (state.srom.curbit < 6 + 3) {
        obit = state.srom.curbit < 6 + 2;
        state.srom.addr <<= 1;
        state.srom.addr |= ibit;
      } else {
        uint16_t romword = state.srom.data[state.srom.addr * 2] +
                           (state.srom.data[state.srom.addr * 2 + 1] << 8);

        //                              if (state.srom.curbit == 6 + 3)
        //                                      debug("[ dec21143: ROM read from
        //                                      offset 0x%03x: 0x%04x ]\n",
        //                                      state.srom.addr, romword);
        obit = romword & (0x8000 >> (state.srom.curbit - 6 - 3)) ? 1 : 0;
#if defined(DEBUG_NIC_SROM)
        printf("%%NIC-I-SROMREAD: Read %04x from %04x\n", romword,
               state.srom.addr);
#endif
      }
      break;

    default:
      printf("[ dec21243: unimplemented SROM/EEPROM opcode %i ]\n",
             state.srom.opcode);
    }

    state.reg[CSR_MIIROM / 8] &= ~MIIROM_SROMDO;
    if (obit)
      state.reg[CSR_MIIROM / 8] |= MIIROM_SROMDO;

    /*  debug("input '%i', output '%i'\n", ibit, obit);  */
  }

  state.srom.curbit++;

  /*
   *  Done opcode + addr + data? Then restart. (At least NetBSD does
   *  sequential reads without turning selection off and then on.)
   */
  if (state.srom.curbit >= 3 + 6 + 16) {
    state.srom.curbit = 0;
    state.srom.opcode = 0;
    state.srom.opcode_has_started = 0;
    state.srom.addr = 0;
  }
}

/**
 * Build the 21040's address ROM.
 *
 * That part has no serial ROM at all: its station address sits in a plain
 * parallel ROM which the chip reads out for the driver a byte at a time.
 * The layout is the one Digital recommended and every driver checks -- the
 * address, a checksum over it, both of those backwards, both of them again
 * forwards, and a test pattern. The chip serialises 128 bytes, which on a
 * real board is the 32-byte image four times over, and drivers do read
 * past the first copy, so we write all four.
 **/
void CTulip::build_address_rom() {
  u8 image[32];
  static const u8 testpat[8] = {0xFF, 0x00, 0x55, 0xAA, 0xFF, 0x00, 0x55, 0xAA};

  memcpy(image, state.mac, 6);

  /* The checksum: the three address words folded together with a doubling
     between each, every step brought back into sixteen bits by subtracting
     0xFFFF. The last step tests for equality as well, so a fold that comes
     out exactly 0xFFFF is carried as zero -- an asymmetry a driver
     reproduces faithfully, which is why we do too. */
  unsigned sum = (unsigned)(image[0] | image[1] << 8) * 2;
  if (sum > 65535)
    sum -= 65535;
  sum += (unsigned)(image[2] | image[3] << 8);
  if (sum > 65535)
    sum -= 65535;
  sum *= 2;
  if (sum > 65535)
    sum -= 65535;
  sum += (unsigned)(image[4] | image[5] << 8);
  if (sum >= 65535)
    sum -= 65535;
  image[6] = (u8)(sum & 0xff);
  image[7] = (u8)((sum >> 8) & 0xff);

  for (int i = 0; i < 8; i++)
    image[8 + i] = image[7 - i];
  memcpy(image + 16, image, 8);
  memcpy(image + 24, testpat, sizeof(testpat));

  for (unsigned i = 0; i < sizeof(state.srom.data); i += sizeof(image))
    memcpy(state.srom.data + i, image, sizeof(image));
  state.srom.addr = 0;
}

/**
 * Build the contents of the serial ROM this part reads its station
 * address and its media description out of.
 *
 * Three of the four parts have one, and they do not describe themselves
 * the same way. The 21143's ROM is version 3 of Digital's format, whose
 * identification block names the board by its subsystem id and whose media
 * blocks are the extended kind. The 21041 and the 21140 came first and use
 * version 1, whose identification block is eighteen zero bytes -- which is
 * exactly how a driver tells the two formats apart. After the header they
 * agree on the station address and on where the controller's own
 * description sits, and differ again in what that description may contain:
 * the 21041's blocks name a medium and may spell out the three SIA
 * registers for it, while the 21140's leaf begins with a direction byte
 * for its general purpose port and its blocks name what to put on it.
 **/
void CTulip::build_srom() {
  int leaf;

  memset(state.srom.data, 0, sizeof(state.srom.data));

  if (m_chip.id_rom == TULIP_ID_ADDRESS_ROM) {
    build_address_rom();
    return;
  }

  /* SROM v3 build per Digital "21X4 Serial ROM Format" 4.05.
   * v3 is the lowest version that defines extended-format info blocks
   * and 21143 block types  */
  if (m_chip.srom == TULIP_SROM_21143) {
    /* ID Block (bytes 0..17) — single-function format 5 */
    const uint16_t subsysVid = m_chip.subsys_vendor; /* DEC       */
    const uint16_t subsysId = m_chip.subsys_id;      /* DE-500BA  */
    state.srom.data[0] = subsysVid & 0xff;
    state.srom.data[1] = (subsysVid >> 8) & 0xff;
    state.srom.data[2] = subsysId & 0xff;
    state.srom.data[3] = (subsysId >> 8) & 0xff;
    /* bytes 4..14 = 0  (CIS pointers, ID_Reserved1) — left zero */
    state.srom.data[15] = 0x00; /* MiscHwOptions   - no PME/STSCHG       */
    /* byte 16 = ID_BLOCK_CRC (Appendix B: low byte of word 8), below */
    state.srom.data[17] = 0x00; /* Func0_HwOptions - no BootROM          */
  }
  /* For version 1 bytes 0..17 stay zero: the format has no identification
     block, and their being zero is the signature drivers look for. */

  /* Board info header (bytes 18..29) */
  state.srom.data[TULIP_ROM_SROM_FORMAT_VERION] =
      m_chip.srom == TULIP_SROM_21143 ? 3 : 1;
  state.srom.data[TULIP_ROM_CHIP_COUNT] = 1;

  /*  Set the MAC address:  */
  memcpy(state.srom.data + TULIP_ROM_IEEE_NETWORK_ADDRESS, state.mac, 6);

  leaf = 30;
  state.srom.data[TULIP_ROM_CHIPn_DEVICE_NUMBER(0)] = 0;
  state.srom.data[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0)] = leaf & 255;
  state.srom.data[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0) + 1] = leaf >> 8;

  if (m_chip.srom == TULIP_SROM_21143) {
    /* Controller info leaf (offset 30) — 21143 7.5.1 */
    state.srom.data[leaf + 0] = 0x00; /* Selected Conn Type LSB         */
    state.srom.data[leaf + 1] = 0x08; /* MSB -> 0x0800 Powerup+Dynamic   */
    state.srom.data[leaf + 2] = 2;    /* Block Count = 2                */
    leaf += 3;

    /* 21143 SYM 100BaseTX-FDX (type 4, extended) 7.5.2.1.3
     * The DE-500BA uses the chip's internal SYM scrambler/PCS for 100TX
     * - there is NO external MII PHY on this board.  */
    state.srom.data[leaf++] = 0x80 | 8; /* F=1, length=8                     */
    state.srom.data[leaf++] = TULIP_ROM_MB_21143_SYM;
    state.srom.data[leaf++] =
        TULIP_ROM_MB_MEDIA_100TX_FDX; /* 0x05 = 100BaseTX FDX */
    state.srom.data[leaf++] = 0x00;   /* GPP Control LSB - no GPP needed   */
    state.srom.data[leaf++] = 0x00;   /* GPP Control MSB                   */
    state.srom.data[leaf++] = 0x00;   /* GPP Data LSB                      */
    state.srom.data[leaf++] = 0x00;   /* GPP Data MSB                      */
    state.srom.data[leaf++] = 0x61;   /* Command LSB: PS|PCS|SCR, no TTM   */
    state.srom.data[leaf++] = 0x80;   /* Command MSB: no media sense pin   */

    /* 21142/3 SIA 10BaseT (type 2, extended, EXT=0) 7.4.2.1.1 */
    state.srom.data[leaf++] =
        0x80 | 6; /* F=1, length=6 (no Media Specific Data) */
    state.srom.data[leaf++] = TULIP_ROM_MB_21142_SIA;
    state.srom.data[leaf++] = 0x00; /* EXT=0, MediaCode=0 (10BaseT) */
    state.srom.data[leaf++] = 0x00; /* GPP Control LSB          */
    state.srom.data[leaf++] = 0x00; /* GPP Control MSB          */
    state.srom.data[leaf++] = 0x00; /* GPP Data LSB             */
    state.srom.data[leaf++] = 0x00; /* GPP Data MSB             */
  } else if (m_chip.srom == TULIP_SROM_21140) {
    /* The 21140's info leaf. After the connection type comes the byte the
       21143's leaf does not have: which of the general purpose port's
       eight pins the chip is to drive. This board drives none of them --
       it has nothing wired there -- so the byte is zero and both media
       blocks below say the driver should not look for a link indicator.

       A driver is free to disbelieve that. The console does: it writes its
       own pin directions (0x1f, the DE500's, where the low five pins are
       outputs and the top three are a board's link indications), reads
       none of them back as asserted, and settles on 10BaseT. That is a
       true answer for a board with nothing on those pins, and the medium
       it picks makes no difference to what reaches the backend. */
    state.srom.data[leaf + 0] = SELECT_CONN_TYPE_100TX & 0xff;
    state.srom.data[leaf + 1] = SELECT_CONN_TYPE_100TX >> 8;
    state.srom.data[leaf + 2] = 0x00; /* general purpose port directions */
    state.srom.data[leaf + 3] = 2;    /* Block Count = 2                 */
    leaf += 4;

    /* Two compact 21140 blocks, four bytes each: the medium, the byte to
       put on the general purpose port for it, and the command word, whose
       bits 0, 4, 5 and 6 are the CSR6 bits the driver is to set. 100BaseTX
       wants the MII/SYM port with its scrambler and PCS; 10BaseT wants the
       serial port and the 10 Mb transmit threshold. */
    state.srom.data[leaf++] = TULIP_ROM_MB_MEDIA_100TX;
    state.srom.data[leaf++] = 0x00; /* nothing to drive               */
    state.srom.data[leaf++] = 0x61; /* Command LSB: PS|PCS|SCR        */
    state.srom.data[leaf++] = 0x80; /* Command MSB: no link indicator */

    state.srom.data[leaf++] = TULIP_ROM_MB_MEDIA_TP;
    state.srom.data[leaf++] = 0x00;
    state.srom.data[leaf++] = 0x10; /* Command LSB: TTM               */
    state.srom.data[leaf++] = 0x80;
  } else {
    /* The 21041's info leaf: a connection type, a block count, and blocks
       of one byte each naming a medium. A block may carry six more bytes
       spelling out CSR13, CSR14 and CSR15 for that medium; this board has
       nothing unusual to say about its twisted pair, so it does not, and
       the driver uses the values it holds for a 21041 itself.

       The blocks begin straight after the count, as NetBSD's if_de reads
       them and as the 21140 leaf below implies -- there the extra byte
       between the two is the port direction, which a 21041 has no port to
       need. (Linux's de2104x leaves a byte of padding there instead. Which
       of them is right cannot be settled here: the console does not read
       this leaf at all. It takes the station address out of the ROM and
       then drives the SIA at 10BaseT from its own table, whatever the leaf
       says -- which is also what if_de does, and why it says that
       thankfully all 21041s act the same.) */
    state.srom.data[leaf + 0] = SELECT_CONN_TYPE_TP & 0xff;
    state.srom.data[leaf + 1] = SELECT_CONN_TYPE_TP >> 8;
    state.srom.data[leaf + 2] = 1; /* Block Count = 1 */
    leaf += 3;

    state.srom.data[leaf++] = TULIP_ROM_MB_MEDIA_TP;
  }

  /* ID_BLOCK_CRC (Appendix B): 8-bit CRC, MSB-first, poly 0x06, init 0xFF.
   * Walks bits of the first 9 words MSB-first, stopping at word 8 bit 7.
   * Per the algorithm in the spec, the CRC result lands in the LOW byte
   * of word 8 (= byte 16). Byte 17 is the high byte of word 8 and is
   * INPUT to the walk (Func0_HwOptions in our layout, value 0). Version 1
   * has no identification block to protect, so it has no such CRC. */
  if (m_chip.srom == TULIP_SROM_21143) {
    unsigned char crc8 = 0xFF;
    for (int word = 0; word < 9; word++) {
      uint16_t w =
          state.srom.data[word * 2] | (state.srom.data[word * 2 + 1] << 8);
      for (int bit = 15; bit >= 0; bit--) {
        if (word == 8 && bit == 7)
          break;
        unsigned char bv = ((w >> bit) & 1) ^ ((crc8 >> 7) & 1);
        crc8 <<= 1;
        if (bv) {
          crc8 ^= 0x06;
          crc8 |= 0x01;
        }
      }
    }
    state.srom.data[16] = crc8;
  }

  // compute the CRC for the SROM data.  This code is from the
  // tu sample driver from HP in if_tu.c, which was apparently
  // derived from the 21140 Hardware Specification
  unsigned int POLY = 0x04c11db6;
  unsigned int FlippedCrc = 0;
  unsigned int Crc = 0xffffffff;
  unsigned char i;
  unsigned char CurrentByte;
  unsigned char Bit;
  unsigned char Msb;
  unsigned char chksm_1;
  unsigned char chksm_2;

  for (i = 0; i < 126; i++) {
    CurrentByte = state.srom.data[i];

    for (Bit = 0; Bit < 8; Bit++) {
      Msb = (Crc >> 31) & 1;
      Crc <<= 1;

      if (Msb ^ (CurrentByte & 1)) {
        Crc ^= POLY;
        Crc |= 1;
      }

      CurrentByte >>= 1;
    }
  }

  for (i = 0; i < 32; i++) {
    FlippedCrc <<= 1;
    Bit = Crc & 1;
    Crc >>= 1;
    FlippedCrc += Bit;
  }

  Crc = FlippedCrc ^ 0xffffffff;

  chksm_1 = Crc & 0xff;
  chksm_2 = Crc >> 8;

  state.srom.data[126] = chksm_1;
  state.srom.data[127] = chksm_2;
#if defined(DEBUG_NIC_SROM)
  printf("%%NIC-I-CKSUM: SROM checksum bytes are %02x, %02x\n",
         state.srom.data[126], state.srom.data[127]);
#endif
}

#endif // defined(HAVE_PCAP) || defined(__linux__)
