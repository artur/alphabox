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

#include "NetworkNull.hpp"

bool CNetworkNull::init(const char *devid_string, CConfigurator *cfg) {
  printf("%s: not connected to a host network (type = null): transmits are "
         "discarded and nothing is ever received.\n",
         devid_string);
  return true;
}

int CNetworkNull::send(const u8 *data, int len) {
  // Report success: the guest driver should see its transmit complete, not
  // a permanently failing NIC.
  return 0;
}

int CNetworkNull::receive(const u8 **data, int *len) {
  *data = nullptr;
  *len = 0;
  return 0; // no packet available
}

void CNetworkNull::set_filter(const NetworkFilter &filter) {
  // Filtering is meaningless when nothing arrives.
}

void CNetworkNull::close() {}
