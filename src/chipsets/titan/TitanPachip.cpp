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
 * The Titan PA-chip ports: each PA-chip has a G-port and an A-port, and each
 * port is a PCI hose with its own CSRs (Linux core_titan.h,
 * titan_pachip_port), 0x1000 bytes apart:
 *
 * \code
 * 000-0C0 WSBA0-3   100-1C0 WSM0-3   200-2C0 TBA0-3   300 PCTL   340 PLAT
 *          G-port              A-port
 * 400      SERROR              AGPERROR
 * 440      SERREN              AGPERREN
 * 480      SERRSET             AGPERRSET
 * 4C0      -                   AGPLASTWR
 * 500      GPERROR             APERROR
 * 540      GPERREN             APERREN
 * 580      GPERRSET            APERRSET
 * 600      GTLBIV              ATLBIV
 * 640      GTLBIA              ATLBIA
 * 700      SCTL                -
 * 800 SPRST (PCI reset)
 * \endcode
 *
 * The windows translate as the Tsunami's (PciWindows.hpp). No error is ever
 * detected, so the error registers read as zero.
 **/
#include "StdAfx.hpp"
#include "System.hpp"
#include "Titan.hpp"

/// PCTL bits a write does not change: <17> PCISPD66 (a strap) and the
/// A-port's <57> AGP_PRESENT [both read-only per the field names in Linux
/// core_titan.h; that they are read-only is assumed].
static const u64 PCTL_RO = U64(0x0200000000020000);

u64 CTitan::port_read(int hose, u32 a) {
  const SState::SPort &p = state.port[hose];
  const u32 i = a >> 6;
  switch (a) {
  case 0x000:
  case 0x040:
  case 0x080:
  case 0x0c0: // WSBA
  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0: // WSM
  case 0x200:
  case 0x240:
  case 0x280:
  case 0x2c0: // TBA
  case 0x300: // PCTL
  case 0x340: // PLAT
  case 0x440: // SERREN / AGPERREN
  case 0x4c0: // AGPLASTWR
  case 0x540: // GPERREN / APERREN
  case 0x700: // SCTL
  case 0x800: // SPRST
    return p.csr[i];
  case 0x400: // SERROR / AGPERROR
  case 0x500: // GPERROR / APERROR
    return 0;
  case 0x480: // SERRSET / AGPERRSET (write only)
  case 0x580: // GPERRSET / APERRSET (write only)
  case 0x600: // TLBIV (write only)
  case 0x640: // TLBIA (write only)
    return 0;
  default:
    m_sys->trace_unknown("PA-chip port CSR", (u64)hose << 12 | a, 64, false, 0,
                         nullptr);
    return 0;
  }
}

void CTitan::port_write(int hose, u32 a, u64 data) {
  SState::SPort &p = state.port[hose];
  const u32 i = a >> 6;
  switch (a) {
  case 0x000:
  case 0x040:
  case 0x080:
    p.csr[i] = data & U64(0x00000000fff00003);
    return;
  case 0x0c0: // window 3 is scatter-gather only (Linux core_titan.c)
    p.csr[i] = (data & U64(0x00000080fff00001)) | 2;
    return;
  case 0x100:
  case 0x140:
  case 0x180:
  case 0x1c0:
    p.csr[i] = data & U64(0x00000000fff00000);
    return;
  case 0x200:
  case 0x240:
  case 0x280:
  case 0x2c0:
    p.csr[i] = data & U64(0x00000007fffffc00);
    return;
  case 0x300:
    p.csr[i] = (p.csr[i] & PCTL_RO) | (data & ~PCTL_RO);
    return;
  case 0x340:
  case 0x440:
  case 0x4c0:
  case 0x540:
    p.csr[i] = data;
    return;
  case 0x700: // SCTL: <7:0> is the revision [read-only: assumed]
    p.csr[i] = (data & ~U64(0xff)) | (p.csr[i] & 0xff);
    return;
  case 0x400: // error registers: write 1 to clear; none are ever set
  case 0x480:
  case 0x500:
  case 0x580:
  case 0x600: // the scatter-gather TLB is not modelled: nothing to drop
  case 0x640:
    return;
  case 0x800: // SPRST: PCI reset while set
    if ((data & 1) && !(p.csr[i] & 1))
      m_sys->reset_pci_devices();
    p.csr[i] = data & 1;
    return;
  default:
    m_sys->trace_unknown("PA-chip port CSR", (u64)hose << 12 | a, 64, true,
                         data, nullptr);
  }
}
