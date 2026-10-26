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

#include "NetworkUdp.hpp"
#include "Configurator.hpp"

#if defined(_WIN32)

bool CNetworkUdp::init(const char *devid_string, CConfigurator *cfg) {
  printf("%s: UDP networking is not supported on Windows.\n", devid_string);
  return false;
}
int CNetworkUdp::send(const u8 *data, int len) { return -1; }
int CNetworkUdp::receive(const u8 **data, int *len) { return 0; }
void CNetworkUdp::close() {}

#else

#include "HostSocket.hpp"

#include <fcntl.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

/// "*" or nothing before the port: the wildcard address.
bool wildcard(const std::string &host) { return host.empty() || host == "*"; }

/// The wildcard address, with `port`, of each family in `like`.
std::vector<hostsock::Endpoint>
wildcards(const std::vector<hostsock::Endpoint> &like,
          const std::string &port) {
  std::vector<hostsock::Endpoint> out;
  for (const hostsock::Endpoint &e : like) {
    bool seen = false;
    for (const hostsock::Endpoint &o : out)
      seen = seen || o.family() == e.family();
    if (!seen)
      out.push_back(hostsock::any_address(e.family(), (u16)atoi(port.c_str())));
  }
  return out;
}

/// The first address of `family` in `v`, or null.
const hostsock::Endpoint *first_of(const std::vector<hostsock::Endpoint> &v,
                                   int family) {
  for (const hostsock::Endpoint &e : v)
    if (e.family() == family)
      return &e;
  return nullptr;
}

} // namespace

bool CNetworkUdp::init(const char *devid_string, CConfigurator *cfg) {
  const char *local = cfg->get_text_value("udp_local");
  const char *remote = cfg->get_text_value("udp_remote");
  if (!local || !remote) {
    printf("%s: type = udp needs udp_local and udp_remote as host:port.\n",
           devid_string);
    return false;
  }

  // host:port, with an IPv6 address in brackets ([::1]:5555); a name is
  // resolved for either family.
  std::string lhost, lport, rhost, rport, why;
  if (!hostsock::split_host_port(local, &lhost, &lport, &why)) {
    printf("%s: udp_local = \"%s\": %s.\n", devid_string, local, why.c_str());
    return false;
  }
  if (!hostsock::split_host_port(remote, &rhost, &rport, &why)) {
    printf("%s: udp_remote = \"%s\": %s.\n", devid_string, remote, why.c_str());
    return false;
  }
  std::vector<hostsock::Endpoint> ls, rs;
  if (!wildcard(rhost) &&
      !hostsock::resolve(rhost, rport, SOCK_DGRAM, false, &rs, &why)) {
    printf("%s: udp_remote = \"%s\": %s.\n", devid_string, remote, why.c_str());
    return false;
  }
  if (!wildcard(lhost) &&
      !hostsock::resolve(lhost, lport, SOCK_DGRAM, true, &ls, &why)) {
    printf("%s: udp_local = \"%s\": %s.\n", devid_string, local, why.c_str());
    return false;
  }
  // A wildcard end ("*:5555", ":5555") takes the other end's family, IPv4
  // when both are wildcards.
  const std::vector<hostsock::Endpoint> v4 = {
      hostsock::any_address(AF_INET, 0)};
  if (wildcard(lhost))
    ls = wildcards(wildcard(rhost) ? v4 : rs, lport);
  if (wildcard(rhost))
    rs = wildcards(ls, rport);

  // One socket, so both ends in one family: IPv4 where both have it (a name
  // with addresses in both families keeps the IPv4 link it always had;
  // write [::1] for IPv6), else IPv6. A local "::" reaches an IPv4-only
  // remote as ::ffff:a.b.c.d, dual stack.
  const hostsock::Endpoint *l = nullptr;
  hostsock::Endpoint r;
  bool mapped = false;
  for (int family : {AF_INET, AF_INET6}) {
    if (!l && first_of(ls, family) && first_of(rs, family)) {
      l = first_of(ls, family);
      r = *first_of(rs, family);
    }
  }
  const hostsock::Endpoint *l6 = first_of(ls, AF_INET6);
  if (!l && l6 && hostsock::is_any(*l6) && first_of(rs, AF_INET)) {
    l = l6;
    r = hostsock::v4_mapped(*first_of(rs, AF_INET));
    mapped = true;
  }
  if (!l) {
    printf("%s: udp_local = \"%s\" and udp_remote = \"%s\" have no address "
           "family in common.\n",
           devid_string, local, remote);
    return false;
  }

  sock = socket(l->family(), SOCK_DGRAM, 0);
  if (sock < 0) {
    printf("%s: cannot open a UDP socket for %s: %s\n", devid_string,
           l->text().c_str(), strerror(errno));
    return false;
  }
  if (mapped) {
    const int v6only = 0;
    if (setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof v6only)) {
      printf("%s: %s cannot reach the IPv4 %s: the host keeps IPv4 off IPv6 "
             "sockets (%s)\n",
             devid_string, l->text().c_str(), r.text().c_str(),
             strerror(errno));
      close();
      return false;
    }
  }
  if (bind(sock, l->sa(), l->len) < 0) {
    printf("%s: cannot bind UDP %s: %s\n", devid_string, l->text().c_str(),
           strerror(errno));
    close();
    return false;
  }
  // A connected datagram socket sends to the peer and only hears from it.
  if (connect(sock, r.sa(), r.len) < 0) {
    printf("%s: cannot address UDP %s: %s\n", devid_string, r.text().c_str(),
           strerror(errno));
    close();
    return false;
  }
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
  // The addresses in use, the port bound for a local port 0.
  hostsock::Endpoint here;
  here.len = sizeof here.addr;
  if (getsockname(sock, (sockaddr *)&here.addr, &here.len) != 0)
    here = *l;
  printf("%s: frames over UDP, %s <-> %s\n", devid_string, here.text().c_str(),
         r.text().c_str());
  return true;
}

int CNetworkUdp::send(const u8 *data, int len) {
  // A peer that is not listening yet refuses the datagram; the frame is
  // lost, as on an unplugged cable.
  return ::send(sock, data, len, 0) == len ? 0 : -1;
}

int CNetworkUdp::receive(const u8 **data, int *len) {
  // Datagrams that are no Ethernet frame are skipped, a bounded number per
  // call: a peer sending nothing else must not hold the NIC thread here.
  for (int tries = 0; tries < 64; tries++) {
    ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
    if (n < 0)
      return 0; // nothing waiting (or a refused earlier send)
    if (n < 14 || n > 1514)
      continue; // not an Ethernet frame
    *data = buffer;
    *len = (int)n;
    return 1;
  }
  return 0;
}

void CNetworkUdp::close() {
  if (sock >= 0) {
    ::close(sock);
    sock = -1;
  }
}

#endif
