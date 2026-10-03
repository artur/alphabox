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
 * The AlphaServer GS1280 (docs/platforms/marvel.md, "M6c"): 8P drawers of
 * four dual-processor modules, every processor using all four
 * interprocessor ports, the rows and columns closed into a torus, and the
 * I/O in separate drawers cabled to processors' I/O ports (GS1280
 * Technical Summary). Here up to two drawers, sixteen processors.
 *
 * Processor n is PID n: the console's GS1280 numbering (coord2id, system
 * type 1) puts the place on the module in PID<0>, the module (the E/W
 * coordinate) in PID<2:1> and the drawer (N/S bit 1) in PID<3>. So the
 * first eight are one drawer, NS 0-1 by EW 0-3, as the 8-processor power-up
 * in the Installation Information draws its CPU grid; sixteen make a 4x4
 * torus. One I/O drawer, on PID 0 [a choice: real systems cable as many as
 * the customer bought].
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Topology.hpp"

namespace {

void gs1280_coordinates(int index, u8 *ns, u8 *ew) {
  *ns = (u8)((index & 1) | ((index >> 3) & 1) << 1);
  *ew = (u8)((index >> 1) & 3);
}

bool gs1280_has_io7(u32 pid) { return pid == 0; }

} // namespace

/// System type 1: "GS1280" (build_dsrdb, 0x2dd8f0).
/// The I/O backplane's revision is not known: 0.
const marvel_layout gs1280_layout = {0x1, gs1280_coordinates, gs1280_has_io7,
                                     0};

void gs1280_board_devices(CConfigurator *cfg, CSystem *sys) {
  marvel_board_devices(cfg, sys);
}
