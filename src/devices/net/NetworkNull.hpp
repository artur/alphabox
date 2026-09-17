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

#if !defined(INCLUDED_NETWORK_NULL_H)
#define INCLUDED_NETWORK_NULL_H

#include "NetworkBackend.hpp"

/**
 * \brief A network backend that is not connected to anything.
 *
 * The guest sees a working NIC that never receives a packet and whose
 * transmissions go nowhere -- the network equivalent of the serial port's
 * null_attach. Configure it with "type = \"null\";" in the dec21143 block.
 *
 * It exists because the other backends need privileges the emulator often
 * does not have: pcap capture needs root (or membership of a bpf group) and
 * TAP is Linux-only, and create_network_backend failing is fatal at device
 * construction. That made a dec21143 impossible to instantiate for testing
 * on an ordinary developer machine -- including the state-file save/restore
 * paths, which have nothing to do with host networking.
 */
class CNetworkNull : public CNetworkBackend {
public:
  virtual ~CNetworkNull() {}

  virtual bool init(const char *devid_string, CConfigurator *cfg);

  /// Accept and discard: the guest's transmit completes normally.
  virtual int send(const u8 *data, int len);

  /// Never a packet waiting.
  virtual int receive(const u8 **data, int *len);

  /// Nothing to filter when nothing arrives.
  virtual void set_filter(const NetworkFilter &filter);

  virtual void close();
};

#endif // !defined(INCLUDED_NETWORK_NULL_H)
