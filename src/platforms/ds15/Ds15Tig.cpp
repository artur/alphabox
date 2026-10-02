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
#include "Ds15Tig.hpp"
#include "StdAfx.hpp"
#include "System.hpp"

namespace {
const u64 TIG_PSIR = U64(0x801300000c0); ///< psir, TIG offset 0x0c0
const u64 TIG_HALT = U64(0x801300005c0); ///< ev6_halt, TIG offset 0x5c0
const u64 TIG_TRR = U64(0x80130000000);  ///< trr, TIG offset 0: revision
const u32 ds15_tig_magic1 = 0xD515716A;
const u32 ds15_tig_magic2 = 0xA617515D;
} // namespace

CDs15Tig::CDs15Tig(CConfigurator *cfg, CSystem *c) : CSystemComponent(cfg, c) {
  c->RegisterMemory(this, 0, TIG_PSIR, 0x40);
  c->RegisterMemory(this, 1, TIG_HALT, 0x40);
  c->RegisterMemory(this, 2, TIG_TRR, 0x40);
}

u64 CDs15Tig::ReadMem(int index, u64 address, int dsize) {
  (void)address;
  (void)dsize;
  if (index == 0) {
    // psir<7>: the RMC is ready (the console's report_rmc_errors, 0x91960
    // in the update utility, prints "RMC is not Ready" when it is clear).
    // The other bits are not known and read 0.
    return 0x80;
  }
  if (index == 2) {
    // The TIG's revision: the DS15 console prints the byte as "<7:4>.<3:0>"
    // (the ES45's adds 2 to the upper digit), and the owner's guide's show
    // config (EK-DS150-OG, example 2-5) has "TIG Rev 1.9".
    return 0x19;
  }
  return m_halt;
}

void CDs15Tig::WriteMem(int index, u64 address, int dsize, u64 data) {
  (void)address;
  (void)dsize;
  // psir: the PALcode writes 0 to it at reset; nothing is known to follow.
  // trr is read-only.
  if (index == 1) {
    // The DS15 console's PALcode writes 0xf here at reset and nothing
    // clears it again (the ES45 and DS25 consoles clear the primary's bit
    // once it has won arbitration). With the ES40's meaning -- a set bit
    // holds that processor's halt line -- the one processor took a halt
    // interrupt without end and the console never saw its interval timer.
    // So on the DS15 it is kept and does nothing [assumed: what the
    // register does on the DS15 is not known].
    m_halt = (u8)data;
  }
}

int CDs15Tig::SaveState(FILE *f) {
  fwrite(&ds15_tig_magic1, sizeof(u32), 1, f);
  fwrite(&m_halt, sizeof(m_halt), 1, f);
  fwrite(&ds15_tig_magic2, sizeof(u32), 1, f);
  return 0;
}

int CDs15Tig::RestoreState(FILE *f) {
  u32 m1 = 0, m2 = 0;
  if (fread(&m1, sizeof(u32), 1, f) != 1 || m1 != ds15_tig_magic1 ||
      fread(&m_halt, sizeof(m_halt), 1, f) != 1 ||
      fread(&m2, sizeof(u32), 1, f) != 1 || m2 != ds15_tig_magic2) {
    printf("%s: state does not match\n", devid_string);
    return -1;
  }
  return 0;
}
