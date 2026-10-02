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
 * The ES45's dual-port RAM: the ES40's (platforms/es40/DPR.hpp), which the
 * ES45 console reads the same way, plus what the ES45 console asks of it
 * that the ES40's does not (docs/platforms/es45.md, Findings).
 **/
#if !defined(INCLUDED_ES45_DPR_H_)
#define INCLUDED_ES45_DPR_H_

#include "DPR.hpp"

class CEs45Dpr : public CDPR {
public:
  CEs45Dpr(CConfigurator *cfg, class CSystem *c) : CDPR(cfg, c) {}

protected:
  void board_init() override;
  bool board_command(u8 command) override;
};

#endif // !defined(INCLUDED_ES45_DPR_H_)
