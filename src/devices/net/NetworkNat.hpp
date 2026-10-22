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

#if !defined(INCLUDED_NETWORK_NAT_H)
#define INCLUDED_NETWORK_NAT_H

#include "NetworkBackend.hpp"

#include <atomic>
#include <deque>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <vector>

struct Slirp;

/// Load libslirp on the first call (later calls cost nothing). Null if it is
/// loaded, else a sentence saying what is missing and which package to
/// install.
const char *nat_library_error();

/**
 * \brief What a NAT back end is configured with: the keys of the NIC's
 * network section, as text, with the defaults of QEMU's "-netdev user".
 */
struct NatOptions {
  std::string net = "10.0.2.0/24"; ///< guest network, address/prefix length
  std::string host = "10.0.2.2";   ///< the host as the guest sees it
  std::string dns = "10.0.2.3";    ///< the DNS forwarder
  std::string dhcp_start = "10.0.2.15"; ///< first address handed out
  std::string hostname;                 ///< name offered in DHCP
  std::string tftp_root;                ///< directory served by TFTP
  std::string bootfile;                 ///< file named in BOOTP/DHCP replies
  std::string hostfwd;       ///< comma separated tcp:/udp: forwarding rules
  bool restrict_net = false; ///< the guest reaches nothing outside
  bool host_loopback = true; ///< host address reaches the host's 127.0.0.1
  bool ipv6 = false;         ///< IPv6 on the virtual network
};

/**
 * \brief Network backend that is a user-mode NAT router in front of the
 * guest: a built-in DHCP/BOOTP, DNS and TFTP server, and TCP, UDP and ICMP
 * echo relayed through ordinary host sockets (libslirp, the library behind
 * QEMU's "-netdev user"). It needs no host privileges and no host
 * interface.
 *
 * Configure with type = "nat" and, all optional: net, host, dns,
 * dhcp_start, hostname, tftp_root, bootfile, hostfwd, restrict,
 * host_loopback and ipv6 (see es40.cfg and docs/networking.md).
 *
 * libslirp is not thread safe. Every call into it is made under one mutex;
 * a worker thread runs its poll loop, and wakes on a socket pair when the
 * NIC sends a frame. Frames for the guest wait in a bounded queue that
 * receive() drains without taking the library's mutex (the library calls
 * its send callback from inside slirp_input, so the lock order is the
 * library's mutex, then the queue's, never the reverse).
 */
class CNetworkNat : public CNetworkBackend {
public:
  virtual ~CNetworkNat() { close(); }

  virtual bool init(const char *devid_string, CConfigurator *cfg);
  /// Start with the options given (init() reads them from the configuration).
  bool start(const char *devid_string, const NatOptions &opt);
  virtual int send(const u8 *data, int len);
  virtual int receive(const u8 **data, int *len);
  virtual void set_filter(const NetworkFilter &filter) {}
  virtual void close();

  /// Frames dropped because the queue towards the guest was full.
  unsigned long dropped() const { return m_dropped; }
  /// Most frames waiting for the guest before further ones are dropped.
  static const size_t QUEUE_LIMIT = 512;

  /// A libslirp timer: the library arms it, the worker fires it.
  struct Timer {
    int id;
    void *cb_fn;
    void *cb_opaque;
    long long expire_ms;
    bool armed;
  };

  // Called by libslirp's callbacks (public only for the C trampolines);
  // always under m_slirp_mx.
  void guest_frame(const void *buf, size_t len);
  Timer *timer_new(int id, void *cb_fn, void *cb_opaque);
  void timer_free(Timer *t);
  void timer_mod(Timer *t, long long expire_ms);
  void wake();
  int add_poll(int fd, int events);
  int get_revents(int idx);

private:
  void run();
  bool add_hostfwd(const char *devid_string, const std::string &rule);

  Slirp *m_slirp = nullptr;
  std::mutex m_slirp_mx; ///< every libslirp call
  std::thread m_thread;
  std::atomic<bool> m_stop{false};
  int m_wake[2] = {-1, -1};

  std::mutex m_queue_mx; ///< m_queue only; taken after m_slirp_mx
  std::deque<std::vector<u8>> m_queue;
  std::vector<u8> m_current; ///< the frame receive() last returned
  std::atomic<unsigned long> m_dropped{0};

  bool m_request_broadcast = false; ///< last BOOTP request asked for one
  std::vector<Timer *> m_timers;    ///< under m_slirp_mx
  std::vector<pollfd> m_pollfds;    ///< the worker's, filled under m_slirp_mx
};

#endif // !defined(INCLUDED_NETWORK_NAT_H)
