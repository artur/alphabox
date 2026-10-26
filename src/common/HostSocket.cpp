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

/**
 * \file
 * Host addresses and listening sockets for either IP family (HostSocket.hpp),
 * written to the POSIX and Winsock 2 interfaces: getaddrinfo, getnameinfo,
 * IPV6_V6ONLY, poll and WSAPoll.
 **/

#include "HostSocket.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <thread>

#if !defined(_WIN32)
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace hostsock {

namespace {

/// The first 12 bytes of an IPv4-mapped IPv6 address (RFC 4291 2.5.5.2).
const unsigned char kMappedPrefix[12] = {0, 0, 0, 0, 0,    0,
                                         0, 0, 0, 0, 0xff, 0xff};

bool is_mapped(const sockaddr_in6 *s6) {
  return memcmp(&s6->sin6_addr, kMappedPrefix, sizeof kMappedPrefix) == 0;
}

void set_cloexec(int64_t fd) {
#if !defined(_WIN32)
  fcntl((int)fd, F_SETFD, fcntl((int)fd, F_GETFD) | FD_CLOEXEC);
#else
  (void)fd;
#endif
}

} // namespace

std::string Endpoint::text() const { return endpoint_text(sa(), len); }

std::string endpoint_text(const sockaddr *sa, socklen_t len) {
  sockaddr_in v4;
  if (sa->sa_family == AF_INET6 && len >= (socklen_t)sizeof(sockaddr_in6) &&
      is_mapped((const sockaddr_in6 *)sa)) {
    const sockaddr_in6 *s6 = (const sockaddr_in6 *)sa;
    memset(&v4, 0, sizeof v4);
    v4.sin_family = AF_INET;
    v4.sin_port = s6->sin6_port;
    memcpy(&v4.sin_addr, (const unsigned char *)&s6->sin6_addr + 12, 4);
    sa = (const sockaddr *)&v4;
    len = sizeof v4;
  }
  char host[NI_MAXHOST];
  char serv[NI_MAXSERV];
  if (getnameinfo(sa, len, host, sizeof host, serv, sizeof serv,
                  NI_NUMERICHOST | NI_NUMERICSERV) != 0)
    return "?";
  if (sa->sa_family == AF_INET6)
    return std::string("[") + host + "]:" + serv;
  return std::string(host) + ":" + serv;
}

bool split_host_port(const std::string &text, std::string *host,
                     std::string *port, std::string *why) {
  size_t colon;
  if (!text.empty() && text[0] == '[') {
    const size_t close = text.find(']');
    if (close == std::string::npos || close + 1 >= text.size() ||
        text[close + 1] != ':') {
      *why = "an address in brackets is followed by :port, as in [::1]:5555";
      return false;
    }
    *host = text.substr(1, close - 1);
    if (host->empty()) {
      *why = "no address between the brackets";
      return false;
    }
    colon = close + 1;
  } else {
    colon = text.rfind(':');
    if (colon == std::string::npos) {
      *why = "no port: host:port, as in 127.0.0.1:5555 or [::1]:5555";
      return false;
    }
    *host = text.substr(0, colon);
    // "::1:5555" could be ::1 port 5555 or the address ::1:5555 itself.
    if (host->find(':') != std::string::npos) {
      *why = "an IPv6 address takes brackets when a port follows, as in "
             "[::1]:5555";
      return false;
    }
  }
  *port = text.substr(colon + 1);
  unsigned long value = 0;
  bool ok = !port->empty() && port->size() <= 5;
  for (char c : *port) {
    ok = ok && c >= '0' && c <= '9';
    value = value * 10 + (unsigned long)(c - '0');
  }
  if (!ok || value > 65535) {
    *why = "\"" + *port + "\" is not a port (0-65535)";
    return false;
  }
  return true;
}

bool resolve(const std::string &host_in, const std::string &port, int socktype,
             bool passive, std::vector<Endpoint> *out, std::string *why) {
  std::string host = host_in;
  if (host.size() >= 2 && host.front() == '[' && host.back() == ']')
    host = host.substr(1, host.size() - 2);
  addrinfo hints = {};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = socktype;
  hints.ai_flags = AI_NUMERICSERV | (passive ? AI_PASSIVE : 0);
  addrinfo *res = nullptr;
  const int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
  if (gai != 0) {
    *why = gai_strerror(gai);
    return false;
  }
  out->clear();
  for (const addrinfo *ai = res; ai; ai = ai->ai_next) {
    if ((ai->ai_family != AF_INET && ai->ai_family != AF_INET6) ||
        ai->ai_addrlen > sizeof(sockaddr_storage))
      continue;
    Endpoint e;
    memcpy(&e.addr, ai->ai_addr, ai->ai_addrlen);
    e.len = (socklen_t)ai->ai_addrlen;
    // A hosts file can name one address twice; binding it twice would
    // fail as "in use".
    bool seen = false;
    for (const Endpoint &o : *out)
      seen = seen || o.text() == e.text();
    if (!seen)
      out->push_back(e);
  }
  freeaddrinfo(res);
  if (out->empty()) {
    *why = "no IPv4 or IPv6 address";
    return false;
  }
  return true;
}

Endpoint any_address(int family, uint16_t port) {
  Endpoint e; // zeroed: the address is 0.0.0.0 or ::
  if (family == AF_INET6) {
    sockaddr_in6 *s6 = (sockaddr_in6 *)&e.addr;
    s6->sin6_family = AF_INET6;
    s6->sin6_port = htons(port);
    e.len = sizeof *s6;
  } else {
    sockaddr_in *s4 = (sockaddr_in *)&e.addr;
    s4->sin_family = AF_INET;
    s4->sin_port = htons(port);
    e.len = sizeof *s4;
  }
  return e;
}

bool is_any(const Endpoint &e) {
  static const unsigned char zero[16] = {0};
  if (e.family() == AF_INET6)
    return memcmp(&((const sockaddr_in6 *)&e.addr)->sin6_addr, zero, 16) == 0;
  if (e.family() == AF_INET)
    return ((const sockaddr_in *)&e.addr)->sin_addr.s_addr == 0;
  return false;
}

Endpoint v4_mapped(const Endpoint &e) {
  const sockaddr_in *s4 = (const sockaddr_in *)&e.addr;
  Endpoint m;
  sockaddr_in6 *s6 = (sockaddr_in6 *)&m.addr;
  s6->sin6_family = AF_INET6;
  s6->sin6_port = s4->sin_port;
  memcpy(&s6->sin6_addr, kMappedPrefix, sizeof kMappedPrefix);
  memcpy((unsigned char *)&s6->sin6_addr + 12, &s4->sin_addr, 4);
  m.len = sizeof *s6;
  return m;
}

int last_error() {
#if defined(_WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

std::string error_text(int err) {
#if defined(_WIN32)
  return "Winsock error " + std::to_string(err);
#else
  return strerror(err);
#endif
}

bool error_unavailable(int err) {
#if defined(_WIN32)
  return err == WSAEADDRNOTAVAIL || err == WSAEAFNOSUPPORT ||
         err == WSAEPROTONOSUPPORT || err == WSAEPFNOSUPPORT;
#else
  return err == EADDRNOTAVAIL || err == EAFNOSUPPORT ||
         err == EPROTONOSUPPORT || err == EPFNOSUPPORT;
#endif
}

void close_socket(int64_t fd) {
#if defined(_WIN32)
  closesocket((SOCKET)fd);
#else
  ::close((int)fd);
#endif
}

bool TcpListener::open(const std::vector<Endpoint> &addrs, int backlog,
                       std::string *why) {
  close();
  skipped.clear();
  bool have_v4 = false;
  for (const Endpoint &e : addrs)
    have_v4 = have_v4 || e.family() == AF_INET;

  for (const Endpoint &e : addrs) {
    const int64_t s = (int64_t)::socket(e.family(), SOCK_STREAM, 0);
    if (s < 0) {
      const int err = last_error();
      if (error_unavailable(err)) {
        skipped.push_back("not listening on " + e.text() + ": " +
                          error_text(err));
        continue;
      }
      *why = "can't create a socket for " + e.text() + ": " + error_text(err);
      close();
      return false;
    }
    set_cloexec(s);
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    // Dual stack when the hosts gave no IPv4 address to listen on beside
    // this one: "::" then takes IPv4 clients too. With one, each family
    // keeps its own socket ("::" and "0.0.0.0" would collide).
    bool is_dual = false;
    if (e.family() == AF_INET6) {
      const int v6only = have_v4 ? 1 : 0;
      if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, (const char *)&v6only,
                     sizeof v6only) == 0)
        is_dual = !v6only && is_any(e);
      else if (!v6only && is_any(e))
        skipped.push_back(e.text() +
                          " takes IPv6 clients only: the host "
                          "keeps IPv4 off IPv6 sockets (" +
                          error_text(last_error()) + ")");
    }
    // Without these checks a busy port went unnoticed: listen() on an
    // unbound socket binds an ephemeral one.
    if (bind(s, e.sa(), e.len) != 0 || listen(s, backlog) != 0) {
      const int err = last_error();
      close_socket(s);
      if (error_unavailable(err)) {
        skipped.push_back("not listening on " + e.text() + ": " +
                          error_text(err));
        continue;
      }
      *why = "can't listen on " + e.text() + ": " + error_text(err);
      close();
      return false;
    }
    Endpoint b;
    b.len = sizeof b.addr;
    if (getsockname(s, (sockaddr *)&b.addr, &b.len) != 0)
      b = e;
    socks.push_back(s);
    bound.push_back(b);
    dual.push_back(is_dual);
  }

  if (socks.empty()) {
    *why = skipped.empty() ? "no address to listen on" : skipped[0];
    for (size_t i = 1; i < skipped.size(); i++)
      *why += "; " + skipped[i];
    skipped.clear();
    return false;
  }
  return true;
}

int64_t TcpListener::accept(int ms, std::string *peer) {
  if (socks.empty()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return -1;
  }
#if defined(_WIN32)
  std::vector<WSAPOLLFD> p;
  for (int64_t s : socks)
    p.push_back({(SOCKET)s, POLLRDNORM, 0});
  const int n = WSAPoll(p.data(), (ULONG)p.size(), ms);
#else
  std::vector<pollfd> p;
  for (int64_t s : socks)
    p.push_back({(int)s, POLLIN, 0});
  const int n = poll(p.data(), (nfds_t)p.size(), ms);
#endif
  if (n < 0) {
    // EINTR or a broken listener: wait out the time rather than spin.
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    return -1;
  }
  for (size_t i = 0; n > 0 && i < p.size(); i++) {
    if (!p[i].revents)
      continue;
    sockaddr_storage a;
    socklen_t alen = sizeof a;
#if defined(_WIN32)
    const SOCKET c = ::accept((SOCKET)socks[i], (sockaddr *)&a, &alen);
    if (c == INVALID_SOCKET)
      continue;
#else
    const int c = ::accept((int)socks[i], (sockaddr *)&a, &alen);
    if (c < 0)
      continue;
#endif
    if (peer)
      *peer = endpoint_text((const sockaddr *)&a, alen);
    return (int64_t)c;
  }
  return -1;
}

void TcpListener::close() {
  for (int64_t s : socks)
    close_socket(s);
  socks.clear();
  bound.clear();
  dual.clear();
}

std::string TcpListener::where() const {
  std::string s;
  for (size_t i = 0; i < bound.size(); i++) {
    if (i)
      s += i + 1 == bound.size() ? " and " : ", ";
    s += bound[i].text();
    if (dual[i])
      s += " (IPv4 too)";
  }
  return s;
}

} // namespace hostsock
