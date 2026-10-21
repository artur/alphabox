/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * Forked from: ES40 emulator
 * Copyright (C) 2007-2008 by the ES40 Emulator Project
 * Copyright (C) 2007 by Camiel Vanderhoeven
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

/* The initial offset of a guest TOY clock from the host clock, from the
 * configuration: shared by the ALi M1543C's RTC and the Marvel CMM's. */

#if !defined(INCLUDED_TOYSEED_H)
#define INCLUDED_TOYSEED_H

/// The TOY's initial offset from the host clock, in seconds, from the system
/// block's `time` (may be null) or else a `timezone` string ("local"/"utc"
/// plus an optional "+<n>"/"-<n>" with unit s|m|h|d|M|y). Throws a
/// configuration error for a bad value; `current` is kept if the result is
/// unrepresentable.
long toy_seed_offset(const char *faketime, const char *tz, long current);

#endif // !defined(INCLUDED_TOYSEED_H)
