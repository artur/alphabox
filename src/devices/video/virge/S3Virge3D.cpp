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
 * S3 ViRGE S3d engine, 3D commands.
 **/

#include "S3Virge.hpp"

using namespace virge;

void CS3Virge::s3d_start(u32 cmd) {
  const u32 op = (cmd >> CMD_COMMAND_SHIFT) & 15;
  char what[64];
  snprintf(what, sizeof(what), "3D command %u", op);
  unimplemented(what);
}

void CS3Virge::s3d_triangle() {}

void CS3Virge::s3d_line() {}
