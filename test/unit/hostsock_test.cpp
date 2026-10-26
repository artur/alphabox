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

/* Unit test of the host endpoints' addresses (src/common/HostSocket.cpp):
 * host:port splitting with IPv6 brackets, the printed form of either
 * family, resolution, and listening on loopback: a dual-stack "::" taking
 * an IPv4 and an IPv6 client, both loopbacks at once, an address the host
 * does not have skipped, a busy port refused. The IPv6 checks are skipped,
 * and say so, on a host without ::1. Exit status 0 when every check
 * passes.
 */

#include "HostSocket.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(_WIN32)
#pragma comment(lib, "ws2_32.lib")
#else
#include <netinet/in.h>
#include <unistd.h>
#endif

static int checks = 0, failures = 0;

static void check(bool ok, const std::string &what) {
  checks++;
  if (!ok) {
    failures++;
    printf("FAIL: %s\n", what.c_str());
  }
}

static void check_eq(const std::string &got, const std::string &want,
                     const std::string &what) {
  check(got == want, what + ": got \"" + got + "\", want \"" + want + "\"");
}

static bool starts(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

static void split_ok(const char *text, const char *host, const char *port) {
  std::string h, p, why;
  const bool ok = hostsock::split_host_port(text, &h, &p, &why);
  check(ok, std::string("split ") + text + ": " + why);
  if (ok) {
    check_eq(h, host, std::string("split ") + text + " host");
    check_eq(p, port, std::string("split ") + text + " port");
  }
}

static void split_bad(const char *text) {
  std::string h, p, why;
  check(!hostsock::split_host_port(text, &h, &p, &why),
        std::string("split ") + text + " is refused");
  check(!why.empty(), std::string("split ") + text + " says why");
}

/// The one address `host` resolves to, printed; "" when it does not.
static std::string resolve_one(const char *host, bool passive) {
  std::vector<hostsock::Endpoint> v;
  std::string why;
  if (!hostsock::resolve(host, "21000", SOCK_STREAM, passive, &v, &why) ||
      v.size() != 1)
    return "";
  return v[0].text();
}

static std::vector<hostsock::Endpoint> addrs(const char *host, int port) {
  std::vector<hostsock::Endpoint> v;
  std::string why;
  hostsock::resolve(host, std::to_string(port), SOCK_STREAM, true, &v, &why);
  return v;
}

static int port_of(const hostsock::Endpoint &e) {
  if (e.family() == AF_INET6)
    return ntohs(((const sockaddr_in6 *)&e.addr)->sin6_port);
  return ntohs(((const sockaddr_in *)&e.addr)->sin_port);
}

/// Connect to `host`:`port` and have `l` accept it: the peer it reports,
/// "" when either side failed.
static std::string connect_and_accept(hostsock::TcpListener &l,
                                      const char *host, int port) {
  std::vector<hostsock::Endpoint> to;
  std::string why, peer;
  if (!hostsock::resolve(host, std::to_string(port), SOCK_STREAM, false, &to,
                         &why))
    return "";
  const int64_t c = (int64_t)socket(to[0].family(), SOCK_STREAM, 0);
  if (c < 0)
    return "";
  if (connect(c, to[0].sa(), to[0].len) != 0) {
    hostsock::close_socket(c);
    return "";
  }
  const int64_t a = l.accept(2000, &peer);
  hostsock::close_socket(c);
  if (a < 0)
    return "";
  hostsock::close_socket(a);
  return peer;
}

/// Whether the host has an IPv6 loopback to bind.
static bool have_ipv6_loopback() {
  hostsock::TcpListener l;
  std::string why;
  return l.open(addrs("::1", 0), 1, &why);
}

static void test_split() {
  split_ok("127.0.0.1:5555", "127.0.0.1", "5555");
  split_ok("[::1]:5555", "::1", "5555");
  split_ok("[fe80::1%lo0]:7", "fe80::1%lo0", "7");
  split_ok("localhost:0", "localhost", "0");
  split_ok(":5555", "", "5555");
  split_ok("*:65535", "*", "65535");
  split_bad("::1:5555");     // IPv6 without brackets: ambiguous
  split_bad("127.0.0.1");    // no port
  split_bad("[::1]");        // no port
  split_bad("[::1]5555");    // no colon
  split_bad("[::1:5555");    // no closing bracket
  split_bad("[]:5555");      // nothing in the brackets
  split_bad("host:");        // empty port
  split_bad("host:65536");   // out of range
  split_bad("host:12a");     // not decimal
  split_bad("host:-1");      // not decimal
  split_bad("host:0000001"); // too long
}

static void test_text() {
  sockaddr_in s4 = {};
  s4.sin_family = AF_INET;
  s4.sin_port = htons(21000);
  s4.sin_addr.s_addr = htonl(0x7f000001);
  check_eq(hostsock::endpoint_text((sockaddr *)&s4, sizeof s4),
           "127.0.0.1:21000", "IPv4 text");

  sockaddr_in6 s6 = {};
  s6.sin6_family = AF_INET6;
  s6.sin6_port = htons(21000);
  s6.sin6_addr.s6_addr[15] = 1;
  check_eq(hostsock::endpoint_text((sockaddr *)&s6, sizeof s6), "[::1]:21000",
           "IPv6 text");

  // An IPv4 client on a dual-stack socket: ::ffff:127.0.0.1.
  hostsock::Endpoint v4;
  memcpy(&v4.addr, &s4, sizeof s4);
  v4.len = sizeof s4;
  const hostsock::Endpoint m = hostsock::v4_mapped(v4);
  check(m.family() == AF_INET6, "mapped address is IPv6");
  check_eq(m.text(), "127.0.0.1:21000", "mapped address prints as IPv4");

  check_eq(hostsock::any_address(AF_INET, 5).text(), "0.0.0.0:5",
           "IPv4 wildcard");
  check_eq(hostsock::any_address(AF_INET6, 5).text(), "[::]:5",
           "IPv6 wildcard");
  check(hostsock::is_any(hostsock::any_address(AF_INET6, 5)), "is_any ::");
  check(hostsock::is_any(hostsock::any_address(AF_INET, 5)), "is_any 0.0.0.0");
  check(!hostsock::is_any(v4), "127.0.0.1 is no wildcard");
  check(!hostsock::is_any(m), "::ffff:127.0.0.1 is no wildcard");
}

static void test_resolve() {
  check_eq(resolve_one("127.0.0.1", false), "127.0.0.1:21000",
           "resolve 127.0.0.1");
  check_eq(resolve_one("0.0.0.0", true), "0.0.0.0:21000", "resolve 0.0.0.0");
  check_eq(resolve_one("::1", false), "[::1]:21000", "resolve ::1");
  check_eq(resolve_one("[::1]", false), "[::1]:21000", "resolve [::1]");
  check_eq(resolve_one("::", true), "[::]:21000", "resolve ::");
  std::vector<hostsock::Endpoint> v;
  std::string why;
  check(!hostsock::resolve("no-such-host.invalid", "1", SOCK_STREAM, false, &v,
                           &why) &&
            !why.empty(),
        "an unknown name does not resolve, and says why");
  check(hostsock::resolve("localhost", "1", SOCK_STREAM, true, &v, &why) &&
            !v.empty(),
        "localhost resolves: " + why);
  for (const hostsock::Endpoint &e : v)
    check(e.text() == "127.0.0.1:1" || e.text() == "[::1]:1" ||
              starts(e.text(), "[fe80:"),
          "localhost is a loopback address: " + e.text());
}

static void test_listen_v4() {
  hostsock::TcpListener l;
  std::string why;
  check(l.open(addrs("127.0.0.1", 0), 4, &why), "listen on 127.0.0.1: " + why);
  if (!l.is_open())
    return;
  const int port = port_of(l.endpoints()[0]);
  check(port != 0, "the bound port is reported");
  check_eq(l.where(), "127.0.0.1:" + std::to_string(port), "where 127.0.0.1");
  const std::string peer = connect_and_accept(l, "127.0.0.1", port);
  check(starts(peer, "127.0.0.1:"), "IPv4 peer: " + peer);

  // The same address and port again: in use, refused.
  hostsock::TcpListener busy;
  check(!busy.open(addrs("127.0.0.1", port), 4, &why) && !why.empty(),
        "a busy port is refused");

  // An address of another machine (TEST-NET-1) is skipped beside one the
  // host has, and refused alone.
  hostsock::TcpListener other;
  std::vector<hostsock::Endpoint> both = addrs("192.0.2.1", 0);
  const std::vector<hostsock::Endpoint> lo = addrs("127.0.0.1", 0);
  both.insert(both.end(), lo.begin(), lo.end());
  check(other.open(both, 4, &why), "192.0.2.1 is skipped: " + why);
  check(other.endpoints().size() == 1 && other.notes().size() == 1,
        "one address listened on, one note");
  hostsock::TcpListener alone;
  check(!alone.open(addrs("192.0.2.1", 0), 4, &why), "192.0.2.1 alone fails");
}

static void test_listen_v6() {
  if (!have_ipv6_loopback()) {
    printf("SKIP: the IPv6 checks (no ::1 on this host)\n");
    return;
  }
  std::string why;
  {
    hostsock::TcpListener l;
    check(l.open(addrs("::1", 0), 4, &why), "listen on ::1: " + why);
    if (!l.is_open())
      return;
    const int port = port_of(l.endpoints()[0]);
    check_eq(l.where(), "[::1]:" + std::to_string(port), "where ::1");
    const std::string peer = connect_and_accept(l, "::1", port);
    check(starts(peer, "[::1]:"), "IPv6 peer: " + peer);
  }
  {
    // "::" is dual stack: an IPv4 client, then an IPv6 one.
    hostsock::TcpListener l;
    check(l.open(addrs("::", 0), 4, &why), "listen on ::: " + why);
    if (!l.is_open())
      return;
    const int port = port_of(l.endpoints()[0]);
    if (!l.notes().empty()) {
      printf("SKIP: dual stack (%s)\n", l.notes()[0].c_str());
    } else {
      check_eq(l.where(), "[::]:" + std::to_string(port) + " (IPv4 too)",
               "where ::");
      const std::string p4 = connect_and_accept(l, "127.0.0.1", port);
      check(starts(p4, "127.0.0.1:"),
            "IPv4 client on :: prints as IPv4: " + p4);
    }
    const std::string p6 = connect_and_accept(l, "::1", port);
    check(starts(p6, "[::1]:"), "IPv6 client on ::: " + p6);
  }
  {
    // Both loopbacks on one port, as the gdb stub listens: each family
    // keeps its own socket.
    hostsock::TcpListener probe;
    if (!probe.open(addrs("127.0.0.1", 0), 1, &why)) {
      check(false, "a free port: " + why);
      return;
    }
    const int port = port_of(probe.endpoints()[0]);
    probe.close();
    std::vector<hostsock::Endpoint> lo = addrs("127.0.0.1", port);
    const std::vector<hostsock::Endpoint> lo6 = addrs("::1", port);
    lo.insert(lo.end(), lo6.begin(), lo6.end());
    hostsock::TcpListener l;
    check(l.open(lo, 4, &why), "listen on both loopbacks: " + why);
    const std::string p = std::to_string(port);
    check_eq(l.where(), "127.0.0.1:" + p + " and [::1]:" + p, "where both");
    check(starts(connect_and_accept(l, "127.0.0.1", port), "127.0.0.1:"),
          "IPv4 client on both loopbacks");
    check(starts(connect_and_accept(l, "::1", port), "[::1]:"),
          "IPv6 client on both loopbacks");
  }
}

int main() {
#if defined(_WIN32)
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  test_split();
  test_text();
  test_resolve();
  test_listen_v4();
  test_listen_v6();
  printf("hostsock_test: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
