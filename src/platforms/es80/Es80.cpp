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
 * The AlphaServer ES80 (docs/platforms/marvel.md, "M6c"): up to four 2P
 * drawers -- each the ES47's drawer: one dual-processor module with its
 * CMM, a backplane with an IO7 on one processor's I/O port, an MBM -- whose
 * processors are cabled through their N/S ports into one ring of up to
 * eight (GS1280 Technical Summary: "ES80 Systems", "Interprocessor
 * Connectivity").
 *
 * Processor n sits at NS n, EW 0; the console numbers it (coord2id, system
 * type 0x11) PID (n & 1) | (n >> 1) << 3: drawer d holds PIDs 8d and 8d + 1,
 * and its memory starts at the PID's base (PID 8 at 0x40_0000_0000, as a
 * real GS1280's listing has it). Each drawer's backplane IO7 hangs on its
 * first processor, PID 8d, giving hoses 32d to 32d + 3: an ES80 Model 8 has
 * IO7s at PIDs 0, 8, 16 and 24. The other processor's I/O port can be
 * cabled to an expansion drawer, which is not modelled.
 **/
#include "StdAfx.hpp"

#include "Boards.hpp"
#include "Topology.hpp"

namespace {

void es80_coordinates(int index, u8 *ns, u8 *ew) {
  *ns = (u8)index;
  *ew = 0;
}

/// The drawer backplane's IO7, on the drawer's first processor.
bool es80_has_io7(u32 pid) { return (pid & 7) == 0; }

} // namespace

/// System type 0x11 with <19:16> 1: "ES80" (build_dsrdb, 0x2dd8f0).
const marvel_layout es80_layout = {0x10011, es80_coordinates, es80_has_io7};

void es80_board_devices(CConfigurator *cfg, CSystem *sys) {
  marvel_board_devices(cfg, sys);
}
