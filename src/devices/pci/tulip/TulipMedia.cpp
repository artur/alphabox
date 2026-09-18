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
  const u32 link_partner = ((u32)(ANLPAR_ACK | ANLPAR_TX_FD | ANLPAR_TX |
                                  ANLPAR_10_FD | ANLPAR_10 | ANLPAR_CSMA)
                            << 16);

  /* Autonegotiation completes immediately against the emulated link partner.
     Report a stable 100baseTX full-duplex link without remote-fault bits. */
  state.reg[CSR_SIASTAT / 8] &= ~(SIASTAT_ANS | SIASTAT_LPC | SIASTAT_LS100 |
                                  SIASTAT_LS10 | SIASTAT_NSN | SIASTAT_TRF);
  state.reg[CSR_SIASTAT / 8] |=
      SIASTAT_ANS_FLPGOOD | SIASTAT_LPN | link_partner;

  state.reg[CSR_STATUS / 8] &= ~STATUS_LNF;
  state.reg[CSR_STATUS / 8] |= STATUS_LNPANC;

  state.reg[CSR_OPMODE / 8] &= ~OPMODE_TTM;
  state.reg[CSR_OPMODE / 8] |=
      OPMODE_PS | OPMODE_PCS | OPMODE_SCR | OPMODE_FD | OPMODE_HBD;

  state.reg[CSR_SIATXRX / 8] &= ~(SIATXRX_TH | SIATXRX_THX | SIATXRX_T4);
  state.reg[CSR_SIATXRX / 8] |= SIATXRX_TXF;

  update_irq();
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
 * Build the contents of the serial ROM this part reads its station
 * address and its media description out of.
 **/
void CTulip::build_srom() {
  int leaf;

  /* SROM v3 build per Digital "21X4 Serial ROM Format" 4.05.
   * v3 is the lowest version that defines extended-format info blocks
   * and 21143 block types  */

  /* ID Block (bytes 0..17) — single-function format 5 */
  const uint16_t subsysVid = 0x1011; /* DEC                 */
  const uint16_t subsysId = 0x500B;  /* DE-500BA            */
  state.srom.data[0] = subsysVid & 0xff;
  state.srom.data[1] = (subsysVid >> 8) & 0xff;
  state.srom.data[2] = subsysId & 0xff;
  state.srom.data[3] = (subsysId >> 8) & 0xff;
  /* bytes 4..14 = 0  (CIS pointers, ID_Reserved1) — left zero */
  state.srom.data[15] = 0x00; /* MiscHwOptions   - no PME/STSCHG       */
  /* byte 16 = ID_BLOCK_CRC (Appendix B: low byte of word 8), filled below */
  state.srom.data[17] = 0x00; /* Func0_HwOptions - no BootROM          */

  /* Board info header (bytes 18..29) */
  state.srom.data[TULIP_ROM_SROM_FORMAT_VERION] = 3;
  state.srom.data[TULIP_ROM_CHIP_COUNT] = 1;

  /*  Set the MAC address:  */
  memcpy(state.srom.data + TULIP_ROM_IEEE_NETWORK_ADDRESS, state.mac, 6);

  leaf = 30;
  state.srom.data[TULIP_ROM_CHIPn_DEVICE_NUMBER(0)] = 0;
  state.srom.data[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0)] = leaf & 255;
  state.srom.data[TULIP_ROM_CHIPn_INFO_LEAF_OFFSET(0) + 1] = leaf >> 8;

  /* Controller info leaf (offset 30) — 21143 7.5.1 */
  state.srom.data[leaf + 0] = 0x00; /* Selected Conn Type LSB         */
  state.srom.data[leaf + 1] = 0x08; /* MSB -> 0x0800 Powerup+Dynamic   */
  state.srom.data[leaf + 2] = 2;    /* Block Count = 2                */
  leaf += 3;

  /* 21143 SYM 100BaseTX-FDX (type 4, extended) 7.5.2.1.3
   * The DE-500BA uses the chip's internal SYM scrambler/PCS for 100TX
   * - there is NO external MII PHY on this board.  */
  state.srom.data[leaf++] = 0x80 | 8; /* F=1, length=8                       */
  state.srom.data[leaf++] = TULIP_ROM_MB_21143_SYM;
  state.srom.data[leaf++] =
      TULIP_ROM_MB_MEDIA_100TX_FDX; /* 0x05 = 100BaseTX FDX */
  state.srom.data[leaf++] = 0x00;   /* GPP Control LSB - no GPP needed     */
  state.srom.data[leaf++] = 0x00;   /* GPP Control MSB                     */
  state.srom.data[leaf++] = 0x00;   /* GPP Data LSB                        */
  state.srom.data[leaf++] = 0x00;   /* GPP Data MSB                        */
  state.srom.data[leaf++] = 0x61;   /* Command LSB: PS|PCS|SCR, no TTM     */
  state.srom.data[leaf++] = 0x80;   /* Command MSB: no media sense pin      */

  /* 21142/3 SIA 10BaseT (type 2, extended, EXT=0) 7.4.2.1.1 */
  state.srom.data[leaf++] =
      0x80 | 6; /* F=1, length=6 (no Media Specific Data) */
  state.srom.data[leaf++] = TULIP_ROM_MB_21142_SIA;
  state.srom.data[leaf++] = 0x00; /* EXT=0, MediaCode=0 (10BaseT) */
  state.srom.data[leaf++] = 0x00; /* GPP Control LSB          */
  state.srom.data[leaf++] = 0x00; /* GPP Control MSB          */
  state.srom.data[leaf++] = 0x00; /* GPP Data LSB             */
  state.srom.data[leaf++] = 0x00; /* GPP Data MSB             */

  /* ID_BLOCK_CRC (Appendix B): 8-bit CRC, MSB-first, poly 0x06, init 0xFF.
   * Walks bits of the first 9 words MSB-first, stopping at word 8 bit 7.
   * Per the algorithm in the spec, the CRC result lands in the LOW byte
   * of word 8 (= byte 16). Byte 17 is the high byte of word 8 and is
   * INPUT to the walk (Func0_HwOptions in our layout, value 0). */
  {
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

  /*  MII Management decoder initial state:  */
  state.mii.state = MII_STATE_RESET;

  state.tx.suspend = false;

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
