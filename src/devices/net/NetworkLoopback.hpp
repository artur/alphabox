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

#if !defined(INCLUDED_NETWORK_LOOPBACK_H)
#define INCLUDED_NETWORK_LOOPBACK_H

#include "NetworkBackend.hpp"
#include <deque>
#include <mutex>
#include <vector>

/**
 * \brief A loopback plug on the NIC's port.
 *
 * Every frame the controller transmits comes back on its own receiver, as
 * with a loopback connector in the socket; nothing else is ever received
 * and nothing reaches the host. The link is up, as it is on every backend.
 * Configure it with "type = \"loopback\";" in the NIC's block.
 *
 * It is what a console's external loopback test needs (`nettest -mode ex`
 * at the SRM prompt), which a real machine fails without the connector too.
 * It lives here, behind the backend interface, so every NIC family has it
 * without knowing: the controller sends and receives as on any network.
 *
 * A frame is queued in send() and handed back by receive(). The queue is
 * bounded: a controller that transmits with its receiver stopped loses the
 * oldest frames, as a plug holds nothing.
 */
class CNetworkLoopback : public CNetworkBackend {
public:
  virtual ~CNetworkLoopback() {}

  virtual bool init(const char *devid_string, CConfigurator *cfg);

  /// Queue the frame for the controller's own receiver.
  virtual int send(const u8 *data, int len);

  /// The oldest frame the controller transmitted, if any.
  virtual int receive(const u8 **data, int *len);

  /// The controller filters its own frames; the plug passes everything.
  virtual void set_filter(const NetworkFilter &filter);

  virtual void close();

private:
  static const size_t MAX_QUEUED = 256;

  std::mutex mtx; ///< send() and receive() may come from different threads
  std::deque<std::vector<u8>> queue;
  std::vector<u8> current; ///< the frame receive() last handed out
};

#endif // !defined(INCLUDED_NETWORK_LOOPBACK_H)
