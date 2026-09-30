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

#if !defined(INCLUDED_VIRTIO_NET_H)
#define INCLUDED_VIRTIO_NET_H

#include "VirtioPci.hpp"
#include <atomic>

/**
 * \brief virtio-net (legacy): an Ethernet NIC on two virtqueues.
 *
 * PCI 1AF4:1000, subsystem 1AF4:0001, class 020000. Receive queue 0,
 * transmit queue 1; every buffer starts with the 10-byte legacy
 * virtio_net_hdr (no mergeable receive buffers, no offloads). The station
 * address comes from the shared NIC default (`mac` key), the link is
 * always up. Frames go through the configured network backend (pcap, TAP,
 * UDP or null, the `type` key, as for the other NICs).
 **/
class CVirtioNet : public CVirtioPci {
public:
  CVirtioNet(class CConfigurator *cfg, class CSystem *c, int pcibus,
             int pcidev);
  ~CVirtioNet() override;

  static constexpr u32 F_MAC = 1u << 5, F_STATUS = 1u << 16;
  static constexpr u16 S_LINK_UP = 1;
  static constexpr u32 kHdrLen = 10; // struct virtio_net_hdr, legacy
  static constexpr int RXQ = 0, TXQ = 1;

private:
  u32 device_features() override;
  void process_queue(int q) override;
  void poll() override;
  int poll_ms() override { return 2; }
  void device_reset() override;
  void config_written(u32 off, int size) override;
  void selftest() override;
  void apply_filter();

  class CNetworkBackend *m_backend = nullptr;
  std::vector<u8> m_pending; // a received frame waiting for a buffer
  std::vector<u8> m_buf;
  std::atomic_bool m_filter_dirty{true};
  u64 m_dropped = 0;
};

#endif
