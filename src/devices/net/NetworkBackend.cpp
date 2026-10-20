/* Alphabox Alpha Emulator
 * Copyright (C) 2020 Tomáš Glozar
 * Copyright (C) 2026 Hendrik Volkmer
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

#include "NetworkLoopback.hpp"
#include "NetworkNull.hpp"
#include "NetworkUdp.hpp"

const char *network_backend_refusal(const char *type) {
  if (!type || !strcasecmp(type, "pcap")) {
#if defined(HAVE_PCAP)
    return nullptr;
#else
    return "pcap networking not available (compiled without pcap support); "
           "type = \"udp\", \"loopback\" or \"null\" need nothing";
#endif
  }
  if (!strcasecmp(type, "tap")) {
#if defined(__linux__)
    return nullptr;
#else
    return "TAP networking is only supported on Linux";
#endif
  }
  if (!strcasecmp(type, "null") || !strcasecmp(type, "loopback") ||
      !strcasecmp(type, "udp"))
    return nullptr;
  return "unknown network type: pcap, tap, udp, loopback or null";
}

CNetworkBackend *create_network_backend(CConfigurator *cfg) {
  char *type = cfg->get_text_value("type");
  if (const char *why = network_backend_refusal(type)) {
    printf("%s: type = \"%s\": %s.\n", cfg->get_myName(), type ? type : "pcap",
           why);
    return nullptr;
  }

  // Not connected to anything: the NIC exists and behaves, but transmits go
  // nowhere and nothing is ever received. Always available, because it needs
  // no host privileges -- pcap capture wants root and TAP is Linux-only,
  // and a backend that cannot be created is fatal at device construction.
  if (type && strcasecmp(type, "null") == 0)
    return new CNetworkNull();

  // A loopback plug on the port: what the controller transmits, it receives.
  if (type && strcasecmp(type, "loopback") == 0)
    return new CNetworkLoopback();

  // A point-to-point link over UDP to another program; no privileges.
  if (type && strcasecmp(type, "udp") == 0)
    return new CNetworkUdp();

#if defined(__linux__)
  if (type && strcasecmp(type, "tap") == 0)
    return new CNetworkTap();
#endif

  // Default: pcap
#if defined(HAVE_PCAP)
  return new CNetworkPcap();
#else
  return nullptr; // network_backend_refusal() said so
#endif
}
