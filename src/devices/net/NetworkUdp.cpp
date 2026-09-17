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

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

/// Resolve "host:port" (IPv4) into `out`.
static bool parse_endpoint(const char *text, sockaddr_in *out) {
  const char *colon = text ? strrchr(text, ':') : nullptr;
  if (!colon)
    return false;
  std::string host(text, colon - text);
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_port = htons((u16)atoi(colon + 1));
  if (host.empty() || host == "*") {
    out->sin_addr.s_addr = htonl(INADDR_ANY);
    return true;
  }
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  addrinfo *res = nullptr;
  if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
    return false;
  out->sin_addr = ((sockaddr_in *)res->ai_addr)->sin_addr;
  freeaddrinfo(res);
  return true;
}

bool CNetworkUdp::init(const char *devid_string, CConfigurator *cfg) {
  const char *local = cfg->get_text_value("udp_local");
  const char *remote = cfg->get_text_value("udp_remote");
  sockaddr_in local_addr;
  sockaddr_in remote_addr;

  if (!parse_endpoint(local, &local_addr) ||
      !parse_endpoint(remote, &remote_addr)) {
    printf("%s: type = udp needs udp_local and udp_remote as host:port.\n",
           devid_string);
    return false;
  }

  sock = socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("udp socket");
    return false;
  }
  if (bind(sock, (sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
    printf("%s: cannot bind UDP %s: %s\n", devid_string, local,
           strerror(errno));
    close();
    return false;
  }
  // A connected datagram socket sends to the peer and only hears from it.
  if (connect(sock, (sockaddr *)&remote_addr, sizeof(remote_addr)) < 0) {
    printf("%s: cannot address UDP %s: %s\n", devid_string, remote,
           strerror(errno));
    close();
    return false;
  }
  fcntl(sock, F_SETFL, fcntl(sock, F_GETFL) | O_NONBLOCK);
  printf("%s: frames over UDP, %s <-> %s\n", devid_string, local, remote);
  return true;
}

int CNetworkUdp::send(const u8 *data, int len) {
  // A peer that is not listening yet refuses the datagram; the frame is
  // lost, as on an unplugged cable.
  return ::send(sock, data, len, 0) == len ? 0 : -1;
}

int CNetworkUdp::receive(const u8 **data, int *len) {
  for (;;) {
    ssize_t n = recv(sock, buffer, sizeof(buffer), 0);
    if (n < 0)
      return 0; // nothing waiting (or a refused earlier send)
    if (n < 14 || n > 1514)
      continue; // not an Ethernet frame
    *data = buffer;
    *len = (int)n;
    return 1;
  }
}

void CNetworkUdp::close() {
  if (sock >= 0) {
    ::close(sock);
    sock = -1;
  }
}

#endif
