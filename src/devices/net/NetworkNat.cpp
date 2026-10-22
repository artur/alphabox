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

#include "NetworkNat.hpp"
#include "Configurator.hpp"
#include "DynLib.hpp"
#include "StdAfx.hpp"

// Without libslirp (HAVE_SLIRP) this file is empty and type = "nat" is
// refused by network_backend_refusal().
#if defined(HAVE_SLIRP)

#include <arpa/inet.h>
#include <chrono>
#include <fcntl.h>
#include <libslirp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

// SlirpConfig version 6 (libslirp 4.9) brought the socket-typed poll calls
// and the timer ids; older libraries have the plain fd and callback forms.
#if SLIRP_CONFIG_VERSION_MAX >= 6
#define NAT_NEW_API 1
typedef slirp_ssize_t nat_ssize_t;
#else
typedef ssize_t nat_ssize_t;
#endif

// ---- libslirp, loaded at first use
//
// alphabox is not linked against libslirp (nor glib): a binary that has no
// use for NAT starts where the library is missing. The header gives the
// types; each call goes through a pointer filled in by nat_library_error().

#define NAT_FUNCTIONS(X)                                                       \
  X(slirp_new)                                                                 \
  X(slirp_cleanup)                                                             \
  X(slirp_input)                                                               \
  X(slirp_pollfds_poll)                                                        \
  X(slirp_add_hostfwd)                                                         \
  X(slirp_version_string)

#define NAT_DECLARE(name) static decltype(&::name) f_##name;
NAT_FUNCTIONS(NAT_DECLARE)
#undef NAT_DECLARE
#if defined(NAT_NEW_API)
static decltype(&::slirp_pollfds_fill_socket) f_slirp_pollfds_fill_socket;
static decltype(&::slirp_handle_timer) f_slirp_handle_timer;
#else
static decltype(&::slirp_pollfds_fill) f_slirp_pollfds_fill;
#endif
#define slirp_new f_slirp_new
#define slirp_cleanup f_slirp_cleanup
#define slirp_input f_slirp_input
#define slirp_pollfds_poll f_slirp_pollfds_poll
#define slirp_add_hostfwd f_slirp_add_hostfwd
#define slirp_version_string f_slirp_version_string
#define slirp_pollfds_fill_socket f_slirp_pollfds_fill_socket
#define slirp_handle_timer f_slirp_handle_timer
#define slirp_pollfds_fill f_slirp_pollfds_fill

const char *nat_library_error() {
  static std::once_flag once;
  static const char *error = nullptr;
  std::call_once(once, [] {
#if defined(__APPLE__)
    static const char *const names[] = {"libslirp.0.dylib", nullptr};
    static const char *const dirs[] = {"/opt/homebrew/lib", "/usr/local/lib",
                                       nullptr};
    error = "NAT networking needs libslirp, which is not installed (brew "
            "install libslirp)";
#else
    static const char *const names[] = {"libslirp.so.0", nullptr};
    static const char *const *const dirs = nullptr;
    error = "NAT networking needs libslirp, which is not installed (apt "
            "install libslirp0, or your distribution's libslirp package)";
#endif
    void *h = dynlib_open(names, dirs, ALPHABOX_SLIRP_LIBDIR);
    if (!h)
      return;
    bool ok = true;
#define NAT_LOAD(name)                                                         \
  ok &= (f_##name = (decltype(f_##name))dynlib_sym(h, #name)) != nullptr;
    NAT_FUNCTIONS(NAT_LOAD)
#if defined(NAT_NEW_API)
    NAT_LOAD(slirp_pollfds_fill_socket)
    NAT_LOAD(slirp_handle_timer)
#else
    NAT_LOAD(slirp_pollfds_fill)
#endif
#undef NAT_LOAD
    if (ok)
      error = nullptr;
    else
      error = "the installed libslirp is too old for NAT networking (needs "
              "libslirp 4.9 or newer; update the libslirp package)";
  });
  return error;
}

static long long now_ns() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static CNetworkNat *self(void *opaque) {
  return static_cast<CNetworkNat *>(opaque);
}

// ---- libslirp callbacks (all run inside a slirp call, so under the mutex)

static nat_ssize_t cb_send_packet(const void *buf, size_t len, void *opaque) {
  self(opaque)->guest_frame(buf, len);
  return (nat_ssize_t)len;
}
static void cb_guest_error(const char *msg, void *opaque) {
  fprintf(stderr, "nat: %s\n", msg);
}
static int64_t cb_clock_get_ns(void *opaque) { return now_ns(); }
static void cb_notify(void *opaque) { self(opaque)->wake(); }
static void cb_timer_free(void *timer, void *opaque) {
  self(opaque)->timer_free((CNetworkNat::Timer *)timer);
}
static void cb_timer_mod(void *timer, int64_t expire_ms, void *opaque) {
  self(opaque)->timer_mod((CNetworkNat::Timer *)timer, expire_ms);
}
static void *cb_timer_new(SlirpTimerCb cb, void *cb_opaque, void *opaque) {
  return self(opaque)->timer_new(-1, (void *)cb, cb_opaque);
}
#if defined(NAT_NEW_API)
static void *cb_timer_new_opaque(SlirpTimerId id, void *cb_opaque,
                                 void *opaque) {
  return self(opaque)->timer_new((int)id, nullptr, cb_opaque);
}
static void cb_register_poll_socket(slirp_os_socket, void *) {}
static void cb_unregister_poll_socket(slirp_os_socket, void *) {}
#else
static void cb_register_poll_fd(int, void *) {}
static void cb_unregister_poll_fd(int, void *) {}
#endif

static int cb_add_poll(
#if defined(NAT_NEW_API)
    slirp_os_socket fd,
#else
    int fd,
#endif
    int events, void *opaque) {
  return self(opaque)->add_poll((int)fd, events);
}
static int cb_get_revents(int idx, void *opaque) {
  return self(opaque)->get_revents(idx);
}

/// The callbacks, built once: libslirp keeps the pointer, not a copy.
static const SlirpCb *callbacks() {
  static const SlirpCb cb = [] {
    SlirpCb cb;
    memset(&cb, 0, sizeof(cb));
    cb.send_packet = cb_send_packet;
    cb.guest_error = cb_guest_error;
    cb.clock_get_ns = cb_clock_get_ns;
    cb.timer_new = cb_timer_new;
    cb.timer_free = cb_timer_free;
    cb.timer_mod = cb_timer_mod;
    cb.notify = cb_notify;
#if defined(NAT_NEW_API)
    cb.timer_new_opaque = cb_timer_new_opaque;
    cb.register_poll_socket = cb_register_poll_socket;
    cb.unregister_poll_socket = cb_unregister_poll_socket;
#else
    cb.register_poll_fd = cb_register_poll_fd;
    cb.unregister_poll_fd = cb_unregister_poll_fd;
#endif
    return cb;
  }();
  return &cb;
}

// ---- the callbacks' work

void CNetworkNat::guest_frame(const void *buf, size_t len) {
  std::lock_guard<std::mutex> g(m_queue_mx);
  if (m_queue.size() >= QUEUE_LIMIT) {
    m_dropped++; // slirp's TCP retransmits; UDP and ICMP are lossy anyway
    return;
  }
  m_queue.emplace_back((const u8 *)buf, (const u8 *)buf + len);

  // libslirp broadcasts every BOOTP/DHCP reply. A client that did not ask
  // for a broadcast (the flag in the request; libslirp's reply does not carry
  // it, so send() noted it) is answered at its own hardware address, as RFC
  // 2131 4.1 allows and dnsmasq does: the SRM console loads a receive filter of
  // its own address alone and would never hear a broadcast.
  u8 *f = m_queue.back().data();
  if (len >= 14 + 20 + 8 + 44 && f[12] == 0x08 && f[13] == 0x00 &&
      (f[14] >> 4) == 4 && f[23] == 17 &&
      !memcmp(f, "\xff\xff\xff\xff\xff\xff", 6)) {
    size_t ihl = (f[14] & 15) * 4;
    if (len >= 14 + ihl + 8 + 44) {
      u8 *udp = f + 14 + ihl;
      u8 *bootp = udp + 8;
      bool server_to_client =
          udp[0] == 0 && udp[1] == 67 && udp[2] == 0 && udp[3] == 68;
      bool broadcast_flag = m_request_broadcast;
      if (server_to_client && bootp[2] == 6 && !broadcast_flag)
        memcpy(f, bootp + 28, 6);
    }
  }
}

CNetworkNat::Timer *CNetworkNat::timer_new(int id, void *cb_fn,
                                           void *cb_opaque) {
  Timer *t = new Timer{id, cb_fn, cb_opaque, 0, false};
  m_timers.push_back(t);
  return t;
}

void CNetworkNat::timer_free(Timer *t) {
  for (size_t i = 0; i < m_timers.size(); i++)
    if (m_timers[i] == t) {
      m_timers.erase(m_timers.begin() + i);
      break;
    }
  delete t;
}

void CNetworkNat::timer_mod(Timer *t, long long expire_ms) {
  t->expire_ms = expire_ms;
  t->armed = true;
}

void CNetworkNat::wake() {
  if (m_wake[1] >= 0) {
    char c = 1;
    // A full socket already holds a wakeup; nothing to add.
    if (::write(m_wake[1], &c, 1) < 0) {
    }
  }
}

int CNetworkNat::add_poll(int fd, int events) {
  pollfd p;
  p.fd = fd;
  p.events = (events & SLIRP_POLL_IN ? POLLIN : 0) |
             (events & SLIRP_POLL_OUT ? POLLOUT : 0) |
             (events & SLIRP_POLL_PRI ? POLLPRI : 0);
  p.revents = 0;
  m_pollfds.push_back(p);
  return (int)m_pollfds.size() - 1;
}

int CNetworkNat::get_revents(int idx) {
  if (idx < 0 || idx >= (int)m_pollfds.size())
    return 0;
  short r = m_pollfds[idx].revents;
  return (r & POLLIN ? SLIRP_POLL_IN : 0) | (r & POLLOUT ? SLIRP_POLL_OUT : 0) |
         (r & POLLPRI ? SLIRP_POLL_PRI : 0) |
         (r & POLLERR ? SLIRP_POLL_ERR : 0) |
         (r & POLLHUP ? SLIRP_POLL_HUP : 0);
}

// ---- configuration

/// "a.b.c.d" into `out` (network order).
static bool parse_ip(const std::string &s, in_addr *out) {
  return inet_pton(AF_INET, s.c_str(), out) == 1;
}

/// "a.b.c.d/len" into address and mask (network order).
static bool parse_net(const std::string &s, in_addr *addr, in_addr *mask) {
  size_t slash = s.find('/');
  if (slash == std::string::npos || !parse_ip(s.substr(0, slash), addr))
    return false;
  int len = atoi(s.c_str() + slash + 1);
  if (len < 8 || len > 30)
    return false;
  mask->s_addr = htonl(0xffffffffu << (32 - len));
  addr->s_addr &= mask->s_addr;
  return true;
}

/// "tcp:[host]:hport-[guest]:gport" (or udp:) for slirp_add_hostfwd.
bool CNetworkNat::add_hostfwd(const char *devid_string,
                              const std::string &rule) {
  size_t dash = rule.find('-');
  size_t c1 = rule.find(':');
  size_t c2 = dash == std::string::npos ? dash : rule.rfind(':', dash);
  size_t c3 = dash == std::string::npos ? dash : rule.find(':', dash);
  if (dash == std::string::npos || c1 == std::string::npos || c2 == c1 ||
      c3 == std::string::npos) {
    printf("%s: hostfwd \"%s\" is not tcp:[hostaddr]:hport-[guestaddr]:gport\n",
           devid_string, rule.c_str());
    return false;
  }
  std::string proto = rule.substr(0, c1);
  std::string haddr = rule.substr(c1 + 1, c2 - c1 - 1);
  int hport = atoi(rule.c_str() + c2 + 1);
  std::string gaddr = rule.substr(dash + 1, c3 - dash - 1);
  int gport = atoi(rule.c_str() + c3 + 1);
  int is_udp;
  if (proto == "tcp")
    is_udp = 0;
  else if (proto == "udp")
    is_udp = 1;
  else {
    printf("%s: hostfwd \"%s\": the protocol is tcp or udp\n", devid_string,
           rule.c_str());
    return false;
  }
  in_addr h, g;
  // The host side defaults to loopback, not every interface: a forward
  // into an emulated machine is not offered to the network unasked.
  if (!parse_ip(haddr.empty() ? "127.0.0.1" : haddr, &h) ||
      (!gaddr.empty() && !parse_ip(gaddr, &g)) || hport < 1 || hport > 65535 ||
      gport < 1 || gport > 65535) {
    printf("%s: hostfwd \"%s\": bad address or port\n", devid_string,
           rule.c_str());
    return false;
  }
  if (gaddr.empty())
    g.s_addr = 0; // slirp: the first DHCP address
  std::lock_guard<std::mutex> lk(m_slirp_mx);
  if (slirp_add_hostfwd(m_slirp, is_udp, h, hport, g, gport) < 0) {
    printf("%s: hostfwd \"%s\": cannot listen on the host port (in use?)\n",
           devid_string, rule.c_str());
    return false;
  }
  return true;
}

bool CNetworkNat::init(const char *devid_string, CConfigurator *cfg) {
  NatOptions o;
  auto text = [&](const char *key, std::string &dst) {
    if (const char *v = cfg->get_text_value(key))
      dst = v;
  };
  text("net", o.net);
  text("host", o.host);
  text("dns", o.dns);
  text("dhcp_start", o.dhcp_start);
  text("hostname", o.hostname);
  text("tftp_root", o.tftp_root);
  text("bootfile", o.bootfile);
  text("hostfwd", o.hostfwd);
  o.restrict_net = cfg->get_bool_value("restrict", false);
  o.host_loopback = cfg->get_bool_value("host_loopback", true);
  o.ipv6 = cfg->get_bool_value("ipv6", false);
  return start(devid_string, o);
}

bool CNetworkNat::start(const char *devid_string, const NatOptions &o) {
  if (const char *why = nat_library_error()) {
    printf("%s: %s.\n", devid_string, why);
    return false;
  }
  in_addr net, mask, host, dns, dhcp;
  if (!parse_net(o.net, &net, &mask)) {
    printf("%s: net = \"%s\" is not a.b.c.d/len with len 8 to 30\n",
           devid_string, o.net.c_str());
    return false;
  }
  struct {
    const char *key;
    const std::string *value;
    in_addr *addr;
  } addrs[] = {{"host", &o.host, &host},
               {"dns", &o.dns, &dns},
               {"dhcp_start", &o.dhcp_start, &dhcp}};
  for (auto &a : addrs) {
    if (!parse_ip(*a.value, a.addr)) {
      printf("%s: %s = \"%s\" is not an IPv4 address\n", devid_string, a.key,
             a.value->c_str());
      return false;
    }
    if ((a.addr->s_addr & mask.s_addr) != net.s_addr ||
        a.addr->s_addr == net.s_addr ||
        a.addr->s_addr == (net.s_addr | ~mask.s_addr)) {
      printf("%s: %s = %s is not a host address inside net %s\n", devid_string,
             a.key, a.value->c_str(), o.net.c_str());
      return false;
    }
  }
  if (host.s_addr == dns.s_addr || host.s_addr == dhcp.s_addr ||
      dns.s_addr == dhcp.s_addr) {
    printf("%s: host, dns and dhcp_start must be three different addresses\n",
           devid_string);
    return false;
  }

  if (socketpair(AF_UNIX, SOCK_STREAM, 0, m_wake) < 0) {
    perror("nat socketpair");
    return false;
  }
  for (int fd : m_wake) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
  }

  SlirpConfig c;
  memset(&c, 0, sizeof(c));
  c.version = SLIRP_CONFIG_VERSION_MAX;
  c.restricted = o.restrict_net ? 1 : 0;
  c.in_enabled = true;
  c.vnetwork = net;
  c.vnetmask = mask;
  c.vhost = host;
  c.vdhcp_start = dhcp;
  c.vnameserver = dns;
  c.in6_enabled = o.ipv6;
  inet_pton(AF_INET6, "fec0::", &c.vprefix_addr6);
  c.vprefix_len = 64;
  inet_pton(AF_INET6, "fec0::2", &c.vhost6);
  inet_pton(AF_INET6, "fec0::3", &c.vnameserver6);
  c.vhostname = o.hostname.empty() ? nullptr : o.hostname.c_str();
  c.tftp_path = o.tftp_root.empty() ? nullptr : o.tftp_root.c_str();
  c.bootfile = o.bootfile.empty() ? nullptr : o.bootfile.c_str();
#if SLIRP_CONFIG_VERSION_MAX >= 4
  c.disable_host_loopback = !o.host_loopback;
#endif
  // enable_emu stays off: it is for old Windows and Linux guests' tricks
  // (ftp/irc/ident helpers, slirp's own "emulation") that nothing needs.

  {
    std::lock_guard<std::mutex> lk(m_slirp_mx);
    m_slirp = slirp_new(&c, callbacks(), this);
  }
  if (!m_slirp) {
    printf("%s: libslirp refused the NAT configuration\n", devid_string);
    close();
    return false;
  }

  // Forwarding rules, comma separated.
  size_t pos = 0;
  while (pos < o.hostfwd.size()) {
    size_t comma = o.hostfwd.find(',', pos);
    if (comma == std::string::npos)
      comma = o.hostfwd.size();
    std::string rule = o.hostfwd.substr(pos, comma - pos);
    rule.erase(0, rule.find_first_not_of(" \t"));
    rule.erase(rule.find_last_not_of(" \t") + 1);
    pos = comma + 1;
    if (!rule.empty() && !add_hostfwd(devid_string, rule)) {
      close();
      return false;
    }
  }

  m_stop = false;
  m_thread = std::thread(&CNetworkNat::run, this);
  printf("%s: NAT network %s, host %s, dns %s, guest %s (libslirp %s)\n",
         devid_string, o.net.c_str(), o.host.c_str(), o.dns.c_str(),
         o.dhcp_start.c_str(), slirp_version_string());
  return true;
}

// ---- the NIC's side

int CNetworkNat::send(const u8 *data, int len) {
  if (!m_slirp || len < 14)
    return -1;
  {
    std::lock_guard<std::mutex> lk(m_slirp_mx);
    // A BOOTP/DHCP request (UDP 68 to 67): does the client want its reply
    // broadcast? slirp answers during this call (see guest_frame()).
    if (len >= 14 + 20 + 8 + 12 && data[12] == 0x08 && data[13] == 0x00 &&
        (data[14] >> 4) == 4 && data[23] == 17) {
      size_t ihl = (data[14] & 15) * 4;
      if ((size_t)len >= 14 + ihl + 8 + 12) {
        const u8 *udp = data + 14 + ihl;
        if (udp[0] == 0 && udp[1] == 68 && udp[2] == 0 && udp[3] == 67)
          m_request_broadcast = udp[8 + 10] & 0x80;
      }
    }
    slirp_input(m_slirp, data, len);
  }
  wake(); // the library may have opened a socket the poll must watch
  return 0;
}

int CNetworkNat::receive(const u8 **data, int *len) {
  std::lock_guard<std::mutex> g(m_queue_mx);
  if (m_queue.empty())
    return 0;
  m_current = std::move(m_queue.front());
  m_queue.pop_front();
  *data = m_current.data();
  *len = (int)m_current.size();
  return 1;
}

// ---- the worker

void CNetworkNat::run() {
  while (!m_stop) {
    int timeout_ms;
    {
      std::lock_guard<std::mutex> lk(m_slirp_mx);
      m_pollfds.clear();
      pollfd w;
      w.fd = m_wake[0];
      w.events = POLLIN;
      w.revents = 0;
      m_pollfds.push_back(w);
      uint32_t to = 1000; // a stop request is never waited on for longer
#if defined(NAT_NEW_API)
      slirp_pollfds_fill_socket(m_slirp, &to, cb_add_poll, this);
#else
      slirp_pollfds_fill(m_slirp, &to, cb_add_poll, this);
#endif
      long long now_ms = now_ns() / 1000000;
      for (Timer *t : m_timers)
        if (t->armed) {
          long long rem = t->expire_ms - now_ms;
          if (rem < 0)
            rem = 0;
          if (rem < (long long)to)
            to = (uint32_t)rem;
        }
      timeout_ms = (int)to;
    }

    // Outside the mutex: the NIC thread's send() must not wait for this.
    int r = poll(m_pollfds.data(), m_pollfds.size(), timeout_ms);
    if (r > 0 && (m_pollfds[0].revents & POLLIN)) {
      char buf[64];
      while (::read(m_wake[0], buf, sizeof(buf)) > 0) {
      }
    }

    std::lock_guard<std::mutex> lk(m_slirp_mx);
    if (m_stop)
      break;
    slirp_pollfds_poll(m_slirp, r < 0, cb_get_revents, this);
    // Fire the timers that are due. A callback may arm, re-arm or free any
    // timer, so look the list over again after each one.
    for (;;) {
      long long now_ms = now_ns() / 1000000;
      Timer *due = nullptr;
      for (Timer *t : m_timers)
        if (t->armed && t->expire_ms <= now_ms) {
          due = t;
          break;
        }
      if (!due)
        break;
      due->armed = false;
#if defined(NAT_NEW_API)
      if (due->id >= 0)
        slirp_handle_timer(m_slirp, (SlirpTimerId)due->id, due->cb_opaque);
      else
#endif
        ((SlirpTimerCb)due->cb_fn)(due->cb_opaque);
    }
  }
}

void CNetworkNat::close() {
  m_stop = true;
  if (m_thread.joinable()) {
    wake();
    m_thread.join();
  }
  {
    std::lock_guard<std::mutex> lk(m_slirp_mx);
    if (m_slirp) {
      slirp_cleanup(m_slirp);
      m_slirp = nullptr;
    }
    for (Timer *t : m_timers)
      delete t;
    m_timers.clear();
  }
  for (int &fd : m_wake)
    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  std::lock_guard<std::mutex> g(m_queue_mx);
  m_queue.clear();
}

#endif // HAVE_SLIRP
