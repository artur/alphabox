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
 * The DS25's dual-port RAM: the ES40's (platforms/es40/DPR.hpp) with what
 * the DS25 console asks of its RMC that the ES40's does not
 * (docs/platforms/ds25.md, Findings).
 **/
#if !defined(INCLUDED_DS25_DPR_H_)
#define INCLUDED_DS25_DPR_H_

#include "DPR.hpp"

class CDs25Dpr : public CDPR {
public:
  CDs25Dpr(CConfigurator *cfg, class CSystem *c) : CDPR(cfg, c) {}

protected:
  void board_init() override;
  bool board_command(u8 command) override;
};

#endif // !defined(INCLUDED_DS25_DPR_H_)
