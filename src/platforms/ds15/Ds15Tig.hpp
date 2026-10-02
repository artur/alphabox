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
 * The two TIG-bus registers the DS15 answers differently from the ES45's TIG
 * (chipsets/titan/TitanTig.cpp): psir, whose bit 7 the DS15 console reads
 * as "the RMC is ready", and the halt register at 0x5c0, which on the DS15
 * does not halt the processor (docs/platforms/ds15.md, Findings). The rest of
 * the TIG bus stays the chipset's.
 **/
#if !defined(INCLUDED_DS15_TIG_H_)
#define INCLUDED_DS15_TIG_H_

#include "SystemComponent.hpp"

class CDs15Tig : public CSystemComponent {
public:
  CDs15Tig(CConfigurator *cfg, class CSystem *c);
  u64 ReadMem(int index, u64 address, int dsize) override;
  void WriteMem(int index, u64 address, int dsize, u64 data) override;
  int SaveState(FILE *f) override;
  int RestoreState(FILE *f) override;

private:
  u8 m_halt = 0; ///< what was written to 0x5c0
};

#endif // !defined(INCLUDED_DS15_TIG_H_)
