/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Website: https://github.com/lenticularis39/axpbox
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

#include "NetworkBackend.hpp"
#include "Configurator.hpp"
#include "StdAfx.hpp"

#if defined(HAVE_PCAP)
#include "NetworkPcap.hpp"
#endif

#if defined(__linux__)
#include "NetworkTap.hpp"
#endif

#include "NetworkNull.hpp"

CNetworkBackend *create_network_backend(CConfigurator *cfg) {
  char *type = cfg->get_text_value("type");

  // Not connected to anything: the NIC exists and behaves, but transmits go
  // nowhere and nothing is ever received. Always available, because it needs
  // no host privileges -- pcap capture wants root and TAP is Linux-only,
  // and a backend that cannot be created is fatal at device construction.
  if (type && strcasecmp(type, "null") == 0)
    return new CNetworkNull();

  if (type && strcasecmp(type, "tap") == 0) {
#if defined(__linux__)
    return new CNetworkTap();
#else
    printf("TAP networking is only supported on Linux.\n");
    return nullptr;
#endif
  }

  // Default: pcap
#if defined(HAVE_PCAP)
  return new CNetworkPcap();
#else
  printf("pcap networking not available (compiled without pcap support).\n");
  return nullptr;
#endif
}
