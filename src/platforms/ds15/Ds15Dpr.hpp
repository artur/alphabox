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
 * The DS15's RMC as its console sees it: the ES40's dual-port RAM
 * (platforms/es40/DPR.hpp) where the DS15 console reads it the same way, a
 * command mailbox at 0xb00 instead of the ES40's at 0xf9, and status bytes
 * the DS15 reads differently (docs/platforms/ds15.md, Findings).
 **/
#if !defined(INCLUDED_DS15_DPR_H_)
#define INCLUDED_DS15_DPR_H_

#include "DPR.hpp"

class CDs15Dpr : public CDPR {
public:
  CDs15Dpr(CConfigurator *cfg, class CSystem *c);
  void WriteMem(int index, u64 address, int dsize, u64 data) override;

protected:
  void board_init() override;
};

#endif // !defined(INCLUDED_DS15_DPR_H_)
