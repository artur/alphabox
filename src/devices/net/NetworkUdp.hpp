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

#if !defined(INCLUDED_NETWORK_UDP_H)
#define INCLUDED_NETWORK_UDP_H

#include "NetworkBackend.hpp"

/**
 * \brief Network backend that carries each Ethernet frame in one UDP
 * datagram: a point-to-point cable to another program.
 *
 * Configure with type = "udp", udp_local = "host:port" (where to listen)
 * and udp_remote = "host:port" (where frames go). The framing is the same
 * as QEMU's "-netdev dgram", so the peer can be a second Alphabox, a QEMU
 * guest, or a test script (test/tools/net_peer.py answers BOOTP and TFTP).
 * Needs no host privileges.
 */
class CNetworkUdp : public CNetworkBackend {
public:
  virtual ~CNetworkUdp() { close(); }

  virtual bool init(const char *devid_string, CConfigurator *cfg);
  virtual int send(const u8 *data, int len);
  virtual int receive(const u8 **data, int *len);
  virtual void set_filter(const NetworkFilter &filter) {}
  virtual void close();

private:
  int sock = -1;
  u8 buffer[2048];
};

#endif // !defined(INCLUDED_NETWORK_UDP_H)
