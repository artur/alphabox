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
 * The host side of the emulator's own network endpoints: the serial ports'
 * telnet listeners, the gdb stub and the UDP network back end. Each takes
 * an IPv4 address, an IPv6 address or a host name, resolved with
 * getaddrinfo for either family, and prints the addresses it uses as
 * `192.0.2.1:21000` or `[2001:db8::1]:21000`. POSIX hosts and Windows
 * (Winsock 2).
 **/

#if !defined(INCLUDED_HOSTSOCKET_H)
#define INCLUDED_HOSTSOCKET_H

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace hostsock {

/// One host address with its port, either family.
struct Endpoint {
  sockaddr_storage addr = {};
  socklen_t len = 0;

  int family() const { return addr.ss_family; }
  const sockaddr *sa() const { return (const sockaddr *)&addr; }
  /// `192.0.2.1:21000` or `[2001:db8::1]:21000` (see endpoint_text).
  std::string text() const;
};

/// `192.0.2.1:21000`, or `[2001:db8::1]:21000` for IPv6. An IPv4 peer that
/// a dual-stack socket reports as `::ffff:192.0.2.1` prints as IPv4.
std::string endpoint_text(const sockaddr *sa, socklen_t len);

/// Split `host:port`, or `[v6 address]:port`: an IPv6 address carries
/// brackets when a port follows, since its own colons would be ambiguous.
/// The host may be empty (`:5555`). The port is decimal, 0 to 65535.
/// Returns false with the reason in `why`.
bool split_host_port(const std::string &text, std::string *host,
                     std::string *port, std::string *why);

/// Resolve `host` (an IPv4 or IPv6 literal, with or without brackets, or a
/// name) with `port`, for `socktype` (SOCK_STREAM, SOCK_DGRAM): every
/// address of either family, in the resolver's order, each once. `passive`
/// for an address to bind. Returns false with the reason in `why`.
bool resolve(const std::string &host, const std::string &port, int socktype,
             bool passive, std::vector<Endpoint> *out, std::string *why);

/// The wildcard address of `family` (0.0.0.0 or ::) with `port`.
Endpoint any_address(int family, uint16_t port);
/// Whether `e` is a wildcard address (0.0.0.0 or ::).
bool is_any(const Endpoint &e);
/// `e`, an IPv4 address, as the IPv6 socket of a dual-stack host reaches
/// it: ::ffff:a.b.c.d.
Endpoint v4_mapped(const Endpoint &e);

/// The last socket error: errno, or WSAGetLastError() on Windows.
int last_error();
std::string error_text(int err);
/// Whether `err` says the address or its family is not on this host (no
/// IPv6, an address of another machine), rather than that it is in use.
bool error_unavailable(int err);
void close_socket(int64_t fd);

/**
 * \brief TCP listening sockets on every address a configured host
 * resolves to.
 *
 * Every address, not the first: `localhost` is 127.0.0.1 and ::1, and a
 * client reaches it whichever its resolver tries first. An IPv6 address
 * with no IPv4 one beside it is dual stack (IPV6_V6ONLY off), so `::`
 * takes IPv4 clients too where the host allows it. An address the host
 * does not have is skipped when another one is listened on, with a note;
 * an address in use is an error.
 **/
class TcpListener {
public:
  TcpListener() = default;
  TcpListener(const TcpListener &) = delete;
  TcpListener &operator=(const TcpListener &) = delete;
  ~TcpListener() { close(); }

  /// Listen at each of `addrs` (from resolve; port 0: one the host picks).
  /// The sockets are close-on-exec, with SO_REUSEADDR. Returns false with
  /// the reason in `why`, and listens on nothing.
  bool open(const std::vector<Endpoint> &addrs, int backlog, std::string *why);
  /// Wait up to `ms` for a connection on any of the sockets and accept it:
  /// the new socket (blocking, as accept returns it), or -1 when nothing
  /// came. `peer` gets the client's address.
  int64_t accept(int ms, std::string *peer);
  void close();

  bool is_open() const { return !socks.empty(); }
  /// What is listened on: `127.0.0.1:21000 and [::1]:21000`; a dual-stack
  /// socket says so (`[::]:21000 (IPv4 too)`).
  std::string where() const;
  /// The addresses listened on, with the ports bound.
  const std::vector<Endpoint> &endpoints() const { return bound; }
  /// What was left out and why, for the caller to print: an address the
  /// host does not have, a host that refused dual stack.
  const std::vector<std::string> &notes() const { return skipped; }

private:
  std::vector<int64_t> socks;
  std::vector<Endpoint> bound;
  std::vector<bool> dual;
  std::vector<std::string> skipped;
};

} // namespace hostsock

#endif // !defined(INCLUDED_HOSTSOCKET_H)
