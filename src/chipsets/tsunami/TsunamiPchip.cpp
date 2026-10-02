/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/lenticularis39/axpbox
 *          https://github.com/artur/alphabox
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
 */

#include "AlphaCPU.hpp"
#include "PciWindows.hpp"
#include "StdAfx.hpp"
#include "System.hpp"
#include "Tsunami.hpp"

/**
 * \brief Read one of the PCHIP registers.
 *
 * Source: HRM, 10.2.5:
 *
 * \code
 * +-------------+---------------+------+----+
 * |Register     | Address       | Type | ## |
 * +-------------+---------------+------+----+
 * | P0-WSBA0    | 801.8000.0000 | RW   | 00 |
 * | P0-WSBA1    | 801.8000.0040 | RW   | 01 |
 * | P0-WSBA2    | 801.8000.0080 | RW   | 02 |
 * | P0-WSBA3    | 801.8000.00C0 | RW   | 03 |
 * +-------------+---------------+------+----+
 * | P0-WSM0     | 801.8000.0100 | RW   | 04 |
 * | P0-WSM1     | 801.8000.0140 | RW   | 05 |
 * | P0-WSM2     | 801.8000.0180 | RW   | 06 |
 * | P0-WSM3     | 801.8000.01C0 | RW   | 07 |
 * +-------------+---------------+------+----+
 * | P0-TBA0     | 801.8000.0200 | RW   | 08 |
 * | P0-TBA1     | 801.8000.0240 | RW   | 09 |
 * | P0-TBA2     | 801.8000.0280 | RW   | 0A |
 * | P0-TBA3     | 801.8000.02C0 | RW   | 0B |
 * +-------------+---------------+------+----+
 * | P0-PCTL     | 801.8000.0300 | RW   | 0C |
 * +-------------+---------------+------+----+
 * | P0-PLAT     | 801.8000.0340 | RW   | 0D |
 * +-------------+---------------+------+----+
 * | P0-RES      | 801.8000.0380 | RW   | 0E |
 * +-------------+---------------+------+----+
 * | P0-PERROR   | 801.8000.03C0 | RW   | 0F |
 * | P0-PERRMASK | 801.8000.0400 | RW   | 10 |
 * | P0-PERRSET  | 801.8000.0440 | WO   | 11 |
 * +-------------+---------------+------+----+
 * | P0-TLBIV    | 801.8000.0480 | WO   | 12 |
 * | P0-TLBIA    | 801.8000.04C0 | WO   | 13 |
 * +-------------+---------------+------+----+
 * | P0-PMONCTL  | 801.8000.0500 | RW   | 14 |
 * | P0-PMONCNT  | 801.8000.0540 | RO   | 15 |
 * +-------------+---------------+------+----+
 * | P0-SPRST    | 801.8000.0800 | WO   | 20 |
 * +-------------+---------------+------+----+
 * \endcode
 *
 * Window Space Base Address Register (WSBAn - RW)
 *
 * Because the information in the WSBAn registers and WSMn registers
 * is used to compare against the PCI address, a clock-domain crossing (from
 * i_sysclk to i_pclko<7:0>) is made when these registers are written.
 *Therefore, for a period of several clock cycles, a window is disabled when its
 *contents are disabled. If PCI bus activity, which accesses the window in
 *question, is not stopped before updating that window, the Pchip might fail to
 *respond with b_devsel_l when it should. This would result in a master abort
 *condition on the PCI bus. Therefore, before a window (base or mask) is
 *updated, all PCI activity accessing that window must be stopped, even if only
 *some activity is being added or deleted.
 *
 * The contents of the window may be read back to confirm that the update has
 *taken place. Then PCI activity through that window can be resumed.
 *
 * \code
 * +-------+---------+---------+------+--------------------------+
 * | Field | Bits    | Type    | Init | Description              |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <63:40> | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * | DAC   | <39>    | RW      | 0    | DAC enable (WSBA3 only!) |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <38:32> | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * | ADDR  | <31:20> | RW      | 0    | Base address (not used   |
 * |       |         |         |      | DAC enable = 1)          |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <19:2>  | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * | SG    | <1>     | RW      | 0    | Scatter-gather           |
 * +-------+---------+---------+------+--------------------------+
 * | ENA   | <0>     | RW      | 0    | Enable                   |
 * +-------+---------+---------+------+--------------------------+
 * \endcode
 *
 * Window Space Mask Register (WSM0, WSM1, WSM2, WSM3 - RW)
 *
 * \code
 * +-------+---------+---------+------+--------------------------+
 * | Field | Bits    | Type    | Init | Description              |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <63:32> | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * | AM    | <31:20> | RW      | 0    | Address mask             |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <19:0>  | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * \endcode
 *
 * Translated Base Address Register (TBAn - RW)
 *
 * \code
 * +-------+---------+---------+------+--------------------------+
 * | Field | Bits    | Type    | Init | Description              |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <63:35> | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * | ADDR  | <34:10> | RW      | 0    | Translated base address  |
 * |       |         |         |      | (if DAC enable = 1, bits |
 * |       |         |         |      | <34:22> are the PT Origin|
 * |       |         |         |      | address <34:22> and bits |
 * |       |         |         |      | <21:10> are ignored)     |
 * +-------+---------+---------+------+--------------------------+
 * | RES   | <9:0>   | MBZ,RAZ | 0    | Reserved                 |
 * +-------+---------+---------+------+--------------------------+
 * \encode
 *
 * Pchip Control Register (PCTL - RW)
 *
 * \code
 * +---------+---------+---------+------+-------------------------------------+
 * | Field   | Bits    | Type    | Init | Description                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | RES     | <63:48> | MBZ,RAZ | 0    | Reserved.                           |
 * +---------+---------+---------+------+-------------------------------------+
 * | PID     | <47:46> | RO      | 1)   | Pchip ID.                           |
 * +---------+---------+---------+------+-------------------------------------+
 * | RPP     | <45>    | RO      | 2)   | Remote Pchip present.               |
 * +---------+---------+---------+------+-------------------------------------+
 * | PTEVRFY | <44>    | RW      |      | PTE verify for DMA read.            |
 * |         |         |         |      |   Val Description                   |
 * |         |         |         |      |   0   If TLB miss, then make DMA    |
 * |         |         |         |      |       read request as soon as possi-|
 * |         |         |         |      |       ble and discard data if PTE   |
 * |         |         |         |      |       was not valid - could cause   |
 * |         |         |         |      |       Cchip nonexistent mem. error. |
 * |         |         |         |      |   1   If TLB miss, then delay read  |
 * |         |         |         |      |       request until PTE is verified |
 * |         |         |         |      |       as valid - no request if not  |
 * |         |         |         |      |       valid.                        |
 * +---------+---------+---------+------+-------------------------------------+
 * | FDWDIS  | <43>    | RW      |      | Fast DMA read cache block wrap      |
 * |         |         |         |      | request disable.                    |
 * |         |         |         |      |   Val Description                   |
 * |         |         |         |      |   0   Normal operation.             |
 * |         |         |         |      |   1   Reserved for testing purposes |
 * |         |         |         |      |       only.                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | FDSDIS  | <42>    | RW      |      | Fast DMA start and SGTE request     |
 * |         |         |         |      | disable.                            |
 * |         |         |         |      |   Val Description                   |
 * |         |         |         |      |   0   Normal operation.             |
 * |         |         |         |      |   1   Reserved for testing purposes |
 * |         |         |         |      |       only.                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | PCLKX   | <41:40> | RO      | 3)   | PCI clock frequency multiplier      |
 * |         |         |         |      |   Val Multiplier                    |
 * |         |         |         |      |   0   x6                            |
 * |         |         |         |      |   1   x4                            |
 * |         |         |         |      |   2   x5                            |
 * |         |         |         |      |   3   Reserved                      |
 * +---------+---------+---------+------+-------------------------------------+
 * | PTPMAX  | <39:36> | RW      | 2    | Maximum PTP requests to Cchip from  |
 * |         |         |         |      | both Pchips until returned on       |
 * |         |         |         |      | CAPbus, modulo 16 (minimum = 2)     |
 * |         |         |         |      | (use 4 for pass 1 Cchip and Dchip). |
 * +---------+---------+---------+------+-------------------------------------+
 * | CRQMAX  | <35:32> | RW      | 1    | Maximum requests to Cchip from both |
 * |         |         |         |      | Pchips until Ack, modulo 16 (use 4  |
 * |         |         |         |      | for Cchip). (Use 3 or less for      |
 * |         |         |         |      | Typhoon because there is one less   |
 * |         |         |         |      | skid buffer in the C4 chip.)        |
 * +---------+---------+---------+------+-------------------------------------+
 * | REV     | <31:24> | RO      | 0    | In conjunction with the state of    |
 * |         |         |         |      | PMONCTL<0>, this field indicates    |
 * |         |         |         |      | the revision of the Pchip.          |
 * +---------+---------+---------+------+-------------------------------------+
 * | CDQMAX  | <23:20> | RW      | 1    | Maximum data transfers to Dchips    |
 * |         |         |         |      | from both Pchips until Ack, modulo  |
 * |         |         |         |      | 16 (use 4 for Dchip). Must be same  |
 * |         |         |         |      | as Cchip CSR CSC<FPQPMAX>.          |
 * +---------+---------+---------+------+-------------------------------------+
 * | PADM    | <19>    | RW      | 4)   | PADbus mode.                        |
 * |         |         |         |      |   Val Mode                          |
 * |         |         |         |      |   0   8-nibble, 8-check bit mode    |
 * |         |         |         |      |   1   4-byte, 4-check bit mode      |
 * +---------+---------+---------+------+-------------------------------------+
 * | ECCEN   | <18>    | RW      | 0    | ECC enable for DMA and SGTE access. |
 * +---------+---------+---------+------+-------------------------------------+
 * | RES     | <17:16> | MBZ,RAZ | 0    | Reserved.                           |
 * +---------+---------+---------+------+-------------------------------------+
 * | PPRI    | <15>    |         | 0    | Arbiter prio group for the Pchip.   |
 * +---------+---------+---------+------+-------------------------------------+
 * | PRIGRP  | <14:8>  | RW      | 0    | Arbiter prio group; one bit per PCI |
 * |         |         |         |      | slot with bits <14:8> corresponding |
 * |         |         |         |      | to input b_req_l<6:0>.              |
 * |         |         |         |      |   Val Group                         |
 * |         |         |         |      |   0   Low-priority group            |
 * |         |         |         |      |   1   High-priority group           |
 * +---------+---------+---------+------+-------------------------------------+
 * | ARBENA  | <7>     | RW      | 0    | Internal arbiter enable.            |
 * +---------+---------+---------+------+-------------------------------------+
 * | MWIN    | <6>     | RW      | 0    | Monster window enable.              |
 * +---------+---------+---------+------+-------------------------------------+
 * | HOLE    | <5>     | RW      | 0    | 512KB-to-1MB window hole enable.    |
 * +---------+---------+---------+------+-------------------------------------+
 * | TGTLAT  | <4>     | RW      | 0    | Target latency timers enable.       |
 * |         |         |         |      |   Val Mode                          |
 * |         |         |         |      |   0   Retry/disconnect after 128    |
 * |         |         |         |      |       PCI clocks without data.      |
 * |         |         |         |      |   1   Retry initial request after   |
 * |         |         |         |      |       32 PCI clocks without data;   |
 * |         |         |         |      |       disconnect subsequent trans-  |
 * |         |         |         |      |       fers after 8 PCI clocks       |
 * |         |         |         |      |       without data.                 |
 * +---------+---------+---------+------+-------------------------------------+
 * | CHAINDIS| <3>     | RW      | 0    | Disable chaining.                   |
 * +---------+---------+---------+------+-------------------------------------+
 * | THDIS   | <2>     | RW      | 0    | Disable antithrash mechan. for TLB. |
 * |         |         |         |      |   Val Mode                          |
 * |         |         |         |      |   0   Normal operation              |
 * |         |         |         |      |   1   Testing purposes only         |
 * +---------+---------+---------+------+-------------------------------------+
 * | FBTB    | <1>     | RW      | 0    | Fast back-to-back enable.           |
 * +---------+---------+---------+------+-------------------------------------+
 * | FDSC    | <0>     | RW      | 0    | Fast discard enable.                |
 * |         |         |         |      |   Val Mode                          |
 * |         |         |         |      |   0   Discard data if no retry      |
 * |         |         |         |      |       after 215 PCI clocks.         |
 * |         |         |         |      |   1   Discard data if no retry      |
 * |         |         |         |      |       after 210 PCI clocks.         |
 * +---------+---------+---------+------+-------------------------------------+
 *
 * 1) This field is initialized from the PID pins.
 * 2) This field is initialized from the assertion of CREQRMT_L pin at system
 *reset. 3) This field is initialized from the PCI i_pclkdiv<1:0> pins. 4) This
 *field is initialized from a decode of the b_cap<1:0> pins. \endcode
 *
 * Pchip Error Register (PERROR - RW)
 *
 * If any of bits <11:0> are set, then this entire register is frozen and the
 *Pchip output signal b_error is asserted. Only bit <0> can be set after that.
 *All other values will be held until all of bits <11:0> are clear. When an
 *error is detected and one of bits <11:0> becomes set, the associated
 *information is captured in bits <63:16> of this register. After the
 *information is captured, the INV bit is cleared, but the information is not
 *valid and should not be used if INV is set.
 *
 * In rare circumstances involving more than one error, INV may remain set
 *because the Pchip cannot correctly capture the SYN, CMD, or ADDR field.
 *
 * Furthermore, if software reads PERROR in a polling loop, or reads PERROR
 *before the Pchip�s error signal is reflected in the Cchip�s DRIR CSR, the INV
 *bit may also be set.
 *
 * To avoid the latter condition, read PERROR only after receiving an IRQ0
 *interrupt, then read the Cchip DIR CSR to determine that this Pchip has
 *detected an error.
 *
 * \code
 * +---------+---------+---------+------+-------------------------------------+
 * | Field   | Bits    | Type    | Init | Description                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | SYN     | <63:56> | RO      | 0    | errors ECC syndrome if CRE or UECC. |
 * +---------+---------+---------+------+-------------------------------------+
 * | CMD     | <55:52> | RO      | 0    | PCI command of transaction when     |
 * |         |         |         |      | error detected if not CRE and not   |
 * |         |         |         |      | UECC. If CRE or UECC, then:         |
 * |         |         |         |      |   Val    Command                    |
 * |         |         |         |      |   0000   DMA read                   |
 * |         |         |         |      |   0001   DMA RMW                    |
 * |         |         |         |      |   0011   SGTE read                  |
 * |         |         |         |      |   Others Reserved                   |
 * +---------+---------+---------+------+-------------------------------------+
 * | INV     | <51>    | RO Rev1 | 0    | Info Not Valid - only meaningful    |
 * |         |         | RAZ Rev0|      | when one of bits <11:0> is set.     |
 * |         |         |         |      | Indicates validity of <SYN>, <CMD>, |
 * |         |         |         |      | and <ADDR> fields.                  |
 * |         |         |         |      |   Val Mode                          |
 * |         |         |         |      |   0   Info fields are valid.        |
 * |         |         |         |      |   1   Info fields are not valid.    |
 * +---------+---------+---------+------+-------------------------------------+
 * | ADDR    | <50:16> | RO      | 0    | If CRE or UECC, then ADDR<50:19> =  |
 * |         |         |         |      | system address <34:3> of erroneous  |
 * |         |         |         |      | quadword and ADDR<18:16> = 0.       |
 * |         |         |         |      | If not CRE and not UECC, then       |
 * |         |         |         |      | ADDR<50:48> = 0; ADDR<47:18> = star-|
 * |         |         |         |      | ting PCI address <31:2> of trans-   |
 * |         |         |         |      | action when error was detected;     |
 * |         |         |         |      | ADDR<17:16> = 00 --> not a DAC      |
 * |         |         |         |      |                      operation;     |
 * |         |         |         |      | ADDR<17:16> = 01 --> via DAC SG     |
 * |         |         |         |      |                      Window 3;      |
 * |         |         |         |      | ADDR<17> = 1 --> via Monster Window |
 * +---------+---------+---------+------+-------------------------------------+
 * | RES     | <15:12> | MBZ,RAZ | 0    | Reserved.                           |
 * +---------+---------+---------+------+-------------------------------------+
 * | CRE     | <11>    | R,W1C   | 0    | Correctable ECC error.              |
 * +---------+---------+---------+------+-------------------------------------+
 * | UECC    | <10>    | R,W1C   | 0    | Uncorrectable ECC error.            |
 * +---------+---------+---------+------+-------------------------------------+
 * | RES     | <9>     | MBZ,RAZ | 0    | Reserved.                           |
 * +---------+---------+---------+------+-------------------------------------+
 * | NDS     | <8>     | R,W1C   | 0    | No b_devsel_l as PCI master.        |
 * +---------+---------+---------+------+-------------------------------------+
 * | RDPE    | <7>     | R,W1C   | 0    | PCI read data parity error as PCI   |
 * |         |         |         |      | master.                             |
 * +---------+---------+---------+------+-------------------------------------+
 * | TA      | <6>     | R,W1C   | 0    | Target abort as PCI master.         |
 * +---------+---------+---------+------+-------------------------------------+
 * | APE     | <5>     | R,W1C   | 0    | Address parity error detected as    |
 * |         |         |         |      | potential PCI target.               |
 * +---------+---------+---------+------+-------------------------------------+
 * | SGE     | <4>     | R,W1C   | 0    | Scatter-gather had invalid page     |
 * |         |         |         |      | table entry.                        |
 * +---------+---------+---------+------+-------------------------------------+
 * | DCRTO   | <3>     | R,W1C   | 0    | Delayed completion retry timeout as |
 * |         |         |         |      | PCI target.                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | PERR    | <2>     | R,W1C   | 0    | b_perr_l sampled asserted.          |
 * +---------+---------+---------+------+-------------------------------------+
 * | SERR    | <1>     | R,W1C   | 0    | b_serr_l sampled asserted.          |
 * +---------+---------+---------+------+-------------------------------------+
 * | LOST    | <0>     | R,W1C   | 0    | Lost an error because it was detec- |
 * |         |         |         |      | ted after this register was frozen, |
 * |         |         |         |      | or while in the process of clearing |
 * |         |         |         |      | this register.                      |
 * +---------+---------+---------+------+-------------------------------------+
 * \endcode
 *
 * Pchip Error Mask Register (PERRMASK - RW)
 *
 * If any of the MASK bits have the value 0, they prevent the setting of the
 *corresponding bit in the PERROR register, regardless of the detection of
 *errors or writing to PERRSET.
 *
 * The default is for all errors to be disabled.
 *
 * Beside masking the reporting of errors in PERROR, certain bits of PERRMASK
 *have the following additional effects:
 *   - If PERRMASK<RDPE> = 0, the Pchip ignores read data parity as the PCI
 *master.
 *   - If PERRMASK<PERR> = 0, the Pchip ignores write data parity as the PCI
 *target.
 *   - If PERRMASK<APE> = 0, the Pchip ignores address parity.
 *   .
 *
 * \code
 * +---------+---------+---------+------+-------------------------------------+
 * | Field   | Bits    | Type    | Init | Description                         |
 * +---------+---------+---------+------+-------------------------------------+
 * | RES     | <63:12> | MBZ,RAZ | 0    | Reserved                            |
 * +---------+---------+---------+------+-------------------------------------+
 * | MASK    | <11:0>  | RW      | 0    | PERROR register bit enables         |
 * +---------+---------+---------+------+-------------------------------------+
 * \endcode
 *
 * Pchip Master Latency Register (PLAT - RW)
 *
 * Bits <15:8> are the master latency timer.
 *
 * Translation Buffer Invalidate Virtual Register (TLBIV - WO)
 *
 * A write to this register invalidates all scatter-gather TLB entries that
 *correspond to PCI addresses whose bits <31:16> and bit 39 match the value
 *written in bits <19:4> and 27 respectively. This invalidates up to eight PTEs
 *at a time, which are the number that can be defined in one 21264 cache block
 *(64 bytes). Because a single TLB PCI tag covers four entries, at most two tags
 *are actually invalidated. PTE bits <22:4> correspond to system address bits
 *<34:16> - where PCI<34:32> must be zeros for scatter- gather window hits - in
 *generating the resulting system address, providing 8-page (8KB) granularity.
 *
 * Translation Buffer Invalidate All Register (TLBIA - WO)
 *
 * A write to this register invalidates the scatter-gather TLB. The value
 *written is ignored.
 **/
u64 CTsunami::pchip_csr_read(int num, u32 a) {
  switch (a) {
  case 0x000:
  case 0x040:
  case 0x080:
  case 0x0c0:
    return state.pchip[num].wsba[(a >> 6) & 3];

  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0:
    return state.pchip[num].wsm[(a >> 6) & 3];

  case 0x200:
  case 0x240:
  case 0x280:
  case 0x2c0:
    return state.pchip[num].tba[(a >> 6) & 3];

  case 0x300:
    return state.pchip[num].pctl;

  case 0x3c0:
    return state.pchip[num].perr;

  case 0x400:
    return state.pchip[num].perrmask;

  case 0x480: // TLBIV
  case 0x4c0: // TLBIA
    return 0;

  case 0x800: // PCI reset
    return 0;

  default:
    printf("Unknown PCHIP %d CSR %07x read attempted.\n", num, a);
    return 0;
  }
}

/**
 * \brief Write one of the PCHIP registers.
 *
 * For a description of the PCHIP registers, see pchip_csr_read.
 **/
void CTsunami::pchip_csr_write(int num, u32 a, u64 data) {
  switch (a) {
  case 0x000:
  case 0x040:
  case 0x080:
    state.pchip[num].wsba[(a >> 6) & 3] = data & U64(0x00000000fff00003);
    return;

  case 0x0c0:
    state.pchip[num].wsba[3] = (data & U64(0x00000080fff00001)) | 2;
    return;

  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0:
    state.pchip[num].wsm[(a >> 6) & 3] = data & U64(0x00000000fff00000);
    return;

  case 0x200:
  case 0x240:
  case 0x280:
  case 0x2c0:
    state.pchip[num].tba[(a >> 6) & 3] = data & U64(0x00000007fffffc00);
    return;

  case 0x300:
    state.pchip[num].pctl &= U64(0xffffe300f0300000);
    state.pchip[num].pctl |= (data & U64(0x00001cff0fcfffff));
    return;

  case 0x340:
    state.pchip[num].plat = data;
    return;

  case 0x3c0: // PERR
    return;

  case 0x400:
    state.pchip[num].perrmask = data;
    return;

  case 0x480: // TLBIV
  case 0x4c0: // TLBIA
    return;

  case 0x800: // PCI reset
    m_sys->reset_pci_devices();
    return;

  default:
    printf("Unknown PCHIP %d CSR %07x write with %016" PRIx64 " attempted.\n",
           num, a, data);
  }
}

/**
 * \brief Translate a 32-bit address coming off the PCI bus into a
 * 64-bit system address. Used by PCI devices when accessing
 * memory (or other PCI devices) as bus master.
 *
 * Source: HRM, 10.1.4:
 *
 * DMA Address Translation (PCI-to-System)
 * The 21272 chipset supports some PCI commands as a target and does not support
 * (ignores) others as a target. The Pchip does not respond as a target when it
 *acts as a PCI master.
 *
 * The Pchip ignores all of the following commands as a target:
 *  - Interrupt acknowledge
 *  - Special cycle
 *  - I/O read
 *  - I/O write
 *  - Configuration read
 *  - Configuration write
 *  .
 *
 * The Pchips may respond to the following commands as a target:
 *  - Memory read
 *  - Memory read line
 *  - Memory write
 *  - Memory write and invalidate
 *  - Memory read multiple
 *  - Dual-address cycle: This command is accepted by the Pchip when the address
 *lies inside the DMA monster window.
 *
 * There are two kinds of DMA address translation: direct mapped and
 *scatter-gather mapped. Each type starts by comparing the incoming PCI address
 *with the monster window (if it is enabled and if it is a DAC), and with the
 *four window base and window mask registers (the window base registers also
 *have an enable window bit and a scatter-gather enable bit). This process is
 *shown in the next figure:
 *
 * \code
 *              31       n n-1      20 19    13 12       0
 *             +----------+-----------+--------+----------+
 * PCI Address |    Peripheral Page Number     |  Offset  |
 *             +----------+-----------+--------+----------+
 *             |<-------->|
 *                  ^
 *                  +-----------> COMPARE ----> Hit
 *                  v
 *             |<-------->|
 *              31       n n-1      20
 * Window Base +----------+-----------+
 * Register    |          |   xxxx    |
 *             +----------+-----------+
 *              31       n n-1      20
 * Window Mask +----------+-----------+
 * Register    |   0000   |   1111    | (Determines n)
 *             +----------+-----------+
 * \endcode
 *
 * If the address resides in one of the windows, and the window is enabled, then
 *if the scatter-gather enable bit is set, the translation is as described for
 * pci_phys_scatter_gather. Otherwise, the translation described for
 *pci_phys_direct_mapped is used.
 *
 * In addition, if the matching window has the PTP bit set, then the result of
 *the address translation is treated as if it had bit <43> set. That is, it is
 *treated like a PIO address from the CPU. Otherwise, the address is a system
 *memory address.
 *
 * Window Hole
 *
 * All window registers are simultaneously subject to a hole that inhibits
 *matching, under the control of the PCTL<HOLE> CSR bit described in
 *Section 10.2.5.4. If that bit is set, the hole is enabled in all windows and
 *has the following extent:
 *  - From PCI address base 512K (address<31:0> = 0008.0000)
 *  - To PCI address limit 1M-1 (address<31:0> = 000F.FFFF)
 *  .
 *
 * If enabled, the hole applies whether or not the PTP bit is set for the
 *window.
 *
 * The documentation is not explicit on this, but the assumption was made that
 *if an address coming off the PCI-bus is not matched, the Pchip doesn't respond
 *to that address, and it is up to other PCI devices to respond to the address.
 *So, if no match is found, we treat the address as an address on the local PCI
 *bus.
 *
 * \todo The documentation mentions a PTP bit set for a window, but the register
 *descriptions don't show a PTP bit in one of the three registers (WSBA, WSM and
 *TBA). So, for now, we can only do peer-to-peer through a scatter-gather PTE.
 *
 * \todo Dual-Acces-Cycle (DAC) access from the PCI bus is not supported. If a
 *device is ever added that uses this, we should probably support it.
 **/
u64 CTsunami::pci_phys(int pcibus, u32 address) {
  u64 a;
  int j;

#if defined(DEBUG_PCI)
  printf("-------------- PCI MEMORY ACCESS FOR PCI HOSE %d --------------\n",
         pcibus);

  // Step through windows
  for (j = 0; j < 4; j++) {
    printf("WSBA%d: %016" PRIx64 " WSM: %016" PRIx64 " TBA: %016" PRIx64 "\n",
           j, state.pchip[pcibus].wsba[j], state.pchip[pcibus].wsm[j],
           state.pchip[pcibus].tba[j]);
  }

  printf("HOLE: %s\n",
         test_bit_64(state.pchip[pcibus].pctl, 5) ? "enabled" : "disabled");
  printf("--------------------------------------------------------------\n");
#endif
  if (!(state.pchip[pcibus].pctl & PCI_PCTL_HOLE) // hole disabled
      || (address < PCI_PCTL_HOLE_START) ||
      (address > PCI_PCTL_HOLE_END)) // or address outside hole
  {

    // Step through windows
    for (j = 0; j < 4; j++) {
      if ((state.pchip[pcibus].wsba[j] & 1) // window enabled...
          && !((address ^ state.pchip[pcibus].wsba[j]) & 0xfff00000 &
               ~state.pchip[pcibus].wsm[j])) // address in range...
      {
        if (state.pchip[pcibus].wsba[j] & 2) {
          try {
            a = pci_phys_scatter_gather(address, state.pchip[pcibus].wsm[j],
                                        state.pchip[pcibus].tba[j]);
          }

          catch (char) {

            // window disabled...
            // not matched; treat as local PCI bus address
            return U64(0x80000000000) | (pcibus * U64(0x200000000)) |
                   (u64)address;
          }
        } else
          a = pci_phys_direct_mapped(address, state.pchip[pcibus].wsm[j],
                                     state.pchip[pcibus].tba[j]);
#if defined(DEBUG_PCI)
        printf("PCI memory address %08x translated to %016" PRIx64 "\n",
               address, a);
#endif
        return a;
      }
    }
  }

  // not matched; treat as local PCI bus address
  return U64(0x80000000000) | (pcibus * U64(0x200000000)) | (u64)address;
}

/**
 * Translate a 32-bit address coming off the PCI bus into a 64-bit
 * system address using direct-mapped DMA address translation.
 *
 * Source: HRM, 10.1.4.2:
 *
 * Direct-Mapped DMA Address Translation
 *
 * Direct-mapped addressing uses a base address register, a translated base
 *address (TBA) register, and a mask register. The block of PCI addresses at
 *base address, of a size as determined by the mask register, is translated to a
 *block of addresses at translated base address. Values in the WSMn field other
 *than those shown produce unspecified results.
 *
 * \code
 * +-------------+----------------+---------------------------+
 * | Window Size | WSMn<31:20>    | Translated Address <34:0> |
 * +-------------+----------------+---------------------------+
 * |         1MB | 0000.0000.0000 | TBA<34:20>:ad<19:0>       |
 * +-------------+----------------+---------------------------+
 * |         2MB | 0000.0000.0001 | TBA<34:21>:ad<20:0>       |
 * +-------------+----------------+---------------------------+
 * |         4MB | 0000.0000.0011 | TBA<34:22>:ad<21:0>       |
 * +-------------+----------------+---------------------------+
 * |         8MB | 0000.0000.0111 | TBA<34:23>:ad<22:0>       |
 * |        ...  |           ...  |                ...        |
 * |         2GB | 0111.1111.1111 | TBA<34:31>:ad<30:0>       |
 * +-------------+----------------+---------------------------+
 * |         4GB |            N/A | 000:ad<31:0> (monster     |
 * |             |                | window only)              |
 * +-------------+----------------+---------------------------+
 * \endcode
 **/
u64 CTsunami::pci_phys_direct_mapped(u32 address, u64 wsm, u64 tba) {
  return pci_window::direct(address, wsm, tba);
}

/**
 * Translate a 32-bit address coming off the PCI bus into a 64-bit
 * system address using scatter-gather DMA address translation.
 *
 * If address can't be matched (PTE is invalid), an exception of type char
 * is thrown. The calling function should catch the exception, and do
 * The Right Thing(tm): treat the address as a local PCI-bus address.
 *
 * Source: HRM, 10.1.4.3:
 *
 * Scatter-Gather DMA Address Translation
 *
 * Scatter-gather addressing uses a base address register, a mask register, a
 *translated base address register, and a page table entry (PTE) in system
 *memory. An 8KB page of PCI addresses at base address is translated to an 8KB
 *page of system addresses through one level of indirection. The PTE contains
 *the address of the 8KB page.
 * [...]
 * At TBA is a region (of size SG PTE AREA) of PTEs, each of which is eight
 *bytes. Bits <22:1> of the PTE become bits <34:13> (the 8KB page) of the system
 *address, and bits <12:0> of the PCI address become bits <12:0> (the page
 *offset) of the system address.
 *
 * The following table shows how the address of the page table entry (to be used
 *as part of the final system address) is generated. Values in the WSM field
 *other than those shown produce unspecified results.
 *
 * \code
 * +-------------+-------------+----------------+----------------------+
 * | Window Size | SG PTE AREA | WSMn<31:20>    | PTE Address <34:3>   |
 * +-------------+-------------+----------------+----------------------+
 * |         1MB |         1KB | 0000.0000.0000 | TBA<34:10>:ad<19:13> |
 * +-------------+-------------+----------------+----------------------+
 * |         2MB |         2KB | 0000.0000.0001 | TBA<34:11>:ad<20:13> |
 * +-------------+-------------+----------------+----------------------+
 * |         4MB |         4KB | 0000.0000.0011 | TBA<34:12>:ad<21:13> |
 * +-------------+-------------+----------------+----------------------+
 * |         8MB |         8KB | 0000.0000.0111 | TBA<34:13>:ad<22:13> |
 * |        ...  |        ...  |           ...  |                ...   |
 * |         2GB |         2MB | 0111.1111.1111 | TBA<34:21>:ad<30:13> |
 * +-------------+-------------+----------------+----------------------+
 * |         4GB |         4MB |            N/A | TBA<34:22>:ad<31:13> |
 * |             |             |                | (Window 3 in DAC     |
 * |             |             |                | mode only)           |
 * +-------------+-------------+----------------+----------------------+
 * \endcode
 *
 * The following figure shows the structure of a page table entry in memory. If
 *either bit <31> or bit <28> is set, the address is interpreted as being a
 *peer-to-peer address.
 *
 * \code
 *  63               32 31 30 29 28 27      23 22                   1 0
 * +-------------------+--+-----+--+----------+----------------------+-+
 * |                   |PP|     |PP|          | Page Address <34:13> |V|
 * +-------------------+--+-----+--+----------+----------------------+-+
 *                                                                    +--> V =
 *valid bit \endcode
 *
 * The last figure shows how a page table entry is used in conjunction with an
 *incoming PCI address to generate a system address.
 *
 * \code
 *        PTE <22:1>              PCI address <12:0>
 *             |                         |
 *  34         v              13 12      v         0
 * +----------------------------+-------------------+
 * |   Page addres <34:13>      |  Offset <12:0>    |
 * +----------------------------+-------------------+
 * \endcode
 **/
u64 CTsunami::pci_phys_scatter_gather(u32 address, u64 wsm, u64 tba) {
  const u64 pte =
      m_sys->ReadMem(pci_window::pte_address(address, wsm, tba), 64, 0);
  if (!(pte & 1))
    throw((char)'0');
  return pci_window::from_pte(pte, address);
}
