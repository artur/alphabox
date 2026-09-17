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

#if !defined(INCLUDED_NIC_ADDRESS_H)
#define INCLUDED_NIC_ADDRESS_H

#include "StdAfx.hpp"

class CConfigurator;

/**
 * The station address a NIC starts with.
 *
 * The device's "mac" config value (xx-xx-xx-xx-xx-xx, with '-', ':' or '.'
 * separators) when present; otherwise the Digital prefix 08-00-2B followed
 * by E5-40 (hexified "ES40") and a number counting every NIC in the
 * system, so two adapters of any kind never share an address.
 */
void nic_station_address(CConfigurator *cfg, const char *devid_string,
                         u8 mac[6]);

#endif // !defined(INCLUDED_NIC_ADDRESS_H)
