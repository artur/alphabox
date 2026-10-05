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

/* The overlay scaler's arithmetic (RadeonOverlay.cpp), apart from the
 * card: the filter at a phase and the YUV to RGB transform. */

#if !defined(INCLUDED_RADEON_OVERLAY_H)
#define INCLUDED_RADEON_OVERLAY_H

#include <cstdint>

#include "datatypes.hpp"

namespace radeon {

/// How a plane is sampled along one axis.
enum class OvFilter { Nearest, Linear, FourTap };

/// The weights, in 32nds, of the taps at pixels i - 1 .. i + 2 for a
/// position `frac` 32nds past pixel i; `coef` is OV0_FOUR_TAP_COEF_0..4.
void overlay_filter_weights(OvFilter kind, const u32 coef[5], unsigned frac,
                            int w[4]);
/// Y, Cb, Cr (eight bits each) through OV0_LIN_TRANS_A..F (`lin`) to
/// eight-bit R, G, B.
void overlay_to_rgb(const u32 lin[6], int y, int cb, int cr, int rgb[3]);

} // namespace radeon

#endif // !defined(INCLUDED_RADEON_OVERLAY_H)
